/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>

#define ZMK_COMBOS_UTIL_ONE(n) +1

// Number of combos defined in devicetree.
#define ZMK_COMBOS_DT_LEN                                                                          \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(zmk_combos),                                             \
                (0 DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), ZMK_COMBOS_UTIL_ONE)),     \
                (0))

// The combo virtual-key-position block (see virtual_key_position.h) reserves
// exactly ZMK_COMBOS_LEN positions. With runtime editing on, every pool slot
// must own a stable virtual position equal to its pool index, so we widen this
// to cover the whole pool (A0). Getting this wrong silently collides combo
// behavior dispatch with the input-processor positions that follow.
#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)
#define ZMK_COMBOS_LEN MAX(ZMK_COMBOS_DT_LEN, CONFIG_ZMK_COMBO_MAX_COMBOS)
#else
#define ZMK_COMBOS_LEN ZMK_COMBOS_DT_LEN
#endif

// Upper bound on key positions per combo carried in the public DTO below. M1 is
// read-only; M2/M3 reconcile this with the DT-derived MAX_COMBO_KEYS (combo.c)
// and the runtime Kconfig (ZMK_COMBO_MAX_KEYS_PER_COMBO_RUNTIME, A1). Combos with
// more positions than this are truncated by the accessor.
#define ZMK_COMBO_MAX_KEYS 16

// Public, header-visible combo DTO. The behavior is expressed as a
// zmk_behavior_binding (device name + params) so it resolves by name at invoke
// time, mirroring the keymap. Filled by the read accessors below.
struct zmk_combo {
    int32_t key_positions[ZMK_COMBO_MAX_KEYS];
    uint8_t key_position_len;
    int32_t timeout_ms;
    int32_t require_prior_idle_ms;
    uint32_t layer_mask;
    bool slow_release;
    struct zmk_behavior_binding behavior;
};

// Number of pool slots to iterate when enumerating combos. With runtime editing
// on this is the pool capacity; callers skip slots that zmk_combos_get reports
// as empty (-ENOENT). Without runtime editing it's the DT array length.
size_t zmk_combos_get_count(void);

// Runtime combo pool capacity (max combos that can exist). Mirrors
// zmk_combos_get_count today (no add/delete yet) but is the value the client
// should treat as max_combos.
size_t zmk_combos_get_capacity(void);

// Effective maximum key positions per combo: MAX(DT-derived, runtime Kconfig),
// clamped to the DTO capacity ZMK_COMBO_MAX_KEYS.
size_t zmk_combos_get_max_keys(void);

// Copy the combo at the given index into *out. Returns 0 on success, -EINVAL if
// idx is out of range, or -ENOENT if the pool slot is currently empty.
int zmk_combos_get(uint16_t idx, struct zmk_combo *out);

// Replace the combo occupying pool slot idx in place. Validates key positions
// (< ZMK_KEYMAP_LEN, count in [1, effective max]) and the binding. If the slot
// is currently an active (pressed) combo it is force-released first. Rebuilds
// the combo lookup table. Marks the slot as having unsaved changes. Returns 0 on
// success, -EINVAL on invalid input, -ENOTSUP when runtime editing is disabled.
int zmk_combos_set(uint16_t idx, const struct zmk_combo *combo);

// Create a new combo in the lowest free pool slot (M4). Validates exactly like
// zmk_combos_set, marks the slot used + dirty, and rebuilds the lookup table.
// Returns the assigned pool index (>= 0) on success, -ENOSPC when the pool is
// full, -EINVAL on invalid input, or -ENOTSUP when runtime editing is disabled.
int zmk_combos_add(const struct zmk_combo *combo);

// Delete the combo occupying pool slot idx (M4): force-release it if currently
// pressed, free + zero the slot, mark it dirty, and rebuild the lookup table.
// Returns 0 on success, -EINVAL if idx is out of range, -ENOENT if the slot is
// already empty, or -ENOTSUP when runtime editing is disabled. A persisted
// deletion of a devicetree (stock) combo is recorded as a tombstone by
// zmk_combos_save_changes so it does not resurrect on reboot.
int zmk_combos_remove(uint16_t idx);

// Returns non-zero if any pool slot has unsaved (un-persisted) changes.
int zmk_combos_check_unsaved_changes(void);

// Persist every dirty pool slot to NVS ("combos/c/<idx>"), clearing its dirty
// bit. Returns 0 on success, a negative errno on failure (-ENOSPC when the
// settings partition is full), or -ENOTSUP when runtime editing is disabled.
int zmk_combos_save_changes(void);

// Drop all unsaved edits: reseed the pool from the devicetree defaults, re-apply
// the persisted NVS records on top, and clear the dirty bits. Returns 0 on
// success, a negative errno on failure, or -ENOTSUP when runtime editing is off.
int zmk_combos_discard_changes(void);

// Delete all persisted combo records and reseed the pool from the devicetree
// defaults (the Studio settings-reset hook). Returns 0, or -ENOTSUP when runtime
// editing is disabled.
int zmk_combos_reset_settings(void);
