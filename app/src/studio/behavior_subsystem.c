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

static bool encode_behavior_summaries(pb_ostream_t *stream, const pb_field_t *field,
                                      void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_local_id_map, beh) {
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
    struct zmk_behavior_ref *zbm = (struct zmk_behavior_ref *)*arg;

    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    return pb_encode_string(stream, zbm->metadata.display_name, strlen(zbm->metadata.display_name));
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

    zmk_behaviors_GetBehaviorDetailsResponse resp =
        zmk_behaviors_GetBehaviorDetailsResponse_init_zero;
    resp.id = behavior_id;
    resp.display_name.funcs.encode = encode_behavior_name;
    resp.display_name.arg = zbm;

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

// The runtime slot's display name comes from the behaviour metadata (DT
// display-name), matching get_behavior_details; falls back to the device name.
static const char *runtime_slot_display_name(const struct device *dev) {
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    STRUCT_SECTION_FOREACH(zmk_behavior_ref, ref) {
        if (ref->device == dev) {
            return ref->metadata.display_name;
        }
    }
#endif
    return dev->name;
}

// Encodes CustomBehaviors.behaviors — one CustomBehavior per runtime pool slot.
static bool encode_custom_behaviors(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_CustomBehavior beh = zmk_behaviors_CustomBehavior_init_zero;
        beh.id = zmk_behavior_get_local_id(slot->dev->name);
        strncpy(beh.display_name, runtime_slot_display_name(slot->dev),
                sizeof(beh.display_name) - 1);
        strncpy(beh.kind, slot->desc->kind, sizeof(beh.kind) - 1);
        beh.config.funcs.encode = encode_config_fields;
        beh.config.arg = (void *)slot;

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

#else // !CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING

zmk_studio_Response get_custom_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_CustomBehaviors resp = zmk_behaviors_CustomBehaviors_init_zero;
    return BEHAVIOR_RESPONSE(get_custom_behaviors, resp);
}

#endif // IS_ENABLED(CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING)

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_custom_behaviors, ZMK_STUDIO_RPC_HANDLER_SECURED);
