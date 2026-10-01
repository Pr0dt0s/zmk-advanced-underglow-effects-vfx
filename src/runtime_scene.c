/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/runtime_scene.h>

static struct vfx_rt_channel channels[VFX_MAX_CHANNELS][VFX_RT_SCENES_PER_CHANNEL];

/* Which scene of each channel is on screen, or VFX_RT_NONE for the compiled
 * list. Kept beside the pool rather than in each scene because at most one of
 * a channel's scenes can be showing.
 */
static uint8_t active_scene[VFX_MAX_CHANNELS];

/* Set by every change that alters what vfx_runtime_state() would write for a
 * scene, cleared by vfx_runtime_take_dirty() as the save work picks it up.
 */
static bool dirty[VFX_MAX_CHANNELS][VFX_RT_SCENES_PER_CHANNEL];

/* A flash entry must fit a settings record and the lot must leave room for
 * everything else on the settings partition. Enforced here so a Kconfig change
 * that outgrows either fails to build instead of failing to save.
 */
_Static_assert(sizeof(struct vfx_rt_saved_channel) <= VFX_RT_SAVED_ENTRY_MAX,
               "one runtime scene no longer fits a settings entry: lower RUNTIME_MAX_LAYERS, "
               "RUNTIME_MAX_COLORS or RUNTIME_MAX_ZONE_PIXELS");
_Static_assert(VFX_MAX_CHANNELS * VFX_RT_SCENES_PER_CHANNEL * sizeof(struct vfx_rt_saved_channel) <=
                   VFX_RT_SAVED_TOTAL_MAX,
               "saved runtime scenes would use too much of the settings partition: lower "
               "RUNTIME_SCENES_PER_CHANNEL or the per-scene limits");

/* What a KEYS zone resolves through. Copied rather than pointed at so this
 * file never depends on the engine's own copy staying put.
 */
static struct vfx_frame_ctx key_ctx;
static bool key_ctx_set;

static bool valid_target(uint8_t target) {
    return VFX_RT_TARGET_CH(target) < VFX_MAX_CHANNELS &&
           VFX_RT_TARGET_SCENE(target) < VFX_RT_SCENES_PER_CHANNEL;
}

/* Callers check valid_target() first, which is what makes this safe. */
static struct vfx_rt_channel *rt(uint8_t target) {
    return &channels[VFX_RT_TARGET_CH(target)][VFX_RT_TARGET_SCENE(target)];
}

static void touch(uint8_t target) {
    dirty[VFX_RT_TARGET_CH(target)][VFX_RT_TARGET_SCENE(target)] = true;
}

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
    memset(active_scene, VFX_RT_NONE, sizeof(active_scene));
    memset(dirty, 0, sizeof(dirty));
    memset(&key_ctx, 0, sizeof(key_ctx));
    key_ctx_set = false;
}

void vfx_runtime_reset(uint8_t target) {
    if (!valid_target(target)) {
        return;
    }

    memset(rt(target), 0, sizeof(*rt(target)));
    rebuild_render(rt(target));
    touch(target);
}

void vfx_runtime_set_key_context(const struct vfx_frame_ctx *ctx) {
    key_ctx = *ctx;
    key_ctx_set = true;

    for (uint8_t ch = 0; ch < VFX_MAX_CHANNELS; ch++) {
        for (uint8_t sc = 0; sc < VFX_RT_SCENES_PER_CHANNEL; sc++) {
            for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
                struct vfx_rt_slot *s = &channels[ch][sc].slots[i];

                if (s->used && s->params.zone_kind == VFX_RT_ZONE_KEYS) {
                    resolve_zone(s);
                }
            }
        }
    }
}

static int add_layer(uint8_t target, const struct vfx_rt_params *params, bool staged) {
    if (!valid_target(target) || !params_valid(params)) {
        return VFX_RT_ERR_INVALID;
    }

    struct vfx_rt_channel *rc = rt(target);

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

    touch(target);

    return slot;
}

int vfx_runtime_add_layer(uint8_t target, const struct vfx_rt_params *params) {
    return add_layer(target, params, false);
}

int vfx_runtime_add_layer_staged(uint8_t target, const struct vfx_rt_params *params) {
    return add_layer(target, params, true);
}

bool vfx_runtime_commit_layer(uint8_t target, uint8_t slot, uint8_t position) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_channel *rc = rt(target);

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
    touch(target);

    return true;
}

