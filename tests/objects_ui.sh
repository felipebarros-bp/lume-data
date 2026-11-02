#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Object source editor on screen (spec OBJ-001..080, AT-01..10): Explorer objects and menus, the
# object tab (header, toolbar, Problems), drafts across restarts, review, apply, conflicts,
# read-only, diagnostics and quick fixes, large definitions; PostgreSQL view contract / overloads /
# PROD confirmation when the dbv-postgres container is up.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/objects-pref; rm -rf "$P"; mkdir -p "$P" build/ui
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: \(target not found\|objopen failed\|rawsql failed\)' ; }
nth() { printf '%s\n' "$1" | grep '^objstate' | sed -n "$2p"; }
run() { LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1; }
db() { sqlite3 "$P/shop.db" "$1"; }

sqlite3 "$P/shop.db" "CREATE TABLE sales(id INTEGER PRIMARY KEY, amount REAL, note TEXT); INSERT INTO sales VALUES (1,10,'a'),(2,20,'b');
CREATE VIEW v_revenue AS SELECT id, amount FROM sales; CREATE TABLE log(msg TEXT);
CREATE TRIGGER v_rev_ins INSTEAD OF INSERT ON v_revenue BEGIN INSERT INTO log VALUES ('ins'); END;
CREATE TRIGGER trg_sales AFTER INSERT ON sales BEGIN INSERT INTO log VALUES ('sale'); END;
CREATE VIEW v_top AS SELECT * FROM v_revenue WHERE amount > 15;"
python3 -I -c "
import sys
w = '\n'.join('    WHEN %d THEN %s' % (i, repr('v%d' % i)) for i in range(6000))
print('CREATE VIEW v_big AS SELECT id, CASE id\n' + w + '\n    ELSE NULL END AS label FROM sales;')" | sqlite3 "$P/shop.db"
cp "$P/shop.db" "$P/ro.db"
printf '[connection]\nid=shop1\nname=Shop\ndriver=sqlite\npath=%s/%s/shop.db\n\n[connection]\nid=ro1\nname=RO copy\ndriver=sqlite\nread_only=1\npath=%s/%s/ro.db\n' "$PWD" "$P" "$PWD" "$P" > "$P/connections.txt"

# ---- AT-01: SQLite view from the Explorer to the server
out=$(run obj_a); L=$P/logs/lume.log
expect "every script target found (SQLite, AT-01)" '[ "$(missing "$out")" = 0 ]'
expect "OBJ-001 / 004: the Explorer lists triggers under Objects (on demand) and the view menu opens the definition" 'nth "$out" 1 | grep -q "key=\"VIEW main.v_revenue\" state=Synced dirty=0 draft=0 loaded=1"'
expect "OBJ-003: typing marks the tab modified, the database is not touched" 'nth "$out" 2 | grep -q "dirty=1 draft=0"'
expect "OBJ-033: Save Draft keeps the text locally (drafts/), the database is unchanged" 'nth "$out" 3 | grep -q "draft=1" && nth "$out" 3 | grep -q "The database is unchanged" && grep -q "Draft saved locally" $L'
expect "OBJ-062: Run is refused in an object tab (no console execution of a definition)" 'grep -q "Not run: This tab edits a database object" $L'
expect "OBJ-012: Format Document rewrote the buffer as one undo step" 'nth "$out" 4 | grep -q "msg=\"Formatted.\""'
expect "OBJ-035 / AT-01: review plan = BEGIN, DROP VIEW, CREATE, INSTEAD OF trigger re-created, compile check, COMMIT" 'printf "%s\n" "$out" | grep -q "^objsql 2: DROP VIEW \"main\".\"v_revenue\"" && printf "%s\n" "$out" | grep -q "^objsql 4: CREATE TRIGGER v_rev_ins" && printf "%s\n" "$out" | grep -q "^objsql 6: COMMIT"'
expect "OBJ-036: nothing ran before the review was confirmed" 'nth "$out" 5 | grep -q "modal=1"'
expect "AT-01: applied, definition read back, trigger still there" 'nth "$out" 6 | grep -q "state=Applied dirty=0 draft=0" && db "SELECT sql FROM sqlite_master WHERE name=\"v_revenue\"" | grep -q "amount \* 2" && [ "$(db "SELECT count(*) FROM sqlite_master WHERE name=\"v_rev_ins\"")" = 1 ]'
expect "OBJ-061: the deployment is in the Output session log and the status bar" 'grep -q "status: Applied to Shop" $L'
expect "OBJ-071: one audit line per publication, without SQL text" 'grep -q "object=\"VIEW main.v_revenue\" strategy=\"DROP + CREATE in one transaction\".*result=applied" $P/logs/object-audit.log && ! grep -qi "select" $P/logs/object-audit.log'
expect "OBJ-045: Submit for Review wrote a diff + deployment SQL artifact whose SHA-256 matches its body (no approval engine pretended)" 'f=$(ls $P/reviews/*.review.sql 2>/dev/null | head -1); [ -n "$f" ] && want=$(sed -n "s/^-- SHA-256 of everything after the next line: //p" "$f") && got=$(sed "1,/^-- ----\$/d" "$f" | shasum -a 256 | cut -c1-64) && [ "$want" = "$got" ] && grep -q "^-- Approval: outside Lume" "$f" && grep -q "DROP VIEW" "$f"'
expect "OBJ-045: the audit tells the author of the edit, the account that applied and the database account apart" 'grep "object=\"VIEW main.v_revenue\"" $P/logs/object-audit.log | head -1 | grep -qE "author=[^ ]+ applied_by=[^ ]+ db_account=-"'
expect "OBJ-041: the previous definition saved as a recovery script" 'cat $P/backups/*v_revenue*.sql 2>/dev/null | grep -q "SELECT id, amount FROM sales"'
expect "OBJ-009: the draft is removed once the change is published" '! ls $P/drafts/*.sql 2>/dev/null | grep -v base | grep -q .'

