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
#include <zmk/vfx/status.h>
#include <zmk/vfx/tuning.h>
#include <zmk/vfx/sync.h>

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

/* One arena per config type, indexed by layer slot. Wasteful in RAM and
 * entirely fine here: the simulator is not the thing that has to fit on an
 * nRF52840, and keeping the layouts identical to the firmware's matters more.
 */
static struct vfx_solid_cfg solid_cfg[MAX_LAYERS];
static struct vfx_solid_state solid_state[MAX_LAYERS];
static struct vfx_gradient_cfg grad_cfg[MAX_LAYERS];
static struct vfx_gradient_state grad_state[MAX_LAYERS];
static uint32_t grad_stops[MAX_LAYERS][MAX_STOPS];

static struct vfx_breathe_cfg breathe_cfg[MAX_LAYERS];
static struct vfx_breathe_state breathe_state[MAX_LAYERS];
static struct vfx_wave_cfg wave_cfg[MAX_LAYERS];
static struct vfx_twinkle_cfg twinkle_cfg[MAX_LAYERS];
static struct vfx_plasma_cfg plasma_cfg[MAX_LAYERS];
static struct vfx_ripple_cfg ripple_cfg[MAX_LAYERS];
static struct vfx_ripple_state ripple_state[MAX_LAYERS];
static struct vfx_keyflash_cfg keyflash_cfg[MAX_LAYERS];
static struct vfx_keyflash_state keyflash_state[MAX_LAYERS];
static struct vfx_water_cfg water_cfg[MAX_LAYERS];
static struct vfx_water_state water_state[MAX_LAYERS];
static struct vfx_matrix_cfg matrix_cfg[MAX_LAYERS];
static struct vfx_matrix_state matrix_state[MAX_LAYERS];
static struct vfx_cross_cfg cross_cfg[MAX_LAYERS];
static struct vfx_cross_state cross_state[MAX_LAYERS];
static struct vfx_fire_cfg fire_cfg[MAX_LAYERS];
static struct vfx_comet_cfg comet_cfg[MAX_LAYERS];
static struct vfx_flag_cfg flag_cfg[MAX_LAYERS];
static struct vfx_wpm_cfg wpm_cfg[MAX_LAYERS];
static struct vfx_peripheral_battery_cfg pbat_cfg[MAX_LAYERS];
static struct vfx_trail_cfg trail_cfg[MAX_LAYERS];
static struct vfx_trail_state trail_state[MAX_LAYERS];
static struct vfx_layer_state_cfg layer_state_cfg[MAX_LAYERS];
static uint32_t layer_state_colors[MAX_LAYERS][MAX_STOPS];
static struct vfx_battery_cfg battery_cfg[MAX_LAYERS];
static struct vfx_ble_profile_cfg ble_cfg[MAX_LAYERS];
static struct vfx_pulse_cfg pulse_cfg[MAX_LAYERS];
static struct vfx_pulse_state pulse_state[MAX_LAYERS];
static struct vfx_hold_cfg hold_cfg[MAX_LAYERS];
static struct vfx_hold_state hold_state[MAX_LAYERS];
static struct vfx_dart_cfg dart_cfg[MAX_LAYERS];
static struct vfx_dart_state dart_state[MAX_LAYERS];
static struct vfx_static_cfg static_cfg[MAX_LAYERS];

/* Stateless generators still need a state pointer. */
static uint8_t stateless[MAX_LAYERS];

static uint8_t scratch[SCRATCH_BYTES];

/* The key map needs storage of its own rather than pointing into the shared
 * staging buffer: loading a scene writes gradient stops through scratch, which
 * would leave ctx.key_pixels aimed at colour data and every reactive effect
 * originating from a nonsense pixel.
 */
static uint8_t key_map[MAX_PIXELS];

/* Whole-board pixel positions, x,y pairs. Copied out of the staging buffer
 * for the same reason the key map is: loading a scene writes through scratch.
 */
static int16_t pixel_xy[MAX_PIXELS * 2];

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

    vfx_tuning_reset_all();

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

/* Keys staged in the scratch buffer, resolved through the key map exactly as
 * the firmware resolves a zone written with the `keys` property.
 */
