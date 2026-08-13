/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/init.h>
#include <sys/types.h>
#include <zephyr/kernel.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/sys/ring_buffer.h>

#include <zmk/ble.h>
#include <zmk/event_manager.h>
#include <zmk/events/ble_active_profile_changed.h>
#include <zmk/studio/rpc.h>

#include "uuid.h"

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

static bool handling_rx = false;

static K_SEM_DEFINE(indicate_sem, 1, 1);

static void rpc_ccc_cfg_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    bool notif_enabled = (value == BT_GATT_CCC_INDICATE);

    LOG_INF("RPC Notifications %s", notif_enabled ? "enabled" : "disabled");

#if CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY < CONFIG_BT_PERIPHERAL_PREF_LATENCY
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    if (conn) {
        uint8_t latency = notif_enabled ? CONFIG_ZMK_STUDIO_TRANSPORT_BLE_PREF_LATENCY
                                        : CONFIG_BT_PERIPHERAL_PREF_LATENCY;

        int ret = bt_conn_le_param_update(
            conn,
            BT_LE_CONN_PARAM(CONFIG_BT_PERIPHERAL_PREF_MIN_INT, CONFIG_BT_PERIPHERAL_PREF_MAX_INT,
                             latency, CONFIG_BT_PERIPHERAL_PREF_TIMEOUT));
        if (ret < 0) {
            LOG_WRN("Failed to request lower latency while studio is active (%d)", ret);
        }

        bt_conn_unref(conn);
    }
#endif
}

static ssize_t read_rpc_resp(struct bt_conn *conn, const struct bt_gatt_attr *attr, void *buf,
                             uint16_t len, uint16_t offset) {

    LOG_DBG("Read response for length %d at offset %d", len, offset);
    return 0;
}

static ssize_t write_rpc_req(struct bt_conn *conn, const struct bt_gatt_attr *attr, const void *buf,
                             uint16_t len, uint16_t offset, uint8_t flags) {
    if (!handling_rx) {
        return len;
    }

    uint32_t copied = 0;
    struct ring_buf *rpc_buf = zmk_rpc_get_rx_buf();
    while (copied < len) {
        uint8_t *buffer;
        uint32_t claim_len = ring_buf_put_claim(rpc_buf, &buffer, len - copied);

        if (claim_len > 0) {
            memcpy(buffer, ((uint8_t *)buf) + copied, claim_len);
            copied += claim_len;
        }

        ring_buf_put_finish(rpc_buf, claim_len);
    }

    zmk_rpc_rx_notify();

    return len;
}

BT_GATT_SERVICE_DEFINE(
    rpc_interface, BT_GATT_PRIMARY_SERVICE(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_SERVICE_UUID)),
    BT_GATT_CHARACTERISTIC(BT_UUID_DECLARE_128(ZMK_STUDIO_BT_RPC_CHRC_UUID),
                           BT_GATT_CHRC_WRITE | BT_GATT_CHRC_READ | BT_GATT_CHRC_INDICATE,
                           BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT, read_rpc_resp,
                           write_rpc_req, NULL),
    BT_GATT_CCC(rpc_ccc_cfg_changed, BT_GATT_PERM_READ_ENCRYPT | BT_GATT_PERM_WRITE_ENCRYPT));

// How much of a response fits in one indication. That bound comes from the ATT
// MTU, not from the link layer: ATT_HANDLE_VALUE_IND spends 3 bytes on its
// opcode and handle. Reading conn_info.le.data_len->tx_max_len conflated the two
// layers — with data-length extension negotiated it reports up to 251 even while
// the ATT MTU is still the 23-byte default, and bt_gatt_indicate rejects a value
// that large.
static uint16_t get_notify_size_for_conn(struct bt_conn *conn) {
    // 23 is the ATT default, in force until the MTU exchange completes.
    uint16_t mtu = conn ? bt_gatt_get_mtu(conn) : 0;

    return MAX(mtu, 23) - 3;
}

static int gatt_start_rx() {
    handling_rx = true;
    return 0;
}

static int gatt_stop_rx(void) {
    handling_rx = false;
    return 0;
}

static uint8_t indicate_buffer[27];

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err);

static struct bt_gatt_indicate_params rpc_indicate_params = {
    .attr = &rpc_interface.attrs[1],
    .data = indicate_buffer,
    .func = indicate_cb,
};

