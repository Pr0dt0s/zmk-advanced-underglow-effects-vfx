/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/layer.h>

/* Render one frame of a scene.
 *
 * This is the whole compositor, and it is deliberately free of kernel and
 * driver calls so the same object code can be built for the firmware and for
 * the WebAssembly simulator. Anything that touches Zephyr lives in engine.c
 * or power.c, on the other side of this call.
 *
 * out must hold ctx->num_pixels entries. any_lit, if given, is set when the
 * finished frame contains a non-zero channel, which is what the power gate
 * keys off; computing it here keeps it free of an extra pass.
 */
void vfx_render_frame(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx,
                      struct vfx_rgb *out, bool *any_lit);

/* True if any layer in the scene still has something to animate. */
bool vfx_scene_is_animating(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx);

/* Fan a key event out to every reactive layer in the scene. */
void vfx_scene_key_event(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx,
                         uint32_t position, bool pressed, uint32_t time_ms);

/* Generator vtables, referenced by the devicetree instantiation macros. */
extern const struct vfx_layer_api vfx_layer_solid_api;
extern const struct vfx_layer_api vfx_layer_gradient_api;
extern const struct vfx_layer_api vfx_layer_breathe_api;
extern const struct vfx_layer_api vfx_layer_wave_api;
extern const struct vfx_layer_api vfx_layer_twinkle_api;
extern const struct vfx_layer_api vfx_layer_plasma_api;
extern const struct vfx_layer_api vfx_layer_ripple_api;
extern const struct vfx_layer_api vfx_layer_water_api;
extern const struct vfx_layer_api vfx_layer_matrix_api;
extern const struct vfx_layer_api vfx_layer_cross_api;
extern const struct vfx_layer_api vfx_layer_fire_api;
extern const struct vfx_layer_api vfx_layer_comet_api;
extern const struct vfx_layer_api vfx_layer_flag_api;
extern const struct vfx_layer_api vfx_layer_wpm_api;
extern const struct vfx_layer_api vfx_layer_peripheral_battery_api;
extern const struct vfx_layer_api vfx_layer_keyflash_api;
extern const struct vfx_layer_api vfx_layer_trail_api;
extern const struct vfx_layer_api vfx_layer_layer_state_api;
extern const struct vfx_layer_api vfx_layer_battery_api;
extern const struct vfx_layer_api vfx_layer_ble_profile_api;
