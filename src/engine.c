/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/drivers/led_strip.h>
#include <zephyr/init.h>
#include <zephyr/kernel.h>
#include <zephyr/logging/log.h>

#include <string.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/power.h>
#include <zmk/vfx/scenes.h>
#include <zmk/vfx/status.h>
#include <zmk/vfx/sync.h>
#include <zmk/vfx/vfx.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#include <zmk/event_manager.h>
#include <zmk/events/position_state_changed.h>

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
#include <zmk/activity.h>
#include <zmk/events/activity_state_changed.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_INDICATORS)
#include <zmk/events/battery_state_changed.h>
#include <zmk/events/layer_state_changed.h>
#include <zmk/keymap.h>
#if IS_ENABLED(CONFIG_ZMK_BLE)
#include <zmk/ble.h>
#include <zmk/events/ble_active_profile_changed.h>
#endif
#if IS_ENABLED(CONFIG_ZMK_CAPS_WORD) || IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
#include <zmk/events/caps_word_state_changed.h>
#endif
#endif

LOG_MODULE_REGISTER(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "ZMK VFX needs a zmk,underglow chosen node pointing at an LED strip."
#endif

#if !DT_HAS_COMPAT_STATUS_OKAY(zmk_vfx_engine)
#error "ZMK VFX is enabled but no zmk,vfx-engine node is declared."
#endif

BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW),
             "ZMK VFX and CONFIG_ZMK_RGB_UNDERGLOW both drive the same strip. "
             "Set CONFIG_ZMK_RGB_UNDERGLOW=n.");

#define STRIP_NODE DT_CHOSEN(zmk_underglow)
#define STRIP_NUM_PIXELS DT_PROP(STRIP_NODE, chain_length)

#define VFX_FPS DT_PROP_OR(VFX_ENGINE_NODE, fps, CONFIG_ZMK_VFX_FPS)
#define VFX_FRAME_MS (1000 / VFX_FPS)
#define VFX_VIRTUAL_LENGTH DT_PROP_OR(VFX_ENGINE_NODE, virtual_length, STRIP_NUM_PIXELS)
#define VFX_STRIP_OFFSET DT_PROP(VFX_ENGINE_NODE, strip_offset)

#if DT_NODE_HAS_PROP(VFX_ENGINE_NODE, key_pixels)
static const uint8_t key_pixels[] = DT_PROP(VFX_ENGINE_NODE, key_pixels);
#define VFX_KEY_PIXELS key_pixels
#define VFX_NUM_KEYS ((uint16_t)ARRAY_SIZE(key_pixels))
#else
/* No map given: vfx_key_pixel() spreads key positions evenly over the strip.
 * Reactive effects still animate, they just will not line up with the keys.
 */
#define VFX_KEY_PIXELS NULL
#define VFX_NUM_KEYS 0
#endif

static const struct device *const led_strip = DEVICE_DT_GET(STRIP_NODE);

static struct led_rgb strip_pixels[STRIP_NUM_PIXELS];
static struct vfx_rgb frame[STRIP_NUM_PIXELS];

static struct {
    uint8_t scene;
    uint8_t brightness; /* 0-255 */
    uint8_t speed;      /* 1-5 */
    int16_t hue_shift;
    int32_t time_offset;
    bool on;
} state;

static bool frame_dirty = true;

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_POWER_GATE)
static struct vfx_power_ctl power_ctl;

static const struct vfx_power_policy power_policy = {
    .blackout_delay_ms = CONFIG_ZMK_VFX_BLACKOUT_DELAY_MS,
    .settle_ms = CONFIG_ZMK_VFX_POWER_SETTLE_MS,
};
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_SKIP_UNCHANGED_FRAMES)
static struct vfx_rgb last_sent[STRIP_NUM_PIXELS];
static bool last_sent_valid;
#endif

static uint8_t brightness_ceiling(uint8_t v) {
    const uint16_t ceiling = (uint16_t)CONFIG_ZMK_VFX_BRT_MAX * 255U / 100U;

    return v > ceiling ? (uint8_t)ceiling : v;
}

