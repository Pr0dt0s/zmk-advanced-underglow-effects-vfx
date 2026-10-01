/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/sync_proto.h>

static void put32(uint8_t *p, uint32_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
    p[2] = (uint8_t)(v >> 16);
    p[3] = (uint8_t)(v >> 24);
}

static uint32_t get32(const uint8_t *p) {
    return (uint32_t)p[0] | ((uint32_t)p[1] << 8) | ((uint32_t)p[2] << 16) | ((uint32_t)p[3] << 24);
}

static void put16(uint8_t *p, uint16_t v) {
    p[0] = (uint8_t)v;
    p[1] = (uint8_t)(v >> 8);
}

static uint16_t get16(const uint8_t *p) { return (uint16_t)(p[0] | (p[1] << 8)); }

uint8_t vfx_sync_encode_check(uint8_t target, uint32_t hash, uint8_t *out) {
    out[0] = VFX_SYNC_MSG_CHECK;
    out[1] = target;
    put32(&out[2], hash);

    return VFX_SYNC_CHECK_LEN;
}

bool vfx_sync_decode_check(const uint8_t *msg, uint8_t len, uint8_t *target, uint32_t *hash) {
    if (len < VFX_SYNC_CHECK_LEN || msg[0] != VFX_SYNC_MSG_CHECK) {
        return false;
    }

    *target = msg[1];
    *hash = get32(&msg[2]);

    return true;
}

uint8_t vfx_sync_encode_report(const struct vfx_sync_report *r, uint8_t *out) {
    out[0] = VFX_SYNC_REPORT_VERSION;
    out[1] = VFX_SYNC_REPORT_KIND_HASH;
    out[2] = r->target;
    out[3] = r->match ? 1 : 0;
    put32(&out[4], r->hash);
    put16(&out[8], r->hash_ok);
    put16(&out[10], r->hash_bad);
    put16(&out[12], r->incomplete);
    put16(&out[14], r->orphan);

    return VFX_SYNC_REPORT_LEN;
}

bool vfx_sync_decode_report(const uint8_t *data, uint8_t len, struct vfx_sync_report *r) {
    if (len < VFX_SYNC_REPORT_LEN || data[0] != VFX_SYNC_REPORT_VERSION ||
        data[1] != VFX_SYNC_REPORT_KIND_HASH) {
        return false;
    }

    r->target = data[2];
    r->match = data[3] != 0;
    r->hash = get32(&data[4]);
    r->hash_ok = get16(&data[8]);
    r->hash_bad = get16(&data[10]);
    r->incomplete = get16(&data[12]);
    r->orphan = get16(&data[14]);

    return true;
}
