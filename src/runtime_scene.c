/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/runtime_scene.h>

static struct vfx_rt_channel channels[VFX_MAX_CHANNELS];

/* What a KEYS zone resolves through. Copied rather than pointed at so this
 * file never depends on the engine's own copy staying put.
 */
static struct vfx_frame_ctx key_ctx;
static bool key_ctx_set;

static bool valid_channel(uint8_t ch) { return ch < VFX_MAX_CHANNELS; }

static bool valid_slot(const struct vfx_rt_channel *rc, uint8_t slot) {
    return slot < VFX_RT_MAX_LAYERS && rc->slots[slot].used;
}

/* Position of `slot` in render order, or VFX_RT_MAX_LAYERS if it is not
 * currently rendered -- which add_layer relies on, since a freshly added
 * slot is used but not yet in render_order.
 */
static uint8_t render_position(const struct vfx_rt_channel *rc, uint8_t slot) {
    for (uint8_t i = 0; i < rc->count; i++) {
        if (rc->render_order[i] == slot) {
            return i;
        }
    }

    return VFX_RT_MAX_LAYERS;
}

/* Everything that goes into the length-checked persisted image and the
 * arrays it indexes, so a corrupt blob cannot walk off the end of one.
 */
static bool params_valid(const struct vfx_rt_params *p) {
    return p->type < VFX_RT_TYPE_COUNT && p->zone_kind <= VFX_RT_ZONE_KEYS &&
           p->zone_count <= VFX_RT_MAX_ZONE_PIXELS && p->num_colors <= VFX_RT_MAX_COLORS;
}

/* Entry `i` of the colour list, or 0 -- black, which every generator that
 * takes an optional colour already reads as "unset" -- past its end.
 */
static uint32_t color_at(const struct vfx_rt_params *p, uint8_t i) {
    return i < p->num_colors ? p->colors[i] : 0;
}

static bool acquire_heavy(struct vfx_rt_channel *rc, struct vfx_rt_slot *slot) {
    if (slot->has_heavy) {
        return true;
    }

    for (uint8_t i = 0; i < VFX_RT_HEAVY_STATES; i++) {
        if (!rc->heavy_used[i]) {
            rc->heavy_used[i] = true;
            slot->has_heavy = true;
            slot->heavy_idx = i;

            return true;
        }
    }

    return false;
}

static void release_heavy(struct vfx_rt_channel *rc, struct vfx_rt_slot *slot) {
    if (slot->has_heavy) {
        rc->heavy_used[slot->heavy_idx] = false;
        slot->has_heavy = false;
    }
}

/* Fills slot->zone from params. A PIXELS zone points straight at the slot's
 * own list; a KEYS zone is resolved into the slot's scratch. Neither moves,
 * so layer.zone never needs touching after the first build.
 */
static void resolve_zone(struct vfx_rt_slot *slot) {
    const struct vfx_rt_params *p = &slot->params;

    switch (p->zone_kind) {
    case VFX_RT_ZONE_PIXELS:
        slot->zone = (struct vfx_zone){.pixels = p->zone_items, .start = 0, .len = p->zone_count};
        break;

    case VFX_RT_ZONE_KEYS:
        slot->zone = (struct vfx_zone){.pixels = slot->zone_pixels, .start = 0, .len = 0};

        if (key_ctx_set) {
            const struct vfx_key_zone kz = {
                .keys = p->zone_items,
                .num_keys = p->zone_count,
                .pixels = slot->zone_pixels,
                .zone = &slot->zone,
            };

            vfx_key_zone_resolve(&kz, &key_ctx);
        }
        break;

    default:
        slot->zone = (struct vfx_zone){.pixels = NULL, .start = p->zone_start, .len = p->zone_len};
        break;
    }
}

static void apply_opts(struct vfx_rt_slot *slot) {
    const struct vfx_rt_params *p = &slot->params;

    slot->layer.blend = p->blend;
    slot->layer.opacity = p->opacity;
    slot->layer.opacity_src = p->opacity_src;
    slot->layer.opacity_min = p->opacity_min;
    slot->layer.opacity_full = p->opacity_full;
    slot->layer.tune_id = p->tune_id;
}

