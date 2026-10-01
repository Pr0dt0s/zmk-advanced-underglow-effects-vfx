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
 * If include/zmk/vfx/hid_protocol.h or runtime_scene.h changes, this has to
 * change by hand to match; tools/verify-host-hid.mjs is what would notice
 * the two drifting, against a fake HID device rather than real hardware,
 * which this project has none of to test against. See the README's own
 * note about that.
 */

import {
  AXES, GENERIC, RT_TYPES, argLabel, c, flagsFor, RT_FLAG_STACK, RT_FLAG_REVERSE,
  RT_FLAG_AXIS_SHIFT, RT_MAX_ARGS, BLENDS, OPACITY_SOURCES, blankLayer, newLayerData,
  composerToRuntime, runtimeToComposer, sameLayers, saveFile, parseSaveFile,
} from './scene-bridge.js';

const OP = {
  PING: 0x00,
  SET_HUE: 0x01,
  SET_LEVEL: 0x02,
  SET_SPEED: 0x03,
  RESET: 0x04,
  GET: 0x05,
  GET_ALL: 0x06,
  SCENE_RESET: 0x07,
  SCENE_ADD_LAYER: 0x08,
  SCENE_SET_ARG: 0x09,
  SCENE_SET_COLOR: 0x0a,
  SCENE_REMOVE_LAYER: 0x0b,
  SCENE_MOVE_LAYER: 0x0c,
  SCENE_ACTIVATE: 0x0d,
  SCENE_DEACTIVATE: 0x0e,
  SCENE_GET_INFO: 0x0f,
  SCENE_GET_LAYER: 0x10,
  SCENE_GET_ORDER: 0x11,
  SCENE_GRADIENT_ADD_STOP: 0x12,
  SCENE_GET_GRADIENT_STOP: 0x13,
  SCENE_SET_LIST_COLOR: 0x14,
  SCENE_GET_LIST_COLOR: 0x15,
  SCENE_SET_ZONE: 0x16,
  SCENE_GET_ZONE: 0x17,
  SCENE_SET_OPTS: 0x18,
  SCENE_GET_LAYER_EXT: 0x19,
  SCENE_SET_FLAGS: 0x1a,
  SCENE_COMMIT_LAYER: 0x1b,
  SCENE_VERIFY: 0x1c,
  SCENE_GET_SYNC: 0x1d,
  SCENE_RESYNC: 0x1e,
};

/* SCENE_ADD_LAYER's flags bit 5: reserve the layer without rendering it until
 * SCENE_COMMIT_LAYER (VFX_HID_FLAG_STAGED). SCENE_COMMIT_LAYER's position 0xFF
 * is "on top" (VFX_HID_POSITION_TOP).
 */
const FLAG_STAGED = 0x20;
const POSITION_TOP = 0xff;

const REPLY_BIT = 0x80;
const REPLY_PONG = OP.PING | REPLY_BIT;
const REPLY_STATE = OP.GET | REPLY_BIT;
const REPLY_SCENE_INFO = OP.SCENE_GET_INFO | REPLY_BIT;
const REPLY_SCENE_LAYER = OP.SCENE_GET_LAYER | REPLY_BIT;
const REPLY_SCENE_ORDER = OP.SCENE_GET_ORDER | REPLY_BIT;
const REPLY_LIST_COLOR = OP.SCENE_GET_LIST_COLOR | REPLY_BIT;
const REPLY_ZONE = OP.SCENE_GET_ZONE | REPLY_BIT;
const REPLY_LAYER_EXT = OP.SCENE_GET_LAYER_EXT | REPLY_BIT;
const REPLY_SYNC = OP.SCENE_GET_SYNC | REPLY_BIT;

const STATUS_OK = 0;
const STATUS_POOL_FULL = 2;
/* The central's relay queue is full, or a replay is running: nothing was
 * applied, so the same request can be sent again once it drains.
 */
const STATUS_BUSY = 3;

/* The wire's target byte: the runtime scene in the high nibble, the channel in
 * the low (VFX_RT_TARGET). Scene 0 is what a host that knows nothing of scenes
 * has always addressed.
 */
const target = (ch, scene = 0) => ((scene & 0xf) << 4) | (ch & 0xf);
const chOf = t => t & 0xf;
const sceneOf = t => t >> 4;
const TARGET_ALL = 0xff;

const PEER_STATES = ['none', 'unknown', 'match', 'MISMATCH', 'resyncing'];
const FEATURE_RETURN_CHANNEL = 0x01;

/* VFX_HID_ZONE_CHUNK: how many zone entries one SET_ZONE / ZONE carries. */
const ZONE_CHUNK = 12;

const ADD_LAYER_ARGS = 4;

/* VFX_RT_ZONE_*, in wire order. */
const ZONE_KINDS = ['range', 'pixels', 'keys'];


/* How many channels to probe for on connect. Larger than any real board is
 * expected to declare -- discovery stops at the first channel that answers
 * "out of range" (see discoverChannels()), so this is just an upper bound
 * on how long that search can run, not a number this page needs to get
 * right. Which slots exist within a channel, and what order they render
 * in, comes from SCENE_GET_ORDER instead of a similar probe.
 */
const MAX_CHANNEL_PROBE = 8;

/* The board's defaults from zmk-raw-hid's Kconfig. A board that changed
 * RAW_HID_USAGE_PAGE / RAW_HID_USAGE needs the matching filter here, which
 * is why both are broken out rather than inlined into requestDevice().
 */
const USAGE_PAGE = 0xff60;
const USAGE = 0x61;


/* Delays `fn` until `ms` after the last call, so a number field can send its
 * edit once the user pauses typing instead of only on blur -- 'change' never
 * fires until the field loses focus, which reads as "nothing happened" for
 * anyone who did not know to click away.
 */
const debounce = (fn, ms) => {
  let timer;

  return (...args) => {
    clearTimeout(timer);
    timer = setTimeout(() => fn(...args), ms);
  };
};


const writeI16 = (buf, offset, value) => {
  const v = value & 0xffff;

  buf[offset] = v & 0xff;
  buf[offset + 1] = (v >> 8) & 0xff;
};

