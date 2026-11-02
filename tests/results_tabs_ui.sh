#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Results belong to the query tab that ran them (Isa, 01/11/2026): a run with two statements shows two
# results; running the tab again replaces them (one statement, one result); another query tab shows
# its own last run; going back shows the first tab's again.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/results-tabs; rm -rf $P; mkdir -p $P
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/results_tabs.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/results_tabs.out
r() { printf '%s\n' "$out" | grep '^resstate' | sed -n "$1p"; }
expect "two statements: two results for this tab" 'r 1 | grep -q "shown=2 "'
expect "running the tab again replaces its results (one statement: one result)" 'r 2 | grep -q "shown=1 total=1 "'
expect "another query tab shows only its own run" 'r 3 | grep -q "shown=1 total=2 " && r 3 | grep -q ":2$"'
expect "back on the first tab: its last run again" 'r 4 | grep -q "shown=1 total=2 " && r 4 | grep -q ":1$"'
expect "re-running a big result (its cursor still open) replaces it without touching the running session (no crash)" 'r 5 | grep -q "shown=1 " && ! printf "%s\n" "$out" | grep -q "FATAL signal"'
if [ $fails = 0 ]; then echo "RESULTS TABS UI: all passed"; else echo "RESULTS TABS UI: FAILED ($fails failures)"; fi
exit $fails
