#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Duration formatting (units.c).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Isrc -o build/units_check tests/units_check.c src/units.c -lm
build/units_check
