#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Result grid: dragging the vertical scroll bar with the mouse scrolls the rows.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/grid-sb; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/grid_scrollbar.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/grid_scrollbar.out
r() { printf '%s\n' "$out" | grep '^resstate' | sed -n "$1p"; }
expect "a result is shown, at the top" 'r 1 | grep -q "shown=1 .*scroll=0 "'
expect "dragging the scroll bar thumb scrolls the grid" 'r 2 | grep -q "shown=1 " && ! r 2 | grep -q "scroll=0 "'
if [ $fails = 0 ]; then echo "GRID SCROLLBAR UI: all passed"; else echo "GRID SCROLLBAR UI: FAILED ($fails failures)"; fi
exit $fails
