# ZMK Advanced Underglow Effects (VFX)

A ZMK module that replaces the built-in RGB underglow with a small compositor:
**scenes** are stacks of **layers**, each scoped to a **zone** of pixels and
combined with a **blend mode**, all described in devicetree.

It also cuts power to the LEDs whenever a frame renders entirely black, which
on a wireless keyboard is worth more than everything else here put together.

![Aurora](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/aurora.gif)

Every clip on this page is the real compositor: the frames come from the
WebAssembly build of the same C the firmware runs, recorded off the simulator
rather than mocked up. They are generated and published to GitHub Pages by
CI rather than committed, so they cannot fall out of date with the effects
they show. See [Recording the clips](#recording-the-clips).

## Why it replaces core underglow rather than extending it

ZMK's `app/src/rgb_underglow.c` dispatches four effects from a `switch` over a
fixed `enum`, with file-static state, compiled only under
`CONFIG_ZMK_RGB_UNDERGLOW`. There is no registration API and no weak symbol, so
a module cannot add an effect to it.

This module therefore owns the strip outright. `CONFIG_ZMK_RGB_UNDERGLOW` must
be `n`, and a build asserts it rather than letting two engines fight over the
same pixels.

Existing keymaps keep working: ZMK declares the `rgb_ug` node unconditionally,
so the module binds it and maps the `RGB_*` commands onto the engine.

## Quick start

`config/west.yml`:

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: pr0dt0s
      url-base: https://github.com/pr0dt0s
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-advanced-underglow-effects-vfx
      remote: pr0dt0s
      revision: main
  self:
    path: config
```

`config/<shield>.conf`:

```ini
CONFIG_ZMK_RGB_UNDERGLOW=n
CONFIG_ZMK_VFX=y
CONFIG_ZMK_VFX_AUTO_POWER_GATE=y
```

`config/<shield>.overlay` — note ZMK looks for `<shield>_<board>.overlay` or
`<shield>.overlay`, so the filename has to match your shield exactly:

```dts
#include <dt-bindings/zmk/vfx.h>
#include <vfx/presets.dtsi>

/ {
    vfx_engine: vfx_engine {
        compatible = "zmk,vfx-engine";
        status = "okay";
        default-scene = <&vfx_aurora>;
    };
};
```

Add `#include <behaviors/vfx.dtsi>` to your keymap for the `&vfx` behavior.
A complete working configuration lives in [`example/`](example/), which is
what CI builds.

### If the colours come out wrong

Before blaming an effect, check `color-mapping` on your strip node. It is a
list of Zephyr `LED_COLOR_ID` values, and those are **not** zero-based on red:

```
LED_COLOR_ID_WHITE 0   LED_COLOR_ID_RED 1
LED_COLOR_ID_GREEN 2   LED_COLOR_ID_BLUE 3
```

So the GRB order that WS2812 and SK6812 want is `<2 1 3>`. Writing `<1 0 2>`
in the belief that it means green-red-blue instead says red-white-green, which
on a strip with no white channel feeds the chip's red line a constant zero and
never transmits blue at all. The symptom is that red never appears however you
configure a scene, greens look blue, and it is very easy to spend an evening
adjusting effects that were correct all along.

A quick way to tell: light three separate runs of pixels at hue 0, 120 and 240
and check they read red, green and blue in that order. If they are rotated or
reversed, it is the mapping, not the scene. Zero the hue shift first
(`&vfx VFX_SET_HUE(0)`), since a stored one rotates all three together and
looks exactly like a wiring fault.

## The simulator

`sim/` compiles the real compositor — `render.c`, `color.c` and the generators,
unmodified — to WebAssembly and renders it under a Lily58 in the browser. No
effect is reimplemented in JavaScript, so a frame on the page is the frame the
keyboard produces.

Every generator is reachable, including the reactive ones and a driven
`opacity-source` — click a key on the board to fire a press, and the WPM
control drives anything whose opacity follows it.

The strip path includes **Underglow then per-key**, which models a board that
lights the keys and the case from one chain: six downward-facing pixels ahead
of one per key, drawn as a wash under the board and as lit keycaps
respectively, with each key mapped to its own LED rather than to whichever is
nearest. That is the wiring the PandaKB map describes, and it is also the one
board on this page that carries more than one channel: a `glow` channel over
the underglow pixels and a `keys` channel over the rest, each defaulted to
its own scene, exactly as `pandakb-lily58.dtsi`'s overlay wires them.

A board with more than one channel gets a row of tabs above the presets, one
per channel. The Engine panel's brightness, speed and hue sliders, and the
on/off checkbox beside them, belong to whichever tab is selected rather than
to the board, and the composer and scene JSON below edit that channel's own
scene — drop any preset onto any channel and it lights only that channel's
LEDs, the same clipping a real channel's `range` does at runtime. **Copy as
devicetree** follows: a one-channel board still exports a plain
`default-scene`, and a multi-channel one exports a `zmk,vfx-channel` node per
tab.

Internally this still runs on one wasm instance per physical half rather
than one per channel — every channel's layers are merged into a single scene
before it is loaded, each tagged with its own tuning slot (`vfx_sim_tune` /
`vfx_sim_set_layer_tune`, the same calls behind [adjusting a layer while the
keyboard runs](#adjusting-a-layer-while-the-keyboard-runs)), which is how
they end up with independent brightness, speed and hue out of one render
pass. That is a simulator trick built out of machinery already linked in,
not what ships: real firmware gives each channel its own render call and
copies out only the pixels it owns. A `keys`-type zone is the one thing this
cannot preview correctly if it names a key outside its own channel's window,
since resolving it happens inside the engine, past where the merge could
clip it.

### Building a scene

The panel under the presets is a composer: add a layer, pick its generator,
and set its zone, blend, colours and properties with real controls rather than
by remembering property names. Layers reorder and delete, every change lands
on the strip immediately, and **Copy as devicetree** emits what you built,
ready to paste into a keymap.

The controls are generated from the same table the loader and the exporter
read, so a generator gains an editor by being described once. The two things
it does not cover are gradient stops and a layer-state colour list, both
variable-length; the **Scene JSON** box underneath is the same object and
edits either way round, so those stay reachable.

`tools/verify-sim.mjs` drives the page in a headless browser and asserts on
what comes out rather than on whether it compiled: that a reactive scene is
black until a key goes down, that a held key stays lit past any decay and
falls when released, that a driven opacity moves with its signal, and that the
composer's controls, its JSON and the strip all agree. CI runs it on every
push.

```sh
./sim/build.sh && python3 -m http.server -d docs/sim
```

`sim/build-artifact.py` folds the whole thing into one self-contained HTML
file, inlining the stylesheet, the script, the key geometry and the
WebAssembly module as base64. That version needs no server and no network at
all, which is what makes it publishable somewhere the page cannot fetch its
own assets.

It runs two instances side by side, one per half, each with its own memory and
animation state, which is what two MCUs actually are. You can:

- design a scene as JSON and press **Copy as devicetree** to paste it into a
  keymap;
- watch the power rail gate off and the estimated current fall to zero;
- compare free-running and synchronised split modes with an injectable clock
  drift;
- click keys to fire reactive effects, and drive battery, profile and layer
  state from sliders so indicators can be checked without hardware in a
  particular state.

### Recording the clips

The GIFs in this README are recorded from the simulator, so they are frames
the real compositor produced rather than an impression of it:

```sh
npm install playwright && npx playwright install chromium
pip install Pillow
apt-get install gifsicle      # optional, worth about a third of the file size
./sim/build.sh && python3 sim/build-artifact.py
node tools/record-gifs.mjs    # everything, or name clips: ... pinwheel fire
```

`CHROMIUM_PATH` overrides the browser if playwright cannot find one itself,
and `VFX_KEEP_FRAMES=1` leaves the PNG frames behind, which is what you want
when comparing encoder settings rather than re-recording each time.

You only need this to preview a change locally. CI records the clips itself
on the way to publishing Pages, so what the README shows always matches the
effects on `main`; `docs/img/` is generated output and is not committed. Every
push also records two clips as a build artifact, so a broken recorder fails on
the push that broke it rather than on the next deploy.

Engine time is stepped explicitly rather than sampled off the wall clock,
which is what lets a clip cover exactly one period of an effect and loop
seamlessly, and it means the same command produces the same frames every
time.

`tests/` holds headless assertions for the parts eyeballing will not catch. It
builds the engine with no Zephyr in scope at all, which is what keeps the
simulator honest: if the compositor ever grows a kernel dependency, that build
breaks loudly instead of quietly diverging from the firmware.

```sh
cd tests && make test
```

## Scenes

Layers composite bottom to top in devicetree order.

Zones and scenes are matched by `compatible` wherever they sit in the tree,
and they must **not** sit under the engine node: a phandle from a node to its
own descendant is a dependency cycle, which devicetree rejects outright.
Give them a container of their own alongside it.

```dts
#include <dt-bindings/zmk/vfx.h>

/ {
    vfx_engine: vfx_engine {
        compatible = "zmk,vfx-engine";
        status = "okay";

        /* Whole board across both halves, and where this half starts in it.
         * This is what lets a gradient run continuously across the seam.
         */
        virtual-length = <72>;
        strip-offset = <0>;      /* 36 on the right half */

        default-scene = <&aurora>;
    };

    my_vfx {
        zones {
            all:  all  { compatible = "zmk,vfx-zone"; range = <0 255>; };
            edge: edge { compatible = "zmk,vfx-zone"; pixels = <0 1 2 33 34 35>; };
        };

        aurora: aurora {
            compatible = "zmk,vfx-scene";
            display-name = "Aurora";

            bg {
                compatible = "zmk,vfx-layer-gradient";
                zone = <&all>;
                stops = <VFX_HSB(200,100,40) VFX_HSB(280,100,60) VFX_HSB(160,90,50)>;
                scroll-speed = <12>;
            };

            ripple {
                compatible = "zmk,vfx-layer-ripple";
                zone = <&all>;
                blend = <VFX_BLEND_ADD>;
                color = <VFX_HSB(0,0,100)>;
                decay-ms = <400>;
            };
        };
    };
};
```

A zone length is clamped to the real `chain-length`, so `range = <0 255>` means
"the whole strip" and the same preset fits a 10 pixel board and a 72 pixel one.

A zone can also be written as the keys it is about rather than as LED indices:

```dts
mods: mods {
    compatible = "zmk,vfx-zone";
    keys = <0 1 11 12 13 22 23 24 25 34 35 42 43 50 51 52 53 54 55 56 57>;
};
```

The engine resolves those through `key-pixels` at startup, so a two-tone
alphas-and-mods scene is written as what it is, and stays correct if the strip
is rerouted. On a split both halves take the same list and each keeps the keys
that landed on its own strip, so one definition covers the whole board.

### Generators

| Compatible | What it does |
|---|---|
| `zmk,vfx-layer-solid` | One colour across the zone |
| `zmk,vfx-layer-gradient` | Cyclic multi-stop gradient, optionally scrolling |
| `zmk,vfx-layer-breathe` | Pulses between a floor and the full colour |
| `zmk,vfx-layer-wave` | Travelling sine along the strip |
| `zmk,vfx-layer-twinkle` | Scattered pixels fading up and out |
| `zmk,vfx-layer-plasma` | Summed sines over both board axes, swinging the hue |
| `zmk,vfx-layer-fire` | A hot bed with flames licking up the board |
| `zmk,vfx-layer-comet` | A bright head running round the board with a tail |
| `zmk,vfx-layer-water` | A rippling surface, disturbed by rain and by typing |
| `zmk,vfx-layer-matrix` | Falling columns of light, started by rain and by typing |
| `zmk,vfx-layer-ripple` | Rings expanding from each pressed key |
| `zmk,vfx-layer-cross` | Lights the pressed key's row and column |
| `zmk,vfx-layer-keyflash` | Lights the pressed key and fades in place |
| `zmk,vfx-layer-trail` | Decaying heat map that builds up as you type |
| `zmk,vfx-layer-pulse` | Lifts the whole zone on any press, ignoring which key |
| `zmk,vfx-layer-hold` | Lights a key for as long as it is held down |
| `zmk,vfx-layer-dart` | A press launches a packet that travels along an axis |
| `zmk,vfx-layer-static` | Independent per-pixel randomness, re-rolled on a clock |
| `zmk,vfx-layer-layer-state` | Colour per active keymap layer |
| `zmk,vfx-layer-battery` | Fills a zone in proportion to charge |
| `zmk,vfx-layer-ble-profile` | One pixel per profile, lighting the selected one |
| `zmk,vfx-layer-peripheral-battery` | The *other* half's cell, on a split central |
| `zmk,vfx-layer-flag` | Lights while a lock LED is on or a modifier is held |
| `zmk,vfx-layer-wpm` | Colours or fills by how fast you are typing |

Three of those read state ZMK only computes when you ask it to, so they need a
Kconfig symbol each — set them in your `.conf`:

| Layer | Needs |
|---|---|
| `flag` with `source = <VFX_FLAG_LOCKS>` | `CONFIG_ZMK_HID_INDICATORS=y` |
| `wpm` | `CONFIG_ZMK_WPM=y`, **central half only** |
| `peripheral-battery` | `CONFIG_ZMK_SPLIT_BLE_CENTRAL_BATTERY_LEVEL_FETCHING=y` |

`CONFIG_ZMK_WPM=y` has to go in the central half's conf, not the shared one.
ZMK compiles `wpm.c` for every role but only produces the keycode events it
listens for on the central, so a peripheral built with it set fails to link.
ZMK applies every matching conf file, so a `<shield>_left.conf` beside your
`<shield>.conf` is merged on top of it for that half alone.

Caps lock is worth a note. A keyboard cannot know it by itself: pressing the
key is a *request* to the host, not a toggle, and the keyboard only learns the
answer when the host sends an LED report back. That report is what
`CONFIG_ZMK_HID_INDICATORS` turns on.

There is still no caps *word* indicator: ZMK exposes neither a state accessor
nor an event for that one, so it could not be driven on hardware.

On a split, the layer and BLE profile indicators only work on the **central**
half. ZMK compiles its keymap and BLE profile code for the central only, so a
peripheral has no layer or profile to report and those layers stay dark there.

### Which reactive generator

They differ in what they take from a keypress, which is what decides whether
one can stand in for another:

| | Reads | Good for |
|---|---|---|
| `ripple`, `cross`, `keyflash`, `trail` | **where** the key is | LEDs that sit under the keys |
| `pulse` | only **that** a key was pressed | LEDs that do not: underglow, a status strip |
| `dart` | where it started, then moves off | anything long enough to travel along |
| `hold` | **when it is let go** | showing a chord, or the layer key you are leaning on |

The first group needs `key-pixels` and pixels mounted under the keys. Point
one at underglow behind the board and it aims at a key position that means
nothing there, which reads as noise. `pulse` takes the other half of a
keypress — that one happened at all — and lifts the whole zone together.

`hold` is the only one that reads a release. The rest are struck and then
decay on a schedule fixed at the moment of the press, which cannot express a
duration nobody knows yet, so "lit for as long as you hold it" is not
reachable by combining them however they are stacked.
Battery works on both, and shows each half's own cell.

Every generator takes `zone`, `blend` (`VFX_BLEND_NORMAL`, `_ADD`,
`_MULTIPLY`, `_SCREEN`, `_MAX`) and `opacity`. See `dts/bindings/` for each
one's own properties; `dts/vfx/presets.dtsi` has seventeen ready-made scenes.

`water` is both an ambient and a reactive effect. Each drop sends out a
decaying wavetrain, and overlapping drops sum as signed surface height before
being coloured, so they interfere rather than drawing as separate rings.
Ambient drops are derived from the clock, so both halves of a split agree on
where the rain falls without exchanging anything. Set `drop-rate-ms = <0>` for
a surface that only moves when you type. Still water at zero brightness draws
nothing at all, which is what lets the power gate cut the rail between
keypresses; any brightness above zero lights every pixel in the zone forever,
and the rail stays up.

`cross` is the other way to react to a key: instead of asking how far a pixel
is from it, it asks whether the pixel is *in line* with it, so the reaction
traces the board's grid. One generator covers three familiar shapes — the full
cross, a band across the board (`axes = <VFX_CROSS_HORIZONTAL>`), and a short
cross around the key that fades over a `radius`.

`matrix` works the same way, and is the other generator that is ambient and
reactive at once: drops fall down the board leaving a fading trail, started
either by `drop-rate-ms` or by a keypress. The two differ in where they stop.
Rain runs off the bottom; a keypress drop falls in from the top of the key's
column and stops on the key you pressed, then drains into it, so the whole
column points at what you typed. `drop-rate-ms = <0>` leaves keypress-only
rain, which is dark and idle between presses and so lets the power gate cut
the rail. It is the one generator that needs to know which way is *down*, so
give it `pixel-positions` below; without a map it treats strip order as the
direction of travel and falls in a single column.

### Letting something drive how strongly a layer shows

A layer's `opacity` is normally a fixed number. Point `opacity-source` at
something the keyboard already knows and it becomes the value reached at full
signal instead, with `opacity-min` at the bottom:

```dts
flames {
    compatible = "zmk,vfx-layer-fire";
    zone = <&vfx_all>;
    /* ...the same fire as ever... */

    opacity-source = <VFX_SRC_WPM>;
    opacity-min = <25>;
    opacity-full = <60>;   /* words per minute that reaches full */
};
```

That is the shipped `Forge` preset, and the point of it is that the generator
is untouched: an ordinary fire banks up as you type and dies back when you
stop, without a line of fire code knowing that typing exists. Because this
lives in the compositor rather than in any generator, every one of them gains
it at once.

| Source | Signal | `opacity-full` means |
|---|---|---|
| `VFX_SRC_NONE` | — | fixed at `opacity`, the default |
| `VFX_SRC_WPM` | typing speed | words per minute, so set it around 60 |
| `VFX_SRC_BATTERY` | charge | percent, so 100, or leave it |
| `VFX_SRC_ACTIVITY` | anyone at the keyboard | ignored, it is a yes or no |

`opacity-full` at 0 means 255, which suits a percentage and is far too high
for a WPM — a fire driven by typing and left at the default would never get
off `opacity-min`.

`VFX_SRC_WPM` needs `CONFIG_ZMK_WPM=y` on the central, with the same caveat as
the `wpm` layer above.

### Channels: different effects on different pixels

Some boards chain LEDs that light quite different things — underglow behind
the board and per-key LEDs above it, on one wire. A channel is a slice of the
strip with its own scene list, and its own scene, brightness, speed, hue and
on/off at runtime:

```dts
&vfx_engine {
    glow: channel-glow {
        compatible = "zmk,vfx-channel";
        range = <0 6>;
        scenes = <&vfx_glow_pulse &vfx_breathe &vfx_ember>;
        default-scene = <&vfx_glow_pulse>;
    };

    keys: channel-keys {
        compatible = "zmk,vfx-channel";
        range = <6 29>;
        scenes = <&vfx_reactive &vfx_matrix &vfx_crosshair>;
    };
};
```

`range` is `<start len>` in **local** indices into this half's own strip, not
into the whole-board space, so the same declaration suits both halves of a
split. A channel's id is its position in the devicetree, and that id is what
the behavior's `VFX_*_ON()` macros address: `glow` above is 0, `keys` is 1.

Scenes are rendered whole and then masked to the channel that asked for them,
which is why the shipped presets — every one of them written against the full
strip — work unchanged on a channel that owns six pixels of it. Declaring no
channel at all leaves one covering everything, which is what a board that has
never heard of channels keeps doing.

**Both halves of a split must declare the same channels carrying the same
scenes in the same order.** Next and previous are resolved to an absolute
index on the central and relayed as a number, so a half that knows fewer
scenes silently clamps what the other half can ever reach.

### A scene per layer, and fading between them

On most keyboards changing layer changes one indicator strip. `layer-scenes`
changes the whole lighting:

```dts
&vfx_engine {
    layer-scenes = <&vfx_warm &vfx_matrix &vfx_status>;
    transition-ms = <400>;
};
```

Indexed by layer number; layers past the end of the list leave whatever is
showing alone, so you can map only the ones you care about. It follows the
active layer and is deliberately **not** persisted — saving it would overwrite
the scene you picked with `&vfx`. On a split it works on the central, which is
the half with a keymap.

`transition-ms` crossfades on any scene change, including one you make
yourself. Both scenes render for the duration, so it costs one extra frame's
work and one extra frame buffer while a fade runs, and nothing when one isn't.
The mix happens after gamma, since a crossfade is a statement about what the
eye sees and those are already the values the eye is going to get.

The simulator does not show the crossfade: it holds one scene at a time, and
two would mean a second copy of every generator's storage. Everything else on
this page it does show.

### What they look like

Pointed across the board with `axis`, one generator covers the whole
directional family:

| | |
|---|---|
| ![Pinwheel](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/pinwheel.gif) | ![Spiral](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/spiral.gif) |
| `gradient`, `axis = <VFX_AXIS_ANGLE>` | `gradient`, `axis = <VFX_AXIS_SPIRAL>` |
| ![Out and in](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/out-and-in.gif) | ![Beacons](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/beacons.gif) |
| `wave`, `axis = <VFX_AXIS_RADIAL>` | `comet`, two of them along x |

Ambient effects, all of them a pure function of the clock so both halves of a
split agree with nothing exchanged:

| | |
|---|---|
| ![Fire](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/fire.gif) | ![Plasma](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/plasma.gif) |
| `fire` | `plasma`, summed over both board axes |
| ![Matrix](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/matrix.gif) | ![Water](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/water.gif) |
| `matrix` | `water` |

Reactive effects. The two on the right have no ambient source at all, so the
board is dark between keystrokes and the power gate cuts the rail:

| | |
|---|---|
| ![Crosshair](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/crosshair.gif) | ![Nexus](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/nexus.gif) |
| `cross` over a dim bed | `cross` with a `radius` |
| ![Matrix, typing only](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/matrix-typing.gif) | ![Water, typing only](https://pr0dt0s.github.io/zmk-advanced-underglow-effects-vfx/img/water-typing.gif) |
| `matrix`, `drop-rate-ms = <0>` | `water`, still water at brightness 0 |

### Pointing an effect across the board

`gradient` and `wave` take an `axis`, which is what turns one generator into
the whole family of directional effects other boards ship separately:

| `axis` | What it does |
|---|---|
| `VFX_AXIS_STRIP` | Along the wire. The default, and the only one that works without a position map |
| `VFX_AXIS_X` | Left to right across the board |
| `VFX_AXIS_Y` | Top to bottom |
| `VFX_AXIS_RADIAL` | Out from the middle — a ring that expands, or a radial gradient |
| `VFX_AXIS_ANGLE` | Around the middle: a pinwheel |
| `VFX_AXIS_SPIRAL` | Around and outward at once |

```dts
pinwheel {
    compatible = "zmk,vfx-layer-gradient";
    zone = <&all>;
    axis = <VFX_AXIS_ANGLE>;
    stops = <VFX_HSB(0,100,70) VFX_HSB(120,100,70) VFX_HSB(240,100,70)>;
    scroll-speed = <40>;
};
```

`span` and `wavelength` are in that axis's own units — pixels along the strip,
`pixel-positions` units for X, Y and RADIAL, and 256ths of a turn for ANGLE and
SPIRAL. Leaving either at `0` means "one cycle across the board" whichever way
the effect is pointing, so you rarely have to work the number out.

`plasma` is 2-D whenever a map exists: it sums sines over both board axes,
which is what makes the cells drift around each other rather than sliding
along in step.

### Effects that radiate need to know where the LEDs are

By default the engine's only notion of position is a pixel's index on the
strip, because that is genuinely all it can know. For gradients, waves and the
indicators that is the right model. For anything that radiates it is not: on a
serpentine strip the LEDs either side of index 14 sit at opposite ends of the
board, so a ripple "expanding" by three lights two scattered pixels rather
than a ring.

`pixel-positions` on the engine node fixes that — x,y for every pixel across
the whole board. `water`, `ripple`, `keyflash` and `trail` then measure real
distance, and `matrix` gets an axis to fall along and columns to fall in:

```dts
#include <vfx/lily58-positions.dtsi>

&vfx_engine {
    pixel-positions = <VFX_LILY58_POSITIONS>;
};
```

That map is **derived, not measured**: it assumes 36 LEDs snaked under each
half's key field in a 6x6 grid, generated by `sim/derive-positions.py`. If
ripples come out the wrong shape on your board, replace it with your own
pairs — it is just a list.

`<vfx/pandakb-lily58.dtsi>` is the other shipped map, for the PandaKB Lily58
RGB MX, and it is worth reading as an example of how far a real board can sit
from the generic guess: 35 LEDs a side rather than 36, six downward-facing
underglow ones chained **before** the 29 per-key ones, and a serpentine that
turns round every row. It brings a `key-pixels` list as well as positions.

Nothing in the firmware can work any of that out, so if your board is not one
of these two, expect to measure it. The quickest way is a throwaway scene per
pixel — one `solid` layer on a `range = <n 1>` zone — stepped with `VFX_NEXT`
while you write down which LED lights.

Distances in effect properties are in whatever units the map uses. The shipped
map puts about 10 units between adjacent LEDs, which leaves sub-LED resolution
so wavefronts move smoothly, and the defaults assume it. Without a map,
distances are strip indices, so those numbers want to be roughly ten times
smaller.

Reactive generators also need `key-pixels` to know which LED a key sits
nearest. Without it, key positions are spread evenly over the board: still
animated, but not lined up with the keys.

## The `&vfx` behavior

`VFX_TOG`, `VFX_ON`, `VFX_OFF`, `VFX_NEXT`, `VFX_PREV`, `VFX_SEL(n)`,
`VFX_BRI`, `VFX_BRD`, `VFX_SPI`, `VFX_SPD`, `VFX_HUI`, `VFX_HUD`.

Relative commands are rewritten to absolute ones on the central before being
relayed, so both halves land on the same value rather than each applying its
own increment to its own starting point.

Every one has an `_ON(ch)` form that addresses a single channel —
`VFX_NEXT_ON(1)`, `VFX_BRI_ON(0)`, `VFX_SEL_ON(1, 4)` — while the plain forms
above mean every channel, which is what they did before channels existed.

The channel rides in `param1` above the command, because `param2` is already
spoken for by the commands that carry a value.

A `mod-morph` is a tidy way to drive two channels without doubling the keys,
since one key can then mean the per-key LEDs alone and the underglow with a
modifier held:

```dts
vfx_next_ch: vfx_next_ch {
    compatible = "zmk,behavior-mod-morph";
    #binding-cells = <0>;
    bindings = <&vfx VFX_NEXT_ON(1)>, <&vfx VFX_NEXT_ON(0)>;
    mods = <MOD_RSFT>;
};
```

`&rgb_ug` keeps working through the compatibility shim. Saturation is the one
command with no equivalent, because VFX sets saturation per layer in
devicetree rather than globally; it is accepted and ignored.

## Power gating

WS2812s draw close to 1 mA each even while displaying black, so a 36 pixel
strip sits around 32 mA whether or not anything is lit, against roughly 15 µA
for an idle nRF52840. No amount of dimming touches that: only cutting the rail
does.

With `CONFIG_ZMK_VFX_AUTO_POWER_GATE=y`, a frame that renders entirely black
for `CONFIG_ZMK_VFX_BLACKOUT_DELAY_MS` switches the external power rail off.
The engine keeps rendering while gated, which is nearly free without the bus
transfer, so the first lit frame brings the rail back.

Details that matter on hardware:

- a final black frame goes out before the supply is cut, or the LEDs hold
  their last colour until the rail decays;
- the bus is suspended first, parking the data line low, because a pin left
  high into an unpowered strip leaks through its protection diode;
- the first frame after waking waits out `CONFIG_ZMK_VFX_POWER_SETTLE_MS`,
  defaulting to the 50 ms the nice_nano uses for this rail.

**Two things to check on your board.** The gated rail usually feeds everything
external, not just the LEDs, so a display sharing it will switch off too. And
if your strip is wired to `RAW` rather than the switched `VCC`, gating will do
nothing at all — worth a multimeter before relying on it.

## Split keyboards

Effects are positioned in a whole-board coordinate space, so each half renders
its own slice and animations line up across the seam.

**Free-running** (default) costs no radio traffic: generators derive their
output from time and position alone, so two halves agreeing on the clock
produce identical frames. What they do not share is a clock, and two crystals
drift apart over hours.

**Synchronised** (`CONFIG_ZMK_VFX_SPLIT_SYNCED=y`) has the central beacon its
uptime, and relay key positions so a ripple crosses the seam. It uses ZMK's
existing behavior relay rather than a GATT service of its own. Corrections are
eased in under a frame at a time rather than stepped, which would tear
whatever is animating.

#### Which half hears which keys

Free-running is often described as "each half only reacts to its own keys",
which is half right and misleading in a way worth spelling out, because it
sends people hunting for a bug that is not there.

The two halves do not start level. ZMK has to hand the central every
peripheral press in order to run the keymap, and the engine reacts to every
key event it sees rather than filtering by source. So:

| Pressed on | Central draws it | Peripheral draws it |
|---|---|---|
| the central | yes, locally | **no**, until something relays it |
| the peripheral | yes, ZMK delivered it | yes, locally |

Relaying only has to go one way, and that is exactly what
`CONFIG_ZMK_VFX_SYNC_RELAY_KEYS` (on by default under synced mode) does: the
central forwards its own local presses, and skips anything that arrived from a
peripheral, which would otherwise be drawn there twice.

The practical upshot when testing: a press on the **peripheral** half already
appears on both, even free-running. It is a press on the **central** that goes
nowhere until you switch synced mode on.

Note that "position" means position **along the strip**, not physical location:
a serpentine strip makes a gradient snake rather than sweep. The simulator lets
you pick the wiring so you can see what yours will do.

## Adjusting a layer while the keyboard runs

Everything on a layer is fixed in flash by devicetree and cannot be written
to. Give one a `tune-id` and three things about it become adjustable at
runtime — hue, level and speed:

```dts
wash {
    compatible = "zmk,vfx-layer-gradient";
    zone = <&vfx_all>;
    stops = <VFX_HSB(200, 100, 40) VFX_HSB(280, 100, 60)>;
    tune-id = <1>;
};
```

```dts
&vfx VFX_TUNE_LEVEL(1, 80)    // dim whatever is on slot 1
&vfx VFX_TUNE_HUE(1, 120)     // rotate its colours
&vfx VFX_TUNE_RESET(1)        // back to what devicetree said
```

Layers sharing an id move together, so a scene built from several of them
dims as one thing. Slots run 1 to 7; 0 means a layer never opted in. The
values persist.

Those three were not chosen for being easy. They are the ones that ride paths
the compositor already had — hue and speed by handing the layer its own frame
context, level by folding into the opacity it was going to be blended with —
so **no generator knows this exists and tuning costs nothing per pixel**. The
alternative, moving every generator's configuration from flash into RAM to
make any field writable, costs memory on every board whether or not anything
is ever adjusted.

### What this is not

This is not runtime scene authoring. You cannot add a layer, change a
generator or repoint a zone on a running keyboard: those live in flash, built
by devicetree at compile time, and reaching them means a different data model
(a RAM scene representation, bounded pools, serialisation) rather than a
different transport. The composer in the simulator is the answer to that for
now — design there, paste the devicetree, flash once.

Nor does anything here talk to a host yet. `zmk_vfx_tune_*()` is the API a
transport would call; see the roadmap.

## Roadmap

Honest about what is missing rather than implied by the rest of this file.

**Host control.** The tuning API above is deliberately transport-independent,
because the transport is a live question. ZMK Studio's RPC supports custom
subsystems a module can register, which is the idiomatic route, but that
support is not upstream: the modules using it pin a forked ZMK at
`main+custom-studio-protocol`. `zzeneg/zmk-raw-hid` needs no fork and is
bidirectional over USB and BLE, which makes it the pragmatic first target. A
UI would then drive `zmk_vfx_tune_*()`, and the simulator's composer is most
of the interface already.

**Runtime scene authoring.** Adding and removing layers on a running board,
as above. Wants a RAM scene model with bounded layer and config pools, and a
way to persist what was built. Much larger than tuning, and worth doing only
once tuning has shown the transport works.

**Caps word.** ZMK exposes neither a state accessor nor an event for it, so
the indicator cannot be driven. Nothing to do here until that changes
upstream.

**More measured board maps.** `pandakb-lily58.dtsi` has the thumb cluster
order and the underglow chain order still inferred rather than measured. Rows
6-23 were read off hardware; the rest follows from a community table and the
serpentine, which is good evidence but not the same thing.

Both are a pixel sweep away for anyone with the hardware in front of them,
and the sweep is worth keeping as a pattern: a scene per pixel, each a
`zmk,vfx-layer-solid` over a `range = <N 1>` zone, all of them appended to
one channel's scene list so that stepping that channel walks the pixels one
at a time. `Pr0dt0s/zmk-config` carries a working copy as
`config/vfx-probe.dtsi`. The trap is that relative commands resolve to an
absolute index on the central and relay as a bare number, so a probe file
included on only one half of a split silently cannot be reached.

## Licence

MIT.
