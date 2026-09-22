/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Headless assertions over the compositor. The browser simulator is for
 * eyeballing effects; this is for the things a human will not reliably catch.
 */

#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/runtime_scene.h>
#include <zmk/vfx/tuning.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/power.h>
#include <zmk/vfx/status.h>
#include <zmk/vfx/sync.h>

static int failures;

#define CHECK(cond, fmt, ...)                                                                      \
    do {                                                                                           \
        if (!(cond)) {                                                                             \
            printf("  FAIL %s:%d " fmt "\n", __func__, __LINE__, ##__VA_ARGS__);                   \
            failures++;                                                                            \
        }                                                                                          \
    } while (0)

/* ---- reference implementation, the float version ZMK core uses ---------- */

static void ref_hsb_to_rgb(int h, int s, int b, int *out) {
    float v = b / 100.0f, sat = s / 100.0f;
    int i = h / 60;
    float f = h / 360.0f * 6 - i;
    float p = v * (1 - sat), q = v * (1 - f * sat), t = v * (1 - (1 - f) * sat);
    float r = 0, g = 0, bl = 0;

    switch (i % 6) {
    case 0: r = v; g = t; bl = p; break;
    case 1: r = q; g = v; bl = p; break;
    case 2: r = p; g = v; bl = t; break;
    case 3: r = p; g = q; bl = v; break;
    case 4: r = t; g = p; bl = v; break;
    case 5: r = v; g = p; bl = q; break;
    }

    out[0] = (int)(r * 255);
    out[1] = (int)(g * 255);
    out[2] = (int)(bl * 255);
}

static void test_hsb_matches_float_reference(void) {
    int worst = 0;

    for (int h = 0; h < 360; h += 1) {
        for (int s = 0; s <= 100; s += 10) {
            for (int b = 0; b <= 100; b += 10) {
                struct vfx_rgb got = vfx_hsb_to_rgb((struct vfx_hsb){h, s, b});
                int ref[3];

                ref_hsb_to_rgb(h, s, b, ref);

                int d[3] = {abs(got.r - ref[0]), abs(got.g - ref[1]), abs(got.b - ref[2])};

                for (int c = 0; c < 3; c++) {
                    if (d[c] > worst) {
                        worst = d[c];
                    }
                }
            }
        }
    }

    printf("  hsb->rgb worst deviation from float reference: %d LSB\n", worst);
    CHECK(worst <= 3, "integer HSV drifted %d LSB from the float reference", worst);
}

static void test_hsb_edges(void) {
    struct vfx_rgb black = vfx_hsb_to_rgb((struct vfx_hsb){200, 100, 0});
    CHECK(black.r == 0 && black.g == 0 && black.b == 0, "zero brightness must be black");

    struct vfx_rgb white = vfx_hsb_to_rgb((struct vfx_hsb){0, 0, 100});
    CHECK(white.r == 255 && white.g == 255 && white.b == 255, "zero saturation must be white");

    /* Hue must wrap, not index off the end of the sector table. */
    struct vfx_rgb a = vfx_hsb_to_rgb((struct vfx_hsb){0, 100, 100});
    struct vfx_rgb b = vfx_hsb_to_rgb((struct vfx_hsb){360, 100, 100});
    CHECK(a.r == b.r && a.g == b.g && a.b == b.b, "hue 360 must equal hue 0");
}

static void test_hue_wrap(void) {
    CHECK(vfx_hue_add(350, 20) == 10, "hue must wrap forward past 359");
    CHECK(vfx_hue_add(10, -20) == 350, "hue must wrap backward past 0");
    CHECK(vfx_hue_add(0, 0) == 0, "identity");
}

static void test_lerp_takes_short_way(void) {
    /* 350 -> 10 should cross zero, landing near 0, not sweep down through 180. */
    struct vfx_hsb mid =
        vfx_hsb_lerp((struct vfx_hsb){350, 100, 100}, (struct vfx_hsb){10, 100, 100}, 128);

    CHECK(mid.h < 10 || mid.h > 350, "lerp took the long way around: h=%u", mid.h);
}

static void test_blend_identities(void) {
    struct vfx_rgb black = {0, 0, 0}, white = {255, 255, 255}, grey = {128, 128, 128};

    struct vfx_rgb r = vfx_blend(grey, black, VFX_BLEND_ADD, 255);
    CHECK(r.r == 128, "add black is identity, got %u", r.r);

    r = vfx_blend(grey, white, VFX_BLEND_ADD, 255);
    CHECK(r.r == 255, "add must saturate, got %u", r.r);

    r = vfx_blend(grey, white, VFX_BLEND_MULTIPLY, 255);
    CHECK(r.r == 128, "multiply by white is identity, got %u", r.r);

    r = vfx_blend(grey, black, VFX_BLEND_MULTIPLY, 255);
    CHECK(r.r == 0, "multiply by black is black, got %u", r.r);

    r = vfx_blend(grey, black, VFX_BLEND_SCREEN, 255);
    CHECK(r.r == 128, "screen with black is identity, got %u", r.r);

    r = vfx_blend(grey, white, VFX_BLEND_MAX, 255);
    CHECK(r.r == 255, "max picks the brighter, got %u", r.r);

    r = vfx_blend(grey, black, VFX_BLEND_MAX, 255);
    CHECK(r.r == 128, "max picks the brighter, got %u", r.r);

    /* Opacity 0 must leave the destination completely untouched. */
    r = vfx_blend(grey, white, VFX_BLEND_NORMAL, 0);
    CHECK(r.r == 128, "opacity 0 must be a no-op, got %u", r.r);

    r = vfx_blend(grey, white, VFX_BLEND_NORMAL, 255);
    CHECK(r.r == 255, "opacity 255 must fully replace, got %u", r.r);
}

static void test_gamma_monotonic(void) {
    CHECK(vfx_gamma(0) == 0, "gamma(0) must be 0");
    CHECK(vfx_gamma(255) == 255, "gamma(255) must be 255");

    for (int i = 1; i < 256; i++) {
        CHECK(vfx_gamma(i) >= vfx_gamma(i - 1), "gamma must not decrease at %d", i);
    }

    /* The whole point of the curve: mid input maps well below mid output. */
    CHECK(vfx_gamma(128) < 70, "gamma(128)=%u looks linear, not perceptual", vfx_gamma(128));
}

/* ---- compositor -------------------------------------------------------- */

#define NPX 36

static struct vfx_frame_ctx test_ctx(void) {
    return (struct vfx_frame_ctx){
        .time_ms = 0,
        .virtual_length = NPX,
        .strip_offset = 0,
        .num_pixels = NPX,
        .speed = 1,
        .brightness = 255,
        .hue_shift = 0,
    };
}

static void test_null_scene_is_black(void) {
    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = true;

    vfx_render_frame(NULL, &ctx, out, &lit);

    CHECK(!lit, "an empty scene must report unlit so the power gate can fire");

    for (int i = 0; i < NPX; i++) {
        CHECK(out[i].r == 0 && out[i].g == 0 && out[i].b == 0, "pixel %d not black", i);
    }
}

static void test_zone_masking(void) {
    /* A solid red layer confined to pixels 4..7 must not touch anything else. */
    static const struct vfx_zone zone = {.pixels = NULL, .start = 4, .len = 4};
    static const struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 100, 100)};
    static struct vfx_solid_state st;
    static const struct vfx_layer layers[] = {{
        .api = &vfx_layer_solid_api,
        .zone = &zone,
        .config = &cfg,
        .state = &st,
        .blend = VFX_BLEND_NORMAL,
        .opacity = 255,
    }};
    static const struct vfx_scene scene = {.name = "zone", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = false;

    vfx_render_frame(&scene, &ctx, out, &lit);

    CHECK(lit, "a red zone must count as lit");

    for (int i = 0; i < NPX; i++) {
        bool inside = (i >= 4 && i < 8);
        bool on = (out[i].r | out[i].g | out[i].b) != 0;

        CHECK(on == inside, "pixel %d: inside=%d but lit=%d", i, inside, on);
    }
}

static void test_zone_clamped_to_short_strip(void) {
    /* A zone declared past the end of the strip must be clamped, not overrun.
     * Devicetree cannot know the chain-length of the board it lands on.
     */
    static const struct vfx_zone zone = {.pixels = NULL, .start = 30, .len = 100};
    static const struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 0, 100)};
    static struct vfx_solid_state st;
    static const struct vfx_layer layers[] = {{
        .api = &vfx_layer_solid_api,
        .zone = &zone,
        .config = &cfg,
        .state = &st,
        .blend = VFX_BLEND_NORMAL,
        .opacity = 255,
    }};
    static const struct vfx_scene scene = {.name = "clamp", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX + 8];
    memset(out, 0xAA, sizeof(out));

    struct vfx_frame_ctx ctx = test_ctx();
    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = NPX; i < NPX + 8; i++) {
        CHECK(out[i].r == 0xAA, "compositor wrote past the end of the strip at %d", i);
    }
}

static void test_split_render_matches_whole_board(void) {
    /* The property that actually matters for the split: rendering the board as
     * two independent 36 pixel halves at offsets 0 and 36 must produce exactly
     * the same pixels as rendering one 72 pixel strip. Anything less and the
     * gradient visibly steps at the seam.
     */
    static const uint32_t stops[] = {VFX_HSB(0, 100, 100), VFX_HSB(120, 100, 100),
                                     VFX_HSB(240, 100, 100)};
    static const struct vfx_zone half_zone = {.pixels = NULL, .start = 0, .len = NPX};
    static const struct vfx_zone whole_zone = {.pixels = NULL, .start = 0, .len = NPX * 2};
    static const struct vfx_gradient_cfg cfg = {
        .stops = stops, .num_stops = 3, .scroll_speed = 7, .span = 0};
    static struct vfx_gradient_state stl, str, stw;

#define LAYER(zone_ptr, state_ptr)                                                                 \
    {                                                                                              \
        .api = &vfx_layer_gradient_api, .zone = (zone_ptr), .config = &cfg, .state = (state_ptr),  \
        .blend = VFX_BLEND_NORMAL, .opacity = 255                                                  \
    }

    static const struct vfx_layer left_layers[] = {LAYER(&half_zone, &stl)};
    static const struct vfx_layer right_layers[] = {LAYER(&half_zone, &str)};
    static const struct vfx_layer whole_layers[] = {LAYER(&whole_zone, &stw)};
#undef LAYER

    static const struct vfx_scene left = {.name = "l", .layers = left_layers, .num_layers = 1};
    static const struct vfx_scene right = {.name = "r", .layers = right_layers, .num_layers = 1};
    static const struct vfx_scene whole = {.name = "w", .layers = whole_layers, .num_layers = 1};

    struct vfx_rgb lout[NPX], rout[NPX], wout[NPX * 2];
    struct vfx_frame_ctx lctx = test_ctx(), rctx = test_ctx(), wctx = test_ctx();

    lctx.virtual_length = rctx.virtual_length = wctx.virtual_length = NPX * 2;
    rctx.strip_offset = NPX;
    wctx.num_pixels = NPX * 2;

    /* Check at several phases, so a scroll offset bug cannot hide at t=0. */
    const uint32_t times[] = {0, 137, 1000, 55555};

    for (unsigned k = 0; k < sizeof(times) / sizeof(times[0]); k++) {
        lctx.time_ms = rctx.time_ms = wctx.time_ms = times[k];

        vfx_render_frame(&left, &lctx, lout, NULL);
        vfx_render_frame(&right, &rctx, rout, NULL);
        vfx_render_frame(&whole, &wctx, wout, NULL);

        CHECK(memcmp(lout, wout, sizeof(lout)) == 0,
              "left half differs from the whole board render at t=%u", times[k]);
        CHECK(memcmp(rout, wout + NPX, sizeof(rout)) == 0,
              "right half differs from the whole board render at t=%u", times[k]);
    }
}

static void test_gradient_scrolls(void) {
    static const uint32_t stops[] = {VFX_HSB(0, 100, 100), VFX_HSB(180, 100, 100)};
    static const struct vfx_zone zone = {.pixels = NULL, .start = 0, .len = NPX};
    static const struct vfx_gradient_cfg cfg = {
        .stops = stops, .num_stops = 2, .scroll_speed = 20, .span = 0};
    static struct vfx_gradient_state st;
    static const struct vfx_layer layers[] = {{.api = &vfx_layer_gradient_api,
                                               .zone = &zone,
                                               .config = &cfg,
                                               .state = &st,
                                               .blend = VFX_BLEND_NORMAL,
                                               .opacity = 255}};
    static const struct vfx_scene scene = {.name = "g", .layers = layers, .num_layers = 1};

    struct vfx_rgb t0[NPX], t1[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(&scene, &ctx, t0, NULL);
    ctx.time_ms = 500;
    vfx_render_frame(&scene, &ctx, t1, NULL);

    CHECK(memcmp(t0, t1, sizeof(t0)) != 0, "a scrolling gradient must change over time");

    /* And it must not blow up on a far-future timestamp: scroll_speed * speed
     * * time_ms overflows 32 bits after a couple of hours of uptime.
     */
    ctx.time_ms = 0xFFFFFF00u;
    vfx_render_frame(&scene, &ctx, t1, NULL);
    CHECK(vfx_scene_is_animating(&scene, &ctx), "scrolling gradient must report animating");
}

static void test_static_scene_reports_idle(void) {
    static const struct vfx_zone zone = {.pixels = NULL, .start = 0, .len = NPX};
    static const struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 100, 50)};
    static struct vfx_solid_state st;
    static const struct vfx_layer layers[] = {{.api = &vfx_layer_solid_api,
                                               .zone = &zone,
                                               .config = &cfg,
                                               .state = &st,
                                               .blend = VFX_BLEND_NORMAL,
                                               .opacity = 255}};
    static const struct vfx_scene scene = {.name = "s", .layers = layers, .num_layers = 1};
    struct vfx_frame_ctx ctx = test_ctx();

    CHECK(!vfx_scene_is_animating(&scene, &ctx),
          "a solid scene must report idle so the engine can park its timer");
}

static void test_opacity_zero_layer_skipped(void) {
    static const struct vfx_zone zone = {.pixels = NULL, .start = 0, .len = NPX};
    static const struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 100, 100)};
    static struct vfx_solid_state st;
    static const struct vfx_layer layers[] = {{.api = &vfx_layer_solid_api,
                                               .zone = &zone,
                                               .config = &cfg,
                                               .state = &st,
                                               .blend = VFX_BLEND_NORMAL,
                                               .opacity = 0}};
    static const struct vfx_scene scene = {.name = "o", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = true;

    vfx_render_frame(&scene, &ctx, out, &lit);

    CHECK(!lit, "a fully transparent layer must leave the frame unlit");
}

static void test_brightness_scales_to_black(void) {
    static const struct vfx_zone zone = {.pixels = NULL, .start = 0, .len = NPX};
    static const struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 100, 100)};
    static struct vfx_solid_state st;
    static const struct vfx_layer layers[] = {{.api = &vfx_layer_solid_api,
                                               .zone = &zone,
                                               .config = &cfg,
                                               .state = &st,
                                               .blend = VFX_BLEND_NORMAL,
                                               .opacity = 255}};
    static const struct vfx_scene scene = {.name = "b", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = true;

    ctx.brightness = 0;
    vfx_render_frame(&scene, &ctx, out, &lit);

    /* This is the path the power gate depends on: brightness down to zero must
     * produce a genuinely black frame, not a very dim one.
     */
    CHECK(!lit, "brightness 0 must produce an unlit frame for the power gate");
}


/* ---- power gate ------------------------------------------------------- */

#define FRAME_MS 20

static const struct vfx_power_policy TEST_POLICY = {.blackout_delay_ms = 500, .settle_ms = 50};

static void test_gate_waits_the_blackout_delay(void) {
    struct vfx_power_ctl c;
    vfx_power_reset(&c);

    /* Black frames transmit normally until the delay is up, so the strip
     * actually shows the black rather than freezing on the last lit frame.
     */
    int transmits = 0;
    enum vfx_power_action a = VFX_POWER_TRANSMIT;

    for (int t = 0; t < 500; t += FRAME_MS) {
        a = vfx_power_step(&c, &TEST_POLICY, false, FRAME_MS);
        if (a == VFX_POWER_TRANSMIT) transmits++;
        else break;
    }

    CHECK(a == VFX_POWER_GATE_OFF, "gate did not fire at the delay, got %d", a);
    CHECK(transmits == 500 / FRAME_MS - 1 || transmits == 500 / FRAME_MS,
          "unexpected transmit count before gating: %d", transmits);
    CHECK(c.state == VFX_POWER_GATED, "state should be GATED after gating");
}

