/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_vfx

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <dt-bindings/zmk/vfx.h>
#include <zmk/vfx/scenes.h>
#include <zmk/vfx/vfx.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)

static const struct behavior_parameter_value_metadata no_arg_values[] = {
    {.display_name = "Toggle On/Off", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_TOG_CMD},
    {.display_name = "Turn On", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VFX_ON_CMD},
    {.display_name = "Turn Off", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VFX_OFF_CMD},
    {.display_name = "Next Scene", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_NEXT_CMD},
    {.display_name = "Previous Scene", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_PREV_CMD},
    {.display_name = "Brightness Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_BRI_CMD},
    {.display_name = "Brightness Down", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_BRD_CMD},
    {.display_name = "Speed Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VFX_SPI_CMD},
    {.display_name = "Speed Down", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
     .value = VFX_SPD_CMD},
    {.display_name = "Hue Up", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VFX_HUI_CMD},
    {.display_name = "Hue Down", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE, .value = VFX_HUD_CMD},
};

static const struct behavior_parameter_value_metadata scene_values[] = {
    {.display_name = "Select Scene", .type = BEHAVIOR_PARAMETER_VALUE_TYPE_RANGE, .range =
         {.min = 0, .max = 255}},
};

static const struct behavior_parameter_metadata_set sets[] = {
    {
        .param1_values = no_arg_values,
        .param1_values_len = ARRAY_SIZE(no_arg_values),
    },
    {
        .param1_values =
            (const struct behavior_parameter_value_metadata[]){
                {.display_name = "Select Scene",
                 .type = BEHAVIOR_PARAMETER_VALUE_TYPE_VALUE,
                 .value = VFX_SEL_CMD},
            },
        .param1_values_len = 1,
        .param2_values = scene_values,
        .param2_values_len = ARRAY_SIZE(scene_values),
    },
};

static const struct behavior_parameter_metadata metadata = {
    .sets_len = ARRAY_SIZE(sets),
    .sets = sets,
};

#endif /* CONFIG_ZMK_BEHAVIOR_METADATA */

/* param1 carries the command in its low byte and, above that, the channel the
 * press addresses. Zero there means none was named, which is every channel.
 */
#define VFX_CMD_OF(p1) ((uint8_t)((p1) & VFX_CMD_MASK))

static uint8_t vfx_channel_of(uint32_t param1) {
    const uint32_t raw = (param1 >> VFX_CH_SHIFT) & 0xFF;

    return raw == 0 ? ZMK_VFX_CH_ALL : (uint8_t)(raw - 1);
}

/* Swap the command but keep the channel, so a rewritten binding still reaches
 * the same place once it is relayed.
 */
static uint32_t vfx_with_cmd(uint32_t param1, uint8_t cmd) {
    return (param1 & ~(uint32_t)VFX_CMD_MASK) | cmd;
}

/* Rewrite a relative command into its absolute equivalent while still on the
 * central, before the binding is relayed.
 *
 * Without this each half would apply its own increment to its own current
 * value, and any missed or duplicated relay would leave the two permanently
 * disagreeing. Sending the resulting value instead makes the halves converge.
 */
static int vfx_convert_central_state_dependent_params(struct zmk_behavior_binding *binding,
                                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    const uint8_t ch = vfx_channel_of(binding->param1);

    switch (VFX_CMD_OF(binding->param1)) {
    case VFX_NEXT_CMD:
        binding->param2 = zmk_vfx_calc_scene(ch, 1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_SCENE_CMD);
        break;
    case VFX_PREV_CMD:
        binding->param2 = zmk_vfx_calc_scene(ch, -1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_SCENE_CMD);
        break;
    case VFX_BRI_CMD:
        binding->param2 = zmk_vfx_calc_brightness(ch, 1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_BRT_CMD);
        break;
    case VFX_BRD_CMD:
        binding->param2 = zmk_vfx_calc_brightness(ch, -1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_BRT_CMD);
        break;
    case VFX_SPI_CMD:
        binding->param2 = zmk_vfx_calc_speed(ch, 1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_SPD_CMD);
        break;
    case VFX_SPD_CMD:
        binding->param2 = zmk_vfx_calc_speed(ch, -1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_SPD_CMD);
        break;
    case VFX_HUI_CMD:
        binding->param2 = zmk_vfx_calc_hue(ch, 1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_HUE_CMD);
        break;
    case VFX_HUD_CMD:
        binding->param2 = zmk_vfx_calc_hue(ch, -1);
        binding->param1 = vfx_with_cmd(binding->param1, VFX_SET_HUE_CMD);
        break;
    default:
        return 0;
    }

    LOG_DBG("VFX relative command resolved to absolute (%d/%d)", binding->param1, binding->param2);

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    const uint8_t ch = vfx_channel_of(binding->param1);

    switch (VFX_CMD_OF(binding->param1)) {
    case VFX_TOG_CMD:
        return zmk_vfx_toggle(ch);
    case VFX_ON_CMD:
        return zmk_vfx_on(ch);
    case VFX_OFF_CMD:
        return zmk_vfx_off(ch);
    case VFX_NEXT_CMD:
        return zmk_vfx_cycle_scene(ch, 1);
    case VFX_PREV_CMD:
        return zmk_vfx_cycle_scene(ch, -1);
    case VFX_SEL_CMD:
    case VFX_SET_SCENE_CMD:
        return zmk_vfx_select_scene(ch, (uint8_t)binding->param2);
    case VFX_BRI_CMD:
        return zmk_vfx_change_brightness(ch, 1);
    case VFX_BRD_CMD:
        return zmk_vfx_change_brightness(ch, -1);
    case VFX_SET_BRT_CMD:
        return zmk_vfx_set_brightness(ch, (uint8_t)binding->param2);
    case VFX_SPI_CMD:
        return zmk_vfx_change_speed(ch, 1);
    case VFX_SPD_CMD:
        return zmk_vfx_change_speed(ch, -1);
    case VFX_SET_SPD_CMD:
        return zmk_vfx_set_speed(ch, (uint8_t)binding->param2);
    case VFX_HUI_CMD:
        return zmk_vfx_change_hue(ch, 1);
    case VFX_HUD_CMD:
        return zmk_vfx_change_hue(ch, -1);
    case VFX_SET_HUE_CMD:
        return zmk_vfx_set_hue(ch, (uint16_t)binding->param2);

    /* These name a tuning slot rather than a channel, so they read param2
     * rather than ch: a slot is a handle on particular layers wherever they
     * happen to sit.
     */
    case VFX_TUNE_HUE_CMD:
        return zmk_vfx_tune_hue((uint8_t)((binding->param2 >> 16) & 0xFF),
                                (int16_t)(binding->param2 & 0x1FF));
    case VFX_TUNE_LEVEL_CMD:
        return zmk_vfx_tune_level((uint8_t)((binding->param2 >> 8) & 0xFF),
                                  (uint8_t)(binding->param2 & 0xFF));
    case VFX_TUNE_SPEED_CMD:
        return zmk_vfx_tune_speed((uint8_t)((binding->param2 >> 8) & 0xFF),
                                  (uint8_t)(binding->param2 & 0xFF));
    case VFX_TUNE_RESET_CMD:
        return zmk_vfx_tune_reset((uint8_t)((binding->param2 >> 8) & 0xFF));

    /* Relayed from the central in synchronised split mode. These never appear
     * in a keymap; they arrive through ZMK's behavior relay, which is why
     * they ride this behavior rather than a GATT service of their own.
     */
    case VFX_SYNC_CMD:
        zmk_vfx_apply_sync(binding->param2);
        return 0;

    case VFX_KEY_CMD:
        zmk_vfx_inject_key(binding->param2);
        return 0;
    }

    return -ENOTSUP;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_vfx_driver_api = {
    .binding_convert_central_state_dependent_params = vfx_convert_central_state_dependent_params,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
#if IS_ENABLED(CONFIG_ZMK_BEHAVIOR_METADATA)
    .parameter_metadata = &metadata,
#endif
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_vfx_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
