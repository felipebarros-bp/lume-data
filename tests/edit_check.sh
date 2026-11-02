#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the editing-model checks against SQLite (two connections on one file).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/edit_check tests/edit_check.c src/edit.c src/db.c src/grid.c src/rs.c src/json.c src/geo.c -lsqlite3 -lm
build/edit_check
