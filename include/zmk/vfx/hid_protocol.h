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
    /* ch, slot, arg index (0-5), value (int16 LE) -> ACK. ADD_LAYER carries
     * the first four numbers; the fifth and sixth (water and matrix use all
     * six) are only reachable here.
     */
    VFX_HID_OP_SCENE_SET_ARG = 0x09,
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

    /* A gradient's stop list rarely fits alongside SCENE_ADD_LAYER's own
     * fields in one report, so it is built afterwards, one stop per
     * message, the same way a scene itself already composes from many
     * small per-layer messages rather than one large one. Build a
     * gradient with an ordinary SCENE_ADD_LAYER (type VFX_RT_GRADIENT,
     * hue/sat/bri ignored), then one of these per stop, in order.
     */
    VFX_HID_OP_SCENE_GRADIENT_ADD_STOP = 0x12, /* ch, slot, hue (int16 LE), sat, bri -> ACK */
    /* ch, slot, stop index -> GRADIENT_STOP. The read side of the same
     * thing, for a host reconstructing a gradient it did not just build
     * itself -- SCENE_GET_LAYER's own reply carries the stop count (in its
     * args[2], which a gradient does not otherwise use) but not the stops
     * themselves, so this is called that many times. LAYER_EXT carries the
     * same count for every type, and GET_LIST_COLOR reads any type's list.
     */
    VFX_HID_OP_SCENE_GET_GRADIENT_STOP = 0x13,

    /* The rest of what a generator needs beyond SCENE_ADD_LAYER's one colour
     * and four numbers, each a small message of its own so a layer of any
     * type is assembled from as many of them as it takes -- which is what
     * lets every generator be built here rather than only the ones whose
     * whole configuration fits one report. See runtime_scene.h for which
     * colour and number feeds which generator field.
     */

    /* ch, slot, index, hue (int16 LE), sat, bri -> ACK. Entry `index` of the
     * layer's colour list, whatever its type: the second colour of water,
     * matrix, fire, comet and cross, the ends of a battery bar, one colour per
     * keymap layer for layer-state. Growing the list is just setting the next
     * index; GRADIENT_ADD_STOP is the same thing for a gradient, appending.
     */
    VFX_HID_OP_SCENE_SET_LIST_COLOR = 0x14,
    /* ch, slot, index -> LIST_COLOR. Read side of SET_LIST_COLOR, for any
     * type, called num_colors times (from LAYER_EXT).
     */
    VFX_HID_OP_SCENE_GET_LIST_COLOR = 0x15,
    /* ch, slot, kind, offset, count, VFX_HID_ZONE_CHUNK data bytes -> ACK.
     * kind 0 is a range (data = start, length); kind 1 a pixel list and kind
     * 2 a key list, sent VFX_HID_ZONE_CHUNK at a time in order, each chunk's
     * offset the number of entries already sent -- offset 0 starts the list
     * over. Always full length on the wire; `count` says how many of the
     * data bytes are real.
     */
    VFX_HID_OP_SCENE_SET_ZONE = 0x16,
    /* ch, slot, offset -> ZONE: kind, total entries and up to
     * VFX_HID_ZONE_CHUNK of them starting at offset. Call again at
     * offset + n until that reaches total.
     */
    VFX_HID_OP_SCENE_GET_ZONE = 0x17,
    /* ch, slot, blend, opacity, opacity source, opacity min, opacity full,
     * tune id -> ACK. Changes how a layer is composited without rebuilding
     * it, so nothing animating restarts.
     */
    VFX_HID_OP_SCENE_SET_OPTS = 0x18,
    /* ch, slot -> LAYER_EXT: everything about a layer SCENE_GET_LAYER has no
     * room for -- zone kind and list length, colour list length, the layer
     * options, and the fifth and sixth numbers.
     */
    VFX_HID_OP_SCENE_GET_LAYER_EXT = 0x19,

    /* ch, slot, flags -> ACK. Replaces a layer's flags byte (stack, reverse,
     * axis) in place -- the one field SCENE_ADD_LAYER carried that had no
     * message of its own, so changing it meant removing and re-adding.
     */
    VFX_HID_OP_SCENE_SET_FLAGS = 0x1A,
    /* ch, slot, position -> ACK. Makes a layer added with VFX_HID_FLAG_STAGED
     * live: it enters the render order at `position` (0 = bottom, 0xFF or
     * anything past the top = on top) in a single step, so a layer built from
     * many messages never shows half-configured.
     */
    VFX_HID_OP_SCENE_COMMIT_LAYER = 0x1B,
};

/* SCENE_ADD_LAYER's flags byte, bit 5: build the layer staged -- reserved and
 * editable, but not rendered until SCENE_COMMIT_LAYER. A wire-only bit; decode
 * strips it into vfx_hid_request.staged, so it never reaches a layer's own
 * flags (VFX_RT_FLAGS_MASK covers bits 0-4).
 */
#define VFX_HID_FLAG_STAGED 0x20

/* SCENE_COMMIT_LAYER's position meaning "on top". */
#define VFX_HID_POSITION_TOP 0xFF

#define VFX_HID_REPLY_BIT 0x80

/* GET's own reply op, reused by GET_ALL since each of its replies is still
 * just one slot's state.
 */
