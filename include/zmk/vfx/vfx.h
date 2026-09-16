/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Runtime control surface. The &vfx behavior and the rgb_ug compatibility
 * shim both drive the engine through these.
 */

int zmk_vfx_on(void);
int zmk_vfx_off(void);
int zmk_vfx_toggle(void);
bool zmk_vfx_is_on(void);

int zmk_vfx_select_scene(uint8_t index);
int zmk_vfx_cycle_scene(int direction);
uint8_t zmk_vfx_current_scene(void);
const char *zmk_vfx_scene_name(uint8_t index);

int zmk_vfx_change_brightness(int direction);
int zmk_vfx_change_speed(int direction);
int zmk_vfx_change_hue(int direction);

uint8_t zmk_vfx_get_brightness(void);
uint8_t zmk_vfx_get_speed(void);
int16_t zmk_vfx_get_hue_shift(void);

/* Shift the engine timebase, used by the synced split mode to follow central.
 * Applied as a slew rather than a jump so corrections are not visible.
 */
void zmk_vfx_set_time_offset(int32_t offset_ms);
int32_t zmk_vfx_get_time_offset(void);

/* Ask for a frame now, outside the normal cadence, after something changed
 * that the scene itself cannot observe.
 */
void zmk_vfx_request_frame(void);