/* Filled in from params on every add and every edit, which is what lets an
 * edit be "throw the slot away and build it again" rather than reaching into
 * a generator-specific config field by a generic wire index. Animation state
 * is zeroed along with it: a mid-flight ripple or dart belongs to the config
 * it was launched under, and keeping it across an edit that changed that
 * config would show state that no longer matches what produced it.
 *
 * False only when a trail or hold cannot get one of the channel's heavy
 * states; the slot is then left unbuilt and the caller gives it back.
 *
 * Which colour and number goes where (args[n] is the n'th listed):
 *   solid      color
 *   breathe    color; period_ms, min_level, hue_swing
 *   wave       color; wavelength, period_ms, depth; axis
 *   twinkle    color; period_ms, density, hue_spread
 *   plasma     color; scale, period_ms, hue_spread
 *   ripple     color; decay_ms, speed, width
 *   keyflash   color; decay_ms, spread
 *   pulse      color; decay_ms, min_level, hue_step; stack flag
 *   dart       color, head_color=colors[0]; speed, lifetime_ms, tail; axis, reverse
 *   static     color; period_ms, density, hue_spread
 *   gradient   colors[] = stops; scroll_speed, span; axis
 *   trail      color; decay_ms, spread
 *   hold       color; release_ms
 *   water      color, crest=colors[0]; wavelength, speed, lifetime_ms, drop_rate_ms,
 *              amplitude, damping
 *   matrix     color, head=colors[0]; speed, tail, drop_rate_ms, columns, jitter, head_size
 *   fire       base=color, tip=colors[0]; period_ms, cell, height, flicker; axis
 *   comet      color, head=colors[0]; period_ms, tail, count; axis
 *   cross      color, centre=colors[0]; decay_ms, radius, thickness, axes
 *   layer_state colors[] = one per keymap layer
 *   battery    low=color, high=colors[0], empty=colors[1]; warn_below
 *   ble_profile connected=color, disconnected=colors[0], usb=colors[1]
 *   flag       color; source, mask
 *   wpm        idle=color, fast=colors[0]; full, bar
 *   peripheral_battery low=color, high=colors[0], empty=colors[1], unknown=colors[2];
 *              source, warn_below
 */
