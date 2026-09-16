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
