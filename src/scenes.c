/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/devicetree.h>
#include <zephyr/sys/util.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/scenes.h>

/* Turns the devicetree into the plain structs the compositor consumes.
 *
 * Node ids expand to valid C identifier tokens, so _CONCAT() against them
 * gives every node a unique symbol without needing an index.
 *
 * Generators are dispatched without a chain of COND_CODE_1 on compatible:
 * each generator's DEFINE macro emits its own api pointer under a predictable
 * symbol, and VFX_LAYER_ENTRY just dereferences it. Adding a generator means
 * adding a DEFINE macro and one DT_FOREACH line, nothing else.
 */

#define VFX_CFG_SYM(node) _CONCAT(vfx_cfg_, node)
#define VFX_STATE_SYM(node) _CONCAT(vfx_state_, node)
#define VFX_API_SYM(node) _CONCAT(vfx_api_, node)
#define VFX_ZONE_SYM(node) _CONCAT(vfx_zone_, node)
#define VFX_ZONE_PX_SYM(node) _CONCAT(vfx_zone_px_, node)
#define VFX_ZONE_KEYS_SYM(node) _CONCAT(vfx_zone_keys_, node)
#define VFX_KEY_ZONE_SYM(node) _CONCAT(vfx_key_zone_, node)
#define VFX_LAYERS_SYM(node) _CONCAT(vfx_layers_, node)
#define VFX_SCENE_SYM(node) _CONCAT(vfx_scene_, node)

/* ------------------------------------------------------------------ zones */

#define VFX_ZONE_PIXELS(node)                                                                      \
    COND_CODE_1(DT_NODE_HAS_PROP(node, pixels),                                                    \
                (static const uint8_t VFX_ZONE_PX_SYM(node)[] = DT_PROP(node, pixels);), ())

/* A zone given as key positions cannot be resolved until the engine knows the
 * key map and this half's offset, so it is left empty here and filled in at
 * startup. It is the one zone that lives in RAM rather than flash.
 */
#define VFX_ZONE_BY_KEYS(node)                                                                     \
    static const uint8_t VFX_ZONE_KEYS_SYM(node)[] = DT_PROP(node, keys);                          \
    static uint8_t VFX_ZONE_PX_SYM(node)[DT_PROP_LEN(node, keys)];                                 \
    static struct vfx_zone VFX_ZONE_SYM(node);                                                     \
    static const struct vfx_key_zone VFX_KEY_ZONE_SYM(node) = {                                    \
        .keys = VFX_ZONE_KEYS_SYM(node),                                                           \
        .num_keys = DT_PROP_LEN(node, keys),                                                       \
        .pixels = VFX_ZONE_PX_SYM(node),                                                           \
        .zone = &VFX_ZONE_SYM(node),                                                               \
    };

#define VFX_ZONE_BY_PIXELS(node)                                                                   \
    VFX_ZONE_PIXELS(node)                                                                          \
    static const struct vfx_zone VFX_ZONE_SYM(node) = {                                            \
        .pixels = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (VFX_ZONE_PX_SYM(node)), (NULL)),    \
        .start = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (0),                                  \
                             (DT_PROP_BY_IDX(node, range, 0))),                                    \
        .len = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (DT_PROP_LEN(node, pixels)),            \
                           (DT_PROP_BY_IDX(node, range, 1))),                                      \
    };

#define VFX_ZONE_DEFINE(node)                                                                      \
    COND_CODE_1(DT_NODE_HAS_PROP(node, keys), (VFX_ZONE_BY_KEYS(node)), (VFX_ZONE_BY_PIXELS(node)))

DT_FOREACH_STATUS_OKAY(zmk_vfx_zone, VFX_ZONE_DEFINE)

/* Every key zone in one table, so the engine can resolve them all at startup
 * without knowing what any of them are for.
 */
#define VFX_KEY_ZONE_ENTRY(node)                                                                   \
    COND_CODE_1(DT_NODE_HAS_PROP(node, keys), (&VFX_KEY_ZONE_SYM(node),), ())

static const struct vfx_key_zone *const vfx_key_zone_list[] = {
    DT_FOREACH_STATUS_OKAY(zmk_vfx_zone, VFX_KEY_ZONE_ENTRY) NULL};

void vfx_resolve_key_zones(const struct vfx_frame_ctx *ctx) {
    for (size_t i = 0; vfx_key_zone_list[i] != NULL; i++) {
        vfx_key_zone_resolve(vfx_key_zone_list[i], ctx);
    }
}

/* ------------------------------------------------------------- generators */

