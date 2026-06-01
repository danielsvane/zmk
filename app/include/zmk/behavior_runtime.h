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

#include <zmk/behavior.h> // zmk_behavior_local_id_t

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
    ZMK_BEHAVIOR_RT_FIELD_INT,       // int (int-range schema)
    ZMK_BEHAVIOR_RT_FIELD_ENUM,      // enum stored as int (enum-options schema)
    ZMK_BEHAVIOR_RT_FIELD_BOOL,      // bool (bool schema)
    ZMK_BEHAVIOR_RT_FIELD_POSITIONS, // int32_t[] + len (key-position-array schema)
};

/** Capacity of a runtime-editable key-position array. A pool slot's positions
 * array is allocated at this length (the DT placeholder sizes the flexible array
 * member), and this is the proto KeyPositions.positions max_count — they MUST
 * match. Manicule54's hold-trigger-key-positions lists are ~24 long. */
#define ZMK_BEHAVIOR_RUNTIME_POSITIONS_MAX 32

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

    // POSITIONS: `offset` points at the int32_t[] array; `len_offset` at its
    // companion int32_t length member; `positions_max` is the array capacity
    // (reported as the KeyPositionArray schema and enforced on decode).
    size_t len_offset;
    uint32_t positions_max;
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

/** Capacity (including the null terminator) of a kind string — matches the proto
 * CustomBehavior.kind max_size. Stored in each NVS record as a sanity guard. */
#define ZMK_BEHAVIOR_RUNTIME_KIND_SIZE 24

/** Per-slot mutable RAM state. The slot struct itself is const (ROM), so the
 * bits that change at runtime — whether the slot has been claimed as a real
 * behaviour (M4), its user-given display name, and whether it has edits not yet
 * persisted to NVS (M6) — live here. */
struct zmk_behavior_runtime_state {
    bool active;                               // claimed via add_custom_behavior?
    bool dirty;                                // edited since last save (M6)
    bool tombstoned;                           // a deleted FACTORY slot, saved (M12)
    char name[ZMK_BEHAVIOR_RUNTIME_NAME_SIZE]; // user display name (empty until claimed)
};

/** One runtime-editable pool slot: a spare device + its RAM config + descriptor. */
struct zmk_behavior_runtime_slot {
    const struct device *dev; // the spare DT behaviour instance
    void *config;             // RAM config object for `dev` (kind-specific layout)
    const struct zmk_behavior_runtime_descriptor *desc;
    struct zmk_behavior_runtime_state *state; // mutable per-slot RAM state

    // FACTORY slots (M12): a slot whose DT node sets `runtime-default-active`
    // boots claimed with `default_name` + its DT config, unless an NVS record or
    // tombstone overrides that. Plain spares leave these false/NULL.
    bool default_active;
    const char *default_name; // DT display-name, copied into state->name when seeded
};

/** Register a pool slot into the iterable section walked by the Studio subsystem.
 * Allocates the slot's mutable RAM state (claimed flag + name) alongside it.
 * `_default_active` / `_default_name` mark a FACTORY slot (M12). */
#define ZMK_BEHAVIOR_RUNTIME_SLOT_DEFINE_SEEDED(name, _dev, _config, _desc, _default_active,        \
                                                _default_name)                                      \
    static struct zmk_behavior_runtime_state name##_state = {0};                                   \
    static const STRUCT_SECTION_ITERABLE(zmk_behavior_runtime_slot, name) = {                       \
        .dev = (_dev),                                                                              \
        .config = (_config),                                                                        \
        .desc = (_desc),                                                                            \
        .state = &name##_state,                                                                     \
        .default_active = (_default_active),                                                        \
        .default_name = (_default_name),                                                            \
    }

/** Register a plain (non-factory) pool slot. */
#define ZMK_BEHAVIOR_RUNTIME_SLOT_DEFINE(name, _dev, _config, _desc)                                \
    ZMK_BEHAVIOR_RUNTIME_SLOT_DEFINE_SEEDED(name, _dev, _config, _desc, false, NULL)

/**
 * Persistence (M6, see app/src/behavior_runtime.c). Edits made via the Studio
 * RPC (set/add) live only in each slot's RAM config until saved. These mirror
 * the combos persistence API (zmk_combos_save_changes etc.); the Studio behaviour
 * subsystem's save/discard/check handlers call straight into them, and the
 * subsystem marks a slot dirty after every successful set/add. Persistence is
 * descriptor-driven (it iterates each slot's fields), so it stays kind-agnostic.
 */

/** Flag the slot owning `local_id` as having unsaved edits. No-op if no slot
 * matches. Called by set_custom_behavior / add_custom_behavior. */
void zmk_behavior_runtime_mark_dirty(zmk_behavior_local_id_t local_id);

/** Non-zero if any slot has edits not yet persisted to NVS. */
int zmk_behavior_runtime_check_unsaved_changes(void);

/** Persist every dirty slot to NVS (active slots written, freed slots deleted)
 * and clear their dirty bits. 0 on success, negative errno on failure. */
int zmk_behavior_runtime_save_changes(void);

/** Revert all slots to their last-saved state (re-applying persisted records
 * over a cleared pool) and clear dirty bits. 0 on success, negative errno. */
int zmk_behavior_runtime_discard_changes(void);

/** Factory reset: delete every persisted slot record + tombstone and clear the
 * pool. Factory slots are re-seeded from devicetree on the next boot. */
int zmk_behavior_runtime_reset_settings(void);

/** Activate every FACTORY slot (runtime-default-active) that has neither a
 * persisted record nor a tombstone, copying its DT display-name into state.
 * Idempotent (skips already-active slots). Run after settings load so user NVS
 * edits/deletes win; safe to call again after discard. The slot's config is the
 * DT default already in RAM (set at device init), so only state is touched. */
void zmk_behavior_runtime_seed_factory_defaults(void);
