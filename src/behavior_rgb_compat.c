/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#define DT_DRV_COMPAT zmk_behavior_rgb_underglow

#include <zephyr/device.h>
#include <zephyr/logging/log.h>

#include <drivers/behavior.h>

#include <dt-bindings/zmk/rgb.h>
#include <zmk/vfx/vfx.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

/* Drop-in for ZMK's own &rgb_ug behavior.
 *
 * app/dts/behaviors/rgb_underglow.dtsi is included unconditionally by ZMK, so
 * the rgb_ug node exists in every build. Its driver is only compiled under
 * CONFIG_ZMK_RGB_UNDERGLOW, which VFX requires to be off, leaving the node
 * unbound and any keymap that references &rgb_ug failing to build. Binding it
 * here means existing keymaps keep working unchanged.
 *
 * Saturation has no engine-wide analogue: in VFX it is a property of each
 * layer's colors in devicetree, not one global value. RGB_SAI and RGB_SAD are
 * accepted and ignored rather than failing the binding.
 *
 * Channels have no analogue either, and &rgb_ug predates them, so everything
 * here addresses the whole board. A keymap that wants one part of the strip on
 * its own has to say so with &vfx.
 */

#if DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT)

BUILD_ASSERT(!IS_ENABLED(CONFIG_ZMK_RGB_UNDERGLOW),
             "The VFX rgb_ug shim and ZMK's own underglow behavior cannot both "
             "bind zmk,behavior-rgb-underglow.");

/* The RGB command set has no absolute brightness op to convert into, so the
 * shim defines one outside the range dt-bindings/zmk/rgb.h uses. It never
 * appears in a keymap: it exists only between the central's conversion step
 * and the peripheral that receives the relayed binding.
 */
#define RGB_COMPAT_SET_BRT (RGB_COLOR_HSB_CMD + 1)

static int rgb_convert_central_state_dependent_params(struct zmk_behavior_binding *binding,
                                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    /* Same reason as the &vfx behavior: resolve to an absolute value on the
     * central so both halves converge instead of each stepping its own copy.
     */
    switch (binding->param1) {
    case RGB_BRI_CMD:
        binding->param2 = zmk_vfx_calc_brightness(ZMK_VFX_CH_ALL, 1);
        binding->param1 = RGB_COMPAT_SET_BRT;
        break;
    case RGB_BRD_CMD:
        binding->param2 = zmk_vfx_calc_brightness(ZMK_VFX_CH_ALL, -1);
        binding->param1 = RGB_COMPAT_SET_BRT;
        break;
    case RGB_EFF_CMD:
        binding->param2 = zmk_vfx_calc_scene(ZMK_VFX_CH_ALL, 1);
        binding->param1 = RGB_EFS_CMD;
        break;
    case RGB_EFR_CMD:
        binding->param2 = zmk_vfx_calc_scene(ZMK_VFX_CH_ALL, -1);
        binding->param1 = RGB_EFS_CMD;
        break;
    default:
        return 0;
    }

    return 0;
}

static int on_keymap_binding_pressed(struct zmk_behavior_binding *binding,
                                     struct zmk_behavior_binding_event event) {
    ARG_UNUSED(event);

    switch (binding->param1) {
    case RGB_TOG_CMD:
        return zmk_vfx_toggle(ZMK_VFX_CH_ALL);
    case RGB_ON_CMD:
        return zmk_vfx_on(ZMK_VFX_CH_ALL);
    case RGB_OFF_CMD:
        return zmk_vfx_off(ZMK_VFX_CH_ALL);
    case RGB_HUI_CMD:
        return zmk_vfx_change_hue(ZMK_VFX_CH_ALL, 1);
    case RGB_HUD_CMD:
        return zmk_vfx_change_hue(ZMK_VFX_CH_ALL, -1);
    case RGB_BRI_CMD:
        return zmk_vfx_change_brightness(ZMK_VFX_CH_ALL, 1);
    case RGB_BRD_CMD:
        return zmk_vfx_change_brightness(ZMK_VFX_CH_ALL, -1);
    case RGB_COMPAT_SET_BRT:
        return zmk_vfx_set_brightness(ZMK_VFX_CH_ALL, (uint8_t)binding->param2);
    case RGB_SPI_CMD:
        return zmk_vfx_change_speed(ZMK_VFX_CH_ALL, 1);
    case RGB_SPD_CMD:
        return zmk_vfx_change_speed(ZMK_VFX_CH_ALL, -1);
    case RGB_EFF_CMD:
        return zmk_vfx_cycle_scene(ZMK_VFX_CH_ALL, 1);
    case RGB_EFR_CMD:
        return zmk_vfx_cycle_scene(ZMK_VFX_CH_ALL, -1);
    case RGB_EFS_CMD:
        return zmk_vfx_select_scene(ZMK_VFX_CH_ALL, (uint8_t)binding->param2);
    case RGB_SAI_CMD:
    case RGB_SAD_CMD:
        LOG_DBG("Ignoring RGB saturation command: VFX sets saturation per layer in devicetree");
        return 0;
    case RGB_COLOR_HSB_CMD:
        /* Hue maps onto the global hue rotation; brightness onto the global
         * brightness. Saturation is dropped, as above.
         */
        zmk_vfx_set_hue(ZMK_VFX_CH_ALL, (uint16_t)((binding->param2 >> 16) & 0x1FF));
        return zmk_vfx_set_brightness(ZMK_VFX_CH_ALL,
                                      (uint8_t)((binding->param2 & 0xFF) * 255U / 100U));
    }

    return -ENOTSUP;
}

static int on_keymap_binding_released(struct zmk_behavior_binding *binding,
                                      struct zmk_behavior_binding_event event) {
    ARG_UNUSED(binding);
    ARG_UNUSED(event);

    return ZMK_BEHAVIOR_OPAQUE;
}

static const struct behavior_driver_api behavior_rgb_compat_driver_api = {
    .binding_convert_central_state_dependent_params = rgb_convert_central_state_dependent_params,
    .binding_pressed = on_keymap_binding_pressed,
    .binding_released = on_keymap_binding_released,
    .locality = BEHAVIOR_LOCALITY_GLOBAL,
};

BEHAVIOR_DT_INST_DEFINE(0, NULL, NULL, NULL, NULL, POST_KERNEL, CONFIG_KERNEL_INIT_PRIORITY_DEFAULT,
                        &behavior_rgb_compat_driver_api);

#endif /* DT_HAS_COMPAT_STATUS_OKAY(DT_DRV_COMPAT) */