/* Generators with nothing to remember between frames still declare a state
 * symbol, a single unread byte, so that VFX_LAYER_ENTRY below stays uniform
 * instead of branching on the generator type.
 */

#define VFX_SOLID_DEFINE(node)                                                                     \
    static const struct vfx_solid_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
    };                                                                                             \
    static struct vfx_solid_state VFX_STATE_SYM(node);                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_solid_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_solid, VFX_SOLID_DEFINE)

#define VFX_GRADIENT_DEFINE(node)                                                                  \
    static const uint32_t _CONCAT(vfx_stops_, node)[] = DT_PROP(node, stops);                      \
    static const struct vfx_gradient_cfg VFX_CFG_SYM(node) = {                                     \
        .stops = _CONCAT(vfx_stops_, node),                                                        \
        .num_stops = DT_PROP_LEN(node, stops),                                                     \
        .scroll_speed = DT_PROP(node, scroll_speed),                                               \
        .span = DT_PROP(node, span),                                                               \
        .axis = DT_PROP(node, axis),                                                               \
    };                                                                                             \
    static struct vfx_gradient_state VFX_STATE_SYM(node);                                          \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_gradient_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_gradient, VFX_GRADIENT_DEFINE)


#define VFX_BREATHE_DEFINE(node)                                                                   \
    static const struct vfx_breathe_cfg VFX_CFG_SYM(node) = {                                      \
        .color = DT_PROP(node, color),                                                             \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .min_level = DT_PROP(node, min_level),                                                      \
        .hue_swing = DT_PROP(node, hue_swing),                                                     \
    };                                                                                             \
    static struct vfx_breathe_state VFX_STATE_SYM(node);                                           \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_breathe_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_breathe, VFX_BREATHE_DEFINE)

#define VFX_WAVE_DEFINE(node)                                                                      \
    static const struct vfx_wave_cfg VFX_CFG_SYM(node) = {                                         \
        .color = DT_PROP(node, color),                                                             \
        .wavelength = DT_PROP(node, wavelength),                                                   \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .depth = DT_PROP(node, depth),                                                             \
        .axis = DT_PROP(node, axis),                                                               \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_wave_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_wave, VFX_WAVE_DEFINE)

#define VFX_TWINKLE_DEFINE(node)                                                                   \
    static const struct vfx_twinkle_cfg VFX_CFG_SYM(node) = {                                      \
        .color = DT_PROP(node, color),                                                             \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .density = DT_PROP(node, density),                                                         \
        .hue_spread = DT_PROP(node, hue_spread),                                                   \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_twinkle_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_twinkle, VFX_TWINKLE_DEFINE)

#define VFX_PLASMA_DEFINE(node)                                                                    \
    static const struct vfx_plasma_cfg VFX_CFG_SYM(node) = {                                       \
        .color = DT_PROP(node, color),                                                             \
        .scale = DT_PROP(node, scale),                                                             \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .hue_spread = DT_PROP(node, hue_spread),                                                   \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_plasma_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_plasma, VFX_PLASMA_DEFINE)

#define VFX_RIPPLE_DEFINE(node)                                                                    \
    static const struct vfx_ripple_cfg VFX_CFG_SYM(node) = {                                       \
        .color = DT_PROP(node, color),                                                             \
        .decay_ms = DT_PROP(node, decay_ms),                                                       \
        .speed = DT_PROP(node, speed),                                                             \
        .width = DT_PROP(node, width),                                                             \
    };                                                                                             \
    static struct vfx_ripple_state VFX_STATE_SYM(node);                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_ripple_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_ripple, VFX_RIPPLE_DEFINE)

#define VFX_WATER_DEFINE(node)                                                                     \
    static const struct vfx_water_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
        .crest_color = DT_PROP(node, crest_color),                                                 \
        .wavelength = DT_PROP(node, wavelength),                                                   \
        .speed = DT_PROP(node, speed),                                                             \
        .lifetime_ms = DT_PROP(node, lifetime_ms),                                                 \
        .drop_rate_ms = DT_PROP(node, drop_rate_ms),                                               \
        .amplitude = DT_PROP(node, amplitude),                                                     \
        .damping = DT_PROP(node, damping),                                                         \
    };                                                                                             \
    static struct vfx_water_state VFX_STATE_SYM(node);                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_water_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_water, VFX_WATER_DEFINE)

