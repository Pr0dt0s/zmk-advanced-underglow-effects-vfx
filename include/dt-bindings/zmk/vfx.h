/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

/* Colors are packed into a single devicetree cell: 0x00HHSSBB.
 * h: 0-359 degrees, s: 0-100 percent, b: 0-100 percent.
 */
#define VFX_HSB(h, s, b) ((((h) & 0x1FF) << 16) | (((s) & 0xFF) << 8) | ((b) & 0xFF))

#define VFX_HSB_H(v) (((v) >> 16) & 0x1FF)
#define VFX_HSB_S(v) (((v) >> 8) & 0xFF)
#define VFX_HSB_B(v) ((v) & 0xFF)

#define VFX_BLACK VFX_HSB(0, 0, 0)
#define VFX_WHITE VFX_HSB(0, 0, 100)

/* What a layer's opacity can be made to follow, so that something the
 * keyboard already knows drives how strongly an effect shows rather than the
 * effect having to be rewritten to care.
 */
#define VFX_SRC_NONE 0     /* fixed at the layer's own opacity */
#define VFX_SRC_WPM 1      /* typing speed */
#define VFX_SRC_BATTERY 2  /* charge remaining */
#define VFX_SRC_ACTIVITY 3 /* whether anyone is at the keyboard */

/* Blend modes for stacking a layer onto what is already composited. */
#define VFX_BLEND_NORMAL 0
#define VFX_BLEND_ADD 1
#define VFX_BLEND_MULTIPLY 2
#define VFX_BLEND_SCREEN 3
#define VFX_BLEND_MAX 4

/* Which way an effect runs across the board. Everything but VFX_AXIS_STRIP
 * needs pixel-positions on the engine node; without it they all fall back to
 * the strip, so a scene stays animated on a board that has no map.
 */
#define VFX_AXIS_STRIP 0  /* along the wire */
#define VFX_AXIS_X 1      /* left to right */
#define VFX_AXIS_Y 2      /* top to bottom */
#define VFX_AXIS_RADIAL 3 /* out from the middle */
#define VFX_AXIS_ANGLE 4  /* around the middle: a pinwheel */
#define VFX_AXIS_SPIRAL 5 /* around and outward at once */

/* Which arms a cross draws from the pressed key. */
#define VFX_CROSS_BOTH 0       /* the row and the column */
#define VFX_CROSS_HORIZONTAL 1 /* the row only: a band across the board */
#define VFX_CROSS_VERTICAL 2   /* the column only */

/* Host lock LEDs, in the bit order the USB HID LED page uses. These are the
 * host's state, not the keyboard's: it only knows them because the host sends
 * an LED report, so they need CONFIG_ZMK_HID_INDICATORS.
 */
#define VFX_LOCK_NUM 0x01
#define VFX_LOCK_CAPS 0x02
#define VFX_LOCK_SCROLL 0x04
#define VFX_LOCK_COMPOSE 0x08
#define VFX_LOCK_KANA 0x10

/* Modifiers, either side counting as held. */
#define VFX_MOD_CTRL 0x11
#define VFX_MOD_SHIFT 0x22
#define VFX_MOD_ALT 0x44
#define VFX_MOD_GUI 0x88

/* Which of those a flag layer reads. */
#define VFX_FLAG_LOCKS 0
#define VFX_FLAG_MODIFIERS 1

/* &vfx behavior commands. */
#define VFX_TOG_CMD 0
#define VFX_ON_CMD 1
#define VFX_OFF_CMD 2
#define VFX_NEXT_CMD 3
#define VFX_PREV_CMD 4
#define VFX_SEL_CMD 5
#define VFX_BRI_CMD 6
#define VFX_BRD_CMD 7
#define VFX_SPI_CMD 8
#define VFX_SPD_CMD 9
#define VFX_HUI_CMD 10
#define VFX_HUD_CMD 11

/* Absolute forms. A relative command pressed on the central is rewritten into
 * one of these before it is relayed, so both halves land on the same value
 * instead of each applying its own increment to its own starting point. This
 * is the mechanism ZMK's own rgb_ug behavior uses.
 */
#define VFX_SET_SCENE_CMD 12
#define VFX_SET_BRT_CMD 13
#define VFX_SET_SPD_CMD 14
#define VFX_SET_HUE_CMD 15

/* Split synchronisation, sent central to peripheral rather than typed in a
 * keymap. VFX_SYNC carries the central's timebase; VFX_KEY relays a key
 * position so the peripheral can draw a ripple for a key it cannot see.
 */
#define VFX_SYNC_CMD 16
#define VFX_KEY_CMD 17

/* Adjusting one tuning slot rather than the whole channel, for layers that
 * opted in with a tune-id. param2 packs the slot into its high byte and the
 * value into the low one, since param1 is already carrying the command and
 * the channel.
 */
