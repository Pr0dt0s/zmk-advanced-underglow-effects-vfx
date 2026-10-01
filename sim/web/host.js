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

const STATUS_OK = 0;
const STATUS_POOL_FULL = 2;

/* VFX_HID_ZONE_CHUNK: how many zone entries one SET_ZONE / ZONE carries. */
const ZONE_CHUNK = 12;

/* VFX_RT_MAX_ARGS: SCENE_ADD_LAYER carries the first four, SCENE_SET_ARG all
 * six. Water and matrix use every one.
 */
const RT_MAX_ARGS = 6;
const ADD_LAYER_ARGS = 4;

/* VFX_RT_ZONE_*, in wire order. */
const ZONE_KINDS = ['range', 'pixels', 'keys'];

/* VFX_BLEND_* and VFX_SRC_* from dt-bindings/zmk/vfx.h, in numeric order. */
const BLENDS = ['normal', 'add', 'multiply', 'screen', 'max'];
const OPACITY_SOURCES = ['none', 'wpm', 'battery', 'activity'];

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

/* VFX_AXIS_* from dt-bindings/zmk/vfx.h, in numeric order. Carried in bits
 * 2-4 of a layer's flags byte, for every generator that has an axis.
 */
const AXES = ['strip', 'x', 'y', 'radial', 'angle', 'spiral'];

const c = (h, s, v) => ({ h, s, v });

/* The colour every layer started as before any type had colours of its own. */
const GENERIC = c(200, 90, 100);

/* Every generator the firmware can build, in the same order runtime_scene.h's
 * enum vfx_rt_type uses -- index into this array is the wire's `type` byte.
 *
 *   primary   the layer's own colour (SCENE_ADD_LAYER's hue/sat/bri, edited by
 *             SCENE_SET_COLOR); null for a type that has none.
 *   list      the colour list (SCENE_SET_LIST_COLOR), in the order
 *             rebuild_slot() in runtime_scene.c reads it: `labels` names the
 *             fixed entries of a generator that draws with a second colour,
 *             `grow` names the entries of one whose list is the point (a
 *             gradient's stops, a layer-state layer's colour per keymap layer)
 *             and can be extended. `defs` is what a fresh layer starts with.
 *   args      the small numbers, in the order rebuild_slot() reads them. A
 *             string is a free number; { label, options } is one whose values
 *             are a short enumeration, shown as a picker.
 *   stack, reverse, axis
 *             which of the flags byte's bits mean something to this generator.
 *
 * `defaultArgs` and the colours are not arbitrary -- each is a real value from
 * one of this module's own devicetree presets (dts/vfx/presets.dtsi), so a
 * freshly added layer already looks like something rather than a guess at what
 * "period ms" or "scale" ought to be. Where no preset uses a generator (wave,
 * keyflash, trail, flag, wpm, peripheral-battery) they come from its own
 * dt-binding's documented default instead. `defaultAxis` is the preset's
 * choice of direction where it is not the generic 'strip'.
 */
const P = { label: 'colour', def: GENERIC };