#define VFX_MATRIX_DEFINE(node)                                                                    \
    static const struct vfx_matrix_cfg VFX_CFG_SYM(node) = {                                       \
        .color = DT_PROP(node, color),                                                             \
        .head_color = DT_PROP(node, head_color),                                                   \
        .speed = DT_PROP(node, speed),                                                             \
        .tail = DT_PROP(node, tail),                                                               \
        .drop_rate_ms = DT_PROP(node, drop_rate_ms),                                               \
        .columns = DT_PROP(node, columns),                                                         \
        .jitter = DT_PROP(node, jitter),                                                           \
        .head_size = DT_PROP(node, head_size),                                                     \
    };                                                                                             \
    static struct vfx_matrix_state VFX_STATE_SYM(node);                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_matrix_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_matrix, VFX_MATRIX_DEFINE)

#define VFX_FLAG_DEFINE(node)                                                                      \
    static const struct vfx_flag_cfg VFX_CFG_SYM(node) = {                                         \
        .color = DT_PROP(node, color),                                                             \
        .source = DT_PROP(node, source),                                                           \
        .mask = DT_PROP(node, mask),                                                               \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_flag_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_flag, VFX_FLAG_DEFINE)

#define VFX_WPM_DEFINE(node)                                                                       \
    static const struct vfx_wpm_cfg VFX_CFG_SYM(node) = {                                          \
        .idle_color = DT_PROP(node, idle_color),                                                   \
        .fast_color = DT_PROP(node, fast_color),                                                   \
        .full = DT_PROP(node, full),                                                               \
        .bar = DT_PROP(node, bar),                                                                 \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_wpm_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_wpm, VFX_WPM_DEFINE)

#define VFX_PERIPHERAL_BATTERY_DEFINE(node)                                                        \
    static const struct vfx_peripheral_battery_cfg VFX_CFG_SYM(node) = {                           \
        .low_color = DT_PROP(node, low_color),                                                     \
        .high_color = DT_PROP(node, high_color),                                                   \
        .empty_color = DT_PROP(node, empty_color),                                                 \
        .unknown_color = DT_PROP(node, unknown_color),                                             \
        .source = DT_PROP(node, source),                                                           \
        .warn_below = DT_PROP(node, warn_below),                                                   \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) =                                   \
        &vfx_layer_peripheral_battery_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_peripheral_battery, VFX_PERIPHERAL_BATTERY_DEFINE)

#define VFX_FIRE_DEFINE(node)                                                                      \
    static const struct vfx_fire_cfg VFX_CFG_SYM(node) = {                                         \
        .base_color = DT_PROP(node, base_color),                                                   \
        .tip_color = DT_PROP(node, tip_color),                                                     \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .cell = DT_PROP(node, cell),                                                               \
        .height = DT_PROP(node, height),                                                           \
        .flicker = DT_PROP(node, flicker),                                                         \
        .axis = DT_PROP(node, axis),                                                               \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_fire_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_fire, VFX_FIRE_DEFINE)

#define VFX_COMET_DEFINE(node)                                                                     \
    static const struct vfx_comet_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
        .head_color = DT_PROP(node, head_color),                                                   \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .tail = DT_PROP(node, tail),                                                               \
        .count = DT_PROP(node, count),                                                             \
        .axis = DT_PROP(node, axis),                                                               \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                            \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_comet_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_comet, VFX_COMET_DEFINE)

#define VFX_CROSS_DEFINE(node)                                                                     \
    static const struct vfx_cross_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
        .centre_color = DT_PROP(node, centre_color),                                               \
        .decay_ms = DT_PROP(node, decay_ms),                                                       \
        .radius = DT_PROP(node, radius),                                                           \
        .thickness = DT_PROP(node, thickness),                                                     \
        .axes = DT_PROP(node, axes),                                                               \
    };                                                                                             \
    static struct vfx_cross_state VFX_STATE_SYM(node);                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_cross_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_cross, VFX_CROSS_DEFINE)

#define VFX_KEYFLASH_DEFINE(node)                                                                  \
    static const struct vfx_keyflash_cfg VFX_CFG_SYM(node) = {                                     \
        .color = DT_PROP(node, color),                                                             \
        .decay_ms = DT_PROP(node, decay_ms),                                                       \
        .spread = DT_PROP(node, spread),                                                           \
    };                                                                                             \
    static struct vfx_keyflash_state VFX_STATE_SYM(node);                                          \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_keyflash_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_keyflash, VFX_KEYFLASH_DEFINE)

#define VFX_TRAIL_DEFINE(node)                                                                     \
    static const struct vfx_trail_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
        .decay_ms = DT_PROP(node, decay_ms),                                                       \
        .spread = DT_PROP(node, spread),                                                           \
    };                                                                                             \
    static struct vfx_trail_state VFX_STATE_SYM(node);                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_trail_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_trail, VFX_TRAIL_DEFINE)