#define VFX_TUNE_HUE_CMD 18
#define VFX_TUNE_LEVEL_CMD 19
#define VFX_TUNE_SPEED_CMD 20
#define VFX_TUNE_RESET_CMD 21

#define VFX_TOG VFX_TOG_CMD 0
#define VFX_ON VFX_ON_CMD 0
#define VFX_OFF VFX_OFF_CMD 0
#define VFX_NEXT VFX_NEXT_CMD 0
#define VFX_PREV VFX_PREV_CMD 0
#define VFX_SEL(n) VFX_SEL_CMD n
#define VFX_BRI VFX_BRI_CMD 0
#define VFX_BRD VFX_BRD_CMD 0
#define VFX_SPI VFX_SPI_CMD 0
#define VFX_SPD VFX_SPD_CMD 0
#define VFX_HUI VFX_HUI_CMD 0
#define VFX_HUD VFX_HUD_CMD 0
#define VFX_SET_BRT(v) VFX_SET_BRT_CMD v
#define VFX_SET_SPD(v) VFX_SET_SPD_CMD v
#define VFX_SET_HUE(v) VFX_SET_HUE_CMD v

/* Channels.
 *
 * A board can split its strip into channels, each with its own scene list and
 * its own scene, brightness, speed and hue, so that the underglow and the
 * per-key LEDs run different effects at different brightnesses at once. They
 * are declared as zmk,vfx-channel children of the engine, and a channel's id
 * is its position in that list.
 *
 * The channel rides in param1 above the command, because param2 is already
 * spoken for by the commands that carry a value. A zero there means every
 * channel, so every unqualified macro above keeps addressing the whole board.
 */
#define VFX_CMD_MASK 0xFF
#define VFX_CH_SHIFT 8

#define VFX_CH_0 0
#define VFX_CH_1 1
#define VFX_CH_2 2
#define VFX_CH_3 3

/* Stored one higher than the id so that an absent channel reads as zero. */
#define VFX_CMD_ON(cmd, ch) ((cmd) | (((ch) + 1) << VFX_CH_SHIFT))

#define VFX_TOG_ON(ch) VFX_CMD_ON(VFX_TOG_CMD, ch) 0
#define VFX_ON_ON(ch) VFX_CMD_ON(VFX_ON_CMD, ch) 0
#define VFX_OFF_ON(ch) VFX_CMD_ON(VFX_OFF_CMD, ch) 0
#define VFX_NEXT_ON(ch) VFX_CMD_ON(VFX_NEXT_CMD, ch) 0
#define VFX_PREV_ON(ch) VFX_CMD_ON(VFX_PREV_CMD, ch) 0
#define VFX_SEL_ON(ch, n) VFX_CMD_ON(VFX_SEL_CMD, ch) n
#define VFX_BRI_ON(ch) VFX_CMD_ON(VFX_BRI_CMD, ch) 0
#define VFX_BRD_ON(ch) VFX_CMD_ON(VFX_BRD_CMD, ch) 0
#define VFX_SPI_ON(ch) VFX_CMD_ON(VFX_SPI_CMD, ch) 0
#define VFX_SPD_ON(ch) VFX_CMD_ON(VFX_SPD_CMD, ch) 0
#define VFX_HUI_ON(ch) VFX_CMD_ON(VFX_HUI_CMD, ch) 0
#define VFX_HUD_ON(ch) VFX_CMD_ON(VFX_HUD_CMD, ch) 0
#define VFX_SET_BRT_ON(ch, v) VFX_CMD_ON(VFX_SET_BRT_CMD, ch) v
#define VFX_SET_SPD_ON(ch, v) VFX_CMD_ON(VFX_SET_SPD_CMD, ch) v
#define VFX_SET_HUE_ON(ch, v) VFX_CMD_ON(VFX_SET_HUE_CMD, ch) v

/* Adjusting a tuning slot, for layers carrying a matching tune-id.
 *
 * These address a slot rather than a channel, because a slot is a handle on
 * particular layers wherever they sit. Hue is in degrees, level 0-255, speed
 * 1-5 with 0 handing the layer back to its channel.
 *
 *   &vfx VFX_TUNE_LEVEL(1, 80)   dim whatever is on slot 1
 */
#define VFX_TUNE_ARG(slot, v) ((((slot) & 0xFF) << 8) | ((v) & 0xFF))

#define VFX_TUNE_HUE(slot, deg) VFX_TUNE_HUE_CMD ((((slot) & 0xFF) << 16) | ((deg) & 0x1FF))
#define VFX_TUNE_LEVEL(slot, v) VFX_TUNE_LEVEL_CMD VFX_TUNE_ARG(slot, v)
#define VFX_TUNE_SPEED(slot, v) VFX_TUNE_SPEED_CMD VFX_TUNE_ARG(slot, v)
#define VFX_TUNE_RESET(slot) VFX_TUNE_RESET_CMD VFX_TUNE_ARG(slot, 0)
