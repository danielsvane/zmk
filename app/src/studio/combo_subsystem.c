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

// Convert a wire Combo into the public DTO, resolving the behavior local-id to a
// device name (mirror set_layer_binding) and validating the binding. Returns
// SET_COMBO_RESP_OK on success, or the matching error code. Shared by set_combo
// and add_combo; add_combo maps these onto its own enum.
static zmk_combos_SetComboResponse combo_from_proto(const zmk_combos_Combo *src,
                                                    struct zmk_combo *out) {
    if (src->key_positions_count > ZMK_COMBO_MAX_KEYS) {
        return zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_LOCATION;
    }

    if (!src->has_binding) {
        return zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_BEHAVIOR;
    }

    const char *behavior_name =
        zmk_behavior_find_behavior_name_from_local_id(src->binding.behavior_id);
    if (!behavior_name) {
        return zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_BEHAVIOR;
    }

    *out = (struct zmk_combo){
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
        out->key_positions[i] = src->key_positions[i];
    }

    if (zmk_behavior_validate_binding(&out->behavior) < 0) {
        return zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_PARAMETERS;
    }

    return zmk_combos_SetComboResponse_SET_COMBO_RESP_OK;
}

zmk_studio_Response set_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_SetComboRequest *set_req = &req->subsystem.combos.request_type.set_combo;

    if (!set_req->has_combo) {
        return COMBOS_RESPONSE(set_combo,
                               zmk_combos_SetComboResponse_SET_COMBO_RESP_INVALID_LOCATION);
    }

    struct zmk_combo combo;
    zmk_combos_SetComboResponse conv = combo_from_proto(&set_req->combo, &combo);
    if (conv != zmk_combos_SetComboResponse_SET_COMBO_RESP_OK) {
        return COMBOS_RESPONSE(set_combo, conv);
    }

    int ret = zmk_combos_set(set_req->index, &combo);
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

zmk_studio_Response add_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_AddComboRequest *add_req = &req->subsystem.combos.request_type.add_combo;

    zmk_combos_AddComboResponse resp = zmk_combos_AddComboResponse_init_zero;
    resp.which_result = zmk_combos_AddComboResponse_err_tag;

    if (!add_req->has_combo) {
        resp.result.err = zmk_combos_AddComboErrorCode_ADD_COMBO_ERR_INVALID_PARAMETERS;
        return COMBOS_RESPONSE(add_combo, resp);
    }

    struct zmk_combo combo;
    zmk_combos_SetComboResponse conv = combo_from_proto(&add_req->combo, &combo);
    if (conv != zmk_combos_SetComboResponse_SET_COMBO_RESP_OK) {
        resp.result.err = zmk_combos_AddComboErrorCode_ADD_COMBO_ERR_INVALID_PARAMETERS;
        return COMBOS_RESPONSE(add_combo, resp);
    }

    int ret = zmk_combos_add(&combo);
    if (ret < 0) {
        LOG_WRN("Adding combo failed with %d", ret);
        resp.result.err = (ret == -ENOSPC)
                              ? zmk_combos_AddComboErrorCode_ADD_COMBO_ERR_NO_SPACE
                              : zmk_combos_AddComboErrorCode_ADD_COMBO_ERR_GENERIC;
        return COMBOS_RESPONSE(add_combo, resp);
    }

    resp.which_result = zmk_combos_AddComboResponse_ok_tag;
    resp.result.ok.index = (uint32_t)ret;
    resp.result.ok.has_combo = true;
    resp.result.ok.combo = add_req->combo;

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, true)});

    return COMBOS_RESPONSE(add_combo, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, add_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

zmk_studio_Response remove_combo(const zmk_studio_Request *req) {
    LOG_DBG("");
    const zmk_combos_RemoveComboRequest *rm_req = &req->subsystem.combos.request_type.remove_combo;

    zmk_combos_RemoveComboResponse resp = zmk_combos_RemoveComboResponse_init_zero;

    int ret = zmk_combos_remove(rm_req->index);
    if (ret < 0) {
        LOG_WRN("Removing combo %d failed with %d", rm_req->index, ret);
        resp.which_result = zmk_combos_RemoveComboResponse_err_tag;
        resp.result.err = (ret == -EINVAL || ret == -ENOENT)
                              ? zmk_combos_RemoveComboErrorCode_REMOVE_COMBO_ERR_INVALID_INDEX
                              : zmk_combos_RemoveComboErrorCode_REMOVE_COMBO_ERR_GENERIC;
        return COMBOS_RESPONSE(remove_combo, resp);
    }

    resp.which_result = zmk_combos_RemoveComboResponse_ok_tag;

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, true)});

    return COMBOS_RESPONSE(remove_combo, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, remove_combo, ZMK_STUDIO_RPC_HANDLER_SECURED);

// These three share request-id names with the keymap subsystem; keep them
// static so the handler symbols don't collide at link time.
static zmk_studio_Response check_unsaved_changes(const zmk_studio_Request *req) {
    LOG_DBG("");
    return COMBOS_RESPONSE(check_unsaved_changes, zmk_combos_check_unsaved_changes() > 0);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, check_unsaved_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

static void map_errno_to_save_resp(int err, zmk_combos_SaveChangesResponse *resp) {
    resp->which_result = zmk_combos_SaveChangesResponse_err_tag;

    switch (err) {
    case -ENOTSUP:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_NOT_SUPPORTED;
        break;
    case -ENOSPC:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_NO_SPACE;
        break;
    default:
        resp->result.err = zmk_combos_SaveChangesErrorCode_SAVE_CHANGES_ERR_GENERIC;
        break;
    }
}

static zmk_studio_Response save_changes(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_combos_SaveChangesResponse resp = zmk_combos_SaveChangesResponse_init_zero;
    resp.which_result = zmk_combos_SaveChangesResponse_ok_tag;
    resp.result.ok = true;

    int ret = zmk_combos_save_changes();
    if (ret < 0) {
        LOG_WRN("Failed to save combo changes (%d)", ret);
        map_errno_to_save_resp(ret, &resp);
        return COMBOS_RESPONSE(save_changes, resp);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, false)});

    return COMBOS_RESPONSE(save_changes, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, save_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

static zmk_studio_Response discard_changes(const zmk_studio_Request *req) {
    LOG_DBG("");
    int ret = zmk_combos_discard_changes();
    if (ret < 0) {
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    raise_zmk_studio_rpc_notification((struct zmk_studio_rpc_notification){
        .notification = COMBOS_NOTIFICATION(unsaved_changes_status_changed, false)});

    return COMBOS_RESPONSE(discard_changes, true);
}

ZMK_RPC_SUBSYSTEM_HANDLER(combos, discard_changes, ZMK_STUDIO_RPC_HANDLER_SECURED);

static int combos_settings_reset(void) { return zmk_combos_reset_settings(); }

ZMK_RPC_SUBSYSTEM_SETTINGS_RESET(combos, combos_settings_reset);

static int event_mapper(const zmk_event_t *eh, zmk_studio_Notification *n) { return 0; }

ZMK_RPC_EVENT_MAPPER(combos, event_mapper);