# ---- OBJ-009 + AT-04: draft across a restart, another session changes the view, three-way
out=$(run obj_b)
expect "an unsaved edit is kept as a draft when Lume quits" 'nth "$out" 1 | grep -q "dirty=1" && ls $P/drafts/*.sql >/dev/null 2>&1'
out=$(run obj_b2)
expect "every script target found (draft / conflict)" '[ "$(missing "$out")" = 0 ]'
expect "OBJ-009: reopening the object restores the draft" 'nth "$out" 1 | grep -q "dirty=1 draft=1" && nth "$out" 1 | grep -q "Draft restored"'
expect "AT-04: another session changed the view: Apply shows the three-way comparison, nothing published" 'nth "$out" 2 | grep -q "modal=3" && printf "%s\n" "$out" | grep "^objplan" | head -1 | grep -q "blocked=1.*conflict=1"'
expect "AT-04: a new explicit decision (keep mine) leads to a new review against the server" 'nth "$out" 3 | grep -q "modal=1" && printf "%s\n" "$out" | grep "^objplan" | tail -1 | grep -q "conflict=0"'
expect "AT-04: published only after that review" 'nth "$out" 4 | grep -q "state=Applied" && db "SELECT sql FROM sqlite_master WHERE name=\"v_top\"" | grep -q "amount > 30"'

# ---- AT-10: read-only connection
out=$(run obj_c)
expect "every script target found (read-only)" '[ "$(missing "$out")" = 0 ]'
expect "AT-10: the review says the connection is read-only and Apply stays disabled" 'nth "$out" 1 | grep -q "modal=1" && printf "%s\n" "$out" | grep "^objplan" | head -1 | grep -q "blocked=1.*read-only" && nth "$out" 2 | grep -q "modal=1"'
expect "AT-10: no DDL reached the database; the draft was exported" '[ "$(sqlite3 $P/ro.db "SELECT sql FROM sqlite_master WHERE name=\"v_revenue\"" | grep -c "SELECT id FROM sales")" = 0 ] && grep -q "SELECT id FROM sales" build/ui/obj_export.sql'

# ---- diagnostics, quick fix, server validation
out=$(run obj_d)
expect "every script target found (diagnostics)" '[ "$(missing "$out")" = 0 ]'
expect "OBJ-022 / 026 / 027: OR REPLACE error, SELECT * warning, unknown table from the catalog, while typing" 'printf "%s\n" "$out" | grep -q "objproblem error DLC001" && printf "%s\n" "$out" | grep -q "objproblem warning INS001" && printf "%s\n" "$out" | grep -q "objproblem warning REF001.*missing_tbl"'
expect "OBJ-028: the quick fix is previewed, then rewrites the verb (error gone)" 'nth "$out" 2 | grep -q "problems=2" && ! printf "%s\n" "$out" | sed -n "/^objstate/{n;}" | awk "/objstate/{c++} c==2" | grep -q DLC001'
expect "OBJ-024 / 031: validation on the server (temporary view, rolled back) reports the missing table" 'nth "$out" 3 | grep -q "validated=-1" && nth "$out" 3 | grep -q "server_problems=1" && nth "$out" 3 | grep -qi "no such table: missing_tbl"'
expect "OBJ-031: the server check left nothing behind" '[ "$(db "SELECT count(*) FROM sqlite_temp_master")" = 0 ] && db "SELECT sql FROM sqlite_master WHERE name=\"v_revenue\"" | grep -q "amount \* 2"'

