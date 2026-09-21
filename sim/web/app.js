/* Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives two instances of the firmware compositor, one per half of the split,
 * and paints their output under a Lily58. The key geometry is the real one,
 * lifted from ZMK's zmk,physical-layout for this board.
 */

const BLEND = { normal: 0, add: 1, multiply: 2, screen: 3, max: 4 };

/* Which way an effect runs across the board. Named rather than numbered in
 * the scene JSON, and turned back into VFX_AXIS_* on the way out.
 */
const AXIS = { strip: 0, x: 1, y: 2, radial: 3, angle: 4, spiral: 5 };
const CROSS = { both: 0, horizontal: 1, vertical: 2 };

/* Downward-facing pixels per half on a mixed board, chained ahead of the
 * per-key ones. Six is what the PandaKB Lily58 RGB MX carries.
 */
const MIXED_GLOW = 6;

/* What a layer's opacity can be made to follow, as VFX_SRC_* names them. */
const SOURCES = { none: 0, wpm: 1, battery: 2, activity: 3 };
const SOURCE_NAME = ['VFX_SRC_NONE', 'VFX_SRC_WPM', 'VFX_SRC_BATTERY', 'VFX_SRC_ACTIVITY'];

/* Host lock LEDs and modifier bits, as <dt-bindings/zmk/vfx.h> names them. */
const LOCKS = { num: 0x01, caps: 0x02, scroll: 0x04, compose: 0x08, kana: 0x10 };
const MODS = { ctrl: 0x11, shift: 0x22, alt: 0x44, gui: 0x88 };
const LOCK_NAME = { 0x01: 'VFX_LOCK_NUM', 0x02: 'VFX_LOCK_CAPS', 0x04: 'VFX_LOCK_SCROLL',
                    0x08: 'VFX_LOCK_COMPOSE', 0x10: 'VFX_LOCK_KANA' };
const MOD_NAME = { 0x11: 'VFX_MOD_CTRL', 0x22: 'VFX_MOD_SHIFT',
                   0x44: 'VFX_MOD_ALT', 0x88: 'VFX_MOD_GUI' };
const LOCK_CAPS = LOCKS.caps;
const MOD_SHIFT_BITS = MODS.shift;
const CROSS_NAME = ['VFX_CROSS_BOTH', 'VFX_CROSS_HORIZONTAL', 'VFX_CROSS_VERTICAL'];
const AXIS_NAME = ['VFX_AXIS_STRIP', 'VFX_AXIS_X', 'VFX_AXIS_Y',
                   'VFX_AXIS_RADIAL', 'VFX_AXIS_ANGLE', 'VFX_AXIS_SPIRAL'];
const BLEND_NAME = ['VFX_BLEND_NORMAL', 'VFX_BLEND_ADD', 'VFX_BLEND_MULTIPLY',
                    'VFX_BLEND_SCREEN', 'VFX_BLEND_MAX'];


/* One description of every generator, used for loading a scene into the
 * engine and for emitting the devicetree. Keeping them in one table is what
 * stops the exported devicetree drifting from what the preview rendered.
 *
 * `args` are the numeric config fields in the order vfx_sim_add_layer takes
 * them, each with the same default the devicetree binding declares.
 */
const LAYER_SPECS = {
  solid:      { id: 0,  args: [] },
  gradient:   { id: 1,  args: [] },
  breathe:    { id: 2,  args: [['period_ms', 4000], ['min_level', 0], [null, 0],
                              ['hue_swing', 0]] },
  wave:       { id: 3,  args: [['wavelength', 0], ['period_ms', 2000], ['depth', 255],
                              ['axis', 0]] },
  twinkle:    { id: 4,  args: [['period_ms', 1200], ['density', 40], [null, 0],
                              ['hue_spread', 0]] },
  plasma:     { id: 5,  args: [['scale', 0], ['period_ms', 6000], ['hue_spread', 40]] },
  ripple:     { id: 6,  args: [['decay_ms', 600], ['speed', 50], ['width', 10]] },
  keyflash:   { id: 7,  args: [['decay_ms', 400], ['spread', 12]] },
  trail:      { id: 8,  args: [['decay_ms', 1500], ['spread', 12]] },
  water:      { id: 13, args: [['wavelength', 20], ['speed', 45], ['lifetime_ms', 2500],
                              ['drop_rate_ms', 0], ['amplitude', 200], ['damping', 7]] },
  fire:       { id: 16, args: [['period_ms', 500], ['cell', 12], ['height', 200]] },
  comet:      { id: 17, args: [['period_ms', 3000], ['tail', 60], ['count', 1]] },
  flag:       { id: 18, args: [] },
  wpm:        { id: 19, args: [['full', 80]] },
  'peripheral-battery': { id: 20, args: [['warn_below', 20]] },
  cross:      { id: 15, args: [['decay_ms', 500], ['radius', 0], ['thickness', 4]] },
  matrix:     { id: 14, args: [['speed', 60], ['tail', 40], ['drop_rate_ms', 0],
                              ['columns', 6], ['jitter', 60], ['head_size', 8]] },
  'layer-state': { id: 9,  args: [] },
  battery:       { id: 10, args: [['warn_below', 20]] },
  'ble-profile': { id: 11, args: [] },
  /* These have add_* entry points of their own rather than going through
   * vfx_sim_add_layer, so the id is never used; the args are here because the
   * devicetree export reads them from the same table.
   */
  pulse:      { id: -1, args: [['decay_ms', 500], ['min_level', 0], ['hue_step', 0]] },
  hold:       { id: -1, args: [['release_ms', 220]] },
  dart:       { id: -1, args: [['speed', 90], ['lifetime_ms', 900], ['tail', 25]] },
  static:     { id: -1, args: [['period_ms', 90], ['density', 60], ['hue_spread', 0]] },
};

const packHsb = (h, s, b) => (((h & 0x1ff) << 16) | ((s & 0xff) << 8) | (b & 0xff)) >>> 0;

/* ------------------------------------------------------------------ engine */

class Half {
  constructor(instance) {
    this.e = instance.exports;
  }

  get mem() { return new Uint8Array(this.e.memory.buffer); }
  get view() { return new DataView(this.e.memory.buffer); }

  init(count, virtualLength, offset) {
    this.count = count;
    this.e.vfx_sim_init(count, virtualLength, offset);
  }

  setState(brightness, speed, hue) { this.e.vfx_sim_set_state(brightness, speed, hue); }

