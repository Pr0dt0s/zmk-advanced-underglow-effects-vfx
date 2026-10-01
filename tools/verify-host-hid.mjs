/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives sim/web/host.js against a fake WebHID device rather than a real
 * keyboard, since this project has none to test against (see the README).
 * The fake device speaks the exact wire format hid_transport.c does --
 * PING/PONG, per-slot tuning, and the runtime scene ops that build any of the
 * 24 generators out of several messages -- so this is really a test of
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
 * navigator` check finds this rather than nothing. Two channels, each with
 * a small pool, is enough to exercise channel discovery stopping at the
 * right place and per-channel layer pools not leaking into each other,
 * without the test itself needing to know VFX_RT_MAX_LAYERS.
 *
 * The limits below mirror the firmware's own (runtime_scene.h, Kconfig): the
 * layer pool is deliberately tiny so a test can fill it, the rest are the
 * defaults.
 */
await page.addInitScript(() => {
  const OP = {
    PING: 0, SET_HUE: 1, SET_LEVEL: 2, SET_SPEED: 3, RESET: 4, GET: 5, GET_ALL: 6,
    SCENE_RESET: 7, SCENE_ADD_LAYER: 8, SCENE_SET_ARG: 9, SCENE_SET_COLOR: 10,
    SCENE_REMOVE_LAYER: 11, SCENE_MOVE_LAYER: 12, SCENE_ACTIVATE: 13, SCENE_DEACTIVATE: 14,
    SCENE_GET_INFO: 15, SCENE_GET_LAYER: 16, SCENE_GET_ORDER: 17,
    SCENE_GRADIENT_ADD_STOP: 18, SCENE_GET_GRADIENT_STOP: 19,
    SCENE_SET_LIST_COLOR: 20, SCENE_GET_LIST_COLOR: 21, SCENE_SET_ZONE: 22, SCENE_GET_ZONE: 23,
    SCENE_SET_OPTS: 24, SCENE_GET_LAYER_EXT: 25,
  };
  const REPLY = 0x80;
  const MAX_LAYERS = 3;
  const MAX_COLORS = 8; // VFX_RT_MAX_COLORS
  const MAX_ARGS = 6; // VFX_RT_MAX_ARGS
  const MAX_ZONE_ENTRIES = 32; // VFX_RT_MAX_ZONE_PIXELS
  const HEAVY_STATES = 2; // VFX_RT_HEAVY_STATES
  const ZONE_CHUNK = 12; // VFX_HID_ZONE_CHUNK
  const GRADIENT_TYPE = 10; // enum vfx_rt_type's VFX_RT_GRADIENT
  const isHeavy = t => t === 11 || t === 12; // VFX_RT_TRAIL, VFX_RT_HOLD
  const STATUS_BAD_SLOT = 1;
  const STATUS_POOL_FULL = 2;

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
        /* A gradient's own args[2] carries num_colors instead -- the only
         * place this reply had room to say how many stops it holds before
         * LAYER_EXT existed, and still how reply_scene_layer() in
         * hid_transport.c fills it.
         */
        writeI16(buf, 16, l.type === GRADIENT_TYPE ? l.colors.length : l.args[2]);
        writeI16(buf, 18, l.args[3]);
        buf[20] = l.flags;
      }

      buf[21] = status;

      return buf;
    }

    layerExtBytes(ch, slot, l, status) {
      const buf = new Array(15).fill(0);

      buf[0] = OP.SCENE_GET_LAYER_EXT | REPLY;
      buf[1] = ch;
      buf[2] = slot;

      if (l) {
        buf[3] = l.zoneKind;
        buf[4] = l.zoneKind === 0 ? 0 : l.zoneItems.length;
        buf[5] = l.colors.length;
        buf[6] = l.opacitySrc;
        buf[7] = l.opacityMin;
        buf[8] = l.opacityFull;
        buf[9] = l.tuneId;
        writeI16(buf, 10, l.args[4]);
        writeI16(buf, 12, l.args[5]);
      }

      buf[14] = status;

      return buf;
    }

    listColorBytes(ch, slot, idx, color, status) {
      const buf = new Array(9).fill(0);

      buf[0] = OP.SCENE_GET_LIST_COLOR | REPLY;
      buf[1] = ch;
      buf[2] = slot;
      buf[3] = idx;

      if (color) {
        writeI16(buf, 4, color.hue);
        buf[6] = color.sat;
        buf[7] = color.bri;
      }

      buf[8] = status;

      return buf;
    }

    zoneBytes(ch, slot, offset, l, status) {
      const buf = new Array(20).fill(0);

      buf[0] = OP.SCENE_GET_ZONE | REPLY;
      buf[1] = ch;
      buf[2] = slot;

      if (l) {
        /* A range reads back as its two numbers, a list as its entries. */
        const items = l.zoneKind === 0 ? [l.zoneStart, l.zoneLen] : l.zoneItems;
        const chunk = items.slice(offset, offset + ZONE_CHUNK);

        buf[3] = l.zoneKind;
        buf[4] = items.length;
        buf[5] = offset;
        buf[6] = chunk.length;
        chunk.forEach((v, i) => { buf[7 + i] = v; });
      }

      buf[19] = status;

      return buf;
    }

    sceneOrderBytes(ch, order, status) {
      return [OP.SCENE_GET_ORDER | REPLY, ch, order.length, ...order, status];
    }

    gradientStopBytes(ch, slot, idx, stop, status) {
      const buf = new Array(9).fill(0);

      buf[0] = OP.SCENE_GET_GRADIENT_STOP | REPLY;
      buf[1] = ch;
      buf[2] = slot;
      buf[3] = idx;

      if (stop) {
        writeI16(buf, 4, stop.hue);
        buf[6] = stop.sat;
        buf[7] = stop.bri;
      }

      buf[8] = status;

      return buf;
    }

    async sendReport(reportId, data) {
      window.__sentReports = (window.__sentReports ?? []).concat([Array.from(data)]);

      const b = data;
      const op = b[0];
      const slot = b[1];
      const st = this.slots.get(slot);
      const ch = this.channels[b[1]];
      const l = ch && ch.slots[b[2]];

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
        const target = this.channels[slot];

        if (target) { target.slots.fill(null); target.order = []; }
        this.reply(this.sceneAckBytes(op, slot, target ? 0 : 1));
      } else if (op === OP.SCENE_GET_INFO) {
        const target = this.channels[slot];

        this.reply(this.sceneInfoBytes(slot, target ? target.order.length : 0,
                                       target ? target.active : false, target ? 0 : 1));
      } else if (op === OP.SCENE_ACTIVATE || op === OP.SCENE_DEACTIVATE) {
        const target = this.channels[slot];

        if (target) target.active = op === OP.SCENE_ACTIVATE;
        this.reply(this.sceneAckBytes(op, slot, target ? 0 : 1));
      } else if (op === OP.SCENE_ADD_LAYER) {
        if (!ch) {
          this.reply(this.sceneAckBytes(op, 0xff, STATUS_BAD_SLOT));
        } else {
          const free = ch.slots.findIndex(s => s === null);
          const heavyUsed = ch.slots.filter(s => s && isHeavy(s.type)).length;

          if (free === -1 || (isHeavy(b[2]) && heavyUsed >= HEAVY_STATES)) {
            this.reply(this.sceneAckBytes(op, 0xff, STATUS_POOL_FULL));
          } else {
            /* Only the first four numbers ride here; the fifth and sixth, the
             * colour list, a zone that is not a range and the options past
             * blend and opacity all arrive as messages of their own.
             */
            ch.slots[free] = {
              type: b[2], zoneKind: 0, zoneStart: b[3], zoneLen: b[4], zoneItems: [],
              blend: b[5], opacity: b[6], opacitySrc: 0, opacityMin: 0, opacityFull: 0,
              tuneId: 0,
              hue: readI16(b, 7), sat: b[9], bri: b[10],
              args: [readI16(b, 11), readI16(b, 13), readI16(b, 15), readI16(b, 17), 0, 0],
              flags: b[19],
              colors: [],
            };
            ch.order.push(free);
            this.reply(this.sceneAckBytes(op, free, 0));
          }
        }
      } else if (op === OP.SCENE_SET_ARG) {
        const idx = b[3];
        const ok = l && idx < MAX_ARGS;

        if (ok) l.args[idx] = readI16(b, 4);
        this.reply(this.sceneAckBytes(op, b[2], ok ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_SET_COLOR) {
        if (l) {
          l.hue = ((readI16(b, 3) % 360) + 360) % 360;
          l.sat = b[5];
          l.bri = b[6];
        }

        this.reply(this.sceneAckBytes(op, b[2], l ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_REMOVE_LAYER) {
        if (l) {
          ch.slots[b[2]] = null;
          ch.order = ch.order.filter(s => s !== b[2]);
        }

        this.reply(this.sceneAckBytes(op, b[2], l ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_MOVE_LAYER) {
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

        this.reply(this.sceneAckBytes(op, b[2], ok ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_GET_LAYER) {
        this.reply(this.sceneLayerBytes(b[1], b[2], l, l ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_GET_LAYER_EXT) {
        this.reply(this.layerExtBytes(b[1], b[2], l, l ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_GET_ORDER) {
        const target = this.channels[slot];

        this.reply(this.sceneOrderBytes(slot, target ? target.order : [], target ? 0 : 1));
      } else if (op === OP.SCENE_SET_LIST_COLOR) {
        const idx = b[3];
        let status = STATUS_BAD_SLOT;

        if (l) {
          if (idx >= MAX_COLORS) {
            status = STATUS_POOL_FULL;
          } else {
            /* Setting past the end grows the list, the entries between
             * reading as black -- same as vfx_runtime_set_list_color().
             */
            while (l.colors.length < idx) l.colors.push({ hue: 0, sat: 0, bri: 0 });

            l.colors[idx] = { hue: ((readI16(b, 4) % 360) + 360) % 360, sat: b[6], bri: b[7] };
            status = 0;
          }
        }

        this.reply(this.sceneAckBytes(op, b[2], status));
      } else if (op === OP.SCENE_GET_LIST_COLOR) {
        const color = l ? l.colors[b[3]] : undefined;

        this.reply(this.listColorBytes(b[1], b[2], b[3], color, color ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_SET_ZONE) {
        const kind = b[3];
        const offset = b[4];
        const count = b[5];
        const data = Array.from(b.slice(6, 6 + ZONE_CHUNK)).slice(0, count);
        let status = STATUS_BAD_SLOT;

        if (l && kind === 0 && count >= 2) {
          Object.assign(l, { zoneKind: 0, zoneStart: data[0], zoneLen: data[1], zoneItems: [] });
          status = 0;
        } else if (l && (kind === 1 || kind === 2)) {
          /* Offset 0 restarts the list (and may change its kind); any other
           * offset has to be exactly how many entries are already held, so a
           * lost message shows up as a refusal instead of a hole.
           */
          const have = offset === 0 || l.zoneKind !== kind ? 0 : l.zoneItems.length;

          if (offset !== have) {
            status = STATUS_BAD_SLOT;
          } else if (offset + count > MAX_ZONE_ENTRIES) {
            status = STATUS_POOL_FULL;
          } else {
            l.zoneKind = kind;
            l.zoneItems = l.zoneItems.slice(0, offset).concat(data);
            status = 0;
          }
        }

        this.reply(this.sceneAckBytes(op, b[2], status));
      } else if (op === OP.SCENE_GET_ZONE) {
        this.reply(this.zoneBytes(b[1], b[2], b[3], l, l ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_SET_OPTS) {
        const ok = l && b[3] <= 4 && b[5] <= 3;

        if (ok) {
          Object.assign(l, {
            blend: b[3], opacity: b[4], opacitySrc: b[5], opacityMin: b[6], opacityFull: b[7],
            tuneId: b[8],
          });
        }

        this.reply(this.sceneAckBytes(op, b[2], ok ? 0 : STATUS_BAD_SLOT));
      } else if (op === OP.SCENE_GRADIENT_ADD_STOP) {
        /* Still answered, now as a spelling of "set the next list colour". */
        const ok = l && l.type === GRADIENT_TYPE && l.colors.length < MAX_COLORS;

        if (ok) l.colors.push({ hue: readI16(b, 3), sat: b[5], bri: b[6] });
        this.reply(this.sceneAckBytes(op, b[2],
          ok ? 0 : (l && l.type === GRADIENT_TYPE ? STATUS_POOL_FULL : STATUS_BAD_SLOT)));
      } else if (op === OP.SCENE_GET_GRADIENT_STOP) {
        const stop = l && l.type === GRADIENT_TYPE ? l.colors[b[3]] : undefined;

        this.reply(this.gradientStopBytes(b[1], b[2], b[3], stop, stop ? 0 : STATUS_BAD_SLOT));
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

/* ------------------------------------------------------------- helpers */

const CARDS = '#host-scene-layers .layer-card';

/* A control by its label. A field's text is its label followed by the option
 * names of any picker inside it, so labels are matched from the start.
 */
const field = re => page.locator('#host-scene-layers .field').filter({ hasText: re });
const numberOf = re => field(re).locator('input[type="number"]');

const waitCards = n => page.waitForFunction(
  k => document.querySelectorAll('#host-scene-layers .layer-card').length === k, n);

/* What the fake device holds for the layer at render position `i` of a channel. */
const layerAt = (i = 0, ch = 0) => page.evaluate(([pos, c]) => {
  const chan = window.__fakeDevice.channels[c];
  const slot = chan.order[pos];

  return slot === undefined ? null : JSON.parse(JSON.stringify({ slot, ...chan.slots[slot] }));
}, [i, ch]);

const orderTypes = (ch = 0) => page.evaluate(
  c => window.__fakeDevice.channels[c].order.map(s => window.__fakeDevice.channels[c].slots[s].type),
  ch);

const clearLayers = async (ch = 0) => {
  for (;;) {
    const n = (await page.$$(CARDS)).length;

    if (!n) break;

    await page.click(`${CARDS} button[title="Remove"]`);
    await waitCards(n - 1);
  }

  await page.waitForFunction(
    c => window.__fakeDevice.channels[c].order.length === 0, ch);
};

/* Forces a full re-read from the device, the path a reconnect takes. */
const reselect = async () => {
  await page.click('[data-ch="1"]');
  await page.click('[data-ch="0"]');
};

const addLayer = async typeIdx => {
  const before = (await page.$$(CARDS)).length;

  await page.selectOption('#host-add-type', String(typeIdx));
  await page.click('#host-add-layer');
  await waitCards(before + 1);
};

const mutatingOps = () => page.evaluate(() => (window.__sentReports ?? [])
  .filter(r => ![0x0f, 0x10, 0x11, 0x13, 0x15, 0x17, 0x19].includes(r[0])).map(r => r[0]));

/* ------------------------------------------------------ connect, tuning */

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

const sliderValue = async (slot, name) => page.$eval(
  `#host-slots .layer-card:nth-of-type(${slot}) .field:has-text("${name}") input`,
  el => el.value);

