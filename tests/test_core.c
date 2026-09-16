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
