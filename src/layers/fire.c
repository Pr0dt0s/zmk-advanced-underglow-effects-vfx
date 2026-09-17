/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Fire: a hot bed with flames licking up from it.
 *
 * The usual way to do this is a heat buffer that cools and diffuses each
 * frame, which is both state and a dependency on every previous frame: a
 * dropped frame changes the fire, and the two halves of a split drift apart
 * within seconds of each other.
 *
 * So this is done the way the rest of the ambient family is done, as a pure
 * function of position and time. Each flame cell gets a height that wanders
 * from a hash of the cell and the current instant, interpolated between
 * instants so it drifts rather than steps. A pixel is then coloured by how
 * far up its cell's flame it sits. Same fire on both halves, no state, and it
 * survives a dropped frame.
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

/* Height of the flame in one cell at one instant, 0-255. */
static uint8_t cell_height(uint32_t cell, uint32_t instant) {
    return (uint8_t)(vfx_hash32(cell * 0x9E3779B9U ^ (instant * 0x85EBCA6BU)) & 0xFFU);
}

static bool fire_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                       uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_fire_cfg *cfg = layer->config;

    if (cfg->period_ms == 0 || cfg->height == 0) {
        return false;
    }

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);

    /* Up the board is the effect's axis; across it is the other one, which is
     * what divides the fire into flames rather than making it one sheet.
     */
    const uint8_t up_axis = cfg->axis;
    const uint8_t across_axis = up_axis == VFX_AXIS_Y ? VFX_AXIS_X : VFX_AXIS_Y;

    const uint32_t up_span = vfx_axis_span(ctx, up_axis);
    const uint32_t up = vfx_axis_pos(ctx, vidx, up_axis);
    const uint32_t across = vfx_axis_pos(ctx, vidx, across_axis);

    const uint32_t cell = cfg->cell ? cfg->cell : 1U;
    const uint32_t column = across / cell;

    /* Fire burns upward, so a pixel's fuel is how far it is from the top of
     * the axis: the bottom row is always lit, the top only when a flame
     * reaches it.
     */
    const uint32_t from_bottom = up_span > up ? up_span - up : 0;

    const uint32_t t = (uint32_t)ctx->time_ms * ctx->speed / 3U;
    const uint32_t instant = t / cfg->period_ms;
    const uint8_t blend = (uint8_t)(((t % cfg->period_ms) * 255U) / cfg->period_ms);

    /* Interpolate between this instant's flame and the next, so the fire
     * drifts instead of stepping every period.
     */
    const uint8_t a = cell_height(column, instant);
    const uint8_t b = cell_height(column, instant + 1U);
    const uint8_t wander = (uint8_t)(a + (((int16_t)b - a) * blend) / 255);

    /* Flicker mixes that wandering height with a steady one, so a low flicker
     * gives a calm fire rather than a still one.
     */
    const uint32_t steady = 255U - cfg->flicker;
    const uint32_t reach = ((steady * 255U + (uint32_t)cfg->flicker * wander) / 255U) *
                           cfg->height / 255U * up_span / 255U;

    if (reach == 0 || from_bottom > reach) {
        return false; /* above the flame: nothing to draw */
    }

    /* Hot at the base of the flame, cooling toward its tip. */
    const uint8_t heat = (uint8_t)(255U - (from_bottom * 255U) / reach);

    struct vfx_hsb hsb =
        vfx_hsb_lerp(vfx_hsb_unpack(cfg->base_color), vfx_hsb_unpack(cfg->tip_color), heat);

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_fire_api = {
    .pixel = fire_pixel,
};
