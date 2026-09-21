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

/* A board with both kinds of LED on one chain. Left until last because it
 * rewires the layout everything above assumed.
 */
{
  await page.selectOption('#path', 'mixed');
  await page.waitForTimeout(400);

  const board = await page.evaluate(() => {
    const l = window.vfxDebug.layout();
    const perHalf = l.leds.length / 2;
    const glow = l.leds.slice(0, perHalf).filter(p => p.kind === 'glow').length;

    return {
      perHalf,
      glow,
      keyed: l.leds.slice(0, perHalf).filter(p => p.kind === 'key').length,
      /* Nearest-wins would hand some keys an underglow pixel, which starts a
       * ripple behind the board instead of under the finger.
       */
      onGlow: l.keyPixels.filter(i => (i % perHalf) < glow).length,
    };
  });

  check('Mixed board is underglow plus one LED per key',
        board.perHalf === board.glow + board.keyed && board.glow > 0 && board.keyed > 0,
        `${board.perHalf} = ${board.glow} + ${board.keyed}`);
  check('No key is mapped onto an underglow pixel', board.onGlow === 0,
        `${board.onGlow} would be`);

  await apply('Underglow and keys');
  await renderAt(120000);

  const rest = await page.evaluate(() => {
    const l = window.vfxDebug.layout();
    const per = l.leds.length / 2;
    const px = window.vfxDebug.pixels(0);
    let glow = 0, keyed = 0;

    for (let i = 0; i < per; i++) {
      const sum = px[i * 3] + px[i * 3 + 1] + px[i * 3 + 2];

      if (l.leds[i].kind === 'glow') glow += sum; else keyed += sum;
    }

    return { glow, keyed };
  });

  /* The point of the scene: an ambient wash below and nothing on the keys
   * until one is pressed, which one uniform strip could not show.
   */
  check('The two groups run different effects', rest.glow > 0 && rest.keyed === 0,
        `glow ${rest.glow}, keys ${rest.keyed}`);
}

/* The composer has three places that must agree about what the scene is: the
 * controls, the JSON, and the strip. Checking one would not notice the others
 * drifting.
 */
{
  await page.click('#clear-layers');
  await renderAt(120000);

  check('Clearing the layers leaves nothing lit', (await lit()) === 0);

  await page.selectOption('#add-type', 'solid');
  await page.click('#add-layer');
  await renderAt(120000);

  check('An added layer reaches the strip', (await lit()) > 0);

  const swatch = page.locator('.layer-card input[type="color"]').first();

  await swatch.fill('#ff0000');
  await swatch.dispatchEvent('change');
  await renderAt(120000);

  const first = await page.evaluate(() => Array.from(window.vfxDebug.pixels(0)).slice(0, 3));

  check('The colour picker reaches the strip', first.join(',') === '255,0,0', first.join(','));

  await page.selectOption('#add-type', 'ripple');
  await page.click('#add-layer');

  const mirrored = JSON.parse(await page.inputValue('#scene'));

  check('The JSON mirrors what the controls built',
        mirrored.layers.map(l => l.type).join('+') === 'solid+ripple',
        mirrored.layers.map(l => l.type).join('+'));

  await page.locator('.layer-card').nth(1).locator('button', { hasText: '↑' }).click();

  const reordered = JSON.parse(await page.inputValue('#scene'));

  check('Layers reorder', reordered.layers.map(l => l.type).join('+') === 'ripple+solid',
        reordered.layers.map(l => l.type).join('+'));

  await page.locator('.layer-card').nth(0).locator('button', { hasText: '✕' }).click();

  const removed = JSON.parse(await page.inputValue('#scene'));

  check('Layers remove', removed.layers.length === 1 && removed.layers[0].type === 'solid');
}

if (errors.length) console.log(`\npage errors:\n  ${errors.join('\n  ')}`);

await browser.close();

process.exit(failed || errors.length ? 1 : 0);