check('Slot 1 starts at the fake device\'s own defaults',
      (await sliderValue(1, 'hue')) === '0' && (await sliderValue(1, 'level')) === '255' &&
      (await sliderValue(1, 'speed')) === '0');

const setSlider = async (slot, name, value) => {
  const el = page.locator(
    `#host-slots .layer-card:nth-of-type(${slot}) .field:has-text("${name}") input`);

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
      !(await page.isHidden('#host-scene')) && (await page.$$(CARDS)).length === 0);

await addLayer(7); // pulse

check('Adding a layer sends SCENE_ADD_LAYER and the device assigns a slot',
      (await page.evaluate(() => window.__fakeDevice.channels[0].order.length)) === 1);

check('The new layer\'s card names the generator it was added as',
      (await page.textContent(`${CARDS} strong`)) === 'pulse');

check('Pulse\'s own "stack" checkbox is shown, dart-only "axis" is not',
      (await field(/^stack/).first().isVisible()) && (await field(/^axis/).count()) > 0 &&
      (await field(/^axis/).first().isHidden()));

/* Pulse's own arg labels (from RT_TYPES in host.js), not the generic "arg
 * N" -- the field shows what the argument means for the selected generator.
 */
await numberOf(/^decay ms/).fill('750');
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.args[0] === 750);