static bool rebuild_slot(struct vfx_rt_channel *rc, struct vfx_rt_slot *slot) {
    const struct vfx_rt_params *p = &slot->params;
    const bool heavy = p->type == VFX_RT_TRAIL || p->type == VFX_RT_HOLD;

    if (heavy && !acquire_heavy(rc, slot)) {
        return false;
    }

    resolve_zone(slot);
    memset(&slot->state, 0, sizeof(slot->state));

    if (heavy) {
        memset(&rc->heavy[slot->heavy_idx], 0, sizeof(rc->heavy[slot->heavy_idx]));
    }

    const uint32_t color = VFX_HSB(p->hue, p->sat, p->bri);
    const uint8_t axis = (uint8_t)((p->flags & VFX_RT_FLAG_AXIS_MASK) >> VFX_RT_FLAG_AXIS_SHIFT);

    struct vfx_layer *l = &slot->layer;

    *l = (struct vfx_layer){.zone = &slot->zone};
    apply_opts(slot);

    switch ((enum vfx_rt_type)p->type) {
    case VFX_RT_SOLID:
        slot->cfg.solid = (struct vfx_solid_cfg){.color = color};
        l->api = &vfx_layer_solid_api;
        l->config = &slot->cfg.solid;
        l->state = &slot->state.solid;
        break;

    case VFX_RT_BREATHE:
        slot->cfg.breathe = (struct vfx_breathe_cfg){
            .color = color,
            .period_ms = (uint16_t)p->args[0],
            .min_level = (uint8_t)p->args[1],
            .hue_swing = (uint8_t)p->args[2],
        };
        l->api = &vfx_layer_breathe_api;
        l->config = &slot->cfg.breathe;
        l->state = &slot->state.breathe;
        break;

    case VFX_RT_WAVE:
        slot->cfg.wave = (struct vfx_wave_cfg){
            .color = color,
            .wavelength = (uint16_t)p->args[0],
            .period_ms = (uint16_t)p->args[1],
            .depth = (uint8_t)p->args[2],
            .axis = axis,
        };
        l->api = &vfx_layer_wave_api;
        l->config = &slot->cfg.wave;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_TWINKLE:
        slot->cfg.twinkle = (struct vfx_twinkle_cfg){
            .color = color,
            .period_ms = (uint16_t)p->args[0],
            .density = (uint8_t)p->args[1],
            .hue_spread = (uint8_t)p->args[2],
        };
        l->api = &vfx_layer_twinkle_api;
        l->config = &slot->cfg.twinkle;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_PLASMA:
        slot->cfg.plasma = (struct vfx_plasma_cfg){
            .color = color,
            .scale = (uint16_t)p->args[0],
            .period_ms = (uint16_t)p->args[1],
            .hue_spread = (uint8_t)p->args[2],
        };
        l->api = &vfx_layer_plasma_api;
        l->config = &slot->cfg.plasma;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_RIPPLE:
        slot->cfg.ripple = (struct vfx_ripple_cfg){
            .color = color,
            .decay_ms = (uint16_t)p->args[0],
            .speed = (uint16_t)p->args[1],
            .width = (uint8_t)p->args[2],
        };
        l->api = &vfx_layer_ripple_api;
        l->config = &slot->cfg.ripple;
        l->state = &slot->state.ripple;
        break;

    case VFX_RT_KEYFLASH:
        slot->cfg.keyflash = (struct vfx_keyflash_cfg){
            .color = color,
            .decay_ms = (uint16_t)p->args[0],
            .spread = (uint8_t)p->args[1],
        };
        l->api = &vfx_layer_keyflash_api;
        l->config = &slot->cfg.keyflash;
        l->state = &slot->state.keyflash;
        break;

    case VFX_RT_PULSE:
        slot->cfg.pulse = (struct vfx_pulse_cfg){
            .color = color,
            .decay_ms = (uint16_t)p->args[0],
            .min_level = (uint8_t)p->args[1],
            .hue_step = (uint8_t)p->args[2],
            .stack = (p->flags & VFX_RT_FLAG_STACK) != 0,
        };
        l->api = &vfx_layer_pulse_api;
        l->config = &slot->cfg.pulse;
        l->state = &slot->state.pulse;
        break;

    case VFX_RT_DART:
        slot->cfg.dart = (struct vfx_dart_cfg){
            .color = color,
            .head_color = color_at(p, 0),
            .speed = (uint16_t)p->args[0],
            .lifetime_ms = (uint16_t)p->args[1],
            .tail = (uint8_t)p->args[2],
            .axis = axis,
            .reverse = (p->flags & VFX_RT_FLAG_REVERSE) != 0,
        };
        l->api = &vfx_layer_dart_api;
        l->config = &slot->cfg.dart;
        l->state = &slot->state.dart;
        break;

    case VFX_RT_STATIC:
        slot->cfg.static_cfg = (struct vfx_static_cfg){
            .color = color,
            .period_ms = (uint16_t)p->args[0],
            .density = (uint8_t)p->args[1],
            .hue_spread = (uint8_t)p->args[2],
        };
        l->api = &vfx_layer_static_api;
        l->config = &slot->cfg.static_cfg;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_GRADIENT:
        /* .stops points into this same slot's own params, not a copy --
         * safe because params outlives everything rebuild_slot() derives
         * from it, and because a stop list is only ever written in place
         * (vfx_runtime_set_list_color()), never reallocated.
         */
        slot->cfg.gradient = (struct vfx_gradient_cfg){
            .stops = p->colors,
            .num_stops = p->num_colors,
            .scroll_speed = p->args[0],
            .span = (uint16_t)p->args[1],
            .axis = axis,
        };
        l->api = &vfx_layer_gradient_api;
        l->config = &slot->cfg.gradient;
        l->state = &slot->state.gradient;
        break;

    case VFX_RT_TRAIL:
        slot->cfg.trail = (struct vfx_trail_cfg){
            .color = color,
            .decay_ms = (uint16_t)p->args[0],
            .spread = (uint8_t)p->args[1],
        };
        l->api = &vfx_layer_trail_api;
        l->config = &slot->cfg.trail;
        l->state = &rc->heavy[slot->heavy_idx].trail;
        break;

    case VFX_RT_HOLD:
        slot->cfg.hold = (struct vfx_hold_cfg){
            .color = color,
            .release_ms = (uint16_t)p->args[0],
        };
        l->api = &vfx_layer_hold_api;
        l->config = &slot->cfg.hold;
        l->state = &rc->heavy[slot->heavy_idx].hold;
        break;

    case VFX_RT_WATER:
        slot->cfg.water = (struct vfx_water_cfg){
            .color = color,
            .crest_color = color_at(p, 0),
            .wavelength = (uint16_t)p->args[0],
            .speed = (uint16_t)p->args[1],
            .lifetime_ms = (uint16_t)p->args[2],
            .drop_rate_ms = (uint16_t)p->args[3],
            .amplitude = (uint8_t)p->args[4],
            .damping = (uint8_t)p->args[5],
        };
        l->api = &vfx_layer_water_api;
        l->config = &slot->cfg.water;
        l->state = &slot->state.water;
        break;

    case VFX_RT_MATRIX:
        slot->cfg.matrix = (struct vfx_matrix_cfg){
            .color = color,
            .head_color = color_at(p, 0),
            .speed = (uint16_t)p->args[0],
            .tail = (uint16_t)p->args[1],
            .drop_rate_ms = (uint16_t)p->args[2],
            .columns = (uint8_t)p->args[3],
            .jitter = (uint8_t)p->args[4],
            .head_size = (uint8_t)p->args[5],
        };
        l->api = &vfx_layer_matrix_api;
        l->config = &slot->cfg.matrix;
        l->state = &slot->state.matrix;
        break;

    case VFX_RT_FIRE:
        slot->cfg.fire = (struct vfx_fire_cfg){
            .base_color = color,
            .tip_color = color_at(p, 0),
            .period_ms = (uint16_t)p->args[0],
            .cell = (uint16_t)p->args[1],
            .height = (uint8_t)p->args[2],
            .flicker = (uint8_t)p->args[3],
            .axis = axis,
        };
        l->api = &vfx_layer_fire_api;
        l->config = &slot->cfg.fire;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_COMET:
        slot->cfg.comet = (struct vfx_comet_cfg){
            .color = color,
            .head_color = color_at(p, 0),
            .period_ms = (uint16_t)p->args[0],
            .tail = (uint16_t)p->args[1],
            .count = (uint8_t)p->args[2],
            .axis = axis,
        };
        l->api = &vfx_layer_comet_api;
        l->config = &slot->cfg.comet;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_CROSS:
        slot->cfg.cross = (struct vfx_cross_cfg){
            .color = color,
            .centre_color = color_at(p, 0),
            .decay_ms = (uint16_t)p->args[0],
            .radius = (uint16_t)p->args[1],
            .thickness = (uint8_t)p->args[2],
            .axes = (uint8_t)p->args[3],
        };
        l->api = &vfx_layer_cross_api;
        l->config = &slot->cfg.cross;
        l->state = &slot->state.cross;
        break;

    case VFX_RT_LAYER_STATE:
        slot->cfg.layer_state = (struct vfx_layer_state_cfg){
            .colors = p->colors,
            .num_colors = p->num_colors,
        };
        l->api = &vfx_layer_layer_state_api;
        l->config = &slot->cfg.layer_state;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_BATTERY:
        slot->cfg.battery = (struct vfx_battery_cfg){
            .low_color = color,
            .high_color = color_at(p, 0),
            .empty_color = color_at(p, 1),
            .warn_below = (uint8_t)p->args[0],
        };
        l->api = &vfx_layer_battery_api;
        l->config = &slot->cfg.battery;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_BLE_PROFILE:
        slot->cfg.ble_profile = (struct vfx_ble_profile_cfg){
            .connected_color = color,
            .disconnected_color = color_at(p, 0),
            .usb_color = color_at(p, 1),
        };
        l->api = &vfx_layer_ble_profile_api;
        l->config = &slot->cfg.ble_profile;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_FLAG:
        slot->cfg.flag = (struct vfx_flag_cfg){
            .color = color,
            .source = (uint8_t)p->args[0],
            .mask = (uint8_t)p->args[1],
        };
        l->api = &vfx_layer_flag_api;
        l->config = &slot->cfg.flag;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_WPM:
        slot->cfg.wpm = (struct vfx_wpm_cfg){
            .idle_color = color,
            .fast_color = color_at(p, 0),
            .full = (uint16_t)p->args[0],
            .bar = (uint8_t)p->args[1],
        };
        l->api = &vfx_layer_wpm_api;
        l->config = &slot->cfg.wpm;
        l->state = &slot->state.stateless;
        break;

    case VFX_RT_PERIPHERAL_BATTERY:
        slot->cfg.peripheral_battery = (struct vfx_peripheral_battery_cfg){
            .low_color = color,
            .high_color = color_at(p, 0),
            .empty_color = color_at(p, 1),
            .unknown_color = color_at(p, 2),
            .source = (uint8_t)p->args[0],
            .warn_below = (uint8_t)p->args[1],
        };
        l->api = &vfx_layer_peripheral_battery_api;
        l->config = &slot->cfg.peripheral_battery;
        l->state = &slot->state.stateless;
        break;

    default:
        /* Unreachable: every caller validates the type before getting here.
         * Left blank rather than defaulted to a real generator, so a bug
         * upstream renders nothing rather than something misleading.
         */
        break;
    }

    return true;
}

