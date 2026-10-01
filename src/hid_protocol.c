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
    case VFX_HID_OP_SCENE_SET_FLAGS:
    case VFX_HID_OP_SCENE_COMMIT_LAYER:
        return 3; /* ch, slot, direction / flags / position */
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

    case VFX_HID_OP_SCENE_SET_LIST_COLOR:
        return 7; /* ch, slot, index, hue lo, hue hi, sat, bri */
    case VFX_HID_OP_SCENE_GET_LIST_COLOR:
        return 3; /* ch, slot, index */
    case VFX_HID_OP_SCENE_SET_ZONE:
        return (uint8_t)(5 + VFX_HID_ZONE_CHUNK); /* ch, slot, kind, offset, count, data */
    case VFX_HID_OP_SCENE_GET_ZONE:
        return 3; /* ch, slot, offset */
    case VFX_HID_OP_SCENE_SET_OPTS:
        return 8; /* ch, slot, blend, opacity, src, min, full, tune id */
    case VFX_HID_OP_SCENE_GET_LAYER_EXT:
        return 2; /* ch, slot */

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

    /* `count` is how much of the fixed-size data field is real; more than the
     * field holds would make the consumer read past it.
     */
    if (op == VFX_HID_OP_SCENE_SET_ZONE && data[5] > VFX_HID_ZONE_CHUNK) {
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
        out->staged = (data[19] & VFX_HID_FLAG_STAGED) != 0;
        out->flags = (uint8_t)(data[19] & ~VFX_HID_FLAG_STAGED);
        break;

    case VFX_HID_OP_SCENE_GRADIENT_ADD_STOP:
        out->ch = data[1];
        out->slot = data[2];
        out->hue = read_i16(&data[3]);
        out->sat = data[5];
        out->bri = data[6];
        break;

    case VFX_HID_OP_SCENE_GET_GRADIENT_STOP:
    case VFX_HID_OP_SCENE_GET_LIST_COLOR:
        out->ch = data[1];
        out->slot = data[2];
        out->arg_idx = data[3];
        break;

    case VFX_HID_OP_SCENE_SET_LIST_COLOR:
        out->ch = data[1];
        out->slot = data[2];
        out->arg_idx = data[3];
        out->hue = read_i16(&data[4]);
        out->sat = data[6];
        out->bri = data[7];
        break;

    case VFX_HID_OP_SCENE_SET_ZONE:
        out->ch = data[1];
        out->slot = data[2];
        out->zone_kind = data[3];
        out->zone_offset = data[4];
        out->zone_count = data[5];
        memcpy(out->zone_data, &data[6], VFX_HID_ZONE_CHUNK);
        break;

    case VFX_HID_OP_SCENE_GET_ZONE:
        out->ch = data[1];
        out->slot = data[2];
        out->zone_offset = data[3];
        break;

    case VFX_HID_OP_SCENE_SET_OPTS:
        out->ch = data[1];
        out->slot = data[2];
        out->blend = data[3];
        out->opacity = data[4];
        out->opacity_src = data[5];
        out->opacity_min = data[6];
        out->opacity_full = data[7];
        out->tune_id = data[8];
        break;

    case VFX_HID_OP_SCENE_GET_LAYER_EXT:
        out->ch = data[1];
        out->slot = data[2];
        break;

    case VFX_HID_OP_SCENE_SET_FLAGS:
        out->ch = data[1];
        out->slot = data[2];
        out->flags = data[3];
        break;

    case VFX_HID_OP_SCENE_COMMIT_LAYER:
        out->ch = data[1];
        out->slot = data[2];
        out->position = data[3];
        break;
    }

    return true;
}

