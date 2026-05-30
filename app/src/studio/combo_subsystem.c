/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/combos.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)
#define COMBOS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(combos, type, __VA_ARGS__)

// Encodes the (unpacked) repeated key_positions for one combo. `arg` points at
// the caller's `struct zmk_combo`, which stays valid for the duration of the
// enclosing ComboEntry submessage encode.
static bool encode_combo_key_positions(pb_ostream_t *stream, const pb_field_t *field,
                                       void *const *arg) {
    const struct zmk_combo *combo = *arg;

    for (int i = 0; i < combo->key_position_len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_varint(stream, combo->key_positions[i])) {
            return false;
        }
    }

    return true;
}

// Encodes the repeated ComboEntry list, mirroring encode_keymap_layers.
static bool encode_combos(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const size_t count = zmk_combos_get_count();

    for (size_t i = 0; i < count; i++) {
        struct zmk_combo combo;
        if (zmk_combos_get(i, &combo) < 0) {
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            LOG_WRN("Failed to encode combo entry tag");
            return false;
        }

        zmk_combos_ComboEntry entry = zmk_combos_ComboEntry_init_zero;
        entry.index = i;
        entry.has_combo = true;
        entry.combo.timeout_ms = combo.timeout_ms;
        entry.combo.require_prior_idle_ms = combo.require_prior_idle_ms;
        entry.combo.layers = combo.layer_mask;
        entry.combo.slow_release = combo.slow_release;

        if (combo.behavior.behavior_dev) {
            entry.combo.has_binding = true;
            entry.combo.binding.behavior_id =
                zmk_behavior_get_local_id(combo.behavior.behavior_dev);
            entry.combo.binding.param1 = combo.behavior.param1;
            entry.combo.binding.param2 = combo.behavior.param2;
        }

        entry.combo.key_positions.funcs.encode = encode_combo_key_positions;
        entry.combo.key_positions.arg = &combo;

        if (!pb_encode_submessage(stream, &zmk_combos_ComboEntry_msg, &entry)) {
            LOG_WRN("Failed to encode combo entry submessage");
            return false;
        }
    }

    return true;
}

zmk_studio_Response get_combos(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_combos_Combos resp = zmk_combos_Combos_init_zero;

    resp.combos.funcs.encode = encode_combos;
    resp.max_combos = zmk_combos_get_count();
    resp.max_keys_per_combo = ZMK_COMBO_MAX_KEYS;

    return COMBOS_RESPONSE(get_combos, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combos, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return 0; }

ZMK_RPC_EVENT_MAPPER(combos, event_mapper);
