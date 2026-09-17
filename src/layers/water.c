/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Water: a rippling surface, disturbed by falling drops and by typing.
 *
 * Unlike the ripple generator, which draws one expanding band, this models a
 * travelling wavetrain: each drop sends out a decaying oscillation, and
 * overlapping drops sum as signed surface height before the result is
 * coloured. That superposition is what makes it read as water rather than as
 * a set of independent rings, and it is why the height is carried as a signed
 * value all the way to the end instead of being clamped per drop.
 *
 * Drops come from two places. Ambient ones are derived from the frame time
 * alone: the drop for a given epoch is a hash of its epoch number, so nothing
 * is stored between frames and the two halves of a split agree on where the
 * rain falls without exchanging anything. Keypress drops need slots, because
 * a keypress is an event rather than a function of time.
 *
 * Setting drop-rate-ms to 0 leaves only the keypress drops, which makes this
 * a purely reactive layer over whatever is beneath it.
 */

#include <stddef.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

/* Guarded against a timebase that moved backwards, which happens when the
 * synced split mode corrects this half's clock.
 */
static uint32_t age_of(uint32_t now, uint32_t start) { return now > start ? now - start : 0; }

static uint16_t distance(uint16_t a, uint16_t b) { return (uint16_t)(a > b ? a - b : b - a); }

static int32_t clamp32(int32_t v, int32_t lo, int32_t hi) {
    return v < lo ? lo : (v > hi ? hi : v);
}

/* How fast the wavefront travels, after the engine's global speed setting.
 * Scaling the propagation rather than the timebase keeps drop ages honest:
 * a keypress drop is stamped in engine time, so stretching that clock would
 * make presses appear to have happened at the wrong moment.
 */
static uint32_t front_speed(const struct vfx_water_cfg *cfg, const struct vfx_frame_ctx *ctx) {
    const uint32_t speed = (uint32_t)cfg->speed * ctx->speed / 3U;

    return speed ? speed : 1U;
}

/* Signed surface height at one pixel from one drop, roughly -128..127. */
static int32_t drop_height(const struct vfx_water_cfg *cfg, uint32_t speed, uint16_t vidx,
                           uint16_t origin, uint32_t age_ms) {
    if (cfg->lifetime_ms == 0 || age_ms >= cfg->lifetime_ms) {
        return 0;
    }

    const uint16_t r = distance(vidx, origin);
    const uint32_t front = speed * age_ms / 1000U;

    /* The surface ahead of the wavefront has not been reached yet, which is
     * what makes the disturbance spread outward instead of appearing at once.
     */
    if (r > front) {
        return 0;
    }

    const uint16_t wavelength = cfg->wavelength ? cfg->wavelength : 8U;

    /* Phase grows with distance behind the front, so the oscillation trails
     * the leading edge as a wavetrain.
     */
    const uint32_t phase = ((front - r) * 256U) / wavelength;

    /* sin8 spans 0..255 around a midpoint of 128, so this is +-128. Doubled,
     * because a single undamped drop should be able to drive the surface to
     * its full crest colour; at +-128 it could only ever reach halfway.
     */
    const int32_t osc = ((int32_t)vfx_sin8((uint8_t)phase) - 128) * 2;

    /* Energy leaves the drop over its life, and spreads out with distance.
     * The 256 sets the scale of `damping`: a wave falls to half height after
     * 256/damping pixels, so the default reaches across a board rather than
     * dying within a few LEDs.
     */
    const int32_t fade = 255 - (int32_t)(age_ms * 255U / cfg->lifetime_ms);
    const int32_t spread = (255 * 256) / (256 + (int32_t)r * cfg->damping);

    return osc * fade / 255 * spread / 255;
}

/* Rain. Each drop belongs to an epoch of drop_rate_ms, and its position is a
 * hash of the epoch number, so the whole pattern is reproducible from the
 * clock with nothing stored.
 */
