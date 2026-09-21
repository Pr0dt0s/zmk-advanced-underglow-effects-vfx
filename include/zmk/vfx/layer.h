/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/color.h>
#include <zmk/vfx/math.h>
#include <zmk/vfx/zone.h>

/* Zephyr's ARG_UNUSED lives in sys/util.h, which the simulator build does not
 * have in scope. Generators are shared between both, so they use this.
 */
#define VFX_UNUSED(x) ((void)(x))

/* The box the pixels occupy. Anything with a direction (falling, sweeping)
 * needs it to know where the board starts and ends.
 */
struct vfx_board_box {
    int16_t min_x, min_y, max_x, max_y;
    bool known; /* false when there is no position map to measure */
};

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

    /* Where each virtual pixel physically sits, as x,y pairs. Without it the
     * engine knows only a pixel's position along the wire, so anything that
     * radiates spreads to whatever happens to be near on the strip rather
     * than near on the board. See vfx_pixel_distance().
     */
    const int16_t *pixel_xy;
    uint16_t num_positions;

    /* Bounds of pixel_xy, measured once when the map is set. Effects that run
     * across the board need them per pixel, and walking every position to
     * find them again each time would be the most expensive thing in the
     * frame. Left unset (known = false) they are measured on demand, which is
     * what keeps a hand-built context in a test correct.
     */
    struct vfx_board_box board;
};

/* Virtual (whole board) index of a local strip index. */
static inline uint16_t vfx_virtual_idx(const struct vfx_frame_ctx *ctx, uint16_t strip_idx) {
    return (uint16_t)(ctx->strip_offset + strip_idx);
}

/* Distance between two pixels, in whatever units pixel-positions is given in.
 *
 * With a position map this is the real distance across the board, so a ripple
 * spreads outward in a ring. Without one it falls back to distance along the
 * strip, which is all the engine can know: on a serpentine strip that makes a
 * "ring" land on whichever LEDs happen to be adjacent on the wire, which is
 * usually nowhere near each other.
 */
static inline uint16_t vfx_pixel_distance(const struct vfx_frame_ctx *ctx, uint16_t a,
                                          uint16_t b) {
    if (!ctx->pixel_xy || a >= ctx->num_positions || b >= ctx->num_positions) {
        return (uint16_t)(a > b ? a - b : b - a);
    }

    const int32_t dx = (int32_t)ctx->pixel_xy[a * 2] - ctx->pixel_xy[b * 2];
    const int32_t dy = (int32_t)ctx->pixel_xy[a * 2 + 1] - ctx->pixel_xy[b * 2 + 1];

    return (uint16_t)vfx_isqrt((uint32_t)(dx * dx + dy * dy));
}

/* Furthest two pixels can be apart, for effects that need to know when they
 * have covered the whole board. With positions this is the diagonal; without
 * it, the strip length.
 */
static inline struct vfx_board_box vfx_board_bounds(const struct vfx_frame_ctx *ctx) {
    struct vfx_board_box box = {0, 0, 0, 0, false};

    if (ctx->board.known) {
        return ctx->board;
    }

    if (!ctx->pixel_xy || ctx->num_positions == 0) {
        return box;
    }

    box.known = true;
    box.min_x = box.max_x = ctx->pixel_xy[0];
    box.min_y = box.max_y = ctx->pixel_xy[1];

    for (uint16_t i = 1; i < ctx->num_positions; i++) {
        const int16_t x = ctx->pixel_xy[i * 2];
        const int16_t y = ctx->pixel_xy[i * 2 + 1];

        box.min_x = x < box.min_x ? x : box.min_x;
        box.max_x = x > box.max_x ? x : box.max_x;
        box.min_y = y < box.min_y ? y : box.min_y;
        box.max_y = y > box.max_y ? y : box.max_y;
    }

    return box;
}

static inline uint16_t vfx_board_extent(const struct vfx_frame_ctx *ctx) {
    const struct vfx_board_box box = vfx_board_bounds(ctx);

    if (!box.known) {
        return ctx->virtual_length;
    }

    const int32_t w = box.max_x - box.min_x;
    const int32_t h = box.max_y - box.min_y;

    return (uint16_t)vfx_isqrt((uint32_t)(w * w + h * h));
}