EXPORT int vfx_sim_add_zone_keys(int len) {
    if (num_zones >= MAX_ZONES || len < 0 || len > MAX_PIXELS) {
        return -1;
    }

    struct vfx_zone *zone = &zones[num_zones];
    const struct vfx_key_zone kz = {
        .keys = scratch,
        .num_keys = (uint16_t)len,
        .pixels = zone_pixels[num_zones],
        .zone = zone,
    };

    vfx_key_zone_resolve(&kz, &ctx);

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

    /* Cleared rather than left alone: the layer array outlives a scene, so a
     * slot that carried a driven opacity once would keep driving the next
     * scene's layer that happened to land in it.
     */
    l->opacity_src = VFX_SRC_NONE;
    l->opacity_min = 0;
    l->opacity_full = 0;
    l->tune_id = 0;

    return l;
}

/* Called after an add_*, since the source is the same question for every
 * generator and threading it through fifteen signatures would say otherwise.
 */
/* Which tuning slot a layer answers to, set after the add for the same reason
 * the opacity source is: it is the same question for every generator.
 */
EXPORT void vfx_sim_set_layer_tune(int layer, int slot) {
    if (layer < 0 || layer >= num_layers) {
        return;
    }

    layers[layer].tune_id = (uint8_t)slot;
}

EXPORT void vfx_sim_tune(int slot, int hue, int level, int speed) {
    vfx_tuning_set_hue((uint8_t)slot, (int16_t)hue);
    vfx_tuning_set_level((uint8_t)slot, (uint8_t)level);
    vfx_tuning_set_speed((uint8_t)slot, (uint8_t)speed);
}

EXPORT void vfx_sim_tune_reset_all(void) { vfx_tuning_reset_all(); }

EXPORT void vfx_sim_set_layer_source(int layer, int src, int min, int full) {
    if (layer < 0 || layer >= num_layers) {
        return;
    }

    layers[layer].opacity_src = (uint8_t)src;
    layers[layer].opacity_min = (uint8_t)(min < 0 ? 0 : (min > 255 ? 255 : min));
    layers[layer].opacity_full = (uint8_t)(full < 0 ? 0 : (full > 255 ? 255 : full));
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
                                int num_stops, int axis) {
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
    grad_cfg[num_layers].axis = (uint8_t)axis;

    l->api = &vfx_layer_gradient_api;
    l->config = &grad_cfg[num_layers];
    l->state = &grad_state[num_layers];

    scene.num_layers = (uint8_t)(++num_layers);

    return num_layers - 1;
}

/* Generator ids shared with app.js. Adding one means appending here and in
 * the LAYER_TYPES table on the page.
 */
enum sim_layer_type {
    SIM_SOLID = 0,
    SIM_GRADIENT,
    SIM_BREATHE,
    SIM_WAVE,
    SIM_TWINKLE,
    SIM_PLASMA,
    SIM_RIPPLE,
    SIM_KEYFLASH,
    SIM_TRAIL,
    SIM_LAYER_STATE,
    SIM_BATTERY,
    SIM_BLE_PROFILE,
    SIM_WATER,
    SIM_MATRIX,
    SIM_CROSS,
    SIM_FIRE,
    SIM_COMET,
};

/* One entry point for every generator whose config is a colour plus up to
 * three numbers, which is all of them but the three below.
 */
