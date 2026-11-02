#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Schema Explorer v2 model, headless: adapters' SQL, featured / Others, pages, search, counts, pins, persistence (170k simulated schemas).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/nsx_check tests/nsx_check.c src/nsx.c src/rs.c tests/rs_stub.c
build/nsx_check
