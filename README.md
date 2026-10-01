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

Every channel-shaped array in this module — the engine's own per-channel
state, and, with `CONFIG_ZMK_VFX_RUNTIME_SCENES`, each channel's runtime
scene pool — is sized to exactly how many channels this board declared, not
to some fixed ceiling: a two-channel board pays RAM for two, a board with
six pays for six, and there is no upper limit to declare past. That size is
baked into the saved settings blobs, so changing how many channels a board
declares changes their layout and resets them to defaults on the next boot,
the same as any other change that resizes a saved struct already does.

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

Tuning is not scene authoring. It cannot add a layer, change a generator or
repoint a zone: those live in flash, built by devicetree at compile time, and
making them writable means a different data model (a RAM scene
representation, bounded pools, serialisation) rather than a different
transport. That model exists, behind its own option, and is
[Runtime scene authoring](#runtime-scene-authoring) below; this section is
only about moving what devicetree already built.

`zmk_vfx_tune_*()` is also the API a host transport calls, which is what the
next section is about.

## Host control

Tuning is transport-independent on purpose, because the transport is a live
question — ZMK Studio's RPC supports custom subsystems a module can
register, which would be the idiomatic route, but that support is not
upstream: the modules using it pin a forked ZMK at
`main+custom-studio-protocol`. `zzeneg/zmk-raw-hid` needs no fork and is
bidirectional over USB and BLE, so it is what `src/hid_transport.c` decodes
into `zmk_vfx_tune_*()` calls, gated behind `CONFIG_ZMK_VFX_RAW_HID`.

```yaml
manifest:
  remotes:
    - name: zmkfirmware
      url-base: https://github.com/zmkfirmware
    - name: pr0dt0s
      url-base: https://github.com/pr0dt0s
    - name: zzeneg
      url-base: https://github.com/zzeneg
  projects:
    - name: zmk
      remote: zmkfirmware
      revision: main
      import: app/west.yml
    - name: zmk-advanced-underglow-effects-vfx
      remote: pr0dt0s
      revision: main
    - name: zmk-raw-hid
      remote: zzeneg
      revision: main
  self:
    path: config
```

Add `raw_hid_adapter` as an additional shield in `build.yaml`, same as
`zmk-raw-hid`'s own README describes, and turn the transport on:

```ini
CONFIG_ZMK_VFX_RAW_HID=y
```

RAW_HID only builds on the split's central, same restriction
`CONFIG_ZMK_WPM` has, so this goes in the central half's `.conf` on a split
rather than the shared one.

### The wire format

A request is `[op, ...]`; a reply is always `[op | 0x80, ...]`, which is
what lets a host recognise a reply without inspecting anything past the
first byte. Every multi-byte field is little-endian. `hid_protocol.h` is the
source of truth; this table is for a host tool that is not `host.js` below.

| Request | Bytes | Reply | Bytes |
|---|---|---|---|
| `PING` (`0x00`) | — | `0x80`: version, max slot | 2 |
| `SET_HUE` (`0x01`) | slot, hue (`int16`) | `0x81`: slot, status | 2 |
| `SET_LEVEL` (`0x02`) | slot, level | `0x82`: slot, status | 2 |
| `SET_SPEED` (`0x03`) | slot, speed | `0x83`: slot, status | 2 |
| `RESET` (`0x04`) | slot | `0x84`: slot, status | 2 |
| `GET` (`0x05`) | slot | `0x85`: slot, hue, level, speed, status | 5 |
| `GET_ALL` (`0x06`) | — | one `0x85` per slot, 1 through the max `PING` reported | 5 each |

`status` is `0` for ok, `1` for a slot outside 1 to `VFX_TUNE_SLOTS - 1`
(slot 0 means "untuned" and is never itself addressable). `hue` is degrees,
`level` is 0-255, `speed` is 1-5 or 0 to stop overriding and follow the
channel again — the same ranges `VFX_TUNE_HUE`/`_LEVEL`/`_SPEED` take from a
keymap.

### The simulator's own panel

The **Host control** panel at the bottom of the simulator page
(`sim/web/host.js`) is a small WebHID client speaking the format above. It
is the one part of that page that is not the compositor: everywhere else,
what you see is a wasm build of the real C; here, connecting drives an
actual keyboard over USB or BLE, and the panel is a JavaScript
reimplementation of the wire format rather than a build of anything that
defines it — the same relationship `toDevicetree()` has to devicetree
syntax. Needs Chrome or Edge.

**None of this has been tried against real hardware.** `tests/` proves the
wire format encodes and decodes correctly with no Zephyr in scope, and
`tools/verify-host-hid.mjs` proves `host.js` drives that format correctly
against a fake HID device built to the same spec. Neither touches USB
descriptors, BLE HID-over-GATT, or a real host OS's HID stack, which is
where a transport like this actually tends to break. If you try it on a
board, especially over BLE where `zmk-raw-hid` requires
`BT_SECURITY_L2`, an issue report is worth more than the code.

## Runtime scene authoring

[Adjusting a layer while the keyboard runs](#adjusting-a-layer-while-the-keyboard-runs)
moves a slider on a layer devicetree already built. This is the other half:
building a channel's scene itself at runtime, over the same transport —
adding, editing, reordering and removing layers on a running board rather
than only in a `.keymap`.

```ini
CONFIG_ZMK_VFX_RAW_HID=y
CONFIG_ZMK_VFX_RUNTIME_SCENES=y
```

It reuses the WebAssembly simulator's own trick for building a scene without
an allocator — `sim/vfx_sim.c` writes into a fixed arena instead of reading
flash-const structs, and `src/runtime_scene.c` does the same into a bounded
per-channel pool of RAM instead. `zmk_vfx_scene_*()` (declared in `vfx.h`,
implemented over `runtime_scene.h`) is the API this builds on, one channel's
pool at a time; devicetree's compiled scene list stays exactly as it was,
and a channel switches to showing this instead of it only once something has
actually been built and activated.

**Every generator is buildable this way**, all 24 of them: `solid`,
`breathe`, `wave`, `twinkle`, `plasma`, `ripple`, `keyflash`, `pulse`, `dart`,
`static`, `gradient`, `trail`, `hold`, `water`, `matrix`, `fire`, `comet`,
`cross`, `layer-state`, `battery`, `ble-profile`, `flag`, `wpm` and
`peripheral-battery`. A 32-byte HID report cannot carry a `water` layer's
whole configuration (two colours and six numbers), let alone a
`layer-state`'s colour per keymap layer, so nothing tries to: a layer is
**assembled from several small messages**, the way a scene itself is
assembled from many layers. `SCENE_ADD_LAYER` creates it with its first
colour and first four numbers; further messages set the fifth and sixth
numbers, the rest of the colour list, the zone and the layer's options. See
[Building a layer](#building-a-layer) for the recipe and for which colour
and which number feeds which generator.

A layer's zone is any of the three forms devicetree has: a plain range
(`range = <start len>`), an explicit list of strip indices (`pixels = <...>`),
or a list of key positions (`keys = <...>`, resolved to pixels through the
same key map a compiled `keys` zone uses). The two lists hold at most
`CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS` entries and are sent twelve at a
time.

What a channel can hold at once is bounded by four Kconfig options, each
sized per channel:

| Option | Default | What it bounds |
|---|---|---|
| `CONFIG_ZMK_VFX_RUNTIME_MAX_LAYERS` | 6 (1–16) | Layers in the pool. |
| `CONFIG_ZMK_VFX_RUNTIME_MAX_COLORS` | 8 (3–16) | Entries in a layer's colour list: a gradient's stops, a layer-state layer's colours, the second colour of the generators that draw with two. At least 3, which is what `peripheral-battery` needs. |
| `CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS` | 32 (4–128) | Entries in a `pixels` or `keys` zone. |
| `CONFIG_ZMK_VFX_RUNTIME_HEAVY_STATES` | 2 (1–8) | `trail` and `hold` layers at once. Each keeps a byte of animation state per pixel, which every other layer would waste if each slot carried it, so they draw from this small pool instead. Adding one when it is empty is refused with `POOL_FULL`, the answer a full layer pool gives. |

The colour list, the zone list and the layer count are persisted with every
layer, so together they size the saved blob (`vfx/rt`), which has to fit one
flash sector. Raising all of them at once can stop that from fitting; the
option help in `Kconfig` says the same.

### The wire format

Eighteen more ops on the same transport as tuning, all gated behind
`CONFIG_ZMK_VFX_RUNTIME_SCENES` in addition to `CONFIG_ZMK_VFX_RAW_HID` — a
board running raw-hid without runtime scenes still answers every one of
these, with `status` `1` and nothing built, rather than leaving a host
waiting on a reply that never comes. `ch` is a channel index; unlike a
tuning slot, a runtime scene belongs to one channel, so there is no "every
channel" form of any of these. `slot` here is a layer's id within that
channel's own pool — a different id space from a tuning slot, assigned by
`SCENE_ADD_LAYER`'s own reply.

| Request | Bytes | Reply | Bytes |
|---|---|---|---|
| `SCENE_RESET` (`0x07`) | ch | `0x87`: ch, status | 3 |
| `SCENE_ADD_LAYER` (`0x08`) | ch, type, zone start, zone len, blend, opacity, hue (`int16`), sat, bri, 4 args (`int16` each), flags | `0x88`: slot (or `0xFF`), status | 3 |
| `SCENE_SET_ARG` (`0x09`) | ch, slot, arg index, value (`int16`) | `0x89`: slot, status | 3 |
| `SCENE_SET_COLOR` (`0x0A`) | ch, slot, hue (`int16`), sat, bri | `0x8A`: slot, status | 3 |
| `SCENE_REMOVE_LAYER` (`0x0B`) | ch, slot | `0x8B`: slot, status | 3 |
| `SCENE_MOVE_LAYER` (`0x0C`) | ch, slot, direction (`int8`, ±1) | `0x8C`: slot, status | 3 |
| `SCENE_ACTIVATE` (`0x0D`) | ch | `0x8D`: ch, status | 3 |
| `SCENE_DEACTIVATE` (`0x0E`) | ch | `0x8E`: ch, status | 3 |
| `SCENE_GET_INFO` (`0x0F`) | ch | `0x8F`: ch, count, active, status | 5 |
| `SCENE_GET_LAYER` (`0x10`) | ch, slot | `0x90`: ch, slot, type, zone start, zone len, blend, opacity, hue, sat, bri, 4 args, flags, status | 22 |
| `SCENE_GET_ORDER` (`0x11`) | ch | `0x91`: ch, count, `count` slot ids, status | 4 + count |
| `SCENE_GRADIENT_ADD_STOP` (`0x12`) | ch, slot, hue (`int16`), sat, bri | `0x92`: slot, status | 3 |
| `SCENE_GET_GRADIENT_STOP` (`0x13`) | ch, slot, stop index | `0x93`: ch, slot, index, hue (`int16`), sat, bri, status | 9 |
| `SCENE_SET_LIST_COLOR` (`0x14`) | ch, slot, index, hue (`int16`), sat, bri | `0x94`: slot, status | 3 |
| `SCENE_GET_LIST_COLOR` (`0x15`) | ch, slot, index | `0x95`: ch, slot, index, hue (`int16`), sat, bri, status | 9 |
| `SCENE_SET_ZONE` (`0x16`) | ch, slot, kind, offset, count, 12 data bytes | `0x96`: slot, status | 3 |
| `SCENE_GET_ZONE` (`0x17`) | ch, slot, offset | `0x97`: ch, slot, kind, total, offset, n, 12 data bytes, status | 20 |
| `SCENE_SET_OPTS` (`0x18`) | ch, slot, blend, opacity, opacity source, opacity min, opacity full, tune id | `0x98`: slot, status | 3 |
| `SCENE_GET_LAYER_EXT` (`0x19`) | ch, slot | `0x99`: ch, slot, zone kind, zone count, colour count, opacity source, opacity min, opacity full, tune id, arg 4 (`int16`), arg 5 (`int16`), status | 15 |

`SCENE_SET_LIST_COLOR` and `SCENE_GET_LIST_COLOR` work on any generator's
colour list; `SCENE_GRADIENT_ADD_STOP` and `SCENE_GET_GRADIENT_STOP` are the
same two operations from before there was a general list (the first appends
rather than naming an index) and are still answered, but nothing new needs
them. `SCENE_SET_ZONE`'s `kind` is `0` for a range, `1` for a pixel list and
`2` for a key list; a range's data is `start`, `len`, and a list's data is
up to twelve entries of it, `count` saying how many of the twelve bytes are
real. `SCENE_GET_ZONE` answers the same way: `total` is the number of entries
the zone holds (2 for a range, which reads back `start`, `len`), and a host
reads a list by calling again at `offset + n` until that reaches `total`.
`SCENE_GET_LAYER_EXT` is everything `SCENE_GET_LAYER` has no room left for:
the zone's kind and length, how many colours are in the list, the layer's
options, and the fifth and sixth numbers.

`status` is `0` for ok, `1` (`BAD_SLOT`) for a channel, slot, generator
type, argument index, colour index, zone kind, blend or opacity source
outside range (or a zone chunk whose `offset` is not the number of entries
the list already holds), and `2` (`POOL_FULL`) for anything that ran out of
room rather than being wrong: `SCENE_ADD_LAYER` when the channel's layer
pool is full or a `trail` or `hold` cannot get one of its heavy states;
`SCENE_SET_LIST_COLOR` and `SCENE_GRADIENT_ADD_STOP` when the index is past
`CONFIG_ZMK_VFX_RUNTIME_MAX_COLORS`; `SCENE_SET_ZONE` when a list would grow
past `CONFIG_ZMK_VFX_RUNTIME_MAX_ZONE_PIXELS`. `POOL_FULL` is broken out from
`BAD_SLOT` because it is the one rejection a host would react to differently,
by removing something rather than by fixing what it sent.

Setting a colour past the end of the list grows it, and any entries skipped
over are black, which every generator that takes an optional colour already
reads as "unset". Setting a chunk of a zone at `offset` `0` starts the list
over, and may change its kind; any later chunk has to continue the same list.

`flags` is one byte: bit 0 is `pulse`'s `stack`, bit 1 is `dart`'s `reverse`,
and bits 2-4 are the `axis` (`VFX_AXIS_*`, the same values
`dt-bindings/zmk/vfx.h` defines) of every generator that has one — `wave`,
`dart`, `gradient`, `fire` and `comet`. Only `SCENE_ADD_LAYER` carries it, so
changing a layer's flags means building it again (see
[The simulator's own panel](#the-simulators-own-panel)); everything else about
a layer is edited in place.

### Cycling in the runtime scene

`SCENE_ACTIVATE` / `SCENE_DEACTIVATE` (and `zmk_vfx_runtime_set_active()` for
an in-firmware caller) are not the only way onto a channel's runtime scene.
Once it holds at least one layer, `&vfx VFX_NEXT` / `VFX_PREV` — the same
behavior the keymap already uses to step through the compiled scene list —
treats it as one more entry, appended after the last compiled scene. Cycling
forward from the last compiled scene lands on it; cycling again leaves it and
wraps to the first compiled scene, same as any other step. An empty runtime
scene (nothing built, or reset back to nothing) is skipped rather than
cycled into, so NEXT never lands on a frame that renders black.

Picking a compiled scene directly — `zmk_vfx_select_scene()` with an index
inside the compiled list, which is also what the rest of `&vfx`'s bindings
resolve to — still gives up the runtime scene explicitly, the same as before:
a scene picked by name has to actually show, not leave a runtime scene stuck
on screen underneath it.

### Building a layer

A layer is built in steps, each one message, in this order:

1. **`SCENE_ADD_LAYER`** creates it: `type`, a zone range, blend and opacity,
   the primary colour (`hue`/`sat`/`bri`), the first four numbers and the
   flags byte. The reply carries the `slot` every later message names. This
   is the message that can fail for want of a layer slot or, for `trail` and
   `hold`, a heavy state.
2. **`SCENE_SET_ARG`** for the fifth and sixth numbers (index `4` and `5`),
   for the generators that use them: `water` and `matrix`, six each. It
   reaches all six, so it is also how any number is edited afterwards.
3. **`SCENE_SET_LIST_COLOR`**, once per entry of the colour list, in index
   order, for every generator whose table row below has one. The list grows
   to whatever index was set last.
4. **`SCENE_SET_ZONE`**, only when the zone is a pixel or key list rather
   than the range `SCENE_ADD_LAYER` already set: one message per twelve
   entries, each at an `offset` equal to the number of entries already sent.
5. **`SCENE_SET_OPTS`**, only when blend, opacity, the opacity source or the
   tuning id need to be something other than what step 1 set.

Every step after the first is an edit of a layer that already exists, and
works exactly the same on one built long ago: nothing has to be removed and
re-added to change a colour, a number, a list entry, a zone or an option,
and changing an option does not restart anything that is animating. (An edit
to a colour, a number or the zone does restart that one layer's own
animation, since what it was mid-way through was drawn under the old
configuration.) Only the flags byte has no message of its own.

Which colour and which number feeds which field is fixed per generator, in
the order `rebuild_slot()` in `runtime_scene.c` reads them. A generator that
draws with two colours takes the second from the first entry of the list:

| Generator | Primary colour | Colour list | Numbers, `SCENE_SET_ARG` index `0`… | Flags |
|---|---|---|---|---|
| `solid` | colour | | | |
| `breathe` | colour | | period ms, min level, hue swing | |
| `wave` | colour | | wavelength, period ms, depth | axis |
| `twinkle` | colour | | period ms, density, hue spread | |
| `plasma` | colour | | scale, period ms, hue spread | |
| `ripple` | colour | | decay ms, speed, width | |
| `keyflash` | colour | | decay ms, spread | |
| `pulse` | colour | | decay ms, min level, hue step | stack |
| `dart` | tail colour | `0` head colour | speed, lifetime ms, tail | axis, reverse |
| `static` | colour | | period ms, density, hue spread | |
| `gradient` | unused | the stops, in order | scroll speed, span | axis |
| `trail` | colour | | decay ms, spread | |
| `hold` | colour | | release ms | |
| `water` | surface colour | `0` crest colour | wavelength, speed, lifetime ms, drop rate ms, amplitude, damping | |
| `matrix` | tail colour | `0` head colour | speed, tail, drop rate ms, columns, jitter, head size | |
| `fire` | base colour | `0` tip colour | period ms, cell, height, flicker | axis |
| `comet` | tail colour | `0` head colour | period ms, tail, count | axis |
| `cross` | arm colour | `0` centre colour | decay ms, radius, thickness, axes (`0` both, `1` horizontal, `2` vertical) | |
| `layer-state` | unused | one colour per keymap layer, layer 0 first | | |
| `battery` | low colour | `0` high, `1` empty | warn below % | |
| `ble-profile` | connected colour | `0` disconnected, `1` USB | | |
| `flag` | colour | | source (`0` locks, `1` modifiers), mask | |
| `wpm` | idle colour | `0` fast colour | full wpm, mode (`0` colour, `1` bar) | |
| `peripheral-battery` | low colour | `0` high, `1` empty, `2` unknown | peripheral, warn below % | |

A list entry that was never set is black, which each generator reads as "not
given". A `layer-state` layer's black entries draw nothing, so the layers
beneath them show through.

On a channel whose runtime scene is active, a layer is visible from the
moment `SCENE_ADD_LAYER` is applied, not from the moment the last message
lands, so a layer with a second colour or a zone list shows briefly
half-configured — second colour black, or over the range zone
`SCENE_ADD_LAYER` set — while the rest arrives. Building a scene while it is
not active, and activating it last, avoids that.

`SCENE_GET_LAYER` answers one slot at a time and says nothing about where
that slot renders relative to the rest, since add, remove and move never
renumber a slot, only its position in render order. `SCENE_GET_ORDER` is
that position: the channel's own slot ids, bottom of the stack first, the
same sense `SCENE_MOVE_LAYER`'s direction argument uses. A host discovers a
channel's layers by reading this once and then, for each id it names,
`SCENE_GET_LAYER` (type, range zone, blend, opacity, colour, first four
numbers, flags), `SCENE_GET_LAYER_EXT` (zone kind and length, colour count,
options, fifth and sixth numbers), `SCENE_GET_LIST_COLOR` once per colour and
`SCENE_GET_ZONE` in chunks for a list zone — which is what `host.js` does,
so a channel selected fresh (including right after a reconnect) shows the
real render order and the whole of every layer, not just whatever a blind
slot probe happened to find. A gradient's `SCENE_GET_LAYER` still reports its
stop count in `args[2]`, a number it never otherwise uses.

### Persistence

Saved the same way tuning is: `vfx_runtime_state()` hands
`vfx_save_work_handler()` a snapshot to write to the `"vfx/rt"` settings key,
debounced the same 2 seconds after the last change as everything else this
module persists. What gets saved is deliberately not the live pool itself —
every slot's `zone`/`config`/`state`/`layer` are plain pointers into that
same pool, not something to trust after a reboot or a different build — only
each slot's params, the render order, the count and whether the channel is
active. Loading rebuilds every slot's pointers from those params before a
frame is ever composited from them.

### Split keyboards

raw-hid only builds on a split's central (see
[Adjusting a layer while the keyboard runs](#adjusting-a-layer-while-the-keyboard-runs)
above), so a scene built from a host would only ever show on whichever half
happens to be plugged in — unless something relays it. It does: once the
central has applied a scene op locally, `scene_relay.c` forwards the same
request's own wire bytes to every peripheral over the `&vfx` behavior's
relay, the mechanism [Split keyboards](#split-keyboards) already describes
for the sync beacon and key relay, and for the same reason (no GATT service
of its own). A behavior invocation carries two `uint32_t` — param1 and
param2 — not three: ZMK's own split transport narrows a relayed event's
position to a single byte before it reaches the peripheral, so only param1
(above a small header) and param2 carry payload, four bytes of a request's
own wire bytes per invocation. A request longer than that goes out in more
than one: `SCENE_ADD_LAYER` (20 bytes) and `SCENE_SET_ZONE` (18) take five
each, `SCENE_SET_OPTS` (9) three, `SCENE_SET_LIST_COLOR` (8) two. That 20-byte
ceiling (`VFX_RELAY_MAX_BYTES`) is why a layer is built from several small
messages to begin with rather than one big one, and why `SCENE_SET_ZONE`
carries twelve entries and no more: every op that changes a scene has to
fit, or it could be built on the central and never reach the other half.

The relay is fire-and-forget. The central answers the host as soon as it has
applied an op itself, and nothing reports back whether the peripheral has
applied the relayed copy. Building one layer this way is a dozen or so relayed
invocations (a `water` with a second colour is eleven), and a host that sends
the next message before the split link has drained the last may find the
two halves ending up with different scenes. `host.js` therefore waits sixteen
milliseconds per four-byte chunk before sending the next request after any
op that mutates a scene. **That figure is a guess, not a measurement**: there
has been no hardware to measure the link on. If the halves disagree after a
build, pace more slowly and file an issue with what you saw; the way back to
a known state is `SCENE_RESET` and building it again, more slowly.

The peripheral applies what it receives to a pool of its own, sized by its own
`CONFIG_ZMK_VFX_RUNTIME_*` options, so give both halves the same ones. An edit
that fits the central's `MAX_COLORS` or `MAX_ZONE_PIXELS` but not the
peripheral's is refused there without anyone being told.

This is independent of `CONFIG_ZMK_VFX_SPLIT_SYNCED`: that choice is about
whether the two halves' animation clocks agree, which has nothing to do
with whether a runtime-built scene reaches both of them. It relays on a
free-running board too.

The peripheral needs `CONFIG_ZMK_VFX_RUNTIME_SCENES=y` of its own to do
anything with what arrives — without it, a relayed edit lands and is
silently dropped, since there is no pool on that half to apply it to. It
does not need `CONFIG_ZMK_VFX_RAW_HID`: that transport is what lets a host
build a scene in the first place, not what lets a half hold one.

### The simulator's own panel

The **Scenes** part of the Host control panel drives all of this: pick a
channel's tab, add a layer of any of the 24 types, reorder or remove it, and
flip **Active** to show it instead of the channel's compiled list. Each
layer's card carries what its generator reads — the primary colour, each
number (a picker where the number is really a choice, like `flag`'s source or
`cross`'s axes), a colour picker per list entry with an add button where the
list can grow (a `gradient`'s stops, a `layer-state`'s colours), and, under
their own headings, the zone (a range, or a pixel or key list typed as
numbers) and the options (blend, opacity, the opacity source with its minimum
and full values, the tuning id). A new layer starts from values taken from one
of the module's own presets, so it looks like something rather than a guess.

Everything on a card is an edit in place, sent as the message the recipe
above names for it, **except** `pulse`'s stack, `dart`'s reverse and the axis
of the generators that have one. Those ride in the flags byte, which only
`SCENE_ADD_LAYER` carries, so changing one removes the layer, adds it again
from what the panel holds, and walks it back to its old place in the stack
with `SCENE_MOVE_LAYER`. The layer is briefly gone while that happens, its
animation restarts, and its slot id may change.

Adding a layer sends `SCENE_ADD_LAYER` and then the messages for whatever else
the type needs (see [Building a layer](#building-a-layer)), so a `water` or a
`layer-state` takes a moment to fill in. Selecting a channel reads every layer
back with `SCENE_GET_LAYER`, `SCENE_GET_LAYER_EXT`, `SCENE_GET_LIST_COLOR` and
`SCENE_GET_ZONE`, so what the panel shows is what the board holds, including
for a scene this page did not build. Adding a `trail` or `hold` to a channel
whose `CONFIG_ZMK_VFX_RUNTIME_HEAVY_STATES` are all in use is refused by the
board, and the panel says so.

Same caveat as tuning: `tests/` proves the wire format round-trips with no
Zephyr in scope, and `tools/verify-host-hid.mjs` now drives the Scenes panel
against a fake device too, but **none of it has been tried against a real
board.**

## Roadmap

Honest about what is missing rather than implied by the rest of this file.

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
