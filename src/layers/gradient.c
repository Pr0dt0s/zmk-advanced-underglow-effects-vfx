/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>

/* A cyclic multi-stop gradient, running along whichever axis it is pointed
 * at: the strip by default, or across, down, out from the middle, or around
 * it once the engine has a position map.
 *
 * Position is derived from ctx->time_ms rather than accumulated into state, so
 * two halves that agree on the timebase render the same phase with no further
 * synchronisation. That is what makes the free-running split mode work.
 */

static void gradient_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_gradient_cfg *cfg = layer->config;
    struct vfx_gradient_state *state = layer->state;

    uint32_t span = cfg->span ? cfg->span : vfx_axis_span(ctx, cfg->axis);
    if (span == 0) {
        span = 1;
    }
    state->span = (uint16_t)span;

    /* 64 bit intermediate: time_ms runs to 2^32 and scroll_speed * speed can
     * be a few hundred, which overflows 32 bits after roughly two hours up.
     */
    int64_t px = (int64_t)cfg->scroll_speed * ctx->speed * (int64_t)ctx->time_ms / 1000;

    px %= span;
    if (px < 0) {
        px += span;
    }

    state->scroll_px = (int32_t)px;
}

static bool gradient_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                           uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_gradient_cfg *cfg = layer->config;
    const struct vfx_gradient_state *state = layer->state;

    if (cfg->num_stops == 0) {
        return false;
    }

    const uint16_t span = state->span;
    const uint32_t axis_pos = vfx_axis_pos(ctx, vfx_virtual_idx(ctx, strip_i), cfg->axis);
    const uint32_t pos = (axis_pos + state->scroll_px) % span;

    /* Where we are around the cycle, 0-255. */
    const uint32_t t = (pos * 256U) / span;

    struct vfx_hsb hsb;

    if (cfg->num_stops == 1) {
        hsb = vfx_hsb_unpack(cfg->stops[0]);
    } else {
        /* Cyclic: n stops make n segments, the last wrapping back to stop 0. */
        const uint32_t scaled = t * cfg->num_stops;
        const uint8_t seg = (uint8_t)(scaled / 256U);
        const uint8_t local = (uint8_t)(scaled % 256U);

        hsb = vfx_hsb_lerp(vfx_hsb_unpack(cfg->stops[seg]),
                           vfx_hsb_unpack(cfg->stops[(seg + 1U) % cfg->num_stops]), local);
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool gradient_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_gradient_cfg *cfg = layer->config;

    return cfg->scroll_speed != 0;
}

const struct vfx_layer_api vfx_layer_gradient_api = {
    .frame = gradient_frame,
    .pixel = gradient_pixel,
    .is_animating = gradient_is_animating,
};
