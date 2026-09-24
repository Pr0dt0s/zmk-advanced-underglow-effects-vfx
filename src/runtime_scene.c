/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/runtime_scene.h>

static struct vfx_rt_channel channels[VFX_MAX_CHANNELS];

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

/* Filled in from params on every add and every edit, which is what lets an
 * edit be "throw the slot away and build it again" rather than reaching into
 * a generator-specific config field by a generic wire index. Animation state
 * is zeroed along with it: a mid-flight ripple or dart belongs to the config
 * it was launched under, and keeping it across an edit that changed that
 * config would show state that no longer matches what produced it.
 */
static void rebuild_slot(struct vfx_rt_slot *slot) {
    const struct vfx_rt_params *p = &slot->params;

    slot->zone = (struct vfx_zone){.pixels = NULL, .start = p->zone_start, .len = p->zone_len};
    memset(&slot->state, 0, sizeof(slot->state));

    const uint32_t color = VFX_HSB(p->hue, p->sat, p->bri);
    const uint8_t axis = (uint8_t)((p->flags & VFX_RT_FLAG_AXIS_MASK) >> VFX_RT_FLAG_AXIS_SHIFT);

    struct vfx_layer *l = &slot->layer;

    *l = (struct vfx_layer){
        .zone = &slot->zone,
        .blend = p->blend,
        .opacity = p->opacity,
        .opacity_src = VFX_SRC_NONE,
        .opacity_min = 0,
        .opacity_full = 0,
        .tune_id = 0,
    };

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
            .head_color = 0,
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

    default:
        /* Unreachable: every caller validates the type before getting here.
         * Left blank rather than defaulted to a real generator, so a bug
         * upstream renders nothing rather than something misleading.
         */
        break;
    }
}

/* vfx_scene.layers must be one contiguous array in render order; the slots
 * it is built from are addressed by a stable id and are not contiguous
 * once anything has been removed or reordered. Called after any change to
 * render_order or count, never after an in-place edit, which touches only
 * the slot the pointers already reach.
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

void vfx_runtime_init(void) { memset(channels, 0, sizeof(channels)); }

void vfx_runtime_reset(uint8_t ch) {
    if (!valid_channel(ch)) {
        return;
    }

    const bool active = channels[ch].active;

    memset(&channels[ch], 0, sizeof(channels[ch]));
    channels[ch].active = active;
    rebuild_render(&channels[ch]);
}

int vfx_runtime_add_layer(uint8_t ch, const struct vfx_rt_params *params) {
    if (!valid_channel(ch) || params->type >= VFX_RT_TYPE_COUNT) {
        return -1;
    }

    struct vfx_rt_channel *rc = &channels[ch];

    if (rc->count >= VFX_RT_MAX_LAYERS) {
        return -1;
    }

    uint8_t slot = VFX_RT_MAX_LAYERS;

    for (uint8_t i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        if (!rc->slots[i].used) {
            slot = i;
            break;
        }
    }

    if (slot == VFX_RT_MAX_LAYERS) {
        return -1; /* count says there is room; used-tracking disagrees */
    }

    rc->slots[slot].used = true;
    rc->slots[slot].params = *params;
    rebuild_slot(&rc->slots[slot]);

    rc->render_order[rc->count] = slot;
    rc->count++;
    rebuild_render(rc);

    return slot;
}

bool vfx_runtime_set_arg(uint8_t ch, uint8_t slot, uint8_t idx, int16_t value) {
    if (!valid_channel(ch) || idx >= 4 || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];

    s->params.args[idx] = value;
    rebuild_slot(s);

    return true;
}

bool vfx_runtime_set_color(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_slot *s = &channels[ch].slots[slot];

    s->params.hue = (uint16_t)(hue % 360);
    s->params.sat = sat;
    s->params.bri = bri;
    rebuild_slot(s);

    return true;
}

bool vfx_runtime_remove_layer(uint8_t ch, uint8_t slot) {
    if (!valid_channel(ch) || !valid_slot(&channels[ch], slot)) {
        return false;
    }

    struct vfx_rt_channel *rc = &channels[ch];
    const uint8_t pos = render_position(rc, slot);

    if (pos == VFX_RT_MAX_LAYERS) {
        return false;
    }

    for (uint8_t i = pos; i + 1 < rc->count; i++) {
        rc->render_order[i] = rc->render_order[i + 1];
    }

    rc->count--;
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
            saved[ch].slots[i].used = channels[ch].slots[i].used;
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
            rc->slots[i].used = saved[ch].slots[i].used;
            rc->slots[i].params = saved[ch].slots[i].params;

            if (rc->slots[i].used) {
                rebuild_slot(&rc->slots[i]);
            }
        }

        memcpy(rc->render_order, saved[ch].render_order, sizeof(rc->render_order));
        rc->count = saved[ch].count;
        rc->active = saved[ch].active;
        rebuild_render(rc);
    }

    return true;
}
