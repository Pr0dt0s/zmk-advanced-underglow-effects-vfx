/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * WebAssembly wrapper around the real compositor.
 *
 * This links the same color.c, zone.c, render.c and generator sources the
 * firmware uses. No effect is reimplemented here or in JavaScript, so a
 * frame in the browser is the frame the keyboard renders.
 *
 * Freestanding: no libc, no allocator. Scenes are assembled into fixed arenas
 * through the builder calls below, which is also how they work on the MCU.
 */

#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/power.h>

#define EXPORT __attribute__((visibility("default")))

#define MAX_PIXELS 128
#define MAX_ZONES 16
#define MAX_LAYERS 16
#define MAX_STOPS 16
#define SCRATCH_BYTES 512

/* struct vfx_rgb is three uint8_t, so a frame can be handed to JavaScript as
 * a flat RGB byte array with no copy. Assert it rather than assume it.
 */
_Static_assert(sizeof(struct vfx_rgb) == 3, "vfx_rgb must be tightly packed for the simulator");

static struct vfx_rgb frame[MAX_PIXELS];

static struct vfx_zone zones[MAX_ZONES];
static uint8_t zone_pixels[MAX_ZONES][MAX_PIXELS];
static int num_zones;

static struct vfx_layer layers[MAX_LAYERS];
static int num_layers;

static struct vfx_solid_cfg solid_cfg[MAX_LAYERS];
static struct vfx_solid_state solid_state[MAX_LAYERS];
static struct vfx_gradient_cfg grad_cfg[MAX_LAYERS];
static struct vfx_gradient_state grad_state[MAX_LAYERS];
static uint32_t grad_stops[MAX_LAYERS][MAX_STOPS];

static uint8_t scratch[SCRATCH_BYTES];

static struct vfx_scene scene = {.name = "sim", .layers = layers, .num_layers = 0};

static struct vfx_frame_ctx ctx = {
    .time_ms = 0,
    .virtual_length = 72,
    .strip_offset = 0,
    .num_pixels = 36,
    .speed = 3,
    .brightness = 255,
    .hue_shift = 0,
};

static bool last_lit;

/* The power gate runs here too, so the rail indicator and the current readout
 * on the page come from the same state machine the firmware runs, not from a
 * JavaScript approximation of it.
 */
static struct vfx_power_ctl power_ctl;
static struct vfx_power_policy power_policy = {.blackout_delay_ms = 500, .settle_ms = 50};
static int last_action = VFX_POWER_TRANSMIT;
static uint32_t last_frame_ms = 0;

/* --------------------------------------------------------------- freestanding */

void *memcpy(void *dst, const void *src, unsigned long n) {
    uint8_t *d = dst;
    const uint8_t *s = src;

    while (n--) {
        *d++ = *s++;
    }

    return dst;
}

void *memset(void *dst, int c, unsigned long n) {
    uint8_t *d = dst;

    while (n--) {
        *d++ = (uint8_t)c;
    }

    return dst;
}

/* -------------------------------------------------------------------- setup */

EXPORT void vfx_sim_init(int num_pixels, int virtual_length, int strip_offset) {
    if (num_pixels < 1) {
        num_pixels = 1;
    }
    if (num_pixels > MAX_PIXELS) {
        num_pixels = MAX_PIXELS;
    }

    ctx.num_pixels = (uint16_t)num_pixels;
    ctx.virtual_length = (uint16_t)(virtual_length > 0 ? virtual_length : num_pixels);
    ctx.strip_offset = (uint16_t)(strip_offset > 0 ? strip_offset : 0);
}

EXPORT void vfx_sim_set_state(int brightness, int speed, int hue_shift) {
    ctx.brightness = (uint8_t)(brightness < 0 ? 0 : (brightness > 255 ? 255 : brightness));
    ctx.speed = (uint8_t)(speed < 1 ? 1 : (speed > 5 ? 5 : speed));
    ctx.hue_shift = (int16_t)hue_shift;
}

/* JavaScript stages variable length data (pixel lists, gradient stops) here
 * before the matching add_* call reads it. Avoids needing an allocator.
 */
EXPORT uint8_t *vfx_sim_scratch(void) { return scratch; }

EXPORT int vfx_sim_scratch_size(void) { return SCRATCH_BYTES; }

EXPORT void vfx_sim_reset_scene(void) {
    num_zones = 0;
    num_layers = 0;
    scene.num_layers = 0;
}

EXPORT int vfx_sim_add_zone_range(int start, int len) {
    if (num_zones >= MAX_ZONES) {
        return -1;
    }

    zones[num_zones].pixels = NULL;
    zones[num_zones].start = (uint16_t)start;
    zones[num_zones].len = (uint16_t)len;

    return num_zones++;
}

