/* Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * A WebHID client for the raw-hid transport in hid_transport.c, talking the
 * wire format in include/zmk/vfx/hid_protocol.h. Kept out of app.js on
 * purpose: this is the one part of the page that is not the compositor --
 * it drives a real keyboard over USB or BLE rather than the wasm build of
 * the same source, and everything below is a JavaScript reimplementation of
 * a wire format rather than a build of the C that defines it, the same way
 * toDevicetree() in app.js is a reimplementation of the devicetree format
 * rather than a build of anything that reads it.
 *
 * If include/zmk/vfx/hid_protocol.h changes, this has to change by hand to
 * match; tools/verify-host-hid.mjs is what would notice the two drifting,
 * against a fake HID device rather than real hardware, which this project
 * has none of to test against. See the README's own note about that.
 */

const OP = {
  PING: 0x00,
  SET_HUE: 0x01,
  SET_LEVEL: 0x02,
  SET_SPEED: 0x03,
  RESET: 0x04,
  GET: 0x05,
  GET_ALL: 0x06,
};

const REPLY_BIT = 0x80;
const REPLY_PONG = OP.PING | REPLY_BIT;
const REPLY_STATE = OP.GET | REPLY_BIT;

const STATUS_OK = 0;

/* The board's defaults from zmk-raw-hid's Kconfig. A board that changed
 * RAW_HID_USAGE_PAGE / RAW_HID_USAGE needs the matching filter here, which
 * is why both are broken out rather than inlined into requestDevice().
 */
const USAGE_PAGE = 0xff60;
const USAGE = 0x61;

const requests = {
  ping: () => new Uint8Array([OP.PING]),
  setHue: (slot, hue) => {
    const b = new Uint8Array([OP.SET_HUE, slot, 0, 0]);
    /* Little-endian signed 16-bit, same as vfx_hid_encode_state packs it. */
    const v = hue & 0xffff;

    b[2] = v & 0xff;
    b[3] = (v >> 8) & 0xff;

    return b;
  },
  setLevel: (slot, level) => new Uint8Array([OP.SET_LEVEL, slot, level]),
  setSpeed: (slot, speed) => new Uint8Array([OP.SET_SPEED, slot, speed]),
  reset: slot => new Uint8Array([OP.RESET, slot]),
  get: slot => new Uint8Array([OP.GET, slot]),
  getAll: () => new Uint8Array([OP.GET_ALL]),
};

/* `data` is the report body WebHID hands the input-report listener; for a
 * report-id-less collection like this one's, byte 0 is already this
 * protocol's own first byte rather than a report id.
 */
function decodeReply(data) {
  const op = data.getUint8(0);

  if (op === REPLY_PONG) {
    return { kind: 'pong', version: data.getUint8(1), maxSlot: data.getUint8(2) };
  }

  if (op === REPLY_STATE) {
    return {
      kind: 'state',
      slot: data.getUint8(1),
      hue: data.getInt16(2, true),
      level: data.getUint8(4),
      speed: data.getUint8(5),
      status: data.getUint8(6),
    };
  }

  /* Every SET_ or RESET ack shares this shape, op set to the request's own
   * op with the reply bit added.
   */
  return { kind: 'ack', op: op & ~REPLY_BIT, slot: data.getUint8(1), status: data.getUint8(2) };
}

const $ = id => document.getElementById(id);