bool vfx_runtime_set_flags(uint8_t target, uint8_t slot, uint8_t flags) {
    if (!valid_target(target) || !valid_slot(rt(target), slot) ||
        (flags & (uint8_t)~VFX_RT_FLAGS_MASK) != 0) {
        return false;
    }

    struct vfx_rt_channel *rc = rt(target);
    struct vfx_rt_slot *s = &rc->slots[slot];

    s->params.flags = flags;

    /* rebuild_slot() rewrites slot->layer, whose copy sits in render[] -- the
     * same reason set_opts rebuilds the render array.
     */
    if (!rebuild_slot(rc, s)) {
        return false;
    }

    rebuild_render(rc);
    touch(target);

    return true;
}

bool vfx_runtime_set_arg(uint8_t target, uint8_t slot, uint8_t idx, int16_t value) {
    if (!valid_target(target) || idx >= VFX_RT_MAX_ARGS || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &rt(target)->slots[slot];

    s->params.args[idx] = value;

    touch(target);

    return rebuild_slot(rt(target), s);
}

bool vfx_runtime_set_color(uint8_t target, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &rt(target)->slots[slot];

    s->params.hue = (uint16_t)(hue % 360);
    s->params.sat = sat;
    s->params.bri = bri;

    touch(target);

    return rebuild_slot(rt(target), s);
}

bool vfx_runtime_remove_layer(uint8_t target, uint8_t slot) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_channel *rc = rt(target);
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
    touch(target);

    return true;
}

bool vfx_runtime_move_layer(uint8_t target, uint8_t slot, int8_t direction) {
    if (!valid_target(target) || !valid_slot(rt(target), slot) ||
        (direction != VFX_RT_MOVE_UP && direction != VFX_RT_MOVE_DOWN)) {
        return false;
    }

    struct vfx_rt_channel *rc = rt(target);
    const uint8_t pos = render_position(rc, slot);
    const int neighbour = (int)pos + direction;

    if (pos == VFX_RT_MAX_LAYERS || neighbour < 0 || neighbour >= rc->count) {
        return false;
    }

    const uint8_t tmp = rc->render_order[pos];

    rc->render_order[pos] = rc->render_order[neighbour];
    rc->render_order[neighbour] = tmp;
    rebuild_render(rc);
    touch(target);

    return true;
}

bool vfx_runtime_set_active(uint8_t target, bool active) {
    if (!valid_target(target)) {
        return false;
    }

    const uint8_t ch = VFX_RT_TARGET_CH(target);
    const uint8_t sc = VFX_RT_TARGET_SCENE(target);

    if (active) {
        if (active_scene[ch] != VFX_RT_NONE && active_scene[ch] != sc) {
            dirty[ch][active_scene[ch]] = true;
        }

        active_scene[ch] = sc;
        touch(target);
    } else if (active_scene[ch] == sc) {
        active_scene[ch] = VFX_RT_NONE;
        touch(target);
    }

    return true;
}

void vfx_runtime_deactivate(uint8_t ch) {
    if (ch < VFX_MAX_CHANNELS && active_scene[ch] != VFX_RT_NONE) {
        dirty[ch][active_scene[ch]] = true;
        active_scene[ch] = VFX_RT_NONE;
    }
}

bool vfx_runtime_is_active(uint8_t target) {
    return valid_target(target) && active_scene[VFX_RT_TARGET_CH(target)] == VFX_RT_TARGET_SCENE(target);
}

uint8_t vfx_runtime_active_scene(uint8_t ch) {
    return ch < VFX_MAX_CHANNELS ? active_scene[ch] : VFX_RT_NONE;
}

const struct vfx_scene *vfx_runtime_current(uint8_t ch) {
    const uint8_t sc = vfx_runtime_active_scene(ch);

    return sc == VFX_RT_NONE ? NULL : vfx_runtime_scene(VFX_RT_TARGET(ch, sc));
}

const struct vfx_scene *vfx_runtime_scene(uint8_t target) {
    if (!valid_target(target) || rt(target)->count == 0) {
        return NULL;
    }

    return &rt(target)->scene;
}

bool vfx_runtime_get_info(uint8_t target, uint8_t *count, bool *active) {
    if (!valid_target(target)) {
        return false;
    }

    *count = rt(target)->count;
    *active = vfx_runtime_is_active(target);

    return true;
}