static struct vfx_frame_ctx build_ctx(void) {
    return (struct vfx_frame_ctx){
        .time_ms = (uint32_t)((int64_t)k_uptime_get() + state.time_offset),
        .virtual_length = VFX_VIRTUAL_LENGTH,
        .strip_offset = VFX_STRIP_OFFSET,
        .num_pixels = STRIP_NUM_PIXELS,
        .speed = state.speed,
        .brightness = brightness_ceiling(state.brightness),
        .hue_shift = state.hue_shift,
        .key_pixels = VFX_KEY_PIXELS,
        .num_keys = VFX_NUM_KEYS,
    };
}

static void push_frame(void) {
    for (uint16_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        strip_pixels[i].r = frame[i].r;
        strip_pixels[i].g = frame[i].g;
        strip_pixels[i].b = frame[i].b;
    }

    int err = led_strip_update_rgb(led_strip, strip_pixels, STRIP_NUM_PIXELS);
    if (err < 0) {
        LOG_ERR("Failed to update the LED strip (%d)", err);
    }
}

/* Pushing a frame is a blocking SPI transfer, and turning the underglow off
 * happens from a behavior on the main work queue. Hand it to the low priority
 * queue rather than stalling keymap processing, which is what ZMK core does.
 */
static void blank_strip_handler(struct k_work *work) {
    ARG_UNUSED(work);

    for (uint16_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        frame[i] = VFX_RGB_BLACK;
    }

    push_frame();
}

K_WORK_DEFINE(vfx_blank_work, blank_strip_handler);

static void blank_strip(void) {
    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &vfx_blank_work);
}

static void vfx_tick(struct k_work *work) {
    ARG_UNUSED(work);

    if (!state.on) {
        return;
    }

    const struct vfx_frame_ctx ctx = build_ctx();
    const struct vfx_scene *scene = vfx_scene_get(state.scene);
    bool any_lit = false;

    vfx_render_frame(scene, &ctx, frame, &any_lit);
    frame_dirty = false;

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_POWER_GATE)
    switch (vfx_power_step(&power_ctl, &power_policy, any_lit, VFX_FRAME_MS)) {
    case VFX_POWER_SKIP:
        /* Rail is down or still settling. We rendered anyway, which is how
         * the first lit frame gets noticed, but the bus stays quiet.
         */
        return;

    case VFX_POWER_GATE_OFF:
        /* The frame is already black, so sending it is what actually turns
         * the LEDs off before the supply goes; without it they would hold
         * their last colour until the rail decayed.
         */
        push_frame();
        vfx_power_rail_disable();
        LOG_DBG("LED rail gated off after %d ms of black frames",
                CONFIG_ZMK_VFX_BLACKOUT_DELAY_MS);
        return;

    case VFX_POWER_WAKE:
        vfx_power_rail_enable();
        LOG_DBG("LED rail woken by a lit frame");
        return;

    case VFX_POWER_TRANSMIT:
        break;
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_SKIP_UNCHANGED_FRAMES)
    if (last_sent_valid && memcmp(last_sent, frame, sizeof(frame)) == 0) {
        return;
    }

    memcpy(last_sent, frame, sizeof(frame));
    last_sent_valid = true;
#endif

    push_frame();
}

K_WORK_DEFINE(vfx_tick_work, vfx_tick);

static void vfx_timer_handler(struct k_timer *timer) {
    ARG_UNUSED(timer);

    if (!state.on) {
        return;
    }

    /* A scene with nothing moving does not need the strip rewritten 50 times a
     * second. Keep a slow heartbeat so an external change still lands promptly.
     */
    if (!frame_dirty) {
        const struct vfx_frame_ctx ctx = build_ctx();

        bool idle = !vfx_scene_is_animating(vfx_scene_get(state.scene), &ctx);

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_POWER_GATE)
        /* A scene can be static and black, in which case the gate still has a
         * countdown to finish. Parking now would leave the rail powered for a
         * frame that will never be drawn again.
         */
        idle = idle && vfx_power_is_stable(&power_ctl);
#endif

        if (idle) {
            /* Nothing left to animate: stop waking the CPU 50 times a second.
             * zmk_vfx_request_frame() restarts us when something changes.
             */
            k_timer_stop(&vfx_timer);
            return;
        }
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &vfx_tick_work);
}

