/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/color.h>
#include <zmk/vfx/zone.h>

/* Zephyr's ARG_UNUSED lives in sys/util.h, which the simulator build does not
 * have in scope. Generators are shared between both, so they use this.
 */
#define VFX_UNUSED(x) ((void)(x))

/* Everything the generators are allowed to know about the current frame.
 *
 * Coordinates: a split keyboard renders one virtual strip that spans both
 * halves. Each half knows its own strip_offset into that space, so a gradient
 * flows continuously across the seam without either half knowing the other
 * exists. virtual_length is the whole board; num_pixels is just this half.
 */
struct vfx_frame_ctx {
    uint32_t time_ms; /* engine timebase; in synced mode this tracks central */
    uint16_t virtual_length;
    uint16_t strip_offset;
    uint16_t num_pixels;
    uint8_t speed;      /* 1-5 */
    uint8_t brightness; /* 0-255, already clamped to CONFIG_ZMK_VFX_BRT_MAX */
    int16_t hue_shift;  /* global hue rotation in degrees */

    /* Virtual pixel nearest each key position, from the engine's key-pixels
     * property. NULL falls back to spreading key positions evenly over the
     * strip, which is wrong in detail but keeps reactive effects usable
     * without anyone having to measure their board.
     */
    const uint8_t *key_pixels;
    uint16_t num_keys;
};

/* Virtual (whole board) index of a local strip index. */
static inline uint16_t vfx_virtual_idx(const struct vfx_frame_ctx *ctx, uint16_t strip_idx) {
    return (uint16_t)(ctx->strip_offset + strip_idx);
}

/* Virtual pixel a key sits nearest. Reactive layers work in virtual space so
 * a ripple started by the other half lands in the right place once the two
 * are synchronised.
 */
static inline uint16_t vfx_key_pixel(const struct vfx_frame_ctx *ctx, uint32_t position) {
    if (ctx->key_pixels && position < ctx->num_keys) {
        return ctx->key_pixels[position];
    }

    if (ctx->num_keys > 0) {
        return (uint16_t)(position * ctx->virtual_length / ctx->num_keys);
    }

    return (uint16_t)(position % (ctx->virtual_length ? ctx->virtual_length : 1));
}

struct vfx_layer;

struct vfx_layer_api {
    /* Optional. Runs once per frame before any pixel is asked for; advance
     * animation state here rather than in pixel(), which is called num_pixels
     * times.
     */
    void (*frame)(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx);

    /* Required. Produce the color for one pixel of the layer's zone. Return
     * false to leave the composited value untouched (cheaper and more correct
     * than returning black, which would still blend).
     *
     * zone_i is the index within the zone; strip_i is the physical pixel.
     */
    bool (*pixel)(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx, uint16_t zone_i,
                  uint16_t strip_i, struct vfx_rgb *out);

    /* Optional. Reactive layers record key activity here. The timestamp comes
     * from the engine's timebase rather than being read inside the generator,
     * both to keep Zephyr out of these files and so a relayed event from the
     * other half can be stamped with when it actually happened.
     */
    void (*key_event)(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                      uint32_t position, bool pressed, uint32_t time_ms);

    /* Optional. Return true if this layer can still change what it renders.
     * When every layer in a scene reports false the engine stops ticking until
     * something external happens. Absent means "always animating".
     */
    bool (*is_animating)(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx);
};

struct vfx_layer {
    const struct vfx_layer_api *api;
    const struct vfx_zone *zone;
    const void *config; /* generator specific, const: lives in flash */
    void *state;        /* generator specific, mutable */
    uint8_t blend;      /* VFX_BLEND_* */
    uint8_t opacity;    /* 0-255 */
};

struct vfx_scene {
    const char *name;
    const struct vfx_layer *layers;
    uint8_t num_layers;
};
