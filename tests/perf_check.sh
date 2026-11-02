#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the grid model timings (US-052 benchmark): 100k x 4, 20k x 200, long strings, big JSON.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O2 -Wall -Wno-unused-parameter -Isrc -Itests/shim -o build/perf_check tests/perf_check.c src/grid.c src/rs.c src/json.c src/geo.c src/sysinfo.c tests/rs_stub.c -lm
build/perf_check
