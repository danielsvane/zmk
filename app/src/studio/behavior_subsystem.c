/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <pb_encode.h>
#include <zmk/studio/rpc.h>
#include <drivers/behavior.h>
#include <zmk/behavior.h>
#include <zmk/behavior_runtime.h>
#include <zmk/hid.h>

ZMK_RPC_SUBSYSTEM(behaviors)

#define BEHAVIOR_RESPONSE(type, ...) ZMK_RPC_RESPONSE(behaviors, type, __VA_ARGS__)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)
// Locate a runtime pool slot by its device's stable local_id, or by device.
static const struct zmk_behavior_runtime_slot *find_runtime_slot(uint32_t local_id) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (zmk_behavior_get_local_id(slot->dev->name) == local_id) {
            return slot;
        }
    }
    return NULL;
}

static const struct zmk_behavior_runtime_slot *runtime_slot_for_device(const struct device *dev) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (slot->dev == dev) {
            return slot;
        }
    }
    return NULL;
}

// An unclaimed pool slot is hidden from list_all_behaviors (and so isn't
// selectable as a binding) until add_custom_behavior turns it into a real
// behaviour (M4). Built-in (non-pool) behaviours are never hidden.
static bool runtime_local_id_hidden(uint32_t local_id) {
    const struct zmk_behavior_runtime_slot *slot = find_runtime_slot(local_id);
    return slot && !slot->state->active;
}
#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)
        if (runtime_local_id_hidden(beh->local_id)) {
            continue;
        }
#endif
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        if (!pb_encode_varint(stream, beh->local_id)) {
            LOG_ERR("Failed to encode behavior ID");
            return false;
        }
    }

    return true;
}

zmk_studio_Response list_all_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_ListAllBehaviorsResponse beh_resp =
        zmk_behaviors_ListAllBehaviorsResponse_init_zero;
    beh_resp.behaviors.funcs.encode = encode_behavior_summaries;

    return BEHAVIOR_RESPONSE(list_all_behaviors, beh_resp);
}

struct encode_metadata_sets_state {
    const struct behavior_parameter_metadata_set *sets;
    size_t sets_len;
    size_t i;
};

static bool encode_value_description_name(pb_ostream_t *stream, const pb_field_t *field,
                                          void *const *arg) {
    struct behavior_parameter_value_metadata *state =
        (struct behavior_parameter_value_metadata *)*arg;

    if (!state->display_name) {
        return true;
    }

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, state->display_name, strlen(state->display_name));
}

static bool encode_value_description(pb_ostream_t *stream, const pb_field_t *field,
                                     void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;

    const struct behavior_parameter_metadata_set *set = &state->sets[state->i];

    bool is_param1 = field->tag == zmk_behaviors_BehaviorBindingParametersSet_param1_tag;
    size_t values_len = is_param1 ? set->param1_values_len : set->param2_values_len;
    const struct behavior_parameter_value_metadata *values =
        is_param1 ? set->param1_values : set->param2_values;

    for (int val_i = 0; val_i < values_len; val_i++) {
        const struct behavior_parameter_value_metadata *val = &values[val_i];

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_BehaviorParameterValueDescription desc =
            zmk_behaviors_BehaviorParameterValueDescription_init_zero;
        desc.name.funcs.encode = encode_value_description_name;
        desc.name.arg = (void *)val;

        switch (val->type) {
        case BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_constant_tag;
            desc.value_type.constant = val->value;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_range_tag;
            desc.value_type.range.min = val->range.min;
            desc.value_type.range.max = val->range.max;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_NIL:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_nil_tag;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_HID_USAGE:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_hid_usage_tag;
            desc.value_type.hid_usage.consumer_max = ZMK_HID_CONSUMER_MAX_USAGE;
            desc.value_type.hid_usage.keyboard_max = ZMK_HID_KEYBOARD_MAX_USAGE;
            break;
        case BEHAVIOR_PARAMETER_VALUE_TYPE_LAYER_ID:
            desc.which_value_type = zmk_behaviors_BehaviorParameterValueDescription_layer_id_tag;
            break;
        default:
            LOG_ERR("Unknown value description type %d", val->type);
            return false;
        }

        if (!pb_encode_submessage(stream, &zmk_behaviors_BehaviorParameterValueDescription_msg,
                                  &desc)) {
            LOG_WRN("Failed to encode submessage for set %d, value %d!", state->i, val_i);
            return false;
        }
    }

    return true;
}