static void test_gate_does_not_thrash_on_a_blinking_scene(void) {
    /* A scene that dips through black every few frames must never gate: this
     * is the failure mode that would cycle the rail continuously.
     */
    struct vfx_power_ctl c;
    vfx_power_reset(&c);

    for (int i = 0; i < 2000; i++) {
        bool lit = (i % 10) < 7;   /* black for 3 frames out of every 10 */
        enum vfx_power_action a = vfx_power_step(&c, &TEST_POLICY, lit, FRAME_MS);

        CHECK(a == VFX_POWER_TRANSMIT, "blinking scene gated at frame %d (action %d)", i, a);
        if (a != VFX_POWER_TRANSMIT) break;
    }
}

static void test_gate_wakes_on_a_lit_frame_after_settling(void) {
    struct vfx_power_ctl c;
    vfx_power_reset(&c);

    /* Get into the gated state. */
    for (int t = 0; t <= 500; t += FRAME_MS) vfx_power_step(&c, &TEST_POLICY, false, FRAME_MS);
    CHECK(c.state == VFX_POWER_GATED, "setup: expected GATED");

    /* While gated and still black, the bus must stay quiet. */
    CHECK(vfx_power_step(&c, &TEST_POLICY, false, FRAME_MS) == VFX_POWER_SKIP,
          "gated black frames must not touch the bus");

    /* First lit frame brings the rail up but must not transmit yet. */
    CHECK(vfx_power_step(&c, &TEST_POLICY, true, FRAME_MS) == VFX_POWER_WAKE,
          "a lit frame while gated must wake the rail");
    CHECK(c.state == VFX_POWER_SETTLING, "should be settling after waking");

    /* Data driven at a rail that has not come up shows as garbage, so the
     * settle window must skip rather than transmit.
     */
    int skipped = 0;
    enum vfx_power_action a;
    for (int i = 0; i < 20; i++) {
        a = vfx_power_step(&c, &TEST_POLICY, true, FRAME_MS);
        if (a == VFX_POWER_SKIP) skipped++;
        else break;
    }

    CHECK(a == VFX_POWER_TRANSMIT, "settling never completed");
    CHECK(skipped * FRAME_MS >= TEST_POLICY.settle_ms - FRAME_MS,
          "transmitted after only %d ms of settling, wanted %d", skipped * FRAME_MS,
          TEST_POLICY.settle_ms);
    CHECK(c.state == VFX_POWER_LIT, "should be LIT once settled");
}

static void test_gate_stability_tracks_the_countdown(void) {
    struct vfx_power_ctl c;
    vfx_power_reset(&c);

    CHECK(vfx_power_is_stable(&c), "a freshly reset gate is stable");

    vfx_power_step(&c, &TEST_POLICY, false, FRAME_MS);
    CHECK(!vfx_power_is_stable(&c),
          "a running blackout countdown must keep the engine ticking, or a "
          "static black scene would never reach the gate");

    vfx_power_step(&c, &TEST_POLICY, true, FRAME_MS);
    CHECK(vfx_power_is_stable(&c), "a lit frame clears the countdown");

    for (int t = 0; t <= 500; t += FRAME_MS) vfx_power_step(&c, &TEST_POLICY, false, FRAME_MS);
    CHECK(vfx_power_is_stable(&c), "a fully gated rail is stable");
}

static void test_force_gated_matches_a_normal_gate(void) {
    /* Switching the underglow off cuts the rail outside the blackout
     * countdown. The gate has to end up in the same state it would have
     * reached normally, or the next wake would misbehave.
     */
    struct vfx_power_ctl forced;
    vfx_power_reset(&forced);
    vfx_power_force_gated(&forced);

    struct vfx_power_ctl counted;
    vfx_power_reset(&counted);
    for (int t = 0; t <= 500; t += FRAME_MS) {
        vfx_power_step(&counted, &TEST_POLICY, false, FRAME_MS);
    }

    CHECK(forced.state == counted.state, "forced gate state differs from a counted one");
    CHECK(vfx_power_is_stable(&forced), "a forced gate must be stable");

    /* And it must still wake normally. */
    CHECK(vfx_power_step(&forced, &TEST_POLICY, true, FRAME_MS) == VFX_POWER_WAKE,
          "a forced gate must still wake on a lit frame");
}

static void test_current_estimate(void) {
    struct vfx_rgb black[36] = {0};
    struct vfx_rgb white[36];

    for (int i = 0; i < 36; i++) white[i] = (struct vfx_rgb){255, 255, 255};

    CHECK(vfx_estimate_ua(black, 36, false) == 0, "an unpowered strip draws nothing");

    const uint32_t idle = vfx_estimate_ua(black, 36, true);
    const uint32_t full = vfx_estimate_ua(white, 36, true);

    printf("  36 px: black-but-powered %u mA, full white %u mA\n", idle / 1000, full / 1000);

    /* The whole justification for gating: a powered strip showing nothing is
     * still tens of milliamps, which dwarfs an idle MCU.
     */
    CHECK(idle > 25000 && idle < 40000, "idle estimate %u uA is outside the expected range", idle);
    CHECK(full > idle * 10, "full white should dwarf the quiescent draw");
}


/* ---- generators -------------------------------------------------------- */

static struct vfx_zone full_zone = {.pixels = NULL, .start = 0, .len = NPX};

#define SCENE1(varname, api_ptr, cfg_ptr, state_ptr)                                               \
    static struct vfx_layer varname##_layers[1];                                                   \
    varname##_layers[0] = (struct vfx_layer){.api = (api_ptr),                                     \
                                             .zone = &full_zone,                                   \
                                             .config = (cfg_ptr),                                  \
                                             .state = (state_ptr),                                 \
                                             .blend = VFX_BLEND_NORMAL,                            \
                                             .opacity = 255};                                      \
    struct vfx_scene varname = {.name = #varname, .layers = varname##_layers, .num_layers = 1}

static void test_ripple_fires_decays_and_goes_idle(void) {
    struct vfx_ripple_cfg cfg = {
        .color = VFX_HSB(0, 0, 100), .decay_ms = 600, .speed = 40, .width = 3};
    struct vfx_ripple_state st = {0};
    SCENE1(scene, &vfx_layer_ripple_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX; /* identity-ish key mapping for the test */

    /* A reactive scene with nothing happening must report idle: that is what
     * lets the engine park its timer and the power gate cut the rail.
     */
    CHECK(!vfx_scene_is_animating(&scene, &ctx), "an idle ripple layer must report idle");

    bool lit = false;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "no ripples means nothing lit");

    vfx_scene_key_event(&scene, &ctx, 10, true, 0);
    CHECK(vfx_scene_is_animating(&scene, &ctx), "a live ripple must report animating");

    ctx.time_ms = 100;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "a live ripple must light something");

    /* Past the decay the ripple is retired and we are idle again, which is
     * what allows the rail to gate back off after a keypress.
     */
    ctx.time_ms = 700;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "an expired ripple must stop lighting pixels");
    CHECK(!vfx_scene_is_animating(&scene, &ctx), "an expired ripple must report idle again");
}

static void test_ripple_travels_outward(void) {
    struct vfx_ripple_cfg cfg = {
        .color = VFX_HSB(0, 0, 100), .decay_ms = 2000, .speed = 40, .width = 2};
    struct vfx_ripple_state st = {0};
    SCENE1(scene, &vfx_layer_ripple_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    vfx_scene_key_event(&scene, &ctx, 18, true, 0);

    /* Brightest pixel should move away from the origin over time. */
    int first = -1, later = -1;

    ctx.time_ms = 100;
    vfx_render_frame(&scene, &ctx, out, NULL);
    for (int i = 0, best = 0; i < NPX; i++) {
        if (out[i].r > best) { best = out[i].r; first = i; }
    }

    /* 300 ms, not later: at 40 px/s the front is 12 pixels out, still on a 36
     * pixel strip. Much past that and the ripple has left the board.
     */
    ctx.time_ms = 300;
    vfx_render_frame(&scene, &ctx, out, NULL);
    for (int i = 0, best = 0; i < NPX; i++) {
        if (out[i].r > best) { best = out[i].r; later = i; }
    }

    const int origin = 18;
    CHECK(first >= 0 && later >= 0, "ripple produced no lit pixels");
    CHECK(abs(later - origin) > abs(first - origin),
          "ripple front did not travel outward: %d then %d from origin %d", first, later, origin);
}

static void test_ripple_retired_once_it_leaves_the_strip(void) {
    /* A ripple that has run off the end draws nothing, so it must not keep
     * reporting the layer as animating: that would hold the engine awake and
     * the power rail up for an invisible effect.
     */
    struct vfx_ripple_cfg cfg = {
        .color = VFX_HSB(0, 0, 100), .decay_ms = 60000, .speed = 40, .width = 2};
    struct vfx_ripple_state st = {0};
    SCENE1(scene, &vfx_layer_ripple_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    vfx_scene_key_event(&scene, &ctx, 18, true, 0);

    /* Long before the 60 s decay, but well past the far end of the strip. */
    ctx.time_ms = 3000;
    vfx_render_frame(&scene, &ctx, out, NULL);

    CHECK(!vfx_scene_is_animating(&scene, &ctx),
          "a ripple past the end of the strip must be retired, not held for its decay");
}

static void test_ripple_slots_reuse_oldest(void) {
    /* More presses than slots must not drop the newest: fast typing would
     * feel dead if the newest press were the one discarded.
     */
    struct vfx_ripple_cfg cfg = {
        .color = VFX_HSB(0, 0, 100), .decay_ms = 5000, .speed = 40, .width = 3};
    struct vfx_ripple_state st = {0};
    SCENE1(scene, &vfx_layer_ripple_api, &cfg, &st);

    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    for (int i = 0; i < VFX_MAX_RIPPLES + 3; i++) {
        vfx_scene_key_event(&scene, &ctx, (uint32_t)i, true, (uint32_t)(i * 10));
    }

    int active = 0, newest = 0;
    for (int i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st.slots[i].active) active++;
        if (st.slots[i].start_ms == (VFX_MAX_RIPPLES + 2) * 10) newest = 1;
    }

    CHECK(active == VFX_MAX_RIPPLES, "expected all slots busy, got %d", active);
    CHECK(newest, "the most recent press was dropped instead of the oldest");
}

static void test_trail_decay_is_time_based_not_frame_based(void) {
    /* Two runs over the same wall time at different frame rates must end up
     * at the same heat, or the effect would change character with frame rate.
     */
    struct vfx_trail_cfg cfg = {.color = VFX_HSB(0, 0, 100), .decay_ms = 1000, .spread = 2};
    struct vfx_trail_state fast = {0}, slow = {0};

    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    struct vfx_layer fl = {.api = &vfx_layer_trail_api, .zone = &full_zone, .config = &cfg,
                           .state = &fast, .blend = VFX_BLEND_NORMAL, .opacity = 255};
    struct vfx_layer sl = fl;
    sl.state = &slow;

    struct vfx_scene fs = {.name = "f", .layers = &fl, .num_layers = 1};
    struct vfx_scene ss = {.name = "s", .layers = &sl, .num_layers = 1};

    vfx_scene_key_event(&fs, &ctx, 10, true, 0);
    vfx_scene_key_event(&ss, &ctx, 10, true, 0);

    struct vfx_rgb out[NPX];

    for (uint32_t t = 0; t <= 500; t += 10) { ctx.time_ms = t; vfx_render_frame(&fs, &ctx, out, NULL); }
    for (uint32_t t = 0; t <= 500; t += 50) { ctx.time_ms = t; vfx_render_frame(&ss, &ctx, out, NULL); }

    const int origin = 10 * NPX / NPX;
    const int diff = abs((int)fast.heat[origin] - (int)slow.heat[origin]);

    printf("  trail heat after 500 ms: 10 ms steps %u, 50 ms steps %u\n",
           fast.heat[origin], slow.heat[origin]);
    CHECK(diff <= 24, "trail decay depends on frame rate: %d apart", diff);
}

static void test_twinkle_is_identical_on_both_halves(void) {
    /* Twinkle picks pixels from a hash rather than storing state. Both halves
     * must therefore agree without exchanging anything, the same property the
     * gradient has.
     */
    struct vfx_twinkle_cfg cfg = {.color = VFX_HSB(0, 0, 100), .period_ms = 1000, .density = 128};
    uint8_t dummy = 0;

    struct vfx_layer half_layer = {.api = &vfx_layer_twinkle_api, .zone = &full_zone,
                                   .config = &cfg, .state = &dummy,
                                   .blend = VFX_BLEND_NORMAL, .opacity = 255};
    static struct vfx_zone whole_zone = {.pixels = NULL, .start = 0, .len = NPX * 2};
    struct vfx_layer whole_layer = half_layer;
    whole_layer.zone = &whole_zone;

    struct vfx_scene half = {.name = "h", .layers = &half_layer, .num_layers = 1};
    struct vfx_scene whole = {.name = "w", .layers = &whole_layer, .num_layers = 1};

    struct vfx_rgb rout[NPX], wout[NPX * 2];
    struct vfx_frame_ctx rctx = test_ctx(), wctx = test_ctx();

    rctx.virtual_length = wctx.virtual_length = NPX * 2;
    rctx.strip_offset = NPX;
    wctx.num_pixels = NPX * 2;
    rctx.time_ms = wctx.time_ms = 4321;

    vfx_render_frame(&half, &rctx, rout, NULL);
    vfx_render_frame(&whole, &wctx, wout, NULL);

    CHECK(memcmp(rout, wout + NPX, sizeof(rout)) == 0,
          "twinkle differs between a half render and the whole board render");
}

static void test_battery_indicator_fills_proportionally(void) {
    struct vfx_battery_cfg cfg = {.low_color = VFX_HSB(0, 100, 100),
                                  .high_color = VFX_HSB(120, 100, 100),
                                  .empty_color = 0,
                                  .warn_below = 20};
    uint8_t dummy = 0;
    static struct vfx_zone bar = {.pixels = NULL, .start = 0, .len = 10};
    struct vfx_layer l = {.api = &vfx_layer_battery_api, .zone = &bar, .config = &cfg,
                          .state = &dummy, .blend = VFX_BLEND_NORMAL, .opacity = 255};
    struct vfx_scene scene = {.name = "bat", .layers = &l, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_status_mutable()->battery_level = 50;
    vfx_render_frame(&scene, &ctx, out, NULL);

    int lit_px = 0;
    for (int i = 0; i < 10; i++) if ((out[i].r | out[i].g | out[i].b) != 0) lit_px++;
    CHECK(lit_px == 5, "50%% battery should fill 5 of 10 pixels, got %d", lit_px);

    /* A nearly flat battery must still show something, or the indicator goes
     * dark exactly when it matters most.
     */
    vfx_status_mutable()->battery_level = 1;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK((out[0].r | out[0].g | out[0].b) != 0, "1%% battery must still light one pixel");
    CHECK(out[0].r > out[0].g, "a battery at 1%% should use the low colour (red)");

    vfx_status_mutable()->battery_level = 100;
}

static void test_ble_profile_lights_only_the_selected_slot(void) {
    struct vfx_ble_profile_cfg cfg = {.connected_color = VFX_HSB(210, 100, 100),
                                      .disconnected_color = VFX_HSB(0, 100, 60),
                                      .usb_color = VFX_HSB(120, 100, 80)};
    uint8_t dummy = 0;
    static struct vfx_zone slots = {.pixels = NULL, .start = 0, .len = 5};
    struct vfx_layer l = {.api = &vfx_layer_ble_profile_api, .zone = &slots, .config = &cfg,
                          .state = &dummy, .blend = VFX_BLEND_NORMAL, .opacity = 255};
    struct vfx_scene scene = {.name = "ble", .layers = &l, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_status_mutable()->ble_profile = 2;
    vfx_status_mutable()->ble_connected = true;
    vfx_status_mutable()->usb_output = false;

    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = 0; i < 5; i++) {
        bool on = (out[i].r | out[i].g | out[i].b) != 0;
        CHECK(on == (i == 2), "slot %d lit=%d, expected only slot 2", i, on);
    }

    vfx_status_mutable()->ble_profile = 0;
}

static void test_layer_state_picks_colour_by_layer(void) {
    static const uint32_t colors[] = {0, VFX_HSB(120, 100, 80), VFX_HSB(280, 100, 80)};
    struct vfx_layer_state_cfg cfg = {.colors = colors, .num_colors = 3};
    uint8_t dummy = 0;
    SCENE1(scene, &vfx_layer_layer_state_api, &cfg, &dummy);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = true;

    /* Layer 0 is given a zero-brightness colour, the usual way to keep the
     * base layer from drawing over whatever is beneath.
     */
    vfx_status_mutable()->active_layer = 0;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "layer 0 with a zero-brightness colour must draw nothing");

    vfx_status_mutable()->active_layer = 1;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit && out[0].g > out[0].r, "layer 1 should render its green");

    /* A layer beyond the list must not read off the end. */
    vfx_status_mutable()->active_layer = 9;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "a layer past the end of the colour list must draw nothing");

    vfx_status_mutable()->active_layer = 0;
}


