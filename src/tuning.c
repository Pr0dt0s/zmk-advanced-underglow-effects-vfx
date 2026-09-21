/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zmk/vfx/color.h>
#include <zmk/vfx/tuning.h>

/* Slot 0 stands for "not tuned" and is never handed out, so a layer with no
 * tune-id cannot accidentally be adjusted by whatever happens to be first.
 */
static struct vfx_tuning slots[VFX_TUNE_SLOTS];

static bool usable(uint8_t id) { return id > 0 && id < VFX_TUNE_SLOTS; }

/* Written by the compositor on every layer it draws, so it must stay cheap
 * and must not care whether anything has ever been tuned.
 */
const struct vfx_tuning *vfx_tuning_get(uint8_t id) {
    return usable(id) ? &slots[id] : (const struct vfx_tuning *)0;
}

bool vfx_tuning_set_hue(uint8_t id, int16_t degrees) {
    if (!usable(id)) {
        return false;
    }

    /* Held in 0-359 so the stored value does not depend on how it was
     * reached, matching what the engine does with its own hue shift.
     */
    int32_t h = degrees % 360;

    if (h < 0) {
        h += 360;
    }

    slots[id].hue = (int16_t)h;

    return true;
}

bool vfx_tuning_set_level(uint8_t id, uint8_t level) {
    if (!usable(id)) {
        return false;
    }

    slots[id].level = level;

    return true;
}

bool vfx_tuning_set_speed(uint8_t id, uint8_t speed) {
    if (!usable(id)) {
        return false;
    }

    /* Zero is meaningful here: it hands the layer back to the channel's own
     * speed rather than pinning it to the slowest setting.
     */
    slots[id].speed = speed > 5 ? 5 : speed;

    return true;
}

void vfx_tuning_reset(uint8_t id) {
    if (!usable(id)) {
        return;
    }

    slots[id] = (struct vfx_tuning){.hue = 0, .level = 255, .speed = 0};
}

void vfx_tuning_reset_all(void) {
    for (uint8_t i = 0; i < VFX_TUNE_SLOTS; i++) {
        slots[i] = (struct vfx_tuning){.hue = 0, .level = 255, .speed = 0};
    }
}

void *vfx_tuning_state(uint16_t *len) {
    if (len) {
        *len = (uint16_t)sizeof(slots);
    }

    return slots;
}
