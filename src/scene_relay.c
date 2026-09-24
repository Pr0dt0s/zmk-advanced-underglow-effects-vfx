/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/vfx.h>
#include <zmk/behavior.h>
#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/runtime_scene.h>
#include <zmk/vfx/vfx.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Runtime scenes are built over raw-hid, which only builds on a split's
 * central (see hid_transport.c and the README) -- there is no raw-hid
 * connection on a peripheral to hand a host's edits to directly. Left
 * there, a scene built from the Host control panel would only ever show on
 * whichever half happens to be plugged in.
 *
 * This relays the same raw wire bytes hid_transport.c already decoded once,
 * over the &vfx behavior's own relay -- the mechanism split_sync.c's beacon
 * and key relay already use, and for the same reason given there: ZMK
 * relays a behavior invocation to every peripheral on its own, so this
 * needs no GATT service of its own either. VFX_RT_RELAY_CMD's own comment
 * has the wire shape; vfx_relay_pack()/vfx_relay_unpack() (hid_protocol.h)
 * do the actual byte packing and are exercised without Zephyr the same way
 * the rest of that file is -- this file is just what calls them on each
 * side of the link, and applies what comes out the other end.
 *
 * Deliberately not using event.position as a third payload word, the way an
 * earlier version of this file did: ZMK's own split transport narrows it to
 * a single byte before it ever reaches the peripheral (see
 * VFX_RELAY_CHUNK_BYTES's own comment), so only param1 and param2 are used.
 *
 * Independent of CONFIG_ZMK_VFX_SPLIT_SYNCED on purpose: that choice is
 * about whether the two halves' animation clocks agree, which has nothing
 * to do with whether a runtime-built scene reaches both of them. It is
 * gated on CONFIG_ZMK_VFX_RUNTIME_SCENES instead (see CMakeLists.txt),
 * since there is nothing to relay, or receive into, without it.
 */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static void send_chunk(uint32_t param1, uint32_t param2) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "vfx",
        .param1 = param1,
        .param2 = param2,
    };
    struct zmk_behavior_binding_event event = {
        .timestamp = k_uptime_get(),
    };

    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        zmk_split_central_invoke_behavior(i, &binding, event, true);
    }
}

void zmk_vfx_scene_relay_send(const uint8_t *data, uint8_t len) {
    if (len == 0 || len > VFX_RELAY_MAX_BYTES) {
        return;
    }

    uint8_t sent = 0;
    uint8_t chunk_index = 0;

    while (sent < len) {
        const uint8_t chunk_len = MIN(VFX_RELAY_CHUNK_BYTES, (uint8_t)(len - sent));
        uint32_t param1;
        uint32_t param2;

        vfx_relay_pack(VFX_RT_RELAY_CMD, chunk_index, len, &data[sent], chunk_len, &param1,
                       &param2);
        send_chunk(param1, param2);

        sent = (uint8_t)(sent + chunk_len);
        chunk_index++;
    }
}

#else

void zmk_vfx_scene_relay_send(const uint8_t *data, uint8_t len) {
    ARG_UNUSED(data);
    ARG_UNUSED(len);
}

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

/* Receiving side: reassembles what zmk_vfx_scene_relay_send() above sent,
 * then makes the same zmk_vfx_scene_*() call the central already made.
 * Fire-and-forget -- nothing here answers a host, because nothing on a
 * peripheral is listening for one.
 */
static uint8_t relay_buf[VFX_RELAY_MAX_BYTES];
static uint8_t relay_have;
static uint8_t relay_total;

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

        memcpy(params.args, req->args, sizeof(params.args));
        zmk_vfx_scene_add_layer(req->ch, &params, &slot);
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

    default:
        break; /* a GET_* or PING is never relayed */
    }
}

void zmk_vfx_scene_relay_receive(uint32_t param1, uint32_t param2) {
    if (!vfx_relay_unpack(param1, param2, relay_buf, &relay_have, &relay_total)) {
        return;
    }

    struct vfx_hid_request req;

    if (vfx_hid_decode(relay_buf, relay_total, &req)) {
        apply_relayed_request(&req);
    } else {
        LOG_WRN("Malformed relayed VFX scene op (%d bytes)", relay_total);
    }
}