const RT_TYPES = [
  { name: 'solid', primary: P, args: [], defaultArgs: [] },
  { name: 'breathe', primary: P, args: ['period ms', 'min level', 'hue swing'],
    defaultArgs: [4500, 20, 0] },
  { name: 'wave', primary: P, args: ['wavelength', 'period ms', 'depth'], axis: true,
    defaultArgs: [20, 2000, 255] },
  { name: 'twinkle', primary: P, args: ['period ms', 'density', 'hue spread'],
    defaultArgs: [1600, 55, 0] },
  { name: 'plasma', primary: P, args: ['scale', 'period ms', 'hue spread'],
    defaultArgs: [14, 7000, 70] },
  { name: 'ripple', primary: P, args: ['decay ms', 'speed', 'width'],
    defaultArgs: [700, 60, 15] },
  { name: 'keyflash', primary: P, args: ['decay ms', 'spread'], defaultArgs: [400, 12] },
  { name: 'pulse', primary: P, args: ['decay ms', 'min level', 'hue step'], stack: true,
    defaultArgs: [900, 40, 7] },
  { name: 'dart', primary: { label: 'tail colour', def: c(285, 90, 70) },
    list: { labels: ['head colour'], defs: [c(300, 20, 100)] },
    args: ['speed', 'lifetime ms', 'tail'], axis: true, reverse: true,
    defaultArgs: [110, 1100, 28], defaultAxis: 1 /* VFX_AXIS_X */ },
  { name: 'static', primary: P, args: ['period ms', 'density', 'hue spread'],
    defaultArgs: [70, 45, 40] },
  /* No colour of its own: its stops are the colour list, added to and edited
   * one at a time.
   */
  { name: 'gradient', primary: null,
    list: { grow: 'stop', defs: [c(0, 100, 70), c(60, 100, 70), c(120, 100, 70),
                                 c(200, 100, 70), c(280, 100, 70), c(330, 100, 70)] },
    args: ['scroll speed', 'span'], axis: true, defaultArgs: [8, 0] },
  { name: 'trail', primary: P, args: ['decay ms', 'spread'], defaultArgs: [1500, 12] },
  { name: 'hold', primary: { label: 'colour', def: c(45, 55, 90) }, args: ['release ms'],
    defaultArgs: [260] },
  { name: 'water', primary: { label: 'surface colour', def: c(205, 95, 30) },
    list: { labels: ['crest colour'], defs: [c(190, 30, 100)] },
    args: ['wavelength', 'speed', 'lifetime ms', 'drop rate ms', 'amplitude', 'damping'],
    defaultArgs: [20, 45, 3200, 700, 255, 6] },
  { name: 'matrix', primary: { label: 'tail colour', def: c(125, 100, 55) },
    list: { labels: ['head colour'], defs: [c(110, 25, 100)] },
    args: ['speed', 'tail', 'drop rate ms', 'columns', 'jitter', 'head size'],
    defaultArgs: [55, 34, 260, 12, 90, 9] },
  { name: 'fire', primary: { label: 'base colour', def: c(0, 100, 55) },
    list: { labels: ['tip colour'], defs: [c(45, 75, 100)] },
    args: ['period ms', 'cell', 'height', 'flicker'], axis: true,
    defaultArgs: [420, 12, 235, 200], defaultAxis: 2 /* VFX_AXIS_Y */ },
  { name: 'comet', primary: { label: 'tail colour', def: c(265, 95, 75) },
    list: { labels: ['head colour'], defs: [c(280, 25, 100)] },
    args: ['period ms', 'tail', 'count'], axis: true,
    defaultArgs: [2600, 45, 2], defaultAxis: 1 /* VFX_AXIS_X */ },
  { name: 'cross', primary: { label: 'arm colour', def: c(190, 90, 60) },
    list: { labels: ['centre colour'], defs: [c(40, 20, 100)] },
    args: ['decay ms', 'radius', 'thickness',
           { label: 'axes', options: ['both', 'horizontal', 'vertical'] }],
    defaultArgs: [600, 0, 4, 0] },
  /* Its colour per keymap layer, layer 0 first: black draws nothing, so the
   * layers underneath show through it.
   */
  { name: 'layer_state', primary: null,
    list: { grow: 'layer', defs: [c(0, 0, 0), c(50, 100, 70), c(280, 100, 70), c(0, 100, 70)] },
    args: [], defaultArgs: [] },
  { name: 'battery', primary: { label: 'low colour', def: c(0, 100, 80) },
    list: { labels: ['high colour', 'empty colour'], defs: [c(120, 100, 70), c(0, 0, 0)] },
    args: ['warn below %'], defaultArgs: [25] },
  { name: 'ble_profile', primary: { label: 'connected colour', def: c(210, 100, 80) },
    list: { labels: ['disconnected colour', 'usb colour'],
            defs: [c(20, 100, 50), c(120, 100, 70)] },
    args: [], defaultArgs: [] },
  { name: 'flag', primary: { label: 'colour', def: c(0, 100, 80) },
    args: [{ label: 'source', options: ['locks', 'modifiers'] },
           { label: 'mask',
             title: 'Which bits light the zone; any one is enough. Locks: num 1, caps 2, scroll 4, compose 8, kana 16. Modifiers: ctrl 17, shift 34, alt 68, gui 136 (each covers both sides). Add them together for more than one.' }],
    defaultArgs: [0, 2] },
  { name: 'wpm', primary: { label: 'idle colour', def: c(220, 80, 20) },
    list: { labels: ['fast colour'], defs: [c(0, 100, 100)] },
    args: ['full wpm', { label: 'mode', options: ['colour', 'bar'] }],
    defaultArgs: [80, 0] },
  { name: 'peripheral_battery', primary: { label: 'low colour', def: c(0, 100, 80) },
    list: { labels: ['high colour', 'empty colour', 'unknown colour'],
            defs: [c(120, 100, 70), c(0, 0, 0), c(0, 0, 0)] },
    args: ['peripheral', 'warn below %'], defaultArgs: [0, 20] },
];

