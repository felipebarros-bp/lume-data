#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the headless grid model checks (no SDL, no database).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/grid_check tests/grid_check.c src/grid.c src/rs.c src/json.c src/geo.c tests/rs_stub.c -lm
build/grid_check