check('Editing an arg sends SCENE_SET_ARG to the right slot', true);

check('Args a generator does not have are not offered',
      (await field(/^arg 3/).count()) > 0 && (await field(/^arg 3/).first().isHidden()));

const pulseSlot = (await layerAt(0)).slot;

await page.click(`${CARDS} button[title="Remove"]`);
await waitCards(0);

check('Removing a layer sends SCENE_REMOVE_LAYER and clears its slot',
      await page.evaluate(slot => window.__fakeDevice.channels[0].slots[slot] === null,
                          pulseSlot));

/* SCENE_GET_ORDER is the fix for a real gap: before it existed, a channel
 * re-read from scratch (a reconnect, or just reselecting the tab) had no
 * way to learn render order and fell back to slot-id probe order, which a
 * move leaves wrong. Two layers, a move, then forcing a full re-read by
 * switching channels away and back -- the same path a reconnect takes --
 * is what proves the fix actually closes that gap rather than only
 * looking right within one already-open session.
 */
await addLayer(0); // solid
await addLayer(1); // breathe

const namesBefore = await page.$$eval(`${CARDS} strong`, els => els.map(e => e.textContent));

check('Two layers land in the order they were added',
      JSON.stringify(namesBefore) === '["solid","breathe"]', JSON.stringify(namesBefore));

