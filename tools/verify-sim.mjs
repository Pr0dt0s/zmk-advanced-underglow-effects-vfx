/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives the simulator page in a real browser and checks that the generators
 * behave, rather than only that they compiled.
 *
 * The build proves a generator links; it says nothing about whether the page
 * can reach it, and an effect wired up wrong renders black rather than
 * failing. These assertions are about behaviour that would otherwise only be
 * caught by flashing a keyboard: that a reactive scene is dark until a key is
 * pressed, that a held key stays lit until it is let go, and that a driven
 * opacity actually follows its signal.
 *
 *   python3 -m http.server 8899 -d docs/sim &
 *   node tools/verify-sim.mjs
 */

import { chromium } from 'playwright';

const PAGE = process.env.VFX_SIM_URL ?? 'http://127.0.0.1:8899/index.html';

const browser = await chromium.launch(
  process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1440, height: 800 } });

const errors = [];
page.on('pageerror', e => errors.push(String(e)));
page.on('console', m => {
  /* The browser asks for a favicon unprompted and logs the miss against the
   * page, which says nothing about the simulator. Matched on the URL, since
   * the message itself only says a resource failed to load.
   */
  if (m.type() === 'error' && (m.location()?.url ?? '').includes('favicon')) return;
  if (m.type() === 'error') errors.push(m.text());
});

await page.goto(PAGE);
await page.waitForFunction(() => window.vfxDebug !== undefined);
await page.evaluate(() => window.vfxDebug.pause());

/* A key lights an LED on one half only, so drive and read the half that owns
 * it. Reading the other one sees nothing and looks exactly like a dead effect.
 */
const KEY = 20;
const half = await page.evaluate(k => window.vfxDebug.layout().rects[k].half, KEY);

const lit = () => page.evaluate(h =>
  window.vfxDebug.pixels(h).reduce((a, v) => a + v, 0), half);

const press = down => page.evaluate(([h, k, d]) =>
  window.vfxDebug.halves[h].key(k, d), [half, KEY, down]);

const renderAt = t => page.evaluate(ms => window.vfxDebug.renderAt(ms), t);

const apply = async name => {
  await page.click(`.preset:text-is("${name}")`);
  await page.waitForTimeout(200);

  const status = await page.$eval('#scene-status', e => e.textContent);

  if (status !== 'applied') throw new Error(`${name}: scene did not apply (${status})`);
};

let failed = 0;

const check = (label, ok, detail = '') => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? '   ' + detail : ''}`);
  if (!ok) failed++;
};

/* Ambient: lights with no input at all. */
for (const name of ['Static']) {
  await apply(name);
  await renderAt(120000);

  check(`${name} lights unprompted`, (await lit()) > 0);
}

/* Reactive: nothing at rest, which is what lets the power gate cut the rail,
 * and something as soon as a key goes down.
 */
for (const name of ['Pulse', 'Darts']) {
  await apply(name);
  await renderAt(120000);

  const before = await lit();

  await press(true);
  await renderAt(120060);

  const after = await lit();

  check(`${name} is dark at rest and lights on a press`, before === 0 && after > 0,
        `${before} -> ${after}`);
}

/* Hold is the one that reads a release, so both halves of that matter: it has
 * to stay up far longer than any decay would allow, and come down when let go.
 */
{
  await apply('Held');

  const bed = await lit();

  await press(true);
  await renderAt(120100);

  const struck = await lit();

  await renderAt(123000);

  const threeSecondsLater = await lit();

  await press(false);
  await renderAt(124000);

  const released = await lit();

  check('Held lights the key that is down', struck > bed, `${bed} -> ${struck}`);
  check('Held holds it for three seconds', threeSecondsLater >= struck * 0.95,
        `${struck} -> ${threeSecondsLater}`);
  check('Held lets go on release', released < threeSecondsLater,
        `${threeSecondsLater} -> ${released}`);
}

/* The whole claim of a driven opacity is that the signal changes the output
 * without the generator knowing anything about it.
 */
{
  await apply('Forge');

  await page.evaluate(() => { window.vfxDebug.state.wpm = 0; });
  await renderAt(120000);

  const idle = await lit();

  await page.evaluate(() => { window.vfxDebug.state.wpm = 80; });
  await renderAt(120000);

  const typing = await lit();

  check('Forge follows words per minute', typing > idle * 1.2, `${idle} -> ${typing}`);
}

if (errors.length) console.log(`\npage errors:\n  ${errors.join('\n  ')}`);

await browser.close();

process.exit(failed || errors.length ? 1 : 0);
