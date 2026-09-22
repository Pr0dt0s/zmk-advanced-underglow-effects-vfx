/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/hid_protocol.h>

/* How many bytes past the op a request of this shape needs. Anything
 * shorter is a truncated report, not a request with defaults.
 */
static uint8_t payload_len(enum vfx_hid_op op) {
    switch (op) {
    case VFX_HID_OP_PING:
    case VFX_HID_OP_GET_ALL:
        return 0;
    case VFX_HID_OP_RESET:
    case VFX_HID_OP_GET:
        return 1; /* slot */
    case VFX_HID_OP_SET_LEVEL:
    case VFX_HID_OP_SET_SPEED:
        return 2; /* slot, value */
    case VFX_HID_OP_SET_HUE:
        return 3; /* slot, hue lo, hue hi */
    default:
        return 0xFF; /* unreachable for a real op; makes an unknown one fail the length check */
    }
}

bool vfx_hid_decode(const uint8_t *data, uint8_t len, struct vfx_hid_request *out) {
    if (len < 1) {
        return false;
    }

    const enum vfx_hid_op op = (enum vfx_hid_op)data[0];
    const uint8_t need = payload_len(op);

    if (need == 0xFF || len < (uint8_t)(1 + need)) {
        return false;
    }

    *out = (struct vfx_hid_request){.op = (uint8_t)op};

    switch (op) {
    case VFX_HID_OP_PING:
    case VFX_HID_OP_GET_ALL:
        break;

    case VFX_HID_OP_RESET:
    case VFX_HID_OP_GET:
        out->slot = data[1];
        break;

    case VFX_HID_OP_SET_LEVEL:
        out->slot = data[1];
        out->level = data[2];
        break;

    case VFX_HID_OP_SET_SPEED:
        out->slot = data[1];
        out->speed = data[2];
        break;

    case VFX_HID_OP_SET_HUE:
        out->slot = data[1];
        /* Little-endian, matching every other multi-byte field this
         * protocol and the sim's own scratch encoding use.
         */
        out->hue = (int16_t)((uint16_t)data[2] | ((uint16_t)data[3] << 8));
        break;
    }

    return true;
}

uint8_t vfx_hid_encode_pong(uint8_t max_slot, uint8_t *out) {
    out[0] = VFX_HID_REPLY_PONG;
    out[1] = VFX_HID_PROTOCOL_VERSION;
    out[2] = max_slot;

    return 3;
}

uint8_t vfx_hid_encode_ack(uint8_t op, uint8_t slot, uint8_t status, uint8_t *out) {
    out[0] = (uint8_t)(op | VFX_HID_REPLY_BIT);
    out[1] = slot;
    out[2] = status;

    return 3;
}

uint8_t vfx_hid_encode_state(uint8_t slot, int16_t hue, uint8_t level, uint8_t speed,
                             uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_STATE;
    out[1] = slot;
    out[2] = (uint8_t)((uint16_t)hue & 0xFF);
    out[3] = (uint8_t)(((uint16_t)hue >> 8) & 0xFF);
    out[4] = level;
    out[5] = speed;
    out[6] = status;

    return 7;
}
