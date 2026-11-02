#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Lume Intelligence in a query tab (roadmap foundation 8-10, P1 12-14), loopback AI mock, SQLite:
# the assistant answers about the active query tab (its SQL, its last error); "Fix the error" proposes an
# edit that is applied to the editor only after the user applies it (one undo step) and only when the tab
# has not changed since; Explain answers in the chat; a new query opens in a new tab; SQL that changes data
# is flagged and never run by Lume; Cmd+K opens the assistant; an answer in prose is refused.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
line() { printf '%s\n' "$out" | grep "^$1" | sed -n "$2p"; }
P=build/ai-sql; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
before=$(sqlite3 $P/shop.db "SELECT count(*) FROM rpt_sales")
printf '# test glossary\nreceita líquida = rpt_sales.net_total of paid orders\n' > $P/glossary.txt
pf=build/ai_sql_mock.port; log=build/ai_sql_mock.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/ai_sql.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
printf '%s\n' "$out" > build/ai_sql.out
first=$(awk 'BEGIN{RS="=====\n"} NR==1' $log)
expect "a query tab is active: the assistant answers about it; Cmd+K opens it with the question box focused" 'line rptsql 1 | grep -q "qmode=1" && line rptsql 2 | grep -q "shown=1 focus=1"'
expect "what was sent: the tab's SQL, its dialect, the last run's error (no rows)" 'printf "%s" "$first" | grep -q "<editor dialect=\"sqlite\" connection=\"Shop\"" && printf "%s" "$first" | grep -q "<last_run untrusted=\"true\">" && printf "%s" "$first" | grep -q "no such column: net_totl" && ! printf "%s" "$first" | grep -q "Norte,"'
expect "the Output offers Fix with AI beside Go to SQL; the command runs the request" '! printf "%s\n" "$out" | grep -q "target not found: hover \"output:fix-with-ai\"" && grep -q "Corrija o erro" $log'
expect "Fix the error: an edit to review (the whole tab), not applied yet" 'block "$out" 1 | grep -q "ast=awaiting-approval" && line rptsql 3 | grep -q "kind=[0-9]* target=all writes=0 .*SUM(net_total)" && printf "%s\n" "$out" | grep -q "^rptstep propose_sql_edit \[done\] replaces the tab"'
expect "applied to the editor after the user applies it; Lume did not run it" 'line edtext 1 | grep -q "text=\"SELECT region, SUM(net_total) AS receita FROM rpt_sales GROUP BY region\"" && block "$out" 2 | grep -q "Applied to the editor (Cmd+Z undoes it). Lume did not run it."'
expect "Cmd+Z takes the edit back (one undo step)" 'line edtext 2 | grep -q "SUM(net_totl)"'
expect "a tab that changed since the request: nothing is applied" 'block "$out" 3 | grep -q "ast=error amsg=\"The tab changed since the request: nothing was applied"'
expect "each answer shows its tokens under it (in / out, and the conversation total)" 'printf "%s\n" "$out" | grep -q "^rptchat tokens: .* [0-9]* in .* [0-9]* out tokens .* conversation [0-9]"'
expect "Explain answers in the conversation" 'block "$out" 4 | grep -q "ast=idle" && grep -q "Explique" $log'
expect "a new query opens in a new tab, its change of data flagged; Lume never runs it" 'line rptsql 4 | grep -q "writes=1 .*DELETE FROM rpt_sales" && line rpttab 2 | grep -q "title=\"limpeza.sql\" conn=shop1 text=\"DELETE FROM rpt_sales WHERE status = .cancelled.\"" && block "$out" 5 | grep -q "Opened in a new tab. Lume did not run it." && [ "$(sqlite3 $P/shop.db "SELECT count(*) FROM rpt_sales")" = "$before" ]'
expect "design_table: a plan to open in the Table Designer; applied, a new unsaved table with its keys there, nothing created" 'block "$out" 6 | grep -q "ast=awaiting-approval" && printf "%s\n" "$out" | grep -q "^rptstep design_table \[done\] rpt_refunds: 4 column(s), primary key, 1 foreign key(s), 1 index(es)" && printf "%s\n" "$out" | grep "^tdstate" | tail -1 | grep -q "table=rpt_refunds new=1 .*cols=4 keys=3" && [ "$(sqlite3 $P/shop.db .tables | grep -c rpt_refunds)" = 0 ] && [ "$(sqlite3 $P/shop.db .tables | grep -c rpt_sales)" = 1 ]'
expect "Review with Lume Intelligence from the designer: its DDL goes to a query tab and is reviewed there" 'awk "BEGIN{RS=\"=====\\n\"} /Request: Revise o modelo desta tabela/" $log | grep -q "CREATE TABLE.*rpt_refunds"'
expect "Document this object asks for the comments as a new tab" 'grep -q "Request: Documente este objeto" $log'
expect "a table design with a name that is not a plain identifier is refused" 'block "$out" 7 | grep -q "ast=error amsg=\"The table design was refused: the table name is not a plain identifier"'
expect "the result summary goes only with its own consent, only for this tab's connection (computed locally)" 'awk "BEGIN{RS=\"=====\\n\"} /Request: RESUMO/" $log > build/ai_sql_resumo.txt; [ "$(grep -c "^Request: RESUMO" build/ai_sql_resumo.txt)" = 2 ] && [ "$(grep -c "<result_summary" build/ai_sql_resumo.txt)" = 1 ] && grep -q "region (.*): nulls 0, distinct" build/ai_sql_resumo.txt && ! grep -q "Juliette" build/ai_sql_resumo.txt'
expect "the workspace glossary goes with every request" 'grep -q "<glossary untrusted=\"true\">" $log && grep -q "receita líquida = rpt_sales.net_total" $log'
expect "an answer in prose (with a DROP in it) is refused; nothing changes" 'block "$out" 8 | grep -q "ast=error amsg=\"the answer is not a JSON object" && [ "$(sqlite3 $P/shop.db "SELECT count(*) FROM rpt_sales")" = "$before" ]'
expect "several SELECTs in one answer are not flagged as changing data (statements checked one by one)" 'line rptsql 5 | grep -q "writes=0 .*sql=.-- total"'
expect "a dashboard asked in a query tab becomes a new report built by the report flow (not SQL in a tab)" 'printf "%s\n" "$out" | grep "^rptstate" | tail -1 | grep -q "docs=1 .*ast=cannot-answer amsg=\"The assistant could not answer: no metric fits"'
expect "the execution plan open for the tab goes along (sanitized, with Lume findings): no need to run EXPLAIN by hand" 'awk "BEGIN{RS=\"=====\\n\"} /Request: PLANO/" $log | grep -q "<plan untrusted=\"true\">" && awk "BEGIN{RS=\"=====\\n\"} /Request: PLANO/" $log | grep -q "ESTIMATED (not run)"'
expect "a rewrite proposed in the chat can be compared with the open plan (its estimated plan, never run)" 'printf "%s\n" "$out" | grep -q "^rptcompare started" && printf "%s\n" "$out" | grep "^planstate" | tail -1 | grep -q "status=\"Candidate plan #[0-9]* vs #[0-9]* (estimated, nothing ran)"'
expect "validate: the free checks run by themselves, then running both queries waits for the user" 'printf "%s\n" "$out" | grep "^rptstate" | grep -q "ast=awaiting-approval amsg=\"The free checks are done"'
expect "approved: both run read-only; the chat shows the numbers (plans, same result, times)" 'printf "%s\n" "$out" | grep -q "^rptchat assistant: Checked: both plans captured (this database gives no costs); same result ([0-9]* rows); [0-9]* ms"'
expect "the model got <validation> with numbers only (no row) and concluded" 'awk "BEGIN{RS=\"=====\\n\"} /<validation untrusted/" $log | grep -q "same result: yes" && ! awk "/<validation untrusted/{on=1} /<\\/validation>/{on=0} on" $log | grep -q "Norte\|Sul\|Leste" && printf "%s\n" "$out" | grep -q "^rptchat assistant: validado"'
expect "one chat: the plan panel AI view asks Lume Intelligence (the plan goes along), no second chat" 'awk "BEGIN{RS=\"=====\\n\"} /Request: Por que esta query/" $log | grep -q "<plan untrusted=\"true\">"'
expect "audit without content" '! grep -q "net_total\|rpt_sales" $P/audit/*.log 2>/dev/null && grep -q "assistant_sql" $P/audit/*.log'
if [ $fails = 0 ]; then echo "AI SQL UI: all passed"; else echo "AI SQL UI: FAILED ($fails failures)"; fi
exit $fails