const argLabel = a => (typeof a === 'string' ? a : a.label);

const RT_FLAG_STACK = 0x01;
const RT_FLAG_REVERSE = 0x02;
const RT_FLAG_AXIS_SHIFT = 2;

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

const flagsFor = (typeIdx, { stack, reverse, axis }) => {
  const spec = RT_TYPES[typeIdx];
  let f = 0;

  if (spec.stack && stack) f |= RT_FLAG_STACK;
  if (spec.reverse && reverse) f |= RT_FLAG_REVERSE;
  if (spec.axis) f |= (axis & 0x7) << RT_FLAG_AXIS_SHIFT;

  return f;
};

const writeI16 = (buf, offset, value) => {
  const v = value & 0xffff;

  buf[offset] = v & 0xff;
  buf[offset + 1] = (v >> 8) & 0xff;
};

const requests = {
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

  if (!connectBtn) return; // panel not on this page

  let device = null;
  let slots = new Map(); // tuning slot -> row elements
  let channels = []; // discovered channel ids, in probe order
  let activeChannel = null;
  let layerRows = new Map(); // device slot -> row elements, for the active channel

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

  function selectChannel(ch) {
    activeChannel = ch;

    document.querySelectorAll('#host-channels button').forEach(b => {
      b.classList.toggle('active', Number(b.dataset.ch) === ch);
    });

    sceneEl.hidden = false;
    layerRows = new Map();
    sceneLayersEl.innerHTML = '';

    refreshChannel(ch);
  }

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

    /* stack, reverse and the axis ride in the flags byte, which only
     * SCENE_ADD_LAYER carries: changing one builds the layer again (see
     * rebuildFromScratch()) rather than editing it in place.
     */
    const editFlag = (key, value) => {
      const l = cur();

      if (!l) return;

      l.data[key] = value;
      rebuildFromScratch(slot, l.data);
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

  function blankLayer() {
    return {
      type: 0, zoneKind: 0, zoneStart: 0, zoneLen: 0, zoneItems: [],
      blend: 0, opacity: 255, opacitySrc: 0, opacityMin: 0, opacityFull: 0, tuneId: 0,
      hue: 0, sat: 0, bri: 0, args: new Array(RT_MAX_ARGS).fill(0),
      stack: false, reverse: false, axis: 0, colors: [],
    };
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
    for (let i = ADD_LAYER_ARGS; i < RT_MAX_ARGS; i++) {
      if (data.args[i]) await act(requests.sceneSetArg(ch, slot, i, data.args[i]));
    }

    for (let i = 0; i < data.colors.length; i++) {
      const { h, s, v } = data.colors[i];
      const msg = await act(requests.sceneSetListColor(ch, slot, i, h, s, v));

      if (msg.status !== STATUS_OK) break; // the rest would be refused too
    }

    if (data.zoneKind !== 0) await writeZone(ch, slot, data.zoneKind, data.zoneItems);

    if (data.opacitySrc || data.opacityMin || data.opacityFull || data.tuneId) {
      await act(requests.sceneSetOpts(ch, slot, data));
    }
  }

  async function addLayer(ch, data) {
    const added = await act(requests.sceneAddLayer(ch, data.type, data.zoneStart, data.zoneLen,
                                                    data.blend, data.opacity, data.hue, data.sat,
                                                    data.bri, data.args, flagsFor(data.type, data)));

    if (added.status === STATUS_OK) await writeLayerExtras(ch, added.slot, data);

    return added;
  }

  /* What Add layer builds: the type's own defaults, colours included. */
  function newLayerData(type) {
    const spec = RT_TYPES[type];
    const primary = spec.primary?.def ?? GENERIC;
    const data = blankLayer();

    Object.assign(data, {
      type,
      zoneLen: 6, // a safe starting width on any channel, even the smallest a board declares
      hue: primary.h, sat: primary.s, bri: primary.v,
      axis: spec.defaultAxis ?? 0,
      colors: (spec.list?.defs ?? []).map(d => ({ ...d })),
    });
    spec.defaultArgs.forEach((v, i) => { data.args[i] = v; });

    return data;
  }

  /* Changing a flag (stack, reverse, axis) is the one edit with no message of
   * its own, so it builds the layer again from what the panel holds and puts
   * it back where it was: a fresh layer is added at the top of the stack, so
   * it is walked down to its old position rather than left there.
   */
  async function rebuildFromScratch(slot, data) {
    const ch = activeChannel;
    const cards = [...sceneLayersEl.children];
    const position = cards.findIndex(el => Number(el.dataset.slot) === slot);

    await act(requests.sceneRemoveLayer(ch, slot));

    const added = await addLayer(ch, data);

    if (added.status === STATUS_OK && position >= 0) {
      for (let i = position; i < cards.length - 1; i++) {
        await act(requests.sceneMoveLayer(ch, added.slot, -1));
      }
    }

    /* Simplest to just ask the device what the channel looks like now
     * rather than guess.
     */
    layerRows.delete(slot);
    const card = sceneLayersEl.querySelector(`[data-slot="${slot}"]`);

    if (card) card.remove();

    refreshChannel(ch);
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

    if (ch !== activeChannel) return; // superseded by a later selection

    sceneActiveEl.checked = info.active;

    const orderMsg = await transact(requests.sceneGetOrder(ch));

    if (ch !== activeChannel) return;

    const seen = new Set();

    for (const slot of orderMsg.order) {
      const layer = await transact(requests.sceneGetLayer(ch, slot));
      const ext = await transact(requests.sceneGetLayerExt(ch, slot));

      if (ch !== activeChannel) return;

      if (layer.status !== STATUS_OK || ext.status !== STATUS_OK) continue;

      const colors = [];

      for (let i = 0; i < ext.numColors; i++) {
        const entry = await transact(requests.sceneGetListColor(ch, slot, i));

        if (ch !== activeChannel) return;

        if (entry.status === STATUS_OK) {
          colors.push({ h: hueOf(entry.hue), s: entry.sat, v: entry.bri });
        }
      }

      const zoneItems = [];

      if (ext.zoneKind !== 0) {
        let total = ext.zoneCount;

        while (zoneItems.length < total) {
          const z = await transact(requests.sceneGetZone(ch, slot, zoneItems.length));

          if (ch !== activeChannel) return;

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
  }

  async function discoverChannels() {
    channels = [];

    for (let ch = 0; ch < MAX_CHANNEL_PROBE; ch++) {
      const info = await transact(requests.sceneGetInfo(ch));

      if (info.status !== STATUS_OK) break;

      channels.push(ch);
    }

    renderChannelTabs();
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
   */
  function act(bytes) {
    return transact(bytes).then(async msg => {
      if (msg.kind === 'ack' && msg.status !== STATUS_OK) {
        const full = {
          [OP.SCENE_GRADIENT_ADD_STOP]: 'stop list is full',
          [OP.SCENE_SET_LIST_COLOR]: 'colour list is full',
          [OP.SCENE_SET_ZONE]: 'zone list is full',
          [OP.SCENE_ADD_LAYER]: 'channel is full (or out of trail/hold state)',
        };
        const label = msg.status !== STATUS_POOL_FULL ? 'refused'
          : full[msg.op] ?? 'channel is full';

        setStatus(`op 0x${msg.op.toString(16)} ${label} (byte1 ${msg.slot})`, false);
      }

      await pace(bytes);

      return msg;
    });
  }

  /* A split board's central hands each scene change to its peripheral in
   * 4-byte pieces, fire and forget -- there is no reply from the far side to
   * wait for -- so a burst of changes can outrun the link. The reply this
   * page just got only says the central applied it; waiting a moment per
   * piece before the next send keeps a long build (a water layer is an add, a
   * list of colours and two more numbers) from queueing faster than it drains.
   * The figure is a guess, not a measurement: this project has no hardware
   * to time it on (see the README).
   */
  const RELAY_OPS = new Set([
    OP.SCENE_RESET, OP.SCENE_ADD_LAYER, OP.SCENE_SET_ARG, OP.SCENE_SET_COLOR,
    OP.SCENE_REMOVE_LAYER, OP.SCENE_MOVE_LAYER, OP.SCENE_ACTIVATE, OP.SCENE_DEACTIVATE,
    OP.SCENE_GRADIENT_ADD_STOP, OP.SCENE_SET_LIST_COLOR, OP.SCENE_SET_ZONE, OP.SCENE_SET_OPTS,
    OP.SCENE_SET_FLAGS, OP.SCENE_COMMIT_LAYER,
  ]);
  const RELAY_CHUNK_BYTES = 4;
  const RELAY_PACE_MS_PER_CHUNK = 16;

  function pace(bytes) {
    if (!RELAY_OPS.has(bytes[0])) return Promise.resolve();

    const chunks = Math.ceil(bytes.length / RELAY_CHUNK_BYTES);

    return new Promise(resolve => setTimeout(resolve, chunks * RELAY_PACE_MS_PER_CHUNK));
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
    sceneEl.hidden = true;
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
    sceneEl.hidden = true;
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

initHostPanel();
