/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/runtime_scene.h>
#include <zmk/vfx/scene_sync.h>
#include <zmk/vfx/scenes.h>
#include <zmk/vfx/split_link.h>
#include <zmk/vfx/sync_proto.h>
#include <zmk/workqueue.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* See scene_sync.h. The wire formats and the replay iterator are in
 * sync_proto.c and runtime_scene.c, where the host tests cover them; this file
 * is only the scheduling around them.
 */

static bool target_ok(uint8_t t) {
    return VFX_RT_TARGET_CH(t) < VFX_MAX_CHANNELS &&
           VFX_RT_TARGET_SCENE(t) < VFX_RT_SCENES_PER_CHANNEL;
}

static uint16_t sat16(uint32_t v) { return v > 0xFFFF ? 0xFFFF : (uint16_t)v; }

/* ---- the peripheral: answer a CHECK -------------------------------------- */

static uint16_t hash_ok;
static uint16_t hash_bad;

void zmk_vfx_sync_on_check(const uint8_t *msg, uint8_t len) {
    uint8_t target;
    uint32_t theirs;

    if (!vfx_sync_decode_check(msg, len, &target, &theirs)) {
        LOG_WRN("Malformed VFX sync check (%d bytes)", len);

        return;
    }

    struct split_link_stats stats;
    struct vfx_sync_report report = {.target = target};

    split_link_get_stats(&stats);

    report.hash = vfx_runtime_hash(target);
    report.match = report.hash == theirs;

    if (report.match) {
        hash_ok++;
    } else {
        hash_bad++;
        LOG_WRN("VFX scene 0x%02x differs from the central's (%08x vs %08x)", target,
                (unsigned)report.hash, (unsigned)theirs);
    }

    report.hash_ok = hash_ok;
    report.hash_bad = hash_bad;
    report.incomplete = sat16(stats.rx_incomplete);
    report.orphan = sat16(stats.rx_orphan);

    uint8_t wire[VFX_SYNC_REPORT_LEN];
    const uint8_t n = vfx_sync_encode_report(&report, wire);

    /* -ENOTSUP without the report channel: the log line above is all there is. */
    split_link_report(wire, n);
}

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

#include <zmk/split/central.h>

#define TARGETS (VFX_MAX_CHANNELS * VFX_RT_SCENES_PER_CHANNEL)
BUILD_ASSERT(TARGETS <= 64, "a peripheral's verdicts are kept as one bit per scene");

#define REPORTS IS_ENABLED(CONFIG_ZMK_VFX_SPLIT_REPORT)
#define AUTO_RESYNC IS_ENABLED(CONFIG_ZMK_VFX_SPLIT_AUTO_RESYNC)
#define MAX_ATTEMPTS 2

#if REPORTS
#define DEBOUNCE_MS CONFIG_ZMK_VFX_SPLIT_CHECK_DEBOUNCE_MS
#else
#define DEBOUNCE_MS 400
#endif

static uint8_t bit_of(uint8_t t) {
    return (uint8_t)(VFX_RT_TARGET_SCENE(t) * VFX_MAX_CHANNELS + VFX_RT_TARGET_CH(t));
}

static uint8_t target_of(uint8_t bit) {
    return VFX_RT_TARGET(bit % VFX_MAX_CHANNELS, bit / VFX_MAX_CHANNELS);
}

static uint64_t bitmask(uint8_t t) { return (uint64_t)1 << bit_of(t); }

static const uint64_t ALL_TARGETS = TARGETS == 64 ? ~(uint64_t)0 : (((uint64_t)1 << TARGETS) - 1);

static struct k_spinlock lock;
static struct k_work_delayable check_work;
static struct k_work_delayable replay_work;
static bool ready;

static uint64_t check_set;  /* targets waiting for a CHECK to be sent */
static uint64_t resync_set; /* targets waiting for a replay */
static uint8_t attempts[TARGETS];

struct peer {
    bool present;
    bool reported;
    uint64_t bad;     /* targets it last reported different */
    uint64_t pending; /* targets a CHECK was sent for and nothing came back yet */
    uint32_t hash;
};

static struct peer peers[VFX_HID_MAX_PEERS];

