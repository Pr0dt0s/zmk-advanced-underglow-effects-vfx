/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives sim/web/host.js against a fake WebHID device rather than a real
 * keyboard, since this project has none to test against (see the README).
 * The fake device speaks the exact wire format hid_transport.c does --
 * PING/PONG, GET_ALL, and acking SET_ or RESET -- so this is really a test of
 * host.js's half of the protocol: that it asks the right questions and
 * updates the page correctly from the answers.
 *
 * It does not, and cannot, prove that a real board's USB or BLE stack
 * round-trips a report the same way a fake EventTarget in a browser page
 * does. That gap only closes on real hardware.
 *
 *   python3 -m http.server 8899 -d docs/sim &
 *   node tools/verify-host-hid.mjs
 */

import { chromium } from 'playwright';

const PAGE = process.env.VFX_SIM_URL ?? 'http://127.0.0.1:8899/index.html';

const browser = await chromium.launch(
  process.env.CHROMIUM_PATH ? { executablePath: process.env.CHROMIUM_PATH } : {});
const page = await browser.newPage({ viewport: { width: 1440, height: 900 } });

const errors = [];
page.on('pageerror', e => errors.push(String(e)));
page.on('console', m => {
  if (m.type() === 'error' && (m.location()?.url ?? '').includes('favicon')) return;
  if (m.type() === 'error') errors.push(m.text());
});

/* Installed before any page script runs, so host.js's own `'hid' in
 * navigator` check finds this rather than nothing.
 */
await page.addInitScript(() => {
  const OP = { PING: 0, SET_HUE: 1, SET_LEVEL: 2, SET_SPEED: 3, RESET: 4, GET: 5, GET_ALL: 6 };
  const REPLY = 0x80;

  class FakeDevice extends EventTarget {
    constructor() {
      super();
      this.opened = false;
      this.collections = [{ usagePage: 0xff60, usage: 0x61 }];
      this.productName = 'Fake VFX keyboard';
      this.slots = new Map();
      for (let s = 1; s <= 3; s++) this.slots.set(s, { hue: 0, level: 255, speed: 0 });
    }

    async open() { this.opened = true; }
    async close() { this.opened = false; }

    reply(bytes) {
      /* A real transport answers asynchronously; queueMicrotask is enough
       * to make sure this is never mistaken for a synchronous call by code
       * that assumes otherwise.
       */
      queueMicrotask(() => {
        const ev = new Event('inputreport');

        ev.device = this;
        ev.reportId = 0;
        ev.data = new DataView(new Uint8Array(bytes).buffer);
        this.dispatchEvent(ev);
      });
    }

    stateBytes(slot, status) {
      const st = this.slots.get(slot) ?? { hue: 0, level: 0, speed: 0 };
      const h = st.hue & 0xffff;

      return [OP.GET | REPLY, slot, h & 0xff, (h >> 8) & 0xff, st.level, st.speed, status];
    }

    async sendReport(reportId, data) {
      window.__sentReports = (window.__sentReports ?? []).concat([Array.from(data)]);

      const b = data;
      const op = b[0];
      const slot = b[1];
      const st = this.slots.get(slot);

      if (op === OP.PING) {
        this.reply([OP.PING | REPLY, 1, this.slots.size]);
      } else if (op === OP.GET_ALL) {
        for (const s of this.slots.keys()) this.reply(this.stateBytes(s, 0));
      } else if (op === OP.GET) {
        this.reply(this.stateBytes(slot, st ? 0 : 1));
      } else if (op === OP.SET_HUE) {
        if (st) {
          const raw = b[2] | (b[3] << 8);

          st.hue = (((raw > 0x7fff ? raw - 0x10000 : raw) % 360) + 360) % 360;
        }

        this.reply([OP.SET_HUE | REPLY, slot, st ? 0 : 1]);
      } else if (op === OP.SET_LEVEL) {
        if (st) st.level = b[2];
        this.reply([OP.SET_LEVEL | REPLY, slot, st ? 0 : 1]);
      } else if (op === OP.SET_SPEED) {
        if (st) st.speed = b[2];
        this.reply([OP.SET_SPEED | REPLY, slot, st ? 0 : 1]);
      } else if (op === OP.RESET) {
        if (st) Object.assign(st, { hue: 0, level: 255, speed: 0 });
        this.reply([OP.RESET | REPLY, slot, st ? 0 : 1]);
      }
    }
  }

  const device = new FakeDevice();
  const hid = new EventTarget();

  hid.requestDevice = async () => [device];
  /* Empty rather than [device]: a real browser has not granted permission
   * to anything on a first visit either, and host.js's own reconnect path
   * is exercised by wire-connect below instead.
   */
  hid.getDevices = async () => [];

  window.__fakeDevice = device;
  Object.defineProperty(navigator, 'hid', { value: hid, configurable: true });
});

await page.goto(PAGE);
await page.waitForFunction(() => window.__fakeDevice !== undefined);

let failed = 0;

const check = (label, ok, detail = '') => {
  console.log(`  ${ok ? 'ok  ' : 'FAIL'}  ${label}${detail ? '   ' + detail : ''}`);
  if (!ok) failed++;
};

check('Starts disconnected rather than assuming a device',
      (await page.textContent('#host-status')) === 'not connected');

await page.click('#host-connect');
await page.waitForFunction(
  () => document.getElementById('host-status').textContent.startsWith('connected'));

check('Connecting pings and reports the protocol version and slot count',
      (await page.textContent('#host-status')) === 'connected — protocol v1, 3 tuning slots',
      await page.textContent('#host-status'));

await page.waitForFunction(() => document.querySelectorAll('#host-slots .layer-card').length === 3);

check('GET_ALL populates one card per slot', true);

const sliderValue = async (slot, field) => page.$eval(
  `#host-slots .layer-card:nth-of-type(${slot}) .field:has-text("${field}") input`,
  el => el.value);

check('Slot 1 starts at the fake device\'s own defaults',
      (await sliderValue(1, 'hue')) === '0' && (await sliderValue(1, 'level')) === '255' &&
      (await sliderValue(1, 'speed')) === '0');

const setSlider = async (slot, field, value) => {
  const el = page.locator(
    `#host-slots .layer-card:nth-of-type(${slot}) .field:has-text("${field}") input`);

  await el.fill(String(value));
  await el.dispatchEvent('change');
};

await setSlider(1, 'level', 128);
await page.waitForFunction(
  slot => window.__fakeDevice.slots.get(slot).level === 128, 1);

check('Dragging a slider sends SET_LEVEL to the device',
      (await page.evaluate(() => window.__fakeDevice.slots.get(1).level)) === 128);

check('The device never receives the slot the panel is not touching',
      (await page.evaluate(() => window.__fakeDevice.slots.get(2).level)) === 255);

await page.locator('#host-slots .layer-card:nth-of-type(1) button:has-text("Reset")').click();
await page.waitForFunction(() => window.__fakeDevice.slots.get(1).level === 255);

check('Reset restores the device state', (await sliderValue(1, 'level')) === '255',
      await sliderValue(1, 'level'));

await page.click('#host-disconnect');
await page.waitForFunction(() => document.getElementById('host-status').textContent === 'disconnected');

check('Disconnecting clears the slot cards',
      (await page.$$('#host-slots .layer-card')).length === 0);

if (errors.length) console.log(`\npage errors:\n  ${errors.join('\n  ')}`);

await browser.close();

process.exit(failed || errors.length ? 1 : 0);
