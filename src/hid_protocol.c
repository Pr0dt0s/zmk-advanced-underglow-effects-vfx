/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/hid_protocol.h>

/* Little-endian throughout, matching every other multi-byte field this
 * protocol and the sim's own scratch encoding use.
 */
static int16_t read_i16(const uint8_t *p) {
    return (int16_t)((uint16_t)p[0] | ((uint16_t)p[1] << 8));
}

static void write_i16(uint8_t *p, int16_t v) {
    p[0] = (uint8_t)((uint16_t)v & 0xFF);
    p[1] = (uint8_t)(((uint16_t)v >> 8) & 0xFF);
}

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

    case VFX_HID_OP_SCENE_ACTIVATE:
    case VFX_HID_OP_SCENE_DEACTIVATE:
    case VFX_HID_OP_SCENE_GET_INFO:
    case VFX_HID_OP_SCENE_GET_ORDER:
    case VFX_HID_OP_SCENE_RESET:
        return 1; /* ch */
    case VFX_HID_OP_SCENE_REMOVE_LAYER:
    case VFX_HID_OP_SCENE_GET_LAYER:
        return 2; /* ch, slot */
    case VFX_HID_OP_SCENE_MOVE_LAYER:
        return 3; /* ch, slot, direction */
    case VFX_HID_OP_SCENE_SET_ARG:
        return 5; /* ch, slot, arg index, value lo, value hi */
    case VFX_HID_OP_SCENE_SET_COLOR:
        return 6; /* ch, slot, hue lo, hue hi, sat, bri */
    case VFX_HID_OP_SCENE_ADD_LAYER:
        /* ch, type, zone start, zone len, blend, opacity, hue lo, hue hi,
         * sat, bri, 4 args x 2 bytes, flags.
         */
        return 19;
    case VFX_HID_OP_SCENE_GRADIENT_ADD_STOP:
        return 6; /* ch, slot, hue lo, hue hi, sat, bri */
    case VFX_HID_OP_SCENE_GET_GRADIENT_STOP:
        return 3; /* ch, slot, stop index */

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
        out->hue = read_i16(&data[2]);
        break;

    case VFX_HID_OP_SCENE_ACTIVATE:
    case VFX_HID_OP_SCENE_DEACTIVATE:
    case VFX_HID_OP_SCENE_GET_INFO:
    case VFX_HID_OP_SCENE_GET_ORDER:
    case VFX_HID_OP_SCENE_RESET:
        out->ch = data[1];
        break;

    case VFX_HID_OP_SCENE_REMOVE_LAYER:
    case VFX_HID_OP_SCENE_GET_LAYER:
        out->ch = data[1];
        out->slot = data[2];
        break;

    case VFX_HID_OP_SCENE_MOVE_LAYER:
        out->ch = data[1];
        out->slot = data[2];
        out->direction = (int8_t)data[3];
        break;

    case VFX_HID_OP_SCENE_SET_ARG:
        out->ch = data[1];
        out->slot = data[2];
        out->arg_idx = data[3];
        out->args[0] = read_i16(&data[4]);
        break;

    case VFX_HID_OP_SCENE_SET_COLOR:
        out->ch = data[1];
        out->slot = data[2];
        out->hue = read_i16(&data[3]);
        out->sat = data[5];
        out->bri = data[6];
        break;

    case VFX_HID_OP_SCENE_ADD_LAYER:
        out->ch = data[1];
        out->type = data[2];
        out->zone_start = data[3];
        out->zone_len = data[4];
        out->blend = data[5];
        out->opacity = data[6];
        out->hue = read_i16(&data[7]);
        out->sat = data[9];
        out->bri = data[10];
        out->args[0] = read_i16(&data[11]);
        out->args[1] = read_i16(&data[13]);
        out->args[2] = read_i16(&data[15]);
        out->args[3] = read_i16(&data[17]);
        out->flags = data[19];
        break;

    case VFX_HID_OP_SCENE_GRADIENT_ADD_STOP:
        out->ch = data[1];
        out->slot = data[2];
        out->hue = read_i16(&data[3]);
        out->sat = data[5];
        out->bri = data[6];
        break;

    case VFX_HID_OP_SCENE_GET_GRADIENT_STOP:
        out->ch = data[1];
        out->slot = data[2];
        out->arg_idx = data[3];
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
    write_i16(&out[2], hue);
    out[4] = level;
    out[5] = speed;
    out[6] = status;

    return 7;
}

