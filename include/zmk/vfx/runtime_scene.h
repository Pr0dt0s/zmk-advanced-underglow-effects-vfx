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
 * Every generator is buildable this way. What differs between them is only
 * how much of their configuration one wire message has to carry, so the
 * configuration is assembled from several small messages instead of one
 * large one:
 *
 *   - a primary colour (hue/sat/bri) and up to VFX_RT_MAX_ARGS small numbers,
 *     which is everything a simple generator needs;
 *   - a short list of further colours (VFX_RT_MAX_COLORS): a gradient's
 *     stops, the second colour water/matrix/fire/comet/cross draw with, the
 *     ends of a battery bar, a colour per keymap layer for layer-state;
 *   - the zone, as a range, an explicit pixel list or a list of key
 *     positions, the last two sent in chunks;
 *   - the layer's own options: blend, opacity, the source that can drive
 *     that opacity, and the tuning slot it answers to.
 *
 * `trail` and `hold` carry a byte per pixel of animation state
 * (VFX_TRAIL_MAX_PIXELS / VFX_HOLD_MAX_PIXELS, 128 each). Sizing every slot
 * for that would waste it on the layers that need none, so those two draw
 * from a small pool of their own instead (VFX_RT_HEAVY_STATES per channel).
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
    VFX_RT_GRADIENT,
    VFX_RT_TRAIL,
    VFX_RT_HOLD,
    VFX_RT_WATER,
    VFX_RT_MATRIX,
    VFX_RT_FIRE,
    VFX_RT_COMET,
    VFX_RT_CROSS,
    VFX_RT_LAYER_STATE,
    VFX_RT_BATTERY,
    VFX_RT_BLE_PROFILE,
    VFX_RT_FLAG,
    VFX_RT_WPM,
    VFX_RT_PERIPHERAL_BATTERY,
    VFX_RT_TYPE_COUNT,
};

/* The CONFIG_ZMK_VFX_RUNTIME_* options do not exist for the simulator or the
 * headless test build, neither of which generate Kconfig -- same reason
 * VFX_MAX_RIPPLES and VFX_TRAIL_MAX_PIXELS in layers.h are plain constants
 * rather than Kconfig ones. Each falls back to its Kconfig option's own
 * default so a test build sizes the pool the same way real firmware does.
 */
#ifndef CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS
#define CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS 6
#endif

#ifndef CONFIG_ZMK_VFX_RUNTIME_MAX_COLORS
#define CONFIG_ZMK_VFX_RUNTIME_MAX_COLORS 8
#endif

#ifndef CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS
#define CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS 32
#endif

#ifndef CONFIG_ZMK_VFX_RUNTIME_HEAVY_STATES
#define CONFIG_ZMK_VFX_RUNTIME_HEAVY_STATES 2
#endif

#define VFX_RT_MAX_LAYERS CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS
#define VFX_RT_MAX_COLORS CONFIG_ZMK_VFX_RUNTIME_MAX_COLORS
#define VFX_RT_MAX_ZONE_PIXELS CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS
#define VFX_RT_HEAVY_STATES CONFIG_ZMK_VFX_RUNTIME_HEAVY_STATES

/* Small numbers a generator can be given, beyond its colours. SCENE_ADD_LAYER
 * carries the first four; SCENE_SET_ARG reaches all six, which is what lets
 * water and matrix (six each) be built at all inside a 20-byte relayed
 * request.
 */
#define VFX_RT_MAX_ARGS 6

/* Kept for the gradient-specific calls below, which predate the general
 * colour list: a gradient's stops are that list.
 */
#define VFX_RT_GRADIENT_MAX_STOPS VFX_RT_MAX_COLORS

/* What vfx_runtime_add_layer() returns instead of a slot id. */
#define VFX_RT_ERR_INVALID (-1) /* channel or type out of range, or a bad zone list */
#define VFX_RT_ERR_FULL (-2)    /* no free slot, or no free heavy state for trail/hold */

/* Move direction for vfx_runtime_move_layer(), same sense the composer's own
 * up/down buttons use.
 */
#define VFX_RT_MOVE_UP (-1)
#define VFX_RT_MOVE_DOWN 1

/* bit 0: pulse's `stack`; bit 1: dart's `reverse`; bits 2-4: the axis of
 * every generator that has one (VFX_AXIS_*, 0-5, three bits). Nothing else
 * buildable here has a flag or an axis, so one byte covers all of it.
 */
#define VFX_RT_FLAG_STACK 0x01
#define VFX_RT_FLAG_REVERSE 0x02
#define VFX_RT_FLAG_AXIS_SHIFT 2
#define VFX_RT_FLAG_AXIS_MASK (0x7 << VFX_RT_FLAG_AXIS_SHIFT)

/* How a layer's zone is written. RANGE is `range = <start len>`; PIXELS is
 * `pixels = <...>`, strip indices; KEYS is `keys = <...>`, key positions
 * resolved to pixels through the engine's key map, the same way a
 * devicetree `keys` zone is.
 */