K_TIMER_DEFINE(vfx_timer, vfx_timer_handler, NULL);

#if IS_ENABLED(CONFIG_SETTINGS)
static struct k_work_delayable save_work;

static void vfx_save_work_handler(struct k_work *work) {
    ARG_UNUSED(work);

    settings_save_one("vfx/state", &state, sizeof(state));
}

static int vfx_settings_set(const char *name, size_t len, settings_read_cb read_cb, void *cb_arg) {
    const char *next;

    if (!settings_name_steq(name, "state", &next) || next) {
        return -ENOENT;
    }

    if (len != sizeof(state)) {
        /* Layout changed between firmware versions; fall back to the defaults
         * rather than reinterpreting old bytes as a new struct.
         */
        return -EINVAL;
    }

    int rc = read_cb(cb_arg, &state, sizeof(state));
    if (rc < 0) {
        return rc;
    }

    /* A saved scene index can point past the end if scenes were removed. */
    if (state.scene >= vfx_scene_count()) {
        state.scene = 0;
    }

    /* The split time offset is a live correction against the other half, not
     * something to carry across a reboot: a stale one would skew the timebase
     * from the first frame.
     */
    state.time_offset = 0;

    frame_dirty = true;

    if (state.on) {
        k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));
    }

    return 0;
}

SETTINGS_STATIC_HANDLER_DEFINE(zmk_vfx, "vfx", NULL, vfx_settings_set, NULL, NULL);
#endif

int zmk_vfx_save_state(void) {
#if IS_ENABLED(CONFIG_SETTINGS)
    return MIN(k_work_reschedule(&save_work, K_MSEC(CONFIG_ZMK_SETTINGS_SAVE_DEBOUNCE)), 0);
#else
    return 0;
#endif
}

void zmk_vfx_request_frame(void) {
    frame_dirty = true;

    /* Restarting rather than just submitting: the timer parks itself once a
     * scene stops animating, so a switch to an animated scene has to wake it.
     */
    if (state.on) {
        k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));
    }
}

int zmk_vfx_on(void) {
    if (!device_is_ready(led_strip)) {
        return -ENODEV;
    }

    state.on = true;
    frame_dirty = true;

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_POWER_GATE)
    vfx_power_reset(&power_ctl);
    vfx_power_rail_enable();
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_SKIP_UNCHANGED_FRAMES)
    /* The strip lost its state while the rail was down, so the next frame has
     * to go out even if it matches what we last sent.
     */
    last_sent_valid = false;
#endif

    k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));

    return zmk_vfx_save_state();
}

int zmk_vfx_off(void) {
    if (!device_is_ready(led_strip)) {
        return -ENODEV;
    }

    state.on = false;
    k_timer_stop(&vfx_timer);
    blank_strip();

#if IS_ENABLED(CONFIG_ZMK_VFX_SKIP_UNCHANGED_FRAMES)
    last_sent_valid = false;
#endif

    return zmk_vfx_save_state();
}

int zmk_vfx_toggle(void) { return state.on ? zmk_vfx_off() : zmk_vfx_on(); }

bool zmk_vfx_is_on(void) { return state.on; }

int zmk_vfx_select_scene(uint8_t index) {
    if (index >= vfx_scene_count()) {
        return -EINVAL;
    }

    state.scene = index;

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_POWER_GATE)
    /* A new scene must not inherit the previous one's blackout countdown. */
    vfx_power_reset(&power_ctl);
#endif

    zmk_vfx_request_frame();

    return zmk_vfx_save_state();
}

int zmk_vfx_cycle_scene(int direction) {
    return zmk_vfx_select_scene(zmk_vfx_calc_scene(direction));
}

