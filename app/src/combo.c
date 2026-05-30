/*
 * Copyright (c) 2020 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_combos

#include <zephyr/device.h>
#include <zephyr/logging/log.h>
#include <zephyr/sys/dlist.h>
#include <zephyr/sys/util.h>
#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>

#include <drivers/behavior.h>

#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/events/keycode_state_changed.h>
#include <zmk/hid.h>
#include <zmk/matrix.h>
#include <zmk/keymap.h>
#include <zmk/virtual_key_position.h>

LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

// Whether any combos are defined in devicetree. The runtime-editing pool can run
// with zero devicetree combos (the common Studio case: build empty, add all at
// runtime), so the file body is gated on "DT combos OR runtime editing". The
// few constructs that genuinely need a devicetree instance (the stock seed array
// and the DT-derived key count) are sub-gated on HAS_DT_COMBOS below.
#define HAS_DT_COMBOS DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if HAS_DT_COMBOS || IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)

#if CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO > 0

#warning                                                                                           \
    "CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO is deprecated, and is auto-calculated from the devicetree now."

#endif

#if CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY > 0

#warning "CONFIG_ZMK_COMBO_MAX_COMBOS_PER_KEY is deprecated, and is auto-calculated."

#endif

#define COMBOS_KEYS_BYTE_ARRAY(node_id)                                                            \
    uint8_t _CONCAT(combo_prop_, node_id)[DT_PROP_LEN(node_id, key_positions)];

#if HAS_DT_COMBOS
#define DT_MAX_COMBO_KEYS sizeof(union {DT_INST_FOREACH_CHILD(0, COMBOS_KEYS_BYTE_ARRAY)})
#else
// No devicetree combos: nothing to derive a key count from. MAX_COMBO_KEYS falls
// back entirely to the runtime Kconfig (MAX(0, ...) below).
#define DT_MAX_COMBO_KEYS 0
#endif

// Effective per-combo key capacity, used to size combo_cfg/active_combo storage.
// With runtime editing on, users can create combos with more keys than any
// devicetree combo, so widen the devicetree-derived maximum to the runtime
// Kconfig (A1).
#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)
#define MAX_COMBO_KEYS MAX(DT_MAX_COMBO_KEYS, CONFIG_ZMK_COMBO_MAX_KEYS_PER_COMBO_RUNTIME)
#else
#define MAX_COMBO_KEYS DT_MAX_COMBO_KEYS
#endif

struct combo_cfg {
    int32_t key_positions[MAX_COMBO_KEYS];
    int16_t key_position_len;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    struct zmk_behavior_binding behavior;
    // if slow release is set, the combo releases when the last key is released.
    // otherwise, the combo releases when the first key is released.
    bool slow_release;
};

struct active_combo {
    uint16_t combo_idx;
    // key_positions_pressed is filled with key_positions when the combo is pressed.
    // The keys are removed from this array when they are released.
    // Once this array is empty, the behavior is released.
    uint16_t key_positions_pressed_count;
    struct zmk_position_state_changed_event key_positions_pressed[MAX_COMBO_KEYS];
};

#define PROP_BIT_AT_IDX(n, prop, idx) BIT(DT_PROP_BY_IDX(n, prop, idx))

#define NODE_PROP_BITMASK(n, prop)                                                                 \
    COND_CODE_1(DT_NODE_HAS_PROP(n, prop),                                                         \
                (DT_FOREACH_PROP_ELEM_SEP(n, prop, PROP_BIT_AT_IDX, (|))), (0))

#define GET_KEY_POSITION_MASK_PORTION(idx, n) ((NODE_PROP_BITMASK(n, key_positions) >> idx) & 0xFF)

#define COMBO_INST(n, positions)                                                                   \
    COND_CODE_1(IS_EQ(DT_PROP_LEN(n, key_positions), positions),                                   \
                (                                                                                  \
                    {                                                                              \
                        .timeout_ms = DT_PROP(n, timeout_ms),                                      \
                        .require_prior_idle_ms = DT_PROP(n, require_prior_idle_ms),                \
                        .key_positions = DT_PROP(n, key_positions),                                \
                        .key_position_len = DT_PROP_LEN(n, key_positions),                         \
                        .behavior = ZMK_KEYMAP_EXTRACT_BINDING(0, n),                              \
                        .slow_release = DT_PROP(n, slow_release),                                  \
                        .layer_mask = NODE_PROP_BITMASK(n, layers),                                \
                    }, ),                                                                          \
                ())

#define COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN(positions, _ignore)                              \
    DT_INST_FOREACH_CHILD_VARGS(0, COMBO_INST, positions)

// We do some magic here to generate the `combos` array by "key position length", looping
// by key position length and on each iteration, only include entries where the `key-positions`
// length matches.
// Doing so allows our bitmasks to be "shorted key positions list first" when searching for matches.
// `20` is chosen as a reasonable limit, since the theoretical maximum number of keys you might
// reasonably press simultaneously with 10 fingers is 20 keys, two keys per finger.
#define COMBO_ONE(n) +1

#define COMBO_CHILDREN_COUNT (0 DT_INST_FOREACH_CHILD(0, COMBO_ONE))

#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)

// Runtime-editable bounded pool. combo_stock keeps the devicetree defaults (for
// discard/reset, M3); combos[] is the mutable working set, seeded from it at
// init. combo_used marks which pool slots are live. The pool index is the
// combo's stable identity: == ZMK_VIRTUAL_KEY_POSITION_COMBO arg, == NVS key
// (M3), == active_combos[].combo_idx. Slots are never compacted; empty slots
// simply have no combo_lookup bit set, so the match loops never select them.
#define COMBO_POOL_SIZE CONFIG_ZMK_COMBO_MAX_COMBOS

#if HAS_DT_COMBOS
static const struct combo_cfg combo_stock[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};
#define COMBO_STOCK_COUNT ARRAY_SIZE(combo_stock)
#else
// No devicetree combos to seed from: the pool starts empty and is populated
// entirely at runtime. The 1-element dummy avoids a zero-length array; it is
// never read because COMBO_STOCK_COUNT is 0 (reseed guards on i < count).
static const struct combo_cfg combo_stock[1] = {0};
#define COMBO_STOCK_COUNT 0
#endif

static struct combo_cfg combos[COMBO_POOL_SIZE];
static bool combo_used[COMBO_POOL_SIZE];

// Dirty-bit array: one bit per pool slot, set when a slot is edited and cleared
// when it is persisted (M3). check_unsaved_changes is the OR of these bits.
#define COMBO_PENDING_ARRAY_SIZE DIV_ROUND_UP(COMBO_POOL_SIZE, 8)
static uint8_t combo_pending_changes[COMBO_PENDING_ARRAY_SIZE];

#else

#define COMBO_POOL_SIZE COMBO_CHILDREN_COUNT

static const struct combo_cfg combos[] = {
    LISTIFY(20, COMBO_CONFIGS_WITH_MATCHING_POSITIONS_LEN, (), 0)};

#endif // CONFIG_ZMK_COMBO_RUNTIME_EDITING

// We need at least 4 bytes to avoid alignment issues
#define BYTES_FOR_COMBOS_MASK DIV_ROUND_UP(COMBO_POOL_SIZE, 32)

uint8_t pressed_keys_count = 0;
// set of keys pressed
struct zmk_position_state_changed_event pressed_keys[MAX_COMBO_KEYS] = {};
// the set of candidate combos based on the currently pressed_keys
uint32_t candidates[BYTES_FOR_COMBOS_MASK];
// the last candidate that was completely pressed
int16_t fully_pressed_combo = INT16_MAX;
// a lookup dict that maps a key position to all combos on that position
uint32_t combo_lookup[ZMK_KEYMAP_LEN][BYTES_FOR_COMBOS_MASK] = {};
// combos that have been activated and still have (some) keys pressed
// this array is always contiguous from 0.
struct active_combo active_combos[CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS] = {};
uint8_t active_combo_count = 0;

struct k_work_delayable timeout_task;
int64_t timeout_task_timeout_at;

// this keeps track of the last non-combo, non-mod key tap
int64_t last_tapped_timestamp = INT32_MIN;
// this keeps track of the last time a combo was pressed
int64_t last_combo_timestamp = INT32_MIN;

static void store_last_tapped(int64_t timestamp) {
    if (timestamp > last_combo_timestamp) {
        last_tapped_timestamp = timestamp;
    }
}

// Store the combo key pointer in the combos array, one pointer for each key position
// The combos are sorted shortest-first, then by virtual-key-position.
static int initialize_combo(size_t index) {
    const struct combo_cfg *new_combo = &combos[index];

    for (size_t kp = 0; kp < new_combo->key_position_len; kp++) {
        sys_bitfield_set_bit((mem_addr_t)&combo_lookup[new_combo->key_positions[kp]], index);
    }

    return 0;
}

static bool combo_active_on_layer(const struct combo_cfg *combo, uint8_t layer) {
    if (!combo->layer_mask) {
        return true;
    }

    return combo->layer_mask & BIT(layer);
}

static bool is_quick_tap(const struct combo_cfg *combo, int64_t timestamp) {
    return (last_tapped_timestamp + combo->require_prior_idle_ms) > timestamp;
}

static int setup_candidates_for_first_keypress(int32_t position, int64_t timestamp) {
    int number_of_combo_candidates = 0;
    uint8_t highest_active_layer = zmk_keymap_highest_layer_active();

    for (size_t i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&combo_lookup[position], i)) {
            const struct combo_cfg *combo = &combos[i];
            if (combo_active_on_layer(combo, highest_active_layer) &&
                !is_quick_tap(combo, timestamp)) {
                sys_bitfield_set_bit((mem_addr_t)&candidates, i);
                number_of_combo_candidates++;
            }
            // LOG_DBG("combo timeout %d %d %d", position, i, candidates[i].timeout_at);
        }
    }

    return number_of_combo_candidates;
}

static inline uint8_t zero_one_or_more_bits(uint32_t field) {
    if (field == 0) {
        return 0;
    }
    if ((field & (field - 1)) == 0) {
        return 1;
    }
    return 2;
}

static int filter_candidates(int32_t position) {
    int matches = 0;
    for (int i = 0; i < BYTES_FOR_COMBOS_MASK; i++) {
        candidates[i] &= combo_lookup[position][i];
        if (matches < 2) {
            matches += zero_one_or_more_bits(candidates[i]);
        }
    }

    LOG_DBG("combo matches after filter %d", matches);
    return matches;
}

static int64_t first_candidate_timeout() {
    if (pressed_keys_count == 0) {
        return LONG_MAX;
    }

    int64_t first_timeout = LONG_MAX;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
            first_timeout = MIN(first_timeout, combos[i].timeout_ms);
        }
    }

    return pressed_keys[0].data.timestamp + first_timeout;
}

static inline bool candidate_is_completely_pressed(const struct combo_cfg *candidate) {
    // this code assumes set(pressed_keys) <= set(candidate->key_positions)
    // this invariant is enforced by filter_candidates
    // since events may have been reraised after clearing one or more slots at
    // the start of pressed_keys (see: release_pressed_keys), we have to check
    // that each key needed to trigger the combo was pressed, not just the last.
    return candidate->key_position_len == pressed_keys_count;
}

static int cleanup();

static int filter_timed_out_candidates(int64_t timestamp) {
    __ASSERT(pressed_keys_count > 0, "Searching for a candidate timeout with no keys pressed");

    int remaining_candidates = 0;
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        if (sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {

            if (pressed_keys[0].data.timestamp + combos[i].timeout_ms > timestamp) {
                remaining_candidates++;
            } else {
                sys_bitfield_clear_bit((mem_addr_t)&candidates, i);
            }
        }
    }

    LOG_DBG(
        "after filtering out timed out combo candidates: remaining_candidates=%d timestamp=%lld",
        remaining_candidates, timestamp);

    return remaining_candidates;
}

static int capture_pressed_key(const struct zmk_position_state_changed *ev) {
    if (pressed_keys_count == MAX_COMBO_KEYS) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    pressed_keys[pressed_keys_count++] = copy_raised_zmk_position_state_changed(ev);
    return ZMK_EV_EVENT_CAPTURED;
}

const struct zmk_listener zmk_listener_combo;

static int release_pressed_keys() {
    uint8_t count = pressed_keys_count;
    pressed_keys_count = 0;
    for (int i = 0; i < count; i++) {
        struct zmk_position_state_changed_event *ev = &pressed_keys[i];
        if (i == 0) {
            LOG_DBG("combo: releasing position event %d", ev->data.position);
            ZMK_EVENT_RELEASE(*ev);
        } else {
            // reprocess events (see tests/combo/fully-overlapping-combos-3 for why this is needed)
            LOG_DBG("combo: reraising position event %d", ev->data.position);
            ZMK_EVENT_RAISE(*ev);
        }
    }

    return count;
}

static inline int press_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                       int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    last_combo_timestamp = timestamp;

    return zmk_behavior_invoke_binding(&combo->behavior, event, true);
}

static inline int release_combo_behavior(int combo_idx, const struct combo_cfg *combo,
                                         int32_t timestamp) {
    struct zmk_behavior_binding_event event = {
        .position = ZMK_VIRTUAL_KEY_POSITION_COMBO(combo_idx),
        .timestamp = timestamp,
#if IS_ENABLED(CONFIG_ZMK_SPLIT)
        .source = ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL,
#endif
    };

    return zmk_behavior_invoke_binding(&combo->behavior, event, false);
}

static void move_pressed_keys_to_active_combo(struct active_combo *active_combo) {

    int combo_length = MIN(pressed_keys_count, combos[active_combo->combo_idx].key_position_len);
    for (int i = 0; i < combo_length; i++) {
        active_combo->key_positions_pressed[i] = pressed_keys[i];
    }
    active_combo->key_positions_pressed_count = combo_length;

    // move any other pressed keys up
    for (int i = 0; i + combo_length < pressed_keys_count; i++) {
        pressed_keys[i] = pressed_keys[i + combo_length];
    }

    pressed_keys_count -= combo_length;
}

static struct active_combo *store_active_combo(int32_t combo_idx) {
    for (int i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        if (active_combos[i].combo_idx == UINT16_MAX) {
            active_combos[i].combo_idx = combo_idx;
            active_combo_count++;
            return &active_combos[i];
        }
    }
    LOG_ERR("Unable to store combo; already %d active. Increase "
            "CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS",
            CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS);
    return NULL;
}

static void activate_combo(int combo_idx) {
    struct active_combo *active_combo = store_active_combo(combo_idx);
    if (active_combo == NULL) {
        // unable to store combo
        release_pressed_keys();
        return;
    }
    move_pressed_keys_to_active_combo(active_combo);
    press_combo_behavior(combo_idx, &combos[combo_idx],
                         active_combo->key_positions_pressed[0].data.timestamp);
}

static void deactivate_combo(int active_combo_index) {
    active_combo_count--;
    if (active_combo_index != active_combo_count) {
        memcpy(&active_combos[active_combo_index], &active_combos[active_combo_count],
               sizeof(struct active_combo));
    }
    active_combos[active_combo_count] = (struct active_combo){0};
    active_combos[active_combo_count].combo_idx = UINT16_MAX;
}

/* returns true if a key was released. */
static bool release_combo_key(int32_t position, int64_t timestamp) {
    for (int combo_idx = 0; combo_idx < active_combo_count; combo_idx++) {
        struct active_combo *active_combo = &active_combos[combo_idx];

        bool key_released = false;
        bool all_keys_pressed = active_combo->key_positions_pressed_count ==
                                combos[active_combo->combo_idx].key_position_len;
        bool all_keys_released = true;
        for (int i = 0; i < active_combo->key_positions_pressed_count; i++) {
            if (key_released) {
                active_combo->key_positions_pressed[i - 1] = active_combo->key_positions_pressed[i];
                all_keys_released = false;
            } else if (active_combo->key_positions_pressed[i].data.position != position) {
                all_keys_released = false;
            } else { // position matches
                key_released = true;
            }
        }

        if (key_released) {
            active_combo->key_positions_pressed_count--;
            const struct combo_cfg *c = &combos[active_combo->combo_idx];
            if ((c->slow_release && all_keys_released) || (!c->slow_release && all_keys_pressed)) {
                release_combo_behavior(active_combo->combo_idx, c, timestamp);
            }
            if (all_keys_released) {
                deactivate_combo(combo_idx);
            }
            return true;
        }
    }
    return false;
}

