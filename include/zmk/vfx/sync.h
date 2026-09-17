/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

/* Timebase agreement between the halves of a split.
 *
 * Free-running mode needs none of this: every generator derives its output
 * from time and position alone, so two halves that happen to agree on the
 * clock render identical frames. What they do not have is a shared clock,
 * and two crystals drift apart over hours of uptime.
 *
 * Synced mode fixes that by having the central periodically tell the
 * peripheral what time it thinks it is. The correction is applied as a slew
 * rather than a jump, because stepping the timebase would visibly tear an
 * animation mid-frame.
 *
 * The decision is a pure function so it can be tested and driven from the
 * simulator's drift slider; the transport lives in split_sync.c.
 */

/* A correction larger than this is applied at once. Slewing it would take
 * minutes, and it only happens on the first beacon after a connection, where
 * there is nothing yet on screen worth protecting.
 */
#define VFX_SYNC_JUMP_THRESHOLD_MS 400

/* Largest correction folded in per beacon once running. Deliberately under
 * one frame at the default 50 fps: a step bigger than a frame skips animation
 * rather than easing it, which is exactly what slewing is meant to avoid.
 */
#define VFX_SYNC_MAX_SLEW_MS 15

/* Next timebase offset, given where we are and where the central says we
 * should be.
 */
int32_t vfx_sync_step(int32_t current_offset, int32_t desired_offset);

/* Offset that would put this half's clock at the central's. */
static inline int32_t vfx_sync_desired(uint32_t central_time_ms, uint32_t local_uptime_ms) {
    return (int32_t)(central_time_ms - local_uptime_ms);
}