/* ---- split sync -------------------------------------------------------- */

static void test_sync_jumps_on_first_beacon(void) {
    /* A peripheral that just connected, or just rebooted, has nothing on
     * screen worth protecting, and slewing seconds of offset would take
     * minutes. Take it all at once.
     */
    const int32_t out = vfx_sync_step(0, 5000);

    CHECK(out == 5000, "a large initial offset must be taken at once, got %d", out);
}

static void test_sync_slews_small_corrections(void) {
    /* Crystal drift shows up as a small, steady error. Stepping it would tear
     * whatever is animating, so it must be eased in under one frame's worth
     * at a time.
     */
    const int32_t out = vfx_sync_step(0, 200);

    CHECK(out != 200, "a small correction must not be applied in one step");
    CHECK(out == VFX_SYNC_MAX_SLEW_MS, "expected a %d ms step, got %d", VFX_SYNC_MAX_SLEW_MS, out);
    CHECK(VFX_SYNC_MAX_SLEW_MS < 1000 / 50,
          "a slew step of %d ms is larger than a frame at 50 fps and would be visible",
          VFX_SYNC_MAX_SLEW_MS);
}

static void test_sync_converges_and_settles(void) {
    int32_t offset = 0;
    const int32_t target = 180;
    int beacons = 0;

    while (offset != target && beacons < 1000) {
        offset = vfx_sync_step(offset, target);
        beacons++;
    }

    CHECK(offset == target, "sync never converged, stuck at %d", offset);
    printf("  converged on a %d ms error in %d beacons\n", target, beacons);

    /* Once there, it must stay put rather than oscillating around the target. */
    CHECK(vfx_sync_step(offset, target) == target, "sync oscillates once converged");
}

static void test_sync_handles_negative_drift(void) {
    CHECK(vfx_sync_step(0, -200) == -VFX_SYNC_MAX_SLEW_MS, "must slew backwards too");
    CHECK(vfx_sync_step(0, -5000) == -5000, "a large negative offset must jump");
}

static void test_sync_desired_survives_wrapping(void) {
    /* k_uptime_get() is truncated to 32 bits for the beacon, so the
     * subtraction has to still give the right delta across the wrap.
     */
    const uint32_t central = 100;          /* just wrapped */
    const uint32_t local = 0xFFFFFF00u;    /* about to wrap */
    const int32_t want = (int32_t)(central - local);

    CHECK(vfx_sync_desired(central, local) == want,
          "offset computed across a 32 bit wrap is wrong: %d", vfx_sync_desired(central, local));
    CHECK(want == 356, "expected a small positive delta across the wrap, got %d", want);
}


/* ---- water -------------------------------------------------------------- */

static struct vfx_water_cfg water_cfg_base(void) {
    return (struct vfx_water_cfg){
        .color = VFX_HSB(210, 90, 30),
        .crest_color = VFX_HSB(190, 40, 100),
        .wavelength = 8,
        .speed = 30,
        .lifetime_ms = 2500,
        .drop_rate_ms = 0, /* reactive only unless a test says otherwise */
        .amplitude = 200,
        .damping = 24,
    };
}

static void test_water_still_until_disturbed(void) {
    /* A reactive-only surface must be idle with nothing happening, so the
     * engine parks its timer and the power gate can cut the rail.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    CHECK(!vfx_scene_is_animating(&scene, &ctx),
          "reactive-only water must report idle before any drop");

    vfx_scene_key_event(&scene, &ctx, 18, true, 0);
    CHECK(vfx_scene_is_animating(&scene, &ctx), "a drop must wake the surface");

    /* And settle again once the drop has run its life. */
    ctx.time_ms = cfg.lifetime_ms + 1;
    struct vfx_rgb out[NPX];
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(!vfx_scene_is_animating(&scene, &ctx), "the surface must settle after lifetime-ms");
}

static void test_water_rain_runs_without_keys(void) {
    /* The general effect: drops keep falling with no input at all. */
    struct vfx_water_cfg cfg = water_cfg_base();
    cfg.drop_rate_ms = 400;
    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    CHECK(vfx_scene_is_animating(&scene, &ctx), "raining water is always animating");

    int changed = 0;
    struct vfx_rgb prev[NPX];
    ctx.time_ms = 1000;
    vfx_render_frame(&scene, &ctx, prev, NULL);

    for (uint32_t t = 1100; t <= 4000; t += 100) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);
        if (memcmp(prev, out, sizeof(out)) != 0) changed++;
        memcpy(prev, out, sizeof(out));
    }

    CHECK(changed > 20, "rain should keep the surface moving, only %d frames differed", changed);
}

static void test_water_wavefront_travels_outward(void) {
    /* Pixels far from the drop must stay still until the front reaches them.
     * That delay is what makes a disturbance spread rather than appear.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    cfg.speed = 20;
    cfg.damping = 0; /* isolate propagation from falloff */
    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;
    ctx.speed = 3; /* so the configured speed is used as-is */

    vfx_scene_key_event(&scene, &ctx, 0, true, 0);

    /* The far pixel is NPX-1 away, so at `speed` px/s the front reaches it
     * only after this long. Sampling either side of that is the test.
     */
    const uint32_t arrival_ms = (uint32_t)(NPX - 1) * 1000U / cfg.speed;

    ctx.time_ms = arrival_ms / 4; /* front still well short of the far end */
    vfx_render_frame(&scene, &ctx, out, NULL);
    struct vfx_rgb before = out[NPX - 1];

    ctx.time_ms = arrival_ms + 250; /* front has now passed it */
    CHECK(ctx.time_ms < cfg.lifetime_ms, "test setup: the drop dies before the front arrives");
    vfx_render_frame(&scene, &ctx, out, NULL);

    CHECK(memcmp(&before, &out[NPX - 1], sizeof(before)) != 0,
          "the far pixel never moved, so the wavefront is not propagating");
}

static void test_water_drops_interfere(void) {
    /* The point of carrying signed height: two drops must combine into one
     * surface, not paint over each other. Rendering them together has to
     * differ from either one alone.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    struct vfx_rgb one[NPX], two[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    struct vfx_water_state st_a = {0};
    SCENE1(scene_a, &vfx_layer_water_api, &cfg, &st_a);
    vfx_scene_key_event(&scene_a, &ctx, 8, true, 0);
    ctx.time_ms = 400;
    vfx_render_frame(&scene_a, &ctx, one, NULL);

    struct vfx_water_state st_b = {0};
    SCENE1(scene_b, &vfx_layer_water_api, &cfg, &st_b);
    vfx_scene_key_event(&scene_b, &ctx, 8, true, 0);
    vfx_scene_key_event(&scene_b, &ctx, 24, true, 0);
    vfx_render_frame(&scene_b, &ctx, two, NULL);

    CHECK(memcmp(one, two, sizeof(one)) != 0,
          "a second drop changed nothing, so drops are not superposing");
}

static void test_water_rain_identical_on_both_halves(void) {
    /* Ambient drops are hashed from the epoch number rather than stored, so
     * two halves sharing a timebase must produce the same rain with nothing
     * exchanged, exactly as the gradient and twinkle do.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    cfg.drop_rate_ms = 500;

    static struct vfx_zone whole = {.pixels = NULL, .start = 0, .len = NPX * 2};
    struct vfx_water_state st_h = {0}, st_w = {0};

    struct vfx_layer half_l = {.api = &vfx_layer_water_api, .zone = &full_zone, .config = &cfg,
                               .state = &st_h, .blend = VFX_BLEND_NORMAL, .opacity = 255};
    struct vfx_layer whole_l = half_l;
    whole_l.zone = &whole;
    whole_l.state = &st_w;

    struct vfx_scene right = {.name = "r", .layers = &half_l, .num_layers = 1};
    struct vfx_scene board = {.name = "w", .layers = &whole_l, .num_layers = 1};

    struct vfx_rgb rout[NPX], wout[NPX * 2];
    struct vfx_frame_ctx rctx = test_ctx(), wctx = test_ctx();

    rctx.virtual_length = wctx.virtual_length = NPX * 2;
    rctx.strip_offset = NPX;
    wctx.num_pixels = NPX * 2;

    for (uint32_t t = 0; t <= 3000; t += 700) {
        rctx.time_ms = wctx.time_ms = t;
        vfx_render_frame(&right, &rctx, rout, NULL);
        vfx_render_frame(&board, &wctx, wout, NULL);

        CHECK(memcmp(rout, wout + NPX, sizeof(rout)) == 0,
              "rain differs between the half and whole-board render at t=%u", t);
    }
}

static void test_water_still_surface_can_be_transparent(void) {
    /* With an unlit base the layer must decline still pixels, so it can sit
     * over another effect instead of flooding the strip.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    cfg.color = VFX_HSB(210, 90, 0);
    cfg.drop_rate_ms = 0;
    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    bool lit = true;

    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "an unlit still surface must draw nothing at all");
}


/* ---- pixel positions ---------------------------------------------------- */

/* The same 6x6 serpentine grid sim/derive-positions.py generates, one half. */
static int16_t grid_xy[NPX * 2];

static void build_grid(void) {
    for (int i = 0; i < NPX; i++) {
        int row = i / 6;
        int col = i % 6;
        if (row % 2 == 1) col = 5 - col;          /* serpentine */
        grid_xy[i * 2] = (int16_t)(col * 10);
        grid_xy[i * 2 + 1] = (int16_t)(row * 8);
    }
}

static struct vfx_frame_ctx grid_ctx(void) {
    struct vfx_frame_ctx ctx = test_ctx();
    build_grid();
    ctx.pixel_xy = grid_xy;
    ctx.num_positions = NPX;
    ctx.num_keys = NPX;
    return ctx;
}

static void test_distance_falls_back_to_the_strip(void) {
    /* Without a map the engine can only know position along the wire, and
     * must say so rather than inventing coordinates.
     */
    struct vfx_frame_ctx ctx = test_ctx();

    CHECK(vfx_pixel_distance(&ctx, 14, 17) == 3, "no map: distance is along the strip");
    CHECK(vfx_pixel_distance(&ctx, 17, 14) == 3, "distance must be symmetric");
    CHECK(vfx_board_extent(&ctx) == ctx.virtual_length, "no map: extent is the strip length");
}

static void test_distance_is_across_the_board_with_a_map(void) {
    struct vfx_frame_ctx ctx = grid_ctx();

    /* LED 14 sits at (20,16); 15 is one step right at (30,16). */
    CHECK(vfx_pixel_distance(&ctx, 14, 15) == 10, "adjacent in a row should be one pitch apart");

    /* 9 is at (20,8): directly above 14, and physically closer than 15, even
     * though it is five further away along the wire.
     */
    CHECK(vfx_pixel_distance(&ctx, 14, 9) == 8, "the row above should be a row pitch away");
    CHECK(vfx_pixel_distance(&ctx, 14, 9) < vfx_pixel_distance(&ctx, 14, 15),
          "a pixel five further along the wire is nearer on the board");

    /* Board diagonal: 50 wide, 40 tall. */
    CHECK(vfx_board_extent(&ctx) == 64, "extent should be the diagonal, got %u",
          vfx_board_extent(&ctx));
}

static void test_ripple_radiates_on_the_board_not_the_wire(void) {
    /* This is the bug the map exists to fix. A drop in the middle of the
     * board must reach the pixels physically around it, including ones that
     * are far away along the strip, and must not jump to a pixel that merely
     * happens to be adjacent on the wire.
     */
    struct vfx_water_cfg cfg = water_cfg_base();
    cfg.wavelength = 40;
    cfg.speed = 60;
    cfg.damping = 0;
    cfg.lifetime_ms = 4000;

    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    /* Key 14 maps to pixel 14, in the middle of the grid. */
    vfx_scene_key_event(&scene, &ctx, 14, true, 0);

    /* Water draws its still surface everywhere, so "reached" means "differs
     * from the resting colour", not "is lit at all".
     */
    struct vfx_water_state rest_st = {0};
    SCENE1(rest_scene, &vfx_layer_water_api, &cfg, &rest_st);
    struct vfx_rgb rest[NPX];
    vfx_render_frame(&rest_scene, &ctx, rest, NULL);

    /* 150 ms at 60 units/s puts the front 9 units out: past 9 and 21, which
     * are one row (8) away, and not yet past 15, which is a full pitch (10).
     */
    ctx.time_ms = 150;
    vfx_render_frame(&scene, &ctx, out, NULL);

    const bool moved_above = memcmp(&out[9], &rest[9], sizeof(rest[9])) != 0;
    const bool moved_below = memcmp(&out[21], &rest[21], sizeof(rest[21])) != 0;
    const bool moved_far_on_wire = memcmp(&out[15], &rest[15], sizeof(rest[15])) != 0;

    CHECK(moved_above && moved_below,
          "the rows above and below the drop should be reached first (9=%d, 21=%d)",
          moved_above, moved_below);
    CHECK(!moved_far_on_wire,
          "pixel 15 is a full pitch away and should not be reached yet, but it moved");
}

static void test_trail_deposits_by_board_distance(void) {
    /* Trail walks every pixel rather than a window of indices, so heat lands
     * on what is near the key, not what is near it on the wire.
     */
    struct vfx_trail_cfg cfg = {.color = VFX_HSB(0, 0, 100), .decay_ms = 5000, .spread = 9};
    struct vfx_trail_state st = {0};
    SCENE1(scene, &vfx_layer_trail_api, &cfg, &st);

    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_scene_key_event(&scene, &ctx, 14, true, 0);

    CHECK(st.heat[14] > 0, "the key's own pixel must get heat");
    CHECK(st.heat[9] > 0, "the pixel directly above must get heat, 5 away on the wire");
    CHECK(st.heat[21] > 0, "the pixel directly below must get heat, 7 away on the wire");
    CHECK(st.heat[13] == 0 && st.heat[15] == 0,
          "pixels a full pitch away are outside a spread of 9");
}

/* ---- matrix rain -------------------------------------------------------- */

static bool rgb_is_black(struct vfx_rgb c) { return c.r == 0 && c.g == 0 && c.b == 0; }

static struct vfx_matrix_cfg matrix_cfg_base(void) {
    return (struct vfx_matrix_cfg){
        .color = VFX_HSB(120, 100, 60),
        .head_color = VFX_HSB(120, 20, 100),
        .speed = 60,
        .tail = 40,
        .drop_rate_ms = 0, /* keypress-only unless a test says otherwise */
        .columns = 6,
        .jitter = 0, /* deterministic speeds; one test turns it back on */
        .head_size = 8,
    };
}

/* In the 6x6 grid, these three pixels share a board column (x=20) and run
 * down it, even though they are 5 and 12 apart along the wire.
 */
#define MX_TOP 9  /* (20, 8)  */
#define MX_MID 14 /* (20, 16) */
#define MX_LOW 21 /* (20, 24) */

static void test_matrix_keypress_falls_from_the_top_to_the_key(void) {
    /* The reaction points at the key: the column starts at the top of the
     * board and travels down it, reaching the rows above the key before the
     * key itself.
     */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3; /* neutral, so the configured speed is used as-is */

    vfx_render_frame(&scene, &ctx, out, NULL); /* frame() sizes the columns */
    vfx_scene_key_event(&scene, &ctx, MX_MID, true, 0);

    /* 60 units/s puts the head on MX_TOP (y=8) at 133 ms and on the key
     * (y=16) at 267 ms, so 200 ms falls between the two.
     */
    ctx.time_ms = 200;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(!rgb_is_black(out[MX_TOP]), "the row above the key should light first");
    CHECK(rgb_is_black(out[MX_MID]), "the drop should not have reached the key yet");

    ctx.time_ms = 300;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(!rgb_is_black(out[MX_MID]), "the drop should have arrived at the key");

    /* And it drains away rather than sitting there. */
    ctx.time_ms = 2000;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(rgb_is_black(out[MX_TOP]) && rgb_is_black(out[MX_MID]),
          "the drop should have drained into the key and gone by now");
}

static void test_matrix_keypress_stops_at_the_key(void) {
    /* Nothing below the key, ever. That is what makes the column land on
     * what you pressed instead of running through it.
     */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    vfx_render_frame(&scene, &ctx, out, NULL);
    vfx_scene_key_event(&scene, &ctx, MX_MID, true, 0);

    for (uint32_t t = 0; t <= 2000; t += 25) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);

        for (int i = 0; i < NPX; i++) {
            /* Everything in the key's column that sits below it. */
            if (grid_xy[i * 2] != 20 || grid_xy[i * 2 + 1] <= 16) {
                continue;
            }

            CHECK(rgb_is_black(out[i]), "pixel %d below the key lit up at t=%u", i, t);
            if (!rgb_is_black(out[i])) {
                return;
            }
        }
    }
}