static int cleanup() {
    k_work_cancel_delayable(&timeout_task);
    memset(candidates, 0, BYTES_FOR_COMBOS_MASK * sizeof(uint32_t));
    if (fully_pressed_combo != INT16_MAX) {
        activate_combo(fully_pressed_combo);
        fully_pressed_combo = INT16_MAX;
    }
    return release_pressed_keys();
}

static void update_timeout_task() {
    int64_t first_timeout = first_candidate_timeout();
    if (timeout_task_timeout_at == first_timeout) {
        return;
    }
    if (first_timeout == LLONG_MAX) {
        timeout_task_timeout_at = 0;
        k_work_cancel_delayable(&timeout_task);
        return;
    }
    if (k_work_schedule(&timeout_task, K_MSEC(first_timeout - k_uptime_get())) >= 0) {
        timeout_task_timeout_at = first_timeout;
    }
}

static int position_state_down(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int num_candidates;
    if (!pressed_keys_count) {
        num_candidates = setup_candidates_for_first_keypress(data->position, data->timestamp);
        if (num_candidates == 0) {
            return ZMK_EV_EVENT_BUBBLE;
        }
    } else {
        filter_timed_out_candidates(data->timestamp);
        num_candidates = filter_candidates(data->position);
    }

    LOG_DBG("combo: capturing position event %d", data->position);
    int ret = capture_pressed_key(data);
    update_timeout_task();

    if (num_candidates) {
        // Select the shortest completely-pressed combo (lowest index breaks
        // ties). The legacy code relied on the combos array being grouped
        // shortest-first and just took the first candidate bit. The runtime
        // pool can't preserve that ordering, so we scan all candidates and pick
        // by key_position_len explicitly - otherwise a shorter, fully-pressed
        // combo sitting at a higher slot index would be skipped.
        int best = -1;
        for (int i = 0; i < ARRAY_SIZE(combos); i++) {
            if (!sys_bitfield_test_bit((mem_addr_t)&candidates, i)) {
                continue;
            }
            const struct combo_cfg *candidate_combo = &combos[i];
            if (candidate_is_completely_pressed(candidate_combo) &&
                (best < 0 || candidate_combo->key_position_len < combos[best].key_position_len)) {
                best = i;
            }
        }

        if (best >= 0) {
            fully_pressed_combo = best;
            if (num_candidates == 1) {
                cleanup();
            }
        }

        return ret;
    } else {
        cleanup();
        return ret;
    }

    return -EINVAL;
}

