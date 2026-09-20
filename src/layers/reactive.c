/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Reactive generators: ripple, keyflash, trail.
 *
 * These are the only generators that carry state between frames, because a
 * keypress is an event rather than a function of time. State is a fixed array
 * of slots sized at compile time; nothing here allocates.
 *
 * Origins are recorded in virtual (whole board) coordinates, so once the
 * halves are synchronised a ripple started on one side lands in the right
 * place on the other without any translation.
 */

#include <stddef.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

/* Claim a slot, preferring a free one and otherwise overwriting the oldest.
 * Dropping the new press instead would make fast typing feel unresponsive,
 * which is the opposite of what a reactive effect is for.
 */
static struct vfx_ripple_slot *claim_slot(struct vfx_ripple_slot *slots, uint8_t *next,
                                          uint32_t time_ms, uint16_t origin) {
    struct vfx_ripple_slot *chosen = NULL;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!slots[i].active) {
            chosen = &slots[i];
            break;
        }
    }

    if (!chosen) {
        chosen = &slots[*next % VFX_MAX_RIPPLES];
        *next = (uint8_t)((*next + 1) % VFX_MAX_RIPPLES);
    }

    chosen->active = true;
    chosen->start_ms = time_ms;
    chosen->origin = origin;

    return chosen;
}

/* Elapsed time guarded against a timebase that moved backwards, which happens
 * when the synced split mode corrects this half's clock.
 */
static uint32_t age_of(uint32_t now, uint32_t start) { return now > start ? now - start : 0; }

/* ------------------------------------------------------------------- ripple */

static void ripple_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                             uint32_t position, bool pressed, uint32_t time_ms) {
    if (!pressed) {
        return;
    }

    struct vfx_ripple_state *st = layer->state;

    claim_slot(st->slots, &st->next, time_ms, vfx_key_pixel(ctx, position));
}

static void ripple_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_ripple_cfg *cfg = layer->config;
    struct vfx_ripple_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->slots[i].active) {
            continue;
        }

        const uint32_t age = age_of(ctx->time_ms, st->slots[i].start_ms);

        if (age >= cfg->decay_ms) {
            st->slots[i].active = false;
            continue;
        }

        /* Retire a ripple whose front has already run past every pixel.
         * Holding it for the rest of its decay would draw nothing while still
         * reporting the layer as animating, which keeps the engine ticking
         * and the power rail up for an invisible effect.
         */
        const uint32_t radius = (uint32_t)cfg->speed * age / 1000U;

        if (radius > (uint32_t)vfx_board_extent(ctx) + cfg->width) {
            st->slots[i].active = false;
        }
    }
}

static bool ripple_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                         uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_ripple_cfg *cfg = layer->config;
    const struct vfx_ripple_state *st = layer->state;

    if (cfg->decay_ms == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    uint16_t best = 0;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->slots[i].active) {
            continue;
        }

        const uint32_t age = age_of(ctx->time_ms, st->slots[i].start_ms);
        if (age >= cfg->decay_ms) {
            continue;
        }

        /* Front position, and how far this pixel is from it. */
        const uint32_t radius = (uint32_t)cfg->speed * age / 1000U;
        const uint16_t d = vfx_pixel_distance(ctx, vidx, st->slots[i].origin);
        const uint16_t off = (uint16_t)(d > radius ? d - radius : radius - d);

        if (cfg->width == 0 || off > cfg->width) {
            continue;
        }

        /* Bright at the front, tapering across the band, and fading out over
         * the ripple's lifetime.
         */
        const uint16_t band = (uint16_t)(255U - (off * 255U) / cfg->width);
        const uint16_t fade = (uint16_t)(255U - (age * 255U) / cfg->decay_ms);
        const uint16_t level = (uint16_t)(band * fade / 255U);

        /* Overlapping ripples take the brighter rather than summing, which
         * would clip to white as soon as two met.
         */
        if (level > best) {
            best = level;
        }
    }

    if (best == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    hsb.b = (uint8_t)((uint16_t)hsb.b * best / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool ripple_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_ripple_state *st = layer->state;

    /* Idle between keypresses, which lets the engine park its timer and the
     * power gate cut the rail on a scene that is only reactive.
     */
    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->slots[i].active) {
            return true;
        }
    }

    return false;
}

