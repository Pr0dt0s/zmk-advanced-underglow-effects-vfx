/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/zone.h>

/* vfx_zone_pixel() is a static inline in the header; this translation unit
 * exists so the zone type has a home and so future zone kinds (mirrored,
 * strided, per-key derived) have somewhere to land without touching engine.c.
 */

/* Clamp a zone to a strip length. Devicetree cannot know the chain-length of
 * the board it will be included from, so an over-long zone is corrected here
 * rather than rejected at build time.
 */
uint16_t vfx_zone_clamped_len(const struct vfx_zone *zone, uint16_t num_pixels) {
    if (zone->pixels) {
        return zone->len;
    }

    if (zone->start >= num_pixels) {
        return 0;
    }

    uint16_t avail = (uint16_t)(num_pixels - zone->start);

    return zone->len < avail ? zone->len : avail;
}
