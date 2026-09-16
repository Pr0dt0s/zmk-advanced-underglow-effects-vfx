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

DT_FOREACH_STATUS_OKAY(zmk_vfx_scene, VFX_SCENE_DEFINE)

#define VFX_SCENE_REF(node) &VFX_SCENE_SYM(node),

static const struct vfx_scene *const vfx_scene_list[] = {
    DT_FOREACH_STATUS_OKAY(zmk_vfx_scene, VFX_SCENE_REF)};

BUILD_ASSERT(ARRAY_SIZE(vfx_scene_list) > 0,
             "ZMK VFX is enabled but no zmk,vfx-scene node is defined. Include "
             "<vfx/presets.dtsi> or declare a scene in your keymap.");

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
