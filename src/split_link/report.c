/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <errno.h>
#include <string.h>

#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <zmk/vfx/split_link.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* The way back: a peripheral reports to the central over a GATT service of its
 * own. ZMK's split transport carries nothing from a peripheral to the central
 * except what ZMK itself defines, so this is a separate notify characteristic
 * that the central finds and subscribes to by itself on every split connection.
 * It touches no ZMK internals, only Zephyr's Bluetooth API.
 *
 * Experimental: see CONFIG_ZMK_VFX_SPLIT_REPORT. Nothing here may include
 * runtime_scene.h, hid_protocol.h or vfx.h.
 */

static void (*report_cb)(uint8_t peripheral, const uint8_t *data, uint8_t len);
static void (*peripheral_cb)(uint8_t peripheral, bool present);

void split_link_set_report_cb(void (*cb)(uint8_t peripheral, const uint8_t *data, uint8_t len)) {
    report_cb = cb;
}

void split_link_set_peripheral_cb(void (*cb)(uint8_t peripheral, bool present)) {
    peripheral_cb = cb;
}

#if !IS_ENABLED(CONFIG_ZMK_VFX_SPLIT_REPORT)

int split_link_report(const uint8_t *data, uint8_t len) {
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    return -ENOTSUP;
}

#else

#include <zephyr/bluetooth/bluetooth.h>
#include <zephyr/bluetooth/conn.h>
#include <zephyr/bluetooth/gatt.h>
#include <zephyr/bluetooth/uuid.h>

#include <zmk/workqueue.h>

#define SERVICE_UUID BT_UUID_128_ENCODE(0x6d1a0001, 0x7a5c, 0x4c7e, 0x9b1d, 0x5a1e5f0c2b10)
#define REPORT_UUID BT_UUID_128_ENCODE(0x6d1a0002, 0x7a5c, 0x4c7e, 0x9b1d, 0x5a1e5f0c2b10)

static struct bt_uuid_128 service_uuid = BT_UUID_INIT_128(SERVICE_UUID);
static struct bt_uuid_128 report_uuid = BT_UUID_INIT_128(REPORT_UUID);

#if !IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

/* ---- the peripheral: a notify characteristic ----------------------------- */

static void ccc_changed(const struct bt_gatt_attr *attr, uint16_t value) {
    ARG_UNUSED(attr);

    LOG_INF("VFX report channel %s", value == BT_GATT_CCC_NOTIFY ? "subscribed" : "unsubscribed");
}

BT_GATT_SERVICE_DEFINE(vfx_report_svc, BT_GATT_PRIMARY_SERVICE(&service_uuid),
                       BT_GATT_CHARACTERISTIC(&report_uuid.uuid, BT_GATT_CHRC_NOTIFY,
                                              BT_GATT_PERM_NONE, NULL, NULL, NULL),
                       BT_GATT_CCC(ccc_changed, BT_GATT_PERM_READ | BT_GATT_PERM_WRITE));

int split_link_report(const uint8_t *data, uint8_t len) {
    if (len > SPLIT_LINK_MAX_MSG) {
        return -EINVAL;
    }

    /* To whoever subscribed, which is the central. */
    return bt_gatt_notify(NULL, &vfx_report_svc.attrs[1], data, len);
}

#else

/* ---- the central: find the characteristic and subscribe ------------------ */

#define MAX_PERIPHERALS 3
#define FIRST_ATTEMPT_MS 5000 /* let ZMK finish discovering its own service first */
#define RETRY_MS 2000
#define MAX_ATTEMPTS 30

enum phase { PH_SERVICE, PH_CHARACTERISTIC, PH_CCC };

struct peripheral {
    struct bt_conn *conn;
    struct k_work_delayable work;
    struct bt_gatt_discover_params discover;
    struct bt_gatt_subscribe_params subscribe;
    enum phase phase;
    uint16_t value_handle;
    uint8_t attempts;
    bool subscribed;
};

static struct peripheral peripherals[MAX_PERIPHERALS];