static bool encode_metadata_sets(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    struct encode_metadata_sets_state *state = (struct encode_metadata_sets_state *)*arg;
    bool ret = true;

    LOG_DBG("Encoding the %d metadata sets with %p", state->sets_len, state->sets);

    for (int i = 0; i < state->sets_len; i++) {
        LOG_DBG("Encoding set %d", i);
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        state->i = i;
        zmk_behaviors_BehaviorBindingParametersSet msg =
            zmk_behaviors_BehaviorBindingParametersSet_init_zero;
        msg.param1.funcs.encode = encode_value_description;
        msg.param1.arg = state;
        msg.param2.funcs.encode = encode_value_description;
        msg.param2.arg = state;
        ret = pb_encode_submessage(stream, &zmk_behaviors_BehaviorBindingParametersSet_msg, &msg);
        if (!ret) {
            LOG_WRN("Failed to encode submessage for set %d", i);
            break;
        }
    }

    return ret;
}

static bool encode_behavior_name(pb_ostream_t *stream, const pb_field_t *field, void *const *arg) {
    const char *name = (const char *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, name, strlen(name));
}

static struct encode_metadata_sets_state state = {};

zmk_studio_Response get_behavior_details(const zmk_studio_Request *req) {
    uint32_t behavior_id = req->subsystem.behaviors.request_type.get_behavior_details.behavior_id;
    const char *behavior_name = zmk_behavior_find_behavior_name_from_local_id(behavior_id);

    LOG_DBG("behavior_id %d, name %s", behavior_id, behavior_name);

    if (!behavior_name) {
        LOG_WRN("No behavior found for ID %d", behavior_id);
        return ZMK_RPC_SIMPLE_ERR(GENERIC);
    }

    const struct device *device = behavior_get_binding(behavior_name);

    struct zmk_behavior_ref *zbm = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, item) {
        if (item->device == device) {
            zbm = item;
            break;
        }
    }

    __ASSERT(zbm != NULL, "Can't find a device without also having metadata");

    struct behavior_parameter_metadata desc = {0};
    int ret = behavior_get_parameter_metadata(device, &desc);
    if (ret < 0) {
        LOG_DBG("Failed to fetch the metadata for %s! %d", zbm->metadata.display_name, ret);
    } else {
        LOG_DBG("Got metadata with %d sets", desc.sets_len);
    }

    // A claimed runtime slot shows its user-given name; everything else uses the
    // const DT metadata name.
    const char *display_name = zbm->metadata.display_name;
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)
    const struct zmk_behavior_runtime_slot *rt_slot = runtime_slot_for_device(device);
    if (rt_slot && rt_slot->state->active && rt_slot->state->name[0]) {
        display_name = rt_slot->state->name;
    }
#endif

    zmk_behaviors_GetBehaviorDetailsResponse resp =
        zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
    resp.id = behavior_id;
    resp.display_name.funcs.encode = encode_behavior_name;
    resp.display_name.arg = (void *)display_name;

    state.sets = desc.sets;
    state.sets_len = desc.sets_len;

    resp.metadata.funcs.encode = encode_metadata_sets;
    resp.metadata.arg = &state;

    return BEHAVIOR_RESPONSE(get_behavior_details, resp);
}

