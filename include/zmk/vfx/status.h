/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Keyboard state that indicator layers render.
 *
 * Generators must stay free of Zephyr and ZMK includes so they can be built
 * for the simulator, so they never call zmk_keymap_highest_layer_active() or
 * the battery API directly. engine.c subscribes to the events and pushes
 * values in here; the simulator sets the same fields from the page.
 */
struct vfx_status {
    uint8_t active_layer;
    uint8_t battery_level; /* percent, 0-100 */
    uint8_t ble_profile;   /* 0 based index of the selected profile */
    bool ble_connected;
    bool usb_output;
};

const struct vfx_status *vfx_status_get(void);
struct vfx_status *vfx_status_mutable(void);