# ---- OBJ-021 / 066 / 043: rename, compare with another connection, several objects at once
out=$(run obj_e)
expect "every script target found (rename / compare / batch)" '[ "$(missing "$out")" = 0 ]'
expect "OBJ-021: Rename Symbol replaced the alias everywhere (whole words, not strings)" 'grep -q "Renamed r to rev in 3 place" $P/logs/lume.log'
expect "OBJ-066: compared with the same view on another connection (RO copy)" 'nth "$out" 2 | grep -q "modal=2"'
expect "OBJ-043: the two modified views are planned together" 'nth "$out" 3 | grep -q "modal=7"'
expect "OBJ-043: applied in order, each in its own deployment" '[ "$(grep -c "result=applied" $P/logs/object-audit.log)" -ge 4 ] && db "SELECT sql FROM sqlite_master WHERE name=\"v_revenue\"" | grep -q "amount + 1" && db "SELECT sql FROM sqlite_master WHERE name=\"v_top\"" | grep -q "rev.amount"'

out=$(run obj_f)
expect "every script target found (extract CTE)" '[ "$(missing "$out")" = 0 ]'
expect "OBJ-021: Extract Subquery to CTE rewrote the view with WITH big AS (...) FROM big r" 'printf "%s\n" "$out" | grep "^edtext" | sed -n 1p | grep -qF "AS WITH big AS (SELECT id, amount FROM sales WHERE amount > 0)" && printf "%s\n" "$out" | grep "^edtext" | sed -n 1p | grep -qF "SELECT r.id FROM big r"'
expect "OBJ-021: Qualify Columns used the catalog: id / amount -> s., msg -> l." 'printf "%s\n" "$out" | grep "^edtext" | sed -n 3p | grep -qF "SELECT s.id, s.amount, l.msg FROM sales s JOIN log l"'
expect "OBJ-021 / 014: one Undo brings the text back" 'printf "%s\n" "$out" | grep "^edtext" | sed -n 2p | grep -q "FROM (SELECT id, amount FROM sales WHERE amount > 0) r"'

# ---- AT-08 / OBJ-079: large definition
out=$(run obj_big)
expect "every script target found (large definition)" '[ "$(missing "$out")" = 0 ]'
expect "AT-08: a 6000-line definition opens" 'nth "$out" 1 | grep -q "loaded=1"'
fr() { printf '%s\n' "$out" | grep "^stat $1:" | sed 's/.*frame \([0-9.]*\) ms.*/\1/'; }
expect "AT-08: typing / diff request / format frames stay under 100 ms (open $(fr open), typing $(fr typing), diff $(fr diff-requested), format $(fr formatted) ms)" \
  'for k in typing diff-requested formatted; do v=$(fr $k); [ -n "$v" ] && awk "BEGIN{exit !($v < 100)}" || exit 1; done'
expect "OBJ-075: memory stays bounded ($(printf '%s\n' "$out" | grep '^stat formatted' | sed 's/.*rss \([0-9.]*\) MB.*/\1/') MB RSS)" 'v=$(printf "%s\n" "$out" | grep "^stat formatted" | sed "s/.*rss \([0-9.]*\) MB.*/\1/"); awk "BEGIN{exit !($v < 400)}"'

