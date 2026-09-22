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
};

const REPLY_BIT = 0x80;
const REPLY_PONG = OP.PING | REPLY_BIT;
const REPLY_STATE = OP.GET | REPLY_BIT;
const REPLY_SCENE_INFO = OP.SCENE_GET_INFO | REPLY_BIT;
const REPLY_SCENE_LAYER = OP.SCENE_GET_LAYER | REPLY_BIT;

const STATUS_OK = 0;
const STATUS_POOL_FULL = 2;

/* How many channels and how many layers per channel to probe for on
 * connect. Larger than any real board is expected to declare -- discovery
 * stops at the first channel that answers "out of range" (see
 * discoverChannels()), so this is just an upper bound on how long that
 * search can run, not a number this page needs to get exactly right.
 */
const MAX_CHANNEL_PROBE = 8;
const MAX_LAYER_PROBE = 16;

/* The board's defaults from zmk-raw-hid's Kconfig. A board that changed
 * RAW_HID_USAGE_PAGE / RAW_HID_USAGE needs the matching filter here, which
 * is why both are broken out rather than inlined into requestDevice().
 */
const USAGE_PAGE = 0xff60;
const USAGE = 0x61;

/* VFX_AXIS_* from dt-bindings/zmk/vfx.h, in numeric order -- the only place
 * a runtime scene can reach an axis is dart's flags byte (see below).
 */
const AXES = ['strip', 'x', 'y', 'radial', 'angle', 'spiral'];

/* Every generator vfx_runtime_add_layer() can build, in the same order
 * runtime_scene.h's enum vfx_rt_type uses -- index into this array is the
 * wire's `type` byte. `args` names line up with the arg indices
 * rebuild_slot() in runtime_scene.c reads. `stack` (pulse) and `reverse` /
 * `axis` (dart) are the only generators with anything in the flags byte;
 * everything else ignores it.
 */
const RT_TYPES = [
  { name: 'solid', args: [] },
  { name: 'breathe', args: ['period ms', 'min level', 'hue swing'] },
  { name: 'wave', args: ['wavelength', 'period ms', 'depth'] },
  { name: 'twinkle', args: ['period ms', 'density', 'hue spread'] },
  { name: 'plasma', args: ['scale', 'period ms', 'hue spread'] },
  { name: 'ripple', args: ['decay ms', 'speed', 'width'] },
  { name: 'keyflash', args: ['decay ms', 'spread'] },
  { name: 'pulse', args: ['decay ms', 'min level', 'hue step'], stack: true },
  { name: 'dart', args: ['speed', 'lifetime ms', 'tail'], axis: true, reverse: true },
  { name: 'static', args: ['period ms', 'density', 'hue spread'] },
];