const struct vfx_layer_api vfx_layer_ripple_api = {
    .frame = ripple_frame,
    .pixel = ripple_pixel,
    .key_event = ripple_key_event,
    .is_animating = ripple_is_animating,
};

/* ----------------------------------------------------------------- keyflash */

static void keyflash_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                               uint32_t position, bool pressed, uint32_t time_ms) {
    if (!pressed) {
        return;
    }

    struct vfx_keyflash_state *st = layer->state;

    claim_slot(st->slots, &st->next, time_ms, vfx_key_pixel(ctx, position));
}

static void keyflash_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_keyflash_cfg *cfg = layer->config;
    struct vfx_keyflash_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->slots[i].active && age_of(ctx->time_ms, st->slots[i].start_ms) >= cfg->decay_ms) {
            st->slots[i].active = false;
        }
    }
}

static bool keyflash_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                           uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_keyflash_cfg *cfg = layer->config;
    const struct vfx_keyflash_state *st = layer->state;

    if (cfg->decay_ms == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    uint16_t best = 0;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->slots[i].active) {
            continue;
        }

        const uint16_t d = vfx_pixel_distance(ctx, vidx, st->slots[i].origin);
        if (d > cfg->spread) {
            continue;
        }

        const uint32_t age = age_of(ctx->time_ms, st->slots[i].start_ms);
        if (age >= cfg->decay_ms) {
            continue;
        }

        /* Unlike a ripple this does not travel: it lights where the key is
         * and fades in place.
         */
        const uint16_t falloff =
            cfg->spread ? (uint16_t)(255U - (d * 255U) / cfg->spread) : 255U;
        const uint16_t fade = (uint16_t)(255U - (age * 255U) / cfg->decay_ms);
        const uint16_t level = (uint16_t)(falloff * fade / 255U);

        if (level > best) {
            best = level;
        }
    }

    if (best == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    hsb.b = (uint8_t)((uint16_t)hsb.b * best / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool keyflash_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_keyflash_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->slots[i].active) {
            return true;
        }
    }

    return false;
}

const struct vfx_layer_api vfx_layer_keyflash_api = {
    .frame = keyflash_frame,
    .pixel = keyflash_pixel,
    .key_event = keyflash_key_event,
    .is_animating = keyflash_is_animating,
};

/* -------------------------------------------------------------------- trail */

static void trail_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                            uint32_t position, bool pressed, uint32_t time_ms) {
    VFX_UNUSED(time_ms);

    if (!pressed) {
        return;
    }

    const struct vfx_trail_cfg *cfg = layer->config;
    struct vfx_trail_state *st = layer->state;

    const uint16_t origin = vfx_key_pixel(ctx, position);
    const uint8_t spread = cfg->spread;

    /* Deposit heat around the key. Unlike the slot based effects this keeps a
     * per-pixel value, so overlapping presses build up into a heat map rather
     * than competing for a fixed number of slots.
     *
     * Every pixel is measured rather than walking a window of indices either
     * side of the origin: with a position map, the pixels near a key are not
     * the ones next to it on the wire.
     */
    const uint16_t limit = ctx->virtual_length < VFX_TRAIL_MAX_PIXELS ? ctx->virtual_length
                                                                     : VFX_TRAIL_MAX_PIXELS;

    for (uint16_t p = 0; p < limit; p++) {
        const uint16_t d = vfx_pixel_distance(ctx, p, origin);

        if (d > spread) {
            continue;
        }

        const uint16_t falloff = spread ? (uint16_t)(255U - ((uint32_t)d * 255U) / spread) : 255U;

        if (falloff > st->heat[p]) {
            st->heat[p] = (uint8_t)falloff;
        }
    }
}

