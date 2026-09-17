/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* VFX_LOCK_* and VFX_MOD_* live here, so devicetree and C name them alike. */
#include <dt-bindings/zmk/vfx.h>

/* Keyboard state that indicator layers render.
 *
 * Generators must stay free of Zephyr and ZMK includes so they can be built
 * for the simulator, so they never call zmk_keymap_highest_layer_active() or
 * the battery API directly. engine.c subscribes to the events and pushes
 * values in here; the simulator sets the same fields from the page.
 */

/* How many peripherals a central can report the battery of. Two covers a
 * split; more is possible in ZMK but nothing here needs to grow for it.
 */
#define VFX_MAX_PERIPHERALS 2

struct vfx_status {
    uint8_t active_layer;
    uint8_t battery_level; /* percent, 0-100 */
    uint8_t ble_profile;   /* 0 based index of the selected profile */
    bool ble_connected;
    bool usb_output;

    /* Lock LEDs the host has asked for: caps, num, scroll. A keyboard cannot
     * know these by itself -- they are the host's state, and it only finds
     * out because the host sends an LED report.
     */
    uint8_t locks;

    /* Modifiers currently held. */
    uint8_t modifiers;

    /* Words per minute, from ZMK's own estimate. */
    uint8_t wpm;

    /* The other half's battery, on a split central. 0 means not reported
     * yet, which is different from a flat cell and is drawn as such.
     */
    uint8_t peripheral_battery[VFX_MAX_PERIPHERALS];
};

const struct vfx_status *vfx_status_get(void);
struct vfx_status *vfx_status_mutable(void);
