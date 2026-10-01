/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/vfx.h>
#include <zmk/behavior.h>
#include <zmk/vfx/split_link.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
#include <zmk/split/central.h>
#endif

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* The Zephyr half of the split link; link_core.c holds everything with logic
 * in it. See split_link.h for what the link is and why it is shaped this way.
 *
 * ZMK relays a behavior invocation to a peripheral through a small queue it
 * shares with everything else it sends, and drops what does not fit. Sending a
 * 5-chunk message back to back is therefore a gamble that grows with the number
 * of messages in flight, so the central queues chunks itself and drains one per
 * CONFIG_ZMK_VFX_SPLIT_LINK_CHUNK_INTERVAL_MS. zmk_split_central_invoke_behavior
 * can also block for up to 100 ms when ZMK's own queue is full, so the interval
 * is not worth setting much below ~12 ms.
 *
 * Nothing in this directory may include runtime_scene.h, hid_protocol.h or
 * vfx.h; the behavior command number comes from the devicetree bindings header
 * only because the &vfx behavior is what carries the invocations.
 */

#if IS_ENABLED(CONFIG_ZMK_VFX_SPLIT_LINK_STATS)
#define LINK_STAT_LOG(...) LOG_WRN(__VA_ARGS__)
#else
#define LINK_STAT_LOG(...)
#endif

static void (*receive_cb)(const uint8_t *msg, uint8_t len);

void split_link_set_receive_cb(void (*cb)(const uint8_t *msg, uint8_t len)) { receive_cb = cb; }

/* ---- receive: runs on both halves ---------------------------------------- */

static struct split_link_rx rx;

void split_link_receive(uint32_t param1, uint32_t param2) {
    const uint32_t incomplete = rx.incomplete;
    const uint32_t orphan = rx.orphan;
    const uint32_t malformed = rx.malformed;
    const uint8_t len = split_link_rx_chunk(&rx, param1, param2);

    if (rx.incomplete != incomplete || rx.orphan != orphan || rx.malformed != malformed) {
        LINK_STAT_LOG("split link: lost data (incomplete %u, orphan %u, malformed %u)",
                      (unsigned)rx.incomplete, (unsigned)rx.orphan, (unsigned)rx.malformed);
    }

    if (len > 0 && receive_cb) {
        receive_cb(rx.buf, len);
    }
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* ---- send: the central's paced queue ------------------------------------- */

static struct split_link_chunk store[CONFIG_ZMK_VFX_SPLIT_LINK_QUEUE_CHUNKS];
static struct split_link_queue queue;
static struct k_spinlock queue_lock;
static struct k_work_delayable drain_work;
static uint32_t sent_chunks;
static bool ready;

static void invoke_all(const struct split_link_chunk *c) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "vfx",
        .param1 = c->param1,
        .param2 = c->param2,
    };
    struct zmk_behavior_binding_event event = {
        .timestamp = k_uptime_get(),
    };

    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        zmk_split_central_invoke_behavior(i, &binding, event, true);
    }
}

static void drain(struct k_work *work) {
    ARG_UNUSED(work);

    struct split_link_chunk c;
    bool more;
    bool got;

    K_SPINLOCK(&queue_lock) {
        got = split_link_queue_pop(&queue, &c);
        more = queue.count > 0;
    }

    if (got) {
        invoke_all(&c);
        sent_chunks++;
    }

    if (more) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &drain_work,
                                    K_MSEC(CONFIG_ZMK_VFX_SPLIT_LINK_CHUNK_INTERVAL_MS));
    }
}

static void ensure_ready(void) {
    if (!ready) {
        split_link_queue_init(&queue, store, ARRAY_SIZE(store));
        k_work_init_delayable(&drain_work, drain);
        ready = true;
    }
}

int split_link_send(const uint8_t *data, uint8_t len) {
    if (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT == 0) {
        return 0;
    }

    ensure_ready();

    bool queued;

    K_SPINLOCK(&queue_lock) {
        queued = split_link_queue_push(&queue, VFX_RT_RELAY_CMD, data, len);
    }

    if (!queued) {
        LINK_STAT_LOG("split link: queue full, message of %d bytes refused", len);

        return split_link_chunks_for(len) == 0 ? -EINVAL : -EBUSY;
    }

    /* A no-op while the drain is already scheduled, so a burst of messages is
     * paced by the interval and not restarted by each one.
     */
    k_work_schedule_for_queue(zmk_workqueue_lowprio_work_q(), &drain_work, K_NO_WAIT);

    return 0;
}

bool split_link_room(uint8_t len) {
    if (ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT == 0) {
        return true;
    }

    ensure_ready();

    const uint8_t chunks = split_link_chunks_for(len);
    uint16_t room;

    K_SPINLOCK(&queue_lock) {
        room = split_link_queue_room(&queue);
    }

    return chunks != 0 && room >= chunks;
}

void split_link_get_stats(struct split_link_stats *out) {
    memset(out, 0, sizeof(*out));
    out->rx_ok = rx.ok;
    out->rx_incomplete = rx.incomplete;
    out->rx_orphan = rx.orphan;
    out->rx_malformed = rx.malformed;

    if (ready) {
        K_SPINLOCK(&queue_lock) {
            out->tx_refused = queue.refused;
            out->tx_high_water = queue.high_water;
            out->tx_queued = queue.count;
        }
    }

    out->tx_chunks = sent_chunks;
}

#else /* a peripheral, or a board that is not split */

int split_link_send(const uint8_t *data, uint8_t len) {
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    return 0;
}

bool split_link_room(uint8_t len) {
    ARG_UNUSED(len);

    return true;
}

void split_link_get_stats(struct split_link_stats *out) {
    memset(out, 0, sizeof(*out));
    out->rx_ok = rx.ok;
    out->rx_incomplete = rx.incomplete;
    out->rx_orphan = rx.orphan;
    out->rx_malformed = rx.malformed;
}

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
