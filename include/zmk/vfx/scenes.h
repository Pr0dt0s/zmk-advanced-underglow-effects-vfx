/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#ifndef VFX_SIM
#include <zephyr/devicetree.h>
#endif

#include <zmk/vfx/layer.h>

#define VFX_ENGINE_NODE DT_INST(0, zmk_vfx_engine)

/* Exactly how many channels this board declared -- the same count
 * vfx_channel_count() reports at runtime, computed here instead so it can
 * also size state.chan[] in engine.c and the per-channel pools in
 * runtime_scene.c, neither of which can size an array off a function
 * result. A board with none declared gets the fallback single channel
 * scenes.c itself falls back to (the whole strip, every scene).
 *
 * Sizing this to the board rather than to some fixed ceiling means a
 * two-channel board no longer pays RAM for four, and a board that wants
 * more than four is no longer capped there either. The one cost: the
 * settings blobs this sizes ("vfx/state" whole, "vfx/rt" per channel)
 * change layout whenever a board's channel count changes, same as they
 * already do whenever any other field is added or removed -- the existing
 * size check on load already treats that as "different build" and falls
 * back to defaults rather than misreading the bytes, so this is a rebuild
 * losing saved brightness/speed/hue and any built runtime scenes once, not
 * a hazard.
 *
 * The simulator and the Zephyr-free test suite have no devicetree to ask,
 * so VFX_SIM keeps this at a fixed 4 there -- enough to exercise
 * multi-channel behaviour without a real board attached.
 */
#ifdef VFX_SIM
#define VFX_MAX_CHANNELS 4
#elif DT_HAS_COMPAT_STATUS_OKAY(zmk_vfx_channel)
#define VFX_MAX_CHANNELS DT_NUM_INST_STATUS_OKAY(zmk_vfx_channel)
#else
#define VFX_MAX_CHANNELS 1
#endif

uint8_t vfx_scene_count(void);
const struct vfx_scene *vfx_scene_get(uint8_t index);
uint8_t vfx_scene_default_index(void);

/* A slice of the strip that carries its own scene list and, at runtime, its
 * own scene, brightness, speed and hue. A board that declares none is treated
 * as one channel covering everything, which is the behaviour that predates
 * channels existing.
 */
struct vfx_channel {
    /* Local indices into this half's strip, clamped to the chain like a zone. */
    uint16_t start;
    uint16_t len;

    const struct vfx_scene *const *scenes;
    uint8_t num_scenes;

    /* Resolved to a list position by vfx_channel_default_index(). */
    const struct vfx_scene *default_scene;
};

uint8_t vfx_channel_count(void);
const struct vfx_channel *vfx_channel_get(uint8_t index);

/* Position within the channel's own list, not the board-wide one. */
uint8_t vfx_channel_default_index(uint8_t index);

/* Resolve every zone written as key positions into pixel indices. Needs the
 * key map and this half's strip offset, so it runs once at startup rather
 * than at build time.
 */
void vfx_resolve_key_zones(const struct vfx_frame_ctx *ctx);

/* Position in the channel's own list of the scene to show while a given keymap
 * layer is the highest active one, or -1 when that layer has none, or when
 * this channel does not carry it and should be left alone.
 */
int16_t vfx_layer_scene_index(uint8_t layer, uint8_t ch);