const RT_FLAG_STACK = 0x01;
const RT_FLAG_REVERSE = 0x02;
const RT_FLAG_AXIS_SHIFT = 2;

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
  sceneAddLayer: (ch, type, zoneStart, zoneLen, blend, opacity, hue, sat, bri, args, flags) => {
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
    b[19] = flags;

    return b;
  },
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

  /* One card per layer, addressed by the device's own slot id rather than
   * by position: SCENE_MOVE_LAYER changes render order without changing
   * which slot a layer lives in, so a slot's card can stay the same DOM
   * node across a move and just get re-inserted at its new position.
   */
  function layerCard(slot) {
    if (layerRows.has(slot)) return layerRows.get(slot);

    const card = document.createElement('div');

    card.className = 'layer-card';
    card.dataset.slot = slot;

    const head = document.createElement('div');

    head.className = 'layer-head';
    head.innerHTML = '<strong></strong>';
    card.appendChild(head);

    const button = (label, title, fn) => {
      const b = document.createElement('button');

      b.textContent = label;
      b.title = title;
      b.className = 'tiny';
      b.addEventListener('click', fn);
      head.appendChild(b);

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

    const grid = document.createElement('div');

    grid.className = 'layer-grid';
    card.appendChild(grid);

    const field = (label, el) => {
      const wrap = document.createElement('label');

      wrap.className = 'field';
      wrap.textContent = label;
      wrap.appendChild(el);
      grid.appendChild(wrap);

      return wrap;
    };

    const number = onChange => {
      const el = document.createElement('input');

      el.type = 'number';
      el.addEventListener('change', () => onChange(Number(el.value)));

      return el;
    };

    /* zone start/len, blend, opacity and the flags byte have no dedicated
     * wire op: editing any of them re-sends the whole layer with
     * SCENE_ADD_LAYER, reusing the current values for everything else. That
     * replaces the slot's config rather than editing it in place, same as
     * devicetree would if you changed a layer's zone -- there is no partial
     * update for "where do my pixels come from".
     */
    const edit = (field, value) => {
      const l = layerRows.get(slot);

      if (!l) return;

      l.data[field] = value;
      rebuildFromScratch(slot, l.data);
    };

    const zoneStartWrap = field('zone start', number(v => edit('zoneStart', v)));
    const zoneLenWrap = field('zone len', number(v => edit('zoneLen', v)));
    const color = document.createElement('input');

    color.type = 'color';
    color.addEventListener('change', () => {
      const [h, s, v] = hexToHsb(color.value);

      act(requests.sceneSetColor(activeChannel, slot, h, s, v));
    });
    field('colour', color);

    const argWraps = [0, 1, 2, 3].map(i => field(`arg ${i}`, number(v => {
      const l = layerRows.get(slot);

      if (!l) return;

      l.data.args[i] = v;
      act(requests.sceneSetArg(activeChannel, slot, i, v));
    })));

    const stackEl = document.createElement('input');

    stackEl.type = 'checkbox';
    stackEl.addEventListener('change', () => edit('stack', stackEl.checked));
    const stackWrap = field('stack (each hit adds a new pulse)', stackEl);

    const reverseEl = document.createElement('input');

    reverseEl.type = 'checkbox';
    reverseEl.addEventListener('change', () => edit('reverse', reverseEl.checked));
    const reverseWrap = field('reverse', reverseEl);

    const axisEl = document.createElement('select');

    AXES.forEach((name, i) => {
      const opt = document.createElement('option');

      opt.value = i;
      opt.textContent = name;
      axisEl.appendChild(opt);
    });
    axisEl.addEventListener('change', () => edit('axis', Number(axisEl.value)));
    const axisWrap = field('axis', axisEl);

    sceneLayersEl.appendChild(card);

    const row = {
      card, head, zoneStartWrap, zoneLenWrap, color, argWraps,
      stackWrap, stackEl, reverseWrap, reverseEl, axisWrap, axisEl,
      data: { args: [0, 0, 0, 0] },
    };

    layerRows.set(slot, row);

    return row;
  }

  function rebuildFromScratch(slot, data) {
    const flags = flagsFor(data.type, data);

    act(requests.sceneRemoveLayer(activeChannel, slot));
    act(requests.sceneAddLayer(activeChannel, data.type, data.zoneStart, data.zoneLen,
                               data.blend, data.opacity, data.hue, data.sat, data.bri,
                               data.args, flags));

    /* The remove/re-add above frees this slot id and may hand out a
     * different one for the replacement; simplest to just ask the device
     * what the channel looks like now rather than guess.
     */
    layerRows.delete(slot);
    const card = sceneLayersEl.querySelector(`[data-slot="${slot}"]`);

    if (card) card.remove();

    refreshChannel(activeChannel);
  }

  function applyLayer(msg) {
    if (msg.status !== STATUS_OK) return;

    const type = RT_TYPES[msg.type] ?? RT_TYPES[0];
    const row = layerCard(msg.slot);
    const stack = (msg.flags & RT_FLAG_STACK) !== 0;
    const reverse = (msg.flags & RT_FLAG_REVERSE) !== 0;
    const axis = (msg.flags >> RT_FLAG_AXIS_SHIFT) & 0x7;

    row.card.dataset.slot = msg.slot;
    row.head.querySelector('strong').textContent = type.name;
    row.data = {
      type: msg.type,
      zoneStart: msg.zoneStart,
      zoneLen: msg.zoneLen,
      blend: msg.blend,
      opacity: msg.opacity,
      hue: msg.hue,
      sat: msg.sat,
      bri: msg.bri,
      args: msg.args.slice(),
      stack,
      reverse,
      axis,
    };

    row.zoneStartWrap.querySelector('input').value = msg.zoneStart;
    row.zoneLenWrap.querySelector('input').value = msg.zoneLen;
    row.color.value = hsbToHex(msg.hue < 0 ? msg.hue + 360 : msg.hue, msg.sat, msg.bri);

    /* Inline style rather than the hidden attribute: .field sets
     * display:flex, which in the cascade outranks the UA stylesheet's
     * [hidden] rule (see app.js's renderChannelTabs() for the same note).
     */
    type.args.forEach((label, i) => {
      row.argWraps[i].style.display = '';
      row.argWraps[i].firstChild.textContent = label;
      row.argWraps[i].querySelector('input').value = msg.args[i];
    });
    for (let i = type.args.length; i < 4; i++) row.argWraps[i].style.display = 'none';

    row.stackWrap.style.display = type.stack ? '' : 'none';
    row.stackEl.checked = stack;
    row.reverseWrap.style.display = type.reverse ? '' : 'none';
    row.reverseEl.checked = reverse;
    row.axisWrap.style.display = type.axis ? '' : 'none';
    row.axisEl.value = axis;

    sceneLayersEl.appendChild(row.card); // re-insert at the end, in probed order
  }

  /* Re-reads everything about a channel from the device: its own info, then
   * every slot 0..MAX_LAYER_PROBE-1, keeping whichever answer OK. There is
   * no op that lists which slots are in use or their render order, so this
   * is what both a fresh channel select and a rebuild after an edit fall
   * back to -- see the README's note on this being a v1 limitation.
   */
  async function refreshChannel(ch) {
    const info = await transact(requests.sceneGetInfo(ch));

    if (ch !== activeChannel) return; // superseded by a later selection

    sceneActiveEl.checked = info.active;

    const seen = new Set();

    for (let slot = 0; slot < MAX_LAYER_PROBE && seen.size < info.count; slot++) {
      const layer = await transact(requests.sceneGetLayer(ch, slot));

      if (ch !== activeChannel) return;

      if (layer.status === STATUS_OK) {
        applyLayer(layer);
        seen.add(slot);
      }
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

    const type = Number(addTypeEl.value);

    /* Enough of a starting point to light up and to be visible in the
     * layer list, same reasoning the main composer's own add-layer default
     * uses. Flags (stack/reverse/axis) start off and are edited on the
     * layer's own card afterwards, once it exists.
     */
    await act(requests.sceneAddLayer(activeChannel, type, 0, 6, 0, 255, 200, 90, 100,
                                     [1000, 0, 0, 0], 0));

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
    return transact(bytes).then(msg => {
      if (msg.kind === 'ack' && msg.status !== STATUS_OK) {
        const label = msg.status === STATUS_POOL_FULL ? 'channel is full' : 'refused';

        setStatus(`op 0x${msg.op.toString(16)} ${label} (byte1 ${msg.slot})`, false);
      }

      return msg;
    });
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
