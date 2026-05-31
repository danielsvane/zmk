/*
 * Copyright (c) 2024 The ZMK Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/logging/log.h>
LOG_MODULE_DECLARE(zmk, CONFIG_ZMK_LOG_LEVEL);

#include <errno.h>
#include <stdio.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/settings/settings.h>
#include <zephyr/sys/util.h>

#include <zmk/behavior.h>
#include <zmk/behavior_runtime.h>

// ---------------------------------------------------------------------------
// Warm persistence for the runtime-editable behaviour pool (M6).
//
// Edits made via the Studio RPC (set/add) live only in each slot's RAM config.
// This file persists them to NVS, keyed by the slot's POOL INDEX, and restores
// them on boot — mirroring combos' combo.c (save/discard/check/reset + a
// settings handler), but generically: a slot's config is (de)serialized by
// walking its descriptor's fields, so no behaviour kind is named here and a new
// kind (M10) needs no change in this file.
//
// Keying by pool index (not by local_id, despite the plan §3.6 hint): this build
// uses CONFIG_ZMK_BEHAVIOR_LOCAL_ID_TYPE_SETTINGS_TABLE, so a device's local_id
// is itself loaded from the "behavior" settings subtree. settings_load()
// dispatches NVS records in storage order, not subtree order, so at the moment
// the "rtbeh" records load the local-id map may not be populated yet — keying on
// a live local_id lookup would intermittently fail to restore. The pool index is
// a static, always-available section position, so it sidesteps the ordering
// hazard entirely. This is exactly what combo.c/keymap do (they key slot records
// by pool index and use local_id only for sub-binding VALUES, re-resolved in
// commit). local_id is still the wire identity (get/set use it); it is simply not
// the persistence key. See the M6 findings-log entry.
//
// Cold-boot re-resolution of sub-binding behaviours is M7 (rtbeh_commit is a
// stub for now); tombstones for deleted slots are M8. In M6 the pool starts
// fully unclaimed, so a freed (or never-claimed) slot simply has no record and
// nothing reseeds it — no tombstone is needed yet.
// ---------------------------------------------------------------------------

#define RTBEH_SETTINGS_SUBTREE "rtbeh"
#define RTBEH_SETTINGS_KEY RTBEH_SETTINGS_SUBTREE "/b/%u"

// On-disk format version, so a future layout change can be detected and ignored
// rather than misread.
#define RTBEH_RECORD_VERSION 1

// Fixed-size header; the variable, descriptor-serialized config tail follows it.
// Only ACTIVE (claimed) slots are written, so a record's presence implies active.
struct rtbeh_record_header {
    uint8_t version;
    char kind[ZMK_BEHAVIOR_RUNTIME_KIND_SIZE];
    char name[ZMK_BEHAVIOR_RUNTIME_NAME_SIZE];
} __packed;

// Worst case: header + every field. Positions is the big one (count + 32 ints).
// 7 scalar fields × 4 B + one positions field (4 + 32×4) keeps this comfortably
// under 256; size generously and assert.
#define RTBEH_CONFIG_TAIL_MAX (ZMK_BEHAVIOR_RUNTIME_POSITIONS_MAX * 4 + 4 + 16 * 4)
#define RTBEH_RECORD_MAX (sizeof(struct rtbeh_record_header) + RTBEH_CONFIG_TAIL_MAX)

static const struct zmk_behavior_runtime_slot *slot_for_local_id(zmk_behavior_local_id_t id) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (zmk_behavior_get_local_id(slot->dev->name) == id) {
            return slot;
        }
    }
    return NULL;
}

// --- generic config (de)serialization, descriptor-driven --------------------

// Append `n` bytes from `src` to buf at *cursor, advancing it; never overruns
// `cap`. Returns false on overflow (a programming error, given RTBEH_RECORD_MAX).
static bool buf_put(uint8_t *buf, size_t cap, size_t *cursor, const void *src, size_t n) {
    if (*cursor + n > cap) {
        return false;
    }
    memcpy(buf + *cursor, src, n);
    *cursor += n;
    return true;
}

// Read `n` bytes into `dst` from buf at *cursor; returns false if the record is
// shorter than expected (truncated / older layout) so the caller can stop.
static bool buf_get(const uint8_t *buf, size_t len, size_t *cursor, void *dst, size_t n) {
    if (*cursor + n > len) {
        return false;
    }
    memcpy(dst, buf + *cursor, n);
    *cursor += n;
    return true;
}

// Serialize a slot's RAM config into `buf` (the descriptor's fields, in order).
// Returns the byte length, or 0 on overflow.
static size_t serialize_config(const struct zmk_behavior_runtime_slot *slot, uint8_t *buf,
                               size_t cap) {
    const uint8_t *config = (const uint8_t *)slot->config;
    size_t cursor = 0;

    for (size_t i = 0; i < slot->desc->fields_len; i++) {
        const struct zmk_behavior_runtime_field *f = &slot->desc->fields[i];
        switch (f->type) {
        case ZMK_BEHAVIOR_RT_FIELD_INT:
        case ZMK_BEHAVIOR_RT_FIELD_ENUM: {
            // INT and int-sized ENUM both round-trip as an int32.
            int32_t v = *(const int *)(config + f->offset);
            if (!buf_put(buf, cap, &cursor, &v, sizeof(v))) {
                return 0;
            }
            break;
        }
        case ZMK_BEHAVIOR_RT_FIELD_BOOL: {
            uint8_t v = *(const bool *)(config + f->offset) ? 1 : 0;
            if (!buf_put(buf, cap, &cursor, &v, sizeof(v))) {
                return 0;
            }
            break;
        }
        case ZMK_BEHAVIOR_RT_FIELD_POSITIONS: {
            int32_t len = *(const int32_t *)(config + f->len_offset);
            uint32_t count = (uint32_t)CLAMP(len, 0, (int32_t)f->positions_max);
            const int32_t *arr = (const int32_t *)(config + f->offset);
            if (!buf_put(buf, cap, &cursor, &count, sizeof(count)) ||
                !buf_put(buf, cap, &cursor, arr, count * sizeof(int32_t))) {
                return 0;
            }
            break;
        }
        }
    }

    return cursor;
}

// Inverse of serialize_config: write a slot's RAM config from `buf`. Stops early
// (leaving later fields at their current values) if the record is truncated.
static void deserialize_config(const struct zmk_behavior_runtime_slot *slot, const uint8_t *buf,
                               size_t len) {
    uint8_t *config = (uint8_t *)slot->config;
    size_t cursor = 0;

    for (size_t i = 0; i < slot->desc->fields_len; i++) {
        const struct zmk_behavior_runtime_field *f = &slot->desc->fields[i];
        switch (f->type) {
        case ZMK_BEHAVIOR_RT_FIELD_INT:
        case ZMK_BEHAVIOR_RT_FIELD_ENUM: {
            int32_t v;
            if (!buf_get(buf, len, &cursor, &v, sizeof(v))) {
                return;
            }
            *(int *)(config + f->offset) = (int)v;
            break;
        }
        case ZMK_BEHAVIOR_RT_FIELD_BOOL: {
            uint8_t v;
            if (!buf_get(buf, len, &cursor, &v, sizeof(v))) {
                return;
            }
            *(bool *)(config + f->offset) = (v != 0);
            break;
        }
        case ZMK_BEHAVIOR_RT_FIELD_POSITIONS: {
            uint32_t count;
            if (!buf_get(buf, len, &cursor, &count, sizeof(count))) {
                return;
            }
            count = MIN(count, f->positions_max);
            int32_t *arr = (int32_t *)(config + f->offset);
            for (uint32_t p = 0; p < count; p++) {
                if (!buf_get(buf, len, &cursor, &arr[p], sizeof(int32_t))) {
                    *(int32_t *)(config + f->len_offset) = (int32_t)p;
                    return;
                }
            }
            *(int32_t *)(config + f->len_offset) = (int32_t)count;
            break;
        }
        }
    }
}

// --- public API -------------------------------------------------------------

void zmk_behavior_runtime_mark_dirty(zmk_behavior_local_id_t local_id) {
    const struct zmk_behavior_runtime_slot *slot = slot_for_local_id(local_id);
    if (slot) {
        slot->state->dirty = true;
    }
}

int zmk_behavior_runtime_check_unsaved_changes(void) {
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        if (slot->state->dirty) {
            return 1;
        }
    }
    return 0;
}

int zmk_behavior_runtime_save_changes(void) {
    size_t idx = 0;
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        size_t slot_idx = idx++;
        if (!slot->state->dirty) {
            continue;
        }

        char key[24];
        snprintf(key, sizeof(key), RTBEH_SETTINGS_KEY, (unsigned)slot_idx);

        int ret;
        if (slot->state->active) {
            uint8_t buf[RTBEH_RECORD_MAX];
            struct rtbeh_record_header *hdr = (struct rtbeh_record_header *)buf;
            *hdr = (struct rtbeh_record_header){.version = RTBEH_RECORD_VERSION};
            strncpy(hdr->kind, slot->desc->kind, sizeof(hdr->kind) - 1);
            strncpy(hdr->name, slot->state->name, sizeof(hdr->name) - 1);

            size_t tail = serialize_config(slot, buf + sizeof(*hdr), sizeof(buf) - sizeof(*hdr));
            ret = settings_save_one(key, buf, sizeof(*hdr) + tail);
        } else {
            // Freed / never-claimed slot: drop any record it had. settings_delete
            // on a never-saved key is harmless.
            ret = settings_delete(key);
        }

        if (ret < 0) {
            LOG_ERR("Failed to persist runtime behaviour %s (%d)", key, ret);
            return ret;
        }

        slot->state->dirty = false;
    }

    return 0;
}

int zmk_behavior_runtime_discard_changes(void) {
    k_sched_lock();
    // Clear the pool back to "unclaimed"; the persisted records below re-mark and
    // re-fill the slots that were saved. (Config of slots with no record keeps its
    // RAM contents, but an unclaimed slot is hidden, so that is invisible.)
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        slot->state->active = false;
        slot->state->name[0] = '\0';
        slot->state->dirty = false;
    }
    k_sched_unlock();

    // Re-apply persisted records over the cleared pool via the settings handler.
    int ret = settings_load_subtree(RTBEH_SETTINGS_SUBTREE);

    return ret;
}

int zmk_behavior_runtime_reset_settings(void) {
    size_t count = 0;
    STRUCT_SECTION_COUNT(zmk_behavior_runtime_slot, &count);
    for (size_t idx = 0; idx < count; idx++) {
        char key[24];
        snprintf(key, sizeof(key), RTBEH_SETTINGS_KEY, (unsigned)idx);
        settings_delete(key);
    }

    k_sched_lock();
    STRUCT_SECTION_FOREACH(zmk_behavior_runtime_slot, slot) {
        slot->state->active = false;
        slot->state->name[0] = '\0';
        slot->state->dirty = false;
    }
    k_sched_unlock();

    return 0;
}

// --- settings handler (boot restore + discard reload) -----------------------

static int rtbeh_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "b", &next) || !next) {
        return 0;
    }

    char *endptr;
    unsigned long idx = strtoul(next, &endptr, 10);
    if (*endptr != '\0') {
        LOG_WRN("Invalid runtime behaviour index in settings key: %s", next);
        return -EINVAL;
    }

    size_t count = 0;
    STRUCT_SECTION_COUNT(zmk_behavior_runtime_slot, &count);
    if (idx >= count) {
        LOG_WRN("Persisted runtime behaviour index %lu exceeds pool size %d; ignoring", idx,
                (int)count);
        return 0;
    }

    const struct zmk_behavior_runtime_slot *slot;
    STRUCT_SECTION_GET(zmk_behavior_runtime_slot, idx, &slot);

    if (len < sizeof(struct rtbeh_record_header) || len > RTBEH_RECORD_MAX) {
        LOG_ERR("Runtime behaviour record %lu has bad size %d", idx, (int)len);
        return -EINVAL;
    }

    uint8_t buf[RTBEH_RECORD_MAX];
    int err = read_cb(cb_arg, buf, len);
    if (err <= 0) {
        LOG_ERR("Failed to read runtime behaviour %lu from settings (%d)", idx, err);
        return err;
    }

    const struct rtbeh_record_header *hdr = (const struct rtbeh_record_header *)buf;
    if (hdr->version != RTBEH_RECORD_VERSION) {
        LOG_WRN("Runtime behaviour record %lu has version %d (expected %d); ignoring", idx,
                hdr->version, RTBEH_RECORD_VERSION);
        return 0;
    }
    // Guard against a firmware change that reassigned this index to a slot of a
    // different kind: don't deserialize a config laid out for another descriptor.
    if (strncmp(hdr->kind, slot->desc->kind, sizeof(hdr->kind)) != 0) {
        LOG_WRN("Persisted kind '%s' != slot kind '%s' at index %lu; ignoring", hdr->kind,
                slot->desc->kind, idx);
        return 0;
    }

    k_sched_lock();
    slot->state->active = true;
    strncpy(slot->state->name, hdr->name, sizeof(slot->state->name) - 1);
    slot->state->name[sizeof(slot->state->name) - 1] = '\0';
    deserialize_config(slot, buf + sizeof(*hdr), len - sizeof(*hdr));
    slot->state->dirty = false;
    k_sched_unlock();

    return 0;
}

// Settings commit runs after every subsystem's `set` callbacks, i.e. after the
// behavior local-id table has finished loading. This is where combos re-resolves
// a runtime combo's stored sub-binding local_id -> behavior_dev (combo.c
// a644b8b / combo_handle_commit), because the combo record can load before the
// local-id table on a cold boot and the set-time resolution would otherwise be
// lost with no retry.
//
// M7 (cold-boot) verified that the runtime-behaviour pool does NOT have that
// hazard for its CURRENT descriptor, so there is nothing to re-resolve here yet:
//   - Slot config is restored by rtbeh_set keyed on POOL INDEX, so it is
//     order-independent of the local-id table (the very reason for index keying).
//   - The slot's own findability as a binding target rides on the EXISTING
//     keymap/combo commit re-resolution: the pool slot is a compile-time DT
//     device with a stable local_id (LOCAL_ID_TYPE_SETTINGS_TABLE), and a keymap
//     or combo that binds it stores it by local_id and re-resolves it in its own
//     commit. Our handler restores only state->active + name + scalar config.
//   - hold-tap's hold/tap sub-bindings are fixed DT device-name strings
//     (DEVICE_DT_NAME of the phandles), not stored local_ids, so they need no
//     re-resolution.
//
// The combos-style re-resolution loop belongs here once M9 makes the hold/tap
// sub-bindings editable: that adds a BEHAVIOR_REF-typed descriptor field whose
// value is persisted as a behavior_local_id, and this commit_cb must then walk
// active slots' descriptor fields and resolve each stored local_id ->
// behavior_dev (mirroring combo_handle_commit), since those records can load
// before the local-id table.
static int rtbeh_commit(void) { return 0; }

SETTINGS_STATIC_HANDLER_DEFINE(rtbeh, RTBEH_SETTINGS_SUBTREE, NULL, rtbeh_set, rtbeh_commit, NULL);