// ---------------------------------------------------------------------------
// Custom behaviours — generic config-schema round-trip.
//
// get_custom_behaviors walks the runtime-editable spare-instance pool
// (zmk_behavior_runtime_slot section, populated by each participating driver)
// and, for each slot, encodes a CustomBehavior whose config fields are produced
// generically from the slot's per-kind descriptor. The encoder never branches
// on a behaviour kind: it reads each field's current value from the RAM config
// struct at the descriptor's offset, and reports the descriptor's schema —
// adding a kind (M10) needs no change here.
//
// The repeated submessage lists (CustomBehaviors.behaviors and
// CustomBehavior.config) are nanopb callbacks, encoded one entry at a time
// (mirroring combos' encode_combos) so each ConfigField — itself a static
// struct — sits on the stack only momentarily.
// ---------------------------------------------------------------------------

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)

// Build a wire ConfigField from a descriptor field + the slot's RAM config,
// reading the current value at the field's struct offset per its type.
static void fill_config_field(zmk_behaviors_ConfigField *out,
                              const struct zmk_behavior_runtime_field *f, const void *config) {
    *out = (zmk_behaviors_ConfigField)zmk_behaviors_ConfigField_init_zero;

    strncpy(out->key, f->key, sizeof(out->key) - 1);
    strncpy(out->display_name, f->display_name, sizeof(out->display_name) - 1);

    const uint8_t *base = (const uint8_t *)config;

    out->has_schema = true;
    out->has_value = true;
    switch (f->type) {
    case ZMK_BEHAVIOR_RT_FIELD_INT: {
        int32_t v = *(const int *)(base + f->offset);
        out->schema.which_s = zmk_behaviors_ConfigSchema_int_range_tag;
        out->schema.s.int_range.min = f->int_min;
        out->schema.s.int_range.max = f->int_max;
        out->value.which_v = zmk_behaviors_ConfigValue_int_value_tag;
        out->value.v.int_value = v;
        break;
    }
    case ZMK_BEHAVIOR_RT_FIELD_ENUM: {
        // enums are int-sized in this codebase (no -fshort-enums).
        int v = *(const int *)(base + f->offset);
        zmk_behaviors_EnumOptions *opts = &out->schema.s.enum_options;
        out->schema.which_s = zmk_behaviors_ConfigSchema_enum_options_tag;
        opts->names_count = MIN(f->enum_len, ARRAY_SIZE(opts->names));
        for (size_t i = 0; i < opts->names_count; i++) {
            strncpy(opts->names[i], f->enum_names[i], sizeof(opts->names[i]) - 1);
        }
        out->value.which_v = zmk_behaviors_ConfigValue_enum_value_tag;
        out->value.v.enum_value = (uint32_t)v;
        break;
    }
    case ZMK_BEHAVIOR_RT_FIELD_BOOL: {
        bool v = *(const bool *)(base + f->offset);
        out->schema.which_s = zmk_behaviors_ConfigSchema_bool_schema_tag;
        out->value.which_v = zmk_behaviors_ConfigValue_bool_value_tag;
        out->value.v.bool_value = v;
        break;
    }
    }
}

// Encodes CustomBehavior.config — one ConfigField submessage per descriptor field.
static bool encode_config_fields(pb_ostream_t *stream, const pb_field_t *field,
                                 void *const *arg) {
    const struct zmk_behavior_runtime_slot *slot = (const struct zmk_behavior_runtime_slot *)*arg;

    for (size_t i = 0; i < slot->desc->fields_len; i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_ConfigField cf;
        fill_config_field(&cf, &slot->desc->fields[i], slot->config);

        if (!pb_encode_submessage(stream, &zmk_behaviors_ConfigField_msg, &cf)) {
            LOG_WRN("Failed to encode config field %d", (int)i);
            return false;
        }
    }

    return true;
}

