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
#define VFX_LAYERS_SYM(node) _CONCAT(vfx_layers_, node)
#define VFX_SCENE_SYM(node) _CONCAT(vfx_scene_, node)

/* ------------------------------------------------------------------ zones */

#define VFX_ZONE_PIXELS(node)                                                                      \
    COND_CODE_1(DT_NODE_HAS_PROP(node, pixels),                                                    \
                (static const uint8_t VFX_ZONE_PX_SYM(node)[] = DT_PROP(node, pixels);), ())

#define VFX_ZONE_DEFINE(node)                                                                      \
    VFX_ZONE_PIXELS(node)                                                                          \
    static const struct vfx_zone VFX_ZONE_SYM(node) = {                                            \
        .pixels = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (VFX_ZONE_PX_SYM(node)), (NULL)),    \
        .start = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (0),                                  \
                             (DT_PROP_BY_IDX(node, range, 0))),                                    \
        .len = COND_CODE_1(DT_NODE_HAS_PROP(node, pixels), (DT_PROP_LEN(node, pixels)),            \
                           (DT_PROP_BY_IDX(node, range, 1))),                                      \
    };

DT_FOREACH_STATUS_OKAY(zmk_vfx_zone, VFX_ZONE_DEFINE)

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
    };                                                                                             \
    static struct vfx_gradient_state VFX_STATE_SYM(node);                                          \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_gradient_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_gradient, VFX_GRADIENT_DEFINE)


#define VFX_BREATHE_DEFINE(node)                                                                   \
    static const struct vfx_breathe_cfg VFX_CFG_SYM(node) = {                                      \
        .color = DT_PROP(node, color),                                                             \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .min_level = DT_PROP(node, min_level),                                                      \
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
    };                                                                                             \
    static uint8_t VFX_STATE_SYM(node);                                                             \
    static const struct vfx_layer_api *const VFX_API_SYM(node) = &vfx_layer_wave_api;

DT_FOREACH_STATUS_OKAY(zmk_vfx_layer_wave, VFX_WAVE_DEFINE)

#define VFX_TWINKLE_DEFINE(node)                                                                   \
    static const struct vfx_twinkle_cfg VFX_CFG_SYM(node) = {                                      \
        .color = DT_PROP(node, color),                                                             \
        .period_ms = DT_PROP(node, period_ms),                                                     \
        .density = DT_PROP(node, density),                                                         \
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
