#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the splitter / async executor checks against SQLite.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/exec_check tests/exec_check.c src/exec.c src/db.c src/rs.c -lsqlite3 -lpthread
build/exec_check