#define VFX_RT_ZONE_RANGE 0
#define VFX_RT_ZONE_PIXELS 1
#define VFX_RT_ZONE_KEYS 2

/* Everything a layer needs, exactly what the SCENE_* ops build up and what
 * an edit touches afterwards. Kept apart from the built vfx_layer/config/
 * state below: an edit rewrites this and rebuilds from it, rather than
 * poking a generator-specific config field by a generic wire index.
 *
 * Which colour and which number feeds which generator field is
 * rebuild_slot()'s table in runtime_scene.c, and the README's runtime
 * section lists it per generator; `colors[0]` is the "second colour" of
 * every generator that has one (crest, head, tip, centre, high, fast...).
 */
struct vfx_rt_params {
    uint8_t type; /* enum vfx_rt_type */

    /* Zone. For RANGE, zone_start/zone_len are the range. For PIXELS and
     * KEYS, zone_items[0..zone_count) is the list and zone_start/zone_len are
     * unused.
     */
    uint8_t zone_kind; /* VFX_RT_ZONE_* */
    uint8_t zone_start;
    uint8_t zone_len;
    uint8_t zone_count;
    uint8_t zone_items[VFX_RT_MAX_ZONE_PIXELS];

    uint8_t blend; /* VFX_BLEND_* */
    uint8_t opacity;
    uint8_t opacity_src; /* VFX_SRC_* */
    uint8_t opacity_min;
    uint8_t opacity_full;
    uint8_t tune_id; /* tuning slot, 0 for none */

    uint16_t hue; /* 0-359: the primary colour */
    uint8_t sat;  /* 0-100 */
    uint8_t bri;  /* 0-100 */
    int16_t args[VFX_RT_MAX_ARGS];
    uint8_t flags;

    /* The colour list. Filled in by vfx_runtime_gradient_add_stop() and
     * vfx_runtime_set_list_color() rather than by SCENE_ADD_LAYER itself: a
     * list rarely fits alongside a layer's other fields in one report, and
     * params is what persists and what an edit rewrites, so it has to
     * survive both the same way every other field here does.
     */
    uint32_t colors[VFX_RT_MAX_COLORS]; /* packed VFX_HSB */
    uint8_t num_colors;
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
    struct vfx_trail_cfg trail;
    struct vfx_hold_cfg hold;
    struct vfx_water_cfg water;
    struct vfx_matrix_cfg matrix;
    struct vfx_fire_cfg fire;
    struct vfx_comet_cfg comet;
    struct vfx_cross_cfg cross;
    struct vfx_battery_cfg battery;
    struct vfx_ble_profile_cfg ble_profile;
    struct vfx_flag_cfg flag;
    struct vfx_wpm_cfg wpm;
    struct vfx_peripheral_battery_cfg peripheral_battery;
    /* .stops and .colors point at the owning slot's own params.colors -- see
     * rebuild_slot() -- never anywhere else, so neither dangles.
     */
    struct vfx_gradient_cfg gradient;
    struct vfx_layer_state_cfg layer_state;
};

union vfx_rt_state {
    struct vfx_solid_state solid;
    struct vfx_breathe_state breathe;
    struct vfx_ripple_state ripple;
    struct vfx_keyflash_state keyflash;
    struct vfx_pulse_state pulse;
    struct vfx_dart_state dart;
    struct vfx_gradient_state gradient;
    struct vfx_water_state water;
    struct vfx_matrix_state matrix;
    struct vfx_cross_state cross;
    /* Everything else reads no state of its own; this still gives each of
     * them a real (if unused) pointer, rather than a NULL a generator's
     * api->frame was never written to expect.
     */
    uint8_t stateless;
};

/* The per-pixel state trail and hold need, one of these per pool entry. */
union vfx_rt_heavy_state {
    struct vfx_trail_state trail;
    struct vfx_hold_state hold;
};

struct vfx_rt_slot {
    struct vfx_rt_params params;
    bool used;

    /* Set while this slot (trail or hold only) holds a pool entry, and which
     * one. Released when the slot is removed.
     */
    bool has_heavy;
    uint8_t heavy_idx;

    /* Rebuilt from params by rebuild_slot() whenever it changes. layer.zone,
     * .config and .state point into this same slot, so nothing here ever
     * needs to be relocated -- only render_order below, which is what makes
     * add, remove and move cheap regardless of how many layers are in play.
     */
    struct vfx_zone zone;
    uint8_t zone_pixels[VFX_RT_MAX_ZONE_PIXELS]; /* a KEYS zone, resolved to pixels */
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

    union vfx_rt_heavy_state heavy[VFX_RT_HEAVY_STATES];
    bool heavy_used[VFX_RT_HEAVY_STATES];

    /* Rebuilt from render_order by rebuild_render() whenever the order, the
     * count or a layer's own options change. vfx_scene.layers has to be one
     * contiguous array, which slots (addressed by a stable id, not by
     * position) are not.
     */
    struct vfx_layer render[VFX_RT_MAX_LAYERS];
    struct vfx_scene scene;