await page.click(`${CARDS}:nth-of-type(2) button[title="Move earlier"]`);
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .layer-card')[0]
          ?.querySelector('strong')?.textContent === 'breathe');

await reselect();
await waitCards(2);

const namesAfter = await page.$$eval(`${CARDS} strong`, els => els.map(e => e.textContent));

check('SCENE_GET_ORDER reproduces render order after a full refresh, not just session memory',
      JSON.stringify(namesAfter) === '["breathe","solid"]', JSON.stringify(namesAfter));

await clearLayers();

/* ------------------------------------------- every generator, buildable */

/* The point of the multi-message protocol: each of the 24 generators can be
 * built from the panel. Per type: how many entries its colour list starts
 * with and how many numbers it has. Each is added, read back off the device
 * (what the firmware would hold), shown on the card, and removed again.
 */
const TYPES = [
  ['solid', 0, 0], ['breathe', 0, 3], ['wave', 0, 3], ['twinkle', 0, 3], ['plasma', 0, 3],
  ['ripple', 0, 3], ['keyflash', 0, 2], ['pulse', 0, 3], ['dart', 1, 3], ['static', 0, 3],
  ['gradient', 6, 2], ['trail', 0, 2], ['hold', 0, 1], ['water', 1, 6], ['matrix', 1, 6],
  ['fire', 1, 4], ['comet', 1, 3], ['cross', 1, 4], ['layer_state', 4, 0], ['battery', 2, 1],
  ['ble_profile', 2, 0], ['flag', 0, 2], ['wpm', 1, 2], ['peripheral_battery', 3, 2],
];

