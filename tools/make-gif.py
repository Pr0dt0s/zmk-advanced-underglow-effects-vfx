#!/usr/bin/env python3
# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT
"""Turn a directory of PNG frames into a looping GIF for the README.

Usage: make-gif.py <frame-dir> <output.gif> <frame-ms> [lossy]

Underglow is a hard thing to put in a GIF: a few saturated colours bleeding
into near-black over the whole frame, which is smooth gradient everywhere and
exactly what a 256 colour palette is worst at. Three things keep it honest:

  * one palette for the whole clip, built from a row sampled out of every
    frame. Quantising each frame on its own makes the background crawl between
    frames, which reads as noise and costs more than it saves;
  * Floyd-Steinberg dithering. Undithered, the glow breaks into contour bands
    that look far worse than the grain does;
  * gifsicle afterwards, if it is installed, which is worth roughly a third of
    the file for nothing.

gifsicle's --lossy is per clip rather than global, because it is not a quality
dial that applies evenly. It works by letting pixels drift toward their
neighbours, which a busy bright frame hides and a mostly near-black one does
not: at 30 it turned the crosshair clip, whose whole point is one clearly
drawn cross, into grain. So the bright ambient clips use it and save about a
third, and the dark reactive ones stay lossless, where they are small anyway.
"""

import pathlib
import shutil
import subprocess
import sys

from PIL import Image

WIDTH = 480     # README column width, two clips side by side in a table
COLORS = 64     # more makes the file bigger without looking better


def main() -> int:
    if len(sys.argv) not in (4, 5):
        print(__doc__, file=sys.stderr)
        return 2

    frame_dir, out_path, frame_ms = sys.argv[1], sys.argv[2], int(sys.argv[3])
    lossy = int(sys.argv[4]) if len(sys.argv) == 5 else 0

    paths = sorted(pathlib.Path(frame_dir).glob("f*.png"))
    if not paths:
        print(f"no frames in {frame_dir}", file=sys.stderr)
        return 1

    frames = []
    for p in paths:
        im = Image.open(p).convert("RGB")
        height = round(im.height * WIDTH / im.width)
        frames.append(im.resize((WIDTH, height), Image.LANCZOS))

    # A row from each frame, so a colour that only appears late -- the head of
    # a comet, a caps lock flash -- still gets entries of its own.
    sheet = Image.new("RGB", (WIDTH, len(frames)))
    for i, f in enumerate(frames):
        sheet.paste(f.resize((WIDTH, 1), Image.LANCZOS), (0, i))

    palette = sheet.quantize(colors=COLORS, method=Image.MEDIANCUT)
    quantised = [f.quantize(palette=palette, dither=Image.FLOYDSTEINBERG) for f in frames]

    quantised[0].save(
        out_path,
        save_all=True,
        append_images=quantised[1:],
        duration=frame_ms,
        loop=0,
        optimize=True,
        disposal=2,
    )

    before = pathlib.Path(out_path).stat().st_size

    if shutil.which("gifsicle"):
        cmd = ["gifsicle", "-O3", "--no-warnings"]
        if lossy:
            cmd.append(f"--lossy={lossy}")
        cmd += ["-o", out_path, out_path]
        subprocess.run(cmd, check=True)

    after = pathlib.Path(out_path).stat().st_size
    saved = f" (from {before // 1024} KB)" if after < before else ""
    print(f"{out_path}: {len(frames)} frames, {after // 1024} KB{saved}")

    return 0


if __name__ == "__main__":
    sys.exit(main())
