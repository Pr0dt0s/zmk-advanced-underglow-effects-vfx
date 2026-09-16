/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>

static void solid_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_solid_cfg *cfg = layer->config;
    struct vfx_solid_state *state = layer->state;

    /* Converted once per frame rather than once per pixel: the whole zone is
     * the same color, and HSV->RGB is the most expensive thing we do.
     */
    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    state->rgb = vfx_hsb_to_rgb(hsb);
}

static bool solid_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(ctx);
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_solid_state *state = layer->state;

    *out = state->rgb;

    return true;
}

static bool solid_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(layer);
    VFX_UNUSED(ctx);

    return false;
}

const struct vfx_layer_api vfx_layer_solid_api = {
    .frame = solid_frame,
    .pixel = solid_pixel,
    .is_animating = solid_is_animating,
};
