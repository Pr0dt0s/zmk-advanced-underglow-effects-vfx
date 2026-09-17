# ZMK Advanced Underglow Effects (VFX)

A ZMK module that replaces the built-in RGB underglow with a small compositor:
**scenes** are stacks of **layers**, each scoped to a **zone** of pixels and
combined with a **blend mode**, all described in devicetree.

It also cuts power to the LEDs whenever a frame renders entirely black, which
on a wireless keyboard is worth more than everything else here put together.

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

## The simulator

`sim/` compiles the real compositor — `render.c`, `color.c` and the generators,
unmodified — to WebAssembly and renders it under a Lily58 in the browser. No
effect is reimplemented in JavaScript, so a frame on the page is the frame the
keyboard produces.

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

`tests/` holds headless assertions for the parts eyeballing will not catch. It
builds the engine with no Zephyr in scope at all, which is what keeps the
simulator honest: if the compositor ever grows a kernel dependency, that build
breaks loudly instead of quietly diverging from the firmware.

```sh
cd tests && make test
```

## Scenes

Layers composite bottom to top in devicetree order.

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

### Generators

| Compatible | What it does |
|---|---|
| `zmk,vfx-layer-solid` | One colour across the zone |
| `zmk,vfx-layer-gradient` | Cyclic multi-stop gradient, optionally scrolling |
| `zmk,vfx-layer-breathe` | Pulses between a floor and the full colour |
| `zmk,vfx-layer-wave` | Travelling sine along the strip |
| `zmk,vfx-layer-twinkle` | Scattered pixels fading up and out |
| `zmk,vfx-layer-plasma` | Two summed sines swinging the hue |
| `zmk,vfx-layer-water` | A rippling surface, disturbed by rain and by typing |
| `zmk,vfx-layer-matrix` | Falling columns of light, started by rain and by typing |
| `zmk,vfx-layer-ripple` | Rings expanding from each pressed key |
| `zmk,vfx-layer-keyflash` | Lights the pressed key and fades in place |
| `zmk,vfx-layer-trail` | Decaying heat map that builds up as you type |
| `zmk,vfx-layer-layer-state` | Colour per active keymap layer |
| `zmk,vfx-layer-battery` | Fills a zone in proportion to charge |
| `zmk,vfx-layer-ble-profile` | One pixel per profile, lighting the selected one |

There is no caps word indicator: ZMK exposes neither a state accessor nor an
event for it, so one could not be driven on hardware.

On a split, the layer and BLE profile indicators only work on the **central**
half. ZMK compiles its keymap and BLE profile code for the central only, so a
peripheral has no layer or profile to report and those layers stay dark there.
Battery works on both, and shows each half's own cell.

Every generator takes `zone`, `blend` (`VFX_BLEND_NORMAL`, `_ADD`,
`_MULTIPLY`, `_SCREEN`, `_MAX`) and `opacity`. See `dts/bindings/` for each
one's own properties; `dts/vfx/presets.dtsi` has thirteen ready-made scenes.

`water` is both an ambient and a reactive effect. Each drop sends out a
decaying wavetrain, and overlapping drops sum as signed surface height before
being coloured, so they interfere rather than drawing as separate rings.
Ambient drops are derived from the clock, so both halves of a split agree on
where the rain falls without exchanging anything. Set `drop-rate-ms = <0>` for
a surface that only moves when you type. Still water at zero brightness draws
nothing at all, which is what lets the power gate cut the rail between
keypresses; any brightness above zero lights every pixel in the zone forever,
and the rail stays up.

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
drift apart over hours. Reactive effects see only their own half's keys.

**Synchronised** (`CONFIG_ZMK_VFX_SPLIT_SYNCED=y`) has the central beacon its
uptime, and optionally relay key positions so a ripple crosses the seam. It
uses ZMK's existing behavior relay rather than a GATT service of its own.
Corrections are eased in under a frame at a time rather than stepped, which
would tear whatever is animating.

Note that "position" means position **along the strip**, not physical location:
a serpentine strip makes a gradient snake rather than sweep. The simulator lets
you pick the wiring so you can see what yours will do.

## Licence

MIT.