  /* Scenes are built through the same kind of fixed-arena calls the firmware
   * makes from devicetree, so nothing here needs an allocator on either side.
   */
  loadScene(scene) {
    this.e.vfx_sim_reset_scene();

    const zoneIds = {};
    for (const [name, z] of Object.entries(scene.zones || {})) {
      if (Array.isArray(z.keys)) {
        const scratch = this.e.vfx_sim_scratch();
        const mem = this.mem;
        z.keys.forEach((p, i) => { mem[scratch + i] = p & 0xff; });
        zoneIds[name] = this.e.vfx_sim_add_zone_keys(z.keys.length);
      } else if (Array.isArray(z.pixels)) {
        const scratch = this.e.vfx_sim_scratch();
        const mem = this.mem;
        z.pixels.forEach((p, i) => { mem[scratch + i] = p & 0xff; });
        zoneIds[name] = this.e.vfx_sim_add_zone_pixels(z.pixels.length);
      } else if (Array.isArray(z.range)) {
        zoneIds[name] = this.e.vfx_sim_add_zone_range(z.range[0], z.range[1]);
      } else {
        throw new Error(`zone "${name}" needs a range, a pixels list or a keys list`);
      }
      if (zoneIds[name] < 0) throw new Error(`zone "${name}" was rejected (too many zones?)`);
    }

    for (const [i, l] of (scene.layers || []).entries()) {
      const zid = zoneIds[l.zone];
      if (zid === undefined) throw new Error(`layer ${i} references unknown zone "${l.zone}"`);

      const spec = LAYER_SPECS[l.type];
      if (!spec) throw new Error(`layer ${i} has unknown type "${l.type}"`);

      const blend = BLEND[l.blend ?? 'normal'];
      if (blend === undefined) throw new Error(`layer ${i} has unknown blend "${l.blend}"`);
      const opacity = l.opacity ?? 255;

      /* A null name is a gap in the generator's argument list: the config
       * field it would fill does not exist for this type.
       */
      const args = spec.args.map(([name, dflt]) => {
        if (name === null) return dflt;
        if (name === 'axis') return AXIS[l.axis ?? 'strip'] ?? 0;
        return l[name] ?? dflt;
      });
      while (args.length < 4) args.push(0);

      let rc;
      switch (l.type) {
        case 'solid':
          rc = this.e.vfx_sim_add_solid(zid, blend, opacity, packHsb(...l.color));
          break;

        case 'gradient': {
          const view = this.view;
          const scratch = this.e.vfx_sim_scratch();
          l.stops.forEach((c, k) => view.setUint32(scratch + k * 4, packHsb(...c), true));
          rc = this.e.vfx_sim_add_gradient(zid, blend, opacity,
                                           l.scroll_speed ?? 0, l.span ?? 0, l.stops.length,
                                           AXIS[l.axis ?? 'strip'] ?? 0);
          break;
        }

        case 'layer-state': {
          const view = this.view;
          const scratch = this.e.vfx_sim_scratch();
          l.colors.forEach((c, k) =>
            view.setUint32(scratch + k * 4, c === null ? 0 : packHsb(...c), true));
          rc = this.e.vfx_sim_add_layer_state(zid, blend, opacity, l.colors.length);
          break;
        }

        case 'water':
          rc = this.e.vfx_sim_add_water(zid, blend, opacity,
                                        packHsb(...l.color),
                                        l.crest_color ? packHsb(...l.crest_color) : 0,
                                        l.wavelength ?? 20, l.speed ?? 45,
                                        l.lifetime_ms ?? 2500, l.drop_rate_ms ?? 0,
                                        l.amplitude ?? 200, l.damping ?? 7);
          break;

        case 'flag':
          rc = this.e.vfx_sim_add_flag(zid, blend, opacity, packHsb(...l.color),
                                       l.source === 'modifiers' ? 1 : 0,
                                       (l.source === 'modifiers' ? MODS : LOCKS)[l.bit] ?? 0x02);
          break;

        case 'wpm':
          rc = this.e.vfx_sim_add_wpm(zid, blend, opacity,
                                      packHsb(...l.idle_color), packHsb(...l.fast_color),
                                      l.full ?? 80, l.bar ? 1 : 0);
          break;

        case 'peripheral-battery':
          rc = this.e.vfx_sim_add_peripheral_battery(zid, blend, opacity,
                                                     packHsb(...l.low_color),
                                                     packHsb(...l.high_color),
                                                     l.empty_color ? packHsb(...l.empty_color) : 0,
                                                     l.unknown_color
                                                       ? packHsb(...l.unknown_color) : 0,
                                                     l.source ?? 0, l.warn_below ?? 20);
          break;

        case 'fire':
          rc = this.e.vfx_sim_add_fire(zid, blend, opacity,
                                       packHsb(...l.base_color), packHsb(...l.tip_color),
                                       l.period_ms ?? 500, l.cell ?? 12, l.height ?? 200,
                                       l.flicker ?? 180, AXIS[l.axis ?? 'y'] ?? 2);
          break;

        case 'comet':
          rc = this.e.vfx_sim_add_comet(zid, blend, opacity,
                                        packHsb(...l.color),
                                        l.head_color ? packHsb(...l.head_color) : 0,
                                        l.period_ms ?? 3000, l.tail ?? 60, l.count ?? 1,
                                        AXIS[l.axis ?? 'strip'] ?? 0);
          break;

        case 'cross':
          rc = this.e.vfx_sim_add_cross(zid, blend, opacity,
                                        packHsb(...l.color),
                                        l.centre_color ? packHsb(...l.centre_color) : 0,
                                        l.decay_ms ?? 500, l.radius ?? 0, l.thickness ?? 4,
                                        CROSS[l.axes ?? 'both'] ?? 0);
          break;

        case 'matrix':
          rc = this.e.vfx_sim_add_matrix(zid, blend, opacity,
                                         packHsb(...l.color),
                                         l.head_color ? packHsb(...l.head_color) : 0,
                                         l.speed ?? 60, l.tail ?? 40, l.drop_rate_ms ?? 0,
                                         l.columns ?? 6, l.jitter ?? 60, l.head_size ?? 8);
          break;

        case 'battery':
          rc = this.e.vfx_sim_add_battery(zid, blend, opacity,
                                          packHsb(...l.low_color), packHsb(...l.high_color),
                                          l.empty_color ? packHsb(...l.empty_color) : 0,
                                          l.warn_below ?? 20);
          break;

        case 'ble-profile':
          rc = this.e.vfx_sim_add_ble_profile(zid, blend, opacity,
                                              packHsb(...l.connected_color),
                                              packHsb(...l.disconnected_color),
                                              l.usb_color ? packHsb(...l.usb_color) : 0);
          break;

        case 'pulse':
          rc = this.e.vfx_sim_add_pulse(zid, blend, opacity, packHsb(...l.color),
                                        l.decay_ms ?? 500, l.min_level ?? 0,
                                        l.hue_step ?? 0, l.stack ? 1 : 0);
          break;

        case 'hold':
          rc = this.e.vfx_sim_add_hold(zid, blend, opacity, packHsb(...l.color),
                                       l.release_ms ?? 220);
          break;

        case 'dart':
          rc = this.e.vfx_sim_add_dart(zid, blend, opacity,
                                       packHsb(...l.color),
                                       l.head_color ? packHsb(...l.head_color) : 0,
                                       l.speed ?? 90, l.lifetime_ms ?? 900, l.tail ?? 25,
                                       AXIS[l.axis ?? 'x'] ?? 1, l.reverse ? 1 : 0);
          break;

        case 'static':
          rc = this.e.vfx_sim_add_static(zid, blend, opacity, packHsb(...l.color),
                                         l.period_ms ?? 90, l.density ?? 60,
                                         l.hue_spread ?? 0);
          break;

        default:
          rc = this.e.vfx_sim_add_layer(spec.id, zid, blend, opacity, packHsb(...l.color),
                                        args[0], args[1], args[2], args[3]);
          break;
      }

      if (rc < 0) throw new Error(`layer ${i} was rejected by the engine`);

      /* Applied after the fact rather than threaded through every add_*,
       * because it is the same question for all of them.
       */
      if (l.opacity_source) {
        this.e.vfx_sim_set_layer_source(rc, SOURCES[l.opacity_source] ?? 0,
                                        l.opacity_min ?? 0, l.opacity_full ?? 0);
      }
    }
  }

