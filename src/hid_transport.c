/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <raw_hid/events.h>

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/tuning.h>
#include <zmk/vfx/vfx.h>

#if IS_ENABLED(CONFIG_ZMK_VFX_RUNTIME_SCENES)
#include <zmk/vfx/runtime_scene.h>
#endif

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Glue between zzeneg/zmk-raw-hid's transport and the tuning and
 * runtime-scene APIs, over the wire format in hid_protocol.h. This is
 * deliberately thin: everything that decides what a byte means lives in
 * hid_protocol.c, and everything that decides what a slot or a layer does
 * lives behind zmk_vfx_tune_*() and zmk_vfx_scene_*(). This file's only job
 * is to move a report to a call and a result back to a report.
 *
 * Enabled by CONFIG_ZMK_VFX_RAW_HID, which the raw-hid module and its
 * raw_hid_adapter shield have to be present for -- see the README. Scene
 * ops additionally need CONFIG_ZMK_VFX_RUNTIME_SCENES.
 */

static void send(const uint8_t *data, uint8_t len) {
    /* raw_hid_sent_event does not copy: every transport listening for it
     * reads .data synchronously before this call returns (usb_hid.c copies
     * into its own static report buffer, hog.c hands the pointer straight
     * to bt_gatt_notify_cb, which serialises the ATT payload before
     * returning), so a stack buffer here is safe.
     */
    raise_raw_hid_sent_event((struct raw_hid_sent_event){
        .data = (uint8_t *)data,
        .length = len,
    });
}

static void reply_state(uint8_t slot) {
    const struct vfx_tuning *t = vfx_tuning_get(slot);
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    uint8_t len;

    if (t) {
        len = vfx_hid_encode_state(slot, t->hue, t->level, t->speed, VFX_HID_STATUS_OK, buf);
    } else {
        len = vfx_hid_encode_state(slot, 0, 0, 0, VFX_HID_STATUS_BAD_SLOT, buf);
    }

    send(buf, len);
}

static void reply_ack(uint8_t op, uint8_t slot, int rc) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    /* zmk_vfx_tune_*() only ever fails this way with -EINVAL, for a slot
     * outside 1..VFX_TUNE_SLOTS-1; a save-state failure behind a successful
     * tune is not this protocol's business to surface as the tune itself
     * having failed.
     */
    const uint8_t status = rc == -EINVAL ? VFX_HID_STATUS_BAD_SLOT : VFX_HID_STATUS_OK;
    const uint8_t len = vfx_hid_encode_ack(op, slot, status, buf);

    send(buf, len);
}

#if IS_ENABLED(CONFIG_ZMK_VFX_RUNTIME_SCENES)
/* zmk_vfx_scene_*() only ever fails with -EINVAL (a bad channel, slot, type,
 * argument index or move) or, from add_layer alone, -ENOSPC (the channel's
 * pool is already full) -- everything else maps to BAD_SLOT the same way
 * reply_ack()'s tuning callers already do.
 */
static uint8_t scene_status(int rc) {
    if (rc == 0) {
        return VFX_HID_STATUS_OK;
    }

    return rc == -ENOSPC ? VFX_HID_STATUS_POOL_FULL : VFX_HID_STATUS_BAD_SLOT;
}

static void reply_scene_ack(uint8_t op, uint8_t slot, int rc) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_ack(op, slot, scene_status(rc), buf);

    send(buf, len);
}

static void reply_scene_info(uint8_t ch) {
    uint8_t count = 0;
    bool active = false;
    const int rc = zmk_vfx_scene_info(ch, &count, &active);
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_scene_info(ch, count, active, scene_status(rc), buf);

    send(buf, len);
}

static void reply_scene_layer(uint8_t ch, uint8_t slot) {
    struct vfx_rt_params p = {0};
    const int rc = zmk_vfx_scene_get_layer(ch, slot, &p);
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_scene_layer(ch, slot, p.type, p.zone_start, p.zone_len,
                                                   p.blend, p.opacity, (int16_t)p.hue, p.sat,
                                                   p.bri, p.args, p.flags, scene_status(rc), buf);

    send(buf, len);
}
#endif

