/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* A zone names the pixels a layer is allowed to touch, either as a contiguous
 * run or as an explicit list. Indices are local to this half's strip.
 */
struct vfx_zone {
    const uint8_t *pixels; /* NULL for a contiguous range */
    uint16_t start;        /* first index, when pixels == NULL */
    uint16_t len;
};

/* Strip index of the i'th pixel in the zone. Callers iterate i over
 * [0, zone->len) so this never needs to range check.
 */
static inline uint16_t vfx_zone_pixel(const struct vfx_zone *zone, uint16_t i) {
    return zone->pixels ? zone->pixels[i] : (uint16_t)(zone->start + i);
}

/* Length of a zone once trimmed to the strip it actually landed on. */
uint16_t vfx_zone_clamped_len(const struct vfx_zone *zone, uint16_t num_pixels);

/* A zone written as key positions rather than pixel indices.
 *
 * "The modifiers" or "the home row" is a statement about keys, and turning it
 * into pixel indices by hand means reading them off a wiring diagram and
 * redoing it whenever the strip is rerouted. The engine already knows which
 * pixel each key sits nearest, so it can do that itself -- but only at
 * runtime, since it needs the key map and this half's offset into the virtual
 * strip. So the zone is resolved once at startup into a scratch buffer, and
 * from then on it is an ordinary pixel-list zone with no cost in the frame.
 */
struct vfx_frame_ctx;

struct vfx_key_zone {
    const uint8_t *keys; /* key positions, as the keymap numbers them */
    uint16_t num_keys;
    uint8_t *pixels;       /* scratch, num_keys long */
    struct vfx_zone *zone; /* filled in by vfx_key_zone_resolve() */
};

/* Fill in a key zone's pixel list. Keys whose pixel belongs to the other half
 * of a split are dropped, which is what lets both halves share one zone
 * definition and each light only its own share of it.
 */
void vfx_key_zone_resolve(const struct vfx_key_zone *kz, const struct vfx_frame_ctx *ctx);
