# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_VFX)
  zephyr_include_directories(include)

  zephyr_library_named(zmk_vfx)

  zephyr_library_sources(
    src/color.c
    src/zone.c
    src/render.c
    src/engine.c
    src/scenes.c
    src/behavior_vfx.c
    src/power_policy.c
  )

  zephyr_library_sources_ifdef(CONFIG_ZMK_VFX_AUTO_POWER_GATE src/power.c)

  zephyr_library_sources_ifdef(CONFIG_ZMK_VFX_RGB_UNDERGLOW_COMPAT
                               src/behavior_rgb_compat.c)

  zephyr_library_sources(src/layers/solid.c)
  zephyr_library_sources(src/layers/gradient.c)
endif()