static int32_t ambient_height(const struct vfx_water_cfg *cfg, const struct vfx_frame_ctx *ctx,
                              uint32_t speed, uint16_t vidx) {
    if (cfg->drop_rate_ms == 0) {
        return 0;
    }

    const uint16_t span = ctx->virtual_length ? ctx->virtual_length : 1U;
    const uint32_t epoch = ctx->time_ms / cfg->drop_rate_ms;
    int32_t height = 0;

    for (uint8_t i = 0; i < VFX_WATER_MAX_AMBIENT; i++) {
        if (epoch < i) {
            break;
        }

        const uint32_t e = epoch - i;
        const uint32_t started = e * cfg->drop_rate_ms;
        const uint32_t age = age_of(ctx->time_ms, started);

        if (age >= cfg->lifetime_ms) {
            continue;
        }

        const uint16_t origin = (uint16_t)(vfx_hash32(e * 0x9E3779B9U) % span);

        height += drop_height(cfg, speed, vidx, origin, age);
    }

    return height;
}

static void water_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                            uint32_t position, bool pressed, uint32_t time_ms) {
    if (!pressed) {
        return;
    }

    struct vfx_water_state *st = layer->state;
    struct vfx_ripple_slot *slot = NULL;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->drops[i].active) {
            slot = &st->drops[i];
            break;
        }
    }

    /* All slots busy: overwrite the oldest rather than drop the new press,
     * so fast typing keeps disturbing the surface.
     */
    if (!slot) {
        slot = &st->drops[st->next % VFX_MAX_RIPPLES];
        st->next = (uint8_t)((st->next + 1) % VFX_MAX_RIPPLES);
    }

    slot->active = true;
    slot->start_ms = time_ms;
    slot->origin = vfx_key_pixel(ctx, position);
}

static void water_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_water_cfg *cfg = layer->config;
    struct vfx_water_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->drops[i].active && age_of(ctx->time_ms, st->drops[i].start_ms) >= cfg->lifetime_ms) {
            st->drops[i].active = false;
        }
    }
}

static bool water_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_water_cfg *cfg = layer->config;
    const struct vfx_water_state *st = layer->state;

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    const uint32_t speed = front_speed(cfg, ctx);

    int32_t height = ambient_height(cfg, ctx, speed, vidx);

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->drops[i].active) {
            continue;
        }

        height += drop_height(cfg, speed, vidx, st->drops[i].origin,
                              age_of(ctx->time_ms, st->drops[i].start_ms));
    }

    /* Waves interfere before anything is drawn; clamping only now is what
     * lets two drops cancel into flat water instead of both showing.
     */
    height = clamp32(height, -255, 255);

    const int32_t lift = height * cfg->amplitude / 255;

    struct vfx_hsb base = vfx_hsb_unpack(cfg->color);
    struct vfx_hsb hsb;

    if (lift > 0 && cfg->crest_color != 0) {
        /* Crests take on their own colour, which is what gives the surface
         * highlights rather than just a brighter version of the water.
         */
        hsb = vfx_hsb_lerp(base, vfx_hsb_unpack(cfg->crest_color), (uint8_t)clamp32(lift, 0, 255));
    } else if (lift > 0) {
        hsb = base;
        hsb.b = (uint8_t)clamp32(base.b + base.b * lift / 255, 0, 100);
    } else {
        /* Troughs darken toward black. */
        hsb = base;
        hsb.b = (uint8_t)clamp32(base.b * (255 + lift) / 255, 0, 100);
    }

    if (hsb.b == 0) {
        /* Still, unlit water leaves whatever is underneath alone, so this
         * works as a reactive overlay as well as a surface in its own right.
         */
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool water_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_water_cfg *cfg = layer->config;
    const struct vfx_water_state *st = layer->state;

    /* Rain never stops on its own. Without it, the surface is only moving
     * while a keypress drop is alive, so a reactive-only water layer lets the
     * engine park its timer and the power gate cut the rail between presses.
     */
    if (cfg->drop_rate_ms != 0) {
        return true;
    }

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->drops[i].active) {
            return true;
        }
    }

    return false;
}

const struct vfx_layer_api vfx_layer_water_api = {
    .frame = water_frame,
    .pixel = water_pixel,
    .key_event = water_key_event,
    .is_animating = water_is_animating,
};
