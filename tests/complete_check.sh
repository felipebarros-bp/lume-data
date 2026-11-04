#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Autocompletion, headless under ASan: JOIN ... ON conditions (foreign keys, composite, same-named keys).
cd "$(dirname "$0")/.."
mkdir -p build
F=$(pkg-config --cflags --libs sdl3)
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Wextra -Wno-unused-parameter -Isrc -o build/complete_check tests/complete_check.c src/complete.c src/objcat.c $F -lm || exit 1
ASAN_OPTIONS=detect_leaks=0 build/complete_check
