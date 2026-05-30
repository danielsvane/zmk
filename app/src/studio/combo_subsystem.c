/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)
#define COMBOS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(combos, type, __VA_ARGS__)

// Milestone 0 walking-skeleton: returns an empty Combos container. Milestone 1
// fleshes this out with nanopb encode-callbacks over the real combos[] array.
zmk_studio_Response get_combos(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_combos_Combos resp = zmk_combos_Combos_init_zero;

    return COMBOS_RESPONSE(get_combos, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combos, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return 0; }

ZMK_RPC_EVENT_MAPPER(combos, event_mapper);
