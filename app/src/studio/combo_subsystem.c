/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>

LOG_MODULE_DECLARE(zmk_studio, CONFIG_ZMK_STUDIO_LOG_LEVEL);

#include <errno.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/combos.h>
#include <zmk/studio/rpc.h>

#include <pb_encode.h>

ZMK_RPC_SUBSYSTEM(combos)

#define COMBOS_RESPONSE(type, ...) ZMK_RPC_RESPONSE(combos, type, __VA_ARGS__)
#define COMBOS_NOTIFICATION(type, ...) ZMK_RPC_NOTIFICATION(combos, type, __VA_ARGS__)

// Encodes the repeated ComboEntry list, mirroring encode_keymap_layers.
// key_positions is now a nanopb static array (see combos.options.in), so it is
// filled directly rather than via a per-field encode callback.
static bool encode_combos(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const size_t count = zmk_combos_get_count();

    for (size_t i = 0; i < count; i++) {
        struct zmk_combo combo;
        if (zmk_combos_get(i, &combo) < 0) {
            // Empty pool slot (-ENOENT) or out of range; skip it.
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

        entry.combo.key_positions_count = combo.key_position_len;
        for (int kp = 0; kp < combo.key_position_len; kp++) {
            entry.combo.key_positions[kp] = combo.key_positions[kp];
        }

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
    resp.max_combos = zmk_combos_get_capacity();
    resp.max_keys_per_combo = zmk_combos_get_max_keys();

    return COMBOS_RESPONSE(get_combos, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, get_combos, ZMK_STUDIO_RPC_HANDLER_SECURED);

zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;

    if (!set_req->has_combo) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_LOCATION);
    }

    const zmk_combos_Combo *src = &set_req->combo;

    if (src->key_positions_count > ZMK_COMBO_MAX_KEYS) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_LOCATION);
    }

    if (!src->has_binding) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_BEHAVIOR);
    }

    // Resolve the behavior by local id -> name (mirror set_layer_binding); the
    // binding resolves by name at invoke time, so in-place edits are safe.
    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(src->binding.behavior_id);
    if (!behavior_name) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_BEHAVIOR);
    }

    struct zmk_combo combo = {
        .timeout_ms = src->timeout_ms,
        .require_prior_idle_ms = src->require_prior_idle_ms,
        .layer_mask = src->layers,
        .slow_release = src->slow_release,
        .key_position_len = src->key_positions_count,
        .behavior =
            (struct zmk_behavior_binding){
                .behavior_dev = behavior_name,
                .param1 = src->binding.param1,
                .param2 = src->binding.param2,
            },
    };
    for (size_t i = 0; i < src->key_positions_count; i++) {
        combo.key_positions[i] = src->key_positions[i];
    }

    int ret = zmk_behavior_validate_binding(&combo.behavior);
    if (ret < 0) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_PARAMETERS);
    }

    ret = zmk_combos_set(set_req->index, &combo);
    if (ret < 0) {
        LOG_WRN("Setting combo %d failed with %d", set_req->index, ret);
        switch (ret) {
        case -EINVAL:
            return COMBOS_RESPONSE(set_combo,
                                   zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_LOCATION);
        default:
            return ZMK_RPC_SIMPLE_ERR(GENERIC);
        }
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, true)});

    return COMBOS_RESPONSE(set_combo, zmk_combos_SetComboResponse_SET_COMBO_RESP_OK);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, set_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return 0; }

ZMK_RPC_EVENT_MAPPER(combos, event_mapper);