/* vfx_scene.layers must be one contiguous array in render order; the slots
 * it is built from are addressed by a stable id and are not contiguous
 * once anything has been removed or reordered. Called after any change to
 * render_order or count, and after a layer's own options change (blend,
 * opacity and friends are copied into render[] here); never after any other
 * in-place edit, which touches only the slot the pointers already reach.
 */
static void rebuild_render(struct vfx_rt_channel *rc) {
    for (uint8_t i = 0; i < rc->count; i++) {
        rc->render[i] = rc->slots[rc->render_order[i]].layer;
    }

    rc->scene = (struct vfx_scene){
        .name = "runtime",
        .layers = rc->render,
        .num_layers = rc->count,
    };
}

void vfx_runtime_init(void) {
    memset(channels, 0, sizeof(channels));
    memset(&key_ctx, 0, sizeof(key_ctx));
    key_ctx_set = false;
}

void vfx_runtime_reset(uint8_t ch) {
    if (!valid_channel(ch)) {
        return;
    }

    const bool active = channels[ch].active;

    memset(&channels[ch], 0, sizeof(channels[ch]));
    channels[ch].active = active;
    rebuild_render(&channels[ch]);
}

void vfx_runtime_set_key_context(const struct vfx_frame_ctx *ctx) {
    key_ctx = *ctx;
    key_ctx_set = true;

    for (uint8_t ch = 0; ch < VFX_MAX_CHANNELS; ch++) {
        for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
            struct vfx_rt_slot *s = &channels[ch].slots[i];

            if (s->used && s->params.zone_kind == VFX_RT_ZONE_KEYS) {
                resolve_zone(s);
            }
        }
    }
}