bool vfx_runtime_get_layer(uint8_t target, uint8_t slot, struct vfx_rt_params *out) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    *out = rt(target)->slots[slot].params;

    return true;
}

bool vfx_runtime_get_order(uint8_t target, uint8_t *order, uint8_t *count) {
    if (!valid_target(target)) {
        return false;
    }

    const struct vfx_rt_channel *rc = rt(target);

    memcpy(order, rc->render_order, rc->count);
    *count = rc->count;

    return true;
}

bool vfx_runtime_set_list_color(uint8_t target, uint8_t slot, uint8_t idx, uint16_t hue,
                                uint8_t sat, uint8_t bri) {
    if (!valid_target(target) || idx >= VFX_RT_MAX_COLORS || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &rt(target)->slots[slot];
    struct vfx_rt_params *p = &s->params;

    for (uint8_t i = p->num_colors; i < idx; i++) {
        p->colors[i] = 0;
    }

    p->colors[idx] = VFX_HSB(hue % 360, sat, bri);

    if (idx >= p->num_colors) {
        p->num_colors = (uint8_t)(idx + 1);
    }

    touch(target);

    return rebuild_slot(rt(target), s);
}

bool vfx_runtime_get_list_color(uint8_t target, uint8_t slot, uint8_t idx, uint16_t *hue,
                                uint8_t *sat, uint8_t *bri) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    const struct vfx_rt_params *p = &rt(target)->slots[slot].params;

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

bool vfx_runtime_gradient_add_stop(uint8_t target, uint8_t slot, uint16_t hue, uint8_t sat,
                                   uint8_t bri) {
    if (!valid_target(target) || !valid_gradient_slot(rt(target), slot)) {
        return false;
    }

    return vfx_runtime_set_list_color(target, slot, rt(target)->slots[slot].params.num_colors, hue,
                                      sat, bri);
}

bool vfx_runtime_gradient_get_stop(uint8_t target, uint8_t slot, uint8_t idx, uint16_t *hue,
                                   uint8_t *sat, uint8_t *bri) {
    if (!valid_target(target) || !valid_gradient_slot(rt(target), slot)) {
        return false;
    }

    return vfx_runtime_get_list_color(target, slot, idx, hue, sat, bri);
}

bool vfx_runtime_set_zone(uint8_t target, uint8_t slot, uint8_t kind, uint8_t offset,
                          const uint8_t *data, uint8_t count) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &rt(target)->slots[slot];
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
    touch(target);

    return true;
}

bool vfx_runtime_get_zone(uint8_t target, uint8_t slot, uint8_t offset, uint8_t *kind, uint8_t *total,
                          uint8_t *data, uint8_t max, uint8_t *n) {
    if (!valid_target(target) || !valid_slot(rt(target), slot)) {
        return false;
    }

    const struct vfx_rt_params *p = &rt(target)->slots[slot].params;
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

bool vfx_runtime_set_opts(uint8_t target, uint8_t slot, uint8_t blend, uint8_t opacity,
                          uint8_t opacity_src, uint8_t opacity_min, uint8_t opacity_full,
                          uint8_t tune_id) {
    if (!valid_target(target) || !valid_slot(rt(target), slot) || blend > VFX_BLEND_MAX ||
        opacity_src > VFX_SRC_ACTIVITY) {
        return false;
    }

    struct vfx_rt_channel *rc = rt(target);
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
    touch(target);

    return true;
}

/* Scratch home for vfx_runtime_state()'s answer. Static rather than a local
 * the caller must size itself, same as vfx_tuning_state() -- the caller
 * only ever wants to hand this straight to settings_save_one().
 */
static struct vfx_rt_saved_channel saved;

const void *vfx_runtime_state(uint8_t target, uint16_t *len) {
    if (!valid_target(target)) {
        return NULL;
    }

    const struct vfx_rt_channel *rc = rt(target);

    memset(&saved, 0, sizeof(saved));

    for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        saved.slots[i].params = rc->slots[i].params;
        saved.slots[i].used = rc->slots[i].used && render_position(rc, i) != VFX_RT_MAX_LAYERS;
    }

    memcpy(saved.render_order, rc->render_order, sizeof(saved.render_order));
    saved.count = rc->count;
    saved.active = vfx_runtime_is_active(target);

    if (len) {
        *len = (uint16_t)sizeof(saved);
    }

    return &saved;
}

