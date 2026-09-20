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
 *
 * Everything that a channel owns is addressed by channel id. Boards that
 * declare no zmk,vfx-channel have exactly one, id 0, covering the strip.
 */

/* Every channel at once, which is what an unqualified keymap binding means. */
#define ZMK_VFX_CH_ALL 0xFF

int zmk_vfx_on(uint8_t ch);
int zmk_vfx_off(uint8_t ch);
int zmk_vfx_toggle(uint8_t ch);
bool zmk_vfx_is_on(uint8_t ch);

int zmk_vfx_select_scene(uint8_t ch, uint8_t index);
int zmk_vfx_cycle_scene(uint8_t ch, int direction);
uint8_t zmk_vfx_current_scene(uint8_t ch);
const char *zmk_vfx_scene_name(uint8_t ch, uint8_t index);

int zmk_vfx_set_brightness(uint8_t ch, uint8_t value);
int zmk_vfx_set_speed(uint8_t ch, uint8_t value);
int zmk_vfx_set_hue(uint8_t ch, uint16_t degrees);

int zmk_vfx_change_brightness(uint8_t ch, int direction);
int zmk_vfx_change_speed(uint8_t ch, int direction);
int zmk_vfx_change_hue(uint8_t ch, int direction);

uint8_t zmk_vfx_get_brightness(uint8_t ch);
uint8_t zmk_vfx_get_speed(uint8_t ch);
int16_t zmk_vfx_get_hue_shift(uint8_t ch);

/* What a relative step would produce, without applying it. The behavior uses
 * these on the central to turn a relative press into an absolute command
 * before relaying it to the peripherals.
 *
 * ZMK_VFX_CH_ALL reads the first channel, since one value has to stand for
 * the board once it is on the wire.
 */
uint8_t zmk_vfx_calc_scene(uint8_t ch, int direction);
uint8_t zmk_vfx_calc_brightness(uint8_t ch, int direction);
uint8_t zmk_vfx_calc_speed(uint8_t ch, int direction);
uint16_t zmk_vfx_calc_hue(uint8_t ch, int direction);

/* Persist the current state, debounced. */
int zmk_vfx_save_state(void);

/* Shift the engine timebase, used by the synced split mode to follow central.
 * Applied as a slew rather than a jump so corrections are not visible.
 */
void zmk_vfx_set_time_offset(int32_t offset_ms);
int32_t zmk_vfx_get_time_offset(void);

/* A synchronisation beacon from the central carrying its uptime. */
void zmk_vfx_apply_sync(uint32_t central_time_ms);

/* A key position relayed from the other half, for reactive effects. */
void zmk_vfx_inject_key(uint32_t position);

/* Ask for a frame now, outside the normal cadence, after something changed
 * that the scene itself cannot observe.
 */
void zmk_vfx_request_frame(void);