// A claimed slot shows its user-given name; otherwise the DT metadata name (the
// "Spare Hold-Tap N" label), falling back to the raw device name.
static const char *runtime_slot_display_name(const struct zmk_behavior_runtime_slot *slot) {
    if (slot->state->name[0]) {
        return slot->state->name;
    }
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, ref) {
        if (ref->device == slot->dev) {
            return ref->metadata.display_name;
        }
    }
#endif
    return slot->dev->name;
}

// Populate a wire CustomBehavior from a pool slot. `config` is a callback whose
// arg is the (static, ROM) slot, so this is safe to use both inside the
// get_custom_behaviors encode and in the add_custom_behavior OK response.
static void fill_custom_behavior(zmk_behaviors_CustomBehavior *beh,
                                 const struct zmk_behavior_runtime_slot *slot) {
    *beh = (zmk_behaviors_CustomBehavior)zmk_behaviors_CustomBehavior_init_zero;
    beh->id = zmk_behavior_get_local_id(slot->dev->name);
    strncpy(beh->display_name, runtime_slot_display_name(slot), sizeof(beh->display_name) - 1);
    strncpy(beh->kind, slot->desc->kind, sizeof(beh->kind) - 1);
    beh->config.funcs.encode = encode_config_fields;
    beh->config.arg = (void *)slot;
}

// Encodes CustomBehaviors.behaviors — one CustomBehavior per CLAIMED pool slot.
// Unclaimed spares are hidden until add_custom_behavior claims them (M4).
static bool encode_custom_behaviors(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (!slot->state->active) {
            continue;
        }

        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_CustomBehavior beh;
        fill_custom_behavior(&beh, slot);

        if (!pb_encode_submessage(stream, &zmk_behaviors_CustomBehavior_msg, &beh)) {
            LOG_WRN("Failed to encode custom behaviour submessage");
            return false;
        }
    }

    return true;
}

zmk_studio_Response get_custom_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_CustomBehaviors resp = zmk_behaviors_CustomBehaviors_init_zero;
    resp.behaviors.funcs.encode = encode_custom_behaviors;

    size_t count = 0;
    STRUCT_SECTION_COUNT(zmk_behavior_runtime_slot, &count);
    resp.max = (uint32_t)count;

    return BEHAVIOR_RESPONSE(get_custom_behaviors, resp);
}

// ---------------------------------------------------------------------------
// set_custom_behavior — the decode direction of the generic config round-trip.
//
// The request's `config` is a static nanopb array (see behaviors.options.in),
// since requests are decoded without callbacks. We locate the target slot by its
// stable local_id, then for each incoming ConfigField match it to the kind's
// descriptor by `key` and write its value into the slot's RAM config at the
// field's offset — the inverse of fill_config_field. The subsystem still never
// names a kind, so adding a kind (M10) needs no change here. The driver reads
// dev->config on every press, so edits take effect live. RAM-only until M6.
// ---------------------------------------------------------------------------

static const struct zmk_behavior_runtime_field *
find_desc_field(const struct zmk_behavior_runtime_descriptor *desc, const char *key) {
    for (size_t i = 0; i < desc->fields_len; i++) {
        if (strcmp(desc->fields[i].key, key) == 0) {
            return &desc->fields[i];
        }
    }
    return NULL;
}

// True if `cf` carries a value of the right type and within bounds for field `f`.
static bool config_field_valid(const struct zmk_behavior_runtime_field *f,
                               const zmk_behaviors_ConfigField *cf) {
    if (!cf->has_value) {
        return false;
    }
    switch (f->type) {
    case ZMK_BEHAVIOR_RT_FIELD_INT:
        return cf->value.which_v == zmk_behaviors_ConfigValue_int_value_tag &&
               cf->value.v.int_value >= f->int_min && cf->value.v.int_value <= f->int_max;
    case ZMK_BEHAVIOR_RT_FIELD_ENUM:
        return cf->value.which_v == zmk_behaviors_ConfigValue_enum_value_tag &&
               cf->value.v.enum_value < f->enum_len;
    case ZMK_BEHAVIOR_RT_FIELD_BOOL:
        return cf->value.which_v == zmk_behaviors_ConfigValue_bool_value_tag;
    }
    return false;
}