export function initHostPanel() {
  const connectBtn = $('host-connect');
  const disconnectBtn = $('host-disconnect');
  const statusEl = $('host-status');
  const slotsEl = $('host-slots');

  if (!connectBtn) return; // panel not on this page

  let device = null;
  let slots = new Map(); // slot -> row elements

  const setStatus = (msg, ok) => {
    statusEl.textContent = msg;
    statusEl.className = 'status ' + (ok ? 'ok' : 'err');
  };

  if (!('hid' in navigator)) {
    setStatus('WebHID is not available in this browser (Chrome or Edge, over HTTPS or localhost)',
              false);
    connectBtn.disabled = true;

    return;
  }

  function slotRow(slot) {
    if (slots.has(slot)) return slots.get(slot);

    const card = document.createElement('div');

    card.className = 'layer-card';
    card.innerHTML = `<div class="layer-head"><strong>Slot ${slot}</strong></div>`;

    const grid = document.createElement('div');

    grid.className = 'layer-grid';
    card.appendChild(grid);

    const field = (label, el) => {
      const wrap = document.createElement('label');

      wrap.className = 'field';
      wrap.textContent = label;
      wrap.appendChild(el);
      grid.appendChild(wrap);

      return el;
    };

    const range = (min, max, fmt) => {
      const wrap = document.createElement('span');
      const el = document.createElement('input');
      const out = document.createElement('output');

      el.type = 'range';
      el.min = min;
      el.max = max;
      out.textContent = fmt(Number(el.value));
      el.addEventListener('input', () => { out.textContent = fmt(Number(el.value)); });
      wrap.append(el, ' ', out);

      return { wrap, el };
    };

    const hue = range(0, 359, v => `${v}°`);
    const level = range(0, 255, v => `${Math.round(v / 255 * 100)}%`);
    /* 0 means "stop overriding, follow the channel", worth keeping reachable
     * rather than clamping the slider to 1.
     */
    const speed = range(0, 5, v => (v === 0 ? 'follows channel' : v));

    field('hue', hue.wrap);
    field('level', level.wrap);
    field('speed', speed.wrap);

    /* On release rather than on every tick: this goes over USB or BLE to a
     * real device, not to a wasm call in the same tab, and a slider dragged
     * across its range would otherwise queue a report per pixel of motion.
     */
    hue.el.addEventListener('change', () => send(requests.setHue(slot, Number(hue.el.value))));
    level.el.addEventListener('change',
                              () => send(requests.setLevel(slot, Number(level.el.value))));
    speed.el.addEventListener('change',
                              () => send(requests.setSpeed(slot, Number(speed.el.value))));

    const resetBtn = document.createElement('button');

    resetBtn.className = 'tiny';
    resetBtn.textContent = 'Reset';
    resetBtn.addEventListener('click', () => send(requests.reset(slot)));
    card.querySelector('.layer-head').appendChild(resetBtn);

    slotsEl.appendChild(card);

    const row = { card, hue: hue.el, level: level.el, speed: speed.el };

    slots.set(slot, row);

    return row;
  }

  function applyState(msg) {
    const row = slotRow(msg.slot);

    if (msg.status !== STATUS_OK) return;

    row.hue.value = msg.hue < 0 ? msg.hue + 360 : msg.hue;
    row.level.value = msg.level;
    row.speed.value = msg.speed;

    for (const el of [row.hue, row.level, row.speed]) {
      el.dispatchEvent(new Event('input'));
    }
  }

  function onInputReport(event) {
    const msg = decodeReply(event.data);

    if (msg.kind === 'pong') {
      setStatus(`connected — protocol v${msg.version}, ${msg.maxSlot} tuning slot` +
                (msg.maxSlot === 1 ? '' : 's'), true);
      device.sendReport(0, requests.getAll());
    } else if (msg.kind === 'state') {
      applyState(msg);
    } else if (msg.kind === 'ack') {
      if (msg.status !== STATUS_OK) {
        setStatus(`slot ${msg.slot} refused (op 0x${msg.op.toString(16)})`, false);
      } else if (msg.op === OP.RESET) {
        /* The only ack whose effect the request itself does not already
         * show in the sliders: a reset's new values live in devicetree, not
         * in anything sent here, so refreshing means asking again.
         */
        send(requests.get(msg.slot));
      }
    }
  }

  async function send(bytes) {
    if (!device?.opened) return;

    try {
      await device.sendReport(0, bytes);
    } catch (err) {
      setStatus(`send failed: ${err.message}`, false);
    }
  }

  async function attach(dev) {
    device = dev;
    slots = new Map();
    slotsEl.innerHTML = '';

    device.addEventListener('inputreport', onInputReport);

    if (!device.opened) await device.open();

    connectBtn.hidden = true;
    disconnectBtn.hidden = false;
    setStatus('connected — pinging…', true);

    await send(requests.ping());
  }

  connectBtn.addEventListener('click', async () => {
    try {
      const [dev] = await navigator.hid.requestDevice({
        filters: [{ usagePage: USAGE_PAGE, usage: USAGE }],
      });

      if (!dev) {
        setStatus('no device selected', false);
        return;
      }

      await attach(dev);
    } catch (err) {
      setStatus(`connect failed: ${err.message}`, false);
    }
  });

  disconnectBtn.addEventListener('click', async () => {
    if (device) {
      device.removeEventListener('inputreport', onInputReport);
      if (device.opened) await device.close();
    }

    device = null;
    slots = new Map();
    slotsEl.innerHTML = '';
    connectBtn.hidden = false;
    disconnectBtn.hidden = true;
    setStatus('disconnected', false);
  });

  navigator.hid.addEventListener('disconnect', ({ device: dev }) => {
    if (dev === device) disconnectBtn.click();
  });

  /* A device granted permission on an earlier visit can be reattached
   * without asking again, which is what makes the page usable without a
   * click every reload.
   */
  navigator.hid.getDevices().then(known => {
    const match = known.find(d => d.collections.some(
      c => c.usagePage === USAGE_PAGE && c.usage === USAGE));

    if (match) attach(match);
    else setStatus('not connected', false);
  });
}

initHostPanel();