uint8_t vfx_hid_encode_request(const struct vfx_hid_request *r, uint8_t *out) {
    const uint8_t need = payload_len((enum vfx_hid_op)r->op);

    if (need == 0xFF) {
        return 0;
    }

    memset(out, 0, VFX_HID_MAX_REQUEST_LEN);
    out[0] = r->op;

    switch ((enum vfx_hid_op)r->op) {
    case VFX_HID_OP_PING:
    case VFX_HID_OP_GET_ALL:
        break;

    case VFX_HID_OP_RESET:
    case VFX_HID_OP_GET:
        out[1] = r->slot;
        break;

    case VFX_HID_OP_SET_LEVEL:
        out[1] = r->slot;
        out[2] = r->level;
        break;

    case VFX_HID_OP_SET_SPEED:
        out[1] = r->slot;
        out[2] = r->speed;
        break;

    case VFX_HID_OP_SET_HUE:
        out[1] = r->slot;
        write_i16(&out[2], r->hue);
        break;

    case VFX_HID_OP_SCENE_ACTIVATE:
    case VFX_HID_OP_SCENE_DEACTIVATE:
    case VFX_HID_OP_SCENE_GET_INFO:
    case VFX_HID_OP_SCENE_GET_ORDER:
    case VFX_HID_OP_SCENE_RESET:
        out[1] = r->ch;
        break;

    case VFX_HID_OP_SCENE_REMOVE_LAYER:
    case VFX_HID_OP_SCENE_GET_LAYER:
    case VFX_HID_OP_SCENE_GET_LAYER_EXT:
        out[1] = r->ch;
        out[2] = r->slot;
        break;

    case VFX_HID_OP_SCENE_MOVE_LAYER:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = (uint8_t)r->direction;
        break;

    case VFX_HID_OP_SCENE_SET_FLAGS:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->flags;
        break;

    case VFX_HID_OP_SCENE_COMMIT_LAYER:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->position;
        break;

    case VFX_HID_OP_SCENE_SET_ARG:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->arg_idx;
        write_i16(&out[4], r->args[0]);
        break;

    case VFX_HID_OP_SCENE_SET_COLOR:
    case VFX_HID_OP_SCENE_GRADIENT_ADD_STOP:
        out[1] = r->ch;
        out[2] = r->slot;
        write_i16(&out[3], r->hue);
        out[5] = r->sat;
        out[6] = r->bri;
        break;

    case VFX_HID_OP_SCENE_ADD_LAYER:
        out[1] = r->ch;
        out[2] = r->type;
        out[3] = r->zone_start;
        out[4] = r->zone_len;
        out[5] = r->blend;
        out[6] = r->opacity;
        write_i16(&out[7], r->hue);
        out[9] = r->sat;
        out[10] = r->bri;
        write_i16(&out[11], r->args[0]);
        write_i16(&out[13], r->args[1]);
        write_i16(&out[15], r->args[2]);
        write_i16(&out[17], r->args[3]);
        out[19] = (uint8_t)((r->flags & ~VFX_HID_FLAG_STAGED) | (r->staged ? VFX_HID_FLAG_STAGED : 0));
        break;

    case VFX_HID_OP_SCENE_GET_GRADIENT_STOP:
    case VFX_HID_OP_SCENE_GET_LIST_COLOR:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->arg_idx;
        break;

    case VFX_HID_OP_SCENE_SET_LIST_COLOR:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->arg_idx;
        write_i16(&out[4], r->hue);
        out[6] = r->sat;
        out[7] = r->bri;
        break;

    case VFX_HID_OP_SCENE_SET_ZONE:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->zone_kind;
        out[4] = r->zone_offset;
        out[5] = r->zone_count;
        memcpy(&out[6], r->zone_data, VFX_HID_ZONE_CHUNK);
        break;

    case VFX_HID_OP_SCENE_GET_ZONE:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->zone_offset;
        break;

    case VFX_HID_OP_SCENE_SET_OPTS:
        out[1] = r->ch;
        out[2] = r->slot;
        out[3] = r->blend;
        out[4] = r->opacity;
        out[5] = r->opacity_src;
        out[6] = r->opacity_min;
        out[7] = r->opacity_full;
        out[8] = r->tune_id;
        break;
    }

    return (uint8_t)(1 + need);
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

uint8_t vfx_hid_encode_scene_info(uint8_t ch, uint8_t count, bool active, uint8_t scenes,
                                  uint8_t active_scene, uint32_t hash, uint8_t status,
                                  uint8_t *out) {
    out[0] = VFX_HID_REPLY_SCENE_INFO;
    out[1] = ch;
    out[2] = count;
    out[3] = active ? 1 : 0;
    out[4] = status;
    out[5] = scenes;
    out[6] = active_scene;
    out[7] = (uint8_t)hash;
    out[8] = (uint8_t)(hash >> 8);
    out[9] = (uint8_t)(hash >> 16);
    out[10] = (uint8_t)(hash >> 24);

    return 11;
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

uint8_t vfx_hid_encode_list_color(uint8_t ch, uint8_t slot, uint8_t idx, int16_t hue, uint8_t sat,
                                  uint8_t bri, uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_LIST_COLOR;
    out[1] = ch;
    out[2] = slot;
    out[3] = idx;
    write_i16(&out[4], hue);
    out[6] = sat;
    out[7] = bri;
    out[8] = status;

    return 9;
}

uint8_t vfx_hid_encode_zone(uint8_t ch, uint8_t slot, uint8_t kind, uint8_t total, uint8_t offset,
                            const uint8_t *data, uint8_t n, uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_ZONE;
    out[1] = ch;
    out[2] = slot;
    out[3] = kind;
    out[4] = total;
    out[5] = offset;
    out[6] = n;

    for (uint8_t i = 0; i < VFX_HID_ZONE_CHUNK; i++) {
        out[7 + i] = i < n ? data[i] : 0;
    }

    out[7 + VFX_HID_ZONE_CHUNK] = status;

    return (uint8_t)(8 + VFX_HID_ZONE_CHUNK);
}

uint8_t vfx_hid_encode_layer_ext(uint8_t ch, uint8_t slot, uint8_t zone_kind, uint8_t zone_count,
                                 uint8_t num_colors, uint8_t opacity_src, uint8_t opacity_min,
                                 uint8_t opacity_full, uint8_t tune_id, int16_t arg4,
                                 int16_t arg5, uint8_t status, uint8_t *out) {
    out[0] = VFX_HID_REPLY_LAYER_EXT;
    out[1] = ch;
    out[2] = slot;
    out[3] = zone_kind;
    out[4] = zone_count;
    out[5] = num_colors;
    out[6] = opacity_src;
    out[7] = opacity_min;
    out[8] = opacity_full;
    out[9] = tune_id;
    write_i16(&out[10], arg4);
    write_i16(&out[12], arg5);
    out[14] = status;

    return 15;
}

uint8_t vfx_hid_request_len(uint8_t op) {
    const uint8_t need = payload_len((enum vfx_hid_op)op);

    return need == 0xFF ? 0 : (uint8_t)(1 + need);
}
