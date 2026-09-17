# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT
#
# Builds the compositor against no Zephyr at all. If this stops working, the
# engine core has grown a kernel dependency it should not have.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS = -DVFX_SIM -I../include
LDLIBS   = -lm

CORE = ../src/color.c ../src/zone.c ../src/render.c ../src/power_policy.c \
       ../src/mathtab.c ../src/status.c ../src/sync_policy.c \
       ../src/layers/solid.c ../src/layers/gradient.c ../src/layers/ambient.c \
       ../src/layers/reactive.c ../src/layers/indicators.c ../src/layers/water.c ../src/layers/matrix.c ../src/layers/cross.c ../src/layers/fire.c ../src/layers/comet.c

.PHONY: test clean
test: test_core
	./test_core

# Headers are listed as prerequisites so that changing a constant in one of
# them rebuilds: a stale binary here reports a pass against code that is no
# longer what is committed.
HEADERS = $(wildcard ../include/zmk/vfx/*.h ../include/dt-bindings/zmk/*.h)

test_core: test_core.c $(CORE) $(HEADERS)
	$(CC) $(CFLAGS) $(CPPFLAGS) test_core.c $(CORE) $(LDLIBS) -o $@

clean:
	rm -f test_core