static void apply_config_field(const struct zmk_behavior_runtime_field *f, void *config,
                               const zmk_behaviors_ConfigField *cf) {
    uint8_t *base = (uint8_t *)config;
    switch (f->type) {
    case ZMK_BEHAVIOR_RT_FIELD_INT:
        *(int *)(base + f->offset) = cf->value.v.int_value;
        break;
    case ZMK_BEHAVIOR_RT_FIELD_ENUM:
        // enums are int-sized in this codebase (no -fshort-enums).
        *(int *)(base + f->offset) = (int)cf->value.v.enum_value;
        break;
    case ZMK_BEHAVIOR_RT_FIELD_BOOL:
        *(bool *)(base + f->offset) = cf->value.v.bool_value;
        break;
    }
}

zmk_studio_Response set_custom_behavior(const zmk_studio_Request *req) {
    const zmk_behaviors_SetCustomBehaviorRequest *set_req =
        &req->subsystem.behaviors.request_type.set_custom_behavior;

    LOG_DBG("id %d, %d fields", set_req->id, (int)set_req->config_count);

    const struct zmk_behavior_runtime_slot *slot = find_runtime_slot(set_req->id);
    if (!slot) {
        LOG_WRN("No runtime behaviour slot with local_id %d", set_req->id);
        return BEHAVIOR_RESPONSE(
            set_custom_behavior,
            zmk_behaviors_SetCustomBehaviorResponse_SET_CUSTOM_BEHAVIOR_RESP_NOT_FOUND);
    }

    // Pass 1 — validate, so the edit is atomic (no half-applied config on a bad
    // value). Unknown keys are ignored for forward-compatibility.
    for (size_t i = 0; i < set_req->config_count; i++) {
        const zmk_behaviors_ConfigField *cf = &set_req->config[i];
        const struct zmk_behavior_runtime_field *f = find_desc_field(slot->desc, cf->key);
        if (f && !config_field_valid(f, cf)) {
            LOG_WRN("Invalid value for field %s", cf->key);
            return BEHAVIOR_RESPONSE(
                set_custom_behavior,
                zmk_behaviors_SetCustomBehaviorResponse_SET_CUSTOM_BEHAVIOR_RESP_INVALID_PARAMETERS);
        }
    }

    // Pass 2 — apply into the slot's mutable RAM config.
    for (size_t i = 0; i < set_req->config_count; i++) {
        const zmk_behaviors_ConfigField *cf = &set_req->config[i];
        const struct zmk_behavior_runtime_field *f = find_desc_field(slot->desc, cf->key);
        if (f) {
            apply_config_field(f, slot->config, cf);
        }
    }

    return BEHAVIOR_RESPONSE(set_custom_behavior,
                             zmk_behaviors_SetCustomBehaviorResponse_SET_CUSTOM_BEHAVIOR_RESP_OK);
}

// ---------------------------------------------------------------------------
// add_custom_behavior — claim a free spare slot of the requested kind, turning
// it into a real behaviour (M4: RAM-only). We find the first unclaimed slot
// whose descriptor kind matches `kind`, validate the seed config atomically
// (reusing the set path's per-field validate/apply), then mark it active, set
// its display name, and apply the seed config. The newly-claimed slot starts
// appearing in list_all_behaviors (so it's selectable as a binding) and in
// get_custom_behaviors. Still kind-agnostic: no behaviour family is named here.
// ---------------------------------------------------------------------------