static bool replaying;
static uint8_t replay_target;
static struct vfx_rt_replay cursor;
static struct vfx_hid_request held;
static bool have_held;

static bool take(uint64_t *set, uint8_t *target) {
    bool got = false;

    K_SPINLOCK(&lock) {
        if (*set != 0) {
            for (uint8_t b = 0; b < TARGETS; b++) {
                if (*set & ((uint64_t)1 << b)) {
                    *set &= ~((uint64_t)1 << b);
                    *target = target_of(b);
                    got = true;
                    break;
                }
            }
        }
    }

    return got;
}

static void set_bits(uint64_t *set, uint64_t bits) {
    K_SPINLOCK(&lock) {
        *set |= bits;
    }
}

static uint64_t bits_for(uint8_t target) {
    return target == VFX_RT_NONE ? ALL_TARGETS : bitmask(target);
}

static void retry_delay(struct k_work_delayable *w) {
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), w,
                                K_MSEC(4 * CONFIG_ZMK_VFX_SPLIT_LINK_CHUNK_INTERVAL_MS));
}

static void check_handler(struct k_work *work) {
    ARG_UNUSED(work);

    uint8_t target;

    while (take(&check_set, &target)) {
        uint8_t msg[VFX_SYNC_CHECK_LEN];

        vfx_sync_encode_check(target, vfx_runtime_hash(target), msg);

        if (split_link_send(msg, sizeof(msg)) != 0) {
            set_bits(&check_set, bitmask(target));
            retry_delay(&check_work);

            return;
        }

        K_SPINLOCK(&lock) {
            for (uint8_t i = 0; i < VFX_HID_MAX_PEERS; i++) {
                if (peers[i].present) {
                    peers[i].pending |= bitmask(target);
                }
            }
        }
    }
}

static void finish_replay(void) {
    const uint8_t target = replay_target;

    K_SPINLOCK(&lock) {
        replaying = false;
    }

    /* Behind the replay in the link's queue, so it is judged against what the
     * replay built.
     */
    set_bits(&check_set, bitmask(target));
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_work, K_NO_WAIT);
}

static void replay_handler(struct k_work *work) {
    ARG_UNUSED(work);

    if (!replaying) {
        if (!take(&resync_set, &replay_target)) {
            return;
        }

        vfx_runtime_replay_begin(&cursor, replay_target);
        have_held = false;

        K_SPINLOCK(&lock) {
            replaying = true;
        }
    }

    for (;;) {
        if (!have_held) {
            if (!vfx_runtime_replay_next(&cursor, &held)) {
                finish_replay();
                break;
            }

            have_held = true;
        }

        uint8_t wire[VFX_HID_MAX_REQUEST_LEN];
        const uint8_t n = vfx_hid_encode_request(&held, wire);

        if (n > 0 && split_link_send(wire, n) == -EBUSY) {
            retry_delay(&replay_work);

            return;
        }

        /* Anything else, including a request the link cannot carry, is dropped:
         * the next CHECK reports what that cost.
         */
        have_held = false;
    }

    /* Whatever is still waiting. */
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &replay_work, K_NO_WAIT);
}

static void ensure_ready(void) {
    if (!ready) {
        k_work_init_delayable(&check_work, check_handler);
        k_work_init_delayable(&replay_work, replay_handler);
        ready = true;
    }
}

static int queue_check(uint8_t target, bool now) {
    if (target != VFX_RT_NONE && !target_ok(target)) {
        return -EINVAL;
    }

    ensure_ready();
    set_bits(&check_set, bits_for(target));
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &check_work,
                                now ? K_NO_WAIT : K_MSEC(DEBOUNCE_MS));

    return 0;
}

int zmk_vfx_sync_verify(uint8_t target) { return queue_check(target, true); }

int zmk_vfx_sync_resync(uint8_t target) {
    if (target != VFX_RT_NONE && !target_ok(target)) {
        return -EINVAL;
    }

    ensure_ready();

    K_SPINLOCK(&lock) {
        resync_set |= bits_for(target);

        for (uint8_t i = 0; i < TARGETS; i++) {
            attempts[i] = 0;
        }
    }

    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &replay_work, K_NO_WAIT);

    return 0;
}