#define VFX_PULSE_DEFINE(node)                                                                     \
    static const struct vfx_pulse_cfg VFX_CFG_SYM(node) = {                                        \
        .color = DT_PROP(node, color),                                                             \
        .decay_ms = DT_PROP(node, decay_ms),                                                       \
        .min_level = DT_PROP(node, min_level),                                                     \
        .hue_step = DT_PROP(node, hue_step),                                                       \
        .stack = DT_PROP(node, stack),                                                             \
    };                                                                                             \
    static struct vfx_pulse_state VFX_STATE_SYM(node);                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_pulse_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_pulse, VFX_PULSE_DEFINE)

#define VFX_LAYER_STATE_DEFINE(node)                                                               \
    static const uint32_t _CONCAT(vfx_colors_, node)[] = DT_PROP(node, colors);                    \
    static const struct vfx_layer_state_cfg VFX_CFG_SYM(node) = {                                  \
        .colors = _CONCAT(vfx_colors_, node),                                                      \
        .num_colors = DT_PROP_LEN(node, colors),                                                   \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_layer_state_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_layer_state, VFX_LAYER_STATE_DEFINE)

#define VFX_BATTERY_DEFINE(node)                                                                   \
    static const struct vfx_battery_cfg VFX_CFG_SYM(node) = {                                      \
        .low_color = DT_PROP(node, low_color),                                                     \
        .high_color = DT_PROP(node, high_color),                                                   \
        .empty_color = DT_PROP(node, empty_color),                                                 \
        .warn_below = DT_PROP(node, warn_below),                                                   \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_battery_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_battery, VFX_BATTERY_DEFINE)

#define VFX_BLE_PROFILE_DEFINE(node)                                                               \
    static const struct vfx_ble_profile_cfg VFX_CFG_SYM(node) = {                                  \
        .connected_color = DT_PROP(node, connected_color),                                         \
        .disconnected_color = DT_PROP(node, disconnected_color),                                   \
        .usb_color = DT_PROP(node, usb_color),                                                     \
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_ble_profile_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_ble_profile, VFX_BLE_PROFILE_DEFINE)


/* ----------------------------------------------------------------- scenes */

#define VFX_LAYER_ENTRY(node)                                                                      \
    {                                                                                              \
        .api = VFX_API_SYM(node),                                                                  \
        .zone = &VFX_ZONE_SYM(DT_PHANDLE(node, zone)),                                             \
        .config = &VFX_CFG_SYM(node),                                                              \
        .state = &VFX_STATE_SYM(node),                                                             \
        .blend = DT_PROP(node, blend),                                                             \
        .opacity = DT_PROP(node, opacity),                                                         \
    },

#define VFX_SCENE_DEFINE(node)                                                                     \
    static const struct vfx_layer VFX_LAYERS_SYM(node)[] = {                                       \
        DT_FOREACH_CHILD_STATUS_OKAY(node, VFX_LAYER_ENTRY)};                                      \
    static const struct vfx_scene VFX_SCENE_SYM(node) = {                                          \
        .name = DT_PROP_OR(node, display_name, DT_NODE_FULL_NAME(node)),                           \
        .layers = VFX_LAYERS_SYM(node),                                                            \
        .num_layers = ARRAY_SIZE(VFX_LAYERS_SYM(node)),                                            \
    };

#if !DT_HAS_COMPAT_STATUS_OKAY(zmk_vfx_scene)
#error "ZMK VFX is enabled but no zmk,vfx-scene node exists. Include <vfx/presets.dtsi> or declare a scene in your keymap."
#endif

DT_FOREACH_STATUS_OKAY(zmk_vfx_scene, VFX_SCENE_DEFINE)

#define VFX_SCENE_REF(node) &VFX_SCENE_SYM(node),

static const struct vfx_scene *const vfx_scene_list[] = {
    DT_FOREACH_STATUS_OKAY(zmk_vfx_scene, VFX_SCENE_REF)};

uint8_t vfx_scene_count(void) { return (uint8_t)ARRAY_SIZE(vfx_scene_list); }

const struct vfx_scene *vfx_scene_get(uint8_t index) {
    if (index >= ARRAY_SIZE(vfx_scene_list)) {
        return NULL;
    }

    return vfx_scene_list[index];
}

#if DT_NODE_HAS_PROP(VFX_ENGINE_NODE, layer_scenes)
#define VFX_LAYER_SCENE_REF(node, prop, idx) &VFX_SCENE_SYM(DT_PHANDLE_BY_IDX(node, prop, idx)),

static const struct vfx_scene *const vfx_layer_scene_list[] = {
    DT_FOREACH_PROP_ELEM(VFX_ENGINE_NODE, layer_scenes, VFX_LAYER_SCENE_REF)};
