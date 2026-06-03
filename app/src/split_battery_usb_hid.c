/*
 * Copyright (c) 2026 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

/*
 * Vendor USB HID interface exposing the split peripherals' battery levels
 * to the host. Intended for dongle (central) builds, where the host has no
 * BLE connection to read the proxied GATT Battery Services from.
 *
 * Subscribes to zmk_peripheral_battery_state_changed (fed by
 * CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING) and pushes an input
 * report on the second USB HID instance (HID_1) whenever a level changes.
 * The same payload is answerable via GET_REPORT (input or feature), so a
 * host program can poll the current levels on startup instead of waiting
 * for the next change.
 *
 * Report layout (Report ID 0x01), one byte per peripheral slot:
 *     byte N: state of charge of peripheral N (0..100, 0xFF = unknown)
 *
 * Requires CONFIG_USB_HID_DEVICE_COUNT >= 2 so Zephyr reserves the HID_1
 * instance next to ZMK's own HID_0.
 */

#include <zephyr/kernel.h>
#include <zephyr/init.h>
#include <zephyr/device.h>
#include <zephyr/usb/usb_device.h>
#include <zephyr/usb/class/usb_hid.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/events/battery_state_changed.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#define BATTERY_REPORT_ID 0x01
#define BATTERY_UNKNOWN 0xFF
#define PERIPHERAL_COUNT CONFIG_ZMK_SPLIT_BLE_CENTRAL_PERIPHERALS

#define HID_GET_REPORT_TYPE_MASK 0xff00
#define HID_GET_REPORT_ID_MASK 0x00ff

#define HID_REPORT_TYPE_INPUT 0x100
#define HID_REPORT_TYPE_FEATURE 0x300

static const uint8_t battery_report_desc[] = {
    0x06, 0x00, 0xFF,              /* Usage Page (Vendor Defined 0xFF00)     */
    0x09, 0x01,                    /* Usage (0x01) — split battery levels    */
    0xA1, 0x01,                    /* Collection (Application)               */
    0x85, BATTERY_REPORT_ID,       /*   Report ID (1)                        */
    0x15, 0x00,                    /*   Logical Minimum (0)                  */
    0x26, 0xFF, 0x00,              /*   Logical Maximum (255)                */
    0x75, 0x08,                    /*   Report Size (8)                      */
    0x95, PERIPHERAL_COUNT,        /*   Report Count (peripherals)           */
    0x19, 0x01,                    /*   Usage Minimum (1)                    */
    0x29, PERIPHERAL_COUNT,        /*   Usage Maximum (peripherals)          */
    0x81, 0x02,                    /*   Input (Data, Variable, Absolute)     */
    0x19, 0x01,                    /*   Usage Minimum (1)                    */
    0x29, PERIPHERAL_COUNT,        /*   Usage Maximum (peripherals)          */
    0xB1, 0x02,                    /*   Feature (Data, Variable, Absolute)   */
    0xC0,                          /* End Collection                         */
};

static const struct device *battery_hid_dev;
static K_SEM_DEFINE(battery_hid_sem, 1, 1);

struct battery_report {
    uint8_t report_id;
    uint8_t levels[PERIPHERAL_COUNT];
} __packed;

static struct battery_report report = {
    .report_id = BATTERY_REPORT_ID,
    .levels = {[0 ... PERIPHERAL_COUNT - 1] = BATTERY_UNKNOWN},
};

static void battery_hid_in_ready_cb(const struct device *dev) { k_sem_give(&battery_hid_sem); }

static int battery_hid_get_report_cb(const struct device *dev, struct usb_setup_packet *setup,
                                     int32_t *len, uint8_t **data) {
    switch (setup->wValue & HID_GET_REPORT_TYPE_MASK) {
    case HID_REPORT_TYPE_INPUT:
    case HID_REPORT_TYPE_FEATURE:
        if ((setup->wValue & HID_GET_REPORT_ID_MASK) != BATTERY_REPORT_ID) {
            return -EINVAL;
        }
        *data = (uint8_t *)&report;
        *len = sizeof(report);
        return 0;
    default:
        return -ENOTSUP;
    }
}

static const struct hid_ops battery_hid_ops = {
    .get_report = battery_hid_get_report_cb,
    .int_in_ready = battery_hid_in_ready_cb,
};

static void battery_hid_send(void) {
    /* Fails harmlessly while USB is not configured or the host is not
     * polling; the host can always GET_REPORT the current state. */
    if (k_sem_take(&battery_hid_sem, K_MSEC(100)) != 0) {
        LOG_DBG("split battery HID endpoint busy, dropping report");
        return;
    }

    int err = hid_int_ep_write(battery_hid_dev, (uint8_t *)&report, sizeof(report), NULL);
    if (err) {
        k_sem_give(&battery_hid_sem);
        LOG_DBG("split battery HID write failed: %d", err);
    }
}

static int battery_hid_listener(const zmk_event_t *eh) {
    const struct zmk_peripheral_battery_state_changed *ev =
        as_zmk_peripheral_battery_state_changed(eh);

    if (ev->source >= PERIPHERAL_COUNT) {
        LOG_WRN("battery event from unexpected peripheral %u", ev->source);
        return ZMK_EV_EVENT_BUBBLE;
    }

    report.levels[ev->source] = ev->state_of_charge;
    battery_hid_send();
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(split_battery_usb_hid, battery_hid_listener);
ZMK_SUBSCRIPTION(split_battery_usb_hid, zmk_peripheral_battery_state_changed);

static int split_battery_usb_hid_init(void) {
    battery_hid_dev = device_get_binding("HID_1");
    if (battery_hid_dev == NULL) {
        LOG_ERR("Unable to locate HID_1; is CONFIG_USB_HID_DEVICE_COUNT >= 2?");
        return -ENODEV;
    }

    usb_hid_register_device(battery_hid_dev, battery_report_desc, sizeof(battery_report_desc),
                            &battery_hid_ops);

    return usb_hid_init(battery_hid_dev);
}

/* Must run before zmk_usb_init (CONFIG_ZMK_USB_INIT_PRIORITY, 96) calls
 * usb_enable(); same priority as ZMK's own HID_0 registration. */
SYS_INIT(split_battery_usb_hid_init, APPLICATION, CONFIG_ZMK_USB_HID_INIT_PRIORITY);