EXPORT int vfx_sim_add_layer(int type, int zone, int blend, int opacity, uint32_t color, int a,
                             int b, int c, int d) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    l->state = &stateless[i];

    switch (type) {
    case SIM_BREATHE:
        breathe_cfg[i] = (struct vfx_breathe_cfg){
            .color = color,
            .period_ms = (uint16_t)a,
            .min_level = (uint8_t)b,
            .hue_swing = (uint8_t)d};
        l->api = &vfx_layer_breathe_api;
        l->config = &breathe_cfg[i];
        l->state = &breathe_state[i];
        break;

    case SIM_WAVE:
        wave_cfg[i] = (struct vfx_wave_cfg){.color = color,
                                            .wavelength = (uint16_t)a,
                                            .period_ms = (uint16_t)b,
                                            .depth = (uint8_t)c};
        l->api = &vfx_layer_wave_api;
        l->config = &wave_cfg[i];
        break;

    case SIM_TWINKLE:
        twinkle_cfg[i] = (struct vfx_twinkle_cfg){
            .color = color,
            .period_ms = (uint16_t)a,
            .density = (uint8_t)b,
            .hue_spread = (uint8_t)d};
        l->api = &vfx_layer_twinkle_api;
        l->config = &twinkle_cfg[i];
        break;

    case SIM_PLASMA:
        plasma_cfg[i] = (struct vfx_plasma_cfg){.color = color,
                                                .scale = (uint16_t)a,
                                                .period_ms = (uint16_t)b,
                                                .hue_spread = (uint8_t)c};
        l->api = &vfx_layer_plasma_api;
        l->config = &plasma_cfg[i];
        break;

    case SIM_RIPPLE:
        ripple_cfg[i] = (struct vfx_ripple_cfg){
            .color = color, .decay_ms = (uint16_t)a, .speed = (uint16_t)b, .width = (uint8_t)c};
        l->api = &vfx_layer_ripple_api;
        l->config = &ripple_cfg[i];
        l->state = &ripple_state[i];
        ripple_state[i] = (struct vfx_ripple_state){0};
        break;

    case SIM_KEYFLASH:
        keyflash_cfg[i] = (struct vfx_keyflash_cfg){
            .color = color, .decay_ms = (uint16_t)a, .spread = (uint8_t)b};
        l->api = &vfx_layer_keyflash_api;
        l->config = &keyflash_cfg[i];
        l->state = &keyflash_state[i];
        keyflash_state[i] = (struct vfx_keyflash_state){0};
        break;

    case SIM_TRAIL:
        trail_cfg[i] = (struct vfx_trail_cfg){
            .color = color, .decay_ms = (uint16_t)a, .spread = (uint8_t)b};
        l->api = &vfx_layer_trail_api;
        l->config = &trail_cfg[i];
        l->state = &trail_state[i];
        trail_state[i] = (struct vfx_trail_state){0};
        break;


    default:
        return -1;
    }

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_water(int zone, int blend, int opacity, uint32_t color, uint32_t crest,
                             int wavelength, int speed, int lifetime_ms, int drop_rate_ms,
                             int amplitude, int damping) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    water_cfg[i] = (struct vfx_water_cfg){
        .color = color,
        .crest_color = crest,
        .wavelength = (uint16_t)wavelength,
        .speed = (uint16_t)speed,
        .lifetime_ms = (uint16_t)lifetime_ms,
        .drop_rate_ms = (uint16_t)drop_rate_ms,
        .amplitude = (uint8_t)amplitude,
        .damping = (uint8_t)damping,
    };
    water_state[i] = (struct vfx_water_state){0};

    l->api = &vfx_layer_water_api;
    l->config = &water_cfg[i];
    l->state = &water_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_matrix(int zone, int blend, int opacity, uint32_t color, uint32_t head,
                              int speed, int tail, int drop_rate_ms, int columns, int jitter,
                              int head_size) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    matrix_cfg[i] = (struct vfx_matrix_cfg){
        .color = color,
        .head_color = head,
        .speed = (uint16_t)speed,
        .tail = (uint16_t)tail,
        .drop_rate_ms = (uint16_t)drop_rate_ms,
        .columns = (uint8_t)columns,
        .jitter = (uint8_t)jitter,
        .head_size = (uint8_t)head_size,
    };
    matrix_state[i] = (struct vfx_matrix_state){0};

    l->api = &vfx_layer_matrix_api;
    l->config = &matrix_cfg[i];
    l->state = &matrix_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_fire(int zone, int blend, int opacity, uint32_t base, uint32_t tip,
                            int period_ms, int cell, int height, int flicker, int axis) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    fire_cfg[i] = (struct vfx_fire_cfg){
        .base_color = base,
        .tip_color = tip,
        .period_ms = (uint16_t)period_ms,
        .cell = (uint16_t)cell,
        .height = (uint8_t)height,
        .flicker = (uint8_t)flicker,
        .axis = (uint8_t)axis,
    };

    l->api = &vfx_layer_fire_api;
    l->config = &fire_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_comet(int zone, int blend, int opacity, uint32_t color, uint32_t head,
                             int period_ms, int tail, int count, int axis) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    comet_cfg[i] = (struct vfx_comet_cfg){
        .color = color,
        .head_color = head,
        .period_ms = (uint16_t)period_ms,
        .tail = (uint16_t)tail,
        .count = (uint8_t)count,
        .axis = (uint8_t)axis,
    };

    l->api = &vfx_layer_comet_api;
    l->config = &comet_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_cross(int zone, int blend, int opacity, uint32_t color, uint32_t centre,
                             int decay_ms, int radius, int thickness, int axes) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    cross_cfg[i] = (struct vfx_cross_cfg){
        .color = color,
        .centre_color = centre,
        .decay_ms = (uint16_t)decay_ms,
        .radius = (uint16_t)radius,
        .thickness = (uint8_t)thickness,
        .axes = (uint8_t)axes,
    };
    cross_state[i] = (struct vfx_cross_state){0};

    l->api = &vfx_layer_cross_api;
    l->config = &cross_cfg[i];
    l->state = &cross_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_layer_state(int zone, int blend, int opacity, int num_colors) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l || num_colors < 1 || num_colors > MAX_STOPS) {
        return -1;
    }

    const int i = num_layers;
    const uint32_t *staged = (const uint32_t *)(void *)scratch;

    for (int k = 0; k < num_colors; k++) {
        layer_state_colors[i][k] = staged[k];
    }

    layer_state_cfg[i] = (struct vfx_layer_state_cfg){.colors = layer_state_colors[i],
                                                      .num_colors = (uint8_t)num_colors};

    l->api = &vfx_layer_layer_state_api;
    l->config = &layer_state_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_battery(int zone, int blend, int opacity, uint32_t low, uint32_t high,
                               uint32_t empty, int warn_below) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    battery_cfg[i] = (struct vfx_battery_cfg){.low_color = low,
                                              .high_color = high,
                                              .empty_color = empty,
                                              .warn_below = (uint8_t)warn_below};

    l->api = &vfx_layer_battery_api;
    l->config = &battery_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_ble_profile(int zone, int blend, int opacity, uint32_t connected,
                                   uint32_t disconnected, uint32_t usb) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    ble_cfg[i] = (struct vfx_ble_profile_cfg){.connected_color = connected,
                                              .disconnected_color = disconnected,
                                              .usb_color = usb};

    l->api = &vfx_layer_ble_profile_api;
    l->config = &ble_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT void vfx_sim_set_status(int active_layer, int battery, int profile, int connected,
                               int usb) {
    struct vfx_status *st = vfx_status_mutable();

    st->active_layer = (uint8_t)active_layer;
    st->battery_level = (uint8_t)battery;
    st->ble_profile = (uint8_t)profile;
    st->ble_connected = connected != 0;
    st->usb_output = usb != 0;
}