static int position_state_up(const zmk_event_t *ev, struct zmk_position_state_changed *data) {
    int released_keys = cleanup();
    if (release_combo_key(data->position, data->timestamp)) {
        return ZMK_EV_EVENT_HANDLED;
    }
    if (released_keys > 1) {
        // The second and further key down events are re-raised. To preserve
        // correct order for e.g. hold-taps, reraise the key up event too.
        struct zmk_position_state_changed_event dupe_ev =
            copy_raised_zmk_position_state_changed(data);
        ZMK_EVENT_RAISE(dupe_ev);
        return ZMK_EV_EVENT_CAPTURED;
    }
    return ZMK_EV_EVENT_BUBBLE;
}

static void combo_timeout_handler(struct k_work *item) {
    if (timeout_task_timeout_at == 0 || k_uptime_get() < timeout_task_timeout_at) {
        // timer was cancelled or rescheduled.
        return;
    }
    if (filter_timed_out_candidates(timeout_task_timeout_at) == 0) {
        LOG_DBG("CLEANUP!");
        cleanup();
    }

    LOG_DBG("ABOUT TO UPDATE IN TIMEOUT");
    update_timeout_task();
}

static int position_state_changed_listener(const zmk_event_t *ev) {
    struct zmk_position_state_changed *data = as_zmk_position_state_changed(ev);
    if (data == NULL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    if (data->state) { // keydown
        return position_state_down(ev, data);
    } else { // keyup
        return position_state_up(ev, data);
    }
}

static int keycode_state_changed_listener(const zmk_event_t *eh) {
    struct zmk_keycode_state_changed *ev = as_zmk_keycode_state_changed(eh);
    if (ev->state && !is_mod(ev->usage_page, ev->keycode)) {
        store_last_tapped(ev->timestamp);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

int behavior_combo_listener(const zmk_event_t *eh) {
    if (as_zmk_position_state_changed(eh) != NULL) {
        return position_state_changed_listener(eh);
    } else if (as_zmk_keycode_state_changed(eh) != NULL) {
        return keycode_state_changed_listener(eh);
    }
    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(combo, behavior_combo_listener);
ZMK_SUBSCRIPTION(combo, zmk_position_state_changed);
ZMK_SUBSCRIPTION(combo, zmk_keycode_state_changed);

#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)

// Rebuild the whole key-position -> combos lookup table from the used pool
// slots. Cold path (only on init and after an edit), so a full rebuild is fine.
// Callers must hold k_sched_lock so the match loops never observe a partially
// cleared table.
static void rebuild_combo_lookup(void) {
    memset(combo_lookup, 0, sizeof(combo_lookup));
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        if (combo_used[i]) {
            initialize_combo(i);
        }
    }
}

// Reset the working pool to the devicetree defaults: slots backed by a stock
// combo are restored and marked used; all other slots are cleared and freed.
// Used at init and as the first step of discard/reset before re-applying NVS.
// Callers must hold k_sched_lock (it mutates the pool the match loops read).
static void reseed_combos_from_stock(void) {
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        if (i < COMBO_STOCK_COUNT) {
            combos[i] = combo_stock[i];
            combo_used[i] = true;
        } else {
            combos[i] = (struct combo_cfg){0};
            combo_used[i] = false;
        }
    }
}

// If pool slot idx is currently a pressed (active) combo, force-release its
// behavior against the OLD definition and free the active-combo entry before
// the slot is mutated. Invokes a behavior, so it runs outside k_sched_lock.
static void release_active_combo_if_present(uint16_t idx) {
    for (int i = 0; i < active_combo_count; i++) {
        if (active_combos[i].combo_idx == idx) {
            release_combo_behavior(idx, &combos[idx], k_uptime_get());
            deactivate_combo(i);
            return;
        }
    }
}

#endif // CONFIG_ZMK_COMBO_RUNTIME_EDITING

static int combo_init(void) {
    for (size_t i = 0; i < CONFIG_ZMK_COMBO_MAX_PRESSED_COMBOS; i++) {
        active_combos[i].combo_idx = UINT16_MAX;
    }

    k_work_init_delayable(&timeout_task, combo_timeout_handler);

#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)
    // Seed the mutable pool from the devicetree defaults. Saved NVS edits are
    // applied later by the settings handler during settings_load() in main().
    LOG_WRN("Have %d combos (pool size %d)!", (int)COMBO_STOCK_COUNT, COMBO_POOL_SIZE);
    k_sched_lock();
    reseed_combos_from_stock();
    rebuild_combo_lookup();
    k_sched_unlock();
#else
    LOG_WRN("Have %d combos!", ARRAY_SIZE(combos));
    for (int i = 0; i < ARRAY_SIZE(combos); i++) {
        initialize_combo(i);
    }
#endif
    return 0;
}

SYS_INIT(combo_init, APPLICATION, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT);

// Public accessors. With runtime editing on, "count" is the pool capacity and
// callers skip empty slots via the -ENOENT return from zmk_combos_get.
size_t zmk_combos_get_count(void) { return COMBO_POOL_SIZE; }

size_t zmk_combos_get_capacity(void) { return COMBO_POOL_SIZE; }

size_t zmk_combos_get_max_keys(void) { return MIN((size_t)MAX_COMBO_KEYS, (size_t)ZMK_COMBO_MAX_KEYS); }

int zmk_combos_get(uint16_t idx, struct zmk_combo *out) {
    if (idx >= COMBO_POOL_SIZE) {
        return -EINVAL;
    }

#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)
    if (!combo_used[idx]) {
        return -ENOENT;
    }
#endif

    const struct combo_cfg *c = &combos[idx];

    out->key_position_len = MIN(c->key_position_len, (int16_t)ZMK_COMBO_MAX_KEYS);
    for (int i = 0; i < out->key_position_len; i++) {
        out->key_positions[i] = c->key_positions[i];
    }
    out->timeout_ms = c->timeout_ms;
    out->require_prior_idle_ms = c->require_prior_idle_ms;
    out->layer_mask = c->layer_mask;
    out->slow_release = c->slow_release;
    out->behavior = c->behavior;

    return 0;
}

#if IS_ENABLED(CONFIG_ZMK_COMBO_RUNTIME_EDITING)

int zmk_combos_set(uint16_t idx, const struct zmk_combo *combo) {
    if (idx >= COMBO_POOL_SIZE) {
        return -EINVAL;
    }

    if (combo->key_position_len < 1 || combo->key_position_len > zmk_combos_get_max_keys()) {
        LOG_WRN("Rejecting combo %d: key count %d out of range", idx, combo->key_position_len);
        return -EINVAL;
    }

    for (int i = 0; i < combo->key_position_len; i++) {
        if (combo->key_positions[i] < 0 || combo->key_positions[i] >= ZMK_KEYMAP_LEN) {
            LOG_WRN("Rejecting combo %d: key position %d out of range", idx,
                    combo->key_positions[i]);
            return -EINVAL;
        }
    }

    // The binding itself is validated by the RPC layer (zmk_behavior_validate_binding)
    // before this is called, mirroring the keymap set path.

    struct combo_cfg cfg = {
        .timeout_ms = combo->timeout_ms,
        .require_prior_idle_ms = combo->require_prior_idle_ms,
        .key_position_len = combo->key_position_len,
        .layer_mask = combo->layer_mask,
        .slow_release = combo->slow_release,
        .behavior = combo->behavior,
    };
    for (int i = 0; i < combo->key_position_len; i++) {
        cfg.key_positions[i] = combo->key_positions[i];
    }

    // Clean up any in-flight press of this slot against its old definition
    // before swapping it out (invokes a behavior, so do it before locking).
    release_active_combo_if_present(idx);

    k_sched_lock();
    combos[idx] = cfg;
    combo_used[idx] = true;
    WRITE_BIT(combo_pending_changes[idx / 8], idx % 8, 1);
    rebuild_combo_lookup();
    k_sched_unlock();

    return 0;
}

int zmk_combos_add(const struct zmk_combo *combo) {
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        if (!combo_used[i]) {
            // zmk_combos_set validates the input, marks the slot used + dirty,
            // and rebuilds the lookup. A fresh slot is never an active combo, so
            // its internal release-if-present is a no-op here.
            int ret = zmk_combos_set((uint16_t)i, combo);
            if (ret < 0) {
                return ret;
            }
            return (int)i;
        }
    }

    return -ENOSPC;
}

int zmk_combos_remove(uint16_t idx) {
    if (idx >= COMBO_POOL_SIZE) {
        return -EINVAL;
    }
    if (!combo_used[idx]) {
        return -ENOENT;
    }

    // If this slot is currently pressed, release its behavior against the old
    // definition before freeing it (invokes a behavior, so do it before locking).
    release_active_combo_if_present(idx);

    k_sched_lock();
    combos[idx] = (struct combo_cfg){0};
    combo_used[idx] = false;
    WRITE_BIT(combo_pending_changes[idx / 8], idx % 8, 1);
    rebuild_combo_lookup();
    k_sched_unlock();

    return 0;
}

// On-disk record for one combo slot. __packed + a save-only-used-length trim
// (see zmk_combos_save_changes) keeps NVS writes small. Behavior is stored as a
// local id (resolved to a device name at load time, mirroring the keymap), so
// in-place edits and reflashes stay valid.
struct zmk_combo_setting {
    uint8_t key_position_len;
    uint8_t slow_release;
    int16_t require_prior_idle_ms;
    int32_t timeout_ms;
    uint32_t layer_mask;
    zmk_behavior_local_id_t behavior_local_id;
    uint32_t param1;
    uint32_t param2;
    int16_t key_positions[MAX_COMBO_KEYS];
} __packed;

#define COMBO_SETTINGS_KEY "combos/c/%d"

int zmk_combos_check_unsaved_changes(void) {
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        if (combo_pending_changes[i / 8] & BIT(i % 8)) {
            return 1;
        }
    }
    return 0;
}