static void handle(const struct vfx_hid_request *req) {
    switch ((enum vfx_hid_op)req->op) {
    case VFX_HID_OP_PING: {
        uint8_t buf[VFX_HID_MAX_REPLY_LEN];
        const uint8_t len = vfx_hid_encode_pong(VFX_TUNE_SLOTS - 1, buf);

        send(buf, len);
        break;
    }

    case VFX_HID_OP_SET_HUE:
        reply_ack(req->op, req->slot, zmk_vfx_tune_hue(req->slot, req->hue));
        break;

    case VFX_HID_OP_SET_LEVEL:
        reply_ack(req->op, req->slot, zmk_vfx_tune_level(req->slot, req->level));
        break;

    case VFX_HID_OP_SET_SPEED:
        reply_ack(req->op, req->slot, zmk_vfx_tune_speed(req->slot, req->speed));
        break;

    case VFX_HID_OP_RESET:
        reply_ack(req->op, req->slot, zmk_vfx_tune_reset(req->slot));
        break;

    case VFX_HID_OP_GET:
        reply_state(req->slot);
        break;

    case VFX_HID_OP_GET_ALL:
        /* Slot 0 is reserved for "untuned" and is never itself addressable,
         * same boundary zmk_vfx_tune_*() enforces on every write above.
         */
        for (uint8_t slot = 1; slot < VFX_TUNE_SLOTS; slot++) {
            reply_state(slot);
        }
        break;

#if IS_ENABLED(CONFIG_ZMK_VFX_RUNTIME_SCENES)
    case VFX_HID_OP_SCENE_RESET:
        reply_scene_ack(req->op, req->ch, zmk_vfx_scene_reset(req->ch));
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

        memcpy(params.args, req->args, sizeof(params.args));

        uint8_t slot = VFX_HID_NO_SLOT;
        const int rc = zmk_vfx_scene_add_layer(req->ch, &params, &slot);

        reply_scene_ack(req->op, slot, rc);
        break;
    }

    case VFX_HID_OP_SCENE_SET_ARG:
        reply_scene_ack(req->op, req->slot,
                        zmk_vfx_scene_set_arg(req->ch, req->slot, req->arg_idx, req->args[0]));
        break;

    case VFX_HID_OP_SCENE_SET_COLOR:
        reply_scene_ack(req->op, req->slot,
                        zmk_vfx_scene_set_color(req->ch, req->slot, (uint16_t)req->hue, req->sat,
                                                req->bri));
        break;

    case VFX_HID_OP_SCENE_REMOVE_LAYER:
        reply_scene_ack(req->op, req->slot, zmk_vfx_scene_remove_layer(req->ch, req->slot));
        break;

    case VFX_HID_OP_SCENE_MOVE_LAYER:
        reply_scene_ack(req->op, req->slot,
                        zmk_vfx_scene_move_layer(req->ch, req->slot, req->direction));
        break;

    case VFX_HID_OP_SCENE_ACTIVATE:
        reply_scene_ack(req->op, req->ch, zmk_vfx_scene_activate(req->ch));
        break;

    case VFX_HID_OP_SCENE_DEACTIVATE:
        reply_scene_ack(req->op, req->ch, zmk_vfx_scene_deactivate(req->ch));
        break;

    case VFX_HID_OP_SCENE_GET_INFO:
        reply_scene_info(req->ch);
        break;

    case VFX_HID_OP_SCENE_GET_LAYER:
        reply_scene_layer(req->ch, req->slot);
        break;
#else
    /* CONFIG_ZMK_VFX_RUNTIME_SCENES is off: hid_protocol.c decodes these
     * fine regardless (see payload_len()), but there is nothing here to
     * dispatch them to. Every case has to be named once somewhere in this
     * switch or an enum value goes unhandled, so they land here, told
     * plainly rather than left waiting on a reply that will never come.
     */
    case VFX_HID_OP_SCENE_RESET:
    case VFX_HID_OP_SCENE_ADD_LAYER:
    case VFX_HID_OP_SCENE_SET_ARG:
    case VFX_HID_OP_SCENE_SET_COLOR:
    case VFX_HID_OP_SCENE_REMOVE_LAYER:
    case VFX_HID_OP_SCENE_MOVE_LAYER:
    case VFX_HID_OP_SCENE_ACTIVATE:
    case VFX_HID_OP_SCENE_DEACTIVATE:
    case VFX_HID_OP_SCENE_GET_INFO:
    case VFX_HID_OP_SCENE_GET_LAYER:
        reply_ack(req->op, req->ch, -EINVAL);
        break;
#endif
    }
}

static int raw_hid_received_listener(const zmk_event_t *eh) {
    const struct raw_hid_received_event *event = as_raw_hid_received_event(eh);

    if (event) {
        struct vfx_hid_request req;

        if (vfx_hid_decode(event->data, event->length, &req)) {
            handle(&req);
        } else {
            LOG_WRN("Malformed VFX raw-hid report (%d bytes)", event->length);
        }
    }

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_vfx_hid, raw_hid_received_listener);
ZMK_SUBSCRIPTION(zmk_vfx_hid, raw_hid_received_event);
