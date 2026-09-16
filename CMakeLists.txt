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
  )

  zephyr_library_sources(src/layers/solid.c)
  zephyr_library_sources(src/layers/gradient.c)
endif()
