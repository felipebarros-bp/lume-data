#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Lume Intelligence with no certified metrics: the user shares the database STRUCTURE (never rows), the
# assistant writes SQL cards, Lume checks them locally with WHERE 1=0 (no rows read) and the read-only
# policy, asks for one correction when a query fails, and marks the cards "AI-written SQL, not certified".
# Also: the e-commerce demo database opens. Loopback mock: tests/tools/ai_mock.py.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
st() { block "$1" "$2" | grep '^rptstate'; }

Q=build/rpt-aisql; rm -rf $Q; sh tests/rpt_setup_sqlite.sh $Q || { echo "FAIL  setup"; exit 1; }
rm -f $Q/semantic.json   # no certified metrics: the structure is the only way
before=$(sqlite3 $Q/shop.db "SELECT count(*) || ':' || total(net_total) FROM rpt_sales")
kpi=$(sqlite3 $Q/shop.db "SELECT printf('%.2f', sum(net_total)) FROM rpt_sales WHERE status = 'paid'")
pf=build/rpt_aisql_mock.port; log=build/rpt_aisql_mock.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_aisql.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf

expect "no catalog metrics: the assistant asks to share the structure first; nothing was sent before consent" 'st "$out" 1 | grep -q "ast=needs-schema" && grep -q "decision=needs-schema-local" $Q/audit/report.log && sed -n "1,/^=====/p" $log | grep -q "<schema" && [ "$(printf "%s\n" "$out" | sed -n "/^rptpayload$/,/^rptpayload-end$/p" | sed -n "1,/^rptpayload-end$/p" | grep -c "<schema")" = 0 ]'
expect "after consent: the structure (names, types, keys) is in the request; no row value is" 'grep -q "<schema connection=\"Shop\"" $log && grep -q "table rpt_sales(id INTEGER PK" $log && grep -q "customer_email TEXT" $log && ! grep -q "@example.com" $log && ! grep -qE "[0-9]{3,}\.[0-9]{2}" $log'
expect "the AI's SQL passed the local check (no rows read) and the plan waits for approval" 'st "$out" 2 | grep -q "ast=awaiting-approval" && grep -q "decision=schema-shared" $Q/audit/report.log'
expect "approved: KPI, monthly line and bars by region load from the AI's SQL, marked not certified" 'b=$(block "$out" 3); printf "%s\n" "$b" | grep -q "^rptvis v1 kind=kpi.*data=ok" && printf "%s\n" "$b" | grep -q "^rptvis v2 kind=line.*data=ok" && printf "%s\n" "$b" | grep -q "^rptvis v3 kind=bar.*data=ok" && [ "$(printf "%s\n" "$b" | grep -c "^rptaisql v[123] conn=shop1")" = 3 ]'
k=$(python3 -I -c "import sys; print(f'{float(sys.argv[1]):,.2f}'.replace(',', '_').replace('.', ',').replace('_', '.'))" "$kpi")
expect "the KPI equals SQLite's own sum of paid orders ($k)" 'block "$out" 3 | grep "^rptvis v1 " | grep -q "$k"'
expect "the saved report keeps the SQL and the not-certified mark" 'grep -q "\"aiSql\":true" $Q/reports/*.lrpt'
expect "a query that fails the local check goes back to the assistant once (error text and column names only), then the plan is valid" 'st "$out" 4 | grep -q "ast=awaiting-approval" && grep -q "decision=sql-check-retry" $Q/audit/report.log && grep -q "<sql_check" $log && grep -q "net_totl" $log && ! sed -n "/<sql_check/,/<\/sql_check>/p" $log | grep -q "@example.com"'
expect "SQL that writes is refused by the read-only policy before anything runs; the database is unchanged" 'st "$out" 5 | grep -q "ast=error amsg=\"The proposal was refused: the SQL of" && [ "$before" = "$(sqlite3 $Q/shop.db "SELECT count(*) || '"'"':'"'"' || total(net_total) FROM rpt_sales")" ]'
expect "the audit has decisions and sizes, never SQL, values or the key" '! grep -qi "select \|net_total\|sk-test\|@example" $Q/audit/report.log'
expect "category values (own consent): status and region values go with the structure; personal columns (customer_email) are never sampled" 'tail -c 4000 $log | grep -q "values status: " && tail -c 4000 $log | grep -q "values region: " && ! grep -q "values customer_email" $log && ! grep -q "@example.com" $log && grep -q "decision=item-on" $Q/audit/report.log'
expect "a proposal beyond the selected card asks for permission instead of refusing; allowed, the plan is built" 'st "$out" 7 | grep -q "ast=confirm-scope amsg=\"The card" && st "$out" 8 | grep -q "ast=awaiting-approval" && grep -q "decision=asks-page-scope" $Q/audit/report.log'
expect "the conversation text can be selected and copied (select all)" 'printf "%s\n" "$out" | grep -q "^rptsel .*SQLREPORT monte um relatório de vendas"'
expect "Up / Down in the question field recall earlier questions and come back to the draft" '[ "$(printf "%s\n" "$out" | grep "^rptq " | tr "\n" "|")" = "rptq \"SQLREPORT mais cards\"|rptq \"SQLREPORT de novo, com os valores\"|rptq \"SQLREPORT mais cards\"|rptq \"rascunho\"|" ]'
expect "page zoom: two steps in = 125 %" 'st "$out" 9 | grep -q "zoom=125"'
expect "the e-commerce demo database opens (Explorer: orders, order_items, products, customers...)" 'printf "%s\n" "$out" | grep -q "connected: Shop Demo (e-commerce) (sqlite), [0-9]* tables" || grep -q "connected: Shop Demo (e-commerce)" $Q/logs/*.log'

if [ $fails = 0 ]; then echo "RPT AI SQL UI: all passed"; else echo "RPT AI SQL UI: FAILED ($fails failures)"; fi
exit $fails
