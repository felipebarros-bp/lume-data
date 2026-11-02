#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# File > Open (and drag and drop): a file opens by what it is. .sql / .md / text -> a query tab bound to
# the file, .lrpt -> a report, .db / .sqlite -> a connection; without a known extension the content
# decides (SQLite header, report JSON). The harness "open" takes the same path as the dialog's result.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
O=build/open-ui; rm -rf $O; mkdir -p $O/p
printf 'SELECT 42 AS answer;\n' > $O/q.sql
printf '{"formatVersion":1,"id":"rpt-open-test","revision":1,"title":"Aberto","pages":[{"id":"p1","name":"P","visuals":[]}]}\n' > $O/rel.lrpt
sqlite3 $O/data.db "CREATE TABLE t(x); INSERT INTO t VALUES(1);"
cp $O/data.db $O/dbnoext; cp $O/rel.lrpt $O/rptnoext; printf '# Title\ntext\n' > $O/n.md
{ echo "wait 300"; for f in q.sql rel.lrpt data.db dbnoext rptnoext n.md; do echo "open $PWD/$O/$f"; echo "wait 300"; done; echo "rptstate"; echo "quit"; } > $O/s.txt
out=$(LUME_PREF_DIR=$O/p LUME_SCRIPT=$O/s.txt ./build/lume 2>&1)
log=$O/p/logs/lume.log
expect ".sql opens as a query tab bound to the file" 'grep -q "open: text file (sql)" $log'
expect ".lrpt opens as a report" 'grep -q "open: report file (lrpt)" $log && [ "$(grep -c "report: opened" $log)" = 2 ]'
expect ".db opens as a SQLite connection" 'grep -q "open: database file (db)" $log && grep -q "connected: data.db (sqlite), 1 tables" $log'
expect "no extension: the SQLite header makes it a database, the report JSON a report" 'grep -q "connected: dbnoext (sqlite)" $log && [ "$(grep -c "open: report file (no extension)" $log)" = 1 ]'
expect ".md opens as text" 'grep -q "open: text file (md)" $log'
expect "both reports are open" 'printf "%s\n" "$out" | grep -q "^rptstate docs=2"'
if [ $fails = 0 ]; then echo "OPEN UI: all passed"; else echo "OPEN UI: FAILED ($fails failures)"; fi
exit $fails
