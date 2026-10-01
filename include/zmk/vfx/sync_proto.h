/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* The two messages the split halves exchange to find out whether a runtime
 * scene reached the peripheral intact. Plain byte encoders with no Zephyr
 * dependency, tested the way hid_protocol.h is.
 *
 * CHECK travels central to peripheral inside the split link, so it begins with
 * a byte that is never a SCENE_* op (those are below 0x40) and a peripheral
 * tells the two apart by it. The report travels back over the GATT report
 * channel (CONFIG_ZMK_VFX_SPLIT_REPORT).
 */

#define VFX_SYNC_MSG_CHECK 0x40
#define VFX_SYNC_CHECK_LEN 6

/* [VFX_SYNC_MSG_CHECK, target, hash32 LE] */
uint8_t vfx_sync_encode_check(uint8_t target, uint32_t hash, uint8_t *out);
bool vfx_sync_decode_check(const uint8_t *msg, uint8_t len, uint8_t *target, uint32_t *hash);

#define VFX_SYNC_REPORT_VERSION 1
#define VFX_SYNC_REPORT_KIND_HASH 1
#define VFX_SYNC_REPORT_LEN 16

/* A peripheral's answer to CHECK. The counters are the peripheral's own and
 * are never reset: hash comparisons that agreed and disagreed, and what its
 * link receiver has had to throw away.
 */
struct vfx_sync_report {
    uint8_t target;
    bool match;
    uint32_t hash; /* the peripheral's own hash of `target` */
    uint16_t hash_ok;
    uint16_t hash_bad;
    uint16_t incomplete;
    uint16_t orphan;
};

/* [version, kind, target, match, hash32 LE, 4 x counter u16 LE] */
uint8_t vfx_sync_encode_report(const struct vfx_sync_report *r, uint8_t *out);
bool vfx_sync_decode_report(const uint8_t *data, uint8_t len, struct vfx_sync_report *r);