static int add_layer(uint8_t ch, const struct vfx_rt_params *params, bool staged) {
    if (!valid_channel(ch) || !params_valid(params)) {
        return VFX_RT_ERR_INVALID;
    }

    struct vfx_rt_channel *rc = &channels[ch];

    if (rc->count >= VFX_RT_MAX_LAYERS) {
        return VFX_RT_ERR_FULL;
    }

    uint8_t slot = VFX_RT_MAX_LAYERS;

    for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        if (!rc->slots[i].used) {
            slot = i;
            break;
        }
    }

    if (slot == VFX_RT_MAX_LAYERS) {
        return VFX_RT_ERR_FULL; /* count says there is room; used-tracking disagrees */
    }

    struct vfx_rt_slot *s = &rc->slots[slot];

    memset(s, 0, sizeof(*s));
    s->params = *params;

    if (!rebuild_slot(rc, s)) {
        memset(s, 0, sizeof(*s));

        return VFX_RT_ERR_FULL;
    }

    s->used = true;

    /* A staged slot is used -- so it can be edited, and holds its heavy
     * state -- but sits outside render_order and count until committed, which
     * is what keeps a half-built layer off the screen.
     */
    if (!staged) {
        rc->render_order[rc->count] = slot;
        rc->count++;
        rebuild_render(rc);
    }

    return slot;
}

