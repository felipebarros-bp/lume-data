#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the execution-output model checks (no SDL, no database).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Wno-missing-field-initializers -Isrc -o build/outlog_check tests/outlog_check.c src/outlog.c src/units.c -lm
build/outlog_check
