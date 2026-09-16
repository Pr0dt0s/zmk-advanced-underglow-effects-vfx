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
