/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/color.h>

#define HUE_MAX 360
#define PCT_MAX 100

/* Guarded with plain defined() rather than Zephyr's IS_ENABLED(): this file is
 * also compiled for the simulator, where no Zephyr headers are in scope.
 */
#if defined(CONFIG_ZMK_VFX_GAMMA) || defined(VFX_SIM)
/* CIE 1931 lightness, so a linear brightness ramp looks linear to the eye.
 * Generated from the CIE lightness curve: for L in 0-100,
 * Y = (L <= 8) ? L / 903.3 : ((L + 16) / 116) ^ 3, scaled to 0-255.
 */
static const uint8_t gamma_lut[256] = {
    0, 0, 0, 0, 0, 1, 1, 1, 1, 1, 1, 1,
    1, 1, 2, 2, 2, 2, 2, 2, 2, 2, 2, 3,
    3, 3, 3, 3, 3, 3, 3, 4, 4, 4, 4, 4,
    4, 5, 5, 5, 5, 5, 6, 6, 6, 6, 6, 7,
    7, 7, 7, 8, 8, 8, 8, 9, 9, 9, 10, 10,
    10, 10, 11, 11, 11, 12, 12, 12, 13, 13, 13, 14,
    14, 15, 15, 15, 16, 16, 17, 17, 17, 18, 18, 19,
    19, 20, 20, 21, 21, 22, 22, 23, 23, 24, 24, 25,
    25, 26, 26, 27, 28, 28, 29, 29, 30, 31, 31, 32,
    32, 33, 34, 34, 35, 36, 37, 37, 38, 39, 39, 40,
    41, 42, 43, 43, 44, 45, 46, 47, 47, 48, 49, 50,
    51, 52, 53, 54, 54, 55, 56, 57, 58, 59, 60, 61,
    62, 63, 64, 65, 66, 67, 68, 70, 71, 72, 73, 74,
    75, 76, 77, 79, 80, 81, 82, 83, 85, 86, 87, 88,
    90, 91, 92, 94, 95, 96, 98, 99, 100, 102, 103, 105,
    106, 108, 109, 110, 112, 113, 115, 116, 118, 120, 121, 123,
    124, 126, 128, 129, 131, 132, 134, 136, 138, 139, 141, 143,
    145, 146, 148, 150, 152, 154, 155, 157, 159, 161, 163, 165,
    167, 169, 171, 173, 175, 177, 179, 181, 183, 185, 187, 189,
    191, 193, 196, 198, 200, 202, 204, 207, 209, 211, 214, 216,
    218, 220, 223, 225, 228, 230, 232, 235, 237, 240, 242, 245,
    247, 250, 252, 255,
};

uint8_t vfx_gamma(uint8_t linear) { return gamma_lut[linear]; }
#else
uint8_t vfx_gamma(uint8_t linear) { return linear; }
#endif

struct vfx_rgb vfx_hsb_to_rgb(struct vfx_hsb hsb) {
    if (hsb.b == 0) {
        return VFX_RGB_BLACK;
    }

    const uint32_t v = (uint32_t)hsb.b * 255U / PCT_MAX;
    const uint32_t s = (uint32_t)hsb.s * 255U / PCT_MAX;

    if (s == 0) {
        return (struct vfx_rgb){(uint8_t)v, (uint8_t)v, (uint8_t)v};
    }

    const uint16_t h = hsb.h % HUE_MAX;
    const uint32_t sector = h / 60U;
    /* Position within the sector, 0-255. */
    const uint32_t frac = ((uint32_t)(h % 60U) * 255U) / 60U;

    const uint32_t p = v * (255U - s) / 255U;
    const uint32_t q = v * (255U - (s * frac) / 255U) / 255U;
    const uint32_t t = v * (255U - (s * (255U - frac)) / 255U) / 255U;

    switch (sector) {
    case 0:
        return (struct vfx_rgb){(uint8_t)v, (uint8_t)t, (uint8_t)p};
    case 1:
        return (struct vfx_rgb){(uint8_t)q, (uint8_t)v, (uint8_t)p};
    case 2:
        return (struct vfx_rgb){(uint8_t)p, (uint8_t)v, (uint8_t)t};
    case 3:
        return (struct vfx_rgb){(uint8_t)p, (uint8_t)q, (uint8_t)v};
    case 4:
        return (struct vfx_rgb){(uint8_t)t, (uint8_t)p, (uint8_t)v};
    default:
        return (struct vfx_rgb){(uint8_t)v, (uint8_t)p, (uint8_t)q};
    }
}

uint16_t vfx_hue_add(uint16_t hue, int16_t delta) {
    int32_t h = (int32_t)hue + delta;

    h %= HUE_MAX;
    if (h < 0) {
        h += HUE_MAX;
    }

    return (uint16_t)h;
}

struct vfx_hsb vfx_hsb_lerp(struct vfx_hsb a, struct vfx_hsb b, uint8_t t) {
    /* Walk the short way around the circle: a 350->10 gradient should cross
     * zero, not run the long way back through green.
     */
    int32_t dh = (int32_t)b.h - (int32_t)a.h;
    if (dh > HUE_MAX / 2) {
        dh -= HUE_MAX;
    } else if (dh < -HUE_MAX / 2) {
        dh += HUE_MAX;
    }

    struct vfx_hsb out;
    out.h = vfx_hue_add(a.h, (int16_t)((dh * t) / 255));
    out.s = (uint8_t)((int32_t)a.s + (((int32_t)b.s - (int32_t)a.s) * t) / 255);
    out.b = (uint8_t)((int32_t)a.b + (((int32_t)b.b - (int32_t)a.b) * t) / 255);

    return out;
}

struct vfx_rgb vfx_rgb_scale(struct vfx_rgb c, uint8_t scale) {
    return (struct vfx_rgb){
        .r = (uint8_t)((uint16_t)c.r * scale / 255U),
        .g = (uint8_t)((uint16_t)c.g * scale / 255U),
        .b = (uint8_t)((uint16_t)c.b * scale / 255U),
    };
}

static uint8_t blend_channel(uint8_t d, uint8_t s, uint8_t mode) {
    uint16_t v;

    switch (mode) {
    case VFX_BLEND_ADD:
        v = (uint16_t)d + s;
        return v > 255U ? 255U : (uint8_t)v;
    case VFX_BLEND_MULTIPLY:
        return (uint8_t)(((uint16_t)d * s) / 255U);
    case VFX_BLEND_SCREEN:
        return (uint8_t)(255U - ((uint16_t)(255U - d) * (255U - s)) / 255U);
    case VFX_BLEND_MAX:
        return d > s ? d : s;
    case VFX_BLEND_NORMAL:
    default:
        return s;
    }
}

struct vfx_rgb vfx_blend(struct vfx_rgb dst, struct vfx_rgb src, uint8_t mode, uint8_t opacity) {
    struct vfx_rgb mixed = {
        .r = blend_channel(dst.r, src.r, mode),
        .g = blend_channel(dst.g, src.g, mode),
        .b = blend_channel(dst.b, src.b, mode),
    };

    if (opacity == 255) {
        return mixed;
    }

    /* Fade from what was already there toward the blended result. */
    return (struct vfx_rgb){
        .r = (uint8_t)((int16_t)dst.r + (((int16_t)mixed.r - dst.r) * opacity) / 255),
        .g = (uint8_t)((int16_t)dst.g + (((int16_t)mixed.g - dst.g) * opacity) / 255),
        .b = (uint8_t)((int16_t)dst.b + (((int16_t)mixed.b - dst.b) * opacity) / 255),
    };
}