int zmk_combos_save_changes(void) {
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        if (!(combo_pending_changes[i / 8] & BIT(i % 8))) {
            continue;
        }

        char setting_name[16];
        sprintf(setting_name, COMBO_SETTINGS_KEY, (int)i);

        int ret;
        if (combo_used[i]) {
            const struct combo_cfg *c = &combos[i];
            struct zmk_combo_setting rec = {
                .key_position_len = (uint8_t)c->key_position_len,
                .slow_release = c->slow_release ? 1 : 0,
                .require_prior_idle_ms = c->require_prior_idle_ms,
                .timeout_ms = c->timeout_ms,
                .layer_mask = c->layer_mask,
                .behavior_local_id = zmk_behavior_get_local_id(c->behavior.behavior_dev),
                .param1 = c->behavior.param1,
                .param2 = c->behavior.param2,
            };
            int kp_count = MIN((int)c->key_position_len, MAX_COMBO_KEYS);
            for (int kp = 0; kp < kp_count; kp++) {
                rec.key_positions[kp] = (int16_t)c->key_positions[kp];
            }

            // Trim trailing unused key_positions slots (same trick as keymap).
            size_t len =
                offsetof(struct zmk_combo_setting, key_positions) + kp_count * sizeof(int16_t);

            ret = settings_save_one(setting_name, &rec, len);
        } else if (i < COMBO_STOCK_COUNT) {
            // Deleted a devicetree (stock) combo. A plain settings_delete is not
            // enough: on reboot combo_init reseeds stock slots as used, and with
            // no saved record to override slot i the deleted combo would
            // resurrect. Persist a tombstone (a record with key_position_len == 0,
            // which zmk_combos_set never produces) so the settings load handler
            // re-empties the slot.
            struct zmk_combo_setting tombstone = {0};
            size_t len = offsetof(struct zmk_combo_setting, key_positions);
            ret = settings_save_one(setting_name, &tombstone, len);
        } else {
            // Freed non-stock slot: nothing reseeds it, so just drop any record
            // it had. settings_delete on a never-saved key is harmless.
            ret = settings_delete(setting_name);
        }

        if (ret < 0) {
            LOG_ERR("Failed to persist combo %d (%d)", (int)i, ret);
            return ret;
        }

        WRITE_BIT(combo_pending_changes[i / 8], i % 8, 0);
    }

    return 0;
}