export const requests = {
  ping: () => new Uint8Array([OP.PING]),
  setHue: (slot, hue) => {
    const b = new Uint8Array(4);

    b[0] = OP.SET_HUE;
    b[1] = slot;
    writeI16(b, 2, hue);

    return b;
  },
  setLevel: (slot, level) => new Uint8Array([OP.SET_LEVEL, slot, level]),
  setSpeed: (slot, speed) => new Uint8Array([OP.SET_SPEED, slot, speed]),
  reset: slot => new Uint8Array([OP.RESET, slot]),
  get: slot => new Uint8Array([OP.GET, slot]),

  sceneReset: ch => new Uint8Array([OP.SCENE_RESET, ch]),
  sceneAddLayer: (ch, type, zoneStart, zoneLen, blend, opacity, hue, sat, bri, args, flags,
                  staged = false) => {
    const b = new Uint8Array(20);

    b[0] = OP.SCENE_ADD_LAYER;
    b[1] = ch;
    b[2] = type;
    b[3] = zoneStart;
    b[4] = zoneLen;
    b[5] = blend;
    b[6] = opacity;
    writeI16(b, 7, hue);
    b[9] = sat;
    b[10] = bri;
    for (let i = 0; i < 4; i++) writeI16(b, 11 + i * 2, args[i] ?? 0);
    b[19] = (flags & ~FLAG_STAGED) | (staged ? FLAG_STAGED : 0);

    return b;
  },
  sceneSetFlags: (ch, slot, flags) => new Uint8Array([OP.SCENE_SET_FLAGS, ch, slot, flags]),
  sceneCommitLayer: (ch, slot, position = POSITION_TOP) =>
    new Uint8Array([OP.SCENE_COMMIT_LAYER, ch, slot, position]),
  sceneSetArg: (ch, slot, idx, value) => {
    const b = new Uint8Array(6);

    b[0] = OP.SCENE_SET_ARG;
    b[1] = ch;
    b[2] = slot;
    b[3] = idx;
    writeI16(b, 4, value);

    return b;
  },
  sceneSetColor: (ch, slot, hue, sat, bri) => {
    const b = new Uint8Array(7);

    b[0] = OP.SCENE_SET_COLOR;
    b[1] = ch;
    b[2] = slot;
    writeI16(b, 3, hue);
    b[5] = sat;
    b[6] = bri;

    return b;
  },
  sceneRemoveLayer: (ch, slot) => new Uint8Array([OP.SCENE_REMOVE_LAYER, ch, slot]),
  sceneMoveLayer: (ch, slot, dir) => new Uint8Array([OP.SCENE_MOVE_LAYER, ch, slot, dir & 0xff]),
  sceneActivate: ch => new Uint8Array([OP.SCENE_ACTIVATE, ch]),
  sceneDeactivate: ch => new Uint8Array([OP.SCENE_DEACTIVATE, ch]),
  sceneGetInfo: ch => new Uint8Array([OP.SCENE_GET_INFO, ch]),
  sceneGetLayer: (ch, slot) => new Uint8Array([OP.SCENE_GET_LAYER, ch, slot]),
  sceneGetOrder: ch => new Uint8Array([OP.SCENE_GET_ORDER, ch]),
  /* Entry `idx` of the colour list, for any type. Setting the next index is
   * how a list grows, which is why there is no separate "add" -- and why a
   * gradient's stops need nothing of their own here (the firmware still
   * answers SCENE_GRADIENT_ADD_STOP, this panel just has no use for it).
   */
  sceneSetListColor: (ch, slot, idx, hue, sat, bri) => {
    const b = new Uint8Array(8);

    b[0] = OP.SCENE_SET_LIST_COLOR;
    b[1] = ch;
    b[2] = slot;
    b[3] = idx;
    writeI16(b, 4, hue);
    b[6] = sat;
    b[7] = bri;

    return b;
  },
  sceneGetListColor: (ch, slot, idx) =>
    new Uint8Array([OP.SCENE_GET_LIST_COLOR, ch, slot, idx]),
  /* One chunk of a zone: `items` is at most ZONE_CHUNK entries, and `offset`
   * is how many entries the device already holds. Always sent full length --
   * the request's size is fixed per op, `count` says how much of it is real.
   * A range is a chunk of two: start, length.
   */
  sceneSetZone: (ch, slot, kind, offset, items) => {
    const b = new Uint8Array(6 + ZONE_CHUNK);

    b[0] = OP.SCENE_SET_ZONE;
    b[1] = ch;
    b[2] = slot;
    b[3] = kind;
    b[4] = offset;
    b[5] = items.length;
    items.forEach((v, i) => { b[6 + i] = v; });

    return b;
  },
  sceneGetZone: (ch, slot, offset) => new Uint8Array([OP.SCENE_GET_ZONE, ch, slot, offset]),
  sceneSetOpts: (ch, slot, o) =>
    new Uint8Array([OP.SCENE_SET_OPTS, ch, slot, o.blend, o.opacity, o.opacitySrc, o.opacityMin,
                    o.opacityFull, o.tuneId]),
  sceneGetLayerExt: (ch, slot) => new Uint8Array([OP.SCENE_GET_LAYER_EXT, ch, slot]),
  sceneVerify: t => new Uint8Array([OP.SCENE_VERIFY, t]),
  sceneGetSync: () => new Uint8Array([OP.SCENE_GET_SYNC]),
  sceneResync: t => new Uint8Array([OP.SCENE_RESYNC, t]),
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

  if (op === REPLY_SCENE_INFO) {
    return {
      kind: 'sceneInfo',
      ch: data.getUint8(1),
      count: data.getUint8(2),
      active: data.getUint8(3) !== 0,
      status: data.getUint8(4),
      // Older firmware stops at byte 4; one scene per channel, no hash.
      scenes: data.byteLength >= 11 ? data.getUint8(5) : 1,
      activeScene: data.byteLength >= 11 ? data.getUint8(6) : data.getUint8(3) !== 0 ? 0 : 0xff,
      hash: data.byteLength >= 11 ? data.getUint32(7, true) : null,
    };
  }

  if (op === REPLY_SCENE_LAYER) {
    return {
      kind: 'sceneLayer',
      ch: data.getUint8(1),
      slot: data.getUint8(2),
      type: data.getUint8(3),
      zoneStart: data.getUint8(4),
      zoneLen: data.getUint8(5),
      blend: data.getUint8(6),
      opacity: data.getUint8(7),
      hue: data.getInt16(8, true),
      sat: data.getUint8(10),
      bri: data.getUint8(11),
      args: [data.getInt16(12, true), data.getInt16(14, true), data.getInt16(16, true),
            data.getInt16(18, true)],
      flags: data.getUint8(20),
      status: data.getUint8(21),
    };
  }

  if (op === REPLY_SCENE_ORDER) {
    const count = data.getUint8(2);
    const order = [];

    for (let i = 0; i < count; i++) order.push(data.getUint8(3 + i));

    return { kind: 'sceneOrder', ch: data.getUint8(1), order, status: data.getUint8(3 + count) };
  }

  if (op === REPLY_LIST_COLOR) {
    return {
      kind: 'listColor',
      ch: data.getUint8(1),
      slot: data.getUint8(2),
      idx: data.getUint8(3),
      hue: data.getInt16(4, true),
      sat: data.getUint8(6),
      bri: data.getUint8(7),
      status: data.getUint8(8),
    };
  }

  if (op === REPLY_ZONE) {
    const n = data.getUint8(6);
    const items = [];

    for (let i = 0; i < n && i < ZONE_CHUNK; i++) items.push(data.getUint8(7 + i));

    return {
      kind: 'zone',
      ch: data.getUint8(1),
      slot: data.getUint8(2),
      zoneKind: data.getUint8(3),
      total: data.getUint8(4),
      offset: data.getUint8(5),
      items,
      status: data.getUint8(7 + ZONE_CHUNK),
    };
  }

  if (op === REPLY_LAYER_EXT) {
    return {
      kind: 'layerExt',
      ch: data.getUint8(1),
      slot: data.getUint8(2),
      zoneKind: data.getUint8(3),
      zoneCount: data.getUint8(4),
      numColors: data.getUint8(5),
      opacitySrc: data.getUint8(6),
      opacityMin: data.getUint8(7),
      opacityFull: data.getUint8(8),
      tuneId: data.getUint8(9),
      args45: [data.getInt16(10, true), data.getInt16(12, true)],
      status: data.getUint8(14),
    };
  }

  if (op === REPLY_SYNC) {
    const peers = [];

    for (let i = 0; i < 3; i++) {
      const state = data.getUint8(4 + i * 5);

      if (state) peers.push({ index: i, state, hash: data.getUint32(5 + i * 5, true) });
    }

    return {
      kind: 'sync',
      status: data.getUint8(1),
      features: data.getUint8(2),
      replaying: data.getUint8(3) !== 0,
      peers,
      queueHighWater: data.getUint8(19),
      refused: data.getUint8(20),
    };
  }

  /* Every SET_/SCENE_ or RESET ack shares this shape, op set to the
   * request's own op with the reply bit added -- SCENE_ADD_LAYER's included,
   * its "slot" byte carrying the newly assigned id rather than an echo.
   */
  return { kind: 'ack', op: op & ~REPLY_BIT, slot: data.getUint8(1), status: data.getUint8(2) };
}

const $ = id => document.getElementById(id);

