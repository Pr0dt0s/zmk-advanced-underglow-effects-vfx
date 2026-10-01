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
 * The `ch` every one of them takes is a target byte: the channel in the low
 * nibble and, in the high one, which of that channel's runtime scenes
 * (VFX_RT_TARGET() in runtime_scene.h). 0 to VFX_MAX_CHANNELS-1 therefore
 * still means "that channel's first scene".
 *
 * `ch` is a channel index, never ZMK_VFX_CH_ALL: a runtime scene belongs to
 * one channel, unlike a tuning slot which can belong to layers on several.
 * -EINVAL for a channel out of range, an unusable generator type, an
 * argument index outside 0-5, or a slot nothing was added to; -ENOSPC if
 * the channel's pool is already full (or, for a trail or hold, if every
 * heavy state is taken).
 *
 * Every generator can be built this way. What does not fit in one
 * SCENE_ADD_LAYER is added by follow-up calls on the slot it returns:
 * set_arg for arguments 4 and 5, set_list_color for a second and later
 * colour, set_zone for a pixel or key zone, set_opts for blend, opacity and
 * the tuning slot.
 */
struct vfx_rt_params;

int zmk_vfx_scene_reset(uint8_t ch);
int zmk_vfx_scene_add_layer(uint8_t ch, const struct vfx_rt_params *params, uint8_t *slot_out);
int zmk_vfx_scene_set_arg(uint8_t ch, uint8_t slot, uint8_t idx, int16_t value);
int zmk_vfx_scene_set_color(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri);
int zmk_vfx_scene_remove_layer(uint8_t ch, uint8_t slot);

/* A layer built in the dark: _add_layer_staged() reserves a slot without
 * rendering it, the usual edits fill it in, and _commit_layer() puts it in
 * render order at `position` (0xFF = on top) in one step. _set_flags() changes
 * stack/reverse/axis on a layer that already exists. See runtime_scene.h.
 */
int zmk_vfx_scene_add_layer_staged(uint8_t ch, const struct vfx_rt_params *params,
                                   uint8_t *slot_out);
int zmk_vfx_scene_commit_layer(uint8_t ch, uint8_t slot, uint8_t position);
int zmk_vfx_scene_set_flags(uint8_t ch, uint8_t slot, uint8_t flags);
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
int zmk_vfx_scene_info(uint8_t ch, uint8_t *count, bool *active, uint8_t *active_scene,
                       uint32_t *hash);
int zmk_vfx_scene_get_layer(uint8_t ch, uint8_t slot, struct vfx_rt_params *out);

/* Slot ids in render order; `order` must have room for
 * CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS. The only way to learn that order --
 * add, remove and move never renumber a slot, only its position here.
 */
int zmk_vfx_scene_get_order(uint8_t ch, uint8_t *order, uint8_t *count);

/* Appends one stop to a gradient slot's list, in the order stops are sent --
 * a gradient's stops rarely fit alongside its other fields in one
 * SCENE_ADD_LAYER, so it is built with an ordinary (type VFX_RT_GRADIENT,
 * hue/sat/bri ignored) add followed by one of these per stop. -EINVAL for a
 * channel or slot out of range or a slot that is not a gradient; -ENOSPC
 * once its list already holds VFX_RT_GRADIENT_MAX_STOPS.
 */
int zmk_vfx_scene_gradient_add_stop(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat,
                                    uint8_t bri);

/* The read side, for a host reconstructing a gradient it did not just build
 * itself -- get_layer's own reply carries how many stops there are (in its
 * args[2]) but not the stops themselves. -EINVAL for a channel, slot or
 * index out of range, or a slot that is not a gradient.
 */
int zmk_vfx_scene_gradient_get_stop(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                    uint8_t *sat, uint8_t *bri);

/* Entry `idx` of a slot's colour list, for any generator type: what a
 * generator's second and later colours are (see runtime_scene.h for which
 * generator reads which entry). Growing the list past its length fills the
 * skipped entries with black, which every generator treats as "unset".
 * -EINVAL for a channel or slot out of range; -ENOSPC for an index at or past
 * VFX_RT_MAX_COLORS. The getter is -EINVAL past the list's current length.
 */
int zmk_vfx_scene_set_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t hue, uint8_t sat,
                                 uint8_t bri);
int zmk_vfx_scene_get_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                 uint8_t *sat, uint8_t *bri);

/* A slot's zone, in pieces: a range (data is start then length), or a list of
 * strip indices or key positions built strictly in order across as many calls
 * as it takes (see vfx_runtime_set_zone()). -ENOSPC when a list would outgrow
 * VFX_RT_MAX_ZONE_PIXELS, -EINVAL for anything else refused, including a chunk
 * whose offset is not where the list currently ends.
 */
int zmk_vfx_scene_set_zone(uint8_t ch, uint8_t slot, uint8_t kind, uint8_t offset,
                           const uint8_t *data, uint8_t count);
int zmk_vfx_scene_get_zone(uint8_t ch, uint8_t slot, uint8_t offset, uint8_t *kind,
                           uint8_t *total, uint8_t *data, uint8_t max, uint8_t *n);

/* blend, opacity, the source driving it and the tuning slot, all at once.
 * -EINVAL for a blend or source this build does not know.
 */
int zmk_vfx_scene_set_opts(uint8_t ch, uint8_t slot, uint8_t blend, uint8_t opacity,
                           uint8_t opacity_src, uint8_t opacity_min, uint8_t opacity_full,
                           uint8_t tune_id);

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

/* Runtime-scene edits, relayed from the central to a peripheral so a scene
 * built from a host over raw-hid (central-only) shows on both halves rather
 * than only the one the host is plugged into. zmk_vfx_scene_relay_send() is
 * called by hid_transport.c once it has applied a scene op locally; a no-op
 * unless this half is a split central. zmk_vfx_scene_relay_receive() is
 * called by behavior_vfx.c for VFX_RT_RELAY_CMD, applying the same op once
 * every chunk of it (see hid_protocol.h) has arrived.
 */
int zmk_vfx_scene_relay_send(const uint8_t *data, uint8_t len);

/* Whether a request of `len` bytes would be accepted by zmk_vfx_scene_relay_send()
 * right now; hid_transport.c asks before applying anything locally.
 */
bool zmk_vfx_scene_relay_room(uint8_t len);
void zmk_vfx_scene_relay_receive(uint32_t param1, uint32_t param2);

/* Ask for a frame now, outside the normal cadence, after something changed
 * that the scene itself cannot observe.
 */
void zmk_vfx_request_frame(void);
