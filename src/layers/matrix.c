/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 *
 * Matrix rain: columns of light falling down the board, each a bright head
 * with a fading trail behind it.
 *
 * This is the first generator that cares which way is down, so it needs the
 * engine's position map. Without one it has no y axis to fall along and falls
 * back to treating strip order as the direction of travel, which still
 * animates but will not line up with the board.
 *
 * Drops come from two places, as with water. Ambient ones are a function of
 * the clock: a drop's column and speed are hashed from its epoch number, so
 * nothing is stored between frames and the two halves of a split agree on the
 * rain without exchanging anything. Keypress drops take a slot, and differ
 * from the rain in one way: they stop at the key. The drop falls in from the
 * top of the key's column and comes to rest on the key you pressed rather
 * than carrying on past it, so the motion points at the key instead of away
 * from it.
 */

#include <stddef.h>

#include <zmk/vfx/engine.h>
#include <zmk/vfx/layers.h>
#include <zmk/vfx/math.h>

static uint32_t age_of(uint32_t now, uint32_t start) { return now > start ? now - start : 0; }

/* Where a pixel sits along the axis drops travel, and which column it is in.
 * With a position map these are real board coordinates; without one, strip
 * order stands in for both so the effect still does something.
 */
static int32_t pixel_y(const struct vfx_frame_ctx *ctx, uint16_t vidx) {
    if (ctx->pixel_xy && vidx < ctx->num_positions) {
        return ctx->pixel_xy[vidx * 2 + 1];
    }

    return vidx;
}

/* How many columns are really in play. Without a position map there is no x
 * axis to divide up, so everything falls in a single column.
 */
static uint8_t active_columns(const struct vfx_matrix_state *st, const struct vfx_matrix_cfg *cfg) {
    return st->col_w == 0 ? 1 : cfg->columns;
}

static uint8_t pixel_column(const struct vfx_frame_ctx *ctx, const struct vfx_matrix_state *st,
                            const struct vfx_matrix_cfg *cfg, uint16_t vidx) {
    if (!ctx->pixel_xy || vidx >= ctx->num_positions || st->col_w == 0) {
        return 0;
    }

    const int32_t x = ctx->pixel_xy[vidx * 2] - st->min_x;
    const uint32_t col = (uint32_t)x / st->col_w;

    return (uint8_t)(col >= cfg->columns ? (uint32_t)cfg->columns - 1U : col);
}

/* Which column an ambient drop falls in. It picks a pixel and takes that
 * pixel's column rather than picking a column number, because a column number
 * can be one no LED sits in -- the gap between the halves of a split, or any
 * column the board does not reach once its width is divided up. Those would
 * otherwise swallow their share of the rain and leave it patchy, and on a
 * split they do not swallow an equal share from each half.
 */
static uint8_t ambient_column(const struct vfx_frame_ctx *ctx, const struct vfx_matrix_state *st,
                              const struct vfx_matrix_cfg *cfg, uint32_t hash) {
    if (ctx->pixel_xy && ctx->num_positions != 0) {
        return pixel_column(ctx, st, cfg, (uint16_t)(hash % ctx->num_positions));
    }

    return (uint8_t)(hash % active_columns(st, cfg));
}

/* Brightness this drop puts on a pixel, 0-255, and whether the pixel is in
 * the head. Returns 0 when the drop is elsewhere or has not reached it.
 */
static uint16_t drop_level(const struct vfx_matrix_cfg *cfg, const struct vfx_matrix_drop *drop,
                           uint32_t base_speed, uint8_t col, int32_t y, uint32_t age_ms,
                           bool *is_head) {
    if (drop->column != col || cfg->tail == 0) {
        return 0;
    }

    /* Nothing below where this drop stops. For rain that is past the bottom
     * of the board and never bites; for a keypress it is the key, which is
     * what makes the column land on it instead of running through it.
     */
    if (y > drop->y_end) {
        return 0;
    }

    const int32_t speed = (int32_t)base_speed * drop->speed_pct / 100;
    const int32_t head_y = drop->y0 + speed * (int32_t)age_ms / 1000;

    /* Behind the head means further up the board: the trail is what the drop
     * has already passed over. The head keeps travelling after it reaches the
     * stop point, which is what drains the trail into the key rather than
     * leaving it lit there.
     */
    const int32_t behind = head_y - y;

    if (behind < 0 || behind > cfg->tail) {
        return 0;
    }

    *is_head = behind <= cfg->head_size;

    return (uint16_t)(255 - (behind * 255) / cfg->tail);
}

/* The user's speed control, the same scaling the other moving generators use:
 * ctx->speed runs 1-5 with 3 as configured.
 */
static uint32_t fall_speed(const struct vfx_matrix_cfg *cfg, const struct vfx_frame_ctx *ctx) {
    return (uint32_t)cfg->speed * ctx->speed / 3U;
}

static void matrix_frame(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    const struct vfx_matrix_cfg *cfg = layer->config;
    struct vfx_matrix_state *st = layer->state;

    const struct vfx_board_box box = vfx_board_bounds(ctx);

    if (box.known) {
        st->min_x = box.min_x;
        st->min_y = box.min_y;
        /* +1 so the rightmost pixel lands inside the last column rather than
         * one past it.
         */
        st->col_w = (uint16_t)((box.max_x - box.min_x + 1 + cfg->columns - 1) / cfg->columns);
        st->fall_len = (uint16_t)(box.max_y - box.min_y);
    } else {
        st->min_x = 0;
        st->min_y = 0;
        st->col_w = 0;
        st->fall_len = ctx->virtual_length;
    }

    if (st->col_w == 0 && box.known) {
        st->col_w = 1;
    }

    /* A drop is done once its trail has drained past where it stopped. */
    const uint32_t speed = fall_speed(cfg, ctx);

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        struct vfx_matrix_drop *drop = &st->drops[i];

        if (!drop->active) {
            continue;
        }

        if (speed == 0) {
            continue;
        }

        const int32_t span = drop->y_end - drop->y0 + cfg->tail;
        const uint32_t life_ms = (uint32_t)(span > 0 ? span : 0) * 100000U / (speed * drop->speed_pct);

        if (age_of(ctx->time_ms, drop->start_ms) > life_ms) {
            drop->active = false;
        }
    }
}