  /* Whether anyone is at the keyboard. ZMK works this out from how long it has
   * been since a keypress, which the page cannot observe, so it is set here.
   */
  setActive(active) {
    this.e.vfx_sim_set_active(active ? 1 : 0);
  }

  setStatus(activeLayer, battery, profile, connected, usb) {
    this.e.vfx_sim_set_status(activeLayer, battery, profile, connected ? 1 : 0, usb ? 1 : 0);
  }

  /* State the keyboard learns from somewhere else: the host's lock LEDs, the
   * modifiers held, the typing estimate, the other half's cell.
   */
  setExtraStatus(locks, modifiers, wpm, peripheral) {
    this.e.vfx_sim_set_extra_status(locks, modifiers, wpm, peripheral);
  }

  /* Key position -> virtual pixel. Without it reactive effects still animate
   * but land in the wrong places, exactly as on a board with no key-pixels.
   */
  /* Real board coordinates for every pixel, scaled so adjacent LEDs sit about
   * 10 apart -- the convention the effect distance properties are tuned for,
   * and what <vfx/lily58-positions.dtsi> uses.
   */
  setPositions(points) {
    const scratch = this.e.vfx_sim_scratch();
    const view = this.view;
    points.forEach((p, i) => {
      view.setInt16(scratch + i * 4, Math.round(p.x), true);
      view.setInt16(scratch + i * 4 + 2, Math.round(p.y), true);
    });
    this.e.vfx_sim_set_positions(points.length);
  }

  setKeyMap(pixels) {
    const scratch = this.e.vfx_sim_scratch();
    const mem = this.mem;
    pixels.forEach((p, i) => { mem[scratch + i] = p & 0xff; });
    this.e.vfx_sim_set_key_map(pixels.length);
  }

  render(timeMs) {
    const ptr = this.e.vfx_sim_render(timeMs >>> 0);
    this.pixels = this.mem.subarray(ptr, ptr + this.count * 3);
    this.lit = this.e.vfx_sim_any_lit() === 1;
    this.animating = this.e.vfx_sim_is_animating() === 1;
    this.powerState = this.e.vfx_sim_power_state();   // 0 lit, 1 gated, 2 settling
    this.microamps = this.e.vfx_sim_estimated_ua();
    return this.pixels;
  }

  setPowerPolicy(blackoutMs, settleMs) {
    this.e.vfx_sim_set_power_policy(blackoutMs, settleMs);
  }

  key(position, pressed) { this.e.vfx_sim_key_event(position, pressed ? 1 : 0); }
}

/* ---------------------------------------------------------------- geometry */

const PAD = 28;
let keys = [], layout = null;

function buildLayout(canvas, ledsPerHalf, stripPath) {
  const maxX = Math.max(...keys.map(k => k.x + k.w));
  const maxY = Math.max(...keys.map(k => k.y + k.h));
  const scale = Math.min((canvas.width - PAD * 2) / maxX, (canvas.height - PAD * 2) / maxY);
  const offX = (canvas.width - maxX * scale) / 2;
  const offY = (canvas.height - maxY * scale) / 2;

  const rects = keys.map((k, i) => ({
    i,
    half: k.x < 800 ? 0 : 1,
    x: offX + k.x * scale,
    y: offY + k.y * scale,
    w: k.w * scale,
    h: k.h * scale,
  }));

  /* Where a pixel sits physically is a property of how the strip was wired,
   * not something the firmware knows: effects are positioned by strip index.
   * That distinction is real and visible, so the simulator lets you pick the
   * wiring rather than hiding it behind one assumed layout.
   */
  const leds = [];

  for (const half of [0, 1]) {
    const hk = rects.filter(r => r.half === half);
    const x0 = Math.min(...hk.map(r => r.x));
    const x1 = Math.max(...hk.map(r => r.x + r.w));
    const y0 = Math.min(...hk.map(r => r.y));
    const y1 = Math.max(...hk.map(r => r.y + r.h));

    const lerp = (a, b, t) => a + (b - a) * t;

    /* Boards that light the keys and the case from one chain, which is what
     * the underglow-first wiring on a PandaKB Lily58 looks like. The two
     * groups want different effects, and the simulator cannot show why until
     * it can show them being different things.
     */
    if (stripPath === 'mixed') {
      for (let i = 0; i < MIXED_GLOW; i++) {
        leds.push({
          half,
          kind: 'glow',
          x: lerp(x0, x1, [0.16, 0.5, 0.84][i % 3]),
          y: lerp(y0, y1, i < 3 ? 0.28 : 0.74),
        });
      }

      /* One per key, snaking so that adjacent indices are adjacent keys,
       * which is how these boards are actually routed.
       */
      const rows = [...new Set(hk.map(r => Math.round(r.y)))].sort((a, b) => a - b);

      rows.forEach((rowY, row) => {
        const inRow = hk.filter(r => Math.round(r.y) === rowY)
                        .sort((a, b) => (row % 2 ? b.x - a.x : a.x - b.x));

        for (const r of inRow) {
          leds.push({ half, kind: 'key', key: r.i, x: r.x + r.w / 2, y: r.y + r.h / 2 });
        }
      });

      continue;
    }

    if (stripPath === 'ring') {
      /* Walk the perimeter clockwise from the top left corner. */
      for (let i = 0; i < ledsPerHalf; i++) {
        const t = (i / ledsPerHalf) * 4;
        let x, y;
        if (t < 1)      { x = lerp(x0, x1, t);        y = y0; }
        else if (t < 2) { x = x1;                     y = lerp(y0, y1, t - 1); }
        else if (t < 3) { x = lerp(x1, x0, t - 2);    y = y1; }
        else            { x = x0;                     y = lerp(y1, y0, t - 3); }
        leds.push({ half, x, y });
      }
      continue;
    }

    const cols = Math.min(6, ledsPerHalf);
    const rows = Math.ceil(ledsPerHalf / cols);

    for (let i = 0; i < ledsPerHalf; i++) {
      const row = Math.floor(i / cols);
      let col = i % cols;

      if (stripPath === 'serpentine' && row % 2 === 1) {
        col = cols - 1 - col;
      }

      leds.push({
        half,
        x: lerp(x0, x1, cols === 1 ? 0.5 : col / (cols - 1)),
        y: lerp(y0, y1, rows === 1 ? 0.5 : row / (rows - 1)),
      });
    }
  }

  /* Map each key to its nearest LED, which is what key-pixels does in
   * devicetree. Computed from the geometry rather than guessed, so a ripple
   * starts under the key that was actually pressed.
   */
  const keyPixels = rects.map(r => {
    /* A board with per-key LEDs knows exactly which one belongs to a key, and
     * nearest-wins would sometimes answer with an underglow pixel instead —
     * a ripple would then start behind the board rather than under the finger.
     */
    if (stripPath === 'mixed') {
      const own = leds.findIndex(l => l.kind === 'key' && l.key === r.i);

      if (own >= 0) return own;
    }

    const cx = r.x + r.w / 2;
    const cy = r.y + r.h / 2;
    let best = 0;
    let bestD = Infinity;

    leds.forEach((led, i) => {
      const d = (led.x - cx) ** 2 + (led.y - cy) ** 2;
      if (d < bestD) { bestD = d; best = i; }
    });

    return best;
  });

  /* Canvas coordinates are in pixels of a 1400px-wide drawing; the engine
   * wants units where adjacent LEDs are ~10 apart. Scale by the actual
   * spacing between the first two LEDs so any wiring or count lands on that
   * convention.
   */
  /* Measured between two adjacent per-key LEDs when there are any. The first
   * two pixels of a mixed board are underglow sitting a third of the board
   * apart, and scaling to that would put every distance property out by a
   * factor of five.
   */
  const gauge = stripPath === 'mixed' ? MIXED_GLOW : 0;

  const step = leds.length > gauge + 1
    ? Math.hypot(leds[gauge + 1].x - leds[gauge].x, leds[gauge + 1].y - leds[gauge].y) || 1
    : 1;
  const k = 10 / step;
  const originX = Math.min(...leds.map(l => l.x));
  const originY = Math.min(...leds.map(l => l.y));
  const positions = leds.map(l => ({ x: (l.x - originX) * k, y: (l.y - originY) * k }));

  const spacing = (Math.max(...rects.map(r => r.w)) || 40);
  layout = { rects, leds, scale, spacing, keyPixels, positions };
  return layout;
}

