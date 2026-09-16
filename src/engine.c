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

#if IS_ENABLED(CONFIG_ZMK_VFX_AUTO_OFF_IDLE)
#include <zmk/activity.h>
#include <zmk/event_manager.h>
#include <zmk/events/activity_state_changed.h>
#endif

LOG_MODULE_REGISTER(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

#if !DT_HAS_CHOSEN(zmk_underglow)
#error "ZMK VFX needs a zmk,underglow chosen node pointing at an LED strip."
#endif

#if !DT_HAS_STATUS_OKAY(zmk_vfx_engine)
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

static void blank_strip(void) {
    for (uint16_t i = 0; i < STRIP_NUM_PIXELS; i++) {
        frame[i] = VFX_RGB_BLACK;
    }

    push_frame();
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
            return;
        }
    }

    k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &vfx_tick_work);
}

K_TIMER_DEFINE(vfx_timer, vfx_timer_handler, NULL);

void zmk_vfx_request_frame(void) {
    frame_dirty = true;

    if (state.on) {
        k_work_submit_to_queue(zmk_workqueue_lowprio_work_q(), &vfx_tick_work);
    }
}

int zmk_vfx_on(void) {
    if (!device_is_ready(led_strip)) {
        return -ENODEV;
    }

    state.on = true;
    frame_dirty = true;

    k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));

    return 0;
}

int zmk_vfx_off(void) {
    if (!device_is_ready(led_strip)) {
        return -ENODEV;
    }

    state.on = false;
    k_timer_stop(&vfx_timer);
    blank_strip();

    return 0;
}

int zmk_vfx_toggle(void) { return state.on ? zmk_vfx_off() : zmk_vfx_on(); }

bool zmk_vfx_is_on(void) { return state.on; }

int zmk_vfx_select_scene(uint8_t index) {
    if (index >= vfx_scene_count()) {
        return -EINVAL;
    }

    state.scene = index;
    zmk_vfx_request_frame();

    return 0;
}

int zmk_vfx_cycle_scene(int direction) {
    const uint8_t count = vfx_scene_count();
    const int next = ((int)state.scene + count + direction) % count;

    return zmk_vfx_select_scene((uint8_t)next);
}

uint8_t zmk_vfx_current_scene(void) { return state.scene; }

const char *zmk_vfx_scene_name(uint8_t index) {
    const struct vfx_scene *scene = vfx_scene_get(index);

    return scene ? scene->name : NULL;
}

int zmk_vfx_change_brightness(int direction) {
    const int step = 255 / 10;
    int b = (int)state.brightness + direction * step;

    state.brightness = (uint8_t)CLAMP(b, 0, 255);
    zmk_vfx_request_frame();

    return 0;
}

int zmk_vfx_change_speed(int direction) {
    int s = (int)state.speed + direction;

    state.speed = (uint8_t)CLAMP(s, 1, 5);
    zmk_vfx_request_frame();

    return 0;
}

int zmk_vfx_change_hue(int direction) {
    /* vfx_hue_add wraps into 0-359, so hue_shift stays in range by construction. */
    state.hue_shift = (int16_t)vfx_hue_add((uint16_t)state.hue_shift, (int16_t)(direction * 10));
    zmk_vfx_request_frame();

    return 0;
}

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

    LOG_INF("VFX ready: %d pixels, %d scenes, %d fps", STRIP_NUM_PIXELS, vfx_scene_count(),
            VFX_FPS);

    if (state.on) {
        k_timer_start(&vfx_timer, K_NO_WAIT, K_MSEC(VFX_FRAME_MS));
    }

    return 0;
}

SYS_INIT(zmk_vfx_init, APPLICATION, CONFIG_APPLICATION_INIT_PRIORITY);