struct inbound {
    uint8_t peripheral;
    uint8_t len;
    uint8_t data[SPLIT_LINK_MAX_MSG];
};

#define INBOUND_SLOTS 8

static struct inbound inbox[INBOUND_SLOTS];
static uint8_t inbox_head;
static uint8_t inbox_count;
static struct k_spinlock inbox_lock;
static struct k_work inbox_work;

static uint8_t index_of(const struct peripheral *p) { return (uint8_t)(p - peripherals); }

static struct peripheral *by_conn(const struct bt_conn *conn) {
    for (int i = 0; i < MAX_PERIPHERALS; i++) {
        if (peripherals[i].conn == conn) {
            return &peripherals[i];
        }
    }

    return NULL;
}

static void inbox_handler(struct k_work *work) {
    ARG_UNUSED(work);

    for (;;) {
        struct inbound in;
        bool got = false;

        K_SPINLOCK(&inbox_lock) {
            if (inbox_count > 0) {
                in = inbox[inbox_head];
                inbox_head = (inbox_head + 1) % INBOUND_SLOTS;
                inbox_count--;
                got = true;
            }
        }

        if (!got) {
            return;
        }

        if (report_cb) {
            report_cb(in.peripheral, in.data, in.len);
        }
    }
}

static uint8_t notify_cb(struct bt_conn *conn, struct bt_gatt_subscribe_params *params,
                         const void *data, uint16_t length) {
    struct peripheral *p = by_conn(conn);

    if (!data) {
        LOG_INF("VFX report channel unsubscribed");

        return BT_GATT_ITER_STOP;
    }

    if (p && length > 0 && length <= SPLIT_LINK_MAX_MSG) {
        /* The Bluetooth RX thread: copy and get out. */
        K_SPINLOCK(&inbox_lock) {
            if (inbox_count < INBOUND_SLOTS) {
                struct inbound *in = &inbox[(inbox_head + inbox_count) % INBOUND_SLOTS];

                in->peripheral = index_of(p);
                in->len = (uint8_t)length;
                memcpy(in->data, data, length);
                inbox_count++;
            }
        }

        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &inbox_work);
    }

    return BT_GATT_ITER_CONTINUE;
}

static void retry(struct peripheral *p) {
    if (++p->attempts > MAX_ATTEMPTS) {
        LOG_WRN("VFX report channel: giving up on peripheral %d", index_of(p));

        return;
    }

    p->phase = PH_SERVICE;
    k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &p->work, K_MSEC(RETRY_MS));
}

static void subscribe(struct peripheral *p, uint16_t ccc_handle) {
    memset(&p->subscribe, 0, sizeof(p->subscribe));
    p->subscribe.notify = notify_cb;
    p->subscribe.value = BT_GATT_CCC_NOTIFY;
    p->subscribe.value_handle = p->value_handle;
    p->subscribe.ccc_handle = ccc_handle;
    atomic_set_bit(p->subscribe.flags, BT_GATT_SUBSCRIBE_FLAG_VOLATILE);

    const int err = bt_gatt_subscribe(p->conn, &p->subscribe);

    if (err && err != -EALREADY) {
        LOG_WRN("VFX report subscribe failed (%d)", err);
        retry(p);

        return;
    }

    p->subscribed = true;
    LOG_INF("VFX report channel up on peripheral %d", index_of(p));

    if (peripheral_cb) {
        peripheral_cb(index_of(p), true);
    }
}

