/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/vfx.h>
#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/runtime_scene.h>
#include <zmk/vfx/split_link.h>
#include <zmk/vfx/vfx.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Runtime scenes are built over raw-hid, which only builds on a split's
 * central, so a scene built from the Host control panel would only ever show on
 * the half that is plugged in. This hands the same wire bytes hid_transport.c
 * already decoded to the split link (split_link.h), which chunks, paces and
 * reassembles them, and applies what comes out of the other end here.
 *
 * Independent of CONFIG_ZMK_VFX_SPLIT_SYNCED on purpose: that is about whether
 * the halves' animation clocks agree, not whether a runtime scene reaches both.
 */

int zmk_vfx_scene_relay_send(const uint8_t *data, uint8_t len) {
    return split_link_send(data, len);
}

bool zmk_vfx_scene_relay_room(uint8_t len) { return split_link_room(len); }

static void apply_relayed_request(const struct vfx_hid_request *req) {
    switch ((enum vfx_hid_op)req->op) {
    case VFX_HID_OP_SCENE_RESET:
        zmk_vfx_scene_reset(req->ch);
        break;

    case VFX_HID_OP_SCENE_ADD_LAYER: {
        struct vfx_rt_params params = {
            .type = req->type,
            .zone_start = req->zone_start,
            .zone_len = req->zone_len,
            .blend = req->blend,
            .opacity = req->opacity,
            .hue = (uint16_t)req->hue,
            .sat = req->sat,
            .bri = req->bri,
            .flags = req->flags,
        };
        uint8_t slot = VFX_HID_NO_SLOT;

        memcpy(params.args, req->args, sizeof(req->args));
        if (req->staged) {
            zmk_vfx_scene_add_layer_staged(req->ch, &params, &slot);
        } else {
            zmk_vfx_scene_add_layer(req->ch, &params, &slot);
        }
        break;
    }

    case VFX_HID_OP_SCENE_SET_ARG:
        zmk_vfx_scene_set_arg(req->ch, req->slot, req->arg_idx, req->args[0]);
        break;

    case VFX_HID_OP_SCENE_SET_COLOR:
        zmk_vfx_scene_set_color(req->ch, req->slot, (uint16_t)req->hue, req->sat, req->bri);
        break;

    case VFX_HID_OP_SCENE_REMOVE_LAYER:
        zmk_vfx_scene_remove_layer(req->ch, req->slot);
        break;

    case VFX_HID_OP_SCENE_MOVE_LAYER:
        zmk_vfx_scene_move_layer(req->ch, req->slot, req->direction);
        break;

    case VFX_HID_OP_SCENE_ACTIVATE:
        zmk_vfx_scene_activate(req->ch);
        break;

    case VFX_HID_OP_SCENE_DEACTIVATE:
        zmk_vfx_scene_deactivate(req->ch);
        break;

    case VFX_HID_OP_SCENE_GRADIENT_ADD_STOP:
        zmk_vfx_scene_gradient_add_stop(req->ch, req->slot, (uint16_t)req->hue, req->sat,
                                        req->bri);
        break;

    case VFX_HID_OP_SCENE_SET_LIST_COLOR:
        zmk_vfx_scene_set_list_color(req->ch, req->slot, req->arg_idx, (uint16_t)req->hue,
                                     req->sat, req->bri);
        break;

    case VFX_HID_OP_SCENE_SET_ZONE:
        zmk_vfx_scene_set_zone(req->ch, req->slot, req->zone_kind, req->zone_offset,
                               req->zone_data, req->zone_count);
        break;

    case VFX_HID_OP_SCENE_SET_OPTS:
        zmk_vfx_scene_set_opts(req->ch, req->slot, req->blend, req->opacity, req->opacity_src,
                               req->opacity_min, req->opacity_full, req->tune_id);
        break;

    case VFX_HID_OP_SCENE_SET_FLAGS:
        zmk_vfx_scene_set_flags(req->ch, req->slot, req->flags);
        break;

    case VFX_HID_OP_SCENE_COMMIT_LAYER:
        zmk_vfx_scene_commit_layer(req->ch, req->slot, req->position);
        break;

    default:
        break; /* a GET_* or PING is never relayed */
    }
}

static void on_message(const uint8_t *msg, uint8_t len) {
    struct vfx_hid_request req;

    if (vfx_hid_decode(msg, len, &req)) {
        apply_relayed_request(&req);
    } else {
        LOG_WRN("Malformed relayed VFX scene op (%d bytes)", len);
    }
}

static int scene_relay_init(void) {
    split_link_set_receive_cb(on_message);

    return 0;
}

SYS_INIT(scene_relay_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

void zmk_vfx_scene_relay_receive(uint32_t param1, uint32_t param2) {
    split_link_receive(param1, param2);
}
