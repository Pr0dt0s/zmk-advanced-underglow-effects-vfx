/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Indicator generators: active layer, battery, BLE profile, caps word.
 *
 * These read keyboard state through vfx_status_get() rather than calling ZMK
 * directly, which keeps them buildable for the simulator and means the page
 * can drive them by setting the same fields. engine.c is what subscribes to
 * the events and fills the struct in.
 *
 * They index by position within the zone, not by strip index, so the same
 * scene works whether its indicator zone is six pixels along the top edge or
 * a scattered handful.
 */

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>
#include <zmk/vfx/status.h>

/* ------------------------------------------------------------- layer state */

static bool layer_state_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                              uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_layer_state_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    /* A layer with no colour listed draws nothing, which is how the base
     * layer usually stays out of the way.
     */
    if (cfg->num_colors == 0 || status->active_layer >= cfg->num_colors) {
        return false;
    }

    const uint32_t packed = cfg->colors[status->active_layer];

    if (VFX_HSB_B(packed) == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(packed);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_layer_state_api = {
    .pixel = layer_state_pixel,
};

/* ----------------------------------------------------------------- battery */

static bool battery_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                          uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(strip_i);

    const struct vfx_battery_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    const uint16_t len = layer->zone->len ? layer->zone->len : 1;

    /* Fill a fraction of the zone proportional to charge. Rounding up means a
     * non-empty battery always shows at least one pixel.
     */
    const uint16_t filled = (uint16_t)(((uint32_t)status->battery_level * len + 99U) / 100U);

    uint32_t packed;

    if (zone_i < filled) {
        packed = status->battery_level <= cfg->warn_below ? cfg->low_color : cfg->high_color;
    } else {
        packed = cfg->empty_color;
    }

    if (VFX_HSB_B(packed) == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(packed);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_battery_api = {
    .pixel = battery_pixel,
};

/* ------------------------------------------------------------- ble profile */

static bool ble_profile_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                              uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(strip_i);

    const struct vfx_ble_profile_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    /* One pixel per profile: the selected one lights, the rest stay dark so
     * the zone reads as a row of slots.
     */
    if (zone_i != status->ble_profile) {
        return false;
    }

    uint32_t packed;

    if (status->usb_output) {
        packed = cfg->usb_color;
    } else {
        packed = status->ble_connected ? cfg->connected_color : cfg->disconnected_color;
    }

    if (VFX_HSB_B(packed) == 0) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(packed);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_ble_profile_api = {
    .pixel = ble_profile_pixel,
};

/* --------------------------------------------------------------- caps word */

static bool caps_word_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                            uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_caps_word_cfg *cfg = layer->config;

    if (!vfx_status_get()->caps_word) {
        return false;
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);
    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);

    if (cfg->period_ms) {
        const uint8_t turn = (uint8_t)(((uint32_t)ctx->time_ms * 256U) / cfg->period_ms);

        hsb.b = (uint8_t)((uint16_t)hsb.b * vfx_sin8(turn) / 255U);
    }

    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool caps_word_is_animating(const struct vfx_layer *layer,
                                   const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_caps_word_cfg *cfg = layer->config;

    /* Only moving while caps word is actually held and a pulse is configured,
     * so this layer does not keep an otherwise static scene awake.
     */
    return cfg->period_ms != 0 && vfx_status_get()->caps_word;
}

const struct vfx_layer_api vfx_layer_caps_word_api = {
    .pixel = caps_word_pixel,
    .is_animating = caps_word_is_animating,
};
