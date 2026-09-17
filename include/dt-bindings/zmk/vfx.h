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
