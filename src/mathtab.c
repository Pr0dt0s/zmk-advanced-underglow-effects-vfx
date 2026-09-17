/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/math.h>

/* 256 entries of 127.5 + 127.5 * sin(2*pi*i/256), rounded. A table rather
 * than a polynomial: it is one flash read against several multiplies, and
 * this runs per pixel per frame.
 */
static const uint8_t sin_lut[256] = {
    128, 131, 134, 137, 140, 143, 146, 149, 152, 155, 158, 162,
    165, 167, 170, 173, 176, 179, 182, 185, 188, 190, 193, 196,
    198, 201, 203, 206, 208, 211, 213, 215, 218, 220, 222, 224,
    226, 228, 230, 232, 234, 235, 237, 238, 240, 241, 243, 244,
    245, 246, 248, 249, 250, 250, 251, 252, 253, 253, 254, 254,
    254, 255, 255, 255, 255, 255, 255, 255, 254, 254, 254, 253,
    253, 252, 251, 250, 250, 249, 248, 246, 245, 244, 243, 241,
    240, 238, 237, 235, 234, 232, 230, 228, 226, 224, 222, 220,
    218, 215, 213, 211, 208, 206, 203, 201, 198, 196, 193, 190,
    188, 185, 182, 179, 176, 173, 170, 167, 165, 162, 158, 155,
    152, 149, 146, 143, 140, 137, 134, 131, 128, 124, 121, 118,
    115, 112, 109, 106, 103, 100, 97, 93, 90, 88, 85, 82,
    79, 76, 73, 70, 67, 65, 62, 59, 57, 54, 52, 49,
    47, 44, 42, 40, 37, 35, 33, 31, 29, 27, 25, 23,
    21, 20, 18, 17, 15, 14, 12, 11, 10, 9, 7, 6,
    5, 5, 4, 3, 2, 2, 1, 1, 1, 0, 0, 0,
    0, 0, 0, 0, 1, 1, 1, 2, 2, 3, 4, 5,
    5, 6, 7, 9, 10, 11, 12, 14, 15, 17, 18, 20,
    21, 23, 25, 27, 29, 31, 33, 35, 37, 40, 42, 44,
    47, 49, 52, 54, 57, 59, 62, 65, 67, 70, 73, 76,
    79, 82, 85, 88, 90, 93, 97, 100, 103, 106, 109, 112,
    115, 118, 121, 124,
};

uint8_t vfx_sin8(uint8_t turn) { return sin_lut[turn]; }

/* atan(r/256) as a 0-255 turn, for r in 0..256, so the whole table covers the
 * first eighth of a turn and the octant folding in vfx_atan2_8() covers the
 * rest. A table again rather than a polynomial: the approximations that are
 * cheap enough to run per pixel are the ones that visibly bend a pinwheel.
 */
static const uint8_t atan_lut[257] = {
    0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 2, 2, 2, 2, 2, 2,
    3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4, 4, 5, 5, 5,
    5, 5, 5, 6, 6, 6, 6, 6, 6, 6, 7, 7, 7, 7, 7, 7,
    8, 8, 8, 8, 8, 8, 8, 9, 9, 9, 9, 9, 9, 10, 10, 10,
    10, 10, 10, 10, 11, 11, 11, 11, 11, 11, 11, 12, 12, 12, 12, 12,
    12, 12, 13, 13, 13, 13, 13, 13, 13, 14, 14, 14, 14, 14, 14, 14,
    15, 15, 15, 15, 15, 15, 15, 16, 16, 16, 16, 16, 16, 16, 17, 17,
    17, 17, 17, 17, 17, 17, 18, 18, 18, 18, 18, 18, 18, 19, 19, 19,
    19, 19, 19, 19, 19, 20, 20, 20, 20, 20, 20, 20, 20, 21, 21, 21,
    21, 21, 21, 21, 21, 21, 22, 22, 22, 22, 22, 22, 22, 22, 23, 23,
    23, 23, 23, 23, 23, 23, 23, 24, 24, 24, 24, 24, 24, 24, 24, 24,
    25, 25, 25, 25, 25, 25, 25, 25, 25, 25, 26, 26, 26, 26, 26, 26,
    26, 26, 26, 27, 27, 27, 27, 27, 27, 27, 27, 27, 27, 28, 28, 28,
    28, 28, 28, 28, 28, 28, 28, 28, 29, 29, 29, 29, 29, 29, 29, 29,
    29, 29, 29, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 30, 31, 31,
    31, 31, 31, 31, 31, 31, 31, 31, 31, 31, 32, 32, 32, 32, 32, 32,
    32,
};

uint8_t vfx_atan2_8(int32_t y, int32_t x) {
    if (x == 0 && y == 0) {
        return 0;
    }

    const int32_t ax = x < 0 ? -x : x;
    const int32_t ay = y < 0 ? -y : y;

    /* Fold into the first octant, where the table is defined, by taking the
     * ratio of the shorter side to the longer one.
     */
    int32_t angle;

    if (ax >= ay) {
        angle = atan_lut[(ay * 256) / ax];
    } else {
        angle = 64 - atan_lut[(ax * 256) / ay];
    }

    /* Then back out to the right quadrant. Screen coordinates, so y grows
     * downward and the turn runs clockwise from the positive x axis.
     */
    if (x < 0) {
        angle = 128 - angle;
    }
    if (y < 0) {
        angle = 256 - angle;
    }

    return (uint8_t)(angle & 0xFF);
}