bool vfx_runtime_restore_one(uint8_t target, const void *blob, uint16_t len) {
    if (!valid_target(target) || len != sizeof(saved)) {
        return false;
    }

    const struct vfx_rt_saved_channel *in = blob;
    struct vfx_rt_channel *rc = rt(target);

    memset(rc, 0, sizeof(*rc));

    for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        struct vfx_rt_slot *s = &rc->slots[i];

        if (!in->slots[i].used || !params_valid(&in->slots[i].params)) {
            continue;
        }

        s->params = in->slots[i].params;

        if (rebuild_slot(rc, s)) {
            s->used = true;
        } else {
            memset(s, 0, sizeof(*s));
        }
    }

    /* Saved order first, trusted only as far as it names distinct slots that
     * survived; anything used it missed goes on the end. For a blob this
     * build wrote that changes nothing.
     */
    bool listed[VFX_RT_MAX_LAYERS] = {false};
    const uint8_t saved_count = in->count < VFX_RT_MAX_LAYERS ? in->count : VFX_RT_MAX_LAYERS;

    for (uint8_t i = 0; i < saved_count; i++) {
        const uint8_t idx = in->render_order[i];

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

    rebuild_render(rc);

    const uint8_t ch = VFX_RT_TARGET_CH(target);
    const uint8_t sc = VFX_RT_TARGET_SCENE(target);

    if (in->active) {
        active_scene[ch] = sc;
    } else if (active_scene[ch] == sc) {
        active_scene[ch] = VFX_RT_NONE;
    }

    /* What was just loaded is what flash already holds. */
    dirty[ch][sc] = false;

    return true;
}

bool vfx_runtime_take_dirty(uint8_t *target) {
    for (uint8_t ch = 0; ch < VFX_MAX_CHANNELS; ch++) {
        for (uint8_t sc = 0; sc < VFX_RT_SCENES_PER_CHANNEL; sc++) {
            if (dirty[ch][sc]) {
                dirty[ch][sc] = false;
                *target = VFX_RT_TARGET(ch, sc);

                return true;
            }
        }
    }

    return false;
}

/* FNV-1a over what a host could have sent, written byte by byte so struct
 * padding, endianness and every derived field stay out of it. Left out on
 * purpose: which scene is active, because that is the one thing a half may
 * legitimately differ on until the next NEXT/PREV lands; zone_pixels, which
 * depends on each half's own strip offset; and the heavy pool, which is
 * private to each half.
 */
#define FNV_BASIS 2166136261u
#define FNV_PRIME 16777619u

static uint32_t fnv8(uint32_t h, uint8_t v) { return (h ^ v) * FNV_PRIME; }

static uint32_t fnv16(uint32_t h, uint16_t v) { return fnv8(fnv8(h, (uint8_t)v), (uint8_t)(v >> 8)); }

static uint32_t fnv32(uint32_t h, uint32_t v) { return fnv16(fnv16(h, (uint16_t)v), (uint16_t)(v >> 16)); }

uint32_t vfx_runtime_hash(uint8_t target) {
    uint32_t h = FNV_BASIS;

    if (!valid_target(target)) {
        return h;
    }

    const struct vfx_rt_channel *rc = rt(target);

    h = fnv8(h, rc->count);

    for (uint8_t n = 0; n < rc->count; n++) {
        const uint8_t slot = rc->render_order[n];
        const struct vfx_rt_params *p = &rc->slots[slot].params;

        h = fnv8(h, slot);
        h = fnv8(h, p->type);
        h = fnv8(h, p->zone_kind);

        if (p->zone_kind == VFX_RT_ZONE_RANGE) {
            h = fnv8(h, p->zone_start);
            h = fnv8(h, p->zone_len);
        } else {
            h = fnv8(h, p->zone_count);

            for (uint8_t i = 0; i < p->zone_count; i++) {
                h = fnv8(h, p->zone_items[i]);
            }
        }

        h = fnv8(h, p->blend);
        h = fnv8(h, p->opacity);
        h = fnv8(h, p->opacity_src);
        h = fnv8(h, p->opacity_min);
        h = fnv8(h, p->opacity_full);
        h = fnv8(h, p->tune_id);
        h = fnv16(h, p->hue);
        h = fnv8(h, p->sat);
        h = fnv8(h, p->bri);

        for (uint8_t i = 0; i < VFX_RT_MAX_ARGS; i++) {
            h = fnv16(h, (uint16_t)p->args[i]);
        }

        h = fnv8(h, p->flags);
        h = fnv8(h, p->num_colors);

        for (uint8_t i = 0; i < p->num_colors; i++) {
            h = fnv32(h, p->colors[i]);
        }
    }

    return h;
}

