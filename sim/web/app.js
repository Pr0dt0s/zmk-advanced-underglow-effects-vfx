/* Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * Drives two instances of the firmware compositor, one per half of the split,
 * and paints their output under a Lily58. The key geometry is the real one,
 * lifted from ZMK's zmk,physical-layout for this board.
 */

const BLEND = { normal: 0, add: 1, multiply: 2, screen: 3, max: 4 };
const BLEND_NAME = ['VFX_BLEND_NORMAL', 'VFX_BLEND_ADD', 'VFX_BLEND_MULTIPLY',
                    'VFX_BLEND_SCREEN', 'VFX_BLEND_MAX'];

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
      if (Array.isArray(z.pixels)) {
        const scratch = this.e.vfx_sim_scratch();
        const mem = this.mem;
        z.pixels.forEach((p, i) => { mem[scratch + i] = p & 0xff; });
        zoneIds[name] = this.e.vfx_sim_add_zone_pixels(z.pixels.length);
      } else if (Array.isArray(z.range)) {
        zoneIds[name] = this.e.vfx_sim_add_zone_range(z.range[0], z.range[1]);
      } else {
        throw new Error(`zone "${name}" needs a range or a pixels list`);
      }
      if (zoneIds[name] < 0) throw new Error(`zone "${name}" was rejected (too many zones?)`);
    }

    for (const [i, l] of (scene.layers || []).entries()) {
      const zid = zoneIds[l.zone];
      if (zid === undefined) throw new Error(`layer ${i} references unknown zone "${l.zone}"`);

      const blend = BLEND[l.blend ?? 'normal'];
      if (blend === undefined) throw new Error(`layer ${i} has unknown blend "${l.blend}"`);
      const opacity = l.opacity ?? 255;

      let rc;
      if (l.type === 'solid') {
        rc = this.e.vfx_sim_add_solid(zid, blend, opacity, packHsb(...l.color));
      } else if (l.type === 'gradient') {
        const scratch = this.e.vfx_sim_scratch();
        const view = this.view;
        l.stops.forEach((c, k) => view.setUint32(scratch + k * 4, packHsb(...c), true));
        rc = this.e.vfx_sim_add_gradient(zid, blend, opacity,
                                         l.scroll_speed ?? 0, l.span ?? 0, l.stops.length);
      } else {
        throw new Error(`layer ${i} has unknown type "${l.type}"`);
      }
      if (rc < 0) throw new Error(`layer ${i} was rejected by the engine`);
    }
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

  const spacing = (Math.max(...rects.map(r => r.w)) || 40);
  layout = { rects, leds, scale, spacing };
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

    const grad = ctx.createRadialGradient(led.x, led.y, 0, led.x, led.y, radius);
    grad.addColorStop(0, `rgba(${r},${g},${b},0.95)`);
    grad.addColorStop(0.35, `rgba(${r},${g},${b},0.38)`);
    grad.addColorStop(1, `rgba(${r},${g},${b},0)`);

    ctx.fillStyle = grad;
    ctx.beginPath();
    ctx.arc(led.x, led.y, radius, 0, Math.PI * 2);
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
  out.push('');
  out.push('        zones {');
  for (const [name, z] of Object.entries(scene.zones || {})) {
    const body = Array.isArray(z.pixels)
      ? `pixels = <${z.pixels.join(' ')}>;`
      : `range = <${z.range[0]} ${z.range[1]}>;`;
    out.push(`            ${name}: ${name} { compatible = "zmk,vfx-zone"; ${body} };`);
  }
  out.push('        };');
  out.push('');
  out.push(`        ${ident(scene.name)}: ${ident(scene.name)} {`);
  out.push('            compatible = "zmk,vfx-scene";');
  out.push(`            display-name = "${scene.name}";`);

  for (const [i, l] of (scene.layers || []).entries()) {
    const node = `${l.type}_${i}`;
    out.push('');
    out.push(`${ind}${node} {`);
    out.push(`${ind}    compatible = "zmk,vfx-layer-${l.type}";`);
    out.push(`${ind}    zone = <&${l.zone}>;`);
    if ((l.blend ?? 'normal') !== 'normal') {
      out.push(`${ind}    blend = <${BLEND_NAME[BLEND[l.blend]]}>;`);
    }
    if ((l.opacity ?? 255) !== 255) out.push(`${ind}    opacity = <${l.opacity}>;`);

    if (l.type === 'solid') {
      out.push(`${ind}    color = <${hsb(l.color)}>;`);
    } else if (l.type === 'gradient') {
      out.push(`${ind}    stops = <${l.stops.map(hsb).join(' ')}>;`);
      if (l.scroll_speed) out.push(`${ind}    scroll-speed = <${l.scroll_speed}>;`);
      if (l.span) out.push(`${ind}    span = <${l.span}>;`);
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
  const state = { brightness: 255, speed: 3, hue: 0, split: 'free', drift: 0 };

  let applyPowerPolicy = () => {};

  function reinit() {
    halves[0].init(ledsPerHalf, ledsPerHalf * 2, 0);
    halves[1].init(ledsPerHalf, ledsPerHalf * 2, ledsPerHalf);
    buildLayout(canvas, ledsPerHalf, stripPath);
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

  function tick(now) {
    const t = playing ? (now - clockStart) : frozenAt;

    for (const h of halves) h.setState(state.brightness, state.speed, state.hue);

    halves[0].render(t);
    /* Free-running halves keep independent clocks, which is what the drift
     * slider stands in for. Synced mode makes the right half follow the left.
     */
    halves[1].render(state.split === 'synced' ? t : t + state.drift);

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

  $('count').addEventListener('input', e => {
    ledsPerHalf = Number(e.target.value);
    $('out-count').textContent = ledsPerHalf;
    reinit();
  });
  $('out-count').textContent = ledsPerHalf;

  $('path').addEventListener('change', e => {
    stripPath = e.target.value;
    buildLayout(canvas, ledsPerHalf, stripPath);
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
  requestAnimationFrame(tick);
}

main().catch(err => {
  document.body.insertAdjacentHTML('afterbegin',
    `<p style="color:#ff7b72">Simulator failed to start: ${err.message}</p>`);
});
