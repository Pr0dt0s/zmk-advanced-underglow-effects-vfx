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