uint8_t zmk_vfx_current_scene(void) { return state.scene; }

const char *zmk_vfx_scene_name(uint8_t index) {
    const struct vfx_scene *scene = vfx_scene_get(index);

    return scene ? scene->name : NULL;
}

#define VFX_BRT_STEP (255 / 10)
#define VFX_HUE_STEP 10

uint8_t zmk_vfx_calc_scene(int direction) {
    const uint8_t count = vfx_scene_count();

    return (uint8_t)(((int)state.scene + count + direction) % count);
}

uint8_t zmk_vfx_calc_brightness(int direction) {
    return (uint8_t)CLAMP((int)state.brightness + direction * VFX_BRT_STEP, 0, 255);
}

uint8_t zmk_vfx_calc_speed(int direction) {
    return (uint8_t)CLAMP((int)state.speed + direction, 1, 5);
}

uint16_t zmk_vfx_calc_hue(int direction) {
    return vfx_hue_add((uint16_t)state.hue_shift, (int16_t)(direction * VFX_HUE_STEP));
}

int zmk_vfx_set_brightness(uint8_t value) {
    state.brightness = value;
    zmk_vfx_request_frame();

    return zmk_vfx_save_state();
}

int zmk_vfx_set_speed(uint8_t value) {
    state.speed = (uint8_t)CLAMP(value, 1, 5);
    zmk_vfx_request_frame();

    return zmk_vfx_save_state();
}

int zmk_vfx_set_hue(uint16_t degrees) {
    /* Kept in 0-359 so the stored value never depends on how it was reached. */
    state.hue_shift = (int16_t)(degrees % 360);
    zmk_vfx_request_frame();

    return zmk_vfx_save_state();
}

int zmk_vfx_change_brightness(int direction) {
    return zmk_vfx_set_brightness(zmk_vfx_calc_brightness(direction));
}

int zmk_vfx_change_speed(int direction) { return zmk_vfx_set_speed(zmk_vfx_calc_speed(direction)); }

int zmk_vfx_change_hue(int direction) { return zmk_vfx_set_hue(zmk_vfx_calc_hue(direction)); }

uint8_t zmk_vfx_get_brightness(void) { return state.brightness; }
uint8_t zmk_vfx_get_speed(void) { return state.speed; }
int16_t zmk_vfx_get_hue_shift(void) { return state.hue_shift; }

void zmk_vfx_set_time_offset(int32_t offset_ms) { state.time_offset = offset_ms; }
int32_t zmk_vfx_get_time_offset(void) { return state.time_offset; }

void zmk_vfx_apply_sync(uint32_t central_time_ms) {
    const int32_t desired = vfx_sync_desired(central_time_ms, (uint32_t)k_uptime_get());
    const int32_t next = vfx_sync_step(state.time_offset, desired);

    if (next == state.time_offset) {
        return;
    }

    LOG_DBG("VFX timebase %d -> %d ms (central wants %d)", state.time_offset, next, desired);

    state.time_offset = next;

    /* The offset only shows up in the next rendered frame, and a scene that
     * had gone idle would not render one.
     */
    zmk_vfx_request_frame();
}

void zmk_vfx_inject_key(uint32_t position) {
    const struct vfx_frame_ctx ctx = build_ctx();

    vfx_scene_key_event(vfx_scene_get(state.scene), &ctx, position, true, ctx.time_ms);
    zmk_vfx_request_frame();
}

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
static bool on_before_idle;

static int vfx_auto_off(bool awake) {
    if (awake) {
        return on_before_idle ? zmk_vfx_on() : 0;
    }

    on_before_idle = state.on;

    return state.on ? zmk_vfx_off() : 0;
}
#endif

