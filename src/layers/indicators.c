/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Indicator generators: active layer, battery, BLE profile.
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

/* ---------------------------------------------------------------- flags */

/* Lock LEDs and held modifiers are the same shape of thing: a bit set
 * somewhere, and a colour to show while any of the bits you care about is on.
 * So one generator reads either, and which one is a property.
 *
 * Locks are worth a word. A keyboard cannot know whether caps lock is on --
 * that is the host's state, not the keyboard's, and pressing the key is a
 * request rather than a toggle. It only finds out because the host sends an
 * LED report back, which is why this needs CONFIG_ZMK_HID_INDICATORS and why
 * there is still no caps *word* indicator: that one ZMK does not expose.
 */

static bool flag_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                       uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);
    VFX_UNUSED(strip_i);

    const struct vfx_flag_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    const uint8_t bits = cfg->source == VFX_FLAG_MODIFIERS ? status->modifiers : status->locks;

    if ((bits & cfg->mask) == 0) {
        return false; /* off: leave whatever is underneath showing */
    }

    struct vfx_hsb hsb = vfx_hsb_unpack(cfg->color);

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_flag_api = {
    .pixel = flag_pixel,
};

/* ------------------------------------------------------------------- wpm */

static bool wpm_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                      uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(strip_i);

    const struct vfx_wpm_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    const uint16_t full = cfg->full ? cfg->full : 1U;
    const uint32_t wpm = status->wpm > full ? full : status->wpm;

    /* How fast you are typing, as 0-255 of the way to flat out. */
    const uint8_t t = (uint8_t)((wpm * 255U) / full);

    if (cfg->bar) {
        /* As a bar, the zone fills up as you speed up. */
        const uint16_t len = vfx_zone_clamped_len(layer->zone, ctx->num_pixels);
        const uint16_t filled = (uint16_t)((uint32_t)len * t / 255U);

        if (zone_i >= filled) {
            return false;
        }
    }

    struct vfx_hsb hsb =
        vfx_hsb_lerp(vfx_hsb_unpack(cfg->idle_color), vfx_hsb_unpack(cfg->fast_color), t);

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_wpm_api = {
    .pixel = wpm_pixel,
};

/* -------------------------------------------------- peripheral battery */

/* The other half's cell, which is the one you cannot check by looking at the
 * half in front of you. Only the central hears about it, so on a peripheral
 * this stays at "not reported" and draws unknown-color.
 */
static bool peripheral_battery_pixel(const struct vfx_layer *layer,
                                     const struct vfx_frame_ctx *ctx, uint16_t zone_i,
                                     uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(strip_i);

    const struct vfx_peripheral_battery_cfg *cfg = layer->config;
    const struct vfx_status *status = vfx_status_get();

    if (cfg->source >= VFX_MAX_PERIPHERALS) {
        return false;
    }

    const uint8_t level = status->peripheral_battery[cfg->source];
    struct vfx_hsb hsb;

    if (level == 0) {
        /* Never reported. Not the same as flat, and worth showing as its own
         * thing rather than as an alarming empty bar.
         */
        if (cfg->unknown_color == 0) {
            return false;
        }

        hsb = vfx_hsb_unpack(cfg->unknown_color);
    } else {
        const uint16_t len = vfx_zone_clamped_len(layer->zone, ctx->num_pixels);
        const uint16_t filled = (uint16_t)((uint32_t)len * level / 100U);

        if (zone_i >= filled) {
            if (cfg->empty_color == 0) {
                return false;
            }

            hsb = vfx_hsb_unpack(cfg->empty_color);
        } else if (level < cfg->warn_below) {
            hsb = vfx_hsb_unpack(cfg->low_color);
        } else {
            /* Colour the whole bar by how full it is, so a glance at the hue
             * tells you as much as counting pixels does.
             */
            hsb = vfx_hsb_lerp(vfx_hsb_unpack(cfg->low_color), vfx_hsb_unpack(cfg->high_color),
                               (uint8_t)((uint32_t)level * 255U / 100U));
        }
    }

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

const struct vfx_layer_api vfx_layer_peripheral_battery_api = {
    .pixel = peripheral_battery_pixel,
};
