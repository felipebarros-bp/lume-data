#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# The Output's text view: text can be selected with the mouse (drag, double click a word, triple click a
# line) and Cmd+C copies the selection, not everything. Uses the system clipboard: do not copy while it runs.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/output-select; rm -rf $P; mkdir -p $P
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/output_select.txt ./build/lume 2>&1)
clip=$(printf '%s\n' "$out" | awk '/^clipboard-begin/{on=1; next} /^clipboard-end/{on=0} on')
printf '%s\n' "$out" > build/output_select.out
expect "a triple click selects one line of the text view and Cmd+C copies only it" '[ -n "$clip" ] && [ "$(printf "%s\n" "$clip" | wc -l | tr -d " ")" = 1 ]'
if [ $fails = 0 ]; then echo "OUTPUT SELECT UI: all passed"; else echo "OUTPUT SELECT UI: FAILED ($fails failures)"; fi
exit $fails