void zmk_vfx_sync_touch(uint8_t target) {
    if (!REPORTS || ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT == 0 || !target_ok(target)) {
        return;
    }

    K_SPINLOCK(&lock) {
        attempts[bit_of(target)] = 0;
    }

    queue_check(target, false);
}

bool zmk_vfx_sync_busy(void) {
    bool busy;

    K_SPINLOCK(&lock) {
        busy = replaying || resync_set != 0;
    }

    return busy;
}

static uint8_t peer_state(const struct peer *p) {
    if (!p->present) {
        return VFX_HID_PEER_NONE;
    }

    if (replaying) {
        return VFX_HID_PEER_RESYNCING;
    }

    if (p->bad != 0) {
        return VFX_HID_PEER_MISMATCH;
    }

    return p->reported && p->pending == 0 ? VFX_HID_PEER_MATCH : VFX_HID_PEER_UNKNOWN;
}

void zmk_vfx_sync_get(struct zmk_vfx_sync_info *out) {
    struct split_link_stats stats;

    memset(out, 0, sizeof(*out));
    split_link_get_stats(&stats);

    out->features = REPORTS ? VFX_HID_SYNC_RETURN_CHANNEL : 0;
    out->queue_high_water = stats.tx_high_water;
    out->refused = stats.tx_refused;

    K_SPINLOCK(&lock) {
        out->replaying = replaying;

        for (uint8_t i = 0; i < VFX_HID_MAX_PEERS; i++) {
            out->peers[i].state = peer_state(&peers[i]);
            out->peers[i].hash = peers[i].hash;
        }
    }
}

/* ---- what the report channel tells the central --------------------------- */

static void on_report(uint8_t peripheral, const uint8_t *data, uint8_t len) {
    struct vfx_sync_report r;

    if (peripheral >= VFX_HID_MAX_PEERS || !vfx_sync_decode_report(data, len, &r) ||
        !target_ok(r.target)) {
        return;
    }

    bool resync = false;

    K_SPINLOCK(&lock) {
        struct peer *p = &peers[peripheral];
        const uint64_t bit = bitmask(r.target);

        p->reported = true;
        p->hash = r.hash;
        p->pending &= ~bit;

        if (r.match) {
            p->bad &= ~bit;
            attempts[bit_of(r.target)] = 0;
        } else {
            p->bad |= bit;

            if (AUTO_RESYNC && attempts[bit_of(r.target)] < MAX_ATTEMPTS) {
                attempts[bit_of(r.target)]++;
                resync_set |= bit;
                resync = true;
            }
        }
    }

    if (!r.match) {
        LOG_WRN("VFX peripheral %d disagrees about scene 0x%02x%s", peripheral, r.target,
                resync ? ", rebuilding it" : "");
    }

    if (resync) {
        k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &replay_work, K_NO_WAIT);
    }
}

static void on_peripheral(uint8_t peripheral, bool present) {
    if (peripheral >= VFX_HID_MAX_PEERS) {
        return;
    }

    K_SPINLOCK(&lock) {
        memset(&peers[peripheral], 0, sizeof(peers[peripheral]));
        peers[peripheral].present = present;
    }

    /* A peripheral that has just appeared may have missed anything since it was
     * last here, or have run on without the central.
     */
    if (present) {
        queue_check(VFX_RT_NONE, false);
    }
}

#else /* a peripheral, or a board that is not split */

int zmk_vfx_sync_verify(uint8_t target) {
    ARG_UNUSED(target);

    return 0;
}

int zmk_vfx_sync_resync(uint8_t target) {
    ARG_UNUSED(target);

    return 0;
}

void zmk_vfx_sync_touch(uint8_t target) { ARG_UNUSED(target); }

bool zmk_vfx_sync_busy(void) { return false; }

void zmk_vfx_sync_get(struct zmk_vfx_sync_info *out) { memset(out, 0, sizeof(*out)); }

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */

static int scene_sync_init(void) {
#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)
    split_link_set_report_cb(on_report);
    split_link_set_peripheral_cb(on_peripheral);
#endif

    return 0;
}

SYS_INIT(scene_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
