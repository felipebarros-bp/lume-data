#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Visual Table Designer core, headless (ASan): golden DDL per dialect, diff, validation, risk, drafts, SQLite end to end.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/tbl_check tests/tbl_check.c src/tbl.c src/tbl_plan.c src/tbl_snap.c src/rs.c tests/rs_stub.c -lsqlite3
build/tbl_check
