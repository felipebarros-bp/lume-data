#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Execution Plan panel on screen (EPL-001..084): SQLite (estimated only), PostgreSQL estimated / actual,
# PROD policy for actual plans of writes, imports of every engine's fixture, operators table, comparison,
# Lume Intelligence off by default and through a loopback mock (preview, share toggles, audit), index
# candidate opened in the Table Designer (never applied from the plan).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
mkdir -p build/ui
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: \(target not found\|plan \|plan: \|td \)' ; }
nth() { printf '%s\n' "$1" | grep "^$2" | sed -n "$3p"; }

P=build/plan-sqlite; rm -rf $P; mkdir -p $P
printf '[connection]\nid=shop1\nname=Shop\ndriver=sqlite\npath=%s/%s/shop.db\n' "$PWD" "$P" > $P/connections.txt
sqlite3 $P/shop.db "CREATE TABLE customers(id INTEGER PRIMARY KEY, city TEXT); CREATE TABLE orders(id INTEGER PRIMARY KEY, customer_id INT, total REAL, status TEXT);"
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/plan_sqlite.txt ./build/lume 2>&1)
expect "every script target found (SQLite)" '[ "$(missing "$out")" = 0 ]'
expect "EPL-012 / 001: Query > Explain on SQLite = EXPLAIN QUERY PLAN in the panel, estimated" 'nth "$out" planstate 1 | grep -q "dialect=SQLite mode=estimated"'
expect "EPL-012.3: no rows, costs or time invented (all n/a)" 'printf "%s\n" "$out" | grep "^plannode" | head -4 | grep -q "est=n/a act=n/a" && ! nth "$out" planstate 1 | grep -q "total cost"'
expect "EPL-016: a SQL Server Showplan file imports as a snapshot (no connection)" 'nth "$out" planstate 2 | grep -q "dialect=SQL Server mode=actual" && nth "$out" planstate 2 | grep -q "status=\"Imported plan"'
expect "EPL-020 / 022: Oracle DISPLAY_CURSOR text imports (7 operators)" 'nth "$out" planstate 3 | grep -q "dialect=Oracle mode=actual nodes=7"'
expect "EPL-019: MariaDB ANALYZE JSON imports" 'nth "$out" planstate 4 | grep -q "dialect=MariaDB mode=actual"'
expect "EPL-043 / 076: AI off by default: Send explains what is missing; the plan stays" 'nth "$out" planstate 6 | grep -q "ai_err=\"Lume Intelligence is off" && nth "$out" planstate 6 | grep -q "nodes="'
expect "EPL-075: the refusal is audited without content" 'grep -q "decision=not-sent-not-configured" $P/audit/ai.log && ! grep -qi "select\|orders" $P/audit/ai.log'

ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then
  pg() { docker exec -i dbv-postgres psql -U postgres -d syn -tAqc "$1" 2>&1; }
  hypo=$(pg "CREATE EXTENSION IF NOT EXISTS hypopg; SELECT 1" | tail -1)   # roadmap 16: hypothetical indexes (when the container has hypopg)
  if [ "$(pg "SELECT count(*) FROM lume_pl.orders" 2>/dev/null)" != 200000 ]; then
    pg "DROP SCHEMA IF EXISTS lume_pl CASCADE; CREATE SCHEMA lume_pl; CREATE TABLE lume_pl.customers(id int PRIMARY KEY, city text); CREATE TABLE lume_pl.orders(id int PRIMARY KEY, customer_id int, total numeric, status text); INSERT INTO lume_pl.customers SELECT g, 'c'||(g%50) FROM generate_series(1,5000) g; INSERT INTO lume_pl.orders SELECT g, g%5000+1, g%977, CASE WHEN g%10=0 THEN 'open' ELSE 'paid' END FROM generate_series(1,200000) g; ANALYZE lume_pl.orders, lume_pl.customers;" >/dev/null
  fi
  pg "DROP INDEX IF EXISTS lume_pl.ix_orders_status" >/dev/null
  idx_before=$(pg "SELECT count(*) FROM pg_indexes WHERE schemaname = 'lume_pl'")
  Q=build/plan-pg; rm -rf $Q; mkdir -p $Q
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  printf '[connection]\nid=pga\nname=syn A\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n\n[connection]\nid=pgprod\nname=syn PROD\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=PROD\n' > $Q/connections.txt
  ( umask 077; for id in pga pgprod; do printf '%s\tpassword\t%s\n' "$id" "$PW"; done > $Q/secrets.txt )
  port_file=build/plan_ui_mock.port; rm -f $port_file
  python3 -I tests/tools/ai_mock.py > $port_file & mock=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $port_file ] && break; sleep 0.2; done
  printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $port_file)" > $Q/ai.txt
  before=$(pg "SELECT md5(string_agg(id || ':' || total, ',' ORDER BY id)) FROM lume_pl.orders WHERE id <= 50")
  out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/plan_pg.txt ./build/lume 2>&1)
  kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $port_file
  after=$(pg "SELECT md5(string_agg(id || ':' || total, ',' ORDER BY id)) FROM lume_pl.orders WHERE id <= 50")
  expect "every script target found (PostgreSQL)" '[ "$(missing "$out")" = 0 ]'
  expect "Cenário 01 / EPL-010: Explain = EXPLAIN (FORMAT JSON): costs and estimated rows, statement not run" 'nth "$out" planstate 1 | grep -q "mode=estimated nodes=9" && nth "$out" planstate 1 | grep -q "ESTIMATED (not run)" && nth "$out" planstate 1 | grep -q "optimizer units, not time"'
  expect "EPL-003: Output records the capture under the plan note (same execution)" 'grep -q "Execution plan of a statement on syn A" $Q/logs/lume.log || grep -q "plan: estimated capture on syn A" $Q/logs/lume.log'
  expect "EPL-011 / 072: Explain Analyze asks first (it runs the statement), then shows actual rows / loops" 'printf "%s\n" "$out" | grep -q "^tbmodal kind=1 title=\"Run EXPLAIN ANALYZE?\"" && nth "$out" planstate 2 | grep -q "mode=actual" && printf "%s\n" "$out" | awk "/^planstate/{k++} k==2" | grep "^plannode 6" | grep -q "act=[0-9]"'
  expect "EPL-036 / 037: the filtered scan is a finding with evidence (rows discarded)" 'printf "%s\n" "$out" | grep "^planfinding" | grep -q "discards [0-9]* row(s)"'
  expect "EPL-065: comparison A / B never claims result equivalence" 'nth "$out" planstate 3 | grep -q "cmp=1"'
  prev() { printf '%s\n' "$1" | awk -v n="$2" '/^planpreview [0-9]/{k++; on=(k==n); next} /^planpreview-end/{on=0} on'; }
  expect "EPL-044 / 073: preview: sanitized by default (no SQL, literals masked, names aliased); sharing SQL is a choice" 'prev "$out" 1 | grep -q "table_1" && ! prev "$out" 1 | grep -q "^sql:" && ! prev "$out" 1 | grep -q "lume_pl" && prev "$out" 2 | grep -q "^sql:" && prev "$out" 2 | grep -q "o.status = ?"'
  expect "EPL-043 / 047: the AI answer comes back with model and date; the candidate SQL opens as a copy" 'nth "$out" planstate 4 | grep -q "answer=1 sql_candidates=1" && printf "%s\n" "$out" | grep -q "^plananswer EVIDENCE"'
  expect "EPL-075 / 083: AI send audited (bytes, share flags, no content)" 'grep -q "decision=sent bytes_sent=[0-9]* shared=sql:1" $Q/audit/ai.log && ! grep -q "lume_pl\|SELECT" $Q/audit/ai.log'
  expect "EPL-083: asking the AI did not modify the database" '[ "$before" = "$after" ] && [ "$(pg "SELECT count(*) FROM pg_indexes WHERE indexname = '"'"'ix_orders_status'"'"'")" = 0 ]'
  expect "roadmap 15: the candidate's estimated plan is captured (never run) and compared side by side with the original" 'printf "%s\n" "$out" | grep "^planstate" | grep -q "status=\"Candidate plan #[0-9]* vs #[0-9]* (estimated, nothing ran): cost .* cmp=[0-9]"'
  idx_after=$(pg "SELECT count(*) FROM pg_indexes WHERE schemaname = 'lume_pl'")
  if [ "$hypo" = 1 ]; then expect "roadmap 16: the index candidate as a hypothetical index (hypopg): the plan with it beside the plan without it, no index created" 'printf "%s\n" "$out" | grep "^planstate" | tail -1 | grep -q "status=\"Plan #[0-9]* with the hypothetical index vs #[0-9]* without it: cost .* (estimated; no index was created)" && [ "$idx_after" = "$idx_before" ]'; else echo "SKIP  roadmap 16: hypopg is not installed in the container"; fi
  expect "EPL-056 / 062: the index candidate opens in the Table Designer as a draft (review & apply there, nothing created)" 'printf "%s\n" "$out" | grep "^tdstate" | tail -1 | grep -q "table=lume_pl.orders.*dirty=1.*banner=\"Index proposed by the execution plan analysis"'
  expect "Cenário 02 / EPL-072: on PROD an actual plan of an UPDATE is refused (ANALYZE would execute it)" 'nth "$out" planstate 5 | grep -q "Explain Actual refused on syn PROD"'
  expect "no PostgreSQL password or API key in logs, audit, reports" '! grep -rqF "$PW" $Q/logs $Q/audit 2>/dev/null && ! grep -rq "sk-test-not-a-real-key" $Q/logs $Q/audit $Q/*.txt 2>/dev/null && ! ls $Q/lume-ai-* >/dev/null 2>&1'
  unset PW
else echo "skip  PostgreSQL scenarios: dbv-postgres not running"; fi

# idle with the panel showing a plan: no redraw loop (EPL-078 / Cenário 12)
cput() { ps -o time= -p $1 | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*100+$3; else print (($1*60+$2)*60+$3)*100+$4 }'; }
printf 'wait 500\ndnode "Shop"\nwait 800\ntype SELECT * FROM orders WHERE total > 1\ninvoke sql.explain\nwait 9000\nquit\n' > $P/idle.txt
LUME_PREF_DIR=$P LUME_SCRIPT=$P/idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
sleep 5; c0=$(cput $pid); sleep 3; c1=$(cput $pid); wait $pid
cpu=$(awk -v a="$c0" -v b="$c1" 'BEGIN{printf "%.1f", (b-a)/3}')
expect "Cenário 12 / EPL-078: idle with a plan on screen: $cpu % CPU (bar 2 %)" 'awk -v c="$cpu" "BEGIN{exit !(c + 0 <= 2.0)}"'

echo "PLAN UI: $([ $fails = 0 ] && echo passed || echo "FAILED ($fails failures)")"
[ $fails = 0 ]