#define VFX_HID_REPLY_STATE (VFX_HID_OP_GET | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_PONG (VFX_HID_OP_PING | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_INFO (VFX_HID_OP_SCENE_GET_INFO | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_LAYER (VFX_HID_OP_SCENE_GET_LAYER | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_SCENE_ORDER (VFX_HID_OP_SCENE_GET_ORDER | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_GRADIENT_STOP (VFX_HID_OP_SCENE_GET_GRADIENT_STOP | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_LIST_COLOR (VFX_HID_OP_SCENE_GET_LIST_COLOR | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_ZONE (VFX_HID_OP_SCENE_GET_ZONE | VFX_HID_REPLY_BIT)
#define VFX_HID_REPLY_LAYER_EXT (VFX_HID_OP_SCENE_GET_LAYER_EXT | VFX_HID_REPLY_BIT)

/* Zone entries carried by one SET_ZONE / ZONE. 12 keeps SET_ZONE at 18 bytes
 * on the wire, inside SPLIT_LINK_MAX_MSG (split_link.h), so it can be relayed to a split
 * peripheral like every other mutating op.
 */
#define VFX_HID_ZONE_CHUNK 12

/* Longest reply this protocol produces (SCENE_LAYER), so a caller can size
 * one buffer for whichever encode_* it ends up calling. SCENE_ORDER's own
 * length depends on the board's CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS (op, ch,
 * count, one byte per slot, status), but that option is capped at 16, so
 * SCENE_ORDER can never exceed this either. GRADIENT_STOP and LIST_COLOR
 * are nine bytes, ZONE twenty and LAYER_EXT fifteen, well under this
 * regardless.
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
/* The central cannot take the request right now (the split link's queue is full
 * or a resync is running). Nothing was applied; retry after a short wait.
 */
#define VFX_HID_STATUS_BUSY 3

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
    int16_t args[4]; /* SCENE_ADD_LAYER's four; SCENE_SET_ARG's value is args[0] */
    uint8_t flags;
    uint8_t arg_idx; /* also a colour list / gradient stop index */
    int8_t direction;

    /* SCENE_SET_ZONE, SCENE_GET_ZONE */
    uint8_t zone_kind;
    uint8_t zone_offset;
    uint8_t zone_count;
    uint8_t zone_data[VFX_HID_ZONE_CHUNK];

    /* SCENE_SET_OPTS (blend and opacity are the fields above) */
    uint8_t opacity_src;
    uint8_t opacity_min;
    uint8_t opacity_full;
    uint8_t tune_id;

    /* SCENE_ADD_LAYER: the STAGED bit, split out of the flags byte. */
    bool staged;
    /* SCENE_COMMIT_LAYER */
    uint8_t position;
};

/* Most bytes vfx_hid_encode_request() can write: SCENE_SET_ZONE's 18, rounded
 * up to what a raw HID report carries.
 */
#define VFX_HID_MAX_REQUEST_LEN 32

/* The inverse of vfx_hid_decode(): writes the wire bytes of `req` into `out`
 * (VFX_HID_MAX_REQUEST_LEN long) and returns their count, 0 for an op it does
 * not know. What a firmware-side sender uses to replay a scene to a peripheral
 * without keeping the bytes it was originally sent.
 */
uint8_t vfx_hid_encode_request(const struct vfx_hid_request *req, uint8_t *out);

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
/* `ch` here is the wire target byte. After the original five bytes it appends
 * how many runtime scenes a channel holds, which of them is showing (0xFF for
 * the compiled list) and vfx_runtime_hash() of the target, so a host that only
 * knows the first five keeps working.
 */
uint8_t vfx_hid_encode_scene_info(uint8_t ch, uint8_t count, bool active, uint8_t scenes,
                                  uint8_t active_scene, uint32_t hash, uint8_t status,
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

uint8_t vfx_hid_encode_gradient_stop(uint8_t ch, uint8_t slot, uint8_t idx, int16_t hue,
                                     uint8_t sat, uint8_t bri, uint8_t status, uint8_t *out);
uint8_t vfx_hid_encode_list_color(uint8_t ch, uint8_t slot, uint8_t idx, int16_t hue, uint8_t sat,
                                  uint8_t bri, uint8_t status, uint8_t *out);

/* `data` is `n` bytes (at most VFX_HID_ZONE_CHUNK); the reply always pads to
 * the full chunk so its length does not depend on n.
 */
uint8_t vfx_hid_encode_zone(uint8_t ch, uint8_t slot, uint8_t kind, uint8_t total, uint8_t offset,
                            const uint8_t *data, uint8_t n, uint8_t status, uint8_t *out);

uint8_t vfx_hid_encode_layer_ext(uint8_t ch, uint8_t slot, uint8_t zone_kind, uint8_t zone_count,
                                 uint8_t num_colors, uint8_t opacity_src, uint8_t opacity_min,
                                 uint8_t opacity_full, uint8_t tune_id, int16_t arg4,
                                 int16_t arg5, uint8_t status, uint8_t *out);

/* Total wire length (op byte plus payload) of a request naming this op, or 0
 * if it is not one of the ops above. Lets a caller holding a decoded
 * request's original bytes -- hid_transport.c, forwarding a scene op to a
 * split peripheral -- know how many of them actually mattered, without a
 * second copy of payload_len()'s own table.
 */
uint8_t vfx_hid_request_len(uint8_t op);

