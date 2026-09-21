/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
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
    int16_t scroll_speed; /* axis units per second; negative reverses */
    uint16_t span;        /* axis units per full cycle; 0 = one cycle across */
    uint8_t axis;         /* VFX_AXIS_*: which way the gradient runs */
};

struct vfx_gradient_state {
    int32_t scroll_px; /* recomputed each frame, wrapped into [0, span) */
    uint16_t span;
};

struct vfx_breathe_cfg {
    uint32_t color;
    uint16_t period_ms; /* one full in-and-out cycle at speed 3 */
    uint8_t min_level;  /* 0-255, floor so a breathe need not reach black */
    uint8_t hue_swing;  /* degrees the hue moves over the cycle; 0 = fixed */
};

struct vfx_breathe_state {
    struct vfx_rgb rgb;
};

struct vfx_wave_cfg {
    uint32_t color;
    uint16_t wavelength; /* axis units per cycle; 0 = one cycle across */
    uint16_t period_ms;  /* time for the wave to travel one wavelength */
    uint8_t depth;       /* 0-255, how far the trough dips */
    uint8_t axis;        /* VFX_AXIS_*: which way the wave travels */
};

struct vfx_twinkle_cfg {
    uint32_t color;
    uint16_t period_ms; /* lifetime of one twinkle */
    uint8_t density;    /* 0-255, roughly the fraction of pixels lit at once */
    uint8_t hue_spread; /* degrees each twinkle may stray from the colour */
};

struct vfx_plasma_cfg {
    uint32_t color;
    uint16_t scale;     /* units per cycle of the first wave; 0 = across */
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

/* Fire: a bed of embers with flames licking up it. Stateless, like the rest
 * of the ambient family, so both halves of a split agree on the flicker.
 */
struct vfx_fire_cfg {
    uint32_t base_color; /* the coolest part, at the bottom */
    uint32_t tip_color;  /* the hottest, at the top of a flame */
    uint16_t period_ms;  /* how quickly the flicker evolves */
    uint16_t cell;       /* units per flame; smaller is a busier fire */
    uint8_t height;      /* 0-255, how far up the board the flames reach */
    uint8_t flicker;     /* 0-255, how much a flame varies over time */
    uint8_t axis;        /* which way is up; usually VFX_AXIS_Y */
};

/* Comet: a bright head running around the board with a fading tail. */
struct vfx_comet_cfg {
    uint32_t color;
    uint32_t head_color; /* 0 just uses `color` at full brightness */
    uint16_t period_ms;  /* time for one lap of the axis */
    uint16_t tail;       /* axis units of trail behind the head */
    uint8_t count;       /* how many comets, spread evenly round the lap */
    uint8_t axis;        /* which way it runs */
};

/* Cross: the pressed key's row and column light up, rather than a ring
 * expanding from it. Reusing the ripple's slots, since a press is a press.
 */
struct vfx_cross_cfg {
    uint32_t color;
    uint32_t centre_color; /* the key itself; 0 uses `color` */
    uint16_t decay_ms;     /* time for one cross to fade out */
    uint16_t radius;       /* units the arms reach; 0 = the whole board */
    uint8_t thickness;     /* units either side of the row or column line */
    uint8_t axes;          /* VFX_CROSS_*: which arms are drawn */
};

struct vfx_cross_state {
    struct vfx_ripple_slot slots[VFX_MAX_RIPPLES];
    uint8_t next;
};

/* Overlapping ambient drops. Four is enough that rain reads as continuous
 * without the per-pixel loop growing expensive.
 */
#define VFX_WATER_MAX_AMBIENT 4

struct vfx_water_cfg {
    uint32_t color;        /* still water */
    uint32_t crest_color;  /* wave peaks; 0 just brightens `color` */
    uint16_t wavelength;   /* virtual pixels per wave cycle */
    uint16_t speed;        /* virtual pixels per second the wavefront travels */
    uint16_t lifetime_ms;  /* how long one drop keeps rippling */
    uint16_t drop_rate_ms; /* gap between ambient drops; 0 = keypresses only */
    uint8_t amplitude;     /* 0-255, how hard the waves move the surface */
    uint8_t damping;       /* how quickly waves lose height with distance */
};

struct vfx_water_state {
    /* Keypress drops only. Ambient ones are a function of time, so they need
     * no storage and both halves of a split agree on them for free.
     */
    struct vfx_ripple_slot drops[VFX_MAX_RIPPLES];
    uint8_t next;
};

/* Overlapping ambient columns. Eight keeps rain reading as continuous
 * without the per-pixel loop growing expensive.
 */
#define VFX_MATRIX_MAX_AMBIENT 8

struct vfx_matrix_cfg {
    uint32_t color;      /* the trail */
    uint32_t head_color; /* leading pixel; 0 just brightens `color` */
    uint16_t speed;      /* units per second the head falls */
    uint16_t tail;       /* units of trail behind the head */
    uint16_t drop_rate_ms; /* gap between ambient drops; 0 = keypresses only */
    uint8_t columns;     /* how many columns the board is divided into */
    uint8_t jitter;      /* 0-255, spread of per-drop speeds */
    uint8_t head_size;   /* units at the front drawn as the head */
};

struct vfx_matrix_drop {
    uint32_t start_ms;
    int32_t y0;       /* where the head began, above the board */
    int32_t y_end;    /* where it stops: the pressed key, or past the bottom */
    uint8_t column;
    uint8_t speed_pct; /* 100 is the configured speed */
    bool active;
};

struct vfx_matrix_state {
    /* Keypress drops only; ambient ones are a function of time. */
    struct vfx_matrix_drop drops[VFX_MAX_RIPPLES];
    uint8_t next;