static uint8_t discover_cb(struct bt_conn *conn, const struct bt_gatt_attr *attr,
                           struct bt_gatt_discover_params *params) {
    struct peripheral *p = by_conn(conn);

    if (!p) {
        return BT_GATT_ITER_STOP;
    }

    if (!attr) {
        /* Not found on this connection (yet). */
        retry(p);

        return BT_GATT_ITER_STOP;
    }

    switch (p->phase) {
    case PH_SERVICE: {
        const struct bt_gatt_service_val *svc = attr->user_data;

        p->discover.start_handle = attr->handle + 1;
        p->discover.end_handle = svc->end_handle;
        p->discover.uuid = &report_uuid.uuid;
        p->discover.type = BT_GATT_DISCOVER_CHARACTERISTIC;
        p->phase = PH_CHARACTERISTIC;
        break;
    }

    case PH_CHARACTERISTIC:
        p->value_handle = bt_gatt_attr_value_handle(attr);
        p->discover.start_handle = p->value_handle + 1;
        p->discover.uuid = BT_UUID_GATT_CCC;
        p->discover.type = BT_GATT_DISCOVER_DESCRIPTOR;
        p->phase = PH_CCC;
        break;

    case PH_CCC:
        subscribe(p, attr->handle);

        return BT_GATT_ITER_STOP;
    }

    const int err = bt_gatt_discover(conn, &p->discover);

    if (err) {
        retry(p);
    }

    return BT_GATT_ITER_STOP;
}

static int run_discovery(struct peripheral *p) {
    memset(&p->discover, 0, sizeof(p->discover));
    p->discover.uuid = &service_uuid.uuid;
    p->discover.func = discover_cb;
    p->discover.start_handle = BT_ATT_FIRST_ATTRIBUTE_HANDLE;
    p->discover.end_handle = BT_ATT_LAST_ATTRIBUTE_HANDLE;
    p->discover.type = BT_GATT_DISCOVER_PRIMARY;
    p->phase = PH_SERVICE;

    return bt_gatt_discover(p->conn, &p->discover);
}

static void discovery_handler(struct k_work *work) {
    struct k_work_delayable *d = k_work_delayable_from_work(work);
    struct peripheral *p = CONTAINER_OF(d, struct peripheral, work);

    if (!p->conn || p->subscribed) {
        return;
    }

    /* -EBUSY while ZMK's own discovery holds the connection: try again later. */
    if (run_discovery(p) != 0) {
        retry(p);
    }
}

static void connected(struct bt_conn *conn, uint8_t err) {
    struct bt_conn_info info;

    if (err || bt_conn_get_info(conn, &info) != 0 || info.role != BT_CONN_ROLE_CENTRAL) {
        return;
    }

    if (by_conn(conn)) {
        return;
    }

    for (int i = 0; i < MAX_PERIPHERALS; i++) {
        struct peripheral *p = &peripherals[i];

        if (!p->conn) {
            p->conn = bt_conn_ref(conn);
            p->attempts = 0;
            p->subscribed = false;
            k_work_reschedule_for_queue(zmk_workqueue_lowprio_work_q(), &p->work,
                                        K_MSEC(FIRST_ATTEMPT_MS));

            return;
        }
    }

    LOG_WRN("VFX report channel: more peripherals than slots");
}

static void disconnected(struct bt_conn *conn, uint8_t reason) {
    ARG_UNUSED(reason);

    struct peripheral *p = by_conn(conn);

    if (!p) {
        return;
    }

    struct bt_conn *held = p->conn;
    const bool was = p->subscribed;

    k_work_cancel_delayable(&p->work);
    p->conn = NULL;
    p->subscribed = false;

    if (was && peripheral_cb) {
        peripheral_cb(index_of(p), false);
    }

    bt_conn_unref(held);
}

BT_CONN_CB_DEFINE(vfx_report_conn_cb) = {
    .connected = connected,
    .disconnected = disconnected,
};

static int report_init(void) {
    k_work_init(&inbox_work, inbox_handler);

    for (int i = 0; i < MAX_PERIPHERALS; i++) {
        k_work_init_delayable(&peripherals[i].work, discovery_handler);
    }

    return 0;
}

SYS_INIT(report_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

int split_link_report(const uint8_t *data, uint8_t len) {
    ARG_UNUSED(data);
    ARG_UNUSED(len);

    return -ENOTSUP; /* the central has nobody to report to */
}

#endif /* central */

#endif /* CONFIG_ZMK_VFX_SPLIT_REPORT */