uint8_t vfx_hid_encode_scene_info(uint8_t ch, uint8_t count, bool active, uint8_t status,
                                  uint8_t *out) {
    out[0] = VFX_HID_REPLY_SCENE_INFO;
    out[1] = ch;
    out[2] = count;
    out[3] = active ? 1 : 0;
    out[4] = status;

    return 5;
}

uint8_t vfx_hid_encode_scene_layer(uint8_t ch, uint8_t slot, uint8_t type, uint8_t zone_start,
                                   uint8_t zone_len, uint8_t blend, uint8_t opacity, int16_t hue,
                                   uint8_t sat, uint8_t bri, const int16_t args[4], uint8_t flags,
                                   uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_SCENE_LAYER;
    out[1] = ch;
    out[2] = slot;
    out[3] = type;
    out[4] = zone_start;
    out[5] = zone_len;
    out[6] = blend;
    out[7] = opacity;
    write_i16(&out[8], hue);
    out[10] = sat;
    out[11] = bri;
    write_i16(&out[12], args[0]);
    write_i16(&out[14], args[1]);
    write_i16(&out[16], args[2]);
    write_i16(&out[18], args[3]);
    out[20] = flags;
    out[21] = status;

    return 22;
}

uint8_t vfx_hid_encode_scene_order(uint8_t ch, const uint8_t *order, uint8_t count, uint8_t status,
                                   uint8_t *out) {
    out[0] = VFX_HID_REPLY_SCENE_ORDER;
    out[1] = ch;
    out[2] = count;

    for (uint8_t i = 0; i < count; i++) {
        out[3 + i] = order[i];
    }

    out[3 + count] = status;

    return (uint8_t)(4 + count);
}

uint8_t vfx_hid_encode_gradient_stop(uint8_t ch, uint8_t slot, uint8_t idx, int16_t hue,
                                     uint8_t sat, uint8_t bri, uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_GRADIENT_STOP;
    out[1] = ch;
    out[2] = slot;
    out[3] = idx;
    write_i16(&out[4], hue);
    out[6] = sat;
    out[7] = bri;
    out[8] = status;

    return 9;
}

uint8_t vfx_hid_request_len(uint8_t op) {
    const uint8_t need = payload_len((enum vfx_hid_op)op);

    return need == 0xFF ? 0 : (uint8_t)(1 + need);
}

void vfx_relay_pack(uint8_t cmd, uint8_t chunk_index, uint8_t total_len,
                    const uint8_t *chunk_bytes, uint8_t chunk_len, uint32_t *param1,
                    uint32_t *param2) {
    uint32_t p2 = 0;

    for (uint8_t i = 0; i < chunk_len; i++) {
        p2 |= (uint32_t)chunk_bytes[i] << (8 * i);
    }

    *param1 = (uint32_t)cmd | ((uint32_t)chunk_len << 8) | ((uint32_t)chunk_index << 16) |
             ((uint32_t)total_len << 24);
    *param2 = p2;
}

bool vfx_relay_unpack(uint32_t param1, uint32_t param2, uint8_t *buf, uint8_t *have,
                      uint8_t *total) {
    const uint8_t chunk_len = (uint8_t)((param1 >> 8) & 0xFF);
    const uint8_t chunk_index = (uint8_t)((param1 >> 16) & 0xFF);
    const uint8_t total_len = (uint8_t)((param1 >> 24) & 0xFF);

    if (chunk_len == 0 || chunk_len > VFX_RELAY_CHUNK_BYTES || total_len == 0 ||
        total_len > VFX_RELAY_MAX_BYTES) {
        return false;
    }

    if (chunk_index == 0) {
        *have = 0;
        *total = total_len;
    }

    if (total_len != *total) {
        return false; /* an interrupted request's tail, not this one's start */
    }

    const uint16_t offset = (uint16_t)chunk_index * VFX_RELAY_CHUNK_BYTES;

    if (offset + chunk_len > VFX_RELAY_MAX_BYTES) {
        return false;
    }

    uint8_t bytes[VFX_RELAY_CHUNK_BYTES];

    for (uint8_t i = 0; i < chunk_len; i++) {
        bytes[i] = (uint8_t)(param2 >> (8 * i));
    }

    memcpy(&buf[offset], bytes, chunk_len);
    *have = (uint8_t)(offset + chunk_len);

    return *have >= *total;
}