static void test_matrix_head_is_brighter_than_its_trail(void) {
    /* What makes rain read as falling rather than as a moving stripe. */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    cfg.head_size = 4; /* under one row, so MX_TOP is trail and not head */
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    vfx_render_frame(&scene, &ctx, out, NULL);
    vfx_scene_key_event(&scene, &ctx, MX_MID, true, 0);

    ctx.time_ms = 267; /* head on the key, MX_TOP a row behind it */
    vfx_render_frame(&scene, &ctx, out, NULL);

    const int head = out[MX_MID].r + out[MX_MID].g + out[MX_MID].b;
    const int tail = out[MX_TOP].r + out[MX_TOP].g + out[MX_TOP].b;

    CHECK(head > tail, "the head (%d) should outshine the trail behind it (%d)", head, tail);
}

static void test_matrix_stays_in_its_column(void) {
    /* Neighbouring columns must not light up: rain falls in lanes.
     * Pixel 15 is beside MX_MID on the board and next to it on the wire.
     */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    vfx_render_frame(&scene, &ctx, out, NULL);
    vfx_scene_key_event(&scene, &ctx, MX_MID, true, 0);

    for (uint32_t t = 0; t <= 1200; t += 50) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);

        for (int i = 0; i < NPX; i++) {
            if (grid_xy[i * 2] == 20) {
                continue; /* the drop's own column */
            }

            CHECK(rgb_is_black(out[i]), "pixel %d in another column lit up at t=%u", i, t);
            if (!rgb_is_black(out[i])) {
                return;
            }
        }
    }
}

static void test_matrix_idle_without_rain(void) {
    /* drop-rate-ms = 0 is the keypress-only mode: dark and idle between
     * presses, so the timer parks and the power gate can cut the rail.
     */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    bool lit = true;

    ctx.speed = 3;

    CHECK(!vfx_scene_is_animating(&scene, &ctx), "keypress-only rain must report idle");
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "nothing should be lit before the first press");

    vfx_scene_key_event(&scene, &ctx, MX_MID, true, 0);
    CHECK(vfx_scene_is_animating(&scene, &ctx), "a press must wake the layer");

    /* The drop has to be retired once it is gone, or the layer never settles
     * and the rail stays up forever.
     */
    ctx.time_ms = 10000;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!vfx_scene_is_animating(&scene, &ctx), "the layer must settle once the drop is gone");
}

static void test_matrix_rain_runs_without_keys(void) {
    /* And the general effect: it keeps raining with no input at all. */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    cfg.drop_rate_ms = 200;
    cfg.jitter = 80;
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX], prev[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    CHECK(vfx_scene_is_animating(&scene, &ctx), "raining matrix is always animating");

    int changed = 0, ever_lit = 0;
    ctx.time_ms = 1000;
    vfx_render_frame(&scene, &ctx, prev, NULL);

    for (uint32_t t = 1050; t <= 5000; t += 50) {
        bool lit = false;

        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, &lit);

        if (memcmp(prev, out, sizeof(out)) != 0) {
            changed++;
        }
        if (lit) {
            ever_lit++;
        }

        memcpy(prev, out, sizeof(out));
    }

    CHECK(changed > 40, "rain should keep moving, only %d frames differed", changed);
    CHECK(ever_lit > 60, "rain should light the board nearly always, only %d frames lit", ever_lit);
}

/* Two 6x6 grids side by side with a gap between them, as on a split: the
 * whole-board map both halves are given.
 */
static int16_t board_xy[NPX * 2 * 2];

static void build_board(void) {
    build_grid();

    for (int i = 0; i < NPX; i++) {
        board_xy[i * 2] = grid_xy[i * 2];
        board_xy[i * 2 + 1] = grid_xy[i * 2 + 1];
        board_xy[(NPX + i) * 2] = (int16_t)(grid_xy[i * 2] + 80);
        board_xy[(NPX + i) * 2 + 1] = grid_xy[i * 2 + 1];
    }
}

static void test_matrix_rain_identical_on_both_halves(void) {
    /* Ambient drops are hashed from the epoch number rather than stored, so
     * two halves sharing a timebase agree on the rain with nothing exchanged.
     * Rendering the right half alone must match the right half of a
     * whole-board render, pixel for pixel.
     */
    build_board();

    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    cfg.drop_rate_ms = 300;
    cfg.jitter = 60;
    cfg.columns = 12; /* both halves' columns */

    static struct vfx_zone whole = {.pixels = NULL, .start = 0, .len = NPX * 2};
    struct vfx_matrix_state st_h = {0}, st_w = {0};

    struct vfx_layer half_l = {.api = &vfx_layer_matrix_api,
                               .zone = &full_zone,
                               .config = &cfg,
                               .state = &st_h,
                               .blend = VFX_BLEND_NORMAL,
                               .opacity = 255};
    struct vfx_layer whole_l = half_l;
    whole_l.zone = &whole;
    whole_l.state = &st_w;

    struct vfx_scene right = {.name = "r", .layers = &half_l, .num_layers = 1};
    struct vfx_scene board = {.name = "w", .layers = &whole_l, .num_layers = 1};

    struct vfx_rgb rout[NPX], wout[NPX * 2];
    struct vfx_frame_ctx rctx = test_ctx(), wctx = test_ctx();

    rctx.virtual_length = wctx.virtual_length = NPX * 2;
    rctx.strip_offset = NPX;
    wctx.num_pixels = NPX * 2;
    rctx.speed = wctx.speed = 3;
    rctx.pixel_xy = wctx.pixel_xy = board_xy;
    rctx.num_positions = wctx.num_positions = NPX * 2;

    for (uint32_t t = 0; t <= 4000; t += 350) {
        rctx.time_ms = wctx.time_ms = t;
        vfx_render_frame(&right, &rctx, rout, NULL);
        vfx_render_frame(&board, &wctx, wout, NULL);

        CHECK(memcmp(rout, wout + NPX, sizeof(rout)) == 0,
              "rain differs between the half and whole-board render at t=%u", t);
    }
}

static void test_matrix_rain_lands_where_the_leds_are(void) {
    /* A column is a slice of the board's width, and nothing says an LED sits
     * in every slice: the gap between the halves of a split is one such empty
     * slice, and a strip that reaches past the key field makes far more of
     * them. If a drop picked a column number, every empty slice would swallow
     * its share of the rain.
     *
     * The map here is the usual grid plus one pixel out on its own, which
     * stretches the board wide enough that eleven of the twelve columns hold
     * nothing.
     */
    static int16_t sparse_xy[NPX * 2];

    build_grid();
    for (int i = 0; i < NPX * 2; i++) {
        sparse_xy[i] = grid_xy[i];
    }
    sparse_xy[(NPX - 1) * 2] = 500;

    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    cfg.drop_rate_ms = 260;
    cfg.jitter = 90;
    cfg.columns = 12;
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    int lit = 0;

    ctx.speed = 3;
    ctx.pixel_xy = sparse_xy;
    ctx.num_positions = NPX;

    for (uint32_t t = 0; t <= 20000; t += 100) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);

        for (int i = 0; i < NPX; i++) {
            if (!rgb_is_black(out[i])) {
                lit++;
            }
        }
    }

    /* Picking a column number leaves this in the low hundreds. */
    CHECK(lit > 2000, "most of the rain fell in columns with no LEDs in them: %d lit pixels", lit);
}

static void test_matrix_falls_back_to_the_strip(void) {
    /* With no position map there is no y axis, so strip order stands in for
     * one. It must still animate rather than sit dark or divide the strip
     * into columns that can never all be reached.
     */
    struct vfx_matrix_cfg cfg = matrix_cfg_base();
    cfg.drop_rate_ms = 250;
    cfg.speed = 20; /* units are now pixels, so a board speed would blur past */
    cfg.tail = 8;
    struct vfx_matrix_state st = {0};
    SCENE1(scene, &vfx_layer_matrix_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx(); /* deliberately no pixel_xy */
    int lit_frames = 0;

    ctx.speed = 3;

    for (uint32_t t = 500; t <= 4000; t += 100) {
        bool lit = false;

        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, &lit);

        if (lit) {
            lit_frames++;
        }
    }

    CHECK(lit_frames > 20, "rain without a map lit only %d of 36 frames", lit_frames);
}

static void test_water_typing_preset_gates_the_rail(void) {
    /* The shipped "Water (typing)" preset exists to be dark between
     * keypresses. Still water of any brightness lights every pixel in the
     * zone forever, which holds the rail up and costs tens of milliamps, so
     * the preset's own numbers have to render nothing at rest.
     */
    struct vfx_water_cfg cfg = {
        .color = VFX_HSB(205, 95, 0),
        .crest_color = VFX_HSB(185, 25, 100),
        .wavelength = 18,
        .speed = 55,
        .lifetime_ms = 2400,
        .drop_rate_ms = 0,
        .amplitude = 255,
        .damping = 7,
    };
    struct vfx_water_state st = {0};
    SCENE1(scene, &vfx_layer_water_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    bool lit = true;

    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "the typing preset lights the board at rest, so the rail can never gate");

    /* And it still shows something when you do type. */
    vfx_scene_key_event(&scene, &ctx, 14, true, 0);
    ctx.time_ms = 200;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "a keypress must still disturb the surface visibly");
}

/* ---- axes ---------------------------------------------------------------- */

static void test_atan2_matches_the_real_thing(void) {
    /* A pinwheel is only as straight as this is accurate, and the cheap
     * approximations are exactly the ones that visibly bend one.
     */
    int worst = 0;

    for (int deg = 0; deg < 360; deg++) {
        const double rad = deg * 3.14159265358979323846 / 180.0;
        const int32_t x = (int32_t)(1000 * cos(rad));
        const int32_t y = (int32_t)(1000 * sin(rad));

        const int got = vfx_atan2_8(y, x);
        const int want = (int)lround(deg * 256.0 / 360.0) & 0xFF;

        int d = abs(got - want);
        if (d > 128) {
            d = 256 - d; /* the turn wraps */
        }
        if (d > worst) {
            worst = d;
        }
    }

    printf("  atan2 worst error: %d/256 of a turn\n", worst);
    CHECK(worst <= 2, "atan2 is off by %d/256 of a turn", worst);

    CHECK(vfx_atan2_8(0, 0) == 0, "the centre has no angle, but must not divide by zero");
    CHECK(vfx_atan2_8(0, 100) == 0, "the positive x axis is turn 0");
    CHECK(vfx_atan2_8(100, 0) == 64, "quarter turn");
    CHECK(vfx_atan2_8(0, -100) == 128, "half turn");
    CHECK(vfx_atan2_8(-100, 0) == 192, "three quarter turn");
}

static void test_axes_fall_back_to_the_strip(void) {
    /* No map means no board to run across. Every axis has to keep working,
     * or a scene written on a mapped board goes dark on an unmapped one.
     */
    struct vfx_frame_ctx ctx = test_ctx();

    for (uint8_t axis = VFX_AXIS_STRIP; axis <= VFX_AXIS_SPIRAL; axis++) {
        CHECK(vfx_axis_pos(&ctx, 7, axis) == 7, "axis %u should fall back to the strip", axis);
        CHECK(vfx_axis_span(&ctx, axis) == NPX, "axis %u span should be the strip length", axis);
    }
}

static void test_axes_measure_the_board(void) {
    /* The 6x6 grid is 50 wide and 40 tall, so the axes have known answers. */
    struct vfx_frame_ctx ctx = grid_ctx();

    CHECK(vfx_axis_span(&ctx, VFX_AXIS_X) == 51, "x span, got %u", vfx_axis_span(&ctx, VFX_AXIS_X));
    CHECK(vfx_axis_span(&ctx, VFX_AXIS_Y) == 41, "y span, got %u", vfx_axis_span(&ctx, VFX_AXIS_Y));
    CHECK(vfx_axis_span(&ctx, VFX_AXIS_ANGLE) == 256, "a turn is 256 units");

    /* Pixel 0 is the top left corner, pixel 35 the bottom left (serpentine). */
    CHECK(vfx_axis_pos(&ctx, 0, VFX_AXIS_X) == 0, "top left is at x 0");
    CHECK(vfx_axis_pos(&ctx, 0, VFX_AXIS_Y) == 0, "top left is at y 0");
    CHECK(vfx_axis_pos(&ctx, 5, VFX_AXIS_X) == 50, "the end of the first row is at x 50");
    CHECK(vfx_axis_pos(&ctx, 5, VFX_AXIS_Y) == 0, "the first row is all at y 0");

    /* Two pixels the same distance out from the middle must agree on radius
     * however far apart they are on the wire: that is the whole point.
     */
    const uint32_t tl = vfx_axis_pos(&ctx, 0, VFX_AXIS_RADIAL);
    const uint32_t tr = vfx_axis_pos(&ctx, 5, VFX_AXIS_RADIAL);
    CHECK(tl == tr, "opposite corners should be the same radius, %u vs %u", tl, tr);

    /* Opposite corners are half a turn apart. Serpentine wiring puts the
     * bottom right corner at index 30, a long way from index 0 on the wire
     * and exactly across the board from it.
     */
    const int a = (int)vfx_axis_pos(&ctx, 0, VFX_AXIS_ANGLE);
    const int b = (int)vfx_axis_pos(&ctx, 30, VFX_AXIS_ANGLE);
    int apart = abs(a - b);
    if (apart > 128) {
        apart = 256 - apart;
    }
    CHECK(apart == 128, "opposite corners should be half a turn apart, got %d", apart);
}

static void test_gradient_runs_along_its_axis(void) {
    /* The point of the axis: a left-right gradient gives every pixel in a
     * column the same colour, and a top-bottom one every pixel in a row --
     * even though on a serpentine strip neither is contiguous on the wire.
     */
    static uint32_t stops[] = {VFX_HSB(0, 100, 100), VFX_HSB(240, 100, 100)};
    struct vfx_gradient_cfg cfg = {
        .stops = stops, .num_stops = 2, .scroll_speed = 0, .span = 0, .axis = VFX_AXIS_X};
    struct vfx_gradient_state st = {0};
    SCENE1(scene, &vfx_layer_gradient_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = 0; i < NPX; i++) {
        for (int j = 0; j < NPX; j++) {
            if (grid_xy[i * 2] != grid_xy[j * 2]) {
                continue; /* different column */
            }

            CHECK(memcmp(&out[i], &out[j], sizeof(out[0])) == 0,
                  "pixels %d and %d share a column but differ under an x gradient", i, j);
            if (memcmp(&out[i], &out[j], sizeof(out[0])) != 0) {
                return;
            }
        }
    }

    /* And it really is a gradient: the two ends of a row differ. */
    CHECK(memcmp(&out[0], &out[5], sizeof(out[0])) != 0,
          "opposite ends of a row should be different colours");

    /* Turn it 90 degrees and rows match instead of columns. */
    cfg.axis = VFX_AXIS_Y;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(memcmp(&out[0], &out[5], sizeof(out[0])) == 0,
          "a top-to-bottom gradient should give a row one colour");
}