int vfx_runtime_add_layer(uint8_t ch, const struct vfx_rt_params *params) {
    return add_layer(ch, params, false);
}

int vfx_runtime_add_layer_staged(uint8_t ch, const struct vfx_rt_params *params) {
    return add_layer(ch, params, true);
}

bool vfx_runtime_commit_layer(uint8_t ch, uint8_t slot, uint8_t position) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];

    if (render_position(rc, slot) != VFX_RT_MAX_LAYERS || rc->count >= VFX_RT_MAX_LAYERS) {
        return false; /* already rendered, or nowhere left to put it */
    }

    const uint8_t pos = position < rc->count ? position : rc->count;

    for (uint8_t i = rc->count; i > pos; i--) {
        rc->render_order[i] = rc->render_order[i - 1];
    }

    rc->render_order[pos] = slot;
    rc->count++;
    rebuild_render(rc);

    return true;
}

bool vfx_runtime_set_flags(uint8_t ch, uint8_t slot, uint8_t flags) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot) ||
        (flags & (uint8_t)~VFX_RT_FLAGS_MASK) != 0) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];
    struct vfx_rt_slot *s = &rc->slots[slot];

    s->params.flags = flags;

    /* rebuild_slot() rewrites slot->layer, whose copy sits in render[] -- the
     * same reason set_opts rebuilds the render array.
     */
    if (!rebuild_slot(rc, s)) {
        return false;
    }

    rebuild_render(rc);

    return true;
}

bool vfx_runtime_set_arg(uint8_t ch, uint8_t slot, uint8_t idx, int16_t value) {
    if (!valid_channel(ch) || idx >= VFX_RT_MAX_ARGS || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];

    s->params.args[idx] = value;

    return rebuild_slot(&channels[ch], s);
}

bool vfx_runtime_set_color(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];

    s->params.hue = (uint16_t)(hue % 360);
    s->params.sat = sat;
    s->params.bri = bri;

    return rebuild_slot(&channels[ch], s);
}

bool vfx_runtime_remove_layer(uint8_t ch, uint8_t slot) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];
    const uint8_t pos = render_position(rc, slot);

    /* A staged slot has no position; removing it is how a half-built layer is
     * abandoned.
     */
    if (pos != VFX_RT_MAX_LAYERS) {
        for (uint8_t i = pos; i + 1 < rc->count; i++) {
            rc->render_order[i] = rc->render_order[i + 1];
        }

        rc->count--;
    }

    release_heavy(rc, &rc->slots[slot]);
    rc->slots[slot].used = false;
    rebuild_render(rc);

    return true;
}

bool vfx_runtime_move_layer(uint8_t ch, uint8_t slot, int8_t direction) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot) ||
        (direction != VFX_RT_MOVE_UP && direction != VFX_RT_MOVE_DOWN)) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];
    const uint8_t pos = render_position(rc, slot);
    const int neighbour = (int)pos + direction;

    if (pos == VFX_RT_MAX_LAYERS || neighbour < 0 || neighbour >= rc->count) {
        return false;
    }

    const uint8_t tmp = rc->render_order[pos];

    rc->render_order[pos] = rc->render_order[neighbour];
    rc->render_order[neighbour] = tmp;
    rebuild_render(rc);

    return true;
}

bool vfx_runtime_set_active(uint8_t ch, bool active) {
    if (!valid_channel(ch)) {
        return false;
    }

    channels[ch].active = active;

    return true;
}

bool vfx_runtime_is_active(uint8_t ch) { return valid_channel(ch) && channels[ch].active; }

const struct vfx_scene *vfx_runtime_scene(uint8_t ch) {
    if (!valid_channel(ch) || channels[ch].count == 0) {
        return NULL;
    }

    return &channels[ch].scene;
}

