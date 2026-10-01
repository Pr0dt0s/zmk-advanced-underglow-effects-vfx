/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 *
 * SPDX-License-Identifier: MIT
 */

#pragma once

#include <stdbool.h>
#include <stdint.h>

#include <zmk/vfx/hid_protocol.h>
#include <zmk/vfx/runtime_scene.h>

/* Keeps a split peripheral's runtime scenes the same as the central's: the
 * central compares hashes with CHECK messages (sync_proto.h) and rebuilds a
 * scene that disagrees by replaying it (vfx_runtime_replay_*). All of it is a
 * no-op on a peripheral and on a board that is not split, except
 * zmk_vfx_sync_on_check(), which is the peripheral's half.
 *
 * Learning that a peripheral disagrees needs the report channel
 * (CONFIG_ZMK_VFX_SPLIT_REPORT). Without it CHECK only makes the peripheral
 * log, and a rebuild has to be asked for by hand.
 */

/* `target` is a runtime-scene target or VFX_RT_NONE for every scene. -EINVAL
 * for any other value.
 */

/* Asks the peripherals to compare their copy against the central's now. */
int zmk_vfx_sync_verify(uint8_t target);

/* Rebuilds the scene on the peripherals from the central's copy. Mutating scene
 * ops answer BUSY until it has been sent.
 */
int zmk_vfx_sync_resync(uint8_t target);

/* A relayed edit changed `target`: compare it once edits go quiet. */
void zmk_vfx_sync_touch(uint8_t target);

/* A rebuild is being sent, so a host edit would land in the middle of it. */
bool zmk_vfx_sync_busy(void);

struct zmk_vfx_sync_info {
    uint8_t features; /* VFX_HID_SYNC_* */
    bool replaying;
    struct vfx_hid_peer peers[VFX_HID_MAX_PEERS];
    uint16_t queue_high_water;
    uint32_t refused;
};

void zmk_vfx_sync_get(struct zmk_vfx_sync_info *out);

/* The peripheral's handler for a CHECK link message. */
void zmk_vfx_sync_on_check(const uint8_t *msg, uint8_t len);