static void test_pinwheel_sweeps_around_the_board(void) {
    /* An angular gradient has to come back to where it started: the pixel at
     * the far side of the board is half a cycle away, not a whole board away.
     */
    static uint32_t stops[] = {VFX_HSB(0, 100, 100), VFX_HSB(180, 100, 100)};
    struct vfx_gradient_cfg cfg = {
        .stops = stops, .num_stops = 2, .scroll_speed = 0, .span = 0, .axis = VFX_AXIS_ANGLE};
    struct vfx_gradient_state st = {0};
    SCENE1(scene, &vfx_layer_gradient_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_render_frame(&scene, &ctx, out, NULL);

    /* Pixels 0 and 30 are opposite corners, so half a turn apart and
     * therefore at opposite ends of the two-stop cycle.
     */
    const int top = out[0].r - out[0].b;
    const int bottom = out[30].r - out[30].b;

    CHECK((top > 0) != (bottom > 0),
          "opposite corners should sit on opposite sides of the colour cycle");

    /* Every pixel must land somewhere: an angular axis that divided by zero
     * or ran off the table would leave black.
     */
    for (int i = 0; i < NPX; i++) {
        CHECK(!rgb_is_black(out[i]), "pixel %d came out black under a pinwheel", i);
        if (rgb_is_black(out[i])) {
            return;
        }
    }
}

static void test_plasma_is_two_dimensional_with_a_map(void) {
    /* With real coordinates the plasma varies down the board as well as
     * across it. Without a map both axes collapse onto the strip, which is
     * the old behaviour and still has to work.
     */
    struct vfx_plasma_cfg cfg = {
        .color = VFX_HSB(280, 90, 80), .scale = 0, .period_ms = 6000, .hue_spread = 60};
    uint8_t st = 0; /* stateless generator */
    SCENE1(scene, &vfx_layer_plasma_api, &cfg, &st);

    struct vfx_rgb mapped[NPX], plain[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    struct vfx_frame_ctx bare = test_ctx();

    ctx.time_ms = bare.time_ms = 1500;
    vfx_render_frame(&scene, &ctx, mapped, NULL);
    vfx_render_frame(&scene, &bare, plain, NULL);

    CHECK(memcmp(mapped, plain, sizeof(mapped)) != 0,
          "the plasma ignored the position map, so it is still one dimensional");

    /* Pixels 0 and 35 sit in the same column, five rows apart. A plasma that
     * only knew the strip would have nothing but their 35 places of
     * separation to go on.
     */
    CHECK(memcmp(&mapped[0], &mapped[35], sizeof(mapped[0])) != 0,
          "a column should not come out flat");

    int lit = 0;
    for (int i = 0; i < NPX; i++) {
        if (!rgb_is_black(mapped[i])) {
            lit++;
        }
    }
    CHECK(lit == NPX, "plasma should cover the zone, only %d of %d lit", lit, NPX);
}

static void test_twinkle_hue_spread_stays_deterministic(void) {
    /* Scattered hues still have to be a pure function of the clock, or the
     * two halves of a split stop agreeing on them.
     */
    struct vfx_twinkle_cfg cfg = {
        .color = VFX_HSB(200, 90, 100), .period_ms = 1200, .density = 200, .hue_spread = 120};
    uint8_t st_a = 0, st_b = 0; /* stateless generator */
    SCENE1(a, &vfx_layer_twinkle_api, &cfg, &st_a);
    SCENE1(b, &vfx_layer_twinkle_api, &cfg, &st_b);

    struct vfx_rgb out_a[NPX], out_b[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    int coloured = 0;

    for (uint32_t t = 0; t <= 4000; t += 250) {
        ctx.time_ms = t;
        vfx_render_frame(&a, &ctx, out_a, NULL);
        vfx_render_frame(&b, &ctx, out_b, NULL);

        CHECK(memcmp(out_a, out_b, sizeof(out_a)) == 0, "twinkle diverged between halves at t=%u",
              t);

        /* And the hues really do differ from each other. */
        for (int i = 1; i < NPX; i++) {
            if (!rgb_is_black(out_a[i]) && !rgb_is_black(out_a[i - 1]) &&
                memcmp(&out_a[i], &out_a[i - 1], sizeof(out_a[0])) != 0) {
                coloured++;
            }
        }
    }

    CHECK(coloured > 20, "hue-spread twinkles came out all the same colour (%d)", coloured);
}

/* ---- cross --------------------------------------------------------------- */

static struct vfx_cross_cfg cross_cfg_base(void) {
    return (struct vfx_cross_cfg){
        .color = VFX_HSB(190, 90, 80),
        .centre_color = VFX_HSB(0, 0, 100),
        .decay_ms = 500,
        .radius = 0,
        .thickness = 4,
        .axes = VFX_CROSS_BOTH,
    };
}

static void test_cross_lights_the_row_and_column(void) {
    /* The whole point: the pressed key's row and column light, and nothing
     * off them does, however near they are on the wire.
     */
    struct vfx_cross_cfg cfg = cross_cfg_base();
    struct vfx_cross_state st = {0};
    SCENE1(scene, &vfx_layer_cross_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_scene_key_event(&scene, &ctx, 14, true, 0); /* (20, 16) */
    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = 0; i < NPX; i++) {
        const bool same_row = grid_xy[i * 2 + 1] == 16;
        const bool same_col = grid_xy[i * 2] == 20;
        const bool lit = !rgb_is_black(out[i]);

        CHECK(lit == (same_row || same_col), "pixel %d at (%d,%d) lit=%d, expected %d", i,
              grid_xy[i * 2], grid_xy[i * 2 + 1], lit, same_row || same_col);
        if (lit != (same_row || same_col)) {
            return;
        }
    }
}

static void test_cross_axes_select_one_arm(void) {
    struct vfx_cross_cfg cfg = cross_cfg_base();
    cfg.axes = VFX_CROSS_HORIZONTAL;
    struct vfx_cross_state st = {0};
    SCENE1(scene, &vfx_layer_cross_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_scene_key_event(&scene, &ctx, 14, true, 0);
    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = 0; i < NPX; i++) {
        const bool lit = !rgb_is_black(out[i]);

        CHECK(lit == (grid_xy[i * 2 + 1] == 16), "horizontal-only lit pixel %d off the row", i);
        if (lit != (grid_xy[i * 2 + 1] == 16)) {
            return;
        }
    }
}

static void test_cross_radius_makes_a_nexus(void) {
    /* With a radius the arms stop short and fade over it, which is what
     * turns a cross into a compact shape around the key.
     */
    struct vfx_cross_cfg cfg = cross_cfg_base();
    cfg.radius = 15; /* a row and a half */
    struct vfx_cross_state st = {0};
    SCENE1(scene, &vfx_layer_cross_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_scene_key_event(&scene, &ctx, 14, true, 0);
    vfx_render_frame(&scene, &ctx, out, NULL);

    /* 15 is at (30,16), one pitch along the row: inside the radius. 16 is at
     * (40,16), two pitches out: beyond it.
     */
    CHECK(!rgb_is_black(out[15]), "the neighbour along the row should be inside the radius");
    CHECK(rgb_is_black(out[16]), "two pitches out should be beyond a radius of 15");

    /* And it fades: nearer is brighter. */
    const int near = out[15].r + out[15].g + out[15].b;
    const int mid = out[14].r + out[14].g + out[14].b;
    CHECK(mid > near, "the key (%d) should outshine its neighbour (%d)", mid, near);
}

static void test_cross_decays_and_goes_idle(void) {
    struct vfx_cross_cfg cfg = cross_cfg_base();
    struct vfx_cross_state st = {0};
    SCENE1(scene, &vfx_layer_cross_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    bool lit = true;

    CHECK(!vfx_scene_is_animating(&scene, &ctx), "an idle cross layer must report idle");
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "nothing pressed means nothing lit");

    vfx_scene_key_event(&scene, &ctx, 14, true, 0);
    ctx.time_ms = 100;
    vfx_render_frame(&scene, &ctx, out, NULL);
    const int early = out[14].r + out[14].g + out[14].b;

    ctx.time_ms = 400;
    vfx_render_frame(&scene, &ctx, out, NULL);
    const int late = out[14].r + out[14].g + out[14].b;

    CHECK(late < early, "the cross should be fading (%d then %d)", early, late);

    ctx.time_ms = cfg.decay_ms + 50;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!vfx_scene_is_animating(&scene, &ctx), "the layer must settle once the cross is gone");
    CHECK(!lit, "and draw nothing, so the power gate can cut the rail");
}

static void test_cross_falls_back_to_the_strip(void) {
    /* Without a map there are no rows, so it lights a run of the strip either
     * side of the key rather than going dark.
     */
    struct vfx_cross_cfg cfg = cross_cfg_base();
    cfg.radius = 5;
    struct vfx_cross_state st = {0};
    SCENE1(scene, &vfx_layer_cross_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.num_keys = NPX;

    vfx_scene_key_event(&scene, &ctx, 18, true, 0);
    vfx_render_frame(&scene, &ctx, out, NULL);

    CHECK(!rgb_is_black(out[18]), "the key itself must light");
    CHECK(!rgb_is_black(out[20]), "and its neighbours on the strip");
    CHECK(rgb_is_black(out[26]), "but not pixels beyond the radius");
}

/* ---- fire and comet ------------------------------------------------------ */

static void test_fire_burns_upward(void) {
    /* The bottom of the board is always alight and the top only sometimes:
     * that asymmetry is the difference between fire and a flicker.
     */
    struct vfx_fire_cfg cfg = {
        .base_color = VFX_HSB(0, 100, 60),
        .tip_color = VFX_HSB(45, 80, 100),
        .period_ms = 500,
        .cell = 12,
        .height = 255, /* tall enough to reach the top, when a flame stretches */
        .flicker = 180,
        .axis = VFX_AXIS_Y,
    };
    uint8_t st = 0; /* stateless generator */
    SCENE1(scene, &vfx_layer_fire_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;

    int bottom_lit = 0, top_lit = 0, frames = 0;

    for (uint32_t t = 0; t <= 8000; t += 100) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);
        frames++;

        for (int i = 0; i < NPX; i++) {
            if (rgb_is_black(out[i])) {
                continue;
            }

            if (grid_xy[i * 2 + 1] == 40) {
                bottom_lit++;
            } else if (grid_xy[i * 2 + 1] == 0) {
                top_lit++;
            }
        }
    }

    /* Six pixels in each row, so a row lit in every frame scores 6x frames. */
    CHECK(bottom_lit > frames * 5, "the bottom row should be alight throughout, got %d of %d",
          bottom_lit, frames * 6);
    CHECK(top_lit < bottom_lit, "the top row (%d) should be lit less than the bottom (%d)", top_lit,
          bottom_lit);
    CHECK(top_lit > 0, "the flames should sometimes reach the top");

    /* And height really is a ceiling: shorten it and the top stays dark. */
    cfg.height = 100;
    top_lit = 0;

    for (uint32_t t = 0; t <= 8000; t += 100) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);

        for (int i = 0; i < NPX; i++) {
            if (!rgb_is_black(out[i]) && grid_xy[i * 2 + 1] == 0) {
                top_lit++;
            }
        }
    }

    CHECK(top_lit == 0, "a height of 100 should leave the top row dark, but lit it %d times",
          top_lit);
}

static void test_fire_is_identical_on_both_halves(void) {
    /* No heat buffer means no drift: the same instant renders the same fire
     * wherever it is computed.
     */
    struct vfx_fire_cfg cfg = {
        .base_color = VFX_HSB(0, 100, 60),
        .tip_color = VFX_HSB(45, 80, 100),
        .period_ms = 400,
        .cell = 12,
        .height = 220,
        .flicker = 200,
        .axis = VFX_AXIS_Y,
    };
    uint8_t st_a = 0, st_b = 0;
    SCENE1(a, &vfx_layer_fire_api, &cfg, &st_a);
    SCENE1(b, &vfx_layer_fire_api, &cfg, &st_b);

    struct vfx_rgb out_a[NPX], out_b[NPX];
    struct vfx_frame_ctx ctx_a = grid_ctx(), ctx_b = grid_ctx();

    ctx_a.speed = ctx_b.speed = 3;

    /* One renders every frame, the other only the frames we compare: a fire
     * that carried state would diverge between the two.
     */
    for (uint32_t t = 0; t <= 5000; t += 50) {
        ctx_a.time_ms = t;
        vfx_render_frame(&a, &ctx_a, out_a, NULL);

        if (t % 1000 != 0) {
            continue;
        }

        ctx_b.time_ms = t;
        vfx_render_frame(&b, &ctx_b, out_b, NULL);

        CHECK(memcmp(out_a, out_b, sizeof(out_a)) == 0, "the fire drifted by t=%u", t);
    }
}

static void test_comet_travels_and_wraps(void) {
    struct vfx_comet_cfg cfg = {
        .color = VFX_HSB(190, 90, 70),
        .head_color = VFX_HSB(190, 10, 100),
        .period_ms = 2000,
        .tail = 8,
        .count = 1,
        .axis = VFX_AXIS_STRIP,
    };
    uint8_t st = 0;
    SCENE1(scene, &vfx_layer_comet_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    ctx.speed = 3;

    int seen_start = 0, seen_end = 0;

    for (uint32_t t = 0; t < 2000; t += 50) {
        ctx.time_ms = t;
        vfx_render_frame(&scene, &ctx, out, NULL);

        int lit = 0;
        for (int i = 0; i < NPX; i++) {
            if (!rgb_is_black(out[i])) {
                lit++;
            }
        }

        CHECK(lit > 0 && lit <= cfg.tail + 1, "a comet should light its tail and no more, got %d",
              lit);
        if (lit == 0 || lit > cfg.tail + 1) {
            return;
        }

        if (!rgb_is_black(out[0])) {
            seen_start++;
        }
        if (!rgb_is_black(out[NPX - 1])) {
            seen_end++;
        }
    }

    CHECK(seen_start > 0 && seen_end > 0, "one lap should pass both ends (%d, %d)", seen_start,
          seen_end);
}

static void test_comet_count_spreads_them_out(void) {
    /* Two comets on an angular axis, half a turn apart, is the dual beacon
     * other keyboards ship as its own effect.
     */
    struct vfx_comet_cfg cfg = {
        .color = VFX_HSB(300, 90, 70),
        .head_color = 0,
        .period_ms = 2000,
        .tail = 20,
        .count = 2,
        .axis = VFX_AXIS_ANGLE,
    };
    uint8_t one_st = 0, two_st = 0;
    struct vfx_comet_cfg one_cfg = cfg;
    one_cfg.count = 1;

    SCENE1(one, &vfx_layer_comet_api, &one_cfg, &one_st);
    SCENE1(two, &vfx_layer_comet_api, &cfg, &two_st);

    struct vfx_rgb out_one[NPX], out_two[NPX];
    struct vfx_frame_ctx ctx = grid_ctx();
    ctx.speed = 3;
    ctx.time_ms = 700;

    vfx_render_frame(&one, &ctx, out_one, NULL);
    vfx_render_frame(&two, &ctx, out_two, NULL);

    int lit_one = 0, lit_two = 0;
    for (int i = 0; i < NPX; i++) {
        if (!rgb_is_black(out_one[i])) {
            lit_one++;
        }
        if (!rgb_is_black(out_two[i])) {
            lit_two++;
        }
    }

    CHECK(lit_two > lit_one, "two comets should light more than one (%d vs %d)", lit_two, lit_one);
}

/* ---- zones written as keys ----------------------------------------------- */

static void test_key_zone_resolves_through_the_key_map(void) {
    /* "The modifiers" is a statement about keys. The engine knows which pixel
     * each key sits nearest, so the zone should not need LED indices.
     */
    static const uint8_t keys[] = {0, 5, 14, 30};
    static uint8_t pixels[4];
    static struct vfx_zone zone;
    const struct vfx_key_zone kz = {
        .keys = keys, .num_keys = 4, .pixels = pixels, .zone = &zone};

    struct vfx_frame_ctx ctx = grid_ctx(); /* identity key map over 36 pixels */

    vfx_key_zone_resolve(&kz, &ctx);

    CHECK(zone.len == 4, "all four keys are on this half, got %u", zone.len);
    CHECK(zone.pixels == pixels, "the zone should point at its own scratch");

    for (int i = 0; i < 4; i++) {
        CHECK(vfx_zone_pixel(&zone, (uint16_t)i) == keys[i], "key %d resolved to pixel %u", i,
              vfx_zone_pixel(&zone, (uint16_t)i));
    }
}

static void test_key_zone_keeps_only_this_halfs_keys(void) {
    /* Both halves of a split are given the same list, and each keeps the keys
     * whose pixel lands on its own strip. That is what lets one zone
     * definition cover the whole board.
     */
    static uint8_t key_map[NPX * 2];

    for (int i = 0; i < NPX * 2; i++) {
        key_map[i] = (uint8_t)i; /* key n sits on virtual pixel n */
    }

    static const uint8_t keys[] = {2, 10, 40, 50, 71};
    static uint8_t left_px[5], right_px[5];
    static struct vfx_zone left_zone, right_zone;

    const struct vfx_key_zone left = {
        .keys = keys, .num_keys = 5, .pixels = left_px, .zone = &left_zone};
    const struct vfx_key_zone right = {
        .keys = keys, .num_keys = 5, .pixels = right_px, .zone = &right_zone};

    struct vfx_frame_ctx lctx = test_ctx(), rctx = test_ctx();

    lctx.virtual_length = rctx.virtual_length = NPX * 2;
    lctx.key_pixels = rctx.key_pixels = key_map;
    lctx.num_keys = rctx.num_keys = NPX * 2;
    rctx.strip_offset = NPX;

    vfx_key_zone_resolve(&left, &lctx);
    vfx_key_zone_resolve(&right, &rctx);

    CHECK(left_zone.len == 2, "keys 2 and 10 are on the left half, got %u", left_zone.len);
    CHECK(right_zone.len == 3, "keys 40, 50 and 71 are on the right half, got %u",
          right_zone.len);

    /* And the right half's indices are local to its own strip. */
    CHECK(vfx_zone_pixel(&right_zone, 0) == 40 - NPX, "key 40 should be local pixel %d on the "
          "right half, got %u", 40 - NPX, vfx_zone_pixel(&right_zone, 0));
    CHECK(vfx_zone_pixel(&right_zone, 2) == 71 - NPX, "key 71 should be the last pixel");

    /* Together they cover every key exactly once, which is the property that
     * makes a whole-board zone work on a split at all.
     */
    CHECK(left_zone.len + right_zone.len == 5, "a key went missing or was counted twice");
}

static void test_key_zone_scopes_a_layer(void) {
    /* And the resolved zone behaves like any other: the layer lights those
     * pixels and no others.
     */
    static const uint8_t keys[] = {3, 9, 27};
    static uint8_t pixels[3];
    static struct vfx_zone zone;
    const struct vfx_key_zone kz = {
        .keys = keys, .num_keys = 3, .pixels = pixels, .zone = &zone};

    struct vfx_frame_ctx ctx = grid_ctx();

    vfx_key_zone_resolve(&kz, &ctx);

    struct vfx_solid_cfg cfg = {.color = VFX_HSB(0, 0, 100)};
    struct vfx_solid_state st = {0};
    static struct vfx_layer layers[1];

    layers[0] = (struct vfx_layer){.api = &vfx_layer_solid_api,
                                   .zone = &zone,
                                   .config = &cfg,
                                   .state = &st,
                                   .blend = VFX_BLEND_NORMAL,
                                   .opacity = 255};
    struct vfx_scene scene = {.name = "k", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX];
    vfx_render_frame(&scene, &ctx, out, NULL);

    for (int i = 0; i < NPX; i++) {
        const bool want = i == 3 || i == 9 || i == 27;

        CHECK(!rgb_is_black(out[i]) == want, "pixel %d lit=%d, expected %d", i,
              !rgb_is_black(out[i]), want);
        if (!rgb_is_black(out[i]) != want) {
            return;
        }
    }
}

/* ---- the new indicators -------------------------------------------------- */

static void test_flag_follows_locks_and_modifiers(void) {
    /* Caps lock is the host's state, not the keyboard's, so all this layer
     * does is render a bit someone else set. What matters is that it draws
     * nothing when clear, so it can sit over another effect.
     */
    struct vfx_flag_cfg cfg = {
        .color = VFX_HSB(0, 90, 100), .source = VFX_FLAG_LOCKS, .mask = VFX_LOCK_CAPS};
    uint8_t st = 0;
    SCENE1(scene, &vfx_layer_flag_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    struct vfx_status *status = vfx_status_mutable();
    bool lit = true;

    status->locks = 0;
    status->modifiers = 0;

    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "caps off must draw nothing at all");

    status->locks = VFX_LOCK_CAPS;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "caps on must light the zone");

    /* Another lock must not trigger a caps indicator. */
    status->locks = VFX_LOCK_NUM;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "num lock must not light a caps indicator");

    /* The mask takes any of its bits. */
    cfg.mask = VFX_LOCK_CAPS | VFX_LOCK_NUM;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "a mask of caps-or-num should take num lock");

    /* Same generator, modifiers instead. */
    cfg.source = VFX_FLAG_MODIFIERS;
    cfg.mask = VFX_MOD_SHIFT;
    status->locks = 0;
    status->modifiers = 0;
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "no modifiers held means nothing lit");

    status->modifiers = 0x02; /* left shift */
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "left shift should light a VFX_MOD_SHIFT layer");

    status->modifiers = 0x20; /* right shift */
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(lit, "and so should right shift");

    status->modifiers = 0x01; /* left control */
    vfx_render_frame(&scene, &ctx, out, &lit);
    CHECK(!lit, "control must not light a shift layer");

    status->modifiers = 0;
}