export function initHostPanel() {
  const connectBtn = $('host-connect');
  const disconnectBtn = $('host-disconnect');
  const statusEl = $('host-status');
  const slotsEl = $('host-slots');
  const channelsEl = $('host-channels');
  const sceneEl = $('host-scene');
  const sceneActiveEl = $('host-scene-active');
  const sceneResetBtn = $('host-scene-reset');
  const sceneLayersEl = $('host-scene-layers');
  const addTypeEl = $('host-add-type');
  const addLayerBtn = $('host-add-layer');
  const addHintEl = $('host-add-hint');
  const scenesEl = $('host-scenes');
  const sendBtn = $('host-send');
  const loadBtn = $('host-load');
  const saveBtn = $('host-save');
  const openBtn = $('host-open');
  const openFileEl = $('host-open-file');
  const progressRow = $('host-progress-row');
  const progressEl = $('host-progress');
  const progressText = $('host-progress-text');
  const transferNote = $('host-transfer-note');
  const syncEl = $('host-sync');
  const verifyBtn = $('host-verify');
  const resyncBtn = $('host-resync');
  const syncRefreshBtn = $('host-sync-refresh');
  const paceEl = $('host-pace');
  const syncOut = $('host-sync-out');
  const stressCountEl = $('host-stress-count');
  const stressBtn = $('host-stress');
  const stressOut = $('host-stress-out');

  if (!connectBtn) return; // panel not on this page

  let device = null;
  let slots = new Map(); // tuning slot -> row elements
  let channels = []; // discovered channel ids, in probe order
  /* The wire target of what is being edited: channel in the low nibble, the
   * runtime scene in the high one (see target()). Every request below passes it
   * as its channel byte.
   */
  let activeChannel = null;
  let layerRows = new Map(); // device slot -> row elements, for the active target
  const sceneCounts = new Map(); // channel -> runtime scenes it holds
  const boardActive = new Map(); // channel -> its active runtime scene, 0xff for none
  const lastScene = new Map(); // channel -> the scene tab last open on it
  let lastInfo = null; // the active target's most recent GET_INFO
  let transferring = false; // a send or load owns the card list: tabs are inert
  let hasSync = false; // the firmware's GET_INFO carries a hash, so it speaks the sync ops

  /* Every wire send goes through transact() below and pushes one resolver
   * here; a real (or fake) device answers reports strictly in the order it
   * received them, so shifting the front of this queue on every inputreport
   * always matches it with the request that caused it, even when several
   * sends go out before the first reply comes back.
   */
  const replyQueue = [];

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

  for (const t of RT_TYPES) {
    const opt = document.createElement('option');

    opt.value = RT_TYPES.indexOf(t);
    opt.textContent = t.name;
    addTypeEl.appendChild(opt);
  }

  /* What "Add layer" is about to build, before it is built -- the card that
   * appears afterwards labels its own arg fields the same way (see
   * applyLayer()), but there is nothing to label until then, which is the
   * one moment a new user most wants to know what they are about to get.
   */
  const updateAddHint = () => {
    const spec = RT_TYPES[Number(addTypeEl.value)];
    const parts = [];

    if (spec.args.length) parts.push(`${spec.args.map(argLabel).join(', ')}`);
    if (spec.list) parts.push(spec.list.grow ? `a growable list of ${spec.list.grow}s` : 'its colours');

    addHintEl.textContent = parts.length
      ? `Starts with ${parts.join(' and ')} set to values from one of this module's own presets -- widen its zone and adjust from there. Sent as several messages, so the layer fills in over a moment.`
      : 'A flat colour across its zone -- no arguments to set.';
  };

  addTypeEl.addEventListener('change', updateAddHint);
  updateAddHint();

  /* ---------------------------------------------------------- tuning UI */

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
    hue.el.addEventListener('change', () => act(requests.setHue(slot, Number(hue.el.value))));
    level.el.addEventListener('change',
                              () => act(requests.setLevel(slot, Number(level.el.value))));
    speed.el.addEventListener('change',
                              () => act(requests.setSpeed(slot, Number(speed.el.value))));

    const resetBtn = document.createElement('button');

    resetBtn.className = 'tiny';
    resetBtn.textContent = 'Reset';
    /* The only ack whose effect the request itself does not already show in
     * the sliders: a reset's new values live in devicetree, not in anything
     * sent here, so refreshing means asking again.
     */
    resetBtn.addEventListener('click', async () => {
      await act(requests.reset(slot));
      applyState(await transact(requests.get(slot)));
    });
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

  /* ----------------------------------------------------------- scene UI */

  function selectTarget(t) {
    if (transferring) return null;

    activeChannel = t;
    lastScene.set(chOf(t), sceneOf(t));

    document.querySelectorAll('#host-channels button').forEach(b => {
      b.classList.toggle('active', Number(b.dataset.ch) === chOf(t));
    });

    renderSceneTabs();
    sceneEl.hidden = false;
    layerRows = new Map();
    sceneLayersEl.innerHTML = '';
    transferNote.textContent = '';

    return refreshChannel(t);
  }

  const selectChannel = ch => selectTarget(target(ch, lastScene.get(ch) ?? 0));

  function renderChannelTabs() {
    channelsEl.innerHTML = '';

    for (const ch of channels) {
      const b = document.createElement('button');

      b.textContent = `Channel ${ch}`;
      b.className = 'preset';
      b.dataset.ch = ch;
      b.addEventListener('click', () => selectChannel(ch));
      channelsEl.appendChild(b);
    }

    if (channels.length && activeChannel === null) selectChannel(channels[0]);
  }

  /* One tab per runtime scene the channel holds. A board built with one (the
   * default) gets no row: it looks exactly as it did before scenes.
   */
  function renderSceneTabs() {
    scenesEl.innerHTML = '';

    const ch = activeChannel === null ? 0 : chOf(activeChannel);
    const n = sceneCounts.get(ch) ?? 1;

    scenesEl.style.display = n < 2 ? 'none' : '';

    for (let k = 0; k < n && n > 1; k++) {
      const b = document.createElement('button');

      b.textContent = `Scene ${k + 1}` + (boardActive.get(ch) === k ? ' (showing)' : '');
      b.className = 'preset' + (activeChannel !== null && sceneOf(activeChannel) === k ? ' active' : '');
      b.dataset.scene = k;
      b.addEventListener('click', () => selectTarget(target(ch, k)));
      scenesEl.appendChild(b);
    }
  }

  /* --------------------------------------------------------- layer widgets */

  const byte = v => Math.max(0, Math.min(255, Math.round(v) || 0));

  function makeGrid(parent) {
    const grid = document.createElement('div');

    grid.className = 'layer-grid';
    parent.appendChild(grid);

    return grid;
  }

  const makeField = (grid, label, el, title) => {
    const wrap = document.createElement('label');

    wrap.className = 'field';
    wrap.textContent = label;
    if (title) wrap.title = title;
    wrap.appendChild(el);
    grid.appendChild(wrap);

    return wrap;
  };

  /* 'change' on a number input only fires on blur or Enter, which reads as
   * "nothing happened" to anyone who just typed a value and looked at the
   * strip. Debounced 'input' instead: it applies a moment after you stop
   * typing (or stop clicking the spinner), without sending a request per
   * keystroke.
   */
  const numberInput = onChange => {
    const el = document.createElement('input');

    el.type = 'number';
    el.addEventListener('input', debounce(() => onChange(Number(el.value)), 400));

    return el;
  };

  const fillOptions = (el, names) => {
    el.innerHTML = '';

    names.forEach((name, i) => {
      const opt = document.createElement('option');

      opt.value = i;
      opt.textContent = name;
      el.appendChild(opt);
    });
  };

  const selectInput = (names, onChange) => {
    const el = document.createElement('select');

    fillOptions(el, names);
    el.addEventListener('change', () => onChange(Number(el.value)));

    return el;
  };

  const colorInput = onChange => {
    const el = document.createElement('input');

    el.type = 'color';
    el.addEventListener('input', debounce(() => onChange(...hexToHsb(el.value)), 150));

    return el;
  };

  const hueOf = h => (h < 0 ? h + 360 : h);

  /* The colour list as the panel shows it: a type with fixed entries always
   * shows every one of them, the device reading any it has not been given as
   * black; a growable one shows just what it holds.
   */
  const listLabel = (spec, i) => spec.list?.labels?.[i] ?? `${spec.list?.grow ?? 'colour'} ${i}`;

  const listSlots = (spec, colors) => {
    const fixed = spec.list?.labels?.length ?? 0;
    const shown = colors.slice();

    while (shown.length < fixed) shown.push(c(0, 0, 0));

    return shown;
  };

  /* One card per layer, addressed by the device's own slot id rather than
   * by position: SCENE_MOVE_LAYER changes render order without changing
   * which slot a layer lives in, so a slot's card can stay the same DOM
   * node across a move and just get re-inserted at its new position.
   */
  function layerCard(slot) {
    if (layerRows.has(slot)) return layerRows.get(slot);

    const cur = () => layerRows.get(slot);
    const card = document.createElement('div');

    card.className = 'layer-card';
    card.dataset.slot = slot;

    const head = document.createElement('div');

    head.className = 'layer-head';
    head.innerHTML = '<strong></strong>';
    card.appendChild(head);

    const button = (label, title, fn, parent = head) => {
      const b = document.createElement('button');

      b.textContent = label;
      b.title = title;
      b.className = 'tiny';
      b.addEventListener('click', fn);
      parent.appendChild(b);

      return b;
    };

    button('↑', 'Move earlier', async () => {
      await act(requests.sceneMoveLayer(activeChannel, slot, -1));
      refreshChannel(activeChannel);
    });
    button('↓', 'Move later', async () => {
      await act(requests.sceneMoveLayer(activeChannel, slot, 1));
      refreshChannel(activeChannel);
    });
    button('✕', 'Remove', () => {
      act(requests.sceneRemoveLayer(activeChannel, slot));
      layerRows.delete(slot);
      card.remove();
    });

    const grid = makeGrid(card);

    const color = colorInput((h, s, v) => {
      const l = cur();

      if (!l) return;

      Object.assign(l.data, { hue: h, sat: s, bri: v });
      act(requests.sceneSetColor(activeChannel, slot, h, s, v));
    });
    const colorWrap = makeField(grid, 'colour', color,
      'This layer\'s own colour. A gradient and a layer-state layer have none: their colours are the list below.');

    /* One control per possible arg, each a number and a picker, one of which
     * applyLayer() shows according to what the generator's arg is.
     */
    const argCtls = [];

    for (let i = 0; i < RT_MAX_ARGS; i++) {
      const setArg = v => {
        const l = cur();

        if (!l) return;

        l.data.args[i] = v;
        act(requests.sceneSetArg(activeChannel, slot, i, v));
      };
      const num = numberInput(setArg);
      const sel = document.createElement('select');

      sel.addEventListener('change', () => setArg(Number(sel.value)));

      const wrap = makeField(grid, `arg ${i}`, num);

      wrap.appendChild(sel);
      argCtls.push({ wrap, num, sel });
    }

    /* stack, reverse and the axis ride in the flags byte, which
     * SCENE_SET_FLAGS rewrites in place: nothing is removed or re-added, so
     * the layer keeps its slot and its place in the stack.
     */
    const editFlag = (key, value) => {
      const l = cur();

      if (!l) return;

      l.data[key] = value;
      act(requests.sceneSetFlags(activeChannel, slot, flagsFor(l.data.type, l.data)));
    };

    const stackEl = document.createElement('input');

    stackEl.type = 'checkbox';
    stackEl.addEventListener('change', () => editFlag('stack', stackEl.checked));
    const stackWrap = makeField(grid, 'stack (each hit adds a new pulse)', stackEl,
      'Off: a new key press restarts the fade. On: it adds another pulse on top of any still fading.');

    const reverseEl = document.createElement('input');

    reverseEl.type = 'checkbox';
    reverseEl.addEventListener('change', () => editFlag('reverse', reverseEl.checked));
    const reverseWrap = makeField(grid, 'reverse', reverseEl, 'Runs the other way along its axis.');

    const axisEl = selectInput(AXES, v => editFlag('axis', v));
    const axisWrap = makeField(grid, 'axis', axisEl,
      'Which way this effect travels across the board. Needs pixel positions in devicetree; falls back to \'strip\' (along the wire) on a board with no map.');

    /* The colour list. Entries are edited in place; a list can grow (the
     * next index) but not shrink, so there is no remove.
     */
    const listWrap = document.createElement('div');

    listWrap.className = 'stops';

    const listHeading = document.createElement('div');

    listHeading.className = 'field';
    listWrap.appendChild(listHeading);

    const listEntries = document.createElement('div');

    listEntries.className = 'row';
    listWrap.appendChild(listEntries);

    const listAddRow = document.createElement('div');

    listAddRow.className = 'row';
    listWrap.appendChild(listAddRow);

    const listAdd = button('Add', 'Append a colour to the list', async () => {
      const l = cur();

      if (!l) return;

      /* No client-side cap check before sending: the device's own POOL_FULL
       * status is the one source of truth for "full", not a count this page
       * keeps a second copy of.
       */
      const idx = l.data.colors.length;
      const base = l.data.colors[idx - 1] ?? GENERIC;
      const msg = await act(requests.sceneSetListColor(activeChannel, slot, idx, base.h, base.s,
                                                       base.v));

      if (msg.status === STATUS_OK) {
        l.data.colors.push({ ...base });
        renderList(l);
      }
    }, listAddRow);

    card.appendChild(listWrap);

    /* The zone: a range is two numbers, a pixel or key list is sent to the
     * board a chunk at a time (see writeZone()).
     */
    const zoneSection = document.createElement('div');

    zoneSection.className = 'layer-section';
    card.appendChild(zoneSection);

    const zoneGrid = makeGrid(zoneSection);

    const applyRange = () => {
      const l = cur();

      if (!l) return;

      l.data.zoneKind = 0;
      l.data.zoneStart = byte(Number(zoneStartEl.value));
      l.data.zoneLen = byte(Number(zoneLenEl.value));
      act(requests.sceneSetZone(activeChannel, slot, 0, 0, [l.data.zoneStart, l.data.zoneLen]));
    };

    /* Picking pixels or keys only reveals the list box: nothing is sent
     * until Apply, because an empty list is a zone that draws nothing and
     * there is no reason to put the layer in that state just to type in one.
     */
    const showZoneEditor = kind => {
      const range = kind === 0;

      zoneStartWrap.style.display = range ? '' : 'none';
      zoneLenWrap.style.display = range ? '' : 'none';
      zoneListWrap.style.display = range ? 'none' : '';
      zoneApplyWrap.style.display = range ? 'none' : '';
      zoneListEl.placeholder = kind === 2 ? 'key positions, e.g. 0, 1, 2, 10'
        : 'strip pixel indices, e.g. 0, 1, 2, 10';
    };

    const zoneKindEl = selectInput(ZONE_KINDS, kind => {
      showZoneEditor(kind);
      if (kind === 0) applyRange();
    });
    const zoneKindWrap = makeField(zoneGrid, 'zone', zoneKindEl,
      'A range of pixels, an explicit list of strip pixels, or a list of key positions resolved through the board\'s key map.');
    const zoneStartEl = numberInput(applyRange);
    const zoneStartWrap = makeField(zoneGrid, 'zone start', zoneStartEl,
      'First pixel this layer covers, counting from the start of the channel\'s own strip.');
    const zoneLenEl = numberInput(applyRange);
    const zoneLenWrap = makeField(zoneGrid, 'zone len', zoneLenEl,
      'How many pixels wide, starting from zone start.');

    const zoneListEl = document.createElement('input');

    zoneListEl.type = 'text';
    const zoneListWrap = makeField(zoneGrid, 'entries', zoneListEl,
      'Whole numbers from 0 to 255, separated by commas or spaces.');

    const zoneApplyEl = document.createElement('span');
    const zoneApplyWrap = makeField(zoneGrid, '', zoneApplyEl);

    const zoneInfo = document.createElement('span');

    zoneInfo.className = 'note';
    zoneInfo.style.margin = '0';

    button('Apply list', 'Send this list to the board', async () => {
      const l = cur();

      if (!l) return;

      const items = zoneListEl.value.split(/[\s,]+/).filter(Boolean).map(Number);

      if (items.some(v => !Number.isInteger(v) || v < 0 || v > 255)) {
        setStatus('a zone list is whole numbers from 0 to 255', false);

        return;
      }

      const kind = Number(zoneKindEl.value);
      const msg = await writeZone(activeChannel, slot, kind, items);

      if (msg.status === STATUS_OK) {
        l.data.zoneKind = kind;
        l.data.zoneItems = items;
        zoneInfo.textContent = `${items.length} entries`;
      } else {
        refreshChannel(activeChannel); // a list cut short on the board is not what the box says
      }
    }, zoneApplyEl);
    zoneApplyEl.appendChild(zoneInfo);

    /* How the layer composites, in place: nothing animating restarts. */
    const optSection = document.createElement('details');

    optSection.className = 'layer-section';
    optSection.innerHTML = '<summary>Options: blend, opacity, tuning</summary>';
    card.appendChild(optSection);

    const optGrid = makeGrid(optSection);

    const opt = (key, label, make, title) => {
      const el = make(v => {
        const l = cur();

        if (!l) return;

        l.data[key] = v;
        act(requests.sceneSetOpts(activeChannel, slot, l.data));
      });

      return { wrap: makeField(optGrid, label, el, title), el };
    };

    const blendCtl = opt('blend', 'blend', cb => selectInput(BLENDS, cb),
      'How this layer combines with what is composited beneath it.');
    const opacityCtl = opt('opacity', 'opacity', numberInput,
      '0-255. With an opacity source, the value reached at full signal.');
    const srcCtl = opt('opacitySrc', 'opacity source', cb => selectInput(OPACITY_SOURCES, cb),
      'Let typing speed, charge or activity drive how strongly this layer shows.');
    const minCtl = opt('opacityMin', 'source min', numberInput,
      'Opacity when the signal reads zero. Ignored without an opacity source.');
    const fullCtl = opt('opacityFull', 'source full', numberInput,
      'Signal value that reaches full opacity: wpm for typing speed, percent for charge. 0 means 255.');
    const tuneCtl = opt('tuneId', 'tune id', numberInput,
      'Tuning slot (see Tuning above) this layer answers to. 0 for none.');

    sceneLayersEl.appendChild(card);

    const row = {
      card, head, color, colorWrap, argCtls,
      stackWrap, stackEl, reverseWrap, reverseEl, axisWrap, axisEl,
      listWrap, listHeading, listEntries, listAdd,
      zoneKindEl, zoneStartEl, zoneLenEl, zoneListEl, zoneInfo, showZoneEditor,
      optCtls: { blend: blendCtl, opacity: opacityCtl, opacitySrc: srcCtl, opacityMin: minCtl,
                 opacityFull: fullCtl, tuneId: tuneCtl },
      data: blankLayer(),
    };

    layerRows.set(slot, row);

    return row;
  }


  /* A colour per entry of the layer's list, each editable in place. */
  function renderList(row) {
    const spec = RT_TYPES[row.data.type] ?? RT_TYPES[0];
    const slot = Number(row.card.dataset.slot);
    const shown = listSlots(spec, row.data.colors);

    row.listEntries.innerHTML = '';
    row.listHeading.textContent = spec.list?.grow
      ? `${spec.list.grow}s (${row.data.colors.length})` : 'colours';

    shown.forEach(({ h, s, v }, i) => {
      const el = colorInput((nh, ns, nv) => {
        row.data.colors[i] = { h: nh, s: ns, v: nv };

        /* An entry past the end of a fixed list that was never sent grows the
         * list to here, the entries between reading as black -- which is what
         * the boxes already show for them.
         */
        for (let j = 0; j < i; j++) row.data.colors[j] ??= c(0, 0, 0);

        act(requests.sceneSetListColor(activeChannel, slot, i, nh, ns, nv));
      });
      const wrap = document.createElement('label');

      wrap.className = 'field';
      wrap.textContent = listLabel(spec, i);
      el.value = hsbToHex(hueOf(h), s, v);
      wrap.appendChild(el);
      row.listEntries.appendChild(wrap);
    });

    row.listAdd.textContent = `Add ${spec.list?.grow ?? 'colour'}`;
    row.listAdd.parentElement.style.display = spec.list?.grow ? '' : 'none';
    row.listWrap.style.display = spec.list ? '' : 'none';
  }

  /* SCENE_SET_ZONE, in as many messages as the list takes. A range is one.
   * A list goes a chunk at a time with each chunk's offset the number of
   * entries the board already holds: offset 0 starts the list over (and is
   * what switches the zone's kind), and a chunk that does not line up is
   * refused rather than leaving a hole, so a lost message shows up here as a
   * failed ack. An empty list is one empty chunk.
   */
  async function writeZone(ch, slot, kind, items) {
    if (kind === 0) return act(requests.sceneSetZone(ch, slot, 0, 0, items));

    let offset = 0;
    let msg;

    do {
      const chunk = items.slice(offset, offset + ZONE_CHUNK);

      msg = await act(requests.sceneSetZone(ch, slot, kind, offset, chunk));

      if (msg.status !== STATUS_OK) return msg;

      offset += chunk.length;
    } while (offset < items.length);

    return msg;
  }

  /* Everything about a layer SCENE_ADD_LAYER has no room for, sent to the
   * slot it was just given: the fifth and sixth numbers, the colour list, a
   * zone that is not a plain range, and the options past blend and opacity.
   * Each is a message of its own, which is the point of the protocol -- a
   * generator of any size is assembled from as many of them as it takes.
   * Left out when it is what a new layer already is.
   */
  async function writeLayerExtras(ch, slot, data) {
    const check = msg => (msg.status === STATUS_OK ? null : msg);

    for (let i = ADD_LAYER_ARGS; i < RT_MAX_ARGS; i++) {
      if (!data.args[i]) continue;

      const bad = check(await act(requests.sceneSetArg(ch, slot, i, data.args[i])));

      if (bad) return bad;
    }

    for (let i = 0; i < data.colors.length; i++) {
      const { h, s, v } = data.colors[i];
      const bad = check(await act(requests.sceneSetListColor(ch, slot, i, h, s, v)));

      if (bad) return bad;
    }

    if (data.zoneKind !== 0) {
      const bad = check(await writeZone(ch, slot, data.zoneKind, data.zoneItems));

      if (bad) return bad;
    }

    if (data.opacitySrc || data.opacityMin || data.opacityFull || data.tuneId) {
      const bad = check(await act(requests.sceneSetOpts(ch, slot, data)));

      if (bad) return bad;
    }

    return null;
  }

  /* A layer is built staged -- reserved on the board but invisible -- and only
   * committed once every message of it has landed, so a half-built layer is
   * never drawn, and one that cannot be finished (the pool ran out part way)
   * is removed instead of left behind. The commit is one message: the layer
   * appears all at once.
   */
  async function addLayer(ch, data, position = POSITION_TOP) {
    const added = await act(requests.sceneAddLayer(ch, data.type, data.zoneStart, data.zoneLen,
                                                    data.blend, data.opacity, data.hue, data.sat,
                                                    data.bri, data.args, flagsFor(data.type, data),
                                                    true));

    if (added.status !== STATUS_OK) return added;

    let failed = await writeLayerExtras(ch, added.slot, data);

    if (!failed) {
      const committed = await act(requests.sceneCommitLayer(ch, added.slot, position));

      if (committed.status !== STATUS_OK) failed = committed;
    }

    if (failed) {
      await act(requests.sceneRemoveLayer(ch, added.slot));

      return { ...added, status: failed.status };
    }

    return added;
  }

  function applyLayer(msg, ext, colors, zoneItems) {
    if (msg.status !== STATUS_OK || ext.status !== STATUS_OK) return;

    const type = RT_TYPES[msg.type] ?? RT_TYPES[0];
    const row = layerCard(msg.slot);
    const stack = (msg.flags & RT_FLAG_STACK) !== 0;
    const reverse = (msg.flags & RT_FLAG_REVERSE) !== 0;
    const axis = (msg.flags >> RT_FLAG_AXIS_SHIFT) & 0x7;

    /* SCENE_LAYER carries four numbers and LAYER_EXT the other two. Anything
     * past what the generator reads is zeroed rather than kept: a gradient's
     * reply uses the third to say how many stops it has.
     */
    const args = [...msg.args, ...ext.args45].map((v, i) => (i < type.args.length ? v : 0));

    row.card.dataset.slot = msg.slot;
    row.head.querySelector('strong').textContent = type.name;
    row.data = {
      type: RT_TYPES.indexOf(type),
      zoneKind: ext.zoneKind,
      zoneStart: msg.zoneStart,
      zoneLen: msg.zoneLen,
      zoneItems,
      blend: msg.blend,
      opacity: msg.opacity,
      opacitySrc: ext.opacitySrc,
      opacityMin: ext.opacityMin,
      opacityFull: ext.opacityFull,
      tuneId: ext.tuneId,
      hue: msg.hue,
      sat: msg.sat,
      bri: msg.bri,
      args,
      stack,
      reverse,
      axis,
      colors,
    };

    /* Inline style rather than the hidden attribute: .field sets
     * display:flex, which in the cascade outranks the UA stylesheet's
     * [hidden] rule (see app.js's renderChannelTabs() for the same note).
     */
    row.colorWrap.style.display = type.primary ? '' : 'none';
    if (type.primary) {
      row.colorWrap.firstChild.textContent = type.primary.label;
      row.color.value = hsbToHex(hueOf(msg.hue), msg.sat, msg.bri);
    }

    row.argCtls.forEach(({ wrap, num, sel }, i) => {
      const spec = type.args[i];

      wrap.style.display = spec === undefined ? 'none' : '';

      if (spec === undefined) return;

      wrap.firstChild.textContent = argLabel(spec);
      wrap.title = spec.title ?? '';

      const options = typeof spec === 'string' ? null : spec.options;

      num.style.display = options ? 'none' : '';
      sel.style.display = options ? '' : 'none';

      if (options) {
        fillOptions(sel, options);
        sel.value = args[i];
      } else {
        num.value = args[i];
      }
    });

    row.stackWrap.style.display = type.stack ? '' : 'none';
    row.stackEl.checked = stack;
    row.reverseWrap.style.display = type.reverse ? '' : 'none';
    row.reverseEl.checked = reverse;
    row.axisWrap.style.display = type.axis ? '' : 'none';
    row.axisEl.value = axis;

    renderList(row);

    row.zoneKindEl.value = ext.zoneKind;
    row.zoneStartEl.value = msg.zoneStart;
    row.zoneLenEl.value = msg.zoneLen;
    row.zoneListEl.value = zoneItems.join(', ');
    row.zoneInfo.textContent = ext.zoneKind === 0 ? '' : `${zoneItems.length} entries`;
    row.showZoneEditor(ext.zoneKind);

    for (const [key, { el }] of Object.entries(row.optCtls)) el.value = row.data[key];

    sceneLayersEl.appendChild(row.card); // re-insert at the end, in true render order
  }

  /* Re-reads everything about a channel from the device: its own info, its
   * render order, then each slot named in that order, in that order --
   * SCENE_GET_ORDER is what makes this exact rather than a guess, so a
   * channel selected fresh (including after a reconnect) looks the same as
   * one that has been open and edited the whole time.
   *
   * A layer takes several reads: SCENE_LAYER for what fits one report,
   * LAYER_EXT for the rest, then one message per colour in its list and one
   * per chunk of a zone that is a list.
   */
  async function refreshChannel(ch) {
    const info = await transact(requests.sceneGetInfo(ch));

    if (ch !== activeChannel) return null; // superseded by a later selection

    lastInfo = info;
    boardActive.set(chOf(ch), info.activeScene);
    sceneCounts.set(chOf(ch), info.scenes);
    renderSceneTabs();
    sceneActiveEl.checked = info.active;

    const orderMsg = await transact(requests.sceneGetOrder(ch));

    if (ch !== activeChannel) return null;

    const seen = new Set();

    for (const slot of orderMsg.order) {
      const layer = await transact(requests.sceneGetLayer(ch, slot));
      const ext = await transact(requests.sceneGetLayerExt(ch, slot));

      if (ch !== activeChannel) return null;

      if (layer.status !== STATUS_OK || ext.status !== STATUS_OK) continue;

      const colors = [];

      for (let i = 0; i < ext.numColors; i++) {
        const entry = await transact(requests.sceneGetListColor(ch, slot, i));

        if (ch !== activeChannel) return null;

        if (entry.status === STATUS_OK) {
          colors.push({ h: hueOf(entry.hue), s: entry.sat, v: entry.bri });
        }
      }

      const zoneItems = [];

      if (ext.zoneKind !== 0) {
        let total = ext.zoneCount;

        while (zoneItems.length < total) {
          const z = await transact(requests.sceneGetZone(ch, slot, zoneItems.length));

          if (ch !== activeChannel) return null;

          if (z.status !== STATUS_OK || !z.items.length) break;

          zoneItems.push(...z.items);
          total = z.total;
        }
      }

      applyLayer(layer, ext, colors, zoneItems);
      seen.add(slot);
    }

    for (const [slot, row] of layerRows) {
      if (!seen.has(slot)) {
        row.card.remove();
        layerRows.delete(slot);
      }
    }

    return boardLayers();
  }

  /* What the board holds for the open target, bottom of the stack first, as
   * the cards last read it. Only meaningful right after a refreshChannel().
   */
  const boardLayers = () => [...sceneLayersEl.children]
    .map(el => layerRows.get(Number(el.dataset.slot))?.data)
    .filter(Boolean);

  async function discoverChannels() {
    channels = [];

    for (let ch = 0; ch < MAX_CHANNEL_PROBE; ch++) {
      const info = await transact(requests.sceneGetInfo(ch));

      if (info.status !== STATUS_OK) break;

      channels.push(ch);
      hasSync ||= info.hash !== null;
      sceneCounts.set(ch, info.scenes);
      boardActive.set(ch, info.activeScene);
    }

    renderChannelTabs();
    syncEl.hidden = !hasSync;
  }

  sceneActiveEl.addEventListener('change', () => {
    if (activeChannel === null) return;

    act(sceneActiveEl.checked
      ? requests.sceneActivate(activeChannel)
      : requests.sceneDeactivate(activeChannel));
  });

  sceneResetBtn.addEventListener('click', async () => {
    if (activeChannel === null) return;

    await act(requests.sceneReset(activeChannel));
    layerRows = new Map();
    sceneLayersEl.innerHTML = '';
  });

  addLayerBtn.addEventListener('click', async () => {
    if (activeChannel === null) return;

    /* Defaults in RT_TYPES come from this module's own devicetree presets,
     * not a guess, so a freshly added layer already looks like something
     * rather than a flat colour or a rate too fast or slow to read.
     */
    await addLayer(activeChannel, newLayerData(Number(addTypeEl.value)));

    refreshChannel(activeChannel);
  });

  /* ------------------------------------- composer <-> board, and files */

  const setProgress = (done, total, text) => {
    progressRow.style.display = total ? '' : 'none';
    progressEl.max = total || 1;
    progressEl.value = done;
    progressText.textContent = text ?? '';
  };

  const note = (msg, ok = true) => {
    transferNote.textContent = msg;
    transferNote.style.color = ok ? '' : '#ff7b72';
  };

  const transferButtons = [sendBtn, loadBtn, saveBtn, openBtn, sceneResetBtn, addLayerBtn];

  async function transfer(body) {
    if (activeChannel === null || transferring) return;

    transferring = true;
    transferButtons.forEach(b => { b.disabled = true; });

    try {
      await body(activeChannel);
    } catch (err) {
      note(`failed: ${err.message}`, false);
    } finally {
      transferring = false;
      transferButtons.forEach(b => { b.disabled = false; });
      setProgress(0, 0);
    }
  }

  /* Replaces what the open target holds with `layers`: reset, then each layer
   * built staged and committed (see addLayer()), then activated, then the
   * board is read back and compared with what was meant to land.
   */
  function sendLayers(layers, warnings, activate = true) {
    return transfer(async t => {
      if (warnings.length) {
        const list = warnings.map(w => `  - ${w}`).join('\n');

        if (!window.confirm(`The board cannot hold this scene as written:\n\n${list}\n\nSend what fits?`)) {
          return;
        }
      }

      const total = layers.length + 2;

      setProgress(0, total, 'clearing the scene…');
      await act(requests.sceneReset(t));
      layerRows = new Map();
      sceneLayersEl.innerHTML = '';

      const sent = [];
      let failure = null;

      for (const [i, d] of layers.entries()) {
        const name = RT_TYPES[d.type].name;

        setProgress(i + 1, total, `layer ${i + 1} of ${layers.length} (${name})…`);

        const r = await addLayer(t, d);

        if (r.status !== STATUS_OK) {
          failure = `layer ${i + 1} (${name}) was refused (status ${r.status}); the rest were not sent`;
          break;
        }

        sent.push(d);
      }

      if (activate && sent.length) await act(requests.sceneActivate(t));

      setProgress(total - 1, total, 'reading it back…');

      const read = await refreshChannel(t);

      if (failure) note(failure, false);
      else if (!read) note('stopped: another scene was opened', false);
      else if (sameLayers(sent, read)) note(`sent ${sent.length} layer${sent.length === 1 ? '' : 's'}; the board reports exactly that`);
      else note('sent, but what the board reports differs from what was sent', false);
    });
  }

  const composerChannel = () => {
    const composer = window.vfxComposer;
    const ci = chOf(activeChannel);

    if (!composer) throw new Error('the composer is not on this page');
    if (!composer.channels()[ci]) throw new Error(`the composer has no channel ${ci}`);

    return { composer, ci };
  };

  sendBtn.addEventListener('click', () => {
    let scene;

    try {
      const { composer, ci } = composerChannel();

      scene = composer.getScene(ci);
    } catch (err) {
      note(err.message, false);

      return;
    }

    const { layers, warnings } = composerToRuntime(scene);

    sendLayers(layers, warnings);
  });

  loadBtn.addEventListener('click', () => transfer(async t => {
    const { composer, ci } = composerChannel();
    const layers = await refreshChannel(t);

    if (!layers) return;

    composer.setScene(ci, runtimeToComposer(layers, `board channel ${ci}`));
    note(`loaded ${layers.length} layer${layers.length === 1 ? '' : 's'} into the composer's channel ${ci}`);
  }));

  function download(name, obj) {
    const url = URL.createObjectURL(new Blob([JSON.stringify(obj, null, 2)],
                                             { type: 'application/json' }));
    const a = document.createElement('a');

    a.href = url;
    a.download = name;
    document.body.appendChild(a);
    a.click();
    a.remove();
    setTimeout(() => URL.revokeObjectURL(url), 1000);
  }

  saveBtn.addEventListener('click', () => transfer(async t => {
    const layers = await refreshChannel(t);

    if (!layers) return;

    download(`vfx-scene-ch${chOf(t)}-${sceneOf(t) + 1}.json`,
             saveFile(layers, { channel: chOf(t), sceneIndex: sceneOf(t),
                                active: sceneActiveEl.checked }));
    note(`saved ${layers.length} layer${layers.length === 1 ? '' : 's'}`);
  }));

  openBtn.addEventListener('click', () => openFileEl.click());

  openFileEl.addEventListener('change', async () => {
    const file = openFileEl.files[0];

    openFileEl.value = '';

    if (!file) return;

    try {
      const parsed = parseSaveFile(await file.text());

      await sendLayers(parsed.layers, parsed.warnings, parsed.active || parsed.channel === null);
    } catch (err) {
      note(`${file.name}: ${err.message}`, false);
    }
  });

  /* ------------------------------------------------- split link and sync */

  const hex32 = h => `0x${(h >>> 0).toString(16).padStart(8, '0')}`;
  const VERIFY_WAIT_MS = 600;

  function describeSync(sync) {
    if (sync.status !== STATUS_OK) return 'the board did not answer GET_SYNC';

    const lines = [];

    lines.push(sync.features & FEATURE_RETURN_CHANNEL
      ? 'return channel: on (peripherals report back)'
      : 'return channel: off (build with CONFIG_ZMK_VFX_SPLIT_REPORT=y; Resync still works)');

    if (lastInfo?.hash != null) lines.push(`central hash, open scene: ${hex32(lastInfo.hash)}`);

    if (!sync.peers.length) lines.push('no peripheral has reported yet');

    for (const p of sync.peers) {
      lines.push(`peripheral ${p.index}: ${PEER_STATES[p.state] ?? p.state}, last hash ${hex32(p.hash)}`);
    }

    lines.push(`relay queue high-water ${sync.queueHighWater} pieces, ${sync.refused} refused as busy` +
               (sync.replaying ? ', a resync is running' : ''));

    return lines.join('\n');
  }

  async function refreshSync() {
    const sync = await transact(requests.sceneGetSync());

    syncOut.textContent = describeSync(sync);

    return sync;
  }

  syncRefreshBtn.addEventListener('click', refreshSync);

  verifyBtn.addEventListener('click', async () => {
    syncOut.textContent = 'asking the peripherals to compare…';
    await act(requests.sceneVerify(TARGET_ALL));
    await sleep(VERIFY_WAIT_MS);
    await refreshSync();
  });

  resyncBtn.addEventListener('click', async () => {
    syncOut.textContent = 'replaying the central\'s scenes onto the peripherals…';
    await act(requests.sceneResync(TARGET_ALL));

    for (let i = 0; i < 60; i++) {
      await sleep(500);

      const sync = await refreshSync();

      if (!sync.replaying && !sync.peers.some(p => p.state === 4)) break;
    }
  });

  /* The measurement the pace and the board's relay interval are tuned from: a
   * run of small changes sent as fast as the pace allows, then a verify. BUSY
   * retries and the central's own refused count say whether the queue
   * overflowed; the peripherals' verdict says whether anything was lost.
   */
  stressBtn.addEventListener('click', () => transfer(async t => {
    const first = sceneLayersEl.children[0];

    if (!first) {
      stressOut.textContent = 'add a layer to this scene first: the test recolours it';
      return;
    }

    const slot = Number(first.dataset.slot);
    const data = layerRows.get(slot).data;
    const n = Math.max(1, Math.min(500, Number(stressCountEl.value) || 1));

    busyRetries = 0;

    let refused = 0;
    const t0 = performance.now();

    for (let i = 0; i < n; i++) {
      setProgress(i, n, `change ${i + 1} of ${n}…`);

      const msg = await act(requests.sceneSetColor(t, slot, (i * 37) % 360, 100, 100));

      if (msg.status !== STATUS_OK) refused++;
    }

    const ms = performance.now() - t0;

    await act(requests.sceneSetColor(t, slot, data.hue, data.sat, data.bri));

    const lines = [
      `${n} changes in ${Math.round(ms)} ms (${(ms / n).toFixed(1)} ms each) at ${paceMs} ms per piece`,
      `BUSY retries ${busyRetries}, never accepted ${refused}`,
    ];

    stressOut.textContent = lines.join('\n') + '\nverifying…';

    await act(requests.sceneVerify(TARGET_ALL));
    await sleep(VERIFY_WAIT_MS);

    const sync = await refreshSync();

    stressOut.textContent = lines.concat(describeSync(sync)).join('\n');
  }));

  /* ------------------------------------------------------- wire plumbing */

  function transact(bytes) {
    return new Promise(resolve => {
      replyQueue.push(resolve);
      dispatch(bytes);
    });
  }

  /* Fire a request whose reply only needs the generic "did it fail" check,
   * not a value the caller reads back. Still goes through the same queue as
   * transact(), so it never gets its reply crossed with a later transact()
   * call's.
   *
   * BUSY means the central's relay backlog was full (or a resync is
   * running) and applied nothing, so the identical request is simply sent
   * again after a growing wait. The retries are counted: they are how the
   * stress test shows that the pace is too fast.
   */
  const BUSY_RETRIES = 8;
  const sleep = ms => new Promise(resolve => setTimeout(resolve, ms));
  let busyRetries = 0;

  async function act(bytes) {
    let msg;

    for (let attempt = 0; ; attempt++) {
      msg = await transact(bytes);

      if (msg.kind !== 'ack' || msg.status !== STATUS_BUSY || attempt >= BUSY_RETRIES) break;

      busyRetries++;
      await sleep(Math.min(500, 20 * 2 ** attempt));
    }

    if (msg.kind === 'ack' && msg.status !== STATUS_OK) {
      const full = {
        [OP.SCENE_GRADIENT_ADD_STOP]: 'stop list is full',
        [OP.SCENE_SET_LIST_COLOR]: 'colour list is full',
        [OP.SCENE_SET_ZONE]: 'zone list is full',
        [OP.SCENE_ADD_LAYER]: 'channel is full (or out of trail/hold state)',
      };
      const label = msg.status === STATUS_BUSY ? 'busy: the split link backlog did not drain'
        : msg.status !== STATUS_POOL_FULL ? 'refused'
          : full[msg.op] ?? 'channel is full';

      setStatus(`op 0x${msg.op.toString(16)} ${label} (byte1 ${msg.slot})`, false);
    }

    await pace(bytes);

    return msg;
  }

  /* A split board's central hands each scene change to its peripheral in
   * 4-byte pieces over a paced queue (CONFIG_ZMK_VFX_RELAY_CHUNK_INTERVAL_MS
   * apart), and answers BUSY when that queue has no room. Waiting a moment per
   * piece keeps a long build from hitting BUSY at all. The value here is the
   * time per piece, and is yours to tune: the relay stress test below shows
   * whether a given figure keeps up.
   */
  const RELAY_OPS = new Set([
    OP.SCENE_RESET, OP.SCENE_ADD_LAYER, OP.SCENE_SET_ARG, OP.SCENE_SET_COLOR,
    OP.SCENE_REMOVE_LAYER, OP.SCENE_MOVE_LAYER, OP.SCENE_ACTIVATE, OP.SCENE_DEACTIVATE,
    OP.SCENE_GRADIENT_ADD_STOP, OP.SCENE_SET_LIST_COLOR, OP.SCENE_SET_ZONE, OP.SCENE_SET_OPTS,
    OP.SCENE_SET_FLAGS, OP.SCENE_COMMIT_LAYER,
  ]);
  const RELAY_CHUNK_BYTES = 4;
  const PACE_KEY = 'vfx-relay-pace-ms';
  const PACE_DEFAULT_MS = 4;

  const readPace = () => {
    try {
      const v = Number(localStorage.getItem(PACE_KEY));

      return localStorage.getItem(PACE_KEY) !== null && v >= 0 ? v : PACE_DEFAULT_MS;
    } catch {
      return PACE_DEFAULT_MS;
    }
  };

  let paceMs = readPace();

  paceEl.value = paceMs;
  paceEl.addEventListener('change', () => {
    paceMs = Math.max(0, Math.min(200, Number(paceEl.value) || 0));
    paceEl.value = paceMs;

    try {
      localStorage.setItem(PACE_KEY, String(paceMs));
    } catch { /* storage blocked: the value just does not persist */ }
  });

  function pace(bytes) {
    if (!RELAY_OPS.has(bytes[0]) || paceMs === 0) return Promise.resolve();

    return sleep(Math.ceil(bytes.length / RELAY_CHUNK_BYTES) * paceMs);
  }

  function onInputReport(event) {
    const msg = decodeReply(event.data);
    const resolve = replyQueue.shift();

    if (resolve) resolve(msg);
  }

  async function dispatch(bytes) {
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
    channels = [];
    activeChannel = null;
    channelsEl.innerHTML = '';
    scenesEl.innerHTML = '';
    sceneEl.hidden = true;
    syncEl.hidden = true;
    sceneCounts.clear();
    boardActive.clear();
    lastScene.clear();
    lastInfo = null;
    hasSync = false;
    replyQueue.length = 0;

    device.addEventListener('inputreport', onInputReport);

    if (!device.opened) await device.open();

    connectBtn.hidden = true;
    disconnectBtn.hidden = false;
    setStatus('connected — pinging…', true);

    const pong = await transact(requests.ping());

    setStatus(`connected — protocol v${pong.version}, ${pong.maxSlot} tuning slot` +
              (pong.maxSlot === 1 ? '' : 's'), true);

    for (let slot = 1; slot <= pong.maxSlot; slot++) {
      applyState(await transact(requests.get(slot)));
    }

    await discoverChannels();
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
    replyQueue.length = 0;
    slots = new Map();
    slotsEl.innerHTML = '';
    channels = [];
    activeChannel = null;
    channelsEl.innerHTML = '';
    scenesEl.innerHTML = '';
    sceneEl.hidden = true;
    syncEl.hidden = true;
    transferring = false;
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

/* HSB<->hex, matching app.js's own conversion exactly -- kept as its own
 * copy rather than a shared import so this file has no dependency on the
 * wasm-facing half of the page.
 */
function hsbToHex(h, s, b) {
  const v = b / 100, sat = s / 100;
  const c = v * sat;
  const x = c * (1 - Math.abs(((h / 60) % 2) - 1));
  const m = v - c;
  const seg = Math.floor((h % 360) / 60);
  const [r, g, bl] = [[c, x, 0], [x, c, 0], [0, c, x],
                      [0, x, c], [x, 0, c], [c, 0, x]][seg];
  const to = n => Math.round((n + m) * 255).toString(16).padStart(2, '0');

  return `#${to(r)}${to(g)}${to(bl)}`;
}

function hexToHsb(hex) {
  const n = parseInt(hex.slice(1), 16);
  const r = ((n >> 16) & 0xff) / 255, g = ((n >> 8) & 0xff) / 255, b = (n & 0xff) / 255;
  const max = Math.max(r, g, b), min = Math.min(r, g, b), d = max - min;

  let h = 0;

  if (d) {
    if (max === r) h = 60 * (((g - b) / d) % 6);
    else if (max === g) h = 60 * ((b - r) / d + 2);
    else h = 60 * ((r - g) / d + 4);
  }

  return [Math.round((h + 360) % 360), Math.round(max ? (d / max) * 100 : 0),
         Math.round(max * 100)];
}

if (typeof document !== 'undefined') initHostPanel();