#endif

int16_t vfx_layer_scene_index(uint8_t layer, uint8_t ch) {
#if DT_NODE_HAS_PROP(VFX_ENGINE_NODE, layer_scenes)
    if (layer >= ARRAY_SIZE(vfx_layer_scene_list)) {
        /* Past the end of the list means this layer has no scene of its own,
         * which is how you map only the layers you care about.
         */
        return -1;
    }

    const struct vfx_scene *want = vfx_layer_scene_list[layer];
    const struct vfx_channel *chan = vfx_channel_get(ch);

    if (!chan) {
        return -1;
    }

    /* Layer scenes are named rather than numbered, so a channel follows the
     * layer only if it actually carries that scene. Naming a reactive scene
     * moves the keys and leaves an underglow channel that has never heard of
     * it alone, which is the useful reading.
     */
    for (uint8_t i = 0; i < chan->num_scenes; i++) {
        if (chan->scenes[i] == want) {
            return (int16_t)i;
        }
    }
#else
    (void)layer;
    (void)ch;
#endif

    return -1;
}

uint8_t vfx_scene_default_index(void) {
#if DT_NODE_HAS_PROP(VFX_ENGINE_NODE, default_scene)
    const struct vfx_scene *want = &VFX_SCENE_SYM(DT_PHANDLE(VFX_ENGINE_NODE, default_scene));

    for (uint8_t i = 0; i < ARRAY_SIZE(vfx_scene_list); i++) {
        if (vfx_scene_list[i] == want) {
            return i;
        }
    }
#endif

    return 0;
}

/* --------------------------------------------------------------- channels */

#define VFX_CH_SCENES_SYM(node) _CONCAT(vfx_ch_scenes_, node)

#define VFX_CH_SCENE_REF(node, prop, idx) &VFX_SCENE_SYM(DT_PHANDLE_BY_IDX(node, prop, idx)),

#define VFX_CH_DEFAULT_REF(node)                                                                   \
    COND_CODE_1(DT_NODE_HAS_PROP(node, default_scene),                                             \
                (&VFX_SCENE_SYM(DT_PHANDLE(node, default_scene))), (NULL))

#if DT_HAS_COMPAT_STATUS_OKAY(zmk_vfx_channel)

#define VFX_CHANNEL_SCENES(node)                                                                   \
    static const struct vfx_scene *const VFX_CH_SCENES_SYM(node)[] = {                             \
        DT_FOREACH_PROP_ELEM(node, scenes, VFX_CH_SCENE_REF)};

DT_FOREACH_STATUS_OKAY(zmk_vfx_channel, VFX_CHANNEL_SCENES)

#define VFX_CHANNEL_ENTRY(node)                                                                    \
    {                                                                                              \
        .start = DT_PROP_BY_IDX(node, range, 0),                                                   \
        .len = DT_PROP_BY_IDX(node, range, 1),                                                     \
        .scenes = VFX_CH_SCENES_SYM(node),                                                         \
        .num_scenes = (uint8_t)ARRAY_SIZE(VFX_CH_SCENES_SYM(node)),                                \
        .default_scene = VFX_CH_DEFAULT_REF(node),                                                 \
    },

static const struct vfx_channel vfx_channel_list[] = {
    DT_FOREACH_STATUS_OKAY(zmk_vfx_channel, VFX_CHANNEL_ENTRY)};

#else

/* Nothing declared, so the whole strip is one channel showing every scene.
 * The length is clamped to the chain at render time, the same way a zone is,
 * so 255 just means "however many pixels this board has".
 */
static const struct vfx_channel vfx_channel_list[] = {{
    .start = 0,
    .len = 255,
    .scenes = vfx_scene_list,
    .num_scenes = (uint8_t)ARRAY_SIZE(vfx_scene_list),
    .default_scene = VFX_CH_DEFAULT_REF(VFX_ENGINE_NODE),
}};

#endif

uint8_t vfx_channel_count(void) { return (uint8_t)ARRAY_SIZE(vfx_channel_list); }

const struct vfx_channel *vfx_channel_get(uint8_t index) {
    if (index >= ARRAY_SIZE(vfx_channel_list)) {
        return NULL;
    }

    return &vfx_channel_list[index];
}

uint8_t vfx_channel_default_index(uint8_t index) {
    const struct vfx_channel *ch = vfx_channel_get(index);

    if (!ch || !ch->default_scene) {
        return 0;
    }

    for (uint8_t i = 0; i < ch->num_scenes; i++) {
        if (ch->scenes[i] == ch->default_scene) {
            return i;
        }
    }

    return 0;
}
