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

/* Adjust one tuning slot, for layers that opted in with a tune-id. These
 * address a slot rather than a channel: a slot is a handle on particular
 * layers wherever they sit, which is what makes it the thing a host would
 * name when changing something live.
 *
 * -EINVAL for a slot outside 1..VFX_TUNE_SLOTS-1, so a request relayed from
 * somewhere else can be refused rather than quietly doing nothing.
 */
int zmk_vfx_tune_hue(uint8_t slot, int16_t degrees);
int zmk_vfx_tune_level(uint8_t slot, uint8_t level);
int zmk_vfx_tune_speed(uint8_t slot, uint8_t speed);
int zmk_vfx_tune_reset(uint8_t slot);

/* Building a channel's own scene at runtime rather than picking one from
 * its compiled list. Requires CONFIG_ZMK_VFX_RUNTIME_SCENES; every one of
 * these is a thin wrapper over runtime_scene.h's own builder, adding the
 * same two side effects zmk_vfx_tune_*() adds over vfx_tuning_set_*():
 * asking for a redraw and persisting, debounced.
 *
 * `ch` is a channel index, never ZMK_VFX_CH_ALL: a runtime scene belongs to
 * one channel, unlike a tuning slot which can belong to layers on several.
 * -EINVAL for a channel out of range, an unusable generator type, an
 * argument index outside 0-3, or a slot nothing was added to; -ENOSPC if
 * the channel's pool is already full.
 */
struct vfx_rt_params;

int zmk_vfx_scene_reset(uint8_t ch);
int zmk_vfx_scene_add_layer(uint8_t ch, const struct vfx_rt_params *params, uint8_t *slot_out);
int zmk_vfx_scene_set_arg(uint8_t ch, uint8_t slot, uint8_t idx, int16_t value);
int zmk_vfx_scene_set_color(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri);
int zmk_vfx_scene_remove_layer(uint8_t ch, uint8_t slot);
int zmk_vfx_scene_move_layer(uint8_t ch, uint8_t slot, int8_t direction);

/* Switches the channel between showing this scene and its compiled list.
 * Selecting any compiled scene (zmk_vfx_select_scene()) also deactivates,
 * so NEXT/PREV on the keymap are never left silently stuck on it.
 */
int zmk_vfx_scene_activate(uint8_t ch);
int zmk_vfx_scene_deactivate(uint8_t ch);

/* Reads, for a host reconnecting to sync its own view rather than trusting
 * whatever it last knew. Same -EINVAL as the writers above; count and
 * active are left untouched on failure so a caller can pass its own
 * defaults in without a separate zero-init.
 */
int zmk_vfx_scene_info(uint8_t ch, uint8_t *count, bool *active);
int zmk_vfx_scene_get_layer(uint8_t ch, uint8_t slot, struct vfx_rt_params *out);

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
