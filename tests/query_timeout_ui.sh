#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Timeouts from Settings take effect: the editor's query timeout cancels a statement that runs
# longer, and the session runs the next one.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/qtimeout-ui; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
printf 'query_timeout_s=2\n' > $P/settings.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/query_timeout.txt ./build/lume 2>&1)
L=$P/logs/lume.log
printf '%s\n' "$out" > build/query_timeout_ui.out
expect "a statement past the query timeout (2 s) is cancelled" 'grep -q "statement 1 passed the query timeout (2 s): cancel requested" $L'
expect "the status says the timeout cancelled it, and where it is set" 'grep -q "cancelled by the query timeout (2 s, Settings > Query Execution)" $L'
expect "the session runs the next statement afterwards" 'grep -qE "Loaded 1 row" $L'
if [ $fails = 0 ]; then echo "QUERY TIMEOUT UI: all passed"; else echo "QUERY TIMEOUT UI: FAILED ($fails failures)"; fi
exit $fails
