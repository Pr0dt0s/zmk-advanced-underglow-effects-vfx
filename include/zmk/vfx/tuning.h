/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Per-layer adjustments that can be changed while the keyboard is running.
 *
 * Everything a generator is configured with otherwise lives in flash, put
 * there by devicetree, and cannot be written to at all. Rather than move all
 * of that into RAM to make any of it adjustable, this holds the three knobs
 * worth reaching for live and lets them ride paths the compositor already
 * has: hue and speed by handing the layer its own frame context, and level by
 * folding into the opacity that layer was already going to be blended with.
 *
 * The consequence is that no generator knows this exists, and tuning costs
 * nothing per pixel.
 *
 * A layer opts in with a tune-id in devicetree. Layers sharing an id are
 * adjusted together, which is how a scene built from several layers can be
 * dimmed as one thing.
 */

struct vfx_tuning {
    int16_t hue;   /* degrees added to this layer's colours */
    uint8_t level; /* 0-255 scaling on the layer's opacity; 255 is as written */
    uint8_t speed; /* 1-5 override; 0 follows the channel */
};

/* Slot 0 is reserved for "not tuned", so a layer that never asked for a
 * tune-id reads as absent rather than as slot zero's settings.
 */
#define VFX_TUNE_SLOTS 8

/* NULL when the id is 0 or out of range, which the compositor takes to mean
 * the layer is used exactly as devicetree described it.
 *
 * vfx_tuning_reset_all() has to have run first. A level of 0 means the layer
 * is fully dimmed away, so zeroed storage is not a usable starting point and
 * the table is given its defaults at init rather than inferred here, which
 * would put a branch on the per-layer path forever to save one call at boot.
 */
const struct vfx_tuning *vfx_tuning_get(uint8_t id);

/* False when the id is unusable, so a caller relaying this from a host can
 * reject a bad request rather than silently writing nothing.
 */
bool vfx_tuning_set_hue(uint8_t id, int16_t degrees);
bool vfx_tuning_set_level(uint8_t id, uint8_t level);
bool vfx_tuning_set_speed(uint8_t id, uint8_t speed);

/* Back to what devicetree said, for one id or for all of them. */
void vfx_tuning_reset(uint8_t id);
void vfx_tuning_reset_all(void);

/* The whole table, for persisting it and loading it back. */
void *vfx_tuning_state(uint16_t *len);
