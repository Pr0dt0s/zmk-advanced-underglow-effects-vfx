/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zmk/vfx/color.h>

/* Per-generator configuration. These are what the devicetree macros emit into
 * flash and what the simulator's JSON loader builds at runtime, so keep them
 * plain data: no pointers back into Zephyr, no function pointers.
 */

struct vfx_solid_cfg {
    uint32_t color; /* packed VFX_HSB */
};

struct vfx_solid_state {
    struct vfx_rgb rgb;
};

struct vfx_gradient_cfg {
    const uint32_t *stops; /* packed VFX_HSB, cyclic: last wraps to first */
    uint8_t num_stops;
    int16_t scroll_speed; /* virtual pixels per second; negative reverses */
    uint16_t span;        /* virtual pixels per full cycle; 0 = whole board */
};

struct vfx_gradient_state {
    int32_t scroll_px; /* recomputed each frame, wrapped into [0, span) */
    uint16_t span;
};