bool vfx_runtime_get_info(uint8_t ch, uint8_t *count, bool *active) {
    if (!valid_channel(ch)) {
        return false;
    }

    *count = channels[ch].count;
    *active = channels[ch].active;

    return true;
}

bool vfx_runtime_get_layer(uint8_t ch, uint8_t slot, struct vfx_rt_params *out) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    *out = channels[ch].slots[slot].params;

    return true;
}

bool vfx_runtime_get_order(uint8_t ch, uint8_t *order, uint8_t *count) {
    if (!valid_channel(ch)) {
        return false;
    }

    const struct vfx_rt_channel *rc = &channels[ch];

    memcpy(order, rc->render_order, rc->count);
    *count = rc->count;

    return true;
}

bool vfx_runtime_set_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t hue,
                                uint8_t sat, uint8_t bri) {
    if (!valid_channel(ch) || idx >= VFX_RT_MAX_COLORS || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];
    struct vfx_rt_params *p = &s->params;

    for (uint8_t i = p->num_colors; i < idx; i++) {
        p->colors[i] = 0;
    }

    p->colors[idx] = VFX_HSB(hue % 360, sat, bri);

    if (idx >= p->num_colors) {
        p->num_colors = (uint8_t)(idx + 1);
    }

    return rebuild_slot(&channels[ch], s);
}

bool vfx_runtime_get_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                uint8_t *sat, uint8_t *bri) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    const struct vfx_rt_params *p = &channels[ch].slots[slot].params;

    if (idx >= p->num_colors) {
        return false;
    }

    const struct vfx_hsb hsb = vfx_hsb_unpack(p->colors[idx]);

    *hue = hsb.h;
    *sat = hsb.s;
    *bri = hsb.b;

    return true;
}

static bool valid_gradient_slot(const struct vfx_rt_channel *rc, uint8_t slot) {
    return valid_slot(rc, slot) && rc->slots[slot].params.type == VFX_RT_GRADIENT;
}

bool vfx_runtime_gradient_add_stop(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat,
                                   uint8_t bri) {
    if (!valid_channel(ch) || !valid_gradient_slot(&channels[ch], slot)) {
        return false;
    }

    return vfx_runtime_set_list_color(ch, slot, channels[ch].slots[slot].params.num_colors, hue,
                                      sat, bri);
}

bool vfx_runtime_gradient_get_stop(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                   uint8_t *sat, uint8_t *bri) {
    if (!valid_channel(ch) || !valid_gradient_slot(&channels[ch], slot)) {
        return false;
    }

    return vfx_runtime_get_list_color(ch, slot, idx, hue, sat, bri);
}

bool vfx_runtime_set_zone(uint8_t ch, uint8_t slot, uint8_t kind, uint8_t offset,
                          const uint8_t *data, uint8_t count) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];
    struct vfx_rt_params *p = &s->params;

    if (kind == VFX_RT_ZONE_RANGE) {
        if (count < 2) {
            return false;
        }

        p->zone_kind = VFX_RT_ZONE_RANGE;
        p->zone_start = data[0];
        p->zone_len = data[1];
        p->zone_count = 0;
    } else if (kind == VFX_RT_ZONE_PIXELS || kind == VFX_RT_ZONE_KEYS) {
        /* Offset 0 starts a list over, and is the only offset that may also
         * change what kind of list it is -- a chunk arriving mid-list under a
         * different kind is a confused sender, not a request to convert.
         */
        const uint8_t have = (offset == 0 || p->zone_kind != kind) ? 0 : p->zone_count;

        if (offset != have || offset + count > VFX_RT_MAX_ZONE_PIXELS) {
            return false;
        }

        p->zone_kind = kind;

        if (count > 0) {
            memcpy(&p->zone_items[offset], data, count);
        }

        p->zone_count = (uint8_t)(offset + count);
    } else {
        return false;
    }

    resolve_zone(s);

    return true;
}