    /* Board geometry, measured once per frame. */
    int16_t min_x, min_y;
    uint16_t col_w;
    uint16_t fall_len;
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

/* Reacts to a press without caring which key it was, so it works on pixels
 * that sit nowhere near the keys: the whole zone lifts and decays together.
 */
struct vfx_pulse_cfg {
    uint32_t color;
    uint16_t decay_ms;  /* time from a press back down to min_level */
    uint8_t min_level;  /* 0-255 floor; 0 lets the power gate cut between presses */
    uint8_t hue_step;   /* degrees the hue advances per press; 0 = fixed */
    bool stack;         /* presses add rather than restarting from full */
};

struct vfx_pulse_state {
    uint8_t level;
    uint16_t hue_offset;
    uint32_t last_ms;
};

#define VFX_HOLD_MAX_PIXELS 128

/* Lit for exactly as long as the key is down.
 *
 * Every other reactive generator is struck and then decays, which cannot
 * express a duration that is not known when the key goes down. This one is
 * the only thing here that reads the release.
 */
struct vfx_hold_cfg {
    uint32_t color;
    uint16_t release_ms; /* fade once let go; 0 cuts straight out */
};

struct vfx_hold_state {
    uint8_t level[VFX_HOLD_MAX_PIXELS];
    uint8_t held[(VFX_HOLD_MAX_PIXELS + 7) / 8];
    uint32_t last_ms;
};

/* A packet launched by a keypress that then travels on its own.
 *
 * Ripple expands as a ring from where it started and comet runs on a timer
 * without being triggered at all, so a struck thing that then moves along an
 * axis is not reachable by combining them.
 */
struct vfx_dart_slot {
    uint32_t start_ms;
    uint16_t origin; /* axis position of the key that launched it */
    bool active;
};

struct vfx_dart_cfg {
    uint32_t color;
    uint32_t head_color;
    uint16_t speed;       /* axis units per second */
    uint16_t lifetime_ms; /* how long one lives */
    uint8_t tail;         /* axis units trailing the head */
    uint8_t axis;
    bool reverse; /* travel toward decreasing axis instead */
};

struct vfx_dart_state {
    struct vfx_dart_slot slots[VFX_MAX_RIPPLES];
    uint8_t next;
};

/* Independent per-pixel randomness, re-rolled on a clock. Twinkle has a fade
 * envelope and plasma is smooth, so neither can be made to look like this.
 */
struct vfx_static_cfg {
    uint32_t color;
    uint16_t period_ms; /* how often the field is re-rolled */
    uint8_t density;    /* 0-255 share of pixels lit in a roll */
    uint8_t hue_spread; /* degrees of hue jitter between pixels */
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

/* Lights up while the host has a lock LED on, or while a modifier is held.
 * One generator for both: they are the same shape, a bit set somewhere and a
 * colour for it, and which one is read is a property.
 */
struct vfx_flag_cfg {
    uint32_t color;
    uint8_t source; /* VFX_FLAG_LOCKS or VFX_FLAG_MODIFIERS */
    uint8_t mask;   /* VFX_LOCK_* / VFX_MOD_*; any bit set lights the zone */
};

/* Typing speed, as a bar or as a colour. */
struct vfx_wpm_cfg {
    uint32_t idle_color; /* at rest */
    uint32_t fast_color; /* at `full` words per minute and above */
    uint16_t full;       /* wpm that counts as flat out */
    uint8_t bar;         /* non-zero fills the zone in proportion instead */
};

/* The other half's battery, drawn the same way as your own. */
struct vfx_peripheral_battery_cfg {
    uint32_t low_color;
    uint32_t high_color;
    uint32_t empty_color;   /* the unfilled part of the bar */
    uint32_t unknown_color; /* before the peripheral has ever reported */
    uint8_t source;         /* which peripheral, 0 based */
    uint8_t warn_below;     /* percent under which the whole bar uses low */
};

