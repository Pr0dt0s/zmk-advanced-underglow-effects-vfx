/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <dt-bindings/zmk/vfx.h>

/* All color math here is integer. The engine runs on the low priority work
 * queue of a battery powered MCU; the float HSV conversion ZMK core does per
 * pixel per frame is the one thing we deliberately do not copy.
 */

struct vfx_hsb {
    uint16_t h; /* 0-359 degrees */
    uint8_t s;  /* 0-100 percent */
    uint8_t b;  /* 0-100 percent */
};

struct vfx_rgb {
    uint8_t r, g, b;
};

#define VFX_RGB_BLACK ((struct vfx_rgb){0, 0, 0})

/* Unpack a devicetree VFX_HSB() cell. */
static inline struct vfx_hsb vfx_hsb_unpack(uint32_t packed) {
    return (struct vfx_hsb){
        .h = (uint16_t)VFX_HSB_H(packed),
        .s = (uint8_t)VFX_HSB_S(packed),
        .b = (uint8_t)VFX_HSB_B(packed),
    };
}

struct vfx_rgb vfx_hsb_to_rgb(struct vfx_hsb hsb);

/* Interpolate two colors in HSV, taking the shorter way around the hue circle
 * so a blue->red gradient runs through magenta rather than sweeping the whole
 * spectrum backwards. t is 0-255.
 */
struct vfx_hsb vfx_hsb_lerp(struct vfx_hsb a, struct vfx_hsb b, uint8_t t);

/* Rotate a hue by a signed degree offset, wrapping. */
uint16_t vfx_hue_add(uint16_t hue, int16_t delta);

/* Composite src over dst. opacity is 0-255. */
struct vfx_rgb vfx_blend(struct vfx_rgb dst, struct vfx_rgb src, uint8_t mode, uint8_t opacity);

/* Scale every channel by 0-255. */
struct vfx_rgb vfx_rgb_scale(struct vfx_rgb c, uint8_t scale);

/* Perceptual correction applied once, at the end of the frame. */
uint8_t vfx_gamma(uint8_t linear);