bool vfx_runtime_get_zone(uint8_t ch, uint8_t slot, uint8_t offset, uint8_t *kind, uint8_t *total,
                          uint8_t *data, uint8_t max, uint8_t *n) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    const struct vfx_rt_params *p = &channels[ch].slots[slot].params;
    uint8_t range[2] = {p->zone_start, p->zone_len};
    const uint8_t *src = p->zone_items;
    uint8_t len = p->zone_count;

    if (p->zone_kind == VFX_RT_ZONE_RANGE) {
        src = range;
        len = 2;
    }

    if (offset > len) {
        return false;
    }

    const uint8_t avail = (uint8_t)(len - offset);
    const uint8_t take = avail < max ? avail : max;

    memcpy(data, &src[offset], take);
    *kind = p->zone_kind;
    *total = len;
    *n = take;

    return true;
}

bool vfx_runtime_set_opts(uint8_t ch, uint8_t slot, uint8_t blend, uint8_t opacity,
                          uint8_t opacity_src, uint8_t opacity_min, uint8_t opacity_full,
                          uint8_t tune_id) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot) || blend > VFX_BLEND_MAX ||
        opacity_src > VFX_SRC_ACTIVITY) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];
    struct vfx_rt_slot *s = &rc->slots[slot];

    s->params.blend = blend;
    s->params.opacity = opacity;
    s->params.opacity_src = opacity_src;
    s->params.opacity_min = opacity_min;
    s->params.opacity_full = opacity_full;
    s->params.tune_id = tune_id;

    /* Animation state is left alone: none of these change what a generator
     * has drawn so far, only how it is composited.
     */
    apply_opts(s);
    rebuild_render(rc);

    return true;
}

/* Scratch home for vfx_runtime_state()'s answer. Static rather than a local
 * the caller must size itself, same as vfx_tuning_state() -- the caller
 * only ever wants to hand this straight to settings_save_one() or compare
 * its length before a restore.
 */
static struct vfx_rt_saved_channel saved[VFX_MAX_CHANNELS];

const void *vfx_runtime_state(uint16_t *len) {
    for (uint8_t ch = 0; ch < VFX_MAX_CHANNELS; ch++) {
        for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
            saved[ch].slots[i].params = channels[ch].slots[i].params;
            saved[ch].slots[i].used = channels[ch].slots[i].used &&
                                      render_position(&channels[ch], i) != VFX_RT_MAX_LAYERS;
        }

        memcpy(saved[ch].render_order, channels[ch].render_order,
              sizeof(saved[ch].render_order));
        saved[ch].count = channels[ch].count;
        saved[ch].active = channels[ch].active;
    }

    if (len) {
        *len = (uint16_t)sizeof(saved);
    }

    return saved;
}

bool vfx_runtime_restore_state(const void *blob, uint16_t len) {
    if (len != sizeof(saved)) {
        return false;
    }

    memcpy(saved, blob, sizeof(saved));

    for (uint8_t ch = 0; ch < VFX_MAX_CHANNELS; ch++) {
        struct vfx_rt_channel *rc = &channels[ch];

        memset(rc, 0, sizeof(*rc));

        for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
            struct vfx_rt_slot *s = &rc->slots[i];

            if (!saved[ch].slots[i].used || !params_valid(&saved[ch].slots[i].params)) {
                continue;
            }

            s->params = saved[ch].slots[i].params;

            if (rebuild_slot(rc, s)) {
                s->used = true;
            } else {
                memset(s, 0, sizeof(*s));
            }
        }

        /* Saved order first, trusted only as far as it names distinct slots
         * that survived; anything used it missed goes on the end. For a blob
         * this build wrote that changes nothing.
         */
        bool listed[VFX_RT_MAX_LAYERS] = {false};
        const uint8_t saved_count =
            saved[ch].count < VFX_RT_MAX_LAYERS ? saved[ch].count : VFX_RT_MAX_LAYERS;

        for (uint8_t i = 0; i < saved_count; i++) {
            const uint8_t idx = saved[ch].render_order[i];

            if (idx < VFX_RT_MAX_LAYERS && rc->slots[idx].used && !listed[idx]) {
                listed[idx] = true;
                rc->render_order[rc->count++] = idx;
            }
        }

        for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
            if (rc->slots[i].used && !listed[i]) {
                rc->render_order[rc->count++] = i;
            }
        }

        rc->active = saved[ch].active;
        rebuild_render(rc);
    }

    return true;
}
