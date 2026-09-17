#!/usr/bin/env bash
# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT
#
# Compiles the real compositor to WebAssembly. No emscripten: the engine core
# has no libc dependency, so clang's bare wasm32 target is enough and the
# output is a few kilobytes rather than a few hundred.

set -euo pipefail

here="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
root="$(dirname "$here")"
out="${1:-$root/docs/sim}"

mkdir -p "$out"

exports=(
  vfx_sim_init vfx_sim_set_state vfx_sim_scratch vfx_sim_scratch_size
  vfx_sim_reset_scene vfx_sim_add_zone_range vfx_sim_add_zone_pixels
  vfx_sim_add_solid vfx_sim_add_gradient
  vfx_sim_render vfx_sim_num_pixels vfx_sim_any_lit vfx_sim_is_animating
  vfx_sim_key_event
  vfx_sim_set_power_policy vfx_sim_power_state vfx_sim_power_action
  vfx_sim_estimated_ua vfx_sim_power_reset
  vfx_sim_add_layer vfx_sim_add_layer_state vfx_sim_add_battery
  vfx_sim_add_ble_profile vfx_sim_set_status vfx_sim_set_key_map
  vfx_sim_sync_step vfx_sim_sync_max_slew vfx_sim_add_water vfx_sim_add_matrix vfx_sim_add_cross vfx_sim_set_positions
)

args=(--target=wasm32 -nostdlib -O2 -flto -DVFX_SIM -I"$root/include"
      -Wall -Wextra -Werror
      -Wl,--no-entry -Wl,--export-memory -Wl,--lto-O2 -Wl,--strip-all)

for e in "${exports[@]}"; do
  args+=(-Wl,--export="$e")
done

clang "${args[@]}" \
  "$here/vfx_sim.c" \
  "$root/src/color.c" "$root/src/zone.c" "$root/src/render.c" \
  "$root/src/power_policy.c" "$root/src/mathtab.c" "$root/src/status.c" \
  "$root/src/sync_policy.c" \
  "$root/src/layers/solid.c" "$root/src/layers/gradient.c" \
  "$root/src/layers/ambient.c" "$root/src/layers/reactive.c" \
  "$root/src/layers/indicators.c" "$root/src/layers/water.c" \
  "$root/src/layers/matrix.c" "$root/src/layers/cross.c" \
  -o "$out/vfx.wasm"

cp -r "$here/web/." "$out/"

echo "built $out/vfx.wasm ($(stat -c%s "$out/vfx.wasm") bytes)"