# ---- PostgreSQL (dbv-postgres): AT-02, AT-03, PROD confirmation
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then
  Q=build/objects-pg; rm -rf $Q; mkdir -p $Q
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  psql_() { docker exec dbv-postgres psql -U postgres -d syn -qtA -c "$1"; }
  psql_ "DROP SCHEMA IF EXISTS lume_ui CASCADE; CREATE SCHEMA lume_ui; CREATE TABLE lume_ui.sales(id int PRIMARY KEY, amount numeric(12,2)); INSERT INTO lume_ui.sales VALUES (1, 10), (2, 20);
         CREATE VIEW lume_ui.v_sales AS SELECT id, amount FROM lume_ui.sales; CREATE VIEW lume_ui.v_prod AS SELECT 1 AS a;
         CREATE MATERIALIZED VIEW lume_ui.mv_total AS SELECT sum(amount) AS total FROM lume_ui.sales;
         CREATE FUNCTION lume_ui.f_pos(p_a integer) RETURNS integer LANGUAGE sql AS \$f\$ SELECT \$1 * 2 \$f\$;
         CREATE FUNCTION lume_ui.f_vars(p_a integer, p_b text) RETURNS numeric LANGUAGE plpgsql AS \$f\$ DECLARE v_total numeric := 0; v_name text; BEGIN RETURN v_total; END \$f\$;
         CREATE FUNCTION lume_ui.calculate(p integer) RETURNS numeric LANGUAGE sql AS \$f\$ SELECT p * 2.0 \$f\$;
         CREATE FUNCTION lume_ui.calculate(p text) RETURNS numeric LANGUAGE sql AS \$f\$ SELECT length(p)::numeric \$f\$; COMMENT ON FUNCTION lume_ui.calculate(integer) IS 'doubles p';" >/dev/null
  printf '[connection]\nid=pga\nname=syn A\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n\n[connection]\nid=pgprod\nname=syn PROD\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=PROD\n' > $Q/connections.txt
  ( umask 077; for id in pga pgprod; do printf '%s\tpassword\t%s\n' "$id" "$PW"; done > $Q/secrets.txt )
  out=$(LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/obj_pg.txt ./build/lume 2>&1); printf "%s\n" "$out" > build/objects_pg_script.out
  expect "every script target found (PostgreSQL)" '[ "$(missing "$out")" = 0 ]'
  expect "OBJ-047: the PostgreSQL view opens as CREATE OR REPLACE VIEW" 'nth "$out" 1 | grep -q "key=\"VIEW lume_ui.v_sales\" state=Synced"'
  expect "AT-02: changing a column type: no simple CREATE OR REPLACE; the review requires the explicit plan" 'printf "%s\n" "$out" | grep "^objplan" | head -1 | grep -q "blocked=1 destructive=1 destructive_available=1.*changes type"'
  expect "AT-02 / OBJ-040: the destructive plan needs the specific consent (Apply did nothing before the box was ticked)" 'nth "$out" 3 | grep -q "modal=1" && nth "$out" 3 | grep -q "Modified"'
  expect "AT-02: applied after consent: DROP + CREATE in one transaction, never CASCADE" 'nth "$out" 4 | grep -q "state=Applied" && [ "$(psql_ "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = '"'"'lume_ui.v_sales'"'"'::regclass AND attname = '"'"'amount'"'"'")" = text ] && ! printf "%s\n" "$out" | grep "^objsql" | grep -qi cascade'
  expect "AT-03: calculate(integer) replaced, calculate(text) untouched" 'nth "$out" 6 | grep -q "state=Applied" && [ "$(psql_ "SELECT lume_ui.calculate(2)")" = 6.0 ] && [ "$(psql_ "SELECT lume_ui.calculate('"'"'abc'"'"'::text)")" = 3 ]'
  expect "OBJ-068: PROD needs the object name typed: Apply did nothing before it" 'nth "$out" 7 | grep -q "key=\"VIEW lume_ui.v_prod\" state=Modified.*modal=1"'
  expect "OBJ-068: published to PROD after typing the name" 'nth "$out" 8 | grep -q "state=Applied" && psql_ "SELECT pg_get_viewdef('"'"'lume_ui.v_prod'"'"'::regclass)" | grep -q "AS b"'
  expect "OBJ-056: Refresh Materialized View Data is its own reviewed operation (REFRESH, the definition untouched)" 'nth "$out" 9 | grep -q "modal=1" && printf "%s\n" "$out" | grep -q "^objsql 1: REFRESH MATERIALIZED VIEW \"lume_ui\".\"mv_total\"" && nth "$out" 10 | grep -q "state=Applied"'
  expect "OBJ-018: typing a routine call shows every overload with argument names, the current one in [ ], the return type and the catalog comment" 'printf "%s\n" "$out" | grep "^objhint" | tail -1 | grep -q "calculate(\[p integer\]) returns numeric  -  doubles p" && printf "%s\n" "$out" | grep "^objhint" | tail -1 | grep -q "calculate(\[p text\]) returns numeric"'
  expect "OBJ-017: completion in an object tab offers the routine's parameters and DECLAREd variables, with their types" 'printf "%s\n" "$out" | grep "^objcomplete" | grep -q "p_a=parameter integer;.*p_b=parameter text;.*v_total=variable numeric;.*v_name=variable text;"'
  expect "OBJ-021: Use Parameter Names turned \$1 into p_a (previewed, one undo step)" 'printf "%s\n" "$out" | grep "^edtext" | tail -1 | grep -q "SELECT p_a \* 2"'
  expect "OBJ-058: a function owned by the pg_stat_statements extension opens read-only, saying why" 'printf "%s\n" "$out" | grep "^objstate" | tail -1 | grep -q "read_only=1.*owned by extension pg_stat_statements"'
  expect "OBJ-071: the PROD deployment is audited with its environment" 'grep -q "env=PROD .*object=\"VIEW lume_ui.v_prod\".*result=applied" $Q/logs/object-audit.log'
  expect "no password in logs, drafts or the audit (credentials never stored)" '! grep -rqF "$PW" $Q/logs $Q/drafts $Q/backups 2>/dev/null'

  # AT-09: a schema with ~16 000 tables (syn_fat): the publication must not reload the catalog
  psql_ "CREATE OR REPLACE VIEW syn_fat.lume_v_fat AS SELECT 1 AS a" >/dev/null
  : > $Q/logs/lume.log
  out=$(LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/obj_fat.txt ./build/lume 2>&1)
  nt=$(sed -n '/deployment .* of VIEW syn_fat.lume_v_fat/,$p' $Q/logs/lume.log | grep -c "catalog reloaded: syn A")
  ntab=$(grep -m1 "connected: syn A" $Q/logs/lume.log | sed 's/.*, \([0-9]*\) tables/\1/')
  expect "AT-09: applied in a schema of a ${ntab}-table catalog; no global re-introspection after it ($nt reloads)" 'nth "$out" 1 | grep -q "state=Applied" && [ "$nt" = 0 ]'
  fa=$(printf '%s\n' "$out" | grep "^stat after-apply" | sed 's/.*frame \([0-9.]*\) ms.*/\1/'); ft=$(printf '%s\n' "$out" | grep "^stat typing" | sed 's/.*frame \([0-9.]*\) ms.*/\1/')
  expect "AT-09: the Explorer stays responsive after the targeted refresh (frames $fa / $ft ms)" '[ -n "$fa" ] && awk "BEGIN{exit !($fa < 100 && $ft < 100)}"'
  psql_ "DROP VIEW IF EXISTS syn_fat.lume_v_fat" >/dev/null
  psql_ "DROP SCHEMA IF EXISTS lume_ui CASCADE" >/dev/null
