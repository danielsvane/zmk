/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <zephyr/devicetree.h>

#include <zmk/behavior.h>

#define ZMK_COMBOS_UTIL_ONE(n) +1

#define ZMK_COMBOS_LEN                                                                             \
    COND_CODE_1(DT_HAS_COMPAT_STATUS_OKAY(zmk_combos),                                             \
                (0 DT_FOREACH_CHILD_STATUS_OKAY(DT_INST(0, zmk_combos), ZMK_COMBOS_UTIL_ONE)),     \
                (0))

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

// Number of combos currently defined (M1: the compile-time DT array length).
size_t zmk_combos_get_count(void);

// Copy the combo at the given index into *out. Returns 0 on success or -EINVAL
// if idx is out of range.
int zmk_combos_get(uint16_t idx, struct zmk_combo *out);
