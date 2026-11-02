#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# BL-051 on PostgreSQL: the assistant's SQL runs through a SELECT-only login. The login's grants limit
# the structure the model sees (a table without grants is not even listed), the write-grant check tells
# the superuser from the read-only login, the database itself refuses writes from that login, and
# require_readonly_ai_sql=1 refuses to share anything without one. Loopback AI mock.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if ! docker exec dbv-postgres true 2>/dev/null || [ ! -f "$ENV_FILE" ]; then echo "SKIP  PostgreSQL container dbv-postgres is not running"; exit 0; fi
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
psql_() { docker exec -i dbv-postgres psql -U postgres -d syn -tAqc "$1" 2>&1; }

PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
RO=$(openssl rand -hex 16)   # the test login's password: never on a command line, only in the test prefs secrets.txt
psql_ "DROP SCHEMA IF EXISTS lume_aiq CASCADE" >/dev/null
psql_ "DROP OWNED BY lume_ai_ro" >/dev/null 2>&1; psql_ "DROP ROLE IF EXISTS lume_ai_ro" >/dev/null
psql_ "CREATE SCHEMA lume_aiq; CREATE TABLE lume_aiq.sales (id int PRIMARY KEY, region text, status text, amount numeric(10,2)); CREATE TABLE lume_aiq.hr_salaries (id int PRIMARY KEY, employee text, salary numeric);
       INSERT INTO lume_aiq.sales SELECT g, (ARRAY['Norte','Sul','Leste','Oeste'])[1 + g % 4], CASE WHEN g % 5 = 0 THEN 'cancelled' ELSE 'paid' END, 10 + g FROM generate_series(1, 200) g;
       INSERT INTO lume_aiq.hr_salaries VALUES (1, 'Fulano', 9999);" >/dev/null
printf "CREATE ROLE lume_ai_ro LOGIN PASSWORD '%s';\n" "$RO" | docker exec -i dbv-postgres psql -U postgres -d syn -q >/dev/null
psql_ "GRANT CONNECT ON DATABASE syn TO lume_ai_ro; GRANT USAGE ON SCHEMA lume_aiq TO lume_ai_ro; GRANT SELECT ON lume_aiq.sales TO lume_ai_ro;" >/dev/null
paid=$(psql_ "SELECT to_char(sum(amount), 'FM999999990.00') FROM lume_aiq.sales WHERE status = 'paid'")

Q=build/rpt-aisql-pg; rm -rf $Q; mkdir -p $Q
printf '[connection]\nid=pgmain\nname=pg main\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n\n[connection]\nid=pgairo\nname=pg ai-ro\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=lume_ai_ro\nenv=DEV\n' > $Q/connections.txt
( umask 077; printf 'pgmain\tpassword\t%s\npgairo\tpassword\t%s\n' "$PW" "$RO" > $Q/secrets.txt )
pf=build/rpt_aisql_pg.port; log=build/rpt_aisql_pg.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_aisql_pg.txt ./build/lume 2>&1)
sch() { printf '%s\n' "$out" | grep '^rptschema' | sed -n "$1p"; }

expect "the superuser login is recognised as able to write" 'sch 1 | grep -q "exec=pg main grants=2 note=\"superuser"'
expect "mapped: the assistant's SQL runs as the SELECT-only login, verified (no write grants), marked read-only in Lume" 'sch 2 | grep -q "main=pg main exec=pg ai-ro grants=1 .*ro=1" && grep -q "^ai_conn=pgairo" $Q/connections.txt'
expect "the structure the model gets is what that login may see: sales listed, hr_salaries (no grant) absent" 'grep -q "lume_aiq.sales" $log && ! grep -q "hr_salaries" $log'
expect "the plan names the read-only login" 'printf "%s\n" "$out" | grep -q "^rptstep ai_connection \[ok\] runs as pg ai-ro: no write grants"'
b=$(block "$out" 2)
expect "approved: the cards run on the read-only connection and load ($paid)" 'printf "%s\n" "$b" | grep -q "^rptaisql v1 conn=pgairo" && printf "%s\n" "$b" | grep -q "^rptvis v1 kind=kpi.*data=ok" && printf "%s\n" "$b" | grep -q "^rptvis v2 kind=bar.*data=ok"'
expect "the database itself refuses a write from that login (permission denied), whatever the SQL" 'psql_ "SET ROLE lume_ai_ro; INSERT INTO lume_aiq.sales VALUES (999, '"'"'x'"'"', '"'"'paid'"'"', 1)" | grep -q "permission denied" && [ "$(psql_ "SELECT count(*) FROM lume_aiq.sales")" = 200 ]'

# ---- the policy: a read-only login is required ----
printf 'require_readonly_ai_sql=1\n' >> $Q/ai.txt
sed -i '' '/^ai_conn=/d' $Q/connections.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_aisql_pg_policy.txt ./build/lume 2>&1)
expect "require_readonly_ai_sql=1 without a read-only login: refused, nothing shared or sent" 'block "$out" 1 | grep -q "ast=error amsg=\"This setup requires a read-only login" && grep -q "decision=refused-not-readonly-login" $Q/audit/report.log && [ "$(grep -c "=====" $log)" = 1 ]'
expect "no password in logs, audit or settings" '! grep -rq "$RO" $Q/logs $Q/audit $Q/connections.txt $Q/ai.txt 2>/dev/null && ! grep -rq "$PW" $Q/logs $Q/audit $Q/connections.txt 2>/dev/null'
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf

psql_ "DROP SCHEMA IF EXISTS lume_aiq CASCADE" >/dev/null; psql_ "DROP OWNED BY lume_ai_ro" >/dev/null 2>&1; psql_ "DROP ROLE IF EXISTS lume_ai_ro" >/dev/null
if [ $fails = 0 ]; then echo "RPT AI SQL PG: all passed"; else echo "RPT AI SQL PG: FAILED ($fails failures)"; fi
exit $fails
