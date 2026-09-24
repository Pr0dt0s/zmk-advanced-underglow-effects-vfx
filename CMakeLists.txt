# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT

if(CONFIG_ZMK_VFX)
  # Exposes include/ to everything, which is how the app finds zmk/vfx/*.h
  # and how a keymap finds dt-bindings/zmk/vfx.h.
  zephyr_include_directories(include)

  # Sources go into the `app` target rather than a zephyr_library.
  #
  # ZMK adds app/include with target_include_directories(app PRIVATE include),
  # so drivers/behavior.h, zmk/event_manager.h, zmk/split/central.h and the
  # rest are not visible to a separate library. Compiling into `app` is how
  # ZMK's own out-of-tree behaviors do it (see app/module/test-behaviors), and
  # the only way to reach those headers without reaching into ZMK's source
  # tree by path.
  target_sources(app PRIVATE
    src/color.c
    src/zone.c
    src/render.c
    src/engine.c
    src/scenes.c
    src/behavior_vfx.c
    src/power_policy.c
    src/mathtab.c
    src/status.c
    src/tuning.c
    src/sync_policy.c
    src/layers/solid.c
    src/layers/gradient.c
    src/layers/ambient.c
    src/layers/reactive.c
    src/layers/water.c
    src/layers/matrix.c
    src/layers/cross.c
    src/layers/fire.c
    src/layers/comet.c
    src/layers/indicators.c
  )

  target_sources_ifdef(CONFIG_ZMK_VFX_RGB_UNDERGLOW_COMPAT app PRIVATE
                       src/behavior_rgb_compat.c)
  target_sources_ifdef(CONFIG_ZMK_VFX_AUTO_POWER_GATE app PRIVATE src/power.c)
  target_sources_ifdef(CONFIG_ZMK_VFX_SPLIT_SYNCED app PRIVATE src/split_sync.c)
  target_sources_ifdef(CONFIG_ZMK_VFX_RAW_HID app PRIVATE
                       src/hid_protocol.c src/hid_transport.c)
  target_sources_ifdef(CONFIG_ZMK_VFX_RUNTIME_SCENES app PRIVATE
                       src/runtime_scene.c src/scene_relay.c)
endif()
