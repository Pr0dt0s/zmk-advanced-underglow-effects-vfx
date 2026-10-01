/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Checks sim/web/scene-bridge.js, which turns a composer scene into the layers
 * a board holds at runtime and back.
 *
 * Two halves:
 *
 *  - Pure logic, run in Node: every composer preset converts to runtime layers
 *    and back to the same layers, what a board cannot hold is reported rather
 *    than silently dropped, and a saved file round-trips.
 *
 *  - Rendering, run in the simulator page: the same preset drawn by the
 *    compiled compositor and by runtime_scene.c (built through the real wire
 *    requests, committed layer by layer) has to be pixel-identical. This is the
 *    check that the bridge's colour and argument mapping agrees with the
 *    firmware's rebuild_slot(); it needs the WebAssembly build, so it is
 *    skipped with --no-browser.
 *
 *   python3 -m http.server 8899 -d docs/sim &
 *   node tools/verify-bridge.mjs [--no-browser]
 */

import { readFile } from 'node:fs/promises';
import { fileURLToPath } from 'node:url';
import path from 'node:path';

const here = path.dirname(fileURLToPath(import.meta.url));
const web = path.join(here, '..', 'sim', 'web');
const bridge = await import(new URL('../sim/web/scene-bridge.js', import.meta.url).href);
const { composerToRuntime, runtimeToComposer, sameLayers, saveFile, parseSaveFile, LIMITS } = bridge;

let failed = 0;