else echo "skip  PostgreSQL scenarios: dbv-postgres not running"; fi

# ---- MySQL (lume-mysql test container): AT-06 through the UI
MYENV="${LUME_MYSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mysql/.env}"
if docker ps --format '{{.Names}}' | grep -qx lume-mysql && [ -f "$MYENV" ]; then
  M=build/objects-mysql; rm -rf $M; mkdir -p $M
  MYSQL_PW=$(grep '^MYSQL_PW=' "$MYENV" | cut -d= -f2-)
  my_() { docker exec -e MYSQL_PWD="$MYSQL_PW" lume-mysql mysql -uroot shop -e "$1" 2>/dev/null; }
  my_ "DROP TABLE IF EXISTS sales; CREATE TABLE sales(id INT PRIMARY KEY, amount DECIMAL(12,2)); INSERT INTO sales VALUES (1, 10); DROP PROCEDURE IF EXISTS p_bump; CREATE PROCEDURE p_bump(IN x INT) UPDATE sales SET amount = amount + x WHERE id = 1"
  printf '[connection]\nid=myshop\nname=shop MySQL\ndriver=mysql\nhost=127.0.0.1\nport=13306\ndatabase=shop\nuser=root\n' > $M/connections.txt
  ( umask 077; printf 'myshop\tpassword\t%s\n' "$MYSQL_PW" > $M/secrets.txt )
  out=$(LUME_PREF_DIR=$M LUME_SCRIPT=tests/scripts/obj_mysql.txt ./build/lume 2>&1)
  expect "every script target found (MySQL)" '[ "$(missing "$out")" = 0 ]'
  expect "BL-013: a MySQL procedure opens from its connection (SHOW CREATE, definer included)" 'nth "$out" 1 | grep -q "key=\"PROCEDURE shop.p_bump()\" state=Synced.*loaded=1"'
  expect "AT-06 (UI): the review refuses a plain apply: DROP + CREATE needs the explicit plan" 'nth "$out" 2 | grep -q "modal=1" && printf "%s\n" "$out" | grep "^objplan" | head -1 | grep -q "blocked=1 destructive=1 destructive_available=1"'
  expect "AT-06 (UI): after the explicit plan and the consent, DROP + CREATE ran and the new body is on the server" 'nth "$out" 3 | grep -q "state=Applied" && my_ "SHOW CREATE PROCEDURE p_bump\G" | grep -q "amount \* x"'
  expect "no MySQL password in logs or the audit" '! grep -rqF "$MYSQL_PW" $M/logs $M/backups 2>/dev/null'