static void matrix_key_event(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                             uint32_t position, bool pressed, uint32_t time_ms) {
    if (!pressed) {
        return;
    }

    const struct vfx_matrix_cfg *cfg = layer->config;
    struct vfx_matrix_state *st = layer->state;
    struct vfx_matrix_drop *slot = NULL;

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->drops[i].active) {
            slot = &st->drops[i];
            break;
        }
    }

    /* All busy: overwrite the oldest rather than drop the new press, so fast
     * typing keeps starting columns.
     */
    if (!slot) {
        slot = &st->drops[st->next % VFX_MAX_RIPPLES];
        st->next = (uint8_t)((st->next + 1) % VFX_MAX_RIPPLES);
    }

    const uint16_t vidx = vfx_key_pixel(ctx, position);

    slot->active = true;
    slot->start_ms = time_ms;
    slot->column = pixel_column(ctx, st, cfg, vidx);
    /* Starts at the top of the board rather than a tail above it, so the
     * column is lit the moment you press rather than after the trail has
     * fallen into view, and stops on the key so the whole thing points at
     * what you pressed.
     */
    slot->y0 = st->min_y;
    slot->y_end = pixel_y(ctx, vidx);
    slot->speed_pct = 100;
}

static bool matrix_pixel(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx,
                         uint16_t zone_i, uint16_t strip_i, struct vfx_rgb *out) {
    VFX_UNUSED(zone_i);

    const struct vfx_matrix_cfg *cfg = layer->config;
    const struct vfx_matrix_state *st = layer->state;

    const uint16_t vidx = vfx_virtual_idx(ctx, strip_i);
    const int32_t y = pixel_y(ctx, vidx);
    const uint8_t col = pixel_column(ctx, st, cfg, vidx);

    const uint32_t speed = fall_speed(cfg, ctx);

    uint16_t best = 0;
    bool head = false;

    /* Ambient rain, reproduced from the clock. */
    if (cfg->drop_rate_ms != 0 && cfg->columns != 0) {
        const uint32_t epoch = ctx->time_ms / cfg->drop_rate_ms;

        for (uint8_t i = 0; i < VFX_MATRIX_MAX_AMBIENT; i++) {
            if (epoch < i) {
                break;
            }

            const uint32_t e = epoch - i;
            const uint32_t hash = vfx_hash32(e * 0x9E3779B9U);

            struct vfx_matrix_drop drop = {
                .start_ms = e * cfg->drop_rate_ms,
                /* Begins a full trail above the board, so it falls in rather
                 * than appearing already lit.
                 */
                .y0 = (int32_t)st->min_y - cfg->tail,
                /* Rain runs off the bottom; only a keypress stops early. */
                .y_end = (int32_t)st->min_y + st->fall_len + cfg->tail,
                .column = ambient_column(ctx, st, cfg, hash),
                /* Spread the speeds a little so columns do not march in step. */
                .speed_pct = (uint8_t)(100 - cfg->jitter / 2 + ((hash >> 8) % (cfg->jitter + 1))),
                .active = true,
            };

            bool this_head = false;
            const uint16_t level =
                drop_level(cfg, &drop, speed, col, y, age_of(ctx->time_ms, drop.start_ms),
                           &this_head);

            if (level > best) {
                best = level;
                head = this_head;
            }
        }
    }

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (!st->drops[i].active) {
            continue;
        }

        bool this_head = false;
        const uint16_t level = drop_level(cfg, &st->drops[i], speed, col, y,
                                          age_of(ctx->time_ms, st->drops[i].start_ms), &this_head);

        if (level > best) {
            best = level;
            head = this_head;
        }
    }

    if (best == 0) {
        return false;
    }

    struct vfx_hsb hsb;

    if (head && cfg->head_color != 0) {
        hsb = vfx_hsb_unpack(cfg->head_color);
    } else {
        hsb = vfx_hsb_unpack(cfg->color);
        hsb.b = (uint8_t)((uint16_t)hsb.b * best / 255U);
    }

    if (hsb.b == 0) {
        return false;
    }

    hsb.h = vfx_hue_add(hsb.h, ctx->hue_shift);
    *out = vfx_hsb_to_rgb(hsb);

    return true;
}

static bool matrix_is_animating(const struct vfx_layer *layer, const struct vfx_frame_ctx *ctx) {
    VFX_UNUSED(ctx);

    const struct vfx_matrix_cfg *cfg = layer->config;
    const struct vfx_matrix_state *st = layer->state;

    /* Rain never stops. Without it the board is dark between keypresses, so
     * the engine can park its timer and the power gate can cut the rail.
     */
    if (cfg->drop_rate_ms != 0) {
        return true;
    }

    for (uint8_t i = 0; i < VFX_MAX_RIPPLES; i++) {
        if (st->drops[i].active) {
            return true;
        }
    }

    return false;
}

const struct vfx_layer_api vfx_layer_matrix_api = {
    .frame = matrix_frame,
    .pixel = matrix_pixel,
    .key_event = matrix_key_event,
    .is_animating = matrix_is_animating,
};
