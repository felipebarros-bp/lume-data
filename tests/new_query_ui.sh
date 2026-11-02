#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Explorer: a connection's context menu offers New Query, a query tab bound to that connection
# (connecting first when it is not connected yet).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/new-query; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/new_query.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/new_query.out
expect "New Query on a connection that was not connected: it connects, the new tab runs on it" 'printf "%s\n" "$out" | grep "^resstate" | grep -q "shown=1 .*:1$" && ! printf "%s\n" "$out" | grep -q "target not found"'
if [ $fails = 0 ]; then echo "NEW QUERY UI: all passed"; else echo "NEW QUERY UI: FAILED ($fails failures)"; fi
exit $fails
