/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/power.h>

/* Datasheet-ish figures for a WS2812B. Quiescent is what the controller draws
 * with every channel off but the rail up, which is the whole reason this file
 * exists. Per-channel is the full-on current for one of the three dies.
 */
#define WS2812_QUIESCENT_UA 900U
#define WS2812_CHANNEL_FULL_UA 19000U

void vfx_power_reset(struct vfx_power_ctl *ctl) {
    ctl->black_ms = 0;
    ctl->settle_left_ms = 0;
    ctl->state = VFX_POWER_LIT;
}

void vfx_power_force_gated(struct vfx_power_ctl *ctl) {
    ctl->black_ms = 0;
    ctl->settle_left_ms = 0;
    ctl->state = VFX_POWER_GATED;
}

enum vfx_power_action vfx_power_step(struct vfx_power_ctl *ctl,
                                     const struct vfx_power_policy *policy, bool lit,
                                     uint16_t elapsed_ms) {
    switch (ctl->state) {
    case VFX_POWER_LIT:
        if (lit) {
            ctl->black_ms = 0;
            return VFX_POWER_TRANSMIT;
        }

        ctl->black_ms += elapsed_ms;

        /* The delay is what stops a scene that blinks, or one that dips
         * through black between cycles, from cycling the rail continuously.
         */
        if (ctl->black_ms >= policy->blackout_delay_ms) {
            ctl->state = VFX_POWER_GATED;
            ctl->black_ms = 0;
            return VFX_POWER_GATE_OFF;
        }

        return VFX_POWER_TRANSMIT;

    case VFX_POWER_GATED:
        if (!lit) {
            /* Still rendering, just not transmitting: that is how the first
             * lit frame after a keypress gets noticed at all.
             */
            return VFX_POWER_SKIP;
        }

        ctl->state = VFX_POWER_SETTLING;
        ctl->settle_left_ms = policy->settle_ms;

        return VFX_POWER_WAKE;

    case VFX_POWER_SETTLING:
        if (ctl->settle_left_ms > elapsed_ms) {
            ctl->settle_left_ms -= elapsed_ms;
            return VFX_POWER_SKIP;
        }

        /* Driving data at an unpowered or half-powered strip produces visible
         * garbage on the first pixels, so the first frame waits for the rail.
         */
        ctl->settle_left_ms = 0;
        ctl->state = VFX_POWER_LIT;

        return VFX_POWER_TRANSMIT;
    }

    return VFX_POWER_TRANSMIT;
}

bool vfx_power_is_stable(const struct vfx_power_ctl *ctl) {
    switch (ctl->state) {
    case VFX_POWER_GATED:
        return true; /* rail already down, nothing further to do */
    case VFX_POWER_SETTLING:
        return false; /* owes a frame once the rail is up */
    case VFX_POWER_LIT:
        return ctl->black_ms == 0; /* non-zero means a countdown is running */
    }

    return true;
}

uint32_t vfx_estimate_ua(const struct vfx_rgb *pixels, uint16_t count, bool powered) {
    if (!powered) {
        return 0;
    }

    uint32_t ua = (uint32_t)count * WS2812_QUIESCENT_UA;

    for (uint16_t i = 0; i < count; i++) {
        const uint32_t sum = (uint32_t)pixels[i].r + pixels[i].g + pixels[i].b;

        ua += sum * WS2812_CHANNEL_FULL_UA / 255U;
    }

    return ua;
}