/* ----------------------------------------------------------------- drawing */

function draw(ctx, halves) {
  const { rects, leds, spacing } = layout;

  ctx.fillStyle = '#0a0c10';
  ctx.fillRect(0, 0, ctx.canvas.width, ctx.canvas.height);

  /* Glow first, additively, so overlapping LEDs bloom the way they do on a
   * diffuser instead of the last one painted winning.
   */
  ctx.globalCompositeOperation = 'lighter';
  const radius = spacing * 1.7;

  for (const [n, led] of leds.entries()) {
    const half = halves[led.half];
    const idx = n - (led.half === 1 ? leds.length / 2 : 0);
    const px = half.pixels;
    if (!px) continue;

    /* A gated half has no power at the strip, so it must draw dark even
     * though the engine is still rendering frames behind the scenes.
     */
    if (half.powerState === 1) continue;

    const r = px[idx * 3], g = px[idx * 3 + 1], b = px[idx * 3 + 2];
    if ((r | g | b) === 0) continue;

    /* A per-key LED points up through its keycap, so it reads as a tight lit
     * square rather than a wash on the desk. Drawing both the same way is
     * what made the two groups indistinguishable.
     */
    const rad = led.kind === 'key' ? radius * 0.55 : radius;

    const grad = ctx.createRadialGradient(led.x, led.y, 0, led.x, led.y, rad);
    grad.addColorStop(0, `rgba(${r},${g},${b},0.95)`);
    grad.addColorStop(0.35, `rgba(${r},${g},${b},${led.kind === 'key' ? 0.72 : 0.38})`);
    grad.addColorStop(1, `rgba(${r},${g},${b},0)`);

    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(led.x, led.y, rad, 0, Math.PI * 2);
    ctx.fill();
  }

  /* Then the plate over the top, translucent, so the light reads as underglow. */
  ctx.globalCompositeOperation = 'source-over';

  for (const r of rects) {
    const rad = Math.min(6, r.w * 0.12);
    ctx.beginPath();
    ctx.roundRect(r.x + 1.5, r.y + 1.5, r.w - 3, r.h - 3, rad);
    ctx.fillStyle = 'rgba(16,19,26,0.62)';
    ctx.fill();
    ctx.strokeStyle = 'rgba(200,214,240,0.18)';
    ctx.lineWidth = 1;
    ctx.stroke();
  }

  /* A per-key LED shines through its own keycap, so it goes on after the
   * plate rather than under it. Underglow stays beneath, which is the whole
   * visible difference between the two on a board that has both.
   */
  ctx.globalCompositeOperation = 'lighter';

  for (const [n, led] of leds.entries()) {
    if (led.kind !== 'key' || led.key === undefined) continue;

    const half = halves[led.half];
    const px = half.pixels;

    if (!px || half.powerState === 1) continue;

    const idx = n - (led.half === 1 ? leds.length / 2 : 0);
    const r = px[idx * 3], g = px[idx * 3 + 1], b = px[idx * 3 + 2];

    if ((r | g | b) === 0) continue;

    const rect = rects[led.key];
    const rad = Math.min(6, rect.w * 0.12);

    ctx.beginPath();
    ctx.roundRect(rect.x + 1.5, rect.y + 1.5, rect.w - 3, rect.h - 3, rad);
    ctx.fillStyle = `rgba(${r},${g},${b},0.42)`;
    ctx.fill();
  }

  ctx.globalCompositeOperation = 'source-over';

  /* Small dots marking where each LED actually sits on the strip. */
  for (const led of leds) {
    ctx.beginPath();
    ctx.arc(led.x, led.y, 1.6, 0, Math.PI * 2);
    ctx.fillStyle = 'rgba(255,255,255,0.22)';
    ctx.fill();
  }
}

/* ------------------------------------------------------------ dt export */

