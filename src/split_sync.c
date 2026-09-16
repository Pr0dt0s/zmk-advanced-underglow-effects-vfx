/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <dt-bindings/zmk/vfx.h>
#include <zmk/behavior.h>
#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>
#include <zmk/split/central.h>
#include <zmk/vfx/sync.h>
#include <zmk/vfx/vfx.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Central half of synchronised split mode.
 *
 * There is no custom GATT service here on purpose. ZMK already relays
 * behavior invocations to every peripheral, and exposes the entry point
 * publicly, so a beacon is just an invocation of the &vfx behavior that no
 * keymap contains. That avoids discovery, a service UUID, and a second
 * connection state to keep track of.
 *
 * Only the central runs any of this; the peripheral simply receives &vfx
 * bindings and handles them like any other.
 */

#if IS_ENABLED(CONFIG_ZMK_SPLIT_ROLE_CENTRAL)

static void send_to_peripherals(uint32_t param1, uint32_t param2, uint32_t position) {
    struct zmk_behavior_binding binding = {
        .behavior_dev = "vfx",
        .param1 = param1,
        .param2 = param2,
    };

    struct zmk_behavior_binding_event event = {
        .position = position,
        .timestamp = k_uptime_get(),
    };

    for (int i = 0; i < ZMK_SPLIT_CENTRAL_PERIPHERAL_COUNT; i++) {
        zmk_split_central_invoke_behavior(i, &binding, event, true);
    }
}

static void sync_beacon(struct k_work *work) {
    ARG_UNUSED(work);

    /* The central's own timebase, which is what the peripheral aligns to.
     * Sending uptime rather than a delta means a peripheral that missed
     * beacons, or rebooted, converges from whatever it currently believes.
     */
    send_to_peripherals(VFX_SYNC_CMD, (uint32_t)k_uptime_get(), 0);
}

K_WORK_DEFINE(sync_beacon_work, sync_beacon);

static void sync_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);

    k_work_submit(&sync_beacon_work);
}

K_TIMER_DEFINE(sync_timer, sync_timer_handler, NULL);

#if IS_ENABLED(CONFIG_ZMK_VFX_SYNC_RELAY_KEYS)
static int relay_key_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);

    if (pos == NULL || !pos->state) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    /* Only the central's own keys need relaying. A press on the peripheral
     * already reached the peripheral locally, and bouncing it back would
     * draw the ripple there twice.
     */
    if (pos->source != ZMK_POSITION_STATE_CHANGE_SOURCE_LOCAL) {
        return ZMK_EV_EVENT_BUBBLE;
    }

    send_to_peripherals(VFX_KEY_CMD, pos->position, pos->position);

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_vfx_sync, relay_key_listener);
ZMK_SUBSCRIPTION(zmk_vfx_sync, zmk_position_state_changed);
#endif

static int vfx_split_sync_init(void) {
    k_timer_start(&sync_timer, K_MSEC(CONFIG_ZMK_VFX_SYNC_INTERVAL_MS),
                  K_MSEC(CONFIG_ZMK_VFX_SYNC_INTERVAL_MS));

    LOG_INF("VFX split sync active, beaconing every %d ms", CONFIG_ZMK_VFX_SYNC_INTERVAL_MS);

    return 0;
}

SYS_INIT(vfx_split_sync_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);

#endif /* CONFIG_ZMK_SPLIT_ROLE_CENTRAL */