static void trail_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_trail_cfg *cfg = layer->config;
    struct vfx_trail_state *st = layer->state;

    if (cfg->decay_ms == 0) {
        return;
    }

    const uint32_t elapsed = age_of(ctx->time_ms, st->last_ms);
    st->last_ms = ctx->time_ms;

    if (elapsed == 0) {
        return;
    }

    /* Decay proportional to real elapsed time rather than per frame, so the
     * trail lasts the same wall time whatever the frame rate is doing.
     */
    const uint32_t drop = (elapsed * 255U) / cfg->decay_ms;

    if (drop == 0) {
        return;
    }

    for (uint16_t i = 0; i < VFX_TRAIL_MAX_PIXELS; i++) {
        st->heat[i] = st->heat[i] > drop ? (uint8_t)(st->heat[i] - drop) : 0;
    }
}

static bool trail_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_trail_cfg *cfg = layer->config;
    const struct vfx_trail_state *st = layer->state;

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);

    if (vidx >= VFX_TRAIL_MAX_PIXELS || st->heat[vidx] == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    hsb.b = (uint8_t)((uint16_t)hsb.b * st->heat[vidx] / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool trail_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_trail_state *st = layer->state;

    for (uint16_t i = 0; i < VFX_TRAIL_MAX_PIXELS; i++) {
        if (st->heat[i]) {
            return true;
        }
    }

    return false;
}

/* -------------------------------------------------------------------- pulse */

/* The only reactive generator that never asks where the key was.
 *
 * Everything else here places itself against the pressed key, which needs the
 * pixels to sit under the keys in the first place. Underglow does not: it is
 * behind the board, so the useful reading of a keypress there is that one
 * happened at all, and the whole zone answers together.
 */

static void pulse_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                            uint32_t position, bool pressed, uint32_t time_ms) {
    VFX_UNUSED(ctx);
    VFX_UNUSED(position);
    VFX_UNUSED(time_ms);

    if (!pressed) {
        return;
    }

    const struct vfx_pulse_cfg *cfg = layer->config;
    struct vfx_pulse_state *st = layer->state;

    if (cfg->stack) {
        /* Typing faster than the decay piles up toward full rather than
         * pinning there, so a fast run still reads as busier than one key.
         */
        const uint16_t sum = (uint16_t)st->level + 128U;

        st->level = sum > 255U ? 255U : (uint8_t)sum;
    } else {
        st->level = 255;
    }

    st->hue_offset = vfx_hue_add(st->hue_offset, (int16_t)cfg->hue_step);
}

static void pulse_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_pulse_cfg *cfg = layer->config;
    struct vfx_pulse_state *st = layer->state;

    const uint32_t elapsed = age_of(ctx->time_ms, st->last_ms);

    st->last_ms = ctx->time_ms;

    if (cfg->decay_ms == 0 || elapsed == 0) {
        return;
    }

    /* Proportional to real elapsed time rather than to frames, so a pulse
     * lasts the same wall time whatever the frame rate is doing.
     */
    const uint32_t drop = (elapsed * 255U) / cfg->decay_ms;

    st->level = st->level > drop ? (uint8_t)(st->level - drop) : 0;
}

static bool pulse_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_pulse_cfg *cfg = layer->config;
    const struct vfx_pulse_state *st = layer->state;

    const uint8_t level = st->level > cfg->min_level ? st->level : cfg->min_level;

    if (level == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);

    hsb.h = vfx_hue_add(hsb.h, (int16_t)ctx->hue_shift);
    hsb.h = vfx_hue_add(hsb.h, (int16_t)st->hue_offset);
    hsb.b = (uint8_t)((uint16_t)hsb.b * level / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool pulse_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_pulse_state *st = layer->state;

    /* Sitting at the floor is a still picture, so the engine may park until
     * the next press wakes it.
     */
    return st->level > 0;
}

const struct vfx_layer_api vfx_layer_pulse_api = {
    .frame = pulse_frame,
    .pixel = pulse_pixel,
    .key_event = pulse_key_event,
    .is_animating = pulse_is_animating,
};

const struct vfx_layer_api vfx_layer_trail_api = {
    .frame = trail_frame,
    .pixel = trail_pixel,
    .key_event = trail_key_event,
    .is_animating = trail_is_animating,
};