static int vfx_event_listener(const zmk_event_t *eh) {
    const struct zmk_position_state_changed *pos = as_zmk_position_state_changed(eh);

    if (pos != NULL) {
        /* Hand the press to the scene's reactive layers, stamped with the
         * engine timebase rather than raw uptime so that a synced peripheral
         * places it correctly, then ask for a frame: a scene that is purely
         * reactive is idle until exactly this moment, and would otherwise
         * stay parked with the rail gated.
         */
        const struct vfx_frame_ctx ctx = build_ctx();

        vfx_scene_key_event(vfx_scene_get(state.scene), &ctx, pos->position, pos->state,
                            ctx.time_ms);

        if (pos->state) {
            zmk_vfx_request_frame();
        }

        return ZMK_EV_EVENT_BUBBLE;
    }

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
    if (as_zmk_activity_state_changed(eh) != NULL) {
        return vfx_auto_off(zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE);
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_INDICATORS)
    {
        struct vfx_status *status = vfx_status_mutable();
        bool changed = false;

        if (as_zmk_layer_state_changed(eh) != NULL) {
            status->active_layer = (uint8_t)zmk_keymap_highest_layer_active();
            changed = true;
        }

        const struct zmk_battery_state_changed *bat = as_zmk_battery_state_changed(eh);
        if (bat != NULL) {
            status->battery_level = bat->state_of_charge;
            changed = true;
        }

#if IS_ENABLED(CONFIG_ZMK_BLE)
        if (as_zmk_ble_active_profile_changed(eh) != NULL) {
            status->ble_profile = (uint8_t)zmk_ble_active_profile_index();
            status->ble_connected = zmk_ble_active_profile_is_connected();
            changed = true;
        }
#endif

#if IS_ENABLED(CONFIG_ZMK_CAPS_WORD) || IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
        const struct zmk_caps_word_state_changed *caps = as_zmk_caps_word_state_changed(eh);
        if (caps != NULL) {
            status->caps_word = caps->active;
            changed = true;
        }
#endif

        if (changed) {
            /* Indicator layers are static between events, so the engine parks
             * its timer; without this the new state would not be drawn.
             */
            zmk_vfx_request_frame();
        }
    }
#endif

    return ZMK_EV_EVENT_BUBBLE;
}

ZMK_LISTENER(zmk_vfx, vfx_event_listener);
ZMK_SUBSCRIPTION(zmk_vfx, zmk_position_state_changed);

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
ZMK_SUBSCRIPTION(zmk_vfx, zmk_activity_state_changed);
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_INDICATORS)
ZMK_SUBSCRIPTION(zmk_vfx, zmk_layer_state_changed);
ZMK_SUBSCRIPTION(zmk_vfx, zmk_battery_state_changed);
#if IS_ENABLED(CONFIG_ZMK_BLE)
ZMK_SUBSCRIPTION(zmk_vfx, zmk_ble_active_profile_changed);
#endif
#if IS_ENABLED(CONFIG_ZMK_CAPS_WORD) || IS_ENABLED(CONFIG_ZMK_BEHAVIOR_CAPS_WORD)
ZMK_SUBSCRIPTION(zmk_vfx, zmk_caps_word_state_changed);
#endif
#endif

static int zmk_vfx_init(void) {
    if (!device_is_ready(led_strip)) {
        LOG_ERR("LED strip device %s is not ready", led_strip->name);
        return -ENODEV;
    }

    state.scene = vfx_scene_default_index();
    state.brightness = (uint8_t)((uint16_t)CONFIG_ZMK_VFX_BRT_START * 255U / 100U);
    state.speed = CONFIG_ZMK_VFX_SPD_START;
    state.hue_shift = 0;
    state.time_offset = 0;
    state.on = IS_ENABLED(CONFIG_ZMK_VFX_ON_START);

#if IS_ENABLED(CONFIG_SETTINGS)
    k_work_init_delayable(&save_work, vfx_save_work_handler);

    /* Anything persisted overwrites these defaults when the settings subsystem
     * loads, which happens after this init runs.
     */
#endif

    LOG_INF("VFX ready: %d pixels, %d scenes, %d fps", STRIP_NUM_PIXELS, vfx_scene_count(),
            VFX_FPS);

    if (state.on) {
        k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));
    }

    return 0;
}

SYS_INIT(zmk_vfx_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
