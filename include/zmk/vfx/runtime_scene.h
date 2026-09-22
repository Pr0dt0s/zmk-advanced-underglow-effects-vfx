/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/layer.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/scenes.h>

/*
 * A scene built at runtime instead of by devicetree, one per channel, kept
 * to a bounded pool the same way the WebAssembly simulator's vfx_sim.c
 * builds one into a fixed arena rather than reading flash-const structs.
 * That is what makes this possible without an allocator: fixed arenas are
 * how scenes already work on this MCU, only the arena is writable RAM here
 * instead of a devicetree-generated ROM section.
 *
 * Only ten generators are buildable this way -- solid, breathe, wave,
 * twinkle, plasma, ripple, keyflash, pulse, dart, static -- all of them
 * "one colour plus up to four small numbers", which is what fits a raw-hid
 * report and keeps every slot's state small. `trail` and `hold` carry a
 * byte per pixel of state (VFX_TRAIL_MAX_PIXELS / VFX_HOLD_MAX_PIXELS, 128
 * each) that would otherwise size every slot in the pool to fit the largest
 * one; `gradient` takes a variable-length stop list; `water`, `matrix`,
 * `fire`, `comet` and `cross` want a second colour and more numbers than
 * fit; the indicator layers read board state through a config that is
 * itself a pointer, not something a generic four-argument wire message can
 * carry. None of those are reachable here. Devicetree remains the only way
 * to reach them, same as it is the only way to reach anything at all today.
 */

enum vfx_rt_type {
    VFX_RT_SOLID = 0,
    VFX_RT_BREATHE,
    VFX_RT_WAVE,
    VFX_RT_TWINKLE,
    VFX_RT_PLASMA,
    VFX_RT_RIPPLE,
    VFX_RT_KEYFLASH,
    VFX_RT_PULSE,
    VFX_RT_DART,
    VFX_RT_STATIC,
    VFX_RT_TYPE_COUNT,
};

/* CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS does not exist for the simulator or the
 * headless test build, neither of which generate Kconfig -- same reason
 * VFX_MAX_RIPPLES and VFX_TRAIL_MAX_PIXELS in layers.h are plain constants
 * rather than Kconfig ones. This falls back to the Kconfig option's own
 * default so a test build sizes the pool the same way real firmware does.
 */
#ifndef CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS
#define CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS 6
#endif

#define VFX_RT_MAX_LAYERS CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS

/* Move direction for vfx_runtime_move_layer(), same sense the composer's own
 * up/down buttons use.
 */
#define VFX_RT_MOVE_UP (-1)
#define VFX_RT_MOVE_DOWN 1

/* bit 0: pulse's `stack`; bit 1: dart's `reverse`; bits 2-4: dart's `axis`
 * (VFX_AXIS_*, 0-5, three bits). Nothing else buildable here has a flag or
 * an axis, so one byte covers all of it.
 */
#define VFX_RT_FLAG_STACK 0x01
#define VFX_RT_FLAG_REVERSE 0x02
#define VFX_RT_FLAG_AXIS_SHIFT 2
#define VFX_RT_FLAG_AXIS_MASK (0x7 << VFX_RT_FLAG_AXIS_SHIFT)

/* Everything a layer needs, exactly what SCENE_ADD_LAYER carries on the wire
 * and what SET_ARG / SET_COLOR / SET_FLAGS touch afterwards. Kept apart from
 * the built vfx_layer/config/state below: an edit rewrites this and rebuilds
 * from it, rather than poking a generator-specific config field by a generic
 * wire index.
 *
 * Zone is a plain pixel range in absolute strip indices, the same numbers
 * `range = <start len>` takes in devicetree. Runtime scenes do not reach the
 * `keys` or `pixels` zone forms.
 */
struct vfx_rt_params {
    uint8_t type; /* enum vfx_rt_type */
    uint8_t zone_start;
    uint8_t zone_len;
    uint8_t blend; /* VFX_BLEND_* */
    uint8_t opacity;
    uint16_t hue; /* 0-359 */
    uint8_t sat;  /* 0-100 */
    uint8_t bri;  /* 0-100 */
    int16_t args[4];
    uint8_t flags;
};

/* The config/state shapes every buildable generator needs. Sized to the
 * largest member rather than to the sum of all of them, which is the entire
 * point of a slot being a union: only one type occupies it at a time.
 */
union vfx_rt_cfg {
    struct vfx_solid_cfg solid;
    struct vfx_breathe_cfg breathe;
    struct vfx_wave_cfg wave;
    struct vfx_twinkle_cfg twinkle;
    struct vfx_plasma_cfg plasma;
    struct vfx_ripple_cfg ripple;
    struct vfx_keyflash_cfg keyflash;
    struct vfx_pulse_cfg pulse;
    struct vfx_dart_cfg dart;
    struct vfx_static_cfg static_cfg;
};

