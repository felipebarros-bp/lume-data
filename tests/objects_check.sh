#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Object source editor core, headless: formatter, diff, header parsing, signatures, planner, diagnostics.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/objects_check tests/objects_check.c src/objmodel.c src/objplan.c src/obj_sqlite.c src/obj_pg.c src/obj_oracle.c src/obj_mysql.c src/objdiag.c src/objlang.c src/sqllex.c src/sqlfmt.c src/textdiff.c src/rs.c tests/rs_stub.c
build/objects_check
