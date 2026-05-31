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
// M1 de-risks the kind-agnostic config representation (proto §3.2) before any
// behaviour driver exists. get_custom_behaviors returns ONE hardcoded fake
// hold-tap behaviour with two schema-described fields (an int-range and an
// enum). The repeated submessage lists (CustomBehaviors.behaviors and
// CustomBehavior.config) are nanopb callbacks, encoded one entry at a time
// (mirroring combos' encode_combos) so each ConfigField — itself a static
// struct — sits on the stack only momentarily.
//
// M2 replaces this fake with the real devicetree spare-instance pool, driving
// the same ConfigField encoding from a per-kind config-schema descriptor.
// ---------------------------------------------------------------------------

// A static description of one fake config field, so the M1 encoder is pure
// data. Each entry becomes one ConfigField submessage. Only int-range and enum
// are exercised here; the proto carries the rest (bool / positions / refs) for
// later milestones.
struct fake_config_field {
    const char *key;
    const char *display_name;
    // schema
    pb_size_t which_schema; // zmk_behaviors_ConfigSchema_*_tag
    int32_t int_min;
    int32_t int_max;
    const char *const *enum_names;
    size_t enum_names_len;
    // current value
    pb_size_t which_value; // zmk_behaviors_ConfigValue_*_tag
    int32_t int_value;
    uint32_t enum_value;
};

static const char *const fake_flavor_names[] = {
    "hold-preferred",
    "balanced",
    "tap-preferred",
    "tap-unless-interrupted",
};

static const struct fake_config_field fake_fields[] = {
    {
        .key = "tapping_term_ms",
        .display_name = "Tapping term (ms)",
        .which_schema = zmk_behaviors_ConfigSchema_int_range_tag,
        .int_min = 0,
        .int_max = 5000,
        .which_value = zmk_behaviors_ConfigValue_int_value_tag,
        .int_value = 200,
    },
    {
        .key = "flavor",
        .display_name = "Flavor",
        .which_schema = zmk_behaviors_ConfigSchema_enum_options_tag,
        .enum_names = fake_flavor_names,
        .enum_names_len = ARRAY_SIZE(fake_flavor_names),
        .which_value = zmk_behaviors_ConfigValue_enum_value_tag,
        .enum_value = 1, // "balanced"
    },
};

// Build a wire ConfigField from a fake_config_field descriptor.
static void fill_config_field(zmk_behaviors_ConfigField *out,
                              const struct fake_config_field *src) {
    *out = (zmk_behaviors_ConfigField)zmk_behaviors_ConfigField_init_zero;

    strncpy(out->key, src->key, sizeof(out->key) - 1);
    strncpy(out->display_name, src->display_name, sizeof(out->display_name) - 1);

    out->has_schema = true;
    out->schema.which_s = src->which_schema;
    switch (src->which_schema) {
    case zmk_behaviors_ConfigSchema_int_range_tag:
        out->schema.s.int_range.min = src->int_min;
        out->schema.s.int_range.max = src->int_max;
        break;
    case zmk_behaviors_ConfigSchema_enum_options_tag: {
        zmk_behaviors_EnumOptions *opts = &out->schema.s.enum_options;
        opts->names_count = MIN(src->enum_names_len, ARRAY_SIZE(opts->names));
        for (size_t i = 0; i < opts->names_count; i++) {
            strncpy(opts->names[i], src->enum_names[i], sizeof(opts->names[i]) - 1);
        }
        break;
    }
    default:
        break;
    }

    out->has_value = true;
    out->value.which_v = src->which_value;
    switch (src->which_value) {
    case zmk_behaviors_ConfigValue_int_value_tag:
        out->value.v.int_value = src->int_value;
        break;
    case zmk_behaviors_ConfigValue_enum_value_tag:
        out->value.v.enum_value = src->enum_value;
        break;
    default:
        break;
    }
}

// Encodes CustomBehavior.config — one ConfigField submessage per fake_field.
static bool encode_config_fields(pb_ostream_t *stream, const pb_field_t *field,
                                 void *const *arg) {
    for (size_t i = 0; i < ARRAY_SIZE(fake_fields); i++) {
        if (!pb_encode_tag_for_field(stream, field)) {
            return false;
        }

        zmk_behaviors_ConfigField cf;
        fill_config_field(&cf, &fake_fields[i]);

        if (!pb_encode_submessage(stream, &zmk_behaviors_ConfigField_msg, &cf)) {
            LOG_WRN("Failed to encode config field %d", (int)i);
            return false;
        }
    }

    return true;
}

// Encodes CustomBehaviors.behaviors — the one hardcoded fake behaviour for M1.
static bool encode_custom_behaviors(pb_ostream_t *stream, const pb_field_t *field,
                                    void *const *arg) {
    if (!pb_encode_tag_for_field(stream, field)) {
        return false;
    }

    zmk_behaviors_CustomBehavior beh = zmk_behaviors_CustomBehavior_init_zero;
    beh.id = 1;
    strncpy(beh.display_name, "Demo HRM", sizeof(beh.display_name) - 1);
    strncpy(beh.kind, "hold-tap", sizeof(beh.kind) - 1);
    beh.config.funcs.encode = encode_config_fields;

    if (!pb_encode_submessage(stream, &zmk_behaviors_CustomBehavior_msg, &beh)) {
        LOG_WRN("Failed to encode custom behaviour submessage");
        return false;
    }

    return true;
}

zmk_studio_Response get_custom_behaviors(const zmk_studio_Request *req) {
    LOG_DBG("");
    zmk_behaviors_CustomBehaviors resp = zmk_behaviors_CustomBehaviors_init_zero;
    resp.behaviors.funcs.encode = encode_custom_behaviors;
    resp.max = 0; // no real pool yet (M2 reports the real capacity)

    return BEHAVIOR_RESPONSE(get_custom_behaviors, resp);
}

ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, list_all_behaviors, ZMK_STUDIO_RPC_HANDLER_UNSECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_behavior_details, ZMK_STUDIO_RPC_HANDLER_SECURED);
ZMK_RPC_SUBSYSTEM_HANDLER(behaviors, get_custom_behaviors, ZMK_STUDIO_RPC_HANDLER_SECURED);