else echo "skip  MySQL scenarios: lume-mysql not running"; fi

# ---- Oracle (dbv-oracle, Instant Client in ~/oracle/instantclient): OBJ-030 / 050 / 051 / 057, AT-05 in the UI
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
if docker ps --format '{{.Names}}' | grep -qx dbv-oracle && [ -f "$OENV" ] && [ -e "$HOME/oracle/instantclient/libclntsh.dylib" ]; then
  O=build/objects-ora; rm -rf $O; mkdir -p $O
  OPW=$(grep '^ORACLE_PASSWORD=' "$OENV" | cut -d= -f2- | tr -d '"')
  docker exec -i dbv-oracle sqlplus -s "system/$OPW@//localhost:1521/FREEPDB1" >/dev/null <<'SQL'
CREATE OR REPLACE PACKAGE lume_uipkg AS FUNCTION f(x NUMBER) RETURN NUMBER; END lume_uipkg;
/
CREATE OR REPLACE PACKAGE BODY lume_uipkg AS FUNCTION f(x NUMBER) RETURN NUMBER IS BEGIN RETURN x * 2; END; END lume_uipkg;
/
CREATE OR REPLACE TYPE lume_uitype AS OBJECT (n NUMBER, MEMBER FUNCTION twice RETURN NUMBER);
/
CREATE OR REPLACE TYPE BODY lume_uitype AS MEMBER FUNCTION twice RETURN NUMBER IS BEGIN RETURN n * 2; END; END;
/
EXIT
SQL
  printf '[connection]\nid=ora1\nname=ora DEV\ndriver=oracle\nhost=localhost\nport=15210\nservice=FREEPDB1\nuser=system\nenv=DEV\n' > $O/connections.txt
  ( umask 077; printf 'ora1\tpassword\t%s\n' "$OPW" > $O/secrets.txt )
  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/obj_ora.txt ./build/lume 2>&1)
  expect "every script target found (Oracle)" '[ "$(missing "$out")" = 0 ]'
  expect "OBJ-050: the package specification opens (ALL_SOURCE) with its own status" 'nth "$out" 1 | grep -q "key=\"PACKAGE SYSTEM.LUME_UIPKG\" state=Synced.*status=VALID"'
  expect "OBJ-050: More > Open Body opens the body in its own tab" 'nth "$out" 2 | grep -q "key=\"PACKAGE BODY SYSTEM.LUME_UIPKG\" state=Synced.*loaded=1"'
  expect "AT-05 / OBJ-030 (UI): a broken body is published, read back INVALID with the PLS error on line 4 in Problems; never reported as success" 'nth "$out" 3 | grep -q "state=Failed.*server_problems=[1-9].*status=INVALID" && printf "%s\n" "$out" | grep "^objproblem server PLS-" | grep -q "line 4"'
  expect "AT-05: the fixed body compiles VALID" 'nth "$out" 4 | grep -q "state=Applied.*status=VALID"'
  expect "OBJ-051: Compile is its own reviewed operation (ALTER PACKAGE ... COMPILE BODY), status read back" 'nth "$out" 5 | grep -q "modal=1" && printf "%s\n" "$out" | grep -q "^objsql 1: ALTER PACKAGE \"SYSTEM\".\"LUME_UIPKG\" COMPILE BODY" && nth "$out" 6 | grep -q "status=VALID"'
  expect "OBJ-057: TYPE and TYPE BODY open as separate objects" 'nth "$out" 7 | grep -q "key=\"TYPE SYSTEM.LUME_UITYPE\".*loaded=1" && nth "$out" 8 | grep -q "key=\"TYPE BODY SYSTEM.LUME_UITYPE\".*loaded=1"'
  expect "no Oracle password in logs or the audit" '! grep -rqF "$OPW" $O/logs $O/backups 2>/dev/null'
else echo "skip  Oracle UI: dbv-oracle or the Instant Client not available"; fi

echo "OBJECTS UI: $( [ $fails = 0 ] && echo OK || echo FAILED ) ($fails failures)"
[ $fails = 0 ]
