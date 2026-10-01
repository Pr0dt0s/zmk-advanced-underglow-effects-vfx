/* Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * The runtime scene's layer table and the conversion between it and the
 * composer's scene JSON. No DOM and no wasm: host.js (the WebHID panel) and the
 * Node tests both import it, which is the only reason it is not part of host.js.
 *
 * If include/zmk/vfx/runtime_scene.h changes (a type, an argument order, a
 * limit), RT_TYPES and LIMITS here change by hand to match.
 */

/* The RAM-bound limits of a default build (CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS,
 * _MAX_COLORS, _MAX_ZONE_PIXELS, _HEAVY_STATES). A board may be built with
 * more; these are what a conversion warns against, since it cannot ask.
 */
export const LIMITS = { layers: 6, colors: 8, zonePixels: 32, heavy: 2 };

export const RT_MAX_ARGS = 6;
export const BLENDS = ['normal', 'add', 'multiply', 'screen', 'max'];
export const OPACITY_SOURCES = ['none', 'wpm', 'battery', 'activity'];

/* VFX_AXIS_* from dt-bindings/zmk/vfx.h, in numeric order. Carried in bits
 * 2-4 of a layer's flags byte, for every generator that has an axis.
 */
export const AXES = ['strip', 'x', 'y', 'radial', 'angle', 'spiral'];

export const c = (h, s, v) => ({ h, s, v });

/* The colour every layer started as before any type had colours of its own. */
export const GENERIC = c(200, 90, 100);

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