function toDevicetree(scene, ledsPerHalf) {
  const hsb = c => `VFX_HSB(${c[0]}, ${c[1]}, ${c[2]})`;
  const ind = '            ';
  const out = [];

  out.push('/* Generated by the ZMK VFX simulator. */');
  out.push('#include <dt-bindings/zmk/vfx.h>');
  out.push('');
  out.push('/ {');
  out.push('    vfx_engine: vfx_engine {');
  out.push('        compatible = "zmk,vfx-engine";');
  out.push('        status = "okay";');
  out.push(`        virtual-length = <${ledsPerHalf * 2}>;`);
  out.push('        /* 0 on the left half, ' + ledsPerHalf + ' on the right. */');
  out.push('        strip-offset = <0>;');
  out.push(`        default-scene = <&${ident(scene.name)}>;`);
  out.push('    };');
  out.push('');
  /* Zones and scenes go beside the engine, never inside it: a phandle from a
   * node to its own descendant is a dependency cycle and devicetree rejects
   * it outright. They are matched by compatible wherever they sit.
   */
  out.push('    vfx_scenes {');
  out.push('        zones {');
  for (const [name, z] of Object.entries(scene.zones || {})) {
    const body = Array.isArray(z.pixels)
      ? `pixels = <${z.pixels.join(' ')}>;`
      : Array.isArray(z.keys)
        ? `keys = <${z.keys.join(' ')}>;`
        : `range = <${z.range[0]} ${z.range[1]}>;`;
    out.push(`            ${name}: ${name} { compatible = "zmk,vfx-zone"; ${body} };`);
  }
  out.push('        };');
  out.push('');
  out.push(`        ${ident(scene.name)}: ${ident(scene.name)} {`);
  out.push('            compatible = "zmk,vfx-scene";');
  out.push(`            display-name = "${scene.name}";`);

  for (const [i, l] of (scene.layers || []).entries()) {
    const node = `${l.type.replace(/-/g, '_')}_${i}`;
    const spec = LAYER_SPECS[l.type];

    out.push('');
    out.push(`${ind}${node} {`);
    out.push(`${ind}    compatible = "zmk,vfx-layer-${l.type}";`);
    out.push(`${ind}    zone = <&${l.zone}>;`);
    if ((l.blend ?? 'normal') !== 'normal') {
      out.push(`${ind}    blend = <${BLEND_NAME[BLEND[l.blend]]}>;`);
    }
    if ((l.opacity ?? 255) !== 255) out.push(`${ind}    opacity = <${l.opacity}>;`);

    if (l.color) out.push(`${ind}    color = <${hsb(l.color)}>;`);

    if (l.type === 'gradient') {
      out.push(`${ind}    stops = <${l.stops.map(hsb).join(' ')}>;`);
      if (l.scroll_speed) out.push(`${ind}    scroll-speed = <${l.scroll_speed}>;`);
      if (l.span) out.push(`${ind}    span = <${l.span}>;`);
    } else if (l.type === 'layer-state') {
      const colors = l.colors.map(c => (c === null ? 'VFX_BLACK' : hsb(c)));
      out.push(`${ind}    colors = <${colors.join(' ')}>;`);
    } else if (l.type === 'water') {
      if (l.crest_color) out.push(`${ind}    crest-color = <${hsb(l.crest_color)}>;`);
    } else if (l.type === 'matrix') {
      if (l.head_color) out.push(`${ind}    head-color = <${hsb(l.head_color)}>;`);
    } else if (l.type === 'flag') {
      if (l.source === 'modifiers') out.push(`${ind}    source = <VFX_FLAG_MODIFIERS>;`);
      const bits = l.source === 'modifiers' ? MODS : LOCKS;
      const names = l.source === 'modifiers' ? MOD_NAME : LOCK_NAME;
      out.push(`${ind}    mask = <${names[bits[l.bit] ?? 0x02]}>;`);
    } else if (l.type === 'wpm') {
      out.push(`${ind}    idle-color = <${hsb(l.idle_color)}>;`);
      out.push(`${ind}    fast-color = <${hsb(l.fast_color)}>;`);
      if (l.bar) out.push(`${ind}    bar = <1>;`);
    } else if (l.type === 'peripheral-battery') {
      out.push(`${ind}    high-color = <${hsb(l.high_color)}>;`);
      out.push(`${ind}    low-color = <${hsb(l.low_color)}>;`);
      if (l.empty_color) out.push(`${ind}    empty-color = <${hsb(l.empty_color)}>;`);
      if (l.unknown_color) out.push(`${ind}    unknown-color = <${hsb(l.unknown_color)}>;`);
      if (l.source) out.push(`${ind}    source = <${l.source}>;`);
    } else if (l.type === 'fire') {
      out.push(`${ind}    base-color = <${hsb(l.base_color)}>;`);
      out.push(`${ind}    tip-color = <${hsb(l.tip_color)}>;`);
      if (l.flicker !== undefined) out.push(`${ind}    flicker = <${l.flicker}>;`);
    } else if (l.type === 'comet') {
      if (l.head_color) out.push(`${ind}    head-color = <${hsb(l.head_color)}>;`);
    } else if (l.type === 'cross') {
      if (l.centre_color) out.push(`${ind}    centre-color = <${hsb(l.centre_color)}>;`);
      if (l.axes && l.axes !== 'both') {
        out.push(`${ind}    axes = <${CROSS_NAME[CROSS[l.axes]]}>;`);
      }
    } else if (l.type === 'battery') {
      out.push(`${ind}    high-color = <${hsb(l.high_color)}>;`);
      out.push(`${ind}    low-color = <${hsb(l.low_color)}>;`);
      if (l.empty_color) out.push(`${ind}    empty-color = <${hsb(l.empty_color)}>;`);
    } else if (l.type === 'ble-profile') {
      out.push(`${ind}    connected-color = <${hsb(l.connected_color)}>;`);
      out.push(`${ind}    disconnected-color = <${hsb(l.disconnected_color)}>;`);
      if (l.usb_color) out.push(`${ind}    usb-color = <${hsb(l.usb_color)}>;`);
    } else if (l.type === 'dart') {
      if (l.head_color) out.push(`${ind}    head-color = <${hsb(l.head_color)}>;`);
      if (l.reverse) out.push(`${ind}    reverse;`);
    } else if (l.type === 'pulse') {
      if (l.stack) out.push(`${ind}    stack;`);
    }

    /* Not tied to any one generator, so emitted for whichever names one. */
    if (l.opacity_source && l.opacity_source !== 'none') {
      out.push(`${ind}    opacity-source = <${SOURCE_NAME[SOURCES[l.opacity_source]]}>;`);
      if (l.opacity_min) out.push(`${ind}    opacity-min = <${l.opacity_min}>;`);
      if (l.opacity_full) out.push(`${ind}    opacity-full = <${l.opacity_full}>;`);
    }

    if (l.axis && l.axis !== 'strip') {
      out.push(`${ind}    axis = <${AXIS_NAME[AXIS[l.axis]]}>;`);
    }

    /* Only emit numeric properties that differ from the binding's default,
     * so the pasted devicetree stays as short as what was actually chosen.
     */
    for (const [name, dflt] of spec.args) {
      if (name === null || name === 'axis') continue; /* a gap, or emitted above */

      const v = l[name];
      if (v !== undefined && v !== dflt) {
        out.push(`${ind}    ${name.replace(/_/g, '-')} = <${v}>;`);
      }
    }

    out.push(`${ind}};`);
  }

  out.push('        };');
  out.push('    };');
  out.push('};');

  return out.join('\n');
}

const ident = s => s.toLowerCase().replace(/[^a-z0-9]+/g, '_').replace(/^_|_$/g, '') || 'scene';

/* -------------------------------------------------------------- presets */

