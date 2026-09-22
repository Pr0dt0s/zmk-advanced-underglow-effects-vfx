/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <raw_hid/events.h>

#include <errno.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/event_manager.h>
#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/tuning.h>
#include <zmk/vfx/vfx.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Glue between zzeneg/zmk-raw-hid's transport and the tuning API, over the
 * wire format in hid_protocol.h. This is deliberately thin: everything that
 * decides what a byte means lives in hid_protocol.c, and everything that
 * decides what a slot does lives in tuning.c behind zmk_vfx_tune_*(). This
 * file's only job is to move a report to a call and a result back to a
 * report.
 *
 * Enabled by CONFIG_ZMK_VFX_RAW_HID, which the raw-hid module and its
 * raw_hid_adapter shield have to be present for -- see the README.
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