/* Which way an effect runs across the board. The VFX_AXIS_* values come from
 * <dt-bindings/zmk/vfx.h>, so devicetree and C name them the same way, as with
 * the blend modes.
 *
 * Without a position map there is no board to run across, so every axis falls
 * back to the strip. That is not a fudge: strip order is genuinely all the
 * engine knows without a map, and a scene written for one board then still
 * animates on another rather than going dark.
 */

/* One full cycle of an axis, in that axis's own units. A generator that
 * leaves its span or wavelength at 0 gets this, so "unset" means "one cycle
 * across the board" whichever way the effect is pointing.
 */
static inline uint32_t vfx_axis_span(const struct vfx_frame_ctx *ctx, uint8_t axis) {
    const struct vfx_board_box box = vfx_board_bounds(ctx);

    if (!box.known || axis == VFX_AXIS_STRIP) {
        return ctx->virtual_length ? ctx->virtual_length : 1;
    }

    switch (axis) {
    case VFX_AXIS_X:
        return (uint32_t)(box.max_x - box.min_x) + 1U;
    case VFX_AXIS_Y:
        return (uint32_t)(box.max_y - box.min_y) + 1U;
    case VFX_AXIS_RADIAL: {
        /* Half the diagonal: the furthest any pixel can be from the middle. */
        const int32_t w = (box.max_x - box.min_x) / 2;
        const int32_t h = (box.max_y - box.min_y) / 2;

        return vfx_isqrt((uint32_t)(w * w + h * h)) + 1U;
    }
    default:
        return 256U; /* angle and spiral are measured in 256ths of a turn */
    }
}

/* Where a pixel sits along that axis. */
static inline uint32_t vfx_axis_pos(const struct vfx_frame_ctx *ctx, uint16_t vidx,
                                    uint8_t axis) {
    const struct vfx_board_box box = vfx_board_bounds(ctx);

    if (!box.known || !ctx->pixel_xy || axis == VFX_AXIS_STRIP ||
        vidx >= ctx->num_positions) {
        return vidx;
    }

    const int32_t x = ctx->pixel_xy[vidx * 2];
    const int32_t y = ctx->pixel_xy[vidx * 2 + 1];

    switch (axis) {
    case VFX_AXIS_X:
        return (uint32_t)(x - box.min_x);
    case VFX_AXIS_Y:
        return (uint32_t)(y - box.min_y);
    default:
        break;
    }

    const int32_t dx = x - (box.min_x + box.max_x) / 2;
    const int32_t dy = y - (box.min_y + box.max_y) / 2;
    const uint32_t radius = vfx_isqrt((uint32_t)(dx * dx + dy * dy));

    if (axis == VFX_AXIS_RADIAL) {
        return radius;
    }

    const uint32_t angle = vfx_atan2_8(dy, dx);

    if (axis == VFX_AXIS_ANGLE) {
        return angle;
    }

    /* Spiral: one extra turn per radius, which is what bends the pinwheel's
     * arms into a spiral rather than leaving them straight.
     */
    return (angle + radius * 256U / vfx_axis_span(ctx, VFX_AXIS_RADIAL)) & 0xFFU;
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
    uint8_t opacity;    /* 0-255, and the value a driven layer reaches at full */

    /* Optionally let something the keyboard knows drive how strongly this
     * layer shows: typing speed, charge, whether anyone is at the keyboard.
     *
     * It lives here rather than in any one generator because it is the same
     * question for all of them, and putting it in the compositor means every
     * generator gains it without knowing that it did.
     */
    uint8_t opacity_src;  /* VFX_SRC_*; VFX_SRC_NONE leaves opacity alone */
    uint8_t opacity_min;  /* opacity when the signal reads zero */
    uint8_t opacity_full; /* signal value that reaches full opacity */

    /* Tuning slot this layer answers to, or 0 for none. See zmk/vfx/tuning.h:
     * everything else here is fixed in flash by devicetree, and this is the
     * one handle a running keyboard has on it.
     */
    uint8_t tune_id;
};

struct vfx_scene {
    const char *name;
    const struct vfx_layer *layers;
    uint8_t num_layers;
};
