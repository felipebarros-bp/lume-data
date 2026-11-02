#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Runs the UI smoke script and converts the captures to PNG (macOS: sips).
set -e
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
cmake --build build >/dev/null
sh tests/grid_check.sh | tail -1
# an isolated app-data folder so the test never touches the real connections store
rm -rf build/pref && mkdir -p build/pref
LUME_PREF_DIR=build/pref LUME_SCRIPT=tests/smoke.txt ./build/lume
for f in build/shot_*.bmp; do
  command -v sips >/dev/null && sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"
done
ls build/shot_*