const PRESETS = {
  Aurora: {
    name: 'Aurora',
    zones: { all: { range: [0, 36] } },
    layers: [
      { type: 'gradient', zone: 'all',
        stops: [[200, 100, 40], [280, 100, 60], [160, 90, 50]], scroll_speed: 8 },
    ],
  },
  Rainbow: {
    name: 'Rainbow',
    zones: { all: { range: [0, 36] } },
    layers: [
      { type: 'gradient', zone: 'all',
        stops: [[0, 100, 100], [60, 100, 100], [120, 100, 100],
                [180, 100, 100], [240, 100, 100], [300, 100, 100]],
        scroll_speed: 14 },
    ],
  },
  Ember: {
    name: 'Ember',
    zones: { all: { range: [0, 36] }, edge: { pixels: [0, 1, 2, 3, 32, 33, 34, 35] } },
    layers: [
      { type: 'solid', zone: 'all', color: [18, 100, 22] },
      { type: 'gradient', zone: 'all', blend: 'add', opacity: 200,
        stops: [[10, 100, 55], [35, 100, 10]], scroll_speed: 5, span: 12 },
      { type: 'solid', zone: 'edge', blend: 'screen', opacity: 90, color: [45, 70, 60] },
    ],
  },
  Reactive: {
    name: 'Reactive',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'solid', zone: 'all', color: [230, 70, 8] },
      { type: 'ripple', zone: 'all', blend: 'add', color: [190, 40, 100],
        decay_ms: 700, speed: 60, width: 10 },
    ],
  },
  'Twinkle night': {
    name: 'Twinkle night',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'solid', zone: 'all', color: [235, 100, 10] },
      { type: 'twinkle', zone: 'all', blend: 'screen', color: [45, 25, 100],
        period_ms: 1600, density: 55 },
    ],
  },
  Plasma: {
    name: 'Plasma',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'plasma', zone: 'all', color: [275, 90, 85],
        scale: 14, period_ms: 7000, hue_spread: 70 },
    ],
  },
  Breathe: {
    name: 'Breathe',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'breathe', zone: 'all', color: [155, 90, 100], period_ms: 4500, min_level: 20 },
    ],
  },
  Water: {
    name: 'Water',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'water', zone: 'all',
        color: [205, 95, 30], crest_color: [190, 30, 100],
        wavelength: 20, speed: 45, lifetime_ms: 3200, drop_rate_ms: 700,
        amplitude: 255, damping: 6 },
    ],
  },
  'Water (typing only)': {
    name: 'Water (typing only)',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'water', zone: 'all',
        color: [205, 95, 0], crest_color: [185, 25, 100],
        wavelength: 18, speed: 55, lifetime_ms: 2400, drop_rate_ms: 0,
        amplitude: 255, damping: 7 },
    ],
  },
  Matrix: {
    name: 'Matrix',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'matrix', zone: 'all',
        color: [125, 100, 55], head_color: [110, 25, 100],
        speed: 55, tail: 34, drop_rate_ms: 260, columns: 12, jitter: 90, head_size: 9 },
    ],
  },
  'Matrix (typing only)': {
    name: 'Matrix (typing only)',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'matrix', zone: 'all',
        color: [125, 100, 60], head_color: [110, 20, 100],
        speed: 150, tail: 24, drop_rate_ms: 0, columns: 12, jitter: 0, head_size: 8 },
    ],
  },
  /* Needs the "Underglow then per-key" wiring to mean anything: on any other
   * path those ranges are just the first six and the rest of one uniform
   * strip. Zones are how one scene addresses the two groups separately;
   * channels go further and give each its own brightness and scene, which
   * this page does not model.
   */
  'Underglow and keys': {
    name: 'Underglow and keys',
    zones: { glow: { range: [0, 6] }, keys: { range: [6, 29] } },
    layers: [
      { type: 'pulse', zone: 'glow', color: [265, 80, 100],
        decay_ms: 900, min_level: 40, hue_step: 7, stack: true },
      { type: 'ripple', zone: 'keys', color: [190, 40, 100],
        decay_ms: 700, speed: 60, width: 15 },
    ],
  },
  Pulse: {
    name: 'Pulse',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'pulse', zone: 'all', color: [190, 60, 100], decay_ms: 450 },
    ],
  },
  'Glow Pulse': {
    name: 'Glow Pulse',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'pulse', zone: 'all', color: [265, 80, 100],
        decay_ms: 900, min_level: 40, hue_step: 7, stack: true },
    ],
  },
  Held: {
    name: 'Held',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'solid', zone: 'all', color: [225, 70, 5] },
      { type: 'hold', zone: 'all', blend: 'add', color: [45, 55, 90], release_ms: 260 },
    ],
  },
  Darts: {
    name: 'Darts',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'dart', zone: 'all', axis: 'x',
        color: [285, 90, 70], head_color: [300, 20, 100],
        speed: 110, lifetime_ms: 1100, tail: 28 },
    ],
  },
  Forge: {
    name: 'Forge',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'solid', zone: 'all', color: [12, 100, 12] },
      /* The same fire as the Fire preset. Only its opacity is driven, which
       * is the whole point of the source: the generator is untouched.
       */
      { type: 'fire', zone: 'all', blend: 'add',
        base_color: [0, 100, 55], tip_color: [45, 75, 100],
        period_ms: 420, cell: 12, height: 235, flicker: 200, axis: 'y',
        opacity_source: 'wpm', opacity_min: 25, opacity_full: 60 },
    ],
  },
  Static: {
    name: 'Static',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'static', zone: 'all', color: [180, 15, 100],
        period_ms: 70, density: 45, hue_spread: 40 },
    ],
  },
  Pinwheel: {
    name: 'Pinwheel',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'gradient', zone: 'all', axis: 'angle',
        stops: [[0, 100, 70], [60, 100, 70], [120, 100, 70],
                [200, 100, 70], [280, 100, 70], [330, 100, 70]],
        scroll_speed: 40 },
    ],
  },
  Spiral: {
    name: 'Spiral',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'gradient', zone: 'all', axis: 'spiral',
        stops: [[190, 95, 75], [230, 100, 20], [280, 95, 70], [230, 100, 20]],
        scroll_speed: 55 },
    ],
  },
  'Out and in': {
    name: 'Out and in',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'wave', zone: 'all', axis: 'radial',
        color: [175, 85, 90], wavelength: 0, period_ms: 2600, depth: 220 },
    ],
  },
  Crosshair: {
    name: 'Crosshair',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'solid', zone: 'all', color: [230, 80, 6] },
      { type: 'cross', zone: 'all', blend: 'add',
        color: [190, 90, 60], centre_color: [40, 20, 100],
        decay_ms: 600, radius: 0, thickness: 4 },
    ],
  },
  Nexus: {
    name: 'Nexus',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'cross', zone: 'all',
        color: [300, 95, 80], centre_color: [0, 0, 100],
        decay_ms: 450, radius: 22, thickness: 4 },
    ],
  },
  Fire: {
    name: 'Fire',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'fire', zone: 'all', axis: 'y',
        base_color: [0, 100, 55], tip_color: [45, 75, 100],
        period_ms: 420, cell: 12, height: 235, flicker: 200 },
    ],
  },
  /* Two comets chasing each other across the board. An angular pair is the
   * classic dual beacon, but a split's halves only occupy two narrow slices
   * of the turn, so most of a lap would pass through the empty gap above and
   * below the board. Sweeping along x uses the whole width instead.
   */
  Beacons: {
    name: 'Beacons',
    zones: { all: { range: [0, 255] } },
    layers: [
      { type: 'comet', zone: 'all', axis: 'x',
        color: [265, 95, 75], head_color: [280, 25, 100],
        period_ms: 2600, tail: 45, count: 2 },
    ],
  },
  /* The two-tone look other keyboards call alphas/mods. Written as the keys
   * it is about, not as LED indices read off a wiring diagram.
   */
  'Alphas and mods': {
    name: 'Alphas and mods',
    zones: {
      all: { range: [0, 255] },
      mods: { keys: [0, 1, 11, 12, 13, 22, 23, 24, 25, 34, 35,
                     42, 43, 50, 51, 52, 53, 54, 55, 56, 57] },
    },
    layers: [
      { type: 'solid', zone: 'all', color: [205, 20, 45] },
      { type: 'solid', zone: 'mods', color: [25, 95, 70] },
    ],
  },
  'Status bar': {
    name: 'Status bar',
    zones: {
      all: { range: [0, 255] },
      battery: { range: [0, 6] },
      profiles: { range: [8, 5] },
      layers: { range: [30, 6] },
    },
    layers: [
      { type: 'solid', zone: 'all', color: [220, 40, 6] },
      { type: 'battery', zone: 'battery',
        high_color: [120, 100, 70], low_color: [0, 100, 80], warn_below: 25 },
      { type: 'ble-profile', zone: 'profiles',
        connected_color: [210, 100, 80], disconnected_color: [20, 100, 50],
        usb_color: [120, 100, 70] },
      { type: 'layer-state', zone: 'layers',
        colors: [null, [50, 100, 70], [280, 100, 70], [0, 100, 70]] },
    ],
  },
  /* Everything a keyboard can tell you about itself, or be told. Drive the
   * sliders and checkboxes in Keyboard state to see each one move.
   */
  'Full status': {
    name: 'Full status',
    zones: {
      all: { range: [0, 255] },
      battery: { range: [0, 6] },
      other_half: { range: [6, 6] },
      profiles: { range: [14, 5] },
      speed: { range: [20, 8] },
      layers: { range: [30, 6] },
    },
    layers: [
      { type: 'solid', zone: 'all', color: [220, 40, 5] },
      { type: 'battery', zone: 'battery',
        high_color: [120, 100, 70], low_color: [0, 100, 80], warn_below: 25 },
      { type: 'peripheral-battery', zone: 'other_half',
        high_color: [170, 100, 70], low_color: [0, 100, 80],
        unknown_color: [230, 60, 18], warn_below: 25 },
      { type: 'ble-profile', zone: 'profiles',
        connected_color: [210, 100, 80], disconnected_color: [20, 100, 50],
        usb_color: [120, 100, 70] },
      { type: 'wpm', zone: 'speed', bar: true,
        idle_color: [200, 90, 30], fast_color: [0, 95, 100], full: 80 },
      { type: 'layer-state', zone: 'layers',
        colors: [null, [50, 100, 70], [280, 100, 70], [0, 100, 70]] },
      /* Caps lock over the top of everything, since it is the one that
       * matters more than whatever it covers. */
      { type: 'flag', zone: 'all', blend: 'add',
        color: [0, 90, 35], source: 'locks', bit: 'caps' },
    ],
  },
  'Zones demo': {
    name: 'Zones demo',
    zones: {
      all: { range: [0, 36] },
      left_edge: { range: [0, 6] },
      right_edge: { range: [30, 6] },
    },
    layers: [
      { type: 'solid', zone: 'all', color: [220, 60, 12] },
      { type: 'solid', zone: 'left_edge', color: [120, 100, 70] },
      { type: 'solid', zone: 'right_edge', color: [300, 100, 70] },
    ],
  },
};

