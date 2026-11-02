#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Report filters as SQL parameters (:name): options from a read-only query, "all" = NULL, a chosen value
# bound as a literal; the report period as :period_start / :period_end; filters saved with the report;
# the assistant adds a filter and cards that use it (options query checked locally, like the cards).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
vis() { block "$1" "$2" | grep "^rptvis $3 "; }
P=build/rpt-filter; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
rm -f $P/semantic.json
OUT=$PWD/build/rpt-filter-out; rm -rf "$OUT"; mkdir -p "$OUT"
pf=build/rpt_filter_mock.port; log=build/rpt_filter_mock.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out=$(LUME_TEST_SAVE_DIR=$OUT LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/rpt_filter.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
f() { printf '%s\n' "$out" | grep '^rptfilter' | sed -n "$1p"; }
expect "'all' (no value): the card shows every region" 'vis "$out" 1 v1 | grep -q "data=ok rows=4"'
expect "the filter's options come from its read-only query" 'f 1 | grep -q "name=reg .*value=\"\" options=4"'
expect "a chosen value filters the card (bound as a literal)" 'vis "$out" 2 v1 | grep -q "data=ok rows=1"'
n12=$(sqlite3 $P/shop.db "SELECT count(DISTINCT region) FROM rpt_sales WHERE status = 'paid' AND ordered_at >= '2025-11-01' AND ordered_at < '2026-11-01'")
expect "the report period reaches SQL cards through :period_start / :period_end" 'vis "$out" 3 v1 | grep -q "data=ok rows=$n12"'
expect "filters are saved with the report" 'grep -q "\"parameters\":\[{\"name\":\"reg\"" "$OUT/Filtros.lrpt"'
expect "the assistant's proposal adds a filter; its options query was checked locally like the cards" 'block "$out" 4 | grep -q "ast=awaiting-approval" && printf "%s\n" "$out" | grep -q "^rptstep add_filter \[done\] Região (:regiao)"'
expect "approved: the new filter exists and filters the assistant's card" 'f 2 | grep -q "name=regiao label=\"Região\"" && vis "$out" 5 v1 | grep -q "data=ok rows=1"'
expect "the assistant was told about filters and period parameters" 'grep -q "\"periodParameters\":\":period_start, :period_end\"" $log'
if [ $fails = 0 ]; then echo "RPT FILTER UI: all passed"; else echo "RPT FILTER UI: FAILED ($fails failures)"; fi
exit $fails
