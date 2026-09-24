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

    /* Runtime scene authoring: building a channel's own scene at runtime,
     * over zmk_vfx_scene_*() (see vfx.h and runtime_scene.h) rather than
     * zmk_vfx_tune_*(). `ch` is a channel index, never ZMK_VFX_CH_ALL --
     * unlike a tuning slot, a runtime scene belongs to one channel, so
     * there is no "every channel" form of any of these.
     *
     * A `slot` below is a layer slot within that channel's own pool, the
     * value SCENE_ADD_LAYER's reply carries back -- a different id space
     * from a tuning slot, and only ever meaningful together with the `ch`
     * it was issued for.
     */
    VFX_HID_OP_SCENE_RESET = 0x07, /* ch -> ACK */
    /* ch, type, zone start, zone len, blend, opacity, hue (int16 LE), sat,
     * bri, four args (int16 LE each), flags -> ACK, slot set to the newly
     * assigned layer id, or 0xFF and VFX_HID_STATUS_POOL_FULL if the
     * channel's pool was already full.
     */
    VFX_HID_OP_SCENE_ADD_LAYER = 0x08,
    VFX_HID_OP_SCENE_SET_ARG = 0x09,      /* ch, slot, arg index, value (int16 LE) -> ACK */
    VFX_HID_OP_SCENE_SET_COLOR = 0x0A,    /* ch, slot, hue (int16 LE), sat, bri -> ACK */
    VFX_HID_OP_SCENE_REMOVE_LAYER = 0x0B, /* ch, slot -> ACK */
    VFX_HID_OP_SCENE_MOVE_LAYER = 0x0C,   /* ch, slot, direction (int8, +/-1) -> ACK */
    VFX_HID_OP_SCENE_ACTIVATE = 0x0D,     /* ch -> ACK */
    VFX_HID_OP_SCENE_DEACTIVATE = 0x0E,   /* ch -> ACK */
    VFX_HID_OP_SCENE_GET_INFO = 0x0F,     /* ch -> SCENE_INFO */
    VFX_HID_OP_SCENE_GET_LAYER = 0x10,    /* ch, slot -> SCENE_LAYER */
    /* ch -> SCENE_ORDER: the channel's own slot ids, in render order --
     * layer 0 in the reply is the bottom of the stack, same sense
     * SCENE_MOVE_LAYER's direction argument uses. The only way to learn
     * that order: nothing else replies with more than one slot's worth of
     * it, and add/remove/move do not touch slot ids, only which position
     * in this order each one holds.
     */
    VFX_HID_OP_SCENE_GET_ORDER = 0x11,
};

#define VFX_HID_REPLY_BIT 0x80

/* GET's own reply op, reused by GET_ALL since each of its replies is still
 * just one slot's state.
 */
#define VFX_HID_REPLY_STATE (VFX_HID_OP_GET | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_PONG (VFX_HID_OP_PING | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_INFO (VFX_HID_OP_SCENE_GET_INFO | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_LAYER (VFX_HID_OP_SCENE_GET_LAYER | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_ORDER (VFX_HID_OP_SCENE_GET_ORDER | VFX_HID_REPLY_BIT)

/* Longest reply this protocol produces (SCENE_LAYER), so a caller can size
 * one buffer for whichever encode_* it ends up calling. SCENE_ORDER's own
 * length depends on the board's CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS (op, ch,
 * count, one byte per slot, status), but that option is capped at 16, so
 * SCENE_ORDER can never exceed this either.
 */
#define VFX_HID_MAX_REPLY_LEN 22

/* Sentinel slot SCENE_ADD_LAYER's ACK carries when it could not assign one. */
#define VFX_HID_NO_SLOT 0xFF

/* 0 means the request succeeded. BAD_SLOT covers every other way a tuning
 * or scene request can be rejected -- a slot or channel outside range, an
 * unusable generator type, an out-of-range argument index or move -- since
 * none of those need their own code for a host to react to; POOL_FULL is
 * broken out because it is the one failure a host would want to react to
 * differently, by removing a layer rather than by fixing what it sent.
 */
#define VFX_HID_STATUS_OK 0
#define VFX_HID_STATUS_BAD_SLOT 1
#define VFX_HID_STATUS_POOL_FULL 2

struct vfx_hid_request {
    uint8_t op;
    uint8_t slot; /* tuning slot (tuning ops) or layer slot (scene ops) */
    int16_t hue;  /* tuning: signed degrees. scene: 0-359 */
    uint8_t level;
    uint8_t speed;

    /* Scene ops only; zero on every tuning request. */
    uint8_t ch;
    uint8_t type;
    uint8_t zone_start;
    uint8_t zone_len;
    uint8_t blend;
    uint8_t opacity;
    uint8_t sat;
    uint8_t bri;
    int16_t args[4];
    uint8_t flags;
    uint8_t arg_idx;
    int8_t direction;
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
uint8_t vfx_hid_encode_scene_info(uint8_t ch, uint8_t count, bool active, uint8_t status,
                                  uint8_t *out);
uint8_t vfx_hid_encode_scene_layer(uint8_t ch, uint8_t slot, uint8_t type, uint8_t zone_start,
                                   uint8_t zone_len, uint8_t blend, uint8_t opacity, int16_t hue,
                                   uint8_t sat, uint8_t bri, const int16_t args[4], uint8_t flags,
                                   uint8_t status, uint8_t *out);

/* `order` is `count` slot ids; the caller (hid_transport.c) owns keeping
 * count within what CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS actually allows, the
 * same trust this file already places in every other fixed-shape caller.
 */
uint8_t vfx_hid_encode_scene_order(uint8_t ch, const uint8_t *order, uint8_t count, uint8_t status,
                                   uint8_t *out);
