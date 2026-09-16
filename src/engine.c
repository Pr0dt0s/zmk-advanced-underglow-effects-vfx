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

#include <zmk/vfx/engine.h>
#include <zmk/vfx/scenes.h>
#include <zmk/vfx/vfx.h>
#include <zmk/workqueue.h>

#if IS_ENABLED(CONFIG_SETTINGS)
#include <zephyr/settings/settings.h>
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
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
    push_frame();

    frame_dirty = false;
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

        if (!vfx_scene_is_animating(vfx_scene_get(state.scene), &ctx)) {
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

    return zmk_vfx_save_state();
}

int zmk_vfx_toggle(void) { return state.on ? zmk_vfx_off() : zmk_vfx_on(); }

bool zmk_vfx_is_on(void) { return state.on; }

int zmk_vfx_select_scene(uint8_t index) {
    if (index >= vfx_scene_count()) {
        return -EINVAL;
    }

    state.scene = index;
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

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
static bool on_before_idle;

static int vfx_activity_listener(const zmk_event_t *eh) {
    if (as_zmk_activity_state_changed(eh) == NULL) {
        return -ENOTSUP;
    }

    const bool awake = zmk_activity_get_state() == ZMK_ACTIVITY_ACTIVE;

    if (awake) {
        return on_before_idle ? zmk_vfx_on() : 0;
    }

    on_before_idle = state.on;

    return state.on ? zmk_vfx_off() : 0;
}

ZMK_LISTENER(zmk_vfx, vfx_activity_listener);
ZMK_SUBSCRIPTION(zmk_vfx, zmk_activity_state_changed);
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