const offered = await page.$$eval('#host-add-type option', els => els.map(e => e.textContent));

check('The Add layer picker offers all 24 generators, in wire order',
      JSON.stringify(offered) === JSON.stringify(TYPES.map(t => t[0])), offered.length);

for (let i = 0; i < TYPES.length; i++) {
  const [name, nColors, nArgs] = TYPES[i];

  await addLayer(i);

  const l = await layerAt(0);

  /* A card's first grid is its colour, then one wrapper per possible number. */
  const shownArgs = await page.$eval(CARDS,
    card => [...card.querySelector('.layer-grid').children].slice(1, 7)
      .filter(w => w.style.display !== 'none').length);

  check(`${name}: built as type ${i} with ${nColors} list colours and ${nArgs} numbers`,
        l.type === i && l.colors.length === nColors && shownArgs === nArgs &&
        l.args.slice(nArgs).every(v => v === 0) &&
        (await page.textContent(`${CARDS} strong`)) === name,
        `device type ${l.type}, colours ${l.colors.length}, args ${JSON.stringify(l.args)}` +
        ` (${shownArgs} controls shown)`);

  await clearLayers();
}

/* ------------------------------------ one layer, built from many messages */

/* Water is the widest: six numbers (two more than SCENE_ADD_LAYER carries),
 * and a second colour. Watching the wire shows how it is assembled.
 */
