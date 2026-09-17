/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdint.h>

#include <zmk/vfx/layer.h>

#define VFX_ENGINE_NODE DT_INST(0, zmk_vfx_engine)

uint8_t vfx_scene_count(void);
const struct vfx_scene *vfx_scene_get(uint8_t index);
uint8_t vfx_scene_default_index(void);

/* Resolve every zone written as key positions into pixel indices. Needs the
 * key map and this half's strip offset, so it runs once at startup rather
 * than at build time.
 */
void vfx_resolve_key_zones(const struct vfx_frame_ctx *ctx);

/* Scene to show while a given keymap layer is the highest active one, or -1
 * when that layer has none and whatever is showing should stay.
 */
int16_t vfx_layer_scene_index(uint8_t layer);
