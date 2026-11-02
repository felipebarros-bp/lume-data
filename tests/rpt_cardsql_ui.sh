#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# The selected card's query in the properties panel: an embedded SQL editor with the card's SQL, Apply
# (read-only policy, undoable, refreshes the data), the last run's rows or the database's error, and the
# compiled SQL of catalog cards shown read-only.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
cs() { printf '%s\n' "$out" | grep '^rptcardsql' | sed -n "$1p"; }
P=build/rpt-cardsql; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
before=$(sqlite3 $P/shop.db "SELECT count(*) FROM rpt_sales")
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/rpt_cardsql.txt ./build/lume 2>&1)
expect "the panel shows the card's SQL in an editable editor, with the last run (rows)" 'cs 1 | grep -q "ro=0 .*status=ok rows=2 .*editor=\"SELECT status AS label"'
expect "a one-line query is shown formatted: one clause per line, indented (whitespace only)" '[ "$(printf "%s\n" "$out" | grep "^rptcardlines" | sed -n 1p | cut -d" " -f2)" -ge 3 ]'
expect "Apply changes the card's query and its data refreshes" 'cs 2 | grep -q "status=ok rows=4 .*editor=\"SELECT region AS label"'
expect "a failing query shows the database's error on the panel" 'cs 3 | grep -q "status=error .*err=\".*no such column: nope"'
expect "a write is refused by the read-only policy before it runs; the database is unchanged" 'cs 4 | grep -q "msg=\"Recusado pela política somente-leitura" && [ "$(sqlite3 $P/shop.db "SELECT count(*) FROM rpt_sales")" = "$before" ]'
expect "a catalog card shows the SQL Lume compiled, read-only" 'cs 5 | grep -q "ro=1 .*editor=\"SELECT .*net_total"'
if [ $fails = 0 ]; then echo "RPT CARD SQL UI: all passed"; else echo "RPT CARD SQL UI: FAILED ($fails failures)"; fi
exit $fails
