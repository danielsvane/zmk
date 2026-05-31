/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stddef.h>
#include <stdint.h>
#include <zephyr/device.h>
#include <zephyr/sys/iterable_sections.h>

/**
 * Kind-agnostic runtime behaviour configuration framework.
 *
 * A "runtime behaviour" is a spare devicetree behaviour instance (drawn from a
 * pre-allocated pool, see CONFIG_ZMK_BEHAVIOR_RUNTIME_EDITING) whose config
 * lives in RAM so the Studio RPC can read it (M2) and later mutate it (M3+)
 * without recompiling firmware.
 *
 * Each behaviour kind that participates registers, per pool slot, a
 * `zmk_behavior_runtime_slot` into an iterable linker section. A slot pairs the
 * spare device with its RAM config object and a `zmk_behavior_runtime_descriptor`
 * — a static table describing the kind's editable config fields. The Studio
 * behaviour subsystem walks the section and the descriptors to encode/decode the
 * generic ConfigField wire representation, so it never branches on a specific
 * kind: adding a kind (M10) is a new pool + descriptor in that driver only.
 */

/** The C type of one editable config field, i.e. how to read/write it via its
 * struct offset and how it maps onto the proto ConfigValue/ConfigSchema oneof. */
enum zmk_behavior_runtime_field_type {
    ZMK_BEHAVIOR_RT_FIELD_INT,  // int (int-range schema)
    ZMK_BEHAVIOR_RT_FIELD_ENUM, // enum stored as int (enum-options schema)
    ZMK_BEHAVIOR_RT_FIELD_BOOL, // bool (bool schema)
};

/** Describes one editable config field of a behaviour kind: where it lives in
 * the kind's config struct, its wire key/label, and its constraints. */
struct zmk_behavior_runtime_field {
    const char *key;          // stable machine key, e.g. "tapping_term_ms"
    const char *display_name; // human label, e.g. "Tapping term (ms)"
    enum zmk_behavior_runtime_field_type type;
    size_t offset; // offsetof(<kind config struct>, <member>)

    // INT: inclusive range bounds reported as the int-range schema.
    int32_t int_min;
    int32_t int_max;

    // ENUM: option names; enum value is an index into this array.
    const char *const *enum_names;
    size_t enum_len;
};

/** A behaviour kind's full editable-config schema. */
struct zmk_behavior_runtime_descriptor {
    const char *kind; // the DT compatible family, e.g. "hold-tap"
    const struct zmk_behavior_runtime_field *fields;
    size_t fields_len;
};

/** Capacity (including the null terminator) of a slot's mutable display name —
 * matches the proto CustomBehavior.display_name max_size. */
#define ZMK_BEHAVIOR_RUNTIME_NAME_SIZE 48

/** Per-slot mutable RAM state. The slot struct itself is const (ROM), so the
 * bits that change at runtime — whether the slot has been claimed as a real
 * behaviour (M4), and its user-given display name — live here. */
struct zmk_behavior_runtime_state {
    bool active;                              // claimed via add_custom_behavior?
    char name[ZMK_BEHAVIOR_RUNTIME_NAME_SIZE]; // user display name (empty until claimed)
};

/** One runtime-editable pool slot: a spare device + its RAM config + descriptor. */
struct zmk_behavior_runtime_slot {
    const struct device *dev; // the spare DT behaviour instance
    void *config;             // RAM config object for `dev` (kind-specific layout)
    const struct zmk_behavior_runtime_descriptor *desc;
    struct zmk_behavior_runtime_state *state; // mutable per-slot RAM state
};

/** Register a pool slot into the iterable section walked by the Studio subsystem.
 * Allocates the slot's mutable RAM state (claimed flag + name) alongside it. */
#define ZMK_BEHAVIOR_RUNTIME_SLOT_DEFINE(name, _dev, _config, _desc)                                \
    static struct zmk_behavior_runtime_state name##_state = {0};                                   \
    static const STRUCT_SECTION_ITERABLE(zmk_behavior_runtime_slot, name) = {                       \
        .dev = (_dev),                                                                              \
        .config = (_config),                                                                        \
        .desc = (_desc),                                                                            \
        .state = &name##_state,                                                                     \
    }