/* ---- replay -------------------------------------------------------------- */

/* The scene as a request stream, for rebuilding it on a half that disagrees.
 * Slots are re-added staged and committed at the end, so the other half never
 * shows a scene mid-rebuild, and slot ids come out the same because a
 * placeholder fills every id below the highest committed slot that is not one
 * (add always takes the lowest free id). A staged-but-uncommitted slot is not
 * part of the scene the hash describes, so it is replayed as a gap too.
 */

#define REPLAY_STEP_ARG4 1
#define REPLAY_STEP_ARG5 2
#define REPLAY_STEP_COLOR0 3
#define REPLAY_STEP_ZONE0 (REPLAY_STEP_COLOR0 + VFX_RT_MAX_COLORS)
#define REPLAY_ZONE_CHUNKS ((VFX_RT_MAX_ZONE_PIXELS + VFX_HID_ZONE_CHUNK - 1) / VFX_HID_ZONE_CHUNK)
#define REPLAY_STEP_OPTS (REPLAY_STEP_ZONE0 + REPLAY_ZONE_CHUNKS)
#define REPLAY_STEP_END (REPLAY_STEP_OPTS + 1)

enum replay_phase {
    REPLAY_RESET,
    REPLAY_ADD,
    REPLAY_REMOVE_GAPS,
    REPLAY_COMMIT,
    REPLAY_ACTIVATE,
    REPLAY_DONE,
};

static bool replay_slot_committed(const struct vfx_rt_channel *rc, uint8_t slot) {
    return rc->slots[slot].used && render_position(rc, slot) != VFX_RT_MAX_LAYERS;
}

bool vfx_runtime_replay_begin(struct vfx_rt_replay *c, uint8_t target) {
    if (!valid_target(target)) {
        return false;
    }

    memset(c, 0, sizeof(*c));
    c->target = target;
    c->phase = REPLAY_RESET;

    const struct vfx_rt_channel *rc = rt(target);

    for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        if (replay_slot_committed(rc, i)) {
            c->ids = (uint8_t)(i + 1);
        }
    }

    return true;
}

/* Fills `out` for step `step` of slot `slot`, or returns false when that step
 * has nothing to say for this slot.
 */
static bool replay_slot_step(const struct vfx_rt_channel *rc, uint8_t slot, uint8_t step,
                             struct vfx_hid_request *out) {
    const struct vfx_rt_params *p = &rc->slots[slot].params;

    out->slot = slot;

    if (step == REPLAY_STEP_ARG4 || step == REPLAY_STEP_ARG5) {
        const uint8_t idx = (uint8_t)(step - REPLAY_STEP_ARG4 + 4);

        if (p->args[idx] == 0) {
            return false;
        }

        out->op = VFX_HID_OP_SCENE_SET_ARG;
        out->arg_idx = idx;
        out->args[0] = p->args[idx];

        return true;
    }

    if (step >= REPLAY_STEP_COLOR0 && step < REPLAY_STEP_ZONE0) {
        const uint8_t i = (uint8_t)(step - REPLAY_STEP_COLOR0);

        if (i >= p->num_colors) {
            return false;
        }

        const struct vfx_hsb hsb = vfx_hsb_unpack(p->colors[i]);

        out->op = VFX_HID_OP_SCENE_SET_LIST_COLOR;
        out->arg_idx = i;
        out->hue = (int16_t)hsb.h;
        out->sat = hsb.s;
        out->bri = hsb.b;

        return true;
    }

    if (step >= REPLAY_STEP_ZONE0 && step < REPLAY_STEP_OPTS) {
        const uint8_t chunk = (uint8_t)(step - REPLAY_STEP_ZONE0);
        const uint8_t offset = (uint8_t)(chunk * VFX_HID_ZONE_CHUNK);

        /* A range rides in the add. A list is sent in chunks; an empty one
         * still needs its chunk 0, which is what sets the zone's kind.
         */
        if (p->zone_kind == VFX_RT_ZONE_RANGE || (offset > 0 && offset >= p->zone_count)) {
            return false;
        }

        const uint8_t left = p->zone_count > offset ? (uint8_t)(p->zone_count - offset) : 0;
        const uint8_t n = left < VFX_HID_ZONE_CHUNK ? left : VFX_HID_ZONE_CHUNK;

        out->op = VFX_HID_OP_SCENE_SET_ZONE;
        out->zone_kind = p->zone_kind;
        out->zone_offset = offset;
        out->zone_count = n;

        if (n > 0) {
            memcpy(out->zone_data, &p->zone_items[offset], n);
        }

        return true;
    }

    if (step == REPLAY_STEP_OPTS) {
        if (p->opacity_src == 0 && p->opacity_min == 0 && p->opacity_full == 0 && p->tune_id == 0) {
            return false;
        }

        out->op = VFX_HID_OP_SCENE_SET_OPTS;
        out->blend = p->blend;
        out->opacity = p->opacity;
        out->opacity_src = p->opacity_src;
        out->opacity_min = p->opacity_min;
        out->opacity_full = p->opacity_full;
        out->tune_id = p->tune_id;

        return true;
    }

    return false;
}

