/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

/* A small message pipe from a split's central to its peripherals, riding the
 * &vfx behavior's relay, plus an optional report path back. It moves opaque
 * bytes of at most SPLIT_LINK_MAX_MSG and knows nothing about layers, scenes
 * or the HID protocol: nothing under src/split_link/ may include
 * runtime_scene.h, hid_protocol.h or vfx.h, so that this can be lifted into a
 * module of its own once it has run on hardware.
 *
 * A behavior invocation carries two uint32_t, param1 above its command byte
 * and param2. ZMK narrows a relayed event's `position` to one byte on the
 * wire, so only those two are usable: param1 holds a header, param2 up to
 * SPLIT_LINK_CHUNK_BYTES of the message. A message longer than that goes out
 * as several invocations, which is what the rest of this file keeps honest.
 *
 * param1 layout: command byte | chunk length << 8 | chunk index << 16 |
 * (message length | sequence << 5) << 24. The sequence is what tells a new
 * message from the stale head of one whose first chunk was lost.
 */

#define SPLIT_LINK_CHUNK_BYTES 4
#define SPLIT_LINK_MAX_MSG 20
#define SPLIT_LINK_MAX_CHUNKS ((SPLIT_LINK_MAX_MSG + SPLIT_LINK_CHUNK_BYTES - 1) / SPLIT_LINK_CHUNK_BYTES)

/* How many invocations a message of `len` bytes takes; 0 for a length the link
 * cannot carry.
 */
uint8_t split_link_chunks_for(uint8_t len);

/* Packs one chunk into a behavior invocation's two payload words. */
void split_link_pack(uint8_t cmd, uint8_t seq, uint8_t chunk_index, uint8_t total_len,
                     const uint8_t *chunk_bytes, uint8_t chunk_len, uint32_t *param1,
                     uint32_t *param2);

/* ---- receive side: reassembly ------------------------------------------- */

struct split_link_rx {
    uint8_t buf[SPLIT_LINK_MAX_MSG];
    uint8_t total; /* length of the message being assembled, 0 when none */
    uint8_t seq;
    uint8_t next; /* index of the chunk expected next */

    /* Counters, never reset by the link. `incomplete`: a partial message was
     * abandoned (a newer one began, or a chunk went missing). `orphan`: a chunk
     * that belongs to no message in progress (its head was lost, or it is a
     * duplicate). `malformed`: a header that cannot be real.
     */
    uint32_t ok;
    uint32_t incomplete;
    uint32_t orphan;
    uint32_t malformed;
};

/* Feeds one received invocation. Returns the message length once `rx->buf`
 * holds a whole message, 0 while it is still incomplete or the chunk was
 * rejected. Chunks must arrive in order; a gap abandons the message rather than
 * guessing, because applying a message with a hole in it corrupts a scene
 * silently.
 */
uint8_t split_link_rx_chunk(struct split_link_rx *rx, uint32_t param1, uint32_t param2);

/* ---- send side: a paced queue -------------------------------------------- */

struct split_link_chunk {
    uint32_t param1;
    uint32_t param2;
};

struct split_link_queue {
    struct split_link_chunk *buf;
    uint16_t cap;
    uint16_t head;
    uint16_t count;
    uint16_t high_water;
    uint8_t seq;
    uint32_t pushed; /* chunks ever queued */
    uint32_t refused; /* messages turned away for want of room */
};

void split_link_queue_init(struct split_link_queue *q, struct split_link_chunk *storage,
                           uint16_t cap);

uint16_t split_link_queue_room(const struct split_link_queue *q);

/* Queues every chunk of a message, or none: a half-queued message is exactly
 * the corruption this exists to prevent. False for a length the link cannot
 * carry or when there is not room for all of it (counted in `refused`).
 */
bool split_link_queue_push(struct split_link_queue *q, uint8_t cmd, const uint8_t *data,
                           uint8_t len);

bool split_link_queue_pop(struct split_link_queue *q, struct split_link_chunk *out);

/* ---- the link itself (Zephyr) -------------------------------------------- */

struct split_link_stats {
    uint32_t rx_ok;
    uint32_t rx_incomplete;
    uint32_t rx_orphan;
    uint32_t rx_malformed;
    uint32_t tx_chunks;
    uint32_t tx_refused;
    uint16_t tx_high_water;
    uint16_t tx_queued;
};

/* Queues a message for every peripheral. Central only; a no-op returning 0
 * anywhere else. -EBUSY when the queue cannot take all of it, in which case
 * nothing was queued.
 */
int split_link_send(const uint8_t *data, uint8_t len);

/* Whether a message of `len` bytes would be accepted right now, so a caller
 * can refuse a request before applying any of it.
 */
bool split_link_room(uint8_t len);

/* Handles one relayed invocation on the receiving half: reassembles, and calls
 * the receive callback with each whole message.
 */
void split_link_receive(uint32_t param1, uint32_t param2);
void split_link_set_receive_cb(void (*cb)(const uint8_t *msg, uint8_t len));

void split_link_get_stats(struct split_link_stats *out);

/* Peripheral to central, over the GATT report channel. Both are inert, and
 * split_link_report() returns -ENOTSUP, unless CONFIG_ZMK_VFX_SPLIT_REPORT is
 * on. `peripheral` is an index in the order the central noticed them.
 */
int split_link_report(const uint8_t *data, uint8_t len);
void split_link_set_report_cb(void (*cb)(uint8_t peripheral, const uint8_t *data, uint8_t len));

/* Called on the central when a peripheral's report channel becomes usable
 * (subscribed) or goes away.
 */
void split_link_set_peripheral_cb(void (*cb)(uint8_t peripheral, bool present));