int zmk_combos_discard_changes(void) {
    k_sched_lock();
    reseed_combos_from_stock();
    k_sched_unlock();

    // Re-apply persisted edits over the stock pool via the settings handler.
    int ret = settings_load_subtree("combos");

    k_sched_lock();
    rebuild_combo_lookup();
    memset(combo_pending_changes, 0, sizeof(combo_pending_changes));
    k_sched_unlock();

    return ret;
}

int zmk_combos_reset_settings(void) {
    for (size_t i = 0; i < COMBO_POOL_SIZE; i++) {
        char setting_name[16];
        sprintf(setting_name, COMBO_SETTINGS_KEY, (int)i);
        settings_delete(setting_name);
    }

    k_sched_lock();
    reseed_combos_from_stock();
    memset(combo_pending_changes, 0, sizeof(combo_pending_changes));
    rebuild_combo_lookup();
    k_sched_unlock();

    return 0;
}

static int combo_handle_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (settings_name_steq(name, "c", &next) && next) {
        char *endptr;
        uint16_t idx = strtoul(next, &endptr, 10);
        if (*endptr != '\0') {
            LOG_WRN("Invalid combo index in settings key: %s", next);
            return -EINVAL;
        }
        if (idx >= COMBO_POOL_SIZE) {
            LOG_WRN("Combo settings index %d exceeds pool size %d", idx, COMBO_POOL_SIZE);
            return -EINVAL;
        }
        if (len > sizeof(struct zmk_combo_setting)) {
            LOG_ERR("Combo setting too large (got %d, max %d)", (int)len,
                    (int)sizeof(struct zmk_combo_setting));
            return -EINVAL;
        }

        struct zmk_combo_setting rec = {0};
        int err = read_cb(cb_arg, &rec, len);
        if (err <= 0) {
            LOG_ERR("Failed to read combo %d from settings (%d)", idx, err);
            return err;
        }

        if (rec.key_position_len == 0) {
            // Tombstone: a deleted stock combo (see zmk_combos_save_changes).
            // Override the stock reseed by emptying the slot.
            combos[idx] = (struct combo_cfg){0};
            combo_used[idx] = false;
            return 0;
        }

        const char *behavior_name =
            zmk_behavior_find_behavior_name_from_local_id(rec.behavior_local_id);
        if (!behavior_name) {
            LOG_WRN("Loaded combo %d but no behavior found for local id %d", idx,
                    rec.behavior_local_id);
        }

        struct combo_cfg cfg = {
            .key_position_len = rec.key_position_len,
            .slow_release = rec.slow_release != 0,
            .require_prior_idle_ms = rec.require_prior_idle_ms,
            .timeout_ms = rec.timeout_ms,
            .layer_mask = rec.layer_mask,
            .behavior =
                (struct zmk_behavior_binding){
                    .behavior_dev = behavior_name,
                    .param1 = rec.param1,
                    .param2 = rec.param2,
                },
        };
        int kp_count = MIN((int)rec.key_position_len, MAX_COMBO_KEYS);
        for (int kp = 0; kp < kp_count; kp++) {
            cfg.key_positions[kp] = rec.key_positions[kp];
        }

        // Mark the slot used (loaded values are persisted, not pending).
        combos[idx] = cfg;
        combo_used[idx] = true;
    }

    return 0;
}

