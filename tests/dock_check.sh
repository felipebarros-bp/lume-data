#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Docking engine checks without a GUI (LUME-PANEL-001, NFR-09 / PNL-044).
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -o build/dock_check tests/dock_check.c src/dock.c src/json.c -lm
build/dock_check