/* ------------------------------------------------------------------- main */

const $ = id => document.getElementById(id);

async function main() {
  const [wasmBytes, keyJson] = await Promise.all([
    fetch('vfx.wasm').then(r => r.arrayBuffer()),
    fetch('lily58-keys.json').then(r => r.json()),
  ]);

  keys = keyJson;

  /* Two instances, two memories: exactly like two MCUs, each holding only its
   * own half's animation state.
   */
  const halves = [
    new Half((await WebAssembly.instantiate(wasmBytes, {})).instance),
    new Half((await WebAssembly.instantiate(wasmBytes, {})).instance),
  ];

  const canvas = $('board');
  const ctx = canvas.getContext('2d');

  let ledsPerHalf = 36;
  let scene = structuredClone(PRESETS.Aurora);
  let playing = true;
  let clockStart = performance.now();
  let frozenAt = 0;

  let stripPath = 'serpentine';

  /* The right half's timebase correction. In synced mode a beacon nudges it
   * toward cancelling the drift, using the firmware's own slew policy, so the
   * page shows the correction easing in over seconds rather than snapping.
   */
  let syncOffset = 0;
  let lastBeacon = 0;
  const SYNC_INTERVAL_MS = 2000;
  const state = {
    brightness: 255, speed: 3, hue: 0, split: 'free', drift: 0,
    activeLayer: 0, battery: 78, profile: 0, connected: true, usb: false,
    periph: 0, wpm: 0, caps: false, shift: false,
  };

  let applyPowerPolicy = () => {};

  function reinit() {
    halves[0].init(ledsPerHalf, ledsPerHalf * 2, 0);
    halves[1].init(ledsPerHalf, ledsPerHalf * 2, ledsPerHalf);
    buildLayout(canvas, ledsPerHalf, stripPath);
    for (const h of halves) {
      h.setKeyMap(layout.keyPixels);
      h.setPositions(layout.positions);
    }
    applyScene();
    applyPowerPolicy();
  }

  function applyScene() {
    try {
      for (const h of halves) h.loadScene(scene);
      setStatus('applied', true);
      return true;
    } catch (err) {
      setStatus(err.message, false);
      return false;
    }
  }

  function setStatus(msg, ok) {
    const el = $('scene-status');
    el.textContent = msg;
    el.className = 'status ' + (ok ? 'ok' : 'err');
  }

  /* Park the clock at an exact engine time and draw there, rather than
   * wherever the wall clock happens to be. Screenshotting a live animation
   * samples it unevenly, which is what makes a recorded GIF stutter, and it
   * cannot land on an effect's period. Stepping lets a capture cover exactly
   * one period so the loop closes. Used by tools/record-gifs.mjs.
   *
   * It freezes rather than drawing one frame and returning: the animation
   * loop is still running and would paint over it before a screenshot lands.
   */
  function renderAt(t) {
    playing = false;
    frozenAt = t;
    drawFrame(t);
  }

  function drawFrame(t) {

    for (const h of halves) {
      h.setState(state.brightness, state.speed, state.hue);
      h.setStatus(state.activeLayer, state.battery, state.profile, state.connected, state.usb);
      h.setExtraStatus(state.caps ? LOCK_CAPS : 0, state.shift ? MOD_SHIFT_BITS : 0,
                       state.wpm, state.periph);
    }

    halves[0].render(t);

    /* The right half runs on its own crystal, which the drift slider stands
     * in for. Free-running mode leaves that error in place. Synced mode
     * beacons a correction toward cancelling it, eased in by the same policy
     * the firmware uses, so the seam visibly pulls back into alignment.
     */
    if (state.split === 'synced') {
      if (t - lastBeacon >= SYNC_INTERVAL_MS) {
        lastBeacon = t;
        syncOffset = halves[1].e.vfx_sim_sync_step(syncOffset, -state.drift);
      }
    } else {
      syncOffset = 0;
      lastBeacon = t;
    }

    halves[1].render(t + state.drift + syncOffset);

    draw(ctx, halves);

    const lit = halves[0].lit || halves[1].lit;
    const anim = halves[0].animating || halves[1].animating;
    const RAIL = ['rail on', 'rail gated off', 'rail settling'];
    const RAIL_CLASS = ['on', 'gated', 'warn'];
    const worst = Math.max(halves[0].powerState === 1 ? 1 : halves[0].powerState,
                           halves[1].powerState === 1 ? 1 : halves[1].powerState);
    const ua = halves[0].microamps + halves[1].microamps;

    $('stat-pixels').textContent = `${ledsPerHalf} LEDs per half (${ledsPerHalf * 2} total)`;
    $('stat-lit').textContent = lit ? 'lit' : 'all pixels off';
    $('stat-lit').className = 'pill ' + (lit ? 'on' : 'off');
    $('stat-anim').textContent = anim ? 'animating' : 'static';
    $('stat-anim').className = 'pill ' + (anim ? 'on' : 'off');

    const bothGated = halves[0].powerState === 1 && halves[1].powerState === 1;
    $('stat-rail').textContent = bothGated ? RAIL[1] : RAIL[worst === 1 ? 0 : worst];
    $('stat-rail').className = 'pill ' + (bothGated ? 'gated' : RAIL_CLASS[worst === 1 ? 0 : worst]);
    $('stat-ma').textContent = `~${(ua / 1000).toFixed(1)} mA at the strip`;
    $('stat-ma').className = 'pill ' + (ua === 0 ? 'on' : 'off');

    const residual = state.split === 'synced' ? state.drift + syncOffset : state.drift;
    $('out-drift').textContent =
      state.split === 'synced' ? `${state.drift} ms, ${residual} ms left` : `${state.drift} ms`;
  }

  function tick(now) {
    drawFrame(playing ? (now - clockStart) : frozenAt);
    requestAnimationFrame(tick);
  }

  /* -- controls -- */

  const bindRange = (id, key, fmt = v => v) => {
    const el = $(id), out = $('out-' + id);
    const sync = () => { out.textContent = fmt(Number(el.value)); };
    el.addEventListener('input', () => { state[key] = Number(el.value); sync(); });
    sync();
  };

  bindRange('brightness', 'brightness', v => `${Math.round(v / 255 * 100)}%`);
  bindRange('speed', 'speed');
  bindRange('hue', 'hue', v => `${v}°`);
  bindRange('drift', 'drift', v => `${v} ms`);
  $('drift').addEventListener('input', () => { lastBeacon = -1e9; });

  applyPowerPolicy = () => {
    const blackout = Number($('blackout').value);
    const settle = Number($('settle').value);
    $('out-blackout').textContent = `${blackout} ms`;
    $('out-settle').textContent = `${settle} ms`;
    for (const h of halves) h.setPowerPolicy(blackout, settle);
  };

  $('blackout').addEventListener('input', applyPowerPolicy);
  $('settle').addEventListener('input', applyPowerPolicy);

  $('fade-black').addEventListener('click', () => {
    /* Ramps brightness down rather than snapping, so the gate is seen to wait
     * out its delay instead of firing the instant the value hits zero.
     */
    const el = $('brightness');
    const from = state.brightness;
    const t0 = performance.now();
    const step = now => {
      const k = Math.min(1, (now - t0) / 900);
      state.brightness = Math.round(from * (1 - k));
      el.value = state.brightness;
      $('out-brightness').textContent = `${Math.round(state.brightness / 255 * 100)}%`;
      if (k < 1) requestAnimationFrame(step);
    };
    requestAnimationFrame(step);
  });

  bindRange('activeLayer', 'activeLayer');
  bindRange('battery', 'battery', v => `${v}%`);
  bindRange('profile', 'profile');
  bindRange('periph', 'periph', v => (v === 0 ? 'not reported' : `${v}%`));
  bindRange('wpm', 'wpm', v => `${v} wpm`);

  for (const [id, key] of [['connected', 'connected'], ['usb', 'usb'],
                           ['caps', 'caps'], ['shift', 'shift']]) {
    const el = $(id);
    el.addEventListener('change', () => { state[key] = el.checked; });
  }

  $('count').addEventListener('input', e => {
    ledsPerHalf = Number(e.target.value);
    $('out-count').textContent = ledsPerHalf;
    reinit();
  });
  $('out-count').textContent = ledsPerHalf;

  $('path').addEventListener('change', e => {
    stripPath = e.target.value;

    /* A mixed board's count is not a free choice: it is one LED per key plus
     * the underglow, so the slider follows the wiring rather than fighting it.
     */
    if (stripPath === 'mixed') {
      const perHalf = keys.filter(k => k.x < 800).length;

      ledsPerHalf = MIXED_GLOW + perHalf;
      $('count').value = ledsPerHalf;
      $('out-count').textContent = ledsPerHalf;
      $('count').disabled = true;

      reinit();

      return;
    }

    $('count').disabled = false;

    buildLayout(canvas, ledsPerHalf, stripPath);

    for (const h of halves) {
      h.setKeyMap(layout.keyPixels);
      h.setPositions(layout.positions);
    }
  });

  $('playpause').addEventListener('click', e => {
    if (playing) { frozenAt = performance.now() - clockStart; }
    else { clockStart = performance.now() - frozenAt; }
    playing = !playing;
    e.target.textContent = playing ? 'Pause' : 'Play';
  });

  $('restart').addEventListener('click', () => { clockStart = performance.now(); frozenAt = 0; });

  for (const el of document.querySelectorAll('input[name=split]')) {
    el.addEventListener('change', e => { state.split = e.target.value; });
  }

  $('apply').addEventListener('click', () => {
    try {
      scene = JSON.parse($('scene').value);
    } catch (err) {
      setStatus('invalid JSON: ' + err.message, false);
      return;
    }
    if (applyScene()) {
      document.querySelectorAll('.preset').forEach(b => b.classList.remove('active'));
    }
  });

  $('export').addEventListener('click', async () => {
    const dt = toDevicetree(scene, ledsPerHalf);
    try {
      await navigator.clipboard.writeText(dt);
      setStatus('devicetree copied to clipboard', true);
    } catch {
      $('scene').value = dt;
      setStatus('clipboard blocked; devicetree placed in the editor', true);
    }
  });

  const presetRow = $('presets');
  for (const name of Object.keys(PRESETS)) {
    const b = document.createElement('button');
    b.textContent = name;
    b.className = 'preset';
    b.addEventListener('click', () => {
      scene = structuredClone(PRESETS[name]);
      $('scene').value = JSON.stringify(scene, null, 2);
      applyScene();
      for (const h of halves) h.e.vfx_sim_power_reset();
      document.querySelectorAll('.preset').forEach(x => x.classList.remove('active'));
      b.classList.add('active');
    });
    presetRow.appendChild(b);
  }

  canvas.addEventListener('click', ev => {
    const r = canvas.getBoundingClientRect();
    const x = (ev.clientX - r.left) * (canvas.width / r.width);
    const y = (ev.clientY - r.top) * (canvas.height / r.height);

    const hit = layout.rects.find(k => x >= k.x && x <= k.x + k.w && y >= k.y && y <= k.y + k.h);
    if (!hit) return;

    /* A peripheral only sees its own keys unless split sync is on; the
     * simulator models that rather than pretending both halves see everything.
     */
    const targets = state.split === 'synced' ? halves : [halves[hit.half]];
    for (const h of targets) {
      h.key(hit.i, true);
      setTimeout(() => h.key(hit.i, false), 40);
    }
  });

  $('scene').value = JSON.stringify(scene, null, 2);
  reinit();

  /* Small handle for poking at the engine from the console or a test. */
  window.vfxDebug = {
    halves, state, layout: () => layout, renderAt,
    pause: () => { playing = false; },
    press: i => { const half = layout.rects[i].half; halves[half].key(i, true); },
    pixels: h => Array.from(halves[h].pixels || []),
  };

  requestAnimationFrame(tick);
}

main().catch(err => {
  document.body.insertAdjacentHTML('afterbegin',
    `<p style="color:#ff7b72">Simulator failed to start: ${err.message}</p>`);
});