export const RT_TYPES = [
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

export const argLabel = a => (typeof a === 'string' ? a : a.label);

export const RT_FLAG_STACK = 0x01;
export const RT_FLAG_REVERSE = 0x02;
export const RT_FLAG_AXIS_SHIFT = 2;

export const flagsFor = (typeIdx, { stack, reverse, axis }) => {
  const spec = RT_TYPES[typeIdx];
  let f = 0;

  if (spec.stack && stack) f |= RT_FLAG_STACK;
  if (spec.reverse && reverse) f |= RT_FLAG_REVERSE;
  if (spec.axis) f |= (axis & 0x7) << RT_FLAG_AXIS_SHIFT;

  return f;
};

export function blankLayer() {
  return {
    type: 0, zoneKind: 0, zoneStart: 0, zoneLen: 0, zoneItems: [],
    blend: 0, opacity: 255, opacitySrc: 0, opacityMin: 0, opacityFull: 0, tuneId: 0,
    hue: 0, sat: 0, bri: 0, args: new Array(RT_MAX_ARGS).fill(0),
    stack: false, reverse: false, axis: 0, colors: [],
  };
}

/* What Add layer builds: the type's own defaults, colours included. */
export function newLayerData(type) {
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

/* --------------------------------------------------- composer <-> runtime */

/* Where each composer field lands in a runtime layer. The composer names its
 * type with hyphens and its numbers by field, the runtime by position, so this
 * is the one table that has to agree with both LAYER_SPECS in app.js and
 * rebuild_slot() in runtime_scene.c; tools/verify-bridge.mjs renders the two
 * against each other to notice when it does not.
 *
 *   args      [composer field, composer default] in the runtime's argument order
 *   primary   the composer colour that is the layer's own colour
 *   list      composer colours that fill the runtime's colour list, in order
 *   stops     a composer array that IS the colour list (gradient stops,
 *             layer-state colours, where null is black)
 *   axis      the composer's `axis` name (and its default) rides in the flags
 */
const MAP = {
  solid: { primary: 'color' },
  breathe: { primary: 'color', args: [['period_ms', 4000], ['min_level', 0], ['hue_swing', 0]] },
  wave: { primary: 'color', axis: 'strip',
          args: [['wavelength', 0], ['period_ms', 2000], ['depth', 255]] },
  twinkle: { primary: 'color', args: [['period_ms', 1200], ['density', 40], ['hue_spread', 0]] },
  plasma: { primary: 'color', args: [['scale', 0], ['period_ms', 6000], ['hue_spread', 40]] },
  ripple: { primary: 'color', args: [['decay_ms', 600], ['speed', 50], ['width', 10]] },
  keyflash: { primary: 'color', args: [['decay_ms', 400], ['spread', 12]] },
  pulse: { primary: 'color', stack: true,
           args: [['decay_ms', 500], ['min_level', 0], ['hue_step', 0]] },
  dart: { primary: 'color', list: ['head_color'], axis: 'x', reverse: true,
          args: [['speed', 90], ['lifetime_ms', 900], ['tail', 25]] },
  static: { primary: 'color', args: [['period_ms', 90], ['density', 60], ['hue_spread', 0]] },
  gradient: { stops: 'stops', axis: 'strip', args: [['scroll_speed', 0], ['span', 0]] },
  trail: { primary: 'color', args: [['decay_ms', 1500], ['spread', 12]] },
  hold: { primary: 'color', args: [['release_ms', 220]] },
  water: { primary: 'color', list: ['crest_color'],
           args: [['wavelength', 20], ['speed', 45], ['lifetime_ms', 2500], ['drop_rate_ms', 0],
                  ['amplitude', 200], ['damping', 7]] },
  matrix: { primary: 'color', list: ['head_color'],
            args: [['speed', 60], ['tail', 40], ['drop_rate_ms', 0], ['columns', 6],
                   ['jitter', 60], ['head_size', 8]] },
  fire: { primary: 'base_color', list: ['tip_color'], axis: 'y',
          args: [['period_ms', 500], ['cell', 12], ['height', 200], ['flicker', 180]] },
  comet: { primary: 'color', list: ['head_color'], axis: 'strip',
           args: [['period_ms', 3000], ['tail', 60], ['count', 1]] },
  cross: { primary: 'color', list: ['centre_color'],
           args: [['decay_ms', 500], ['radius', 0], ['thickness', 4]] },
  'layer-state': { stops: 'colors' },
  battery: { primary: 'low_color', list: ['high_color', 'empty_color'],
             args: [['warn_below', 20]] },
  'ble-profile': { primary: 'connected_color', list: ['disconnected_color', 'usb_color'] },
  flag: { primary: 'color' },
  wpm: { primary: 'idle_color', list: ['fast_color'], args: [['full', 80]] },
  'peripheral-battery': { primary: 'low_color',
                          list: ['high_color', 'empty_color', 'unknown_color'],
                          args: [['source', 0], ['warn_below', 20]] },
};

const CROSS_AXES = ['both', 'horizontal', 'vertical'];
const LOCK_BITS = { num: 0x01, caps: 0x02, scroll: 0x04, compose: 0x08, kana: 0x10 };
const MOD_BITS = { ctrl: 0x11, shift: 0x22, alt: 0x44, gui: 0x88 };

const composerName = name => name.replace(/_/g, '-');
const typeIndex = composerName_ => RT_TYPES.findIndex(t => composerName(t.name) === composerName_);
const BLACK = { h: 0, s: 0, v: 0 };
const toColor = a => (Array.isArray(a) ? { h: a[0], s: a[1], v: a[2] } : null);
const fromColor = k => [k.h, k.s, k.v];

/* A composer scene -> the layers a board holds, bottom of the stack first.
 * `warnings` lists everything the board cannot hold as written (more layers
 * than its pool, a longer pixel list than its zone buffer, ...), each of which
 * is dropped or cut rather than sent half-formed.
 */
export function composerToRuntime(scene, limits = LIMITS) {
  const warnings = [];
  const layers = [];
  let heavy = 0;

  for (const [i, l] of (scene.layers ?? []).entries()) {
    const map = MAP[l.type];
    const t = typeIndex(l.type);
    const label = `layer ${i + 1} (${l.type})`;

    if (!map || t < 0) {
      warnings.push(`${label}: the board has no such generator, skipped`);
      continue;
    }

    if (layers.length >= limits.layers) {
      warnings.push(`${label}: the board holds at most ${limits.layers} layers, skipped`);
      continue;
    }

    if ((l.type === 'trail' || l.type === 'hold') && heavy >= limits.heavy) {
      warnings.push(`${label}: the board has state for only ${limits.heavy} trail/hold layers, skipped`);
      continue;
    }

    const zone = scene.zones?.[l.zone];

    if (!zone) {
      warnings.push(`${label}: zone "${l.zone}" is not defined, skipped`);
      continue;
    }

    const d = blankLayer();

    d.type = t;
    d.blend = BLENDS.indexOf(l.blend ?? 'normal');
    if (d.blend < 0) {
      warnings.push(`${label}: unknown blend "${l.blend}", using normal`);
      d.blend = 0;
    }
    d.opacity = l.opacity ?? 255;

    if (l.opacity_source) {
      d.opacitySrc = Math.max(0, OPACITY_SOURCES.indexOf(l.opacity_source));
      d.opacityMin = l.opacity_min ?? 0;
      d.opacityFull = l.opacity_full ?? 0;
    }
    d.tuneId = l.tune_id ?? 0;

    if (Array.isArray(zone.range)) {
      if (zone.range[0] > 255) warnings.push(`${label}: zone starts past pixel 255, clipped`);
      d.zoneKind = 0;
      d.zoneStart = Math.min(zone.range[0], 255);
      d.zoneLen = Math.min(zone.range[1], 255);
    } else {
      const items = zone.pixels ?? zone.keys ?? [];

      d.zoneKind = zone.pixels ? 1 : 2;
      d.zoneItems = items.filter(v => v >= 0 && v <= 255).slice(0, limits.zonePixels);
      if (d.zoneItems.length < items.length) {
        warnings.push(`${label}: its zone holds ${items.length} entries, the board keeps ` +
                      `${d.zoneItems.length}`);
      }
    }

    const need = (k) => {
      const v = toColor(l[k]);

      if (!v) warnings.push(`${label}: "${k}" is missing, using black`);

      return v ?? { ...BLACK };
    };
    const optional = (k) => toColor(l[k]) ?? { ...BLACK };

    if (map.primary) {
      const p = need(map.primary);

      d.hue = p.h; d.sat = p.s; d.bri = p.v;
    }
    (map.list ?? []).forEach(k => d.colors.push(optional(k)));
    if (map.stops) {
      d.colors = (l[map.stops] ?? []).map(a => toColor(a) ?? { ...BLACK });
      if (d.colors.length > limits.colors) {
        warnings.push(`${label}: ${d.colors.length} colours, the board keeps ${limits.colors}`);
        d.colors.length = limits.colors;
      }
    }
    // A fixed list whose trailing entries are all unset need not be sent.
    if (!map.stops) {
      while (d.colors.length && d.colors.at(-1).v === 0 && d.colors.at(-1).s === 0) {
        d.colors.pop();
      }
    }

    (map.args ?? []).forEach(([name, dflt], k) => { d.args[k] = l[name] ?? dflt; });

    if (l.type === 'cross') d.args[3] = Math.max(0, CROSS_AXES.indexOf(l.axes ?? 'both'));
    if (l.type === 'flag') {
      const mods = l.source === 'modifiers';

      d.args[0] = mods ? 1 : 0;
      d.args[1] = (mods ? MOD_BITS : LOCK_BITS)[l.bit] ?? 0x02;
    }
    if (l.type === 'wpm') d.args[1] = l.bar ? 1 : 0;

    if (map.axis) d.axis = Math.max(0, AXES.indexOf(l.axis ?? map.axis));
    if (map.stack) d.stack = !!l.stack;
    if (map.reverse) d.reverse = !!l.reverse;

    if (l.type === 'trail' || l.type === 'hold') heavy++;

    layers.push(d);
  }

  return { layers, warnings };
}

/* The other way: the board's layers as a composer scene. Each layer gets a
 * zone of its own, shared only when two are identical, since a board keeps its
 * zone per layer rather than by name.
 */
export function runtimeToComposer(layers, name = 'board') {
  const zones = {};
  const zoneNames = new Map();
  const out = [];

  for (const d of layers) {
    const spec = RT_TYPES[d.type];

    if (!spec) continue;

    const cname = composerName(spec.name);
    const map = MAP[cname];
    const zone = d.zoneKind === 0 ? { range: [d.zoneStart, d.zoneLen] }
      : d.zoneKind === 1 ? { pixels: [...d.zoneItems] } : { keys: [...d.zoneItems] };
    const key = JSON.stringify(zone);

    if (!zoneNames.has(key)) {
      zoneNames.set(key, `z${zoneNames.size}`);
      zones[zoneNames.get(key)] = zone;
    }

    const l = { type: cname, zone: zoneNames.get(key) };

    if (d.blend) l.blend = BLENDS[d.blend] ?? 'normal';
    if (d.opacity !== 255) l.opacity = d.opacity;
    if (d.opacitySrc) {
      l.opacity_source = OPACITY_SOURCES[d.opacitySrc];
      l.opacity_min = d.opacityMin;
      l.opacity_full = d.opacityFull;
    }
    if (d.tuneId) l.tune_id = d.tuneId;

    if (map.primary) l[map.primary] = fromColor({ h: d.hue, s: d.sat, v: d.bri });
    (map.list ?? []).forEach((k, n) => {
      const v = d.colors[n];

      if (v && (v.s || v.v)) l[k] = fromColor(v);
    });
    if (map.stops) {
      l[map.stops] = d.colors.map(v => (cname === 'layer-state' && !v.v ? null : fromColor(v)));
    }

    (map.args ?? []).forEach(([field], k) => { l[field] = d.args[k]; });

    if (cname === 'cross') l.axes = CROSS_AXES[d.args[3]] ?? 'both';
    if (cname === 'flag') {
      const mods = d.args[0] === 1;
      const bits = mods ? MOD_BITS : LOCK_BITS;

      l.source = mods ? 'modifiers' : 'locks';
      l.bit = Object.keys(bits).find(k => bits[k] === d.args[1]) ?? 'caps';
    }
    if (cname === 'wpm') l.bar = d.args[1] === 1;

    if (map.axis) l.axis = AXES[d.axis] ?? map.axis;
    if (map.stack) l.stack = d.stack;
    if (map.reverse) l.reverse = d.reverse;

    out.push(l);
  }

  return { name, zones, layers: out };
}

/* Whether two lists of layers are the same scene as far as a board is
 * concerned: only what the type reads is compared, since a generator with
 * three numbers keeps zeros in the other three and a primary colour it ignores.
 */
const significant = d => {
  const spec = RT_TYPES[d.type];

  return {
    type: spec?.name,
    zone: d.zoneKind === 0 ? [0, d.zoneStart, d.zoneLen] : [d.zoneKind, ...d.zoneItems],
    blend: d.blend,
    opacity: d.opacity,
    src: [d.opacitySrc, d.opacitySrc ? d.opacityMin : 0, d.opacitySrc ? d.opacityFull : 0],
    tune: d.tuneId,
    color: spec?.primary ? [d.hue, d.sat, d.bri] : null,
    list: d.colors.map(v => [v.h, v.s, v.v]),
    args: d.args.slice(0, spec?.args.length ?? 0),
    stack: spec?.stack ? d.stack : false,
    reverse: spec?.reverse ? d.reverse : false,
    axis: spec?.axis ? d.axis : 0,
  };
};

export function sameLayers(a, b) {
  return JSON.stringify(a.map(significant)) === JSON.stringify(b.map(significant));
}

/* ------------------------------------------------------------- save files */

export const SAVE_FORMAT = 'vfx-runtime-scene';

/* Layers are written by type name, never by index, so a type renumbered in a
 * later firmware cannot silently turn an old file into a different effect.
 */
export function saveFile(layers, { channel = 0, sceneIndex = 0, active = false } = {}) {
  return {
    format: SAVE_FORMAT,
    version: 1,
    channel,
    scene_index: sceneIndex,
    active,
    scene: runtimeToComposer(layers, 'saved'),
  };
}

export function parseSaveFile(text) {
  let doc;

  try {
    doc = typeof text === 'string' ? JSON.parse(text) : text;
  } catch (err) {
    throw new Error(`not JSON: ${err.message}`);
  }

  // A bare composer scene is accepted too, so what the composer exports loads.
  if (doc && doc.format === undefined && Array.isArray(doc.layers)) {
    return { ...composerToRuntime(doc), channel: null, sceneIndex: null, active: false };
  }

  if (!doc || doc.format !== SAVE_FORMAT) throw new Error('not a runtime-scene file');
  if (doc.version !== 1) throw new Error(`unsupported file version ${doc.version}`);

  return {
    ...composerToRuntime(doc.scene),
    channel: doc.channel ?? null,
    sceneIndex: doc.scene_index ?? null,
    active: !!doc.active,
  };
}
