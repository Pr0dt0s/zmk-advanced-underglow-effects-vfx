/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/status.h>

/* Single source of truth for what the indicator layers draw. Written from
 * event handlers in engine.c (or from the simulator), read from generators.
 * Every field is a byte or a bool, so a torn read shows one stale value for
 * one frame rather than anything worse; that is not worth a lock on the
 * render path.
 */
static struct vfx_status status = {
    .active_layer = 0,
    .battery_level = 100,
    .ble_profile = 0,
    .ble_connected = false,
    .usb_output = false,

    /* Assumed until told otherwise: a board that has just come up has someone
     * at it, and starting dark would look like a fault.
     */
    .active = true,
};

const struct vfx_status *vfx_status_get(void) { return &status; }

struct vfx_status *vfx_status_mutable(void) { return &status; }
