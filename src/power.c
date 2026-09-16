/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#include <zephyr/device.h>
#include <zephyr/devicetree.h>
#include <zephyr/logging/log.h>
#include <zephyr/pm/device.h>

#include <drivers/ext_power.h>

#include <zmk/vfx/power.h>

LOG_MODULE_DECLARE(zmk_vfx, CONFIG_ZMK_VFX_LOG_LEVEL);

#define VFX_HAS_EXT_POWER DT_HAS_COMPAT_STATUS_OKAY(zmk_ext_power_generic)

#if !VFX_HAS_EXT_POWER
#warning "CONFIG_ZMK_VFX_AUTO_POWER_GATE is set, but this board declares no "                       \
         "zmk,ext-power-generic node, so the LED rail cannot be switched. "                        \
         "Blackout frames will still skip the bus transfer, but the strip's "                      \
         "quiescent draw will remain."
#endif

#if VFX_HAS_EXT_POWER
static const struct device *const ext_power = DEVICE_DT_GET(DT_INST(0, zmk_ext_power_generic));
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_SUSPEND_STRIP_BUS)
/* The strip hangs off a bus (SPI on most boards). Suspending it applies the
 * node's "sleep" pinctrl state, which parks the data line low. That matters:
 * a data pin left high into an unpowered strip leaks current through the
 * chip's protection diode, quietly undoing what gating the rail just saved.
 */
static const struct device *const strip_bus = DEVICE_DT_GET(DT_PARENT(DT_CHOSEN(zmk_underglow)));
#endif

void vfx_power_rail_enable(void) {
#if VFX_HAS_EXT_POWER
    if (device_is_ready(ext_power)) {
        int rc = ext_power_enable(ext_power);

        if (rc != 0) {
            LOG_ERR("Failed to enable the LED rail (%d)", rc);
        }
    }
#endif

#if IS_ENABLED(CONFIG_ZMK_VFX_SUSPEND_STRIP_BUS)
    if (device_is_ready(strip_bus)) {
        pm_device_action_run(strip_bus, PM_DEVICE_ACTION_RESUME);
    }
#endif
}

void vfx_power_rail_disable(void) {
#if IS_ENABLED(CONFIG_ZMK_VFX_SUSPEND_STRIP_BUS)
    /* Bus first, then the rail: park the data line before the strip loses
     * power, not after.
     */
    if (device_is_ready(strip_bus)) {
        pm_device_action_run(strip_bus, PM_DEVICE_ACTION_SUSPEND);
    }
#endif

#if VFX_HAS_EXT_POWER
    if (device_is_ready(ext_power)) {
        int rc = ext_power_disable(ext_power);

        if (rc != 0) {
            LOG_ERR("Failed to disable the LED rail (%d)", rc);
        }
    }
#endif
}