static void notif_rpc_tx_cb(struct k_work *work);

// Delayable so a rejected indication can be retried. -ENOMEM here is transient
// (the connection's TX buffers are momentarily exhausted), and dropping the
// attempt truncated the response for good.
static K_WORK_DELAYABLE_DEFINE(notify_tx_work, notif_rpc_tx_cb);

static void notif_rpc_tx_cb(struct k_work *work) {
    struct bt_conn *conn = zmk_ble_active_profile_conn();
    struct ring_buf *tx_buf = zmk_rpc_get_tx_buf();

    if (!conn) {
        LOG_WRN("No active connection for queued data, dropping");
        ring_buf_reset(tx_buf);
        return;
    }

    uint16_t notify_size = MIN(get_notify_size_for_conn(conn), sizeof(indicate_buffer));

    if (ring_buf_size_get(tx_buf) > 0) {
        int ret = k_sem_take(&indicate_sem, K_NO_WAIT);
        if (ret < 0) {
            // An indication is already in flight; indicate_cb reschedules us.
            goto release;
        }

        uint16_t added = 0;
        while (added < notify_size) {
            uint8_t *buf;
            uint32_t len = ring_buf_get_claim(tx_buf, &buf, notify_size - added);
            if (len == 0) {
                break;
            }

            memcpy(indicate_buffer + added, buf, len);
            added += len;
        }

        rpc_indicate_params.len = added;

        int err = bt_gatt_indicate(conn, &rpc_indicate_params);

        // Consume the claim only once the stack has taken the data. Finishing up
        // front and then failing discarded these bytes permanently, truncating
        // the response mid-frame — the client then waits forever for an EOF that
        // can never arrive, which is indistinguishable from a dead keyboard.
        ring_buf_get_finish(tx_buf, err < 0 ? 0 : added);

        if (err < 0) {
            LOG_WRN("Failed to indicate the response (%d), retrying", err);
            k_sem_give(&indicate_sem);
            k_work_reschedule(&notify_tx_work, K_MSEC(2));
        }
    }

release:
    // Not unref'ing on every exit leaked a reference per drain, so the
    // connection could never be freed.
    bt_conn_unref(conn);
}

static void indicate_cb(struct bt_conn *conn, struct bt_gatt_indicate_params *params, uint8_t err) {
    k_sem_give(&indicate_sem);
    k_work_reschedule(&notify_tx_work, K_NO_WAIT);
}

// Drain whenever the buffer is half full, exactly as the UART transport does.
//
// This used to trigger on a running count of bytes written exceeding the
// link-layer PDU size (up to 251 with data-length extension negotiated). That
// threshold is unrelated to the ring buffer's capacity —
// CONFIG_ZMK_STUDIO_RPC_TX_BUF_SIZE, 64 bytes by default — so the count could
// never reach it on a link with DLE: no drain was ever scheduled mid-message, and
// once the encoder had filled those 64 bytes rpc_tx_buffer_write spun forever on
// a zero-length claim. Any response larger than the buffer wedged the
// RPC thread permanently: no response, no error, no disconnect, and a keyboard
// that still typed fine because that runs on another thread. USB was immune only
// because its transport gated on capacity, never on an absolute size.
static void gatt_tx_notify(struct ring_buf *tx_buf, size_t added, bool msg_done, void *user_data) {
    if (msg_done || ring_buf_size_get(tx_buf) > (ring_buf_capacity_get(tx_buf) / 2)) {
        k_work_reschedule(&notify_tx_work, K_NO_WAIT);
    }
}

ZMK_RPC_TRANSPORT(gatt, ZMK_TRANSPORT_BLE, gatt_start_rx, gatt_stop_rx, NULL, gatt_tx_notify);

static int gatt_rpc_listener(const zmk_event_t *eh) {
#if IS_ENABLED(CONFIG_ZMK_STUDIO_LOCK_ON_DISCONNECT)
    struct bt_conn *conn = zmk_ble_active_profile_conn();

    if (!conn) {
        zmk_studio_core_lock();
    } else {
        bt_conn_unref(conn);
    }
#endif

    return 0;
}

ZMK_LISTENER(gatt_rpc_listener, gatt_rpc_listener);
ZMK_SUBSCRIPTION(gatt_rpc_listener, zmk_ble_active_profile_changed);