await page.evaluate(() => { window.__sentReports = []; });
await addLayer(13); // water

const waterOps = await mutatingOps();

check('A water layer is one ADD_LAYER, then SET_ARG for numbers 4 and 5, then its colour',
      JSON.stringify(waterOps) === '[8,9,9,20]', JSON.stringify(waterOps));

const waterArgIdx = await page.evaluate(
  () => window.__sentReports.filter(r => r[0] === 9).map(r => r[3]));

check('The two extra numbers go to indexes 4 and 5',
      JSON.stringify(waterArgIdx) === '[4,5]', JSON.stringify(waterArgIdx));

const water = await layerAt(0);

check('Water starts with the preset\'s surface colour, crest colour and six numbers',
      water.hue === 205 && water.sat === 95 && water.bri === 30 &&
      JSON.stringify(water.args) === '[20,45,3200,700,255,6]' &&
      water.colors.length === 1 && water.colors[0].hue === 190,
      JSON.stringify(water));

check('The card labels the crest colour and the sixth number',
      (await field(/^crest colour/).first().isVisible()) &&
      (await field(/^damping/).first().isVisible()));

await numberOf(/^amplitude/).fill('100');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.args[4] === 100);

check('Editing the fifth number sends SET_ARG index 4 in place', true);

await field(/^crest colour/).locator('input[type="color"]').fill('#00ff00');
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.colors[0].hue === 120);

check('Editing the second colour sends SET_LIST_COLOR in place',
      (await layerAt(0)).colors[0].sat === 100);

await field(/^surface colour/).locator('input[type="color"]').fill('#0000ff');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.hue === 240);

check('Editing the first colour still sends SET_COLOR', true);

await reselect();
await waitCards(1);

check('A refresh reads the extra numbers and colour back: amplitude 100, damping 6, crest green',
      (await numberOf(/^amplitude/).inputValue()) === '100' &&
      (await numberOf(/^damping/).inputValue()) === '6' &&
      (await field(/^crest colour/).locator('input[type="color"]').inputValue()) === '#00ff00');

await clearLayers();

/* ----------------------------------------- an enumerated number is a picker */

await addLayer(17); // cross

await field(/^axes/).locator('select').selectOption('2');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.args[3] === 2);

check('A number with named values is a picker, and choosing one sends SET_ARG', true);

await clearLayers();

/* ------------------------------------------------------- colour lists */

await addLayer(10); // gradient

check('A gradient\'s own colour picker is not shown, its stops are',
      (await field(/^colour/).first().isHidden()) &&
      (await page.$$(`${CARDS} .stops input[type="color"]`)).length === 6);

await page.locator(`${CARDS} .stops input[type="color"]`).nth(2).fill('#ff0000');
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.colors[2].hue === 0);

check('Editing one stop sends SET_LIST_COLOR for that index only',
      (await layerAt(0)).colors.map(c => c.hue).join(',') === '0,60,0,200,280,330');

await page.click(`${CARDS} button:has-text("Add stop")`);
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.colors.length === 7);
await page.click(`${CARDS} button:has-text("Add stop")`);
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.colors.length === 8);

/* The card grows once the device has acked, which is a beat after it has
 * applied the change.
 */
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .stops input[type="color"]').length === 8);

check('Adding stops sets the next index, and the card grows a colour per stop', true);

await page.click(`${CARDS} button:has-text("Add stop")`);
await page.waitForFunction(
  () => document.getElementById('host-status').textContent.includes('colour list is full'));

check('A full colour list is reported distinctly, not as a plain refusal', true);

check('The rejected ninth colour did not grow the device\'s own list past the cap',
      (await layerAt(0)).colors.length === 8);

await reselect();
await waitCards(1);
await page.waitForFunction(
  () => document.querySelectorAll('#host-scene-layers .stops input[type="color"]').length === 8);

