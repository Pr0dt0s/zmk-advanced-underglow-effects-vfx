/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* Wire format for driving zmk_vfx_tune_*() from a host, over whatever raw
 * HID transport is attached (see hid_transport.c). Kept as plain encode and
 * decode functions with no Zephyr or event-manager dependency, so it can be
 * exercised the same way the compositor itself is: built and asserted on
 * with no kernel in scope at all.
 *
 * A request is [op, ...payload], however long its op needs. A reply is
 * always [op | VFX_HID_REPLY_BIT, ...], which is what lets a host tell a
 * reply from an echo of its own request without inspecting anything past
 * the first byte.
 */

#define VFX_HID_PROTOCOL_VERSION 1

enum vfx_hid_op {
    VFX_HID_OP_PING = 0x00,      /* -> PONG: protocol version, highest usable slot */
    VFX_HID_OP_SET_HUE = 0x01,   /* slot, hue (int16 LE) -> ACK */
    VFX_HID_OP_SET_LEVEL = 0x02, /* slot, level -> ACK */
    VFX_HID_OP_SET_SPEED = 0x03, /* slot, speed -> ACK */
    VFX_HID_OP_RESET = 0x04,     /* slot -> ACK */
    VFX_HID_OP_GET = 0x05,       /* slot -> STATE */
    /* No payload. One STATE reply per usable slot, in slot order, rather
     * than a request per slot -- what a host wants on first connecting.
     */
    VFX_HID_OP_GET_ALL = 0x06,
};

#define VFX_HID_REPLY_BIT 0x80

/* GET's own reply op, reused by GET_ALL since each of its replies is still
 * just one slot's state.
 */
#define VFX_HID_REPLY_STATE (VFX_HID_OP_GET | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_PONG (VFX_HID_OP_PING | VFX_HID_REPLY_BIT)

/* Longest reply this protocol produces (STATE), so a caller can size one
 * buffer for whichever encode_* it ends up calling.
 */
#define VFX_HID_MAX_REPLY_LEN 7

/* 0 means the request succeeded; every other value is a decode or dispatch
 * failure, always because the request named a slot outside 1..VFX_TUNE_SLOTS-1
 * (0 is reserved for "untuned" and is never itself a valid target).
 */
#define VFX_HID_STATUS_OK 0
#define VFX_HID_STATUS_BAD_SLOT 1

struct vfx_hid_request {
    uint8_t op;
    uint8_t slot;
    int16_t hue;
    uint8_t level;
    uint8_t speed;
};

/* Decodes one request out of a report. False when len is too short for the
 * op it names, or the op is not one of the above -- both are a truncated or
 * corrupt report, not something to guess at.
 */
bool vfx_hid_decode(const uint8_t *data, uint8_t len, struct vfx_hid_request *out);

/* Each returns the number of bytes it wrote to `out`, which must have room
 * for at least VFX_HID_MAX_REPLY_LEN.
 */
uint8_t vfx_hid_encode_pong(uint8_t max_slot, uint8_t *out);
uint8_t vfx_hid_encode_ack(uint8_t op, uint8_t slot, uint8_t status, uint8_t *out);
uint8_t vfx_hid_encode_state(uint8_t slot, int16_t hue, uint8_t level, uint8_t speed,
                             uint8_t status, uint8_t *out);