union vfx_rt_state {
    struct vfx_solid_state solid;
    struct vfx_breathe_state breathe;
    struct vfx_ripple_state ripple;
    struct vfx_keyflash_state keyflash;
    struct vfx_pulse_state pulse;
    struct vfx_dart_state dart;
    /* wave, twinkle, plasma and static read no state of their own; this
     * still gives each of them a real (if unused) pointer of its own,
     * rather than a NULL a generator's api->frame was never written to
     * expect.
     */
    uint8_t stateless;
};

struct vfx_rt_slot {
    struct vfx_rt_params params;
    bool used;

    /* Rebuilt from params by rebuild_slot() whenever it changes. layer.zone,
     * .config and .state point into this same slot, so nothing here ever
     * needs to be relocated -- only render_order below, which is what makes
     * add, remove and move cheap regardless of how many layers are in play.
     */
    struct vfx_zone zone;
    union vfx_rt_cfg cfg;
    union vfx_rt_state state;
    struct vfx_layer layer;
};

struct vfx_rt_channel {
    struct vfx_rt_slot slots[VFX_RT_MAX_LAYERS];

    /* Slot indices in render order. A move or a remove touches only this
     * array; the slots themselves, and everything a layer's pointers reach
     * inside one, never move.
     */
    uint8_t render_order[VFX_RT_MAX_LAYERS];
    uint8_t count;

    /* Rebuilt from render_order by rebuild_render() whenever the order or
     * count changes. vfx_scene.layers has to be one contiguous array, which
     * slots (addressed by a stable id, not by position) are not.
     */
    struct vfx_layer render[VFX_RT_MAX_LAYERS];
    struct vfx_scene scene;

    bool active; /* true once this channel is showing this scene */
};

void vfx_runtime_init(void);

void vfx_runtime_reset(uint8_t ch);

/* Slot id (0..VFX_RT_MAX_LAYERS-1) on success, -1 if ch is out of range or
 * the channel's pool is full.
 */
int vfx_runtime_add_layer(uint8_t ch, const struct vfx_rt_params *params);

bool vfx_runtime_set_arg(uint8_t ch, uint8_t slot, uint8_t idx, int16_t value);
bool vfx_runtime_set_color(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat, uint8_t bri);
bool vfx_runtime_remove_layer(uint8_t ch, uint8_t slot);

/* Swaps the layer at `slot` with its neighbour in render order, same as the
 * composer's up/down buttons. False at either end of the list.
 */
bool vfx_runtime_move_layer(uint8_t ch, uint8_t slot, int8_t direction);

bool vfx_runtime_set_active(uint8_t ch, bool active);
bool vfx_runtime_is_active(uint8_t ch);

/* NULL if ch is out of range or its scene has no layers -- render_frame()
 * already treats a NULL scene as black, so a caller need not special-case
 * "nothing built yet" itself.
 */
const struct vfx_scene *vfx_runtime_scene(uint8_t ch);

bool vfx_runtime_get_info(uint8_t ch, uint8_t *count, bool *active);
bool vfx_runtime_get_layer(uint8_t ch, uint8_t slot, struct vfx_rt_params *out);

/* Everything worth persisting about one channel's runtime scene: which
 * slots are used and what they were built from, the order they render in,
 * the count, and whether the channel is showing this rather than its
 * compiled list. Deliberately not the slots themselves: zone/config/state/
 * layer/render/scene are all rebuilt deterministically from params, and
 * every one of them holds plain pointers into this same image -- not
 * something to write to flash and trust on a later, possibly different,
 * build. Kept as its own type, in a scratch buffer vfx_runtime_state()
 * fills on demand, rather than letting a caller reach into vfx_rt_channel
 * and serialise whatever is there.
 */
struct vfx_rt_saved_slot {
    struct vfx_rt_params params;
    bool used;
};

struct vfx_rt_saved_channel {
    struct vfx_rt_saved_slot slots[VFX_RT_MAX_LAYERS];
    uint8_t render_order[VFX_RT_MAX_LAYERS];
    uint8_t count;
    bool active;
};

/* The whole pool, across every channel, for persisting it. Same shape as
 * vfx_tuning_state(): the returned pointer is scratch storage this module
 * owns, valid until the next call.
 */
const void *vfx_runtime_state(uint16_t *len);

/* The other half of that: rebuilds every channel's slots, render order and
 * activation from a blob vfx_runtime_state() previously produced. False
 * (state left untouched) if len does not match, which a caller takes to
 * mean the saved layout does not match this build.
 */
bool vfx_runtime_restore_state(const void *blob, uint16_t len);
