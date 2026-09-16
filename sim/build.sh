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
  "$root/src/layers/solid.c" "$root/src/layers/gradient.c" \
  -o "$out/vfx.wasm"

cp -r "$here/web/." "$out/"

echo "built $out/vfx.wasm ($(stat -c%s "$out/vfx.wasm") bytes)"
