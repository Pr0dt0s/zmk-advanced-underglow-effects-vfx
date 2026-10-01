/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <string.h>

#include <zmk/vfx/split_link.h>

/* The Zephyr-free half of the split link: chunk packing, reassembly and the
 * send queue's ring. Compiled by the host tests so what decides whether a
 * scene arrives intact is exercised without a radio.
 */

#define SEQ_MASK 0x07
#define LEN_MASK 0x1F

uint8_t split_link_chunks_for(uint8_t len) {
    if (len == 0 || len > SPLIT_LINK_MAX_MSG) {
        return 0;
    }

    return (uint8_t)((len + SPLIT_LINK_CHUNK_BYTES - 1) / SPLIT_LINK_CHUNK_BYTES);
}

/* What chunk `idx` of a `total`-byte message must be, since the sender always
 * fills every chunk but the last.
 */
static uint8_t expected_chunk_len(uint8_t idx, uint8_t total) {
    const uint8_t start = (uint8_t)(idx * SPLIT_LINK_CHUNK_BYTES);
    const uint8_t left = (uint8_t)(total - start);

    return left < SPLIT_LINK_CHUNK_BYTES ? left : SPLIT_LINK_CHUNK_BYTES;
}

void split_link_pack(uint8_t cmd, uint8_t seq, uint8_t chunk_index, uint8_t total_len,
                     const uint8_t *chunk_bytes, uint8_t chunk_len, uint32_t *param1,
                     uint32_t *param2) {
    uint32_t p2 = 0;

    for (uint8_t i = 0; i < chunk_len; i++) {
        p2 |= (uint32_t)chunk_bytes[i] << (8 * i);
    }

    const uint32_t len_seq = (uint32_t)(total_len & LEN_MASK) | ((uint32_t)(seq & SEQ_MASK) << 5);

    *param1 = (uint32_t)cmd | ((uint32_t)chunk_len << 8) | ((uint32_t)chunk_index << 16) |
              (len_seq << 24);
    *param2 = p2;
}

uint8_t split_link_rx_chunk(struct split_link_rx *rx, uint32_t param1, uint32_t param2) {
    const uint8_t chunk_len = (uint8_t)((param1 >> 8) & 0xFF);
    const uint8_t idx = (uint8_t)((param1 >> 16) & 0xFF);
    const uint8_t len_seq = (uint8_t)((param1 >> 24) & 0xFF);
    const uint8_t total = len_seq & LEN_MASK;
    const uint8_t seq = (uint8_t)(len_seq >> 5);
    const uint8_t chunks = split_link_chunks_for(total);

    if (chunks == 0 || idx >= chunks || chunk_len != expected_chunk_len(idx, total)) {
        rx->malformed++;

        return 0;
    }

    if (idx == 0) {
        if (rx->total != 0) {
            rx->incomplete++; /* a message was still being assembled */
        }

        rx->total = total;
        rx->seq = seq;
        rx->next = 0;
    } else if (rx->total == 0 || rx->seq != seq || rx->total != total) {
        rx->orphan++;

        return 0;
    } else if (idx != rx->next) {
        /* A chunk went missing, or this one is a repeat: either way what has been
         * assembled cannot be trusted, and neither can what follows.
         */
        if (idx > rx->next) {
            rx->incomplete++;
            rx->total = 0;
        } else {
            rx->orphan++;
        }

        return 0;
    }

    for (uint8_t i = 0; i < chunk_len; i++) {
        rx->buf[idx * SPLIT_LINK_CHUNK_BYTES + i] = (uint8_t)(param2 >> (8 * i));
    }

    rx->next = (uint8_t)(idx + 1);

    if (rx->next < chunks) {
        return 0;
    }

    rx->ok++;
    rx->total = 0;

    return total;
}

void split_link_queue_init(struct split_link_queue *q, struct split_link_chunk *storage,
                           uint16_t cap) {
    memset(q, 0, sizeof(*q));
    q->buf = storage;
    q->cap = cap;
}

uint16_t split_link_queue_room(const struct split_link_queue *q) {
    return (uint16_t)(q->cap - q->count);
}

bool split_link_queue_push(struct split_link_queue *q, uint8_t cmd, const uint8_t *data,
                           uint8_t len) {
    const uint8_t chunks = split_link_chunks_for(len);

    if (chunks == 0) {
        return false;
    }

    if (split_link_queue_room(q) < chunks) {
        q->refused++;

        return false;
    }

    const uint8_t seq = q->seq;

    q->seq = (uint8_t)((q->seq + 1) & SEQ_MASK);

    for (uint8_t i = 0; i < chunks; i++) {
        const uint8_t offset = (uint8_t)(i * SPLIT_LINK_CHUNK_BYTES);
        struct split_link_chunk *slot = &q->buf[(q->head + q->count) % q->cap];

        split_link_pack(cmd, seq, i, len, &data[offset], expected_chunk_len(i, len), &slot->param1,
                        &slot->param2);
        q->count++;
        q->pushed++;
    }

    if (q->count > q->high_water) {
        q->high_water = q->count;
    }

    return true;
}

bool split_link_queue_pop(struct split_link_queue *q, struct split_link_chunk *out) {
    if (q->count == 0) {
        return false;
    }

    *out = q->buf[q->head];
    q->head = (uint16_t)((q->head + 1) % q->cap);
    q->count--;

    return true;
}