static void test_wpm_colours_and_fills(void) {
    struct vfx_wpm_cfg cfg = {.idle_color = VFX_HSB(210, 90, 40),
                              .fast_color = VFX_HSB(0, 90, 100),
                              .full = 80,
                              .bar = 0};
    uint8_t st = 0;
    SCENE1(scene, &vfx_layer_wpm_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    struct vfx_status *status = vfx_status_mutable();

    status->wpm = 0;
    vfx_render_frame(&scene, &ctx, out, NULL);
    const struct vfx_rgb idle = out[0];

    status->wpm = 80;
    vfx_render_frame(&scene, &ctx, out, NULL);
    const struct vfx_rgb fast = out[0];

    CHECK(memcmp(&idle, &fast, sizeof(idle)) != 0, "typing speed should change the colour");
    CHECK(fast.r > idle.r, "flat out should be redder than at rest");

    /* Past `full` it saturates rather than wrapping the hue round. */
    status->wpm = 200;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(memcmp(&fast, &out[0], sizeof(fast)) == 0, "above `full` should clamp, not wrap");

    /* As a bar it fills instead. */
    cfg.bar = 1;
    status->wpm = 40; /* half of full */
    vfx_render_frame(&scene, &ctx, out, NULL);

    int lit = 0;
    for (int i = 0; i < NPX; i++) {
        if (!rgb_is_black(out[i])) {
            lit++;
        }
    }

    CHECK(lit > NPX / 3 && lit < NPX * 2 / 3, "half speed should fill about half the bar, got %d",
          lit);

    status->wpm = 0;
}

static void test_peripheral_battery_distinguishes_unknown_from_flat(void) {
    /* A half that has not reported yet is not a half that is about to die,
     * and drawing it as an empty bar would say the wrong thing.
     */
    struct vfx_peripheral_battery_cfg cfg = {
        .low_color = VFX_HSB(0, 100, 100),
        .high_color = VFX_HSB(120, 100, 100),
        .empty_color = 0,
        .unknown_color = VFX_HSB(240, 60, 30),
        .source = 0,
        .warn_below = 20,
    };
    uint8_t st = 0;
    SCENE1(scene, &vfx_layer_peripheral_battery_api, &cfg, &st);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();
    struct vfx_status *status = vfx_status_mutable();

    status->peripheral_battery[0] = 0; /* never reported */
    vfx_render_frame(&scene, &ctx, out, NULL);

    int lit = 0;
    for (int i = 0; i < NPX; i++) {
        if (!rgb_is_black(out[i])) {
            lit++;
        }
    }
    CHECK(lit == NPX, "an unreported half should show its own colour across the zone, got %d",
          lit);
    CHECK(out[0].b > out[0].r, "and that colour should be the configured blue");

    /* Once it reports, it is an ordinary bar. */
    status->peripheral_battery[0] = 50;
    vfx_render_frame(&scene, &ctx, out, NULL);

    lit = 0;
    for (int i = 0; i < NPX; i++) {
        if (!rgb_is_black(out[i])) {
            lit++;
        }
    }
    CHECK(lit == NPX / 2, "half a cell should fill half the bar, got %d of %d", lit, NPX);

    /* And a flat one warns. */
    status->peripheral_battery[0] = 10;
    vfx_render_frame(&scene, &ctx, out, NULL);
    CHECK(out[0].r > out[0].g, "below warn-below the bar should be the low colour");

    status->peripheral_battery[0] = 0;
}

/* ---- scene transitions --------------------------------------------------- */

static void test_transition_mixes_between_scenes(void) {
    /* A scene switch that cuts is jarring, and a fade is the one thing the
     * compositor can do that a single scene cannot express.
     */
    struct vfx_solid_cfg red_cfg = {.color = VFX_HSB(0, 100, 100)};
    struct vfx_solid_cfg blue_cfg = {.color = VFX_HSB(240, 100, 100)};
    struct vfx_solid_state red_st = {0}, blue_st = {0};

    SCENE1(red, &vfx_layer_solid_api, &red_cfg, &red_st);
    SCENE1(blue, &vfx_layer_solid_api, &blue_cfg, &blue_st);

    struct vfx_rgb out[NPX], scratch[NPX], plain[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    /* At either end the mix must be exactly the scene, or a fade would blink
     * at the moment it starts and again when it finishes.
     */
    vfx_render_frame(&red, &ctx, plain, NULL);
    vfx_render_transition(&red, &blue, &ctx, 0, out, scratch, NULL);
    CHECK(memcmp(out, plain, sizeof(out)) == 0, "t=0 must be exactly the outgoing scene");

    vfx_render_frame(&blue, &ctx, plain, NULL);
    vfx_render_transition(&red, &blue, &ctx, 255, out, scratch, NULL);
    CHECK(memcmp(out, plain, sizeof(out)) == 0, "t=255 must be exactly the incoming scene");

    /* And the middle is genuinely between the two. */
    vfx_render_transition(&red, &blue, &ctx, 128, out, scratch, NULL);
    CHECK(out[0].r > 0 && out[0].b > 0, "half way should show both scenes, got r=%d b=%d",
          out[0].r, out[0].b);

    /* Monotonic: red gives way to blue over the fade, never doubling back. */
    int last_r = 256, last_b = -1;

    for (int t = 0; t <= 255; t += 15) {
        vfx_render_transition(&red, &blue, &ctx, (uint8_t)t, out, scratch, NULL);

        CHECK(out[0].r <= last_r, "red went back up at t=%d", t);
        CHECK(out[0].b >= last_b, "blue went back down at t=%d", t);

        if (out[0].r > last_r || out[0].b < last_b) {
            return;
        }

        last_r = out[0].r;
        last_b = out[0].b;
    }
}

static void test_transition_keeps_the_rail_up_until_it_finishes(void) {
    /* Fading from a lit scene to a black one, the frame is lit the whole way.
     * Reporting it as black early would gate the rail mid-fade and cut the
     * tail off it.
     */
    struct vfx_solid_cfg lit_cfg = {.color = VFX_HSB(120, 100, 100)};
    struct vfx_solid_cfg dark_cfg = {.color = VFX_HSB(0, 0, 0)};
    struct vfx_solid_state a_st = {0}, b_st = {0};

    SCENE1(lit_scene, &vfx_layer_solid_api, &lit_cfg, &a_st);
    SCENE1(dark_scene, &vfx_layer_solid_api, &dark_cfg, &b_st);

    struct vfx_rgb out[NPX], scratch[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    for (int t = 0; t < 255; t += 15) {
        bool lit = false;

        vfx_render_transition(&lit_scene, &dark_scene, &ctx, (uint8_t)t, out, scratch, &lit);
        CHECK(lit, "the frame must count as lit at t=%d, part way out of a lit scene", t);

        if (!lit) {
            return;
        }
    }
}

static void test_isqrt(void) {
    CHECK(vfx_isqrt(0) == 0, "isqrt(0)");
    CHECK(vfx_isqrt(1) == 1, "isqrt(1)");
    CHECK(vfx_isqrt(100) == 10, "isqrt(100)");
    CHECK(vfx_isqrt(101) == 10, "isqrt truncates");
    CHECK(vfx_isqrt(65535u * 65535u) == 65535, "isqrt at the top of the range");

    for (uint32_t n = 0; n < 5000; n++) {
        const uint32_t r = vfx_isqrt(n);
        CHECK(r * r <= n && (r + 1) * (r + 1) > n, "isqrt(%u) = %u is not the floor", n, r);
        if (!(r * r <= n && (r + 1) * (r + 1) > n)) break;
    }
}

/* ------------------------------------------------------------------ tuning */

/* A layer carrying a tune-id can be adjusted while the keyboard runs, which
 * nothing else about a layer can be: the rest is fixed in flash by devicetree.
 */
static const struct vfx_zone tune_zone = {.pixels = NULL, .start = 0, .len = NPX};
static const struct vfx_solid_cfg tune_cfg = {.color = VFX_HSB(0, 100, 100)};
static struct vfx_solid_state tune_st;

static struct vfx_rgb render_tuned(uint8_t tune_id) {
    const struct vfx_layer layers[] = {{
        .api = &vfx_layer_solid_api,
        .zone = &tune_zone,
        .config = &tune_cfg,
        .state = &tune_st,
        .blend = VFX_BLEND_NORMAL,
        .opacity = 255,
        .tune_id = tune_id,
    }};
    const struct vfx_scene scene = {.name = "tune", .layers = layers, .num_layers = 1};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(&scene, &ctx, out, NULL);

    return out[0];
}

static void test_tuning_leaves_untuned_layers_alone(void) {
    vfx_tuning_reset_all();
    CHECK(vfx_tuning_set_level(1, 0), "slot 1 must be settable");

    /* Slot 0 means the layer never opted in, so dimming slot 1 to nothing
     * must not reach it.
     */
    struct vfx_rgb px = render_tuned(0);

    CHECK(px.r > 0, "an untuned layer must ignore the tuning table");
}

static void test_tuning_level_dims(void) {
    vfx_tuning_reset_all();

    const struct vfx_rgb full = render_tuned(1);

    CHECK(vfx_tuning_set_level(1, 64), "slot 1 must be settable");

    const struct vfx_rgb dimmed = render_tuned(1);

    CHECK(dimmed.r < full.r && dimmed.r > 0, "level must dim rather than cut: %d -> %d",
          full.r, dimmed.r);
}

static void test_tuning_hue_rotates(void) {
    vfx_tuning_reset_all();

    const struct vfx_rgb red = render_tuned(1);

    CHECK(red.r > red.g, "the layer starts red");

    CHECK(vfx_tuning_set_hue(1, 120), "slot 1 must be settable");

    const struct vfx_rgb green = render_tuned(1);

    CHECK(green.g > green.r, "a hue of 120 must turn it green: %d,%d,%d", green.r, green.g,
          green.b);
}

static void test_tuning_rejects_bad_slots(void) {
    CHECK(!vfx_tuning_set_level(0, 128), "slot 0 means untuned and must be refused");
    CHECK(!vfx_tuning_set_level(VFX_TUNE_SLOTS, 128), "a slot past the end must be refused");
    CHECK(vfx_tuning_get(0) == NULL, "slot 0 must read as absent");
    CHECK(vfx_tuning_get(VFX_TUNE_SLOTS) == NULL, "a slot past the end must read as absent");
}

static void test_tuning_does_not_leak_between_layers(void) {
    vfx_tuning_reset_all();
    CHECK(vfx_tuning_set_hue(1, 120), "slot 1 must be settable");

    /* The tuned layer is drawn first and must not leave its adjustment on the
     * context the next layer is handed.
     */
    static const struct vfx_zone lower = {.pixels = NULL, .start = 0, .len = 1};
    static const struct vfx_zone upper = {.pixels = NULL, .start = 1, .len = 1};
    const struct vfx_layer layers[] = {
        {.api = &vfx_layer_solid_api, .zone = &lower, .config = &tune_cfg, .state = &tune_st,
         .blend = VFX_BLEND_NORMAL, .opacity = 255, .tune_id = 1},
        {.api = &vfx_layer_solid_api, .zone = &upper, .config = &tune_cfg, .state = &tune_st,
         .blend = VFX_BLEND_NORMAL, .opacity = 255, .tune_id = 0},
    };
    const struct vfx_scene scene = {.name = "leak", .layers = layers, .num_layers = 2};

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(&scene, &ctx, out, NULL);

    CHECK(out[0].g > out[0].r, "the tuned layer is rotated to green");
    CHECK(out[1].r > out[1].g, "the untuned layer beside it stays red: %d,%d,%d", out[1].r,
          out[1].g, out[1].b);
}

/* ---- host control: the raw-hid wire format, with no transport in scope -- */

static void test_hid_ping_decodes_and_pongs(void) {
    const uint8_t req[] = {VFX_HID_OP_PING};
    struct vfx_hid_request out;

    CHECK(vfx_hid_decode(req, sizeof(req), &out), "a bare PING is a complete request");
    CHECK(out.op == VFX_HID_OP_PING, "op must round-trip");

    uint8_t reply[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_pong(VFX_TUNE_SLOTS - 1, reply);

    CHECK(len == 3, "PONG is three bytes, got %d", len);
    CHECK(reply[0] == VFX_HID_REPLY_PONG, "PONG must be PING's op with the reply bit set");
    CHECK(reply[0] & VFX_HID_REPLY_BIT, "every reply must carry the reply bit");
    CHECK(reply[1] == VFX_HID_PROTOCOL_VERSION, "PONG must report the protocol version");
    CHECK(reply[2] == VFX_TUNE_SLOTS - 1, "PONG must report the highest usable slot");
}

static void test_hid_set_hue_decodes_a_negative_value(void) {
    /* -30 as int16 LE: 0xFFE2 -> E2, FF. */
    const uint8_t req[] = {VFX_HID_OP_SET_HUE, 3, 0xE2, 0xFF};
    struct vfx_hid_request out;

    CHECK(vfx_hid_decode(req, sizeof(req), &out), "a full SET_HUE report must decode");
    CHECK(out.op == VFX_HID_OP_SET_HUE, "op must round-trip");
    CHECK(out.slot == 3, "slot must round-trip, got %d", out.slot);
    CHECK(out.hue == -30, "hue must round-trip as signed, got %d", out.hue);
}

static void test_hid_set_level_and_speed_decode(void) {
    const uint8_t level_req[] = {VFX_HID_OP_SET_LEVEL, 2, 128};
    struct vfx_hid_request level_out;

    CHECK(vfx_hid_decode(level_req, sizeof(level_req), &level_out), "SET_LEVEL must decode");
    CHECK(level_out.slot == 2 && level_out.level == 128, "slot and level must round-trip: %d, %d",
          level_out.slot, level_out.level);

    const uint8_t speed_req[] = {VFX_HID_OP_SET_SPEED, 5, 4};
    struct vfx_hid_request speed_out;

    CHECK(vfx_hid_decode(speed_req, sizeof(speed_req), &speed_out), "SET_SPEED must decode");
    CHECK(speed_out.slot == 5 && speed_out.speed == 4, "slot and speed must round-trip: %d, %d",
          speed_out.slot, speed_out.speed);
}

static void test_hid_reset_and_get_decode(void) {
    const uint8_t reset_req[] = {VFX_HID_OP_RESET, 6};
    struct vfx_hid_request reset_out;

    CHECK(vfx_hid_decode(reset_req, sizeof(reset_req), &reset_out), "RESET must decode");
    CHECK(reset_out.slot == 6, "slot must round-trip, got %d", reset_out.slot);

    const uint8_t get_req[] = {VFX_HID_OP_GET, 1};
    struct vfx_hid_request get_out;

    CHECK(vfx_hid_decode(get_req, sizeof(get_req), &get_out), "GET must decode");
    CHECK(get_out.slot == 1, "slot must round-trip, got %d", get_out.slot);

    const uint8_t get_all_req[] = {VFX_HID_OP_GET_ALL};
    struct vfx_hid_request get_all_out;

    CHECK(vfx_hid_decode(get_all_req, sizeof(get_all_req), &get_all_out),
          "GET_ALL has no payload and must still decode");
}

static void test_hid_decode_rejects_short_reports(void) {
    /* SET_HUE needs three bytes past the op; two is a truncated report, not
     * a request with a default hue.
     */
    const uint8_t truncated[] = {VFX_HID_OP_SET_HUE, 1, 0x10};
    struct vfx_hid_request out;

    CHECK(!vfx_hid_decode(truncated, sizeof(truncated), &out),
          "a report shorter than its op needs must be refused");
    CHECK(!vfx_hid_decode(truncated, 0, &out), "an empty report must be refused");
}

static void test_hid_decode_rejects_unknown_op(void) {
    const uint8_t req[] = {0xEE, 1, 2, 3, 4};
    struct vfx_hid_request out;

    CHECK(!vfx_hid_decode(req, sizeof(req), &out), "an op outside the enum must be refused");
}

static void test_hid_ack_carries_the_requests_own_op(void) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_ack(VFX_HID_OP_SET_LEVEL, 4, VFX_HID_STATUS_OK, buf);

    CHECK(len == 3, "an ACK is three bytes, got %d", len);
    CHECK(buf[0] == (VFX_HID_OP_SET_LEVEL | VFX_HID_REPLY_BIT),
          "an ACK's op must be the request's own op with the reply bit set");
    CHECK(buf[1] == 4, "an ACK must echo the slot it was about");
    CHECK(buf[2] == VFX_HID_STATUS_OK, "status must round-trip");
}

static void test_hid_state_encodes_a_negative_hue(void) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    const uint8_t len = vfx_hid_encode_state(2, -30, 200, 3, VFX_HID_STATUS_OK, buf);

    CHECK(len == 7, "STATE is seven bytes, got %d", len);
    CHECK(buf[0] == VFX_HID_REPLY_STATE, "STATE must use GET's op with the reply bit set");
    CHECK(buf[1] == 2, "slot must round-trip");

    const int16_t hue = (int16_t)((uint16_t)buf[2] | ((uint16_t)buf[3] << 8));

    CHECK(hue == -30, "hue must round-trip through the wire as signed, got %d", hue);
    CHECK(buf[4] == 200 && buf[5] == 3, "level and speed must round-trip: %d, %d", buf[4], buf[5]);
    CHECK(buf[6] == VFX_HID_STATUS_OK, "status must round-trip");
}

/* ---- host control: the scene-authoring ops on top of the wire format --- */

static void test_hid_scene_add_layer_decodes_every_field(void) {
    const uint8_t req[] = {
        VFX_HID_OP_SCENE_ADD_LAYER,
        2,          /* ch */
        5,          /* type */
        10,         /* zone start */
        20,         /* zone len */
        1,          /* blend */
        200,        /* opacity */
        0x2C, 0x01, /* hue = 300 */
        80,         /* sat */
        90,         /* bri */
        0xE2, 0xFF, /* arg0 = -30 */
        0x64, 0x00, /* arg1 = 100 */
        0x00, 0x00, /* arg2 = 0 */
        0x01, 0x00, /* arg3 = 1 */
        0x03,       /* flags */
    };
    struct vfx_hid_request out;

    CHECK(vfx_hid_decode(req, sizeof(req), &out), "a full SCENE_ADD_LAYER report must decode");
    CHECK(out.ch == 2 && out.type == 5, "ch and type must round-trip: %d, %d", out.ch, out.type);
    CHECK(out.zone_start == 10 && out.zone_len == 20, "zone must round-trip: %d, %d",
        out.zone_start, out.zone_len);
    CHECK(out.blend == 1 && out.opacity == 200, "blend and opacity must round-trip: %d, %d",
        out.blend, out.opacity);
    CHECK(out.hue == 300 && out.sat == 80 && out.bri == 90, "colour must round-trip: %d,%d,%d",
        out.hue, out.sat, out.bri);
    CHECK(out.args[0] == -30 && out.args[1] == 100 && out.args[2] == 0 && out.args[3] == 1,
        "all four args must round-trip, including a negative one: %d,%d,%d,%d", out.args[0],
        out.args[1], out.args[2], out.args[3]);
    CHECK(out.flags == 3, "flags must round-trip");

    const uint8_t truncated[] = {VFX_HID_OP_SCENE_ADD_LAYER, 0, 0, 0, 0, 0, 0, 0, 0, 0, 0};

    CHECK(!vfx_hid_decode(truncated, sizeof(truncated), &out),
        "a SCENE_ADD_LAYER report short of its 19 payload bytes must be refused");
}

static void test_hid_scene_set_arg_and_color_decode(void) {
    const uint8_t arg_req[] = {VFX_HID_OP_SCENE_SET_ARG, 1, 3, 2, 0x38, 0xFF};
    struct vfx_hid_request arg_out;

    CHECK(vfx_hid_decode(arg_req, sizeof(arg_req), &arg_out), "SCENE_SET_ARG must decode");
    CHECK(arg_out.ch == 1 && arg_out.slot == 3 && arg_out.arg_idx == 2 && arg_out.args[0] == -200,
        "ch, slot, index and a negative value must all round-trip: %d,%d,%d,%d", arg_out.ch,
        arg_out.slot, arg_out.arg_idx, arg_out.args[0]);

    const uint8_t color_req[] = {VFX_HID_OP_SCENE_SET_COLOR, 0, 4, 0x2C, 0x01, 50, 60};
    struct vfx_hid_request color_out;

    CHECK(vfx_hid_decode(color_req, sizeof(color_req), &color_out), "SCENE_SET_COLOR must decode");
    CHECK(color_out.slot == 4 && color_out.hue == 300 && color_out.sat == 50 &&
              color_out.bri == 60,
        "slot and colour must all round-trip: %d,%d,%d,%d", color_out.slot, color_out.hue,
        color_out.sat, color_out.bri);
}

static void test_hid_scene_channel_only_ops_decode(void) {
    for (uint8_t op = VFX_HID_OP_SCENE_RESET; op <= VFX_HID_OP_SCENE_GET_INFO; op++) {
        if (op == VFX_HID_OP_SCENE_ADD_LAYER || op == VFX_HID_OP_SCENE_SET_ARG ||
            op == VFX_HID_OP_SCENE_SET_COLOR || op == VFX_HID_OP_SCENE_REMOVE_LAYER ||
            op == VFX_HID_OP_SCENE_MOVE_LAYER || op == VFX_HID_OP_SCENE_GET_LAYER) {
            continue; /* covered by their own tests; these carry more than ch */
        }

        const uint8_t req[] = {op, 3};
        struct vfx_hid_request out;

        CHECK(vfx_hid_decode(req, sizeof(req), &out), "op 0x%02x must decode from ch alone", op);
        CHECK(out.ch == 3, "op 0x%02x must carry ch through: got %d", op, out.ch);
    }
}

static void test_hid_scene_remove_and_move_decode(void) {
    const uint8_t remove_req[] = {VFX_HID_OP_SCENE_REMOVE_LAYER, 1, 5};
    struct vfx_hid_request remove_out;

    CHECK(vfx_hid_decode(remove_req, sizeof(remove_req), &remove_out),
        "SCENE_REMOVE_LAYER must decode");
    CHECK(remove_out.ch == 1 && remove_out.slot == 5, "ch and slot must round-trip: %d, %d",
        remove_out.ch, remove_out.slot);

    const uint8_t move_req[] = {VFX_HID_OP_SCENE_MOVE_LAYER, 1, 5, 0xFF};
    struct vfx_hid_request move_out;

    CHECK(vfx_hid_decode(move_req, sizeof(move_req), &move_out), "SCENE_MOVE_LAYER must decode");
    CHECK(move_out.direction == -1, "direction must decode as signed, got %d", move_out.direction);
}

static void test_hid_scene_info_and_layer_encode(void) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    uint8_t len = vfx_hid_encode_scene_info(2, 4, true, VFX_HID_STATUS_OK, buf);

    CHECK(len == 5, "SCENE_INFO is five bytes, got %d", len);
    CHECK(buf[0] == VFX_HID_REPLY_SCENE_INFO, "SCENE_INFO must use GET_INFO's op with the reply bit");
    CHECK(buf[1] == 2 && buf[2] == 4 && buf[3] == 1, "ch, count and active must round-trip: %d,%d,%d",
        buf[1], buf[2], buf[3]);

    const int16_t args[4] = {-30, 100, 0, 1};

    len = vfx_hid_encode_scene_layer(2, 5, 6, 10, 20, 1, 200, 300, 80, 90, args, 3,
                                     VFX_HID_STATUS_OK, buf);

    CHECK(len == 22, "SCENE_LAYER is 22 bytes, got %d", len);
    CHECK(buf[0] == VFX_HID_REPLY_SCENE_LAYER, "SCENE_LAYER must use GET_LAYER's op with the reply bit");
    CHECK(buf[1] == 2 && buf[2] == 5 && buf[3] == 6, "ch, slot and type must round-trip: %d,%d,%d",
        buf[1], buf[2], buf[3]);
    CHECK(buf[4] == 10 && buf[5] == 20, "zone must round-trip: %d, %d", buf[4], buf[5]);

    const int16_t hue = (int16_t)((uint16_t)buf[8] | ((uint16_t)buf[9] << 8));
    const int16_t arg0 = (int16_t)((uint16_t)buf[12] | ((uint16_t)buf[13] << 8));

    CHECK(hue == 300, "hue must round-trip through the wire, got %d", hue);
    CHECK(arg0 == -30, "a negative arg must round-trip through the wire, got %d", arg0);
    CHECK(buf[20] == 3, "flags must round-trip");
    CHECK(buf[21] == VFX_HID_STATUS_OK, "status must round-trip");
}

static void test_hid_scene_add_layer_ack_reports_the_assigned_slot(void) {
    uint8_t buf[VFX_HID_MAX_REPLY_LEN];
    /* SCENE_ADD_LAYER's success reply is an ordinary ACK, with slot set to
     * the id the pool assigned rather than one echoed from the request --
     * ADD_LAYER's request never named a slot in the first place.
     */
    const uint8_t len = vfx_hid_encode_ack(VFX_HID_OP_SCENE_ADD_LAYER, 4, VFX_HID_STATUS_OK, buf);

    CHECK(len == 3, "an ACK is three bytes regardless of which op it answers, got %d", len);
    CHECK(buf[0] == (VFX_HID_OP_SCENE_ADD_LAYER | VFX_HID_REPLY_BIT),
        "the reply op must be SCENE_ADD_LAYER's own with the reply bit set");
    CHECK(buf[1] == 4, "byte 1 must carry the newly assigned slot");
}

/* ---- runtime scene authoring -------------------------------------------
 *
 * A scene built through vfx_runtime_add_layer() and friends rather than by
 * devicetree, rendered through the exact same vfx_render_frame() every
 * compiled scene goes through -- these tests are about the pool (add,
 * remove, reorder, edit, persist), not about the generators, which their
 * own tests above already cover.
 */

static struct vfx_rt_params rt_solid(uint8_t start, uint8_t len, uint16_t hue) {
    return (struct vfx_rt_params){
        .type = VFX_RT_SOLID,
        .zone_start = start,
        .zone_len = len,
        .blend = VFX_BLEND_NORMAL,
        .opacity = 255,
        .hue = hue,
        .sat = 100,
        .bri = 100,
    };
}

static void test_runtime_add_layer_renders(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    const int slot = vfx_runtime_add_layer(0, &red);

    CHECK(slot == 0, "the first layer in an empty channel gets slot 0, got %d", slot);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);

    CHECK(out[0].r > out[0].g && out[0].r > out[0].b, "hue 0 must render red: %d,%d,%d", out[0].r,
          out[0].g, out[0].b);
}

