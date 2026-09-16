# Copyright (c) 2026 The ZMK VFX Contributors
# SPDX-License-Identifier: MIT
#
# Builds the compositor against no Zephyr at all. If this stops working, the
# engine core has grown a kernel dependency it should not have.

CC      ?= cc
CFLAGS  ?= -std=c11 -Wall -Wextra -Werror -O2
CPPFLAGS = -DVFX_SIM -I../include
LDLIBS   = -lm

CORE = ../src/color.c ../src/zone.c ../src/render.c \
       ../src/layers/solid.c ../src/layers/gradient.c

.PHONY: test clean
test: test_core
	./test_core

test_core: test_core.c $(CORE)
	$(CC) $(CFLAGS) $(CPPFLAGS) $^ $(LDLIBS) -o $@

clean:
	rm -f test_core
