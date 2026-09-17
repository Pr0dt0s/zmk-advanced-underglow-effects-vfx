/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Comet: a bright head running round the board with a fading tail.
 *
 * Matrix rain is the same idea with drops that start and end; this one never
 * stops, and wraps, so it suits the axes that are a loop -- around the board,
 * or spiralling -- as well as the ones that are a line.
 *
 * Its position is a function of the clock, not something accumulated, so the
 * two halves of a split stay in step without exchanging anything and a
 * dropped frame does not shift the comet.
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

static bool comet_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                        uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_comet_cfg *cfg = layer->config;

    if (cfg->period_ms == 0 || cfg->tail == 0 || cfg->count == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    const uint32_t span = vfx_axis_span(ctx, cfg->axis);

    if (span == 0) {
        return false;
    }

    const uint32_t pos = vfx_axis_pos(ctx, vidx, cfg->axis) % span;

    /* Where the leading comet is, as a fraction of a lap. 64 bit because
     * time_ms runs to 2^32 and multiplying it by the span overflows well
     * inside a day of uptime.
     */
    const uint64_t t = (uint64_t)ctx->time_ms * ctx->speed / 3U;
    const uint32_t lead = (uint32_t)((t % cfg->period_ms) * span / cfg->period_ms);

    uint16_t best = 0;

    for (uint8_t i = 0; i < cfg->count; i++) {
        /* The others trail evenly round the lap behind it. */
        const uint32_t head = (lead + (uint32_t)i * span / cfg->count) % span;
        const uint32_t behind = (head + span - pos) % span;

        if (behind > cfg->tail) {
            continue;
        }

        const uint16_t level = (uint16_t)(255U - (behind * 255U) / cfg->tail);

        if (level > best) {
            best = level;
        }
    }

    if (best == 0) {
        return false;
    }

    struct vfx_hsb hsb;

    /* The head keeps its own colour at full strength; behind it the trail
     * fades. Without a head colour the whole comet is one colour fading out.
     */
    if (cfg->head_color != 0 && best > 240) {
        hsb = vfx_hsb_unpack(cfg->head_color);
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

const struct vfx_layer_api vfx_layer_comet_api = {
    .pixel = comet_pixel,
};