static void test_runtime_empty_scene_is_null(void) {
    vfx_runtime_init();

    CHECK(vfx_runtime_scene(0) == NULL, "a channel with nothing added must have no scene yet");

    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    const int slot = vfx_runtime_add_layer(0, &red);

    vfx_runtime_remove_layer(0, (uint8_t)slot);

    CHECK(vfx_runtime_scene(0) == NULL, "removing the only layer must go back to no scene");
}

static void test_runtime_channels_are_independent(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX, 0);

    vfx_runtime_add_layer(0, &red);

    CHECK(vfx_runtime_scene(0) != NULL, "channel 0 got a layer");
    CHECK(vfx_runtime_scene(1) == NULL, "channel 1 got nothing and must have no scene of its own");
}

static void test_runtime_remove_layer_shifts_order(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    struct vfx_rt_params green = rt_solid(0, NPX, 120);
    const int first = vfx_runtime_add_layer(0, &red);
    const int second = vfx_runtime_add_layer(0, &green);

    vfx_runtime_remove_layer(0, (uint8_t)first);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);

    CHECK(out[0].g > out[0].r, "the remaining layer must still render: %d,%d,%d", out[0].r,
          out[0].g, out[0].b);

    /* The freed slot must be reusable rather than leaked. */
    struct vfx_rt_params blue = rt_solid(0, NPX, 240);
    const int reused = vfx_runtime_add_layer(0, &blue);

    CHECK(reused == first, "a freed slot must be handed back out, got %d instead of %d", reused,
          first);
    VFX_UNUSED(second);
}

static void test_runtime_move_layer_swaps_render_order(void) {
    vfx_runtime_init();

    /* Both fill the same zone at full opacity in NORMAL blend, so whichever
     * one renders *later* -- the higher render-order position -- is the one
     * that shows, same as the composer's own layer stack.
     */
    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    struct vfx_rt_params green = rt_solid(0, NPX, 120);
    const int first = vfx_runtime_add_layer(0, &red);
    const int second = vfx_runtime_add_layer(0, &green);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);
    CHECK(out[0].g > out[0].r, "green, added second, renders later and must show before the move");

    /* MOVE_UP means toward index 0 (earlier), same sense the composer's own
     * "Move earlier" button uses -- so moving `second` up swaps it with
     * `first`, leaving order [second, first] and `first` rendering last.
     */
    CHECK(vfx_runtime_move_layer(0, (uint8_t)second, VFX_RT_MOVE_UP),
          "moving the later layer earlier must succeed with a neighbour to swap with");

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);
    CHECK(out[0].r > out[0].g, "red must render last, and so show, after the swap: %d,%d,%d",
          out[0].r, out[0].g, out[0].b);

    CHECK(!vfx_runtime_move_layer(0, (uint8_t)second, VFX_RT_MOVE_UP),
        "second is now at index 0 and has no earlier position to swap with");
    CHECK(!vfx_runtime_move_layer(0, (uint8_t)first, VFX_RT_MOVE_DOWN),
        "first is now at the last index and has no later position to swap with");
}