/* The state a keyboard learns from somewhere else: the host's lock LEDs, the
 * modifiers being held, the typing estimate, the other half's cell. Driving
 * them from the page is how an indicator gets checked without a second half
 * and a flat battery.
 */
EXPORT void vfx_sim_set_extra_status(int locks, int modifiers, int wpm, int peripheral_battery) {
    struct vfx_status *st = vfx_status_mutable();

    st->locks = (uint8_t)locks;
    st->modifiers = (uint8_t)modifiers;
    st->wpm = (uint8_t)(wpm < 0 ? 0 : (wpm > 255 ? 255 : wpm));
    st->peripheral_battery[0] = (uint8_t)peripheral_battery;
}

/* Whether anyone is at the keyboard. ZMK decides this from how long it has
 * been since a keypress, which the page has no way to reach, so it is driven
 * directly here.
 */
EXPORT void vfx_sim_set_active(int active) { vfx_status_mutable()->active = active != 0; }

EXPORT int vfx_sim_add_pulse(int zone, int blend, int opacity, uint32_t color, int decay_ms,
                             int min_level, int hue_step, int stack) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    pulse_cfg[i] = (struct vfx_pulse_cfg){
        .color = color,
        .decay_ms = (uint16_t)decay_ms,
        .min_level = (uint8_t)min_level,
        .hue_step = (uint8_t)hue_step,
        .stack = stack != 0,
    };
    pulse_state[i] = (struct vfx_pulse_state){0};

    l->api = &vfx_layer_pulse_api;
    l->config = &pulse_cfg[i];
    l->state = &pulse_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_hold(int zone, int blend, int opacity, uint32_t color, int release_ms) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    hold_cfg[i] = (struct vfx_hold_cfg){
        .color = color,
        .release_ms = (uint16_t)release_ms,
    };
    hold_state[i] = (struct vfx_hold_state){0};

    l->api = &vfx_layer_hold_api;
    l->config = &hold_cfg[i];
    l->state = &hold_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_dart(int zone, int blend, int opacity, uint32_t color, uint32_t head,
                            int speed, int lifetime_ms, int tail, int axis, int reverse) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    dart_cfg[i] = (struct vfx_dart_cfg){
        .color = color,
        .head_color = head,
        .speed = (uint16_t)speed,
        .lifetime_ms = (uint16_t)lifetime_ms,
        .tail = (uint8_t)tail,
        .axis = (uint8_t)axis,
        .reverse = reverse != 0,
    };
    dart_state[i] = (struct vfx_dart_state){0};

    l->api = &vfx_layer_dart_api;
    l->config = &dart_cfg[i];
    l->state = &dart_state[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_static(int zone, int blend, int opacity, uint32_t color, int period_ms,
                              int density, int hue_spread) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    static_cfg[i] = (struct vfx_static_cfg){
        .color = color,
        .period_ms = (uint16_t)period_ms,
        .density = (uint8_t)density,
        .hue_spread = (uint8_t)hue_spread,
    };

    l->api = &vfx_layer_static_api;
    l->config = &static_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_flag(int zone, int blend, int opacity, uint32_t color, int source,
                            int mask) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    flag_cfg[i] = (struct vfx_flag_cfg){
        .color = color, .source = (uint8_t)source, .mask = (uint8_t)mask};

    l->api = &vfx_layer_flag_api;
    l->config = &flag_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_wpm(int zone, int blend, int opacity, uint32_t idle, uint32_t fast,
                           int full, int bar) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    wpm_cfg[i] = (struct vfx_wpm_cfg){
        .idle_color = idle, .fast_color = fast, .full = (uint16_t)full, .bar = (uint8_t)bar};

    l->api = &vfx_layer_wpm_api;
    l->config = &wpm_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT int vfx_sim_add_peripheral_battery(int zone, int blend, int opacity, uint32_t low,
                                          uint32_t high, uint32_t empty, uint32_t unknown,
                                          int source, int warn_below) {
    struct vfx_layer *l = next_layer(zone, blend, opacity);

    if (!l) {
        return -1;
    }

    const int i = num_layers;

    pbat_cfg[i] = (struct vfx_peripheral_battery_cfg){
        .low_color = low,
        .high_color = high,
        .empty_color = empty,
        .unknown_color = unknown,
        .source = (uint8_t)source,
        .warn_below = (uint8_t)warn_below,
    };

    l->api = &vfx_layer_peripheral_battery_api;
    l->config = &pbat_cfg[i];
    l->state = &stateless[i];

    scene.num_layers = (uint8_t)(++num_layers);

    return i;
}

EXPORT void vfx_sim_set_key_map(int num_keys) {
    if (num_keys <= 0) {
        ctx.key_pixels = NULL;
        ctx.num_keys = 0;
        return;
    }

    if (num_keys > MAX_PIXELS) {
        num_keys = MAX_PIXELS;
    }

    /* Copied out of the staging buffer, not aliased to it. */
    for (int i = 0; i < num_keys; i++) {
        key_map[i] = scratch[i];
    }

    ctx.key_pixels = key_map;
    ctx.num_keys = (uint16_t)num_keys;
}

/* ------------------------------------------------------------------- render */

EXPORT void vfx_sim_set_power_policy(int blackout_delay_ms, int settle_ms) {
    power_policy.blackout_delay_ms = (uint16_t)blackout_delay_ms;
    power_policy.settle_ms = (uint16_t)settle_ms;
    vfx_power_reset(&power_ctl);
}

/* The page knows where it draws every LED, so it can hand the engine real
 * positions for whichever wiring is selected. Staged in scratch as int16
 * pairs.
 */
EXPORT void vfx_sim_set_positions(int count) {
    if (count <= 0) {
        ctx.pixel_xy = NULL;
        ctx.num_positions = 0;
        ctx.board = (struct vfx_board_box){0, 0, 0, 0, false};
        return;
    }

    if (count > MAX_PIXELS) {
        count = MAX_PIXELS;
    }

    const int16_t *staged = (const int16_t *)(void *)scratch;

    for (int i = 0; i < count * 2; i++) {
        pixel_xy[i] = staged[i];
    }

    ctx.pixel_xy = pixel_xy;
    ctx.num_positions = (uint16_t)count;

    /* Measure the bounds once, as the firmware does: effects that run across
     * the board ask for them per pixel.
     */
    ctx.board = (struct vfx_board_box){0, 0, 0, 0, false};
    ctx.board = vfx_board_bounds(&ctx);
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

/* The same slew the firmware applies to a synchronisation beacon, so the
 * page shows convergence rather than a snap.
 */
EXPORT int vfx_sim_sync_step(int current_offset, int desired_offset) {
    return (int)vfx_sync_step((int32_t)current_offset, (int32_t)desired_offset);
}

EXPORT int vfx_sim_sync_max_slew(void) { return VFX_SYNC_MAX_SLEW_MS; }

EXPORT void vfx_sim_key_event(int position, int pressed) {
    vfx_scene_key_event(&scene, &ctx, (uint32_t)position, pressed != 0, ctx.time_ms);
}