check('A refresh reads every stop back (LAYER_EXT count, then one LIST_COLOR each)', true);

check('A refresh shows the gradient\'s own two numbers, scroll speed 8 and span 0',
      (await numberOf(/^scroll speed/).inputValue()) === '8' &&
      (await field(/^span/).locator('input[type="number"]').inputValue()) === '0');

/* ------------------------------------------- flags need a rebuild, in place */

/* A flags byte (stack, reverse, axis) only rides SCENE_ADD_LAYER, so editing
 * one is the one change that removes and re-adds a layer. It has to come
 * back with everything the panel holds -- every stop including the one just
 * edited -- and in the position it was in, not on top.
 */
await addLayer(0); // solid, on top of the gradient

await page.evaluate(() => { window.__sentReports = []; });
await page.locator(`${CARDS}:nth-of-type(1)`).locator('.field').filter({ hasText: /^axisstrip/ })
  .locator('select').selectOption('2');

await page.waitForFunction(() => {
  const ch = window.__fakeDevice.channels[0];
  const types = ch.order.map(s => ch.slots[s].type);
  const g = ch.slots[ch.order[0]];

  return types.join() === '10,0' && ((g.flags >> 2) & 7) === 2 && g.colors.length === 8;
});

const rebuilt = await layerAt(0);

check('Changing an axis rebuilds the layer with its edited stops and all eight of them',
      rebuilt.colors.map(c => c.hue).join(',') === '0,60,0,200,280,330,330,330',
      rebuilt.colors.map(c => c.hue).join(','));

/* A gradient's layer reply puts its colour count in args[2]; carried back
 * into the rebuild it would land in the new layer's third number.
 */
const rebuildAdd = await page.evaluate(() => window.__sentReports.find(r => r[0] === 8 && r[2] === 10));

check('The rebuild does not send the colour count back as a number',
      rebuildAdd && rebuildAdd[15] === 0 && rebuildAdd[16] === 0, JSON.stringify(rebuildAdd));

check('The rebuilt layer is walked back to where it was, under the solid',
      JSON.stringify(await orderTypes()) === '[10,0]' &&
      JSON.stringify(await page.$$eval(`${CARDS} strong`, els => els.map(e => e.textContent)))
        === '["gradient","solid"]');

await clearLayers();

/* --------------------------------------------------------------- zones */

await addLayer(0); // solid

check('A new layer\'s zone is a range, shown as start and length',
      (await field(/^zone start/).first().isVisible()) &&
      (await field(/^entries/).first().isHidden()));

await field(/^zonerangepixelskeys/).locator('select').selectOption('1');

check('Choosing pixels shows the list box and sends nothing yet',
      (await field(/^entries/).first().isVisible()) && (await layerAt(0)).zoneKind === 0);

const pixels = Array.from({ length: 20 }, (_, i) => i);

await page.evaluate(() => { window.__sentReports = []; });
await field(/^entries/).locator('input').fill(pixels.join(', '));
await page.click(`${CARDS} button:has-text("Apply list")`);
await page.waitForFunction(
  () => window.__fakeDevice.channels[0].slots.find(s => s)?.zoneItems.length === 20);

const zoneSends = await page.evaluate(() => window.__sentReports.filter(r => r[0] === 22)
  .map(r => ({ kind: r[3], offset: r[4], count: r[5] })));

check('A 20-entry list goes in two chunks, 12 then 8, at offsets 0 and 12',
      JSON.stringify(zoneSends) ===
      JSON.stringify([{ kind: 1, offset: 0, count: 12 }, { kind: 1, offset: 12, count: 8 }]),
      JSON.stringify(zoneSends));

check('The device holds every entry in order as a pixel list',
      (await layerAt(0)).zoneKind === 1 && (await layerAt(0)).zoneItems.join() === pixels.join());

await reselect();
await waitCards(1);

check('A refresh reads the list back chunk by chunk into the box',
      (await field(/^entries/).locator('input').inputValue()) === pixels.join(', '));

const tooMany = Array.from({ length: 40 }, (_, i) => i);