bool vfx_runtime_replay_next(struct vfx_rt_replay *c, struct vfx_hid_request *out) {
    const struct vfx_rt_channel *rc = rt(c->target);

    memset(out, 0, sizeof(*out));
    out->ch = c->target;

    for (;;) {
        switch ((enum replay_phase)c->phase) {
        case REPLAY_RESET:
            out->op = VFX_HID_OP_SCENE_RESET;
            c->phase = REPLAY_ADD;
            c->slot = 0;
            c->step = 0;

            return true;

        case REPLAY_ADD:
            if (c->slot >= c->ids) {
                c->phase = REPLAY_REMOVE_GAPS;
                c->slot = 0;
                break;
            }

            if (c->step == 0) {
                out->op = VFX_HID_OP_SCENE_ADD_LAYER;
                out->staged = true;
                out->slot = VFX_HID_NO_SLOT;
                c->step = 1;

                if (replay_slot_committed(rc, c->slot)) {
                    const struct vfx_rt_params *p = &rc->slots[c->slot].params;

                    out->type = p->type;
                    out->zone_start = p->zone_start;
                    out->zone_len = p->zone_len;
                    out->blend = p->blend;
                    out->opacity = p->opacity;
                    out->hue = (int16_t)p->hue;
                    out->sat = p->sat;
                    out->bri = p->bri;
                    memcpy(out->args, p->args, sizeof(out->args));
                    out->flags = p->flags;
                } else {
                    out->type = VFX_RT_SOLID; /* a placeholder, removed again below */
                    c->step = REPLAY_STEP_END;
                }

                return true;
            }

            while (c->step < REPLAY_STEP_END) {
                const uint8_t step = c->step++;

                if (replay_slot_step(rc, c->slot, step, out)) {
                    return true;
                }
            }

            c->slot++;
            c->step = 0;
            break;

        case REPLAY_REMOVE_GAPS:
            while (c->slot < c->ids) {
                const uint8_t slot = c->slot++;

                if (!replay_slot_committed(rc, slot)) {
                    out->op = VFX_HID_OP_SCENE_REMOVE_LAYER;
                    out->slot = slot;

                    return true;
                }
            }

            c->phase = REPLAY_COMMIT;
            c->slot = 0; /* the index into render order from here on */
            break;

        case REPLAY_COMMIT:
            if (c->slot >= rc->count) {
                c->phase = REPLAY_ACTIVATE;
                break;
            }

            out->op = VFX_HID_OP_SCENE_COMMIT_LAYER;
            out->slot = rc->render_order[c->slot++];
            out->position = VFX_HID_POSITION_TOP;

            return true;

        case REPLAY_ACTIVATE:
            c->phase = REPLAY_DONE;

            if (vfx_runtime_is_active(c->target)) {
                out->op = VFX_HID_OP_SCENE_ACTIVATE;

                return true;
            }

            break;

        case REPLAY_DONE:
        default:
            return false;
        }
    }
}
