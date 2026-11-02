#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Report Studio + Lume Intelligence for reports, headless (ASan): read-only policy, semantic catalog and
# SQL compiler, golden dataset on SQLite, ReportSpec, periods, formatting, AI proposals, tools, PNG.
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/rpt_check tests/rpt_check.c src/sqlguard.c src/sqllex.c src/sem.c src/jw.c src/json.c src/rpt.c src/agent.c src/pngw.c src/image.c src/demo_shop.c src/rtheme.c -lsqlite3 -lm || exit 1
build/rpt_check
