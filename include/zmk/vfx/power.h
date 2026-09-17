/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/color.h>

/* Automatic LED rail gating.
 *
 * WS2812s draw roughly 0.7-1 mA each even while displaying black, so a 36
 * pixel strip sits at around 30 mA whether or not anything is lit. That is
 * three orders of magnitude more than an idle nRF52840. Cutting the rail
 * whenever a frame comes out entirely black is the single biggest saving
 * available to an always-on underglow.
 *
 * The decision is a pure state machine so it can be unit tested and shown in
 * the simulator; the Zephyr side of it lives in power.c.
 */

enum vfx_power_state {
    VFX_POWER_LIT,      /* rail on, frames going out */
    VFX_POWER_GATED,    /* rail off, still rendering to spot the first lit frame */
    VFX_POWER_SETTLING, /* rail just came back, waiting for it to stabilise */
};

enum vfx_power_action {
    VFX_POWER_TRANSMIT, /* push this frame to the strip */
    VFX_POWER_SKIP,     /* rail is down or settling; do not touch the bus */
    VFX_POWER_GATE_OFF, /* push one last black frame, then cut the rail */
    VFX_POWER_WAKE,     /* bring the rail up, but do not transmit yet */
};

struct vfx_power_policy {
    uint16_t blackout_delay_ms; /* consecutive black time before gating */
    uint16_t settle_ms;         /* rail stabilisation time before the first frame */
};

struct vfx_power_ctl {
    enum vfx_power_state state;
    uint32_t black_ms;
    uint32_t settle_left_ms;
};

/* Advance the gate by one frame. `lit` is the any_lit flag the compositor
 * already produced, `elapsed_ms` the time since the previous call.
 */
enum vfx_power_action vfx_power_step(struct vfx_power_ctl *ctl,
                                     const struct vfx_power_policy *policy, bool lit,
                                     uint16_t elapsed_ms);

/* True when the gate has nothing pending: either already off, or on with no
 * blackout countdown running. The engine parks its timer when a scene stops
 * animating, and must not do that mid-countdown or a static black scene would
 * never reach the point of gating the rail at all.
 */
bool vfx_power_is_stable(const struct vfx_power_ctl *ctl);

/* Put the gate straight into its off state, for when the rail was cut outside
 * the normal blackout countdown: switching the underglow off stops the tick,
 * so the countdown that usually gets us here never runs.
 */
void vfx_power_force_gated(struct vfx_power_ctl *ctl);

/* Anything that changes the scene out from under the gate resets it, so a
 * newly selected scene is never judged by the previous one's darkness.
 */
void vfx_power_reset(struct vfx_power_ctl *ctl);

/* Rough current draw of the strip in microamps, for the simulator's readout
 * and for anyone wanting a sanity check against a multimeter. Counts the
 * per-chip quiescent draw plus per-channel drive current.
 */
uint32_t vfx_estimate_ua(const struct vfx_rgb *pixels, uint16_t count, bool powered);

/* Hardware side, implemented in power.c against Zephyr. */
void vfx_power_rail_enable(void);
void vfx_power_rail_disable(void);
