/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/sync.h>

int32_t vfx_sync_step(int32_t current_offset, int32_t desired_offset) {
    const int32_t delta = desired_offset - current_offset;

    if (delta == 0) {
        return current_offset;
    }

    const int32_t magnitude = delta < 0 ? -delta : delta;

    /* First beacon after connecting, or a peripheral that rebooted: there is
     * no continuity to preserve, so take the whole correction now.
     */
    if (magnitude > VFX_SYNC_JUMP_THRESHOLD_MS) {
        return desired_offset;
    }

    /* Otherwise ease toward it. Capping the per-beacon step keeps the
     * correction under one frame at 50 fps, so it is invisible rather than a
     * jolt in whatever is on screen.
     */
    if (magnitude <= VFX_SYNC_MAX_SLEW_MS) {
        return desired_offset;
    }

    return current_offset + (delta > 0 ? VFX_SYNC_MAX_SLEW_MS : -VFX_SYNC_MAX_SLEW_MS);
}
