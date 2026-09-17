/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Cross: a keypress lights the row and column it sits in, which then fade.
 *
 * Where ripple answers "how far is this pixel from the key", this asks "is
 * this pixel in line with the key", so the reaction traces the board's grid
 * instead of ignoring it. Three shapes come out of the same generator:
 *
 *   - the full cross, both arms and no radius limit;
 *   - a band across the board, with axes set to one direction;
 *   - a short cross around the key, with a radius, which is what other
 *     keyboards call a nexus.
 *
 * It needs pixel-positions to know what a row and a column are. Without one
 * it lights a run of the strip either side of the key, which is the closest
 * thing to "in line with it" that strip order can express.
 */

#include <stddef.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

static uint32_t age_of(uint32_t now, uint32_t start) { return now > start ? now - start : 0; }

static void cross_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                            uint32_t position, bool pressed, uint32_t time_ms) {
    if (!pressed) {
        return;
    }

    struct vfx_cross_state *st = layer->state;
    struct vfx_ripple_slot *slot = NULL;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->slots[i].active) {
            slot = &st->slots[i];
            break;
        }
    }

    if (!slot) {
        slot = &st->slots[st->next % VFX_MAX_RIPPLES];
        st->next = (uint8_t)((st->next + 1) % VFX_MAX_RIPPLES);
    }

    slot->active = true;
    slot->start_ms = time_ms;
    slot->origin = vfx_key_pixel(ctx, position);
}

static void cross_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_cross_cfg *cfg = layer->config;
    struct vfx_cross_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->slots[i].active && age_of(ctx->time_ms, st->slots[i].start_ms) > cfg->decay_ms) {
            st->slots[i].active = false;
        }
    }
}

/* How brightly one cross lights a pixel, 0-255, and whether the pixel is the
 * key at its middle. Returns 0 when the pixel is off both arms.
 */
static uint16_t arm_level(const struct vfx_cross_cfg *cfg, const struct vfx_frame_ctx *ctx,
                          uint16_t origin, uint16_t vidx, bool *is_centre) {
    uint32_t across; /* distance from the arm's line */
    uint32_t along;  /* distance along it from the key */

    if (!ctx->pixel_xy || origin >= ctx->num_positions || vidx >= ctx->num_positions) {
        /* No map: a run of the strip either side of the key is the closest
         * thing to "in line with it" that strip order can express.
         */
        across = 0;
        along = (uint32_t)(vidx > origin ? vidx - origin : origin - vidx);
    } else {
        const int32_t dx = (int32_t)ctx->pixel_xy[vidx * 2] - ctx->pixel_xy[origin * 2];
        const int32_t dy = (int32_t)ctx->pixel_xy[vidx * 2 + 1] - ctx->pixel_xy[origin * 2 + 1];

        const uint32_t adx = (uint32_t)(dx < 0 ? -dx : dx);
        const uint32_t ady = (uint32_t)(dy < 0 ? -dy : dy);

        /* The nearer arm wins, so the key's own pixel reads as the middle of
         * a cross rather than as a point on one line.
         */
        const bool on_row = cfg->axes != VFX_CROSS_VERTICAL && ady <= cfg->thickness;
        const bool on_col = cfg->axes != VFX_CROSS_HORIZONTAL && adx <= cfg->thickness;

        if (on_row && (!on_col || adx >= ady)) {
            across = ady;
            along = adx;
        } else if (on_col) {
            across = adx;
            along = ady;
        } else {
            return 0;
        }
    }

    if (across > cfg->thickness) {
        return 0;
    }

    if (cfg->radius != 0 && along > cfg->radius) {
        return 0;
    }

    *is_centre = along <= cfg->thickness;

    /* Fade along the arm when it has a length to fade over. An unbounded
     * cross stays even, which is what makes it read as a line rather than as
     * something radiating.
     */
    if (cfg->radius == 0) {
        return 255;
    }

    return (uint16_t)(255U - (along * 255U) / cfg->radius);
}

static bool cross_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_cross_cfg *cfg = layer->config;
    const struct vfx_cross_state *st = layer->state;

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);

    uint16_t best = 0;
    bool centre = false;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->slots[i].active) {
            continue;
        }

        bool this_centre = false;
        uint16_t level = arm_level(cfg, ctx, st->slots[i].origin, vidx, &this_centre);

        if (level == 0) {
            continue;
        }

        /* Then fade the whole cross out over its lifetime. */
        if (cfg->decay_ms != 0) {
            const uint32_t age = age_of(ctx->time_ms, st->slots[i].start_ms);

            level = (uint16_t)(level * (cfg->decay_ms - age) / cfg->decay_ms);
        }

        if (level > best) {
            best = level;
            centre = this_centre;
        }
    }

    if (best == 0) {
        return false;
    }

    struct vfx_hsb hsb;

    if (centre && cfg->centre_color != 0) {
        hsb = vfx_hsb_unpack(cfg->centre_color);
        hsb.b = (uint8_t)((uint16_t)hsb.b * best / 255U);
    } else {
        hsb = vfx_hsb_unpack(cfg->color);
        hsb.b = (uint8_t)((uint16_t)hsb.b * best / 255U);
    }

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool cross_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_cross_state *st = layer->state;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->slots[i].active) {
            return true;
        }
    }

    return false;
}

const struct vfx_layer_api vfx_layer_cross_api = {
    .frame = cross_frame,
    .pixel = cross_pixel,
    .key_event = cross_key_event,
    .is_animating = cross_is_animating,
};
