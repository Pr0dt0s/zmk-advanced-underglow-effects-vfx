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

struct vfx_breathe_cfg {
    uint32_t color;
    uint16_t period_ms; /* one full in-and-out cycle at speed 3 */
    uint8_t min_level;  /* 0-255, floor so a breathe need not reach black */
};

struct vfx_breathe_state {
    struct vfx_rgb rgb;
};

struct vfx_wave_cfg {
    uint32_t color;
    uint16_t wavelength; /* virtual pixels per cycle */
    uint16_t period_ms;  /* time for the wave to travel one wavelength */
    uint8_t depth;       /* 0-255, how far the trough dips */
};

struct vfx_twinkle_cfg {
    uint32_t color;
    uint16_t period_ms; /* lifetime of one twinkle */
    uint8_t density;    /* 0-255, roughly the fraction of pixels lit at once */
};

struct vfx_plasma_cfg {
    uint32_t color;
    uint16_t scale;     /* virtual pixels per cycle of the first wave */
    uint16_t period_ms;
    uint8_t hue_spread; /* degrees of hue swing around the base color */
};

#define VFX_MAX_RIPPLES 6

struct vfx_ripple_cfg {
    uint32_t color;
    uint16_t decay_ms; /* time for one ripple to fade out */
    uint16_t speed;    /* virtual pixels per second the front travels */
    uint8_t width;     /* virtual pixels of the bright band */
};

struct vfx_ripple_slot {
    uint32_t start_ms;
    uint16_t origin;
    bool active;
};

struct vfx_ripple_state {
    struct vfx_ripple_slot slots[VFX_MAX_RIPPLES];
    uint8_t next;
};

struct vfx_keyflash_cfg {
    uint32_t color;
    uint16_t decay_ms;
    uint8_t spread; /* virtual pixels either side of the key that light up */
};

struct vfx_keyflash_state {
    struct vfx_ripple_slot slots[VFX_MAX_RIPPLES];
    uint8_t next;
};

#define VFX_TRAIL_MAX_PIXELS 128

struct vfx_trail_cfg {
    uint32_t color;
    uint16_t decay_ms;
    uint8_t spread;
};

struct vfx_trail_state {
    uint8_t heat[VFX_TRAIL_MAX_PIXELS];
    uint32_t last_ms;
};

struct vfx_layer_state_cfg {
    const uint32_t *colors; /* one per keymap layer */
    uint8_t num_colors;
};

struct vfx_battery_cfg {
    uint32_t low_color;
    uint32_t high_color;
    uint32_t empty_color; /* drawn on the unfilled part of the zone */
    uint8_t warn_below;   /* percent under which the whole bar uses low_color */
};

struct vfx_ble_profile_cfg {
    uint32_t connected_color;
    uint32_t disconnected_color;
    uint32_t usb_color;
};

