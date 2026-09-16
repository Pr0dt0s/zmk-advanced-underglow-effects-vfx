/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Ambient generators: breathe, wave, twinkle, plasma.
 *
 * All four derive their output from ctx->time_ms and the pixel's virtual
 * index alone. Nothing accumulates between frames, so a dropped frame does
 * not shift the animation and two halves sharing a timebase stay in phase
 * without exchanging anything.
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

/* Speed is 1-5 with 3 as the configured period. Scaling the phase rather than
 * the period keeps the arithmetic integer and monotonic.
 */
static uint32_t phase_turns(const struct vfx_frame_ctx *ctx, uint16_t period_ms) {
    if (period_ms == 0) {
        return 0;
    }

    const uint32_t scaled = (uint32_t)ctx->time_ms * ctx->speed / 3U;

    return (scaled * 256U) / period_ms;
}

/* ------------------------------------------------------------------ breathe */

static void breathe_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_breathe_cfg *cfg = layer->config;
    struct vfx_breathe_state *st = layer->state;

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    const uint8_t level = vfx_sin8((uint8_t)phase_turns(ctx, cfg->period_ms));
    const uint8_t floored =
        (uint8_t)(cfg->min_level + ((255U - cfg->min_level) * level) / 255U);

    hsb.b = (uint8_t)((uint16_t)hsb.b * floored / 255U);

    /* One conversion for the whole zone: every pixel is the same colour. */
    st->rgb = vfx_hsb_to_rgb(hsb);
}

static bool breathe_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                          uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(ctx);
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_breathe_state *st = layer->state;

    *out = st->rgb;

    return true;
}

const struct vfx_layer_api vfx_layer_breathe_api = {
    .frame = breathe_frame,
    .pixel = breathe_pixel,
};

/* --------------------------------------------------------------------- wave */

static bool wave_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                       uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_wave_cfg *cfg = layer->config;
    const uint16_t wavelength = cfg->wavelength ? cfg->wavelength : ctx->virtual_length;

    if (wavelength == 0) {
        return false;
    }

    const uint32_t spatial = ((uint32_t)vfx_virtual_idx(ctx, strip_i) * 256U) / wavelength;
    const uint8_t level = vfx_sin8((uint8_t)(spatial + phase_turns(ctx, cfg->period_ms)));

    /* depth 0 leaves the colour flat; 255 takes the trough to black. */
    const uint8_t scale = (uint8_t)(255U - ((uint32_t)cfg->depth * (255U - level)) / 255U);

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    hsb.b = (uint8_t)((uint16_t)hsb.b * scale / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_wave_api = {
    .pixel = wave_pixel,
};

/* ------------------------------------------------------------------ twinkle */

static bool twinkle_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                          uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_twinkle_cfg *cfg = layer->config;

    if (cfg->period_ms == 0 || cfg->density == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    const uint32_t t = (uint32_t)ctx->time_ms * ctx->speed / 3U;

    /* Give each pixel its own offset into the cycle, so they do not all
     * twinkle in lockstep, then re-roll which pixels participate each cycle.
     */
    const uint32_t offset = vfx_hash32(vidx) % cfg->period_ms;
    const uint32_t local = (t + offset) % cfg->period_ms;
    const uint32_t cycle = (t + offset) / cfg->period_ms;

    const uint32_t roll = vfx_hash32(vidx ^ (cycle * 0x9E3779B9U));

    if ((roll & 0xFFU) >= cfg->density) {
        return false; /* dark this cycle: leave whatever is underneath */
    }

    const uint8_t level = vfx_tri8((uint8_t)((local * 256U) / cfg->period_ms));

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    hsb.b = (uint8_t)((uint16_t)hsb.b * level / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_twinkle_api = {
    .pixel = twinkle_pixel,
};

/* ------------------------------------------------------------------- plasma */

static bool plasma_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                         uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_plasma_cfg *cfg = layer->config;
    const uint16_t scale = cfg->scale ? cfg->scale : ctx->virtual_length;

    if (scale == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    const uint32_t turns = phase_turns(ctx, cfg->period_ms);

    /* Two waves at different rates and directions. Their sum never repeats on
     * a short cycle, which is what stops it reading as an obvious sine.
     */
    const uint8_t a = vfx_sin8((uint8_t)(((uint32_t)vidx * 256U) / scale + turns));
    const uint8_t b = vfx_sin8((uint8_t)(((uint32_t)vidx * 384U) / scale - turns * 2U));
    const uint8_t mixed = (uint8_t)(((uint16_t)a + b) / 2U);

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);

    /* Swing the hue around the configured colour rather than sweeping the
     * whole circle, so a plasma keeps the palette it was given.
     */
    const int16_t swing = (int16_t)(((int32_t)mixed - 128) * cfg->hue_spread / 128);

    hsb.h = vfx_hue_add(hsb.h, (int16_t)(swing + ctx->hue_shift));
    hsb.b = (uint8_t)((uint16_t)hsb.b * (uint8_t)(128U + mixed / 2U) / 255U);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_plasma_api = {
    .pixel = plasma_pixel,
};