await field(/^entries/).locator('input').fill(tooMany.join(', '));
await page.click(`${CARDS} button:has-text("Apply list")`);
await page.waitForFunction(
  () => document.getElementById('host-status').textContent.includes('zone list is full'));

check('A list longer than the board\'s cap is refused distinctly', true);

await page.waitForFunction(
  () => document.querySelector('#host-scene-layers .layer-card input[type="text"]')
          ?.value.split(',').length === 24);

check('What the board kept of a cut-short list is read back rather than left as typed',
      (await layerAt(0)).zoneItems.length === 24);

await field(/^zonerangepixelskeys/).locator('select').selectOption('0');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.zoneKind === 0);

check('Going back to a range sends SET_ZONE with its two numbers',
      (await layerAt(0)).zoneStart === 0 && (await layerAt(0)).zoneLen === 6);

const addsBefore = await page.evaluate(() => window.__sentReports.filter(r => r[0] === 8).length);

await numberOf(/^zone len/).fill('9');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.zoneLen === 9);

check('Editing a range changes it in place, without adding the layer again',
      (await page.evaluate(() => window.__sentReports.filter(r => r[0] === 8).length))
        === addsBefore);

await clearLayers();

/* ------------------------------------------------------------- options */

await addLayer(0); // solid
await page.click(`${CARDS} details summary`);

await field(/^blend/).locator('select').selectOption('1');
await page.waitForFunction(() => window.__fakeDevice.channels[0].slots.find(s => s)?.blend === 1);
await field(/^opacity source/).locator('select').selectOption('1');
await numberOf(/^source full/).fill('90');
await numberOf(/^tune id/).fill('2');
await page.waitForFunction(() => {
  const l = window.__fakeDevice.channels[0].slots.find(s => s);

  return l.opacitySrc === 1 && l.opacityFull === 90 && l.tuneId === 2;
});

const opts = await layerAt(0);

check('Blend, opacity source, source full and tune id all reach the device via SET_OPTS',
      opts.blend === 1 && opts.opacitySrc === 1 && opts.opacityFull === 90 && opts.tuneId === 2,
      JSON.stringify(opts));

check('Each option edit carries the others, so none is reset by another',
      opts.opacity === 255 && opts.opacityMin === 0);

await reselect();
await waitCards(1);
await page.click(`${CARDS} details summary`);

check('A refresh reads the options back into their controls',
      (await field(/^blend/).locator('select').inputValue()) === '1' &&
      (await field(/^opacity source/).locator('select').inputValue()) === '1' &&
      (await numberOf(/^source full/).inputValue()) === '90' &&
      (await numberOf(/^tune id/).inputValue()) === '2');

await clearLayers();

/* ---------------------------------------------- the per-pixel state pool */

await addLayer(11); // trail
await addLayer(12); // hold
await page.selectOption('#host-add-type', '11');
await page.click('#host-add-layer');
await page.waitForFunction(
  () => document.getElementById('host-status').textContent.includes('trail/hold'));

check('A third trail or hold is refused once the channel\'s per-pixel pool is used',
      (await page.$$(CARDS)).length === 2 && (await orderTypes()).length === 2);

await addLayer(0); // solid: the pool is not the layer pool, so this still fits

check('A layer that needs no per-pixel state still fits beside them',
      (await orderTypes()).join() === '11,12,0');

await clearLayers();

/* ------------------------------------------------------- activation etc */

await page.click('#host-scene-active');
await page.waitForFunction(() => window.__fakeDevice.channels[0].active === true);

check('The active checkbox sends SCENE_ACTIVATE', true);

await page.click('[data-ch="1"]');
await page.waitForFunction(
  () => document.querySelector('#host-channels [data-ch="1"]').classList.contains('active'));

check('Switching channels shows the other channel\'s own (empty) scene, not the first one\'s',
      (await page.$$(CARDS)).length === 0);

/* Fill channel 1's pool (MAX_LAYERS = 3 in the fake device) to check the
 * "pool full" status path, distinct from every other rejection.
 */
await page.selectOption('#host-add-type', '0');

for (let i = 0; i < 3; i++) {
  await page.click('#host-add-layer');
  await waitCards(i + 1);
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