zmk_studio_Response add_custom_behavior(const zmk_studio_Request *req) {
    const zmk_behaviors_AddCustomBehaviorRequest *add_req =
        &req->subsystem.behaviors.request_type.add_custom_behavior;

    LOG_DBG("kind %s, name %s, %d fields", add_req->kind, add_req->display_name,
            (int)add_req->config_count);

    zmk_behaviors_AddCustomBehaviorResponse resp =
        zmk_behaviors_AddCustomBehaviorResponse_init_zero;
    resp.which_result = zmk_behaviors_AddCustomBehaviorResponse_err_tag;

    // Find the first free pool slot of the requested kind.
    const struct zmk_behavior_runtime_slot *slot = NULL;
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, s) {
        if (!s->state->active && strcmp(s->desc->kind, add_req->kind) == 0) {
            slot = s;
            break;
        }
    }
    if (!slot) {
        LOG_WRN("No free runtime slot for kind %s", add_req->kind);
        resp.result.err = zmk_behaviors_AddCustomBehaviorErrorCode_ADD_CUSTOM_BEHAVIOR_ERR_NO_SPACE;
        return BEHAVIOR_RESPONSE(add_custom_behavior, resp);
    }

    // Validate the seed config before mutating anything (atomic claim).
    for (size_t i = 0; i < add_req->config_count; i++) {
        const zmk_behaviors_ConfigField *cf = &add_req->config[i];
        const struct zmk_behavior_runtime_field *f = find_desc_field(slot->desc, cf->key);
        if (f && !config_field_valid(f, cf)) {
            LOG_WRN("Invalid seed value for field %s", cf->key);
            resp.result.err =
                zmk_behaviors_AddCustomBehaviorErrorCode_ADD_CUSTOM_BEHAVIOR_ERR_INVALID_PARAMETERS;
            return BEHAVIOR_RESPONSE(add_custom_behavior, resp);
        }
    }

    // Claim: mark active, set the display name, apply the seed config into the
    // slot's RAM config (omitted fields keep their DT-seeded defaults).
    slot->state->active = true;
    strncpy(slot->state->name, add_req->display_name, sizeof(slot->state->name) - 1);
    slot->state->name[sizeof(slot->state->name) - 1] = '\0';

    for (size_t i = 0; i < add_req->config_count; i++) {
        const zmk_behaviors_ConfigField *cf = &add_req->config[i];
        const struct zmk_behavior_runtime_field *f = find_desc_field(slot->desc, cf->key);
        if (f) {
            apply_config_field(f, slot->config, cf);
        }
    }

    resp.which_result = zmk_behaviors_AddCustomBehaviorResponse_ok_tag;
    resp.result.ok.id = zmk_behavior_get_local_id(slot->dev->name);
    resp.result.ok.has_behavior = true;
    fill_custom_behavior(&resp.result.ok.behavior, slot);

    return BEHAVIOR_RESPONSE(add_custom_behavior, resp);
}

#else // !CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING

zmk_studio_Response get_custom_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_CustomBehaviors resp = zmk_behaviors_CustomBehaviors_init_zero;
    return BEHAVIOR_RESPONSE(get_custom_behaviors, resp);
}

zmk_studio_Response set_custom_behavior(const zmk_studio_Request *req) {
    LOG_DBG("");
    return BEHAVIOR_RESPONSE(
        set_custom_behavior,
        zmk_behaviors_SetCustomBehaviorResponse_SET_CUSTOM_BEHAVIOR_RESP_NOT_FOUND);
}

zmk_studio_Response add_custom_behavior(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_AddCustomBehaviorResponse resp =
        zmk_behaviors_AddCustomBehaviorResponse_init_zero;
    resp.which_result = zmk_behaviors_AddCustomBehaviorResponse_err_tag;
    resp.result.err = zmk_behaviors_AddCustomBehaviorErrorCode_ADD_CUSTOM_BEHAVIOR_ERR_NO_SPACE;
    return BEHAVIOR_RESPONSE(add_custom_behavior, resp);
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_custom_behaviors, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, set_custom_behavior, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, add_custom_behavior, ZMK_STUDIO_RPC_HANDLER_SECURED);
