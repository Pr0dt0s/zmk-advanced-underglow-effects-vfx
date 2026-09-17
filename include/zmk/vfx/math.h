/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Integer helpers shared by the generators. Everything here is deterministic
 * and depends only on its arguments: two halves of a split that agree on the
 * timebase then produce identical frames with nothing else exchanged.
 */

/* Sine over a 0-255 turn, returned as 0-255 centred on 128. */
uint8_t vfx_sin8(uint8_t turn);

/* Angle of (x, y) as a 0-255 turn, clockwise from the positive x axis in
 * screen coordinates (y growing downward). This is what lets an effect run
 * around the board rather than across it.
 */
uint8_t vfx_atan2_8(int32_t y, int32_t x);

/* Triangle wave over a 0-255 turn: 0 at the ends, 255 in the middle. */
static inline uint8_t vfx_tri8(uint8_t turn) {
    return turn < 128 ? (uint8_t)(turn * 2) : (uint8_t)((255 - turn) * 2);
}

/* Cheap integer hash. Used to scatter per-pixel randomness without carrying
 * any state, so a twinkle looks the same on both halves.
 */
static inline uint32_t vfx_hash32(uint32_t x) {
    x ^= x >> 16;
    x *= 0x7feb352dU;
    x ^= x >> 15;
    x *= 0x846ca68bU;
    x ^= x >> 16;

    return x;
}

/* Integer square root by Newton's method. Used for distances between pixel
 * positions; the engine has no floating point and should not grow one.
 */
static inline uint32_t vfx_isqrt(uint32_t n) {
    if (n == 0) {
        return 0;
    }

    uint32_t x = n;
    uint32_t y = (x + 1) / 2;

    while (y < x) {
        x = y;
        y = (x + n / x) / 2;
    }

    return x;
}

static inline uint8_t vfx_scale8(uint8_t value, uint8_t scale) {
    return (uint8_t)(((uint16_t)value * scale) / 255U);
}
