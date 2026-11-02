#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# SQL editor: the horizontal scroll never goes past the text (short lines stay put; a mostly vertical
# trackpad swipe does not move sideways; a long line scrolls to its end plus a margin, no further).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/edscroll-prefs; rm -rf $P; mkdir -p $P
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/editor_hscroll.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/editor_hscroll.out
s() { printf '%s\n' "$out" | grep '^edscroll' | sed -n "$1p"; }
expect "a short line: a strong sideways scroll does not move the text" 's 1 | grep -q "sx=0 "'
expect "a mostly vertical swipe with a little horizontal motion stays put" 's 2 | grep -q "sx=0 "'
expect "a long line scrolls to its end plus a margin, never past it" 'v=$(s 3); sx=$(echo "$v" | sed "s/.*sx=\([0-9]*\).*/\1/"); wd=$(echo "$v" | sed "s/.*widest=\([0-9]*\).*/\1/"); vw=$(echo "$v" | sed "s/.*view=\([0-9]*\).*/\1/"); [ "$sx" -gt 0 ] && [ "$sx" -le $((wd - vw + 200)) ]'
expect "no script target missing" '! printf "%s\n" "$out" | grep -q "target not found\|unknown command"'
if [ $fails = 0 ]; then echo "EDITOR HSCROLL UI: all passed"; else echo "EDITOR HSCROLL UI: FAILED ($fails failures)"; fi
exit $fails