static void test_runtime_set_color_and_arg_rebuild_the_layer(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    const int slot = vfx_runtime_add_layer(0, &red);

    struct vfx_rgb out[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);
    CHECK(out[0].r > out[0].g, "starts red");

    CHECK(vfx_runtime_set_color(0, (uint8_t)slot, 120, 100, 100), "set_color must succeed");

    vfx_render_frame(vfx_runtime_scene(0), &ctx, out, NULL);
    CHECK(out[0].g > out[0].r, "set_color must actually change what renders: %d,%d,%d", out[0].r,
          out[0].g, out[0].b);

    /* Breathe's min_level is args[1]: raising it must lift the floor a fully
     * decayed breathe still shows, which is the one argument easy to see
     * without stepping through an animation.
     */
    struct vfx_rt_params dim_breathe = {
        .type = VFX_RT_BREATHE,
        .zone_start = 0,
        .zone_len = NPX,
        .blend = VFX_BLEND_NORMAL,
        .opacity = 255,
        .hue = 0,
        .sat = 100,
        .bri = 100,
        .args = {4000, 0, 0, 0},
    };
    const int b = vfx_runtime_add_layer(1, &dim_breathe);

    vfx_render_frame(vfx_runtime_scene(1), &ctx, out, NULL);
    const uint8_t floor_before = out[0].r;

    CHECK(vfx_runtime_set_arg(1, (uint8_t)b, 1, 200), "set_arg must succeed");

    vfx_render_frame(vfx_runtime_scene(1), &ctx, out, NULL);
    CHECK(out[0].r > floor_before, "raising min_level must raise the floor: %d -> %d",
          floor_before, out[0].r);
}

static void test_runtime_pool_full_rejects_further_adds(void) {
    vfx_runtime_init();

    int last = -1;

    for (int i = 0; i < VFX_RT_MAX_LAYERS; i++) {
        struct vfx_rt_params p = rt_solid(0, 1, 0);

        last = vfx_runtime_add_layer(0, &p);
        CHECK(last >= 0, "layer %d of a full pool must still be accepted", i);
    }

    struct vfx_rt_params overflow = rt_solid(0, 1, 0);

    CHECK(vfx_runtime_add_layer(0, &overflow) == -1,
        "a pool already at VFX_RT_MAX_LAYERS must refuse one more");
    VFX_UNUSED(last);
}

static void test_runtime_rejects_bad_slots_and_channels(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX, 0);
    const int slot = vfx_runtime_add_layer(0, &red);

    CHECK(vfx_runtime_add_layer(VFX_MAX_CHANNELS, &red) == -1,
        "a channel past the end must be refused");
    CHECK(!vfx_runtime_set_arg(0, (uint8_t)(slot + 1), 0, 0),
        "a slot nothing was ever added to must be refused");
    CHECK(!vfx_runtime_set_color(0, (uint8_t)(slot + 1), 0, 0, 0),
        "set_color on an unused slot must be refused");
    CHECK(!vfx_runtime_remove_layer(0, (uint8_t)(slot + 1)),
        "removing an unused slot must be refused");
    CHECK(!vfx_runtime_set_arg(0, (uint8_t)slot, 4, 0), "an arg index past 0-3 must be refused");
}

static void test_runtime_activate_is_independent_of_scene_content(void) {
    vfx_runtime_init();

    CHECK(!vfx_runtime_is_active(0), "a channel starts inactive");
    CHECK(vfx_runtime_set_active(0, true), "activating must succeed");
    CHECK(vfx_runtime_is_active(0), "must read back active");

    /* Activation and content are independent: a channel can be switched on
     * with nothing built yet, which is what lets a host activate first and
     * then build live rather than having to stage everything invisibly.
     */
    CHECK(vfx_runtime_scene(0) == NULL, "still no scene with nothing added");

    struct vfx_rt_params red = rt_solid(0, NPX, 0);

    vfx_runtime_add_layer(0, &red);
    CHECK(vfx_runtime_scene(0) != NULL, "adding a layer while active must take effect immediately");
}

static void test_runtime_reset_clears_layers_but_keeps_active(void) {
    vfx_runtime_init();
    vfx_runtime_set_active(0, true);

    struct vfx_rt_params red = rt_solid(0, NPX, 0);

    vfx_runtime_add_layer(0, &red);
    vfx_runtime_reset(0);

    CHECK(vfx_runtime_scene(0) == NULL, "reset must clear every layer");
    CHECK(vfx_runtime_is_active(0),
        "reset must not deactivate the channel: a host resets to rebuild live, not to hand "
        "control back to the compiled list");
}

static void test_runtime_get_info_and_get_layer_report_the_pool(void) {
    vfx_runtime_init();

    uint8_t count;
    bool active;

    CHECK(vfx_runtime_get_info(0, &count, &active) && count == 0 && !active,
        "an untouched channel reports zero layers and inactive");

    struct vfx_rt_params red = rt_solid(3, 5, 0);
    const int slot = vfx_runtime_add_layer(0, &red);

    vfx_runtime_set_active(0, true);

    CHECK(vfx_runtime_get_info(0, &count, &active) && count == 1 && active,
        "get_info must reflect the layer just added and the activation");

    struct vfx_rt_params out;

    CHECK(vfx_runtime_get_layer(0, (uint8_t)slot, &out), "get_layer must find a used slot");
    CHECK(out.zone_start == 3 && out.zone_len == 5,
        "get_layer must return what was actually built, not defaults: %d,%d", out.zone_start,
        out.zone_len);
}

static void test_runtime_save_and_restore_round_trips(void) {
    vfx_runtime_init();

    struct vfx_rt_params red = rt_solid(0, NPX / 2, 0);
    struct vfx_rt_params green = rt_solid(NPX / 2, NPX / 2, 120);

    vfx_runtime_add_layer(0, &red);
    vfx_runtime_add_layer(0, &green);
    vfx_runtime_set_active(0, true);

    struct vfx_rgb before[NPX];
    struct vfx_frame_ctx ctx = test_ctx();

    vfx_render_frame(vfx_runtime_scene(0), &ctx, before, NULL);

    uint16_t len;
    const void *blob = vfx_runtime_state(&len);
    uint8_t copy[4096];

    CHECK(len <= sizeof(copy), "test buffer must be large enough for the real blob (%u bytes)",
          len);
    memcpy(copy, blob, len);

    /* Simulates a reboot: the live pool is gone, only the saved bytes are
     * left, exactly as though settings had just been loaded from flash into
     * a fresh boot image.
     */
    vfx_runtime_init();
    CHECK(vfx_runtime_scene(0) == NULL, "the fresh pool must start empty");

    CHECK(vfx_runtime_restore_state(copy, len), "restoring a blob of the right length must succeed");
    CHECK(vfx_runtime_is_active(0), "restore must bring the activation flag back");

    struct vfx_rgb after[NPX];

    vfx_render_frame(vfx_runtime_scene(0), &ctx, after, NULL);

    for (int i = 0; i < NPX; i++) {
        CHECK(before[i].r == after[i].r && before[i].g == after[i].g && before[i].b == after[i].b,
            "pixel %d must render the same after restore: %d,%d,%d -> %d,%d,%d", i, before[i].r,
            before[i].g, before[i].b, after[i].r, after[i].g, after[i].b);
    }

    CHECK(!vfx_runtime_restore_state(copy, (uint16_t)(len - 1)),
        "a blob of the wrong length must be refused rather than misread");
}

int main(void) {
    struct {
        const char *name;
        void (*fn)(void);
    } tests[] = {
        {"hsb matches float reference", test_hsb_matches_float_reference},
        {"hsb edges", test_hsb_edges},
        {"hue wrap", test_hue_wrap},
        {"lerp takes short way", test_lerp_takes_short_way},
        {"blend identities", test_blend_identities},
        {"gamma monotonic", test_gamma_monotonic},
        {"null scene is black", test_null_scene_is_black},
        {"zone masking", test_zone_masking},
        {"zone clamped to short strip", test_zone_clamped_to_short_strip},
        {"split render matches whole board", test_split_render_matches_whole_board},
        {"gradient scrolls", test_gradient_scrolls},
        {"static scene reports idle", test_static_scene_reports_idle},
        {"opacity zero layer skipped", test_opacity_zero_layer_skipped},
        {"brightness scales to black", test_brightness_scales_to_black},
        {"gate waits the blackout delay", test_gate_waits_the_blackout_delay},
        {"gate does not thrash on a blinking scene", test_gate_does_not_thrash_on_a_blinking_scene},
        {"gate wakes on a lit frame after settling", test_gate_wakes_on_a_lit_frame_after_settling},
        {"gate stability tracks the countdown", test_gate_stability_tracks_the_countdown},
        {"forced gate matches a normal gate", test_force_gated_matches_a_normal_gate},
        {"current estimate", test_current_estimate},
        {"ripple fires, decays and goes idle", test_ripple_fires_decays_and_goes_idle},
        {"ripple travels outward", test_ripple_travels_outward},
        {"ripple retired once it leaves the strip", test_ripple_retired_once_it_leaves_the_strip},
        {"ripple slots reuse oldest", test_ripple_slots_reuse_oldest},
        {"trail decay is time based", test_trail_decay_is_time_based_not_frame_based},
        {"twinkle identical on both halves", test_twinkle_is_identical_on_both_halves},
        {"battery indicator fills proportionally", test_battery_indicator_fills_proportionally},
        {"ble profile lights selected slot", test_ble_profile_lights_only_the_selected_slot},
        {"layer state picks colour by layer", test_layer_state_picks_colour_by_layer},
        {"sync jumps on first beacon", test_sync_jumps_on_first_beacon},
        {"sync slews small corrections", test_sync_slews_small_corrections},
        {"sync converges and settles", test_sync_converges_and_settles},
        {"sync handles negative drift", test_sync_handles_negative_drift},
        {"sync desired survives wrapping", test_sync_desired_survives_wrapping},
        {"water still until disturbed", test_water_still_until_disturbed},
        {"water rain runs without keys", test_water_rain_runs_without_keys},
        {"water wavefront travels outward", test_water_wavefront_travels_outward},
        {"water drops interfere", test_water_drops_interfere},
        {"water rain identical on both halves", test_water_rain_identical_on_both_halves},
        {"water still surface can be transparent", test_water_still_surface_can_be_transparent},
        {"water typing preset gates the rail", test_water_typing_preset_gates_the_rail},
        {"atan2 matches the real thing", test_atan2_matches_the_real_thing},
        {"axes fall back to the strip", test_axes_fall_back_to_the_strip},
        {"axes measure the board", test_axes_measure_the_board},
        {"gradient runs along its axis", test_gradient_runs_along_its_axis},
        {"pinwheel sweeps around the board", test_pinwheel_sweeps_around_the_board},
        {"plasma is two dimensional with a map", test_plasma_is_two_dimensional_with_a_map},
        {"twinkle hue spread stays deterministic", test_twinkle_hue_spread_stays_deterministic},
        {"cross lights the row and column", test_cross_lights_the_row_and_column},
        {"cross axes select one arm", test_cross_axes_select_one_arm},
        {"cross radius makes a nexus", test_cross_radius_makes_a_nexus},
        {"cross decays and goes idle", test_cross_decays_and_goes_idle},
        {"cross falls back to the strip", test_cross_falls_back_to_the_strip},
        {"fire burns upward", test_fire_burns_upward},
        {"fire is identical on both halves", test_fire_is_identical_on_both_halves},
        {"comet travels and wraps", test_comet_travels_and_wraps},
        {"comet count spreads them out", test_comet_count_spreads_them_out},
        {"key zone resolves through the key map", test_key_zone_resolves_through_the_key_map},
        {"key zone keeps only this half's keys", test_key_zone_keeps_only_this_halfs_keys},
        {"key zone scopes a layer", test_key_zone_scopes_a_layer},
        {"flag follows locks and modifiers", test_flag_follows_locks_and_modifiers},
        {"wpm colours and fills", test_wpm_colours_and_fills},
        {"peripheral battery distinguishes unknown from flat",
         test_peripheral_battery_distinguishes_unknown_from_flat},
        {"transition mixes between scenes", test_transition_mixes_between_scenes},
        {"transition keeps the rail up until it finishes",
         test_transition_keeps_the_rail_up_until_it_finishes},
        {"isqrt", test_isqrt},
        {"distance falls back to the strip", test_distance_falls_back_to_the_strip},
        {"distance is across the board with a map", test_distance_is_across_the_board_with_a_map},
        {"ripple radiates on the board", test_ripple_radiates_on_the_board_not_the_wire},
        {"trail deposits by board distance", test_trail_deposits_by_board_distance},
        {"matrix keypress falls from the top to the key",
         test_matrix_keypress_falls_from_the_top_to_the_key},
        {"matrix keypress stops at the key", test_matrix_keypress_stops_at_the_key},
        {"matrix head is brighter than its trail", test_matrix_head_is_brighter_than_its_trail},
        {"matrix stays in its column", test_matrix_stays_in_its_column},
        {"matrix idle without rain", test_matrix_idle_without_rain},
        {"matrix rain runs without keys", test_matrix_rain_runs_without_keys},
        {"matrix rain identical on both halves", test_matrix_rain_identical_on_both_halves},
        {"matrix rain lands where the leds are", test_matrix_rain_lands_where_the_leds_are},
        {"matrix falls back to the strip", test_matrix_falls_back_to_the_strip},
        {"tuning leaves untuned layers alone", test_tuning_leaves_untuned_layers_alone},
        {"tuning level dims", test_tuning_level_dims},
        {"tuning hue rotates", test_tuning_hue_rotates},
        {"tuning rejects bad slots", test_tuning_rejects_bad_slots},
        {"tuning does not leak between layers", test_tuning_does_not_leak_between_layers},
        {"hid ping decodes and pongs", test_hid_ping_decodes_and_pongs},
        {"hid set hue decodes a negative value", test_hid_set_hue_decodes_a_negative_value},
        {"hid set level and speed decode", test_hid_set_level_and_speed_decode},
        {"hid reset and get decode", test_hid_reset_and_get_decode},
        {"hid decode rejects short reports", test_hid_decode_rejects_short_reports},
        {"hid decode rejects unknown op", test_hid_decode_rejects_unknown_op},
        {"hid ack carries the request's own op", test_hid_ack_carries_the_requests_own_op},
        {"hid state encodes a negative hue", test_hid_state_encodes_a_negative_hue},
        {"hid scene add layer decodes every field", test_hid_scene_add_layer_decodes_every_field},
        {"hid scene set arg and color decode", test_hid_scene_set_arg_and_color_decode},
        {"hid scene channel only ops decode", test_hid_scene_channel_only_ops_decode},
        {"hid scene remove and move decode", test_hid_scene_remove_and_move_decode},
        {"hid scene info and layer encode", test_hid_scene_info_and_layer_encode},
        {"hid scene add layer ack reports the assigned slot",
         test_hid_scene_add_layer_ack_reports_the_assigned_slot},
        {"runtime add layer renders", test_runtime_add_layer_renders},
        {"runtime empty scene is null", test_runtime_empty_scene_is_null},
        {"runtime channels are independent", test_runtime_channels_are_independent},
        {"runtime remove layer shifts order", test_runtime_remove_layer_shifts_order},
        {"runtime move layer swaps render order", test_runtime_move_layer_swaps_render_order},
        {"runtime set color and arg rebuild the layer",
         test_runtime_set_color_and_arg_rebuild_the_layer},
        {"runtime pool full rejects further adds", test_runtime_pool_full_rejects_further_adds},
        {"runtime rejects bad slots and channels", test_runtime_rejects_bad_slots_and_channels},
        {"runtime activate is independent of scene content",
         test_runtime_activate_is_independent_of_scene_content},
        {"runtime reset clears layers but keeps active",
         test_runtime_reset_clears_layers_but_keeps_active},
        {"runtime get info and get layer report the pool",
         test_runtime_get_info_and_get_layer_report_the_pool},
        {"runtime save and restore round trips", test_runtime_save_and_restore_round_trips},
    };

    for (unsigned i = 0; i < sizeof(tests) / sizeof(tests[0]); i++) {
        int before = failures;

        printf("%s\n", tests[i].name);
        tests[i].fn();

        if (failures == before) {
            printf("  ok\n");
        }
    }

    printf("\n%s (%d failure%s)\n", failures ? "FAILED" : "PASSED", failures,
           failures == 1 ? "" : "s");

    return failures ? 1 : 0;
}
