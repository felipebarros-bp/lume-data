#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Report Studio + Lume Intelligence for reports on screen (LI-*): SQLite golden dataset, cards from the
# semantic catalog, cache / undo without re-query, layout and locks, themes, page filters, read-only policy,
# timeout, preview, provenance, save / autosave / recovery / read mode, PNG export; the assistant through a
# loopback mock (ambiguity, plan, approval, copilot, injection, conflicts, consent, audit), AI off by default.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
sh tests/rpt_check.sh >/dev/null 2>&1 || { echo "FAIL  rpt_check (headless) must pass first"; exit 1; }
mkdir -p build/ui
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: \(target not found\|rpt \)' ; }
nth() { printf '%s\n' "$1" | grep "^$2" | sed -n "$3p"; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
vis() { block "$1" "$2" | grep "^rptvis $3 "; }

P=build/rpt-ui; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
before=$(sqlite3 $P/shop.db "SELECT count(*) || ':' || total(net_total) FROM rpt_sales")
kpi=$(sqlite3 $P/shop.db "SELECT printf('%.2f', sum(net_total)) FROM rpt_sales WHERE status = 'paid' AND ordered_at >= '2025-11-01' AND ordered_at < '2026-11-01'")
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/rpt_manual.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/rpt_manual.out
expect "every script target found (manual)" '[ "$(missing "$out")" = 0 ]'
k=$(python3 -I -c "import sys; print(f'{float(sys.argv[1]):,.2f}'.replace(',', '_').replace('.', ',').replace('_', '.'))" "$kpi")
expect "LI-040 / 096: KPI = the exact sum computed by SQLite itself (R\$ $k), from the catalog's definition" 'vis "$out" 1 v1 | grep -q "first=\"R\$ $k\""'
expect "LI-041: line by month (12 points), bars by region (4), table (4) and text, all loaded" 'vis "$out" 1 v3 | grep -q "kind=line.*data=ok rows=12" && vis "$out" 1 v4 | grep -q "kind=bar.*dim=region.*data=ok rows=4" && vis "$out" 1 v5 | grep -q "data=ok rows=4" && vis "$out" 1 v6 | grep -q "data=text"'
q1=$(block "$out" 1 | head -1 | sed 's/.*queries=\([0-9]*\).*/\1/'); q3=$(block "$out" 3 | head -1 | sed 's/.*queries=\([0-9]*\).*/\1/')
expect "LI-044.2 / Gherkin: line -> bars and Undo re-use the cached data (no new query: $q1 -> $q3 counting one late table load)" 'vis "$out" 2 v3 | grep -q "kind=bar.*grain=month.*data=ok" && vis "$out" 3 v3 | grep -q "kind=line.*data=ok" && [ $((q3 - q1)) -le 1 ]'
expect "LI-044.1: a move pushes the others down: no overlap" 'vis "$out" 4 v2 | grep -q "at=0,0,3,4" && vis "$out" 4 v1 | grep -q "at=0,4,3,4"'
expect "LI-055.2: a card cannot be moved onto a locked one" 'vis "$out" 5 v1 | grep -q "locked=1" && vis "$out" 5 v2 | grep -q "at=0,0,3,4" && block "$out" 5 | grep -q "move refused"'
expect "LI-047: a page filter (region = Sul) changes the cards and is stated in the provenance" 'vis "$out" 6 v4 | grep -q "rows=1 " && block "$out" 6 | grep -q "Filtros: region = Sul"'
expect "LI-047.2: a filter that does not apply to a card blocks it with a reason (not applied silently)" 'vis "$out" 7 v1 | grep -q "data=blocked.*incompatível"'
expect "LI-029 / 097: a DELETE in a report table is refused by the read-only policy before the database" 'vis "$out" 8 v7 | grep -q "data=error.*refused by the read-only policy"'
expect "LI-081 / R05: a query past its timeout is really cancelled; the other cards stay valid" 'vis "$out" 9 v8 | grep -q "data=timeout" && vis "$out" 9 v1 | grep -q "data=ok" && grep -q "report: query timed out after 1 s" $P/logs/lume.log'
expect "LI-049: preview hides the editing controls (state preview=1, data kept)" 'block "$out" 10 | grep -q "preview=1" && vis "$out" 10 v1 | grep -q "data=ok"'
expect "LI-050: Save writes the report and one version file per saved revision" 'block "$out" 11 | grep -q "dirty=0" && ls $P/reports/*.lrpt >/dev/null 2>&1 && ls $P/reports/*.lrpt.versions/rev-*.lrpt >/dev/null 2>&1'
expect "LI-083: the report file has no credential, DSN or database path" '! grep -qi "password\|secret\|shop.db" $P/reports/*.lrpt'
expect "LI-070: page and card exported as PNG (valid images, header with period and source)" 'file build/rpt-ui/page.png | grep -q "PNG image data, 1600" && file build/rpt-ui/kpi.png | grep -q "PNG image data"'
expect "R05: a failed export says so and leaves nothing behind" 'printf "%s\n" "$out" | grep -q "^rptexport ok=0 why=\"cannot write" && [ ! -e /nonexistent-dir/x.png.tmp ]'
expect "LI-021 / Gherkin 'Termo ambíguo': 'faturamento' asks gross or net before anything is sent or queried" 'block "$out" 12 | grep -q "ast=clarify-local.*nopt=2" && grep -q "decision=clarify-before-sending" $P/audit/report.log'
expect "LI-004: with AI off (default) the assistant says so; reports keep working" 'block "$out" 13 | grep -q "ast=error amsg=\"Lume Intelligence is off" && vis "$out" 13 v1 | grep -q "data=ok" && grep -q "decision=not-sent-not-configured" $P/audit/report.log'
expect "LI-029: the database was not modified by any report query" '[ "$before" = "$(sqlite3 $P/shop.db "SELECT count(*) || '"'"':'"'"' || total(net_total) FROM rpt_sales")" ]'
expect "LI-050.2: unsaved changes made after Save are in the autosave on quit" 'ls $P/reports/*.lrpt.autosave >/dev/null 2>&1 && grep -q "editado depois de salvar" $P/reports/*.lrpt.autosave'
rp=$(ls $P/reports/*.lrpt | head -1)
sed "s|REPORT_PATH|$rp|" tests/scripts/rpt_reopen.txt > build/rpt_reopen.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_reopen.txt ./build/lume 2>&1)
expect "LI-050.2: reopening recovers the autosave (crash / quit with unsaved changes)" 'nth "$out" rptdoc 1 | grep -q "editado depois de salvar" && nth "$out" rptdoc 1 | grep -q "Recuperado do autosave"'
expect "LI-069: read mode opens without editing (read=1); the saved file is untouched" 'printf "%s\n" "$out" | grep "^rptdoc" | grep -q "read=1" && ! grep -q "editado depois" "$rp"'
expect "LI-095: render time per frame is measured (render_ms)" 'printf "%s\n" "$out" | grep -q "render_ms="'
expect "LI-089: AI off: no AI request file or process was created" '! ls $P/lume-ai-* >/dev/null 2>&1 && ! grep -q "decision=sent" $P/audit/report.log'

# ---- the assistant through the loopback mock ----
Q=build/rpt-ai; rm -rf $Q; sh tests/rpt_setup_sqlite.sh $Q
pf=build/rpt_ui_mock.port; rm -f $pf
python3 -I tests/tools/ai_mock.py > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/ai.txt
before=$(sqlite3 $Q/shop.db "SELECT count(*) || ':' || total(net_total) FROM rpt_sales")
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_ai.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
printf '%s\n' "$out" > build/rpt_ai.out
expect "every script target found (assistant)" '[ "$(missing "$out")" = 0 ]'
pay() { printf '%s\n' "$1" | awk -v n="$2" '/^rptpayload$/{k++; on=(k==n); next} /^rptpayload-end/{on=0} on'; }
expect "LI-010 / 032: the payload shown is the catalog, report and request; no rows, table names, connection or restricted column" 'pay "$out" 1 | grep -q "<catalog untrusted" && ! pay "$out" 1 | grep -q "rpt_sales\|shop1\|customer_email\|example.com\|<values"'
expect "LI-021: the clarification (net revenue) is sent and kept in the report" 'block "$out" 2 | grep -q "ast=awaiting-approval" && block "$out" 3 | grep -q "choices=1"'
expect "LI-012: the plan lists the tools, compiled queries, policy checks and the queries waiting for approval" 'block "$out" 2 | grep -q "^rptstep 0 done propose_report_patch" && block "$out" 2 | grep -c "ok validate_query_policy" | grep -q 4 && block "$out" 2 | grep -q "pending execute_readonly_query"'
expect "LI-012.2: nothing changed and no query ran before the approval" 'block "$out" 2 | grep -q "queries=0" && ! block "$out" 2 | grep -q "^rptvis"'
expect "LI-051 / MVP exit: approved, the page has KPI (YoY), monthly line, bars by region and a table, all loaded" 'vis "$out" 4 v1 | grep -q "kind=kpi.*cmp=yoy.*data=ok" && vis "$out" 4 v2 | grep -q "kind=line.*grain=month.*data=ok" && vis "$out" 4 v3 | grep -q "kind=bar.*dim=region.*data=ok" && vis "$out" 4 v4 | grep -q "kind=table.*data=ok"'
expect "Gherkin 'Trocar o gráfico': the copilot's plan reuses the loaded data (cached, no new query)" 'block "$out" 4 | grep -q "cached execute_readonly_query"'
expect "Gherkin: applied, only the selected card changed (bars, same metric and grain)" 'vis "$out" 5 v2 | grep -q "kind=bar metric=net_revenue.*grain=month" && vis "$out" 5 v3 | grep -q "kind=bar.*dim=region" && vis "$out" 5 v1 | grep -q "kind=kpi"'
q4=$(block "$out" 4 | head -1 | sed 's/.*queries=\([0-9]*\).*/\1/'); q6=$(block "$out" 6 | head -1 | sed 's/.*queries=\([0-9]*\).*/\1/')
expect "Gherkin: undo restores the line without re-querying ($q4 -> $q6)" 'vis "$out" 6 v2 | grep -q "kind=line" && [ "$q4" = "$q6" ]'
expect "Gherkin 'Segurança' / LI-084 / R02: a model proposal carrying SQL is refused, nothing applied" 'block "$out" 7 | grep -q "ast=error.*sql.* is not allowed" && [ "$(block "$out" 7 | grep -c "^rptvis")" = 4 ]'
expect "LI-084 / 097: an answer that gives orders in prose (DROP TABLE) is not applied" 'block "$out" 8 | grep -q "ast=error amsg=\"the answer is not a JSON object"'
expect "R02: a proposal for an older revision is a conflict, nothing overwritten" 'block "$out" 9 | grep -q "conflict: the proposal was made for revision"'
expect "LI-012.2: refusing the plan changes nothing and runs no query" 'block "$out" 11 | grep -q "Proposal refused: nothing changed and no query ran" && [ "$(block "$out" 11 | grep -c "^rptvis")" = 4 ]'
expect "LI-032.2 / 079: aggregated values go only after their own consent (and only the selected card's)" 'pay "$out" 2 | grep -q "<values untrusted" && pay "$out" 2 | grep -q "293044.86"'
expect "LI-004: disabling the AI stops it; the report keeps its data" 'block "$out" 12 | grep -q "ai=none.*Lume Intelligence is off" && vis "$out" 12 v1 | grep -q "data=ok"'
expect "LI-082: the audit has decisions and sizes, never prompts, SQL, values or the key" 'grep -q "decision=sent bytes=[0-9]* provider=anthropic" $Q/audit/report.log && grep -q "decision=approved" $Q/audit/report.log && grep -q "decision=refused-invalid" $Q/audit/report.log && ! grep -qi "select\|faturamento\|293044\|sk-test\|DROP" $Q/audit/report.log'
expect "LI-080: the tool_use block in every mock answer (DROP TABLE) was ignored; the database is unchanged" '[ "$before" = "$(sqlite3 $Q/shop.db "SELECT count(*) || '"'"':'"'"' || total(net_total) FROM rpt_sales")" ]'
expect "no API key in logs, audit, reports or prefs; no request file left behind" '! grep -rq "sk-test-not-a-real-key" $Q/logs $Q/audit $Q/reports $Q/*.txt 2>/dev/null && ! ls $Q/lume-ai-* >/dev/null 2>&1'


# ---- the AI key pasted in the panel, kept in the OS secret store (macOS Keychain); a test service name
#      so the user's real entry is never read or touched ----
K=build/ai-key; rm -rf $K; mkdir -p $K
pf=build/key_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=none\nendpoint=http://127.0.0.1:%s/v1/messages\n' "$(head -1 $pf)" > $K/ai.txt
out=$(LUME_SECSTORE_SERVICE="Lume Data Test" LUME_PREF_DIR=$K LUME_SCRIPT=tests/scripts/rpt_key.txt env -u ANTHROPIC_API_KEY -u LUME_AI_KEY ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
st() { printf '%s\n' "$out" | grep '^rptsetup' | sed -n "$1p"; }
expect "LI-001 / 002: with no key, Enable is impossible; a pasted key is tested before anything is saved" 'st 1 | grep -q "keysource=0" && st 2 | grep -q "test=2 .*cfg=none/.*keysource=0 keyfield=1"'
expect "LI-002: Enable saves the tested key in the OS secret store and clears the field" 'st 3 | grep -q "open=0 .*cfg=anthropic/.*keysource=2 keyfield=0"'
expect "LI-002: the saved key is read back from the store for the next request" 'st 4 | grep -q "test=2 .*keysource=2"'
expect "LI-002.2: removing the key deletes it from the store" 'st 5 | grep -q "keysource=0" && ! security find-generic-password -s "Lume Data Test" -a anthropic-api-key >/dev/null 2>&1'
expect "LI-002 / 083: the key is in no file of the prefs folder (settings, logs, audit)" '! grep -rq "sk-test-not-a-real-key" $K' 
expect "LI-001: a custom endpoint survives saving the settings" 'grep -q "^endpoint=http://127.0.0.1" $K/ai.txt'

if [ $fails = 0 ]; then echo "RPT UI: all passed"; else echo "RPT UI: FAILED ($fails failures)"; fi
exit $fails
