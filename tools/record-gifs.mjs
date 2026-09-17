/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Records the README's GIFs from the simulator, which means they are frames
 * the real compositor produced rather than an artist's impression of it.
 *
 * Engine time is stepped explicitly rather than sampled off the wall clock:
 * screenshotting a live animation samples it unevenly, which is what makes a
 * recorded GIF stutter, and it cannot land on an effect's period. Stepping
 * lets each clip cover exactly one period, so the loop closes seamlessly.
 *
 *   node tools/record-gifs.mjs [clip ...]
 *
 * Needs playwright and Pillow, and uses gifsicle if it is installed.
 * tools/make-gif.py does the encoding. Set CHROMIUM_PATH if playwright cannot
 * find a browser itself, and VFX_KEEP_FRAMES=1 to leave the PNGs behind for
 * comparing encoder settings.
 */

import { chromium } from 'playwright';
import { spawnSync } from 'node:child_process';
import fs from 'node:fs';
import path from 'node:path';

const OUT = 'docs/img';
const PAGE = `file://${path.resolve('docs/vfx-simulator.html')}`;
const FPS = 20;

/* Each clip says how long a loop is, how hard gifsicle may compress it, and
 * for the reactive ones when to press what. Periods are worked out from the
 * preset's own numbers: a gradient scrolling at `scroll-speed` covers its
 * span in span/(speed*3) seconds, and a comet's lap is its period-ms.
 *
 * `lossy` is per clip on purpose -- see tools/make-gif.py. The bright ambient
 * clips can take it; the dark ones show it as speckle.
 */
const CLIPS = {
  aurora:        { preset: 'Aurora', ms: 2000, lossy: 40 },
  pinwheel:      { preset: 'Pinwheel', ms: 2133, lossy: 40 },
  spiral:        { preset: 'Spiral', ms: 1552, lossy: 40 },
  'out-and-in':  { preset: 'Out and in', ms: 2600, lossy: 40 },
  fire:          { preset: 'Fire', ms: 3000 },
  beacons:       { preset: 'Beacons', ms: 2600, lossy: 40 },
  matrix:        { preset: 'Matrix', ms: 3000 },
  water:         { preset: 'Water', ms: 2400, lossy: 40 },
  plasma:        { preset: 'Plasma', ms: 4000, step: 2, lossy: 40 },

  /* Reactive clips: the keys are pressed on the engine's own clock, so the
   * reaction lands at the same frame every time this is run.
   */
  /* Typing fast enough that two or three reactions overlap. One at a time is
   * what it looks like in use, but at README size a single fading cross on an
   * otherwise dark board barely registers.
   */
  crosshair: {
    preset: 'Crosshair', ms: 2800,
    keys: [[150, 20], [400, 33], [650, 8], [900, 45], [1150, 27],
           [1400, 14], [1650, 39], [1900, 3], [2150, 31], [2400, 22]],
  },
  'matrix-typing': {
    preset: 'Matrix (typing only)', ms: 2800,
    keys: [[100, 16], [300, 29], [500, 41], [700, 22], [900, 50], [1100, 7],
           [1300, 34], [1500, 19], [1700, 45], [1900, 11], [2100, 37], [2300, 26]],
  },
  'water-typing': {
    preset: 'Water (typing only)', ms: 3200,
    keys: [[150, 21], [600, 38], [1050, 12], [1500, 46], [1950, 25], [2400, 8]],
  },
  nexus: {
    preset: 'Nexus', ms: 2800,
    keys: [[150, 14], [450, 31], [750, 24], [1050, 44], [1350, 9],
           [1650, 36], [1950, 18], [2250, 41]],
  },
};

const wanted = process.argv.slice(2);
const names = wanted.length ? wanted : Object.keys(CLIPS);

fs.mkdirSync(OUT, { recursive: true });

/* Let playwright find its own browser unless told otherwise, so this is not
 * tied to the machine it was first written on.
 */
const browser = await chromium.launch(
  process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1440, height: 700 } });
const errors = [];
page.on('pageerror', e => errors.push(String(e)));

await page.goto(PAGE);
await page.waitForFunction(() => window.vfxDebug !== undefined);
await page.evaluate(() => window.vfxDebug.pause());

const canvas = await page.$('canvas');

for (const name of names) {
  const clip = CLIPS[name];
  if (!clip) {
    console.error(`unknown clip "${name}"`);
    continue;
  }

  await page.click(`.preset:text-is("${clip.preset}")`);
  await page.waitForTimeout(250);

  const status = await page.$eval('#scene-status', e => e.textContent);
  if (status !== 'applied') {
    throw new Error(`${name}: scene did not apply (${status})`);
  }

  const step = (clip.step ?? 1) * Math.round(1000 / FPS);
  const frames = Math.round(clip.ms / step);
  const dir = fs.mkdtempSync(path.join('/tmp', `vfx-${name}-`));

  /* Start well past zero so nothing is caught in its first moments, when an
   * epoch-hashed effect has fewer drops in flight than it will settle at.
   */
  const base = 120000;

  await page.evaluate(() => window.vfxDebug.halves.forEach(h => h.e.vfx_sim_power_reset()));

  for (let i = 0; i < frames; i++) {
    const t = base + i * step;

    await page.evaluate(([t, prev, keys]) => {
      for (const [at, key] of keys || []) {
        /* Fire a press in the window this frame covers, stamped with the
         * engine time it happened at rather than whenever the browser got
         * round to it.
         */
        if (at >= prev && at < t - 120000) {
          const half = window.vfxDebug.layout().rects[key].half;
          window.vfxDebug.halves[half].key(key, true);
        }
      }
      window.vfxDebug.renderAt(t);
    }, [t, i === 0 ? -1 : (i - 1) * step, clip.keys ?? []]);

    await canvas.screenshot({ path: path.join(dir, `f${String(i).padStart(4, '0')}.png`) });
  }

  const out = path.join(OUT, `${name}.gif`);
  const rc = spawnSync(
    'python3',
    ['tools/make-gif.py', dir, out, String(step), String(clip.lossy ?? 0)],
    { stdio: 'inherit' });
  if (rc.status !== 0) {
    throw new Error(`${name}: encoding failed`);
  }

  if (process.env.VFX_KEEP_FRAMES) {
    console.log(`  frames kept in ${dir}`);
  } else {
    fs.rmSync(dir, { recursive: true, force: true });
  }
}

if (errors.length) {
  console.error('page errors:', errors);
  process.exitCode = 1;
}

await browser.close();
