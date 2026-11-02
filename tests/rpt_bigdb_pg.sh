#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Lume Intelligence on a large database (PostgreSQL: 3,000 generated tables on top of the test database's own thousands): the structure is read once
# in the background into a local index (the screen keeps drawing) and cached; each question sends only
# the tables it needs plus a summary; the model asks search_schema and gets the matches from the local
# index (nothing queried); its SQL then runs as usual. Loopback AI mock; no real catalog involved.
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
psql_ "DROP SCHEMA IF EXISTS lume_big CASCADE" >/dev/null
psql_ "CREATE SCHEMA lume_big;
DO \$\$ BEGIN FOR i IN 1..3000 LOOP EXECUTE format('CREATE TABLE lume_big.%I (id int PRIMARY KEY, cd_%s int, ds_%s varchar(40), dt_carga date)', 'tb_' || (ARRAY['estoque','lote','contabil','fiscal','param','config','integra','carga','remessa','titulo'])[1 + i % 10] || '_' || lpad(i::text, 5, '0'), i, i); END LOOP; END \$\$;
CREATE TABLE lume_big.zz_rp_med (id int PRIMARY KEY, nm_med varchar(80), vl_rep numeric(12,2), dt_pag date);
COMMENT ON TABLE lume_big.zz_rp_med IS 'Repasse aos médicos (honorários pagos)';
COMMENT ON COLUMN lume_big.zz_rp_med.vl_rep IS 'Valor do repasse';
INSERT INTO lume_big.zz_rp_med VALUES (1, 'Dra. Ana', 1500.50, '2026-10-01'), (2, 'Dr. Beto', 2499.50, '2026-10-02');" >/dev/null
Q=build/rpt-bigdb-pg; rm -rf $Q; mkdir -p $Q
printf '[connection]\nid=pgbig\nname=pg big\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n' > $Q/connections.txt
( umask 077; printf 'pgbig\tpassword\t%s\n' "$PW" > $Q/secrets.txt )
pf=build/rpt_bigdb_pg.port; log=build/rpt_bigdb_pg.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_bigdb_pg.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
printf '%s\n' "$out" > build/rpt_bigdb_pg.out
first=$(awk 'BEGIN{RS="=====\n"} NR==1' $log); second=$(awk 'BEGIN{RS="=====\n"} NR==2' $log)
expect "consent given: the structure is indexed in the background before anything is sent" 'block "$out" 1 | grep -q "ast=indexing-structure"'
expect "the index is built off the UI thread with a few bulk catalog reads, and cached" 'grep -Eq "structure index of pg big built: [0-9]{4,} tables" $Q/logs/lume.log && [ -s $Q/cache/schidx-pgbig.idx ]'
expect "the question words found the table by its description (honorários), among thousands" 'printf "%s" "$first" | grep -q "^table lume_big.zz_rp_med -- Repasse aos m"'
expect "the first request carries a summary and a bounded subset, not the 3,000 tables" 'printf "%s" "$first" | grep -Eq "tables_in_database=\"[0-9]{4,}\"" && [ $(printf "%s" "$first" | grep -c "^table ") -le 30 ] && [ $(printf "%s" "$first" | wc -c) -lt 60000 ]'
expect "the model searched the index (search_schema) and got the described table back, nothing queried" 'printf "%s" "$second" | grep -q "<schema_search terms=\"repasse medicos\"" && printf "%s" "$second" | grep -q "zz_rp_med -- Repasse aos m" && grep -q "search_schema answered from the local structure index: [1-9]" $Q/logs/lume.log'
expect "then it proposed, the SQL was checked and the card loads (4000.00)" 'block "$out" 2 | grep -q "ast=awaiting-approval" && block "$out" 3 | grep -q "rptvis v1 kind=kpi .*data=ok rows=1 first=\"4.000\""'
third=$(awk 'BEGIN{RS="=====\n"} NR==3' $log); fourth=$(awk 'BEGIN{RS="=====\n"} NR==4' $log)
expect "scope: limited to one schema, the structure is read again with only it (3001 tables)" 'printf "%s\n" "$out" | grep -q "^rptindex state=2 tables=3001 " && printf "%s" "$third" | grep -q "tables_in_database=\"3001\""'
expect "scope: a table on \"Never send\" is in no request, not even in search results" '[ -n "$fourth" ] && ! printf "%s%s" "$third" "$fourth" | grep -q "zz_rp_med --" && grep -q "^pgbig	block	lume_big.zz_rp_med" $Q/ai-scope.txt'
grep -q "^pgbig	consent	schema" $Q/ai-scope.txt; consent_saved=$?
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/ai.txt
printf 'prestadores = lume_big.zz_rp_med (repasse)\n' > $Q/glossary.txt
printf '1793575744\tpg big\tcompleted\t0.1\t1\tSELECT nf, dt FROM lume_big.tb_fiscal_00003 -- entradas de mercadoria recebidas\n' >> $Q/history.tsv
out2=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/rpt_bigdb_pg2.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
expect "a second session: the structure consent is remembered for this connection and the cached index is used (no new read)" '[ $consent_saved = 0 ] && block "$out2" 1 | grep -vq "ast=needs-schema" && block "$out2" 2 | grep -q "ast=awaiting-approval" && grep -q "structure index of pg big loaded from the cache" $Q/logs/lume.log'
gl=$(awk 'BEGIN{RS="=====\n"} /Request: BIGDB quanto gastamos com prestadores/' $log | head -c 200000)
expect "the glossary steers picking: a word only the glossary explains (prestadores) brings its table" 'printf "%s" "$gl" | grep -q "^table lume_big.zz_rp_med" && printf "%s" "$gl" | grep -q "<glossary untrusted"'
hs=$(awk 'BEGIN{RS="=====\n"} /Request: BIGDB entradas de mercadoria recebidas/' $log | head -c 200000)
expect "the user own queries (history) teach which table goes with which words; their text is never sent" 'printf "%s" "$hs" | grep -q "^table lume_big.tb_fiscal_00003" && ! printf "%s" "$hs" | grep -q "SELECT nf, dt FROM" && grep -q "learned from [1-9][0-9]* use(s) of tables in your query history" $Q/logs/lume.log'
expect "nothing of the catalog is logged beyond counts" '! grep -q "zz_rp_med" $Q/logs/lume.log'
psql_ "DROP SCHEMA IF EXISTS lume_big CASCADE" >/dev/null
if [ $fails = 0 ]; then echo "RPT BIGDB PG: all passed"; else echo "RPT BIGDB PG: FAILED ($fails failures)"; fi
exit $fails
