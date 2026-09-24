/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives sim/web/host.js against a fake WebHID device rather than a real
 * keyboard, since this project has none to test against (see the README).
 * The fake device speaks the exact wire format hid_transport.c does --
 * PING/PONG, per-slot tuning, and the ten-generator runtime scene ops -- so
 * this is really a test of host.js's half of the protocol: that it asks the
 * right questions and updates the page correctly from the answers.
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
 * navigator` check finds this rather than nothing. Two channels, each with
 * a small pool, is enough to exercise channel discovery stopping at the
 * right place and per-channel layer pools not leaking into each other,
 * without the test itself needing to know VFX_RT_MAX_LAYERS.
 */
await page.addInitScript(() => {
  const OP = {
    PING: 0, SET_HUE: 1, SET_LEVEL: 2, SET_SPEED: 3, RESET: 4, GET: 5, GET_ALL: 6,
    SCENE_RESET: 7, SCENE_ADD_LAYER: 8, SCENE_SET_ARG: 9, SCENE_SET_COLOR: 10,
    SCENE_REMOVE_LAYER: 11, SCENE_MOVE_LAYER: 12, SCENE_ACTIVATE: 13, SCENE_DEACTIVATE: 14,
    SCENE_GET_INFO: 15, SCENE_GET_LAYER: 16, SCENE_GET_ORDER: 17,
  };
  const REPLY = 0x80;
  const MAX_LAYERS = 3;

  const readI16 = (b, off) => {
    const u = b[off] | (b[off + 1] << 8);

    return u > 0x7fff ? u - 0x10000 : u;
  };
  const writeI16 = (buf, off, v) => {
    const u = v & 0xffff;

    buf[off] = u & 0xff;
    buf[off + 1] = (u >> 8) & 0xff;
  };

  class FakeDevice extends EventTarget {
    constructor() {
      super();
      this.opened = false;
      this.collections = [{ usagePage: 0xff60, usage: 0x61 }];
      this.productName = 'Fake VFX keyboard';
      this.slots = new Map();
      for (let s = 1; s <= 3; s++) this.slots.set(s, { hue: 0, level: 255, speed: 0 });

      /* Two channels, matching a two-channel board -- channel discovery in
       * host.js is expected to stop right after these.
       */
      this.channels = [0, 1].map(() => ({ active: false, slots: Array(MAX_LAYERS).fill(null),
                                          order: [] }));
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

    sceneAckBytes(op, slot, status) { return [op | REPLY, slot, status]; }

    sceneInfoBytes(ch, count, active, status) {
      return [OP.SCENE_GET_INFO | REPLY, ch, count, active ? 1 : 0, status];
    }

    sceneLayerBytes(ch, slot, l, status) {
      const buf = new Array(22).fill(0);

      buf[0] = OP.SCENE_GET_LAYER | REPLY;
      buf[1] = ch;
      buf[2] = slot;

      if (l) {
        buf[3] = l.type;
        buf[4] = l.zoneStart;
        buf[5] = l.zoneLen;
        buf[6] = l.blend;
        buf[7] = l.opacity;
        writeI16(buf, 8, l.hue);
        buf[10] = l.sat;
        buf[11] = l.bri;
        writeI16(buf, 12, l.args[0]);
        writeI16(buf, 14, l.args[1]);
        writeI16(buf, 16, l.args[2]);
        writeI16(buf, 18, l.args[3]);
        buf[20] = l.flags;
      }

      buf[21] = status;

      return buf;
    }

    sceneOrderBytes(ch, order, status) {
      return [OP.SCENE_GET_ORDER | REPLY, ch, order.length, ...order, status];
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
      } else if (op === OP.SCENE_RESET) {
        const ch = this.channels[slot];

        if (ch) { ch.slots.fill(null); ch.order = []; }
        this.reply(this.sceneAckBytes(op, slot, ch ? 0 : 1));
      } else if (op === OP.SCENE_GET_INFO) {
        const ch = this.channels[slot];

        this.reply(this.sceneInfoBytes(slot, ch ? ch.order.length : 0, ch ? ch.active : false,
                                       ch ? 0 : 1));
      } else if (op === OP.SCENE_ACTIVATE || op === OP.SCENE_DEACTIVATE) {
        const ch = this.channels[slot];

        if (ch) ch.active = op === OP.SCENE_ACTIVATE;
        this.reply(this.sceneAckBytes(op, slot, ch ? 0 : 1));
      } else if (op === OP.SCENE_ADD_LAYER) {
        const ch = this.channels[b[1]];

        if (!ch) {
          this.reply(this.sceneAckBytes(op, 0xff, 1));
        } else {
          const free = ch.slots.findIndex(s => s === null);

          if (free === -1) {
            this.reply(this.sceneAckBytes(op, 0xff, 2)); // POOL_FULL
          } else {
            ch.slots[free] = {
              type: b[2], zoneStart: b[3], zoneLen: b[4], blend: b[5], opacity: b[6],
              hue: readI16(b, 7), sat: b[9], bri: b[10],
              args: [readI16(b, 11), readI16(b, 13), readI16(b, 15), readI16(b, 17)],
              flags: b[19],
            };
            ch.order.push(free);
            this.reply(this.sceneAckBytes(op, free, 0));
          }
        }
      } else if (op === OP.SCENE_SET_ARG) {
        const ch = this.channels[b[1]];
        const l = ch && ch.slots[b[2]];
        const idx = b[3];
        const ok = l && idx < 4;

        if (ok) l.args[idx] = readI16(b, 4);
        this.reply(this.sceneAckBytes(op, b[2], ok ? 0 : 1));
      } else if (op === OP.SCENE_SET_COLOR) {
        const ch = this.channels[b[1]];
        const l = ch && ch.slots[b[2]];

        if (l) {
          l.hue = ((readI16(b, 3) % 360) + 360) % 360;
          l.sat = b[5];
          l.bri = b[6];
        }

        this.reply(this.sceneAckBytes(op, b[2], l ? 0 : 1));
      } else if (op === OP.SCENE_REMOVE_LAYER) {
        const ch = this.channels[b[1]];
        const l = ch && ch.slots[b[2]];

        if (l) {
          ch.slots[b[2]] = null;
          ch.order = ch.order.filter(s => s !== b[2]);
        }

        this.reply(this.sceneAckBytes(op, b[2], l ? 0 : 1));
      } else if (op === OP.SCENE_MOVE_LAYER) {
        const ch = this.channels[b[1]];
        const l = ch && ch.slots[b[2]];
        const dir = b[3] > 127 ? b[3] - 256 : b[3];
        let ok = false;

        if (l) {
          const pos = ch.order.indexOf(b[2]);
          const next = pos + dir;

          if (pos !== -1 && next >= 0 && next < ch.order.length) {
            [ch.order[pos], ch.order[next]] = [ch.order[next], ch.order[pos]];
            ok = true;
          }
        }

        this.reply(this.sceneAckBytes(op, b[2], ok ? 0 : 1));
      } else if (op === OP.SCENE_GET_LAYER) {
        const ch = this.channels[b[1]];
        const l = ch && ch.slots[b[2]];

        this.reply(this.sceneLayerBytes(b[1], b[2], l, l ? 0 : 1));
      } else if (op === OP.SCENE_GET_ORDER) {
        const ch = this.channels[slot];

        this.reply(this.sceneOrderBytes(slot, ch ? ch.order : [], ch ? 0 : 1));
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

check('Connecting populates one tuning card per slot', true);

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

/* ------------------------------------------------------ runtime scenes */

await page.waitForFunction(() => document.querySelectorAll('#host-channels button').length === 2);

check('Channel discovery stops at the fake device\'s own two channels', true);

check('The first channel is auto-selected and starts with an empty layer list',
      !(await page.isHidden('#host-scene')) &&
      (await page.$$('#host-scene-layers .layer-card')).length === 0);

await page.selectOption('#host-add-type', '7'); // pulse
await page.click('#host-add-layer');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card').length === 1);

check('Adding a layer sends SCENE_ADD_LAYER and the device assigns a slot',
      (await page.evaluate(() => window.__fakeDevice.channels[0].order.length)) === 1);

check('The new layer\'s card names the generator it was added as',
      (await page.textContent('#host-scene-layers .layer-card strong')) === 'pulse');

check('Pulse\'s own "stack" checkbox is shown, dart-only "axis" is not',
      !(await page.isHidden('#host-scene-layers .field:has-text("stack") input')) &&
      (await page.isHidden('#host-scene-layers .field:has-text("axis") select')));

/* Pulse's own arg labels (from RT_TYPES in host.js), not the generic "arg
 * N" -- the field shows what the argument means for the selected generator.
 */
const decayInput = page.locator('#host-scene-layers .field:has-text("decay ms") input');

await decayInput.fill('750');
await decayInput.dispatchEvent('change');
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.args[0] === 750);

check('Editing an arg sends SCENE_SET_ARG to the right slot', true);

const deviceSlot = await page.evaluate(() => window.__fakeDevice.channels[0].order[0]);

await page.click('#host-scene-layers .layer-card button[title="Remove"]');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card').length === 0);

check('Removing a layer sends SCENE_REMOVE_LAYER and clears its slot',
      (await page.evaluate(
        slot => window.__fakeDevice.channels[0].slots[slot] === null, deviceSlot)));

/* SCENE_GET_ORDER is the fix for a real gap: before it existed, a channel
 * re-read from scratch (a reconnect, or just reselecting the tab) had no
 * way to learn render order and fell back to slot-id probe order, which a
 * move leaves wrong. Two layers, a move, then forcing a full re-read by
 * switching channels away and back -- the same path a reconnect takes --
 * is what proves the fix actually closes that gap rather than only
 * looking right within one already-open session.
 */
await page.selectOption('#host-add-type', '0'); // solid
await page.click('#host-add-layer');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card').length === 1);

await page.selectOption('#host-add-type', '1'); // breathe
await page.click('#host-add-layer');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card').length === 2);

const namesBefore = await page.$$eval('#host-scene-layers .layer-card strong',
                                       els => els.map(e => e.textContent));

check('Two layers land in the order they were added', true, JSON.stringify(namesBefore));

await page.click('#host-scene-layers .layer-card:nth-of-type(2) button[title="Move earlier"]');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card')[0]
          ?.querySelector('strong')?.textContent === 'breathe');

await page.click('[data-ch="1"]');
await page.click('[data-ch="0"]');
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card').length === 2);

const namesAfter = await page.$$eval('#host-scene-layers .layer-card strong',
                                      els => els.map(e => e.textContent));

check('SCENE_GET_ORDER reproduces render order after a full refresh, not just session memory',
      JSON.stringify(namesAfter) === '["breathe","solid"]', JSON.stringify(namesAfter));

/* Back to empty, so the rest of this file can keep assuming channel 0
 * starts with nothing built when it gets to the active-checkbox check
 * below.
 */
for (let left = 2; left > 0; left--) {
  await page.click('#host-scene-layers .layer-card button[title="Remove"]');
  await page.waitForFunction(
    n => document.querySelectorAll('#host-scene-layers .layer-card').length === n, left - 1);
}

await page.click('#host-scene-active');
await page.waitForFunction(() => window.__fakeDevice.channels[0].active === true);

check('The active checkbox sends SCENE_ACTIVATE', true);

await page.click('[data-ch="1"]');
await page.waitForFunction(
  () => document.querySelector('#host-channels [data-ch="1"]').classList.contains('active'));

check('Switching channels shows the other channel\'s own (empty) scene, not the first one\'s',
      (await page.$$('#host-scene-layers .layer-card')).length === 0);

/* Fill channel 1's pool (MAX_LAYERS = 3 in the fake device) to check the
 * "pool full" status path, distinct from every other rejection.
 */
for (let i = 0; i < 3; i++) {
  await page.click('#host-add-layer');
  await page.waitForFunction(
    n => document.querySelectorAll('#host-scene-layers .layer-card').length === n, i + 1);
}

await page.click('#host-add-layer');
await page.waitForFunction(
  () => document.getElementById('host-status').textContent.includes('channel is full'));

check('A full channel\'s pool is reported distinctly from a plain refusal', true);

await page.click('#host-disconnect');
await page.waitForFunction(() => document.getElementById('host-status').textContent === 'disconnected');

check('Disconnecting clears the tuning and scene panels',
      (await page.$$('#host-slots .layer-card')).length === 0 &&
      (await page.$$('#host-channels button')).length === 0 &&
      (await page.isHidden('#host-scene')));

if (errors.length) console.log(`\npage errors:\n  ${errors.join('\n  ')}`);

await browser.close();

process.exit(failed || errors.length ? 1 : 0);