static int combo_handle_commit(void) {
    k_sched_lock();
    rebuild_combo_lookup();
    k_sched_unlock();
    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(combos, "combos", NULL, combo_handle_set, combo_handle_commit, NULL);

#else

int zmk_combos_set(uint16_t idx, const struct zmk_combo *combo) {
    ARG_UNUSED(idx);
    ARG_UNUSED(combo);
    return -ENOTSUP;
}

int zmk_combos_add(const struct zmk_combo *combo) {
    ARG_UNUSED(combo);
    return -ENOTSUP;
}

int zmk_combos_remove(uint16_t idx) {
    ARG_UNUSED(idx);
    return -ENOTSUP;
}

int zmk_combos_check_unsaved_changes(void) { return 0; }
int zmk_combos_save_changes(void) { return -ENOTSUP; }
int zmk_combos_discard_changes(void) { return -ENOTSUP; }
int zmk_combos_reset_settings(void) { return -ENOTSUP; }

#endif // CONFIG_ZMK_COMBO_RUNTIME_EDITING

#else // !HAS_DT_COMBOS && !CONFIG_ZMK_COMBO_RUNTIME_EDITING

// Neither devicetree combos nor runtime editing: keep the read API linkable for
// the Studio combo subsystem (compiled whenever CONFIG_ZMK_STUDIO_RPC is on).
// With runtime editing enabled the zero-DT pool lives in the block above instead.
size_t zmk_combos_get_count(void) { return 0; }

size_t zmk_combos_get_capacity(void) { return 0; }

size_t zmk_combos_get_max_keys(void) { return ZMK_COMBO_MAX_KEYS; }

int zmk_combos_get(uint16_t idx, struct zmk_combo *out) {
    ARG_UNUSED(idx);
    ARG_UNUSED(out);
    return -EINVAL;
}

int zmk_combos_set(uint16_t idx, const struct zmk_combo *combo) {
    ARG_UNUSED(idx);
    ARG_UNUSED(combo);
    return -ENOTSUP;
}

int zmk_combos_add(const struct zmk_combo *combo) {
    ARG_UNUSED(combo);
    return -ENOTSUP;
}

int zmk_combos_remove(uint16_t idx) {
    ARG_UNUSED(idx);
    return -ENOTSUP;
}

int zmk_combos_check_unsaved_changes(void) { return 0; }
int zmk_combos_save_changes(void) { return -ENOTSUP; }
int zmk_combos_discard_changes(void) { return -ENOTSUP; }
int zmk_combos_reset_settings(void) { return -ENOTSUP; }

#endif