EXPORT int vfx_sim_add_zone_pixels(int len) {
    if (num_zones >= MAX_ZONES || len < 0 || len > MAX_PIXELS) {
        return -1;
    }

    for (int i = 0; i < len; i++) {
        zone_pixels[num_zones][i] = scratch[i];
    }

    zones[num_zones].pixels = zone_pixels[num_zones];
    zones[num_zones].start = 0;
    zones[num_zones].len = (uint16_t)len;

    return num_zones++;
}

static struct vfx_layer *next_layer(int zone, int blend, int opacity) {
    if (num_layers >= MAX_LAYERS || zone < 0 || zone >= num_zones) {
        return NULL;
    }

    struct vfx_layer *l = &layers[num_layers];

    l->zone = &zones[zone];
    l->blend = (uint8_t)blend;
    l->opacity = (uint8_t)(opacity < 0 ? 0 : (opacity > 255 ? 255 : opacity));

    return l;
}

EXPORT int vfx_sim_add_solid(int zone, int blend, int opacity, uint32_t color) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    solid_cfg[num_layers].color = color;

    l->api = &vfx_layer_solid_api;
    l->config = &solid_cfg[num_layers];
    l->state = &solid_state[num_layers];

    scene.num_layers = (uint8_t)(++num_layers);

    return num_layers - 1;
}

EXPORT int vfx_sim_add_gradient(int zone, int blend, int opacity, int scroll_speed, int span,
                                int num_stops) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l || num_stops < 1 || num_stops > MAX_STOPS) {
        return -1;
    }

    /* Stops arrive in the scratch buffer as little endian uint32. */
    const uint32_t *staged = (const uint32_t *)(void *)scratch;

    for (int i = 0; i < num_stops; i++) {
        grad_stops[num_layers][i] = staged[i];
    }

    grad_cfg[num_layers].stops = grad_stops[num_layers];
    grad_cfg[num_layers].num_stops = (uint8_t)num_stops;
    grad_cfg[num_layers].scroll_speed = (int16_t)scroll_speed;
    grad_cfg[num_layers].span = (uint16_t)span;

    l->api = &vfx_layer_gradient_api;
    l->config = &grad_cfg[num_layers];
    l->state = &grad_state[num_layers];

    scene.num_layers = (uint8_t)(++num_layers);

    return num_layers - 1;
}

/* ------------------------------------------------------------------- render */

EXPORT void vfx_sim_set_power_policy(int blackout_delay_ms, int settle_ms) {
    power_policy.blackout_delay_ms = (uint16_t)blackout_delay_ms;
    power_policy.settle_ms = (uint16_t)settle_ms;
    vfx_power_reset(&power_ctl);
}

EXPORT const uint8_t *vfx_sim_render(uint32_t time_ms) {
    /* Frames arrive at whatever rate the browser paints, so feed the gate the
     * real elapsed time rather than a nominal frame interval.
     */
    uint32_t elapsed = time_ms > last_frame_ms ? time_ms - last_frame_ms : 0;
    if (elapsed > 1000) {
        elapsed = 1000; /* a backgrounded tab should not gate instantly on return */
    }
    last_frame_ms = time_ms;

    ctx.time_ms = time_ms;

    vfx_render_frame(&scene, &ctx, frame, &last_lit);

    last_action = (int)vfx_power_step(&power_ctl, &power_policy, last_lit, (uint16_t)elapsed);

    return (const uint8_t *)frame;
}

/* 0 = lit, 1 = gated, 2 = settling. */
EXPORT int vfx_sim_power_state(void) { return (int)power_ctl.state; }

EXPORT int vfx_sim_power_action(void) { return last_action; }

EXPORT int vfx_sim_estimated_ua(void) {
    const bool powered = power_ctl.state != VFX_POWER_GATED;

    return (int)vfx_estimate_ua(frame, ctx.num_pixels, powered);
}

EXPORT void vfx_sim_power_reset(void) { vfx_power_reset(&power_ctl); }

EXPORT int vfx_sim_num_pixels(void) { return ctx.num_pixels; }

EXPORT int vfx_sim_any_lit(void) { return last_lit ? 1 : 0; }

EXPORT int vfx_sim_is_animating(void) { return vfx_scene_is_animating(&scene, &ctx) ? 1 : 0; }

EXPORT void vfx_sim_key_event(int position, int pressed) {
    vfx_scene_key_event(&scene, (uint32_t)position, pressed != 0);
}
