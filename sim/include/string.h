/*
 * Copyright (c) 2026 The ZMK VFX Contributors
 * SPDX-License-Identifier: MIT
 *
 * The simulator is built with clang's bare wasm32 target and no libc, so there
 * is no <string.h>. The engine only needs these; vfx_sim.c defines them.
 */

#pragma once

#include <stddef.h>

void *memcpy(void *dst, const void *src, size_t n);
void *memset(void *dst, int c, size_t n);