    bool active; /* true once this channel is showing this scene */
};

void vfx_runtime_init(void);

void vfx_runtime_reset(uint8_t ch);

/* Where key positions turn into pixels: the engine's frame context, whose
 * key map and this half's strip offset are what a KEYS zone resolves through.
 * Copied, and every KEYS zone already built is resolved again, so calling
 * this after a scene was restored from flash is what makes its key zones
 * land. Until it is called a KEYS zone resolves to nothing.
 */
void vfx_runtime_set_key_context(const struct vfx_frame_ctx *ctx);

/* Slot id (0..VFX_RT_MAX_LAYERS-1) on success, otherwise VFX_RT_ERR_INVALID
 * (ch out of range, unknown type, or a zone list longer than the pool
 * allows) or VFX_RT_ERR_FULL (the channel's pool is full, or it is a trail
 * or hold and every heavy state is taken).
 */
int vfx_runtime_add_layer(uint8_t ch, const struct vfx_rt_params *params);

/* idx 0..VFX_RT_MAX_ARGS-1. */
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

/* Slot ids in render order, bottom of the stack first, same sense
 * render_order[] itself already keeps them in. `order` must have room for
 * VFX_RT_MAX_LAYERS; `*count` is how many of those it actually filled. This
 * is the only way to learn that order at all -- get_layer answers one slot
 * at a time and says nothing about where it renders relative to the rest.
 */
bool vfx_runtime_get_order(uint8_t ch, uint8_t *order, uint8_t *count);

/* Sets entry `idx` of a slot's colour list (any generator type), growing the
 * list to idx+1 if it is longer than the list was; entries a growth skips
 * over read as black, which every generator treats as "unset". False if ch or
 * slot is not valid or idx is VFX_RT_MAX_COLORS or more.
 */
bool vfx_runtime_set_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t hue,
                                uint8_t sat, uint8_t bri);

/* False if ch, slot or idx is out of range (idx at or past the list's
 * current length).
 */
bool vfx_runtime_get_list_color(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                uint8_t *sat, uint8_t *bri);

/* Appends one stop to a gradient slot's list and rebuilds it, in the order
 * stops are sent. False if ch or slot is out of range, the slot is not a
 * gradient, or its list already holds VFX_RT_MAX_COLORS stops.
 */
bool vfx_runtime_gradient_add_stop(uint8_t ch, uint8_t slot, uint16_t hue, uint8_t sat,
                                   uint8_t bri);

/* The other half, for a host reading a gradient back one stop at a time.
 * False if ch, slot or idx is out of range, or the slot is not a gradient.
 */
bool vfx_runtime_gradient_get_stop(uint8_t ch, uint8_t slot, uint8_t idx, uint16_t *hue,
                                   uint8_t *sat, uint8_t *bri);

/* Writes part of a slot's zone.
 *
 * RANGE: data[0] is the start and data[1] the length (count must be at least
 * 2, offset is ignored), replacing whatever the zone was.
 *
 * PIXELS and KEYS: appends `count` entries -- strip indices or key positions
 * -- to a list of at most VFX_RT_MAX_ZONE_PIXELS. offset must equal the
 * number of entries already there, so a list is built strictly in order and a
 * lost chunk shows up as a refusal rather than a silently short zone; offset
 * 0 starts the list over, and is also what switches a zone from one kind to
 * another. count of 0 with offset 0 makes an empty list.
 *
 * False if ch or slot is invalid, the kind is unknown, or the list would
 * overrun the pool.
 */
bool vfx_runtime_set_zone(uint8_t ch, uint8_t slot, uint8_t kind, uint8_t offset,
                          const uint8_t *data, uint8_t count);

/* Reads a slot's zone back in pieces. *kind and *total are what the zone
 * is; up to `max` bytes of it starting at `offset` go to `data` and `*n` is
 * how many. For RANGE the "list" is the two bytes start, length. False if
 * ch or slot is invalid or offset is past the end.
 */
bool vfx_runtime_get_zone(uint8_t ch, uint8_t slot, uint8_t offset, uint8_t *kind, uint8_t *total,
                          uint8_t *data, uint8_t max, uint8_t *n);

/* blend, opacity, the source that drives opacity, and the tuning slot. False
 * if ch or slot is invalid, blend is past VFX_BLEND_MAX or opacity_src past
 * VFX_SRC_ACTIVITY.
 */
bool vfx_runtime_set_opts(uint8_t ch, uint8_t slot, uint8_t blend, uint8_t opacity,
                          uint8_t opacity_src, uint8_t opacity_min, uint8_t opacity_full,
                          uint8_t tune_id);

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
 * mean the saved layout does not match this build. A slot whose saved
 * params fail validation, or that needs a heavy state none is left for, is
 * dropped rather than trusted.
 */
bool vfx_runtime_restore_state(const void *blob, uint16_t len);