const check = (label, ok, detail = '') => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? '   ' + detail : ''}`);
  if (!ok) failed++;
};

/* The presets live inside the composer's module, which needs a page to load, so
 * lift the literal out of its source instead.
 */
const appSrc = await readFile(path.join(web, 'app.js'), 'utf8');
const start = appSrc.indexOf('const PRESETS = {');
const end = appSrc.indexOf('\n};\n', start);

if (start < 0 || end < 0) throw new Error('could not find PRESETS in app.js');

const PRESETS = new Function(`${appSrc.slice(start, end + 3)}; return PRESETS;`)();
const names = Object.keys(PRESETS);

check('The composer presets were read', names.length >= 10, `${names.length} presets`);

/* ------------------------------------------------------- round trips */

const fitting = [];

for (const name of names) {
  const { layers, warnings } = composerToRuntime(PRESETS[name]);

  if (warnings.length) {
    console.log(`  note  ${name}: ${warnings.length} thing(s) the board cannot hold: ${warnings[0]}`);
    continue;
  }

  fitting.push(name);

  const back = runtimeToComposer(layers, name);
  const again = composerToRuntime(back);

  check(`${name}: composer -> runtime -> composer -> runtime gives the same layers`,
        again.warnings.length === 0 && sameLayers(layers, again.layers),
        again.warnings.join('; '));

  check(`${name}: the layer types survive the trip, in order`,
        back.layers.map(l => l.type).join() === PRESETS[name].layers.map(l => l.type).join());
}

check('Most presets fit a board at the default limits', fitting.length >= names.length - 3,
      `${fitting.length} of ${names.length}`);

/* ------------------------------------------------------- what does not fit */

{
  const solid = { type: 'solid', zone: 'all', color: [10, 100, 100] };
  const zones = { all: { range: [0, 36] } };
  const many = { zones, layers: Array.from({ length: LIMITS.layers + 2 }, () => solid) };
  const r = composerToRuntime(many);

  check('More layers than the pool holds are cut, and said so',
        r.layers.length === LIMITS.layers && r.warnings.length === 2, r.warnings.join('; '));
}

{
  const trail = { type: 'trail', zone: 'all', color: [10, 100, 100] };
  const r = composerToRuntime({ zones: { all: { range: [0, 36] } }, layers: [trail, trail, trail] });

  check('A third trail/hold layer, which has no per-pixel state to use, is cut',
        r.layers.length === LIMITS.heavy && r.warnings.length === 1, r.warnings.join('; '));
}

{
  const r = composerToRuntime({
    zones: { big: { pixels: Array.from({ length: LIMITS.zonePixels + 8 }, (_, i) => i) } },
    layers: [{ type: 'solid', zone: 'big', color: [0, 0, 100] }],
  });

  check('A pixel list longer than the board keeps is cut to its cap',
        r.layers[0].zoneItems.length === LIMITS.zonePixels && r.warnings.length === 1,
        r.warnings.join('; '));
}

{
  const r = composerToRuntime({ zones: {}, layers: [{ type: 'solid', zone: 'nowhere', color: [0, 0, 100] }] });

  check('A layer whose zone is not defined is skipped, and said so',
        r.layers.length === 0 && /nowhere/.test(r.warnings[0] ?? ''), r.warnings.join('; '));
}

{
  const r = composerToRuntime({ zones: { all: { range: [0, 36] } },
                                layers: [{ type: 'no-such-generator', zone: 'all' }] });

  check('A generator the board does not have is skipped, and said so',
        r.layers.length === 0 && r.warnings.length === 1);
}

/* ------------------------------------------------------------ saved files */

{
  const { layers } = composerToRuntime(PRESETS[fitting[0]]);
  const text = JSON.stringify(saveFile(layers, { channel: 1, sceneIndex: 2, active: true }));
  const parsed = parseSaveFile(text);

  check('A saved file names its format, channel and scene',
        JSON.parse(text).format === 'vfx-runtime-scene' && parsed.channel === 1 &&
        parsed.sceneIndex === 2 && parsed.active === true);

  check('A saved file loads back as the same layers',
        parsed.warnings.length === 0 && sameLayers(layers, parsed.layers));

  const bare = parseSaveFile(JSON.stringify(PRESETS[fitting[0]]));

  check('A bare composer scene loads too, with no channel',
        bare.channel === null && bare.layers.length === layers.length);

  let refused = false;

  try {
    parseSaveFile(JSON.stringify({ format: 'something-else', version: 1 }));
  } catch {
    refused = true;
  }

  check('A file that is not one of ours is refused', refused);
}

/* ----------------------------------------------- rendering, in the browser */

if (process.argv.includes('--no-browser')) {
  console.log('\n  (rendering comparison skipped: --no-browser)');
} else {
  const { chromium } = await import('playwright');
  const PAGE = process.env.VFX_SIM_URL ?? 'http://127.0.0.1:8899/index.html';
  const browser = await chromium.launch(
    process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
  const page = await browser.newPage({ viewport: { width: 1440, height: 800 } });
  const errors = [];

  page.on('pageerror', e => errors.push(String(e)));

  await page.goto(PAGE);
  await page.waitForFunction(() => window.vfxDebug !== undefined);
  await page.evaluate(() => window.vfxDebug.pause());

  /* Per preset and time: what the compiled compositor draws, against what
   * runtime_scene.c draws once the same layers are built with wire requests.
   * Both read from the one simulator instance, so the clock, the key map and
   * the pixel count are identical by construction.
   */
  const result = await page.evaluate(async ([presets, times]) => {
    const { composerToRuntime, flagsFor } = await import('./scene-bridge.js');
    const { requests } = await import('./host.js');
    const out = [];
    const chan = window.vfxComposer.active();
    const e = window.vfxDebug.halves[0].e;
    const send = bytes => {
      new Uint8Array(e.memory.buffer).set(bytes, e.vfx_sim_scratch());

      return e.vfx_sim_rt_apply(bytes.length);
    };

    for (const [name, preset] of presets) {
      const { layers, warnings } = composerToRuntime(preset);

      if (warnings.length) continue;

      window.vfxComposer.setScene(chan, preset);

      const compiled = times.map(t => {
        window.vfxDebug.renderAt(t);

        return Array.from(window.vfxDebug.pixels(0));
      });

      let bad = send(requests.sceneReset(0));

      for (const d of layers) {
        bad |= send(requests.sceneAddLayer(0, d.type, d.zoneStart, d.zoneLen, d.blend, d.opacity,
                                            d.hue, d.sat, d.bri, d.args, flagsFor(d.type, d), true));

        const slot = e.vfx_sim_rt_last_slot();

        for (let i = 4; i < 6; i++) if (d.args[i]) bad |= send(requests.sceneSetArg(0, slot, i, d.args[i]));
        d.colors.forEach((c, i) => { bad |= send(requests.sceneSetListColor(0, slot, i, c.h, c.s, c.v)); });

        if (d.zoneKind !== 0) {
          for (let off = 0; off < d.zoneItems.length; off += 12) {
            bad |= send(requests.sceneSetZone(0, slot, d.zoneKind, off, d.zoneItems.slice(off, off + 12)));
          }
        }

        if (d.opacitySrc || d.opacityMin || d.opacityFull || d.tuneId) {
          bad |= send(requests.sceneSetOpts(0, slot, d));
        }

        bad |= send(requests.sceneCommitLayer(0, slot));
      }

      e.vfx_sim_rt_show(0);

      const runtime = times.map(t => {
        window.vfxDebug.renderAt(t);

        return Array.from(window.vfxDebug.pixels(0));
      });

      e.vfx_sim_rt_show(-1);

      let diff = 0;
      let first = null;
      let lit = 0;

      times.forEach((_, k) => compiled[k].forEach((v, i) => {
        if (v !== runtime[k][i]) {
          diff++;
          first ??= { t: times[k], i, compiled: v, runtime: runtime[k][i] };
        }

        if (v) lit++;
      }));

      out.push({ name, rejected: bad, diff, first, lit, bytes: compiled[0].length });
    }

    return out;
  }, [names.map(n => [n, PRESETS[n]]), [800, 3100, 7300]]);

  for (const r of result) {
    check(`${r.name}: runtime_scene.c draws what the compiled compositor draws`,
          r.rejected === 0 && r.diff === 0,
          r.rejected ? 'the firmware refused a request' :
            r.diff ? `${r.diff} bytes differ, first ${JSON.stringify(r.first)}` :
              `${r.lit} lit bytes compared`);
  }

  check('A good share of the presets could be compared', result.length >= 6, `${result.length} compared`);

  if (errors.length) {
    console.log(`\npage errors:\n  ${errors.join('\n  ')}`);
    failed++;
  }

  await browser.close();
}

process.exit(failed ? 1 : 0);
