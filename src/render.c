/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/engine.h>

/* The compositor.
 *
 * No kernel calls, no driver calls, no allocation: everything Zephyr shaped
 * lives in engine.c on the other side of vfx_render_frame(). That boundary is
 * what lets the simulator compile this exact file to WebAssembly and get
 * frames identical to the ones the keyboard produces.
 */
void vfx_render_frame(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx,
                      struct vfx_rgb *out, bool *any_lit) {
    for (uint16_t i = 0; i < ctx->num_pixels; i++) {
        out[i] = VFX_RGB_BLACK;
    }

    if (scene) {
        for (uint8_t l = 0; l < scene->num_layers; l++) {
            const struct vfx_layer *layer = &scene->layers[l];

            if (layer->opacity == 0) {
                continue;
            }

            if (layer->api->frame) {
                layer->api->frame(layer, ctx);
            }

            const uint16_t len = vfx_zone_clamped_len(layer->zone, ctx->num_pixels);

            for (uint16_t z = 0; z < len; z++) {
                const uint16_t p = vfx_zone_pixel(layer->zone, z);

                if (p >= ctx->num_pixels) {
                    continue; /* explicit pixel list overshooting a short strip */
                }

                struct vfx_rgb src;

                /* A layer declining a pixel leaves what is underneath alone.
                 * Returning black instead would be wrong for every blend mode
                 * except NORMAL, and wasteful for that one.
                 */
                if (!layer->api->pixel(layer, ctx, z, p, &src)) {
                    continue;
                }

                out[p] = vfx_blend(out[p], src, layer->blend, layer->opacity);
            }
        }
    }

    bool lit = false;

    for (uint16_t i = 0; i < ctx->num_pixels; i++) {
        out[i] = vfx_rgb_scale(out[i], ctx->brightness);

        out[i].r = vfx_gamma(out[i].r);
        out[i].g = vfx_gamma(out[i].g);
        out[i].b = vfx_gamma(out[i].b);

        /* Folded into the write loop so the power gate costs no extra pass. */
        lit |= (out[i].r | out[i].g | out[i].b) != 0;
    }

    if (any_lit) {
        *any_lit = lit;
    }
}

void vfx_render_transition(const struct vfx_scene *from, const struct vfx_scene *to,
                           const struct vfx_frame_ctx *ctx, uint8_t t, struct vfx_rgb *out,
                           struct vfx_rgb *scratch, bool *any_lit) {
    bool lit_to = false, lit_from = false;

    vfx_render_frame(to, ctx, out, &lit_to);
    vfx_render_frame(from, ctx, scratch, &lit_from);

    /* Mixed after gamma rather than before it. A crossfade is a statement
     * about what the eye sees, and these are already the values the eye is
     * going to get; correcting them a second time would bend the fade.
     */
    for (uint16_t i = 0; i < ctx->num_pixels; i++) {
        out[i].r = (uint8_t)(scratch[i].r + (((int16_t)out[i].r - scratch[i].r) * t) / 255);
        out[i].g = (uint8_t)(scratch[i].g + (((int16_t)out[i].g - scratch[i].g) * t) / 255);
        out[i].b = (uint8_t)(scratch[i].b + (((int16_t)out[i].b - scratch[i].b) * t) / 255);
    }

    if (any_lit) {
        /* Either end being lit is enough: a fade from a lit scene to a black
         * one must keep the rail up until it has actually finished.
         */
        *any_lit = lit_to || lit_from;
    }
}

bool vfx_scene_is_animating(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx) {
    if (!scene) {
        return false;
    }

    for (uint8_t l = 0; l < scene->num_layers; l++) {
        const struct vfx_layer *layer = &scene->layers[l];

        /* No hook means the generator makes no promises, so assume it moves. */
        if (!layer->api->is_animating || layer->api->is_animating(layer, ctx)) {
            return true;
        }
    }

    return false;
}

void vfx_scene_key_event(const struct vfx_scene *scene, const struct vfx_frame_ctx *ctx,
                         uint32_t position, bool pressed, uint32_t time_ms) {
    if (!scene) {
        return;
    }

    for (uint8_t l = 0; l < scene->num_layers; l++) {
        const struct vfx_layer *layer = &scene->layers[l];

        if (layer->api->key_event) {
            layer->api->key_event(layer, ctx, position, pressed, time_ms);
        }
    }
}
