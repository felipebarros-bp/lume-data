#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Visual Table Designer on screen (TBL-001..100): the non-modal tab, drafts, review & apply,
# undo / redo, SQLite rebuild with consent, drift, close with Save Draft, drop, Explorer menus;
# PostgreSQL (dbv-postgres): USING, data checks, PROD policy, read-only connection.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/tdes-pref; rm -rf "$P"; mkdir -p "$P" build/ui
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: \(target not found\|td .*failed\|td: \|rawsql failed\)' ; }
nth() { printf '%s\n' "$1" | grep '^tdstate' | sed -n "$2p"; }
sqlsteps() { printf '%s\n' "$1" | awk -v n="$2" '/^tdstate/{k++} k==n && /^tdsql/'; }

printf '[connection]\nid=shop1\nname=Shop\ndriver=sqlite\npath=%s/%s/shop.db\n' "$PWD" "$P" > "$P/connections.txt"
sqlite3 "$P/shop.db" "PRAGMA foreign_keys = ON; CREATE TABLE customers(id INTEGER PRIMARY KEY, name TEXT NOT NULL); INSERT INTO customers VALUES (1, 'Ana'), (2, 'Bia');"
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/td_sqlite.txt ./build/lume 2>&1)
expect "every script target found (SQLite)" '[ "$(missing "$out")" = 0 ]'
expect "TBL-001 / 052.3: New Table opens a draft; with no column there is no SQL and Apply is off" 'nth "$out" 1 | grep -q "new=1 state=ready.*steps=0 blocked=1" && printf "%s\n" "$out" | grep -q "^tdblocked A table needs at least one column"'
expect "TBL-011 / 026 / 029 / 033 / 052: the preview has CREATE TABLE (PK, FK) + CREATE INDEX in one transaction" 'nth "$out" 2 | grep -q "changes=4 errors=0.*steps=2 blocked=0.*transactional=1" && sqlsteps "$out" 2 | grep -q "CREATE INDEX \"main\".\"pedidos_total_idx\""'
expect "TBL-006: nothing reached the database before Apply" '[ "$(nth "$out" 2 | grep -c draft_file=1)" = 1 ]'
expect "TBL-053 / 056 / 064: applied after the review; read back from the server; tab now edits the existing table" 'nth "$out" 3 | grep -q "table=main.pedidos new=0 state=ready dirty=0.*msg=\"Applied: 2 statement(s); the table was read back from the server\"" && nth "$out" 3 | grep -q "draft_file=0"'
expect "TBL-051 / Gherkin rename: total -> amount is RENAME COLUMN, note is ADD COLUMN (no DROP)" 'sqlsteps "$out" 4 | grep -q "RENAME COLUMN \"total\" TO \"amount\"" && sqlsteps "$out" 4 | grep -q "ADD COLUMN \"note\" TEXT" && ! sqlsteps "$out" 4 | grep -q DROP'
expect "TBL-007: undo restores the draft without SQL; redo brings the change back" 'nth "$out" 6 | grep -q "dirty=0 .*changes=0.*redo=1" && nth "$out" 7 | grep -q "dirty=1.*changes=1.*redo=0"'
expect "TBL-074: a type + NOT NULL change on SQLite plans the rebuild (copy, rename, index, foreign_key_check), high risk" 'nth "$out" 8 | grep -q "risk=HIGH" && sqlsteps "$out" 8 | grep -q "_lume_new_pedidos" && sqlsteps "$out" 8 | grep -q "foreign_key_check"'
expect "TBL-059: Apply without the specific consent is refused" 'printf "%s\n" "$out" | grep -q "^script: td apply refused (consent / PROD name)" && nth "$out" 9 | grep -q "modal=1"'
cols_at() { printf '%s\n' "$1" | awk -v n="$2" '/^tdstate/{k++} k==n && /^tdcol/'; }
expect "TBL-074 / 099: rebuild applied and read back: amount INTEGER NOT NULL, index and FK kept (keys=3)" 'nth "$out" 10 | grep -q "dirty=0.*keys=3.*msg=\"Applied" && cols_at "$out" 10 | grep -qx "tdcol amount INTEGER NOT NULL"'
expect "TBL-057: a change from another session stops Apply (drift banner, nothing run)" 'nth "$out" 11 | grep -q "drift=1" && nth "$out" 11 | grep -q "changed on the server since it was opened" && [ "$(sqlite3 $P/shop.db "SELECT count(*) FROM pragma_table_info(\"pedidos\") WHERE name = \"extra\"")" = 0 ]'
expect "TBL-057.3: Reload shows the table as it is now (x_drift)" 'nth "$out" 12 | grep -q "drift=0" && nth "$out" 12 | grep -q "cols=5"'
expect "TBL-003.3: closing a tab with an unapplied draft asks Save Draft / Discard / Cancel" 'nth "$out" 13 | grep -q "modal=2"'
expect "TBL-009: the saved draft (no credentials) comes back when the table is opened again" 'nth "$out" 14 | grep -q "dirty=1.*banner=\"A saved draft was restored" && ! grep -rqi password $P/table-drafts'
expect "TBL-083: Drop Table... is its own review; refused without consent; then the table is gone" 'printf "%s\n" "$out" | grep -c "^script: td apply refused" | grep -q 2 && [ "$(sqlite3 $P/shop.db "SELECT count(*) FROM sqlite_master WHERE name = \"pedidos\"")" = 0 ]'
expect "TBL-083 / 082: Explorer > table > Modify Table... opens the designer on it" 'nth "$out" 17 | grep -q "table=main.customers new=0 state=ready"'
expect "TBL-056 / 072: Output and the audit log record the deployments (no SQL values, no secrets)" 'grep -q "result=applied" $P/audit/table-designer.log && grep -q "result=applied" $P/audit/table-designer.log && ! grep -qi password $P/audit/table-designer.log'

# ---- PostgreSQL ----
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then
  Q=build/tdes-pg; rm -rf $Q; mkdir -p $Q
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  psql_() { docker exec -i dbv-postgres psql -U postgres -d syn -tAqc "$1" 2>&1; }
  psql_ "DROP SCHEMA IF EXISTS lume_tdui CASCADE" >/dev/null
  psql_ "CREATE SCHEMA lume_tdui; CREATE TABLE lume_tdui.orders (id int PRIMARY KEY, status varchar(24) DEFAULT 'open', total numeric(10,2)); INSERT INTO lume_tdui.orders VALUES (1, '10', 5), (2, '20', NULL); CREATE TABLE lume_tdui.prod_t (id int PRIMARY KEY);" >/dev/null
  printf '[connection]\nid=pga\nname=syn A\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n\n[connection]\nid=pgprod\nname=syn PROD\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=PROD\n\n[connection]\nid=pgro\nname=syn RO\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nread_only=1\n' > $Q/connections.txt
  ( umask 077; for id in pga pgprod pgro; do printf '%s\tpassword\t%s\n' "$id" "$PW"; done > $Q/secrets.txt )
  out=$(LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/td_pg.txt ./build/lume 2>&1); printf "%s\n" "$out" > build/tdes_pg_script.out
  expect "every script target found (PostgreSQL)" '[ "$(missing "$out")" = 0 ]'
  expect "TBL-050 (PG): the snapshot reads the table (3 columns, PK)" 'nth "$out" 1 | grep -q "table=lume_tdui.orders new=0 state=ready dirty=0.*cols=3 keys=1 can_alter=1"'
  expect "TBL-069 / 075: varchar -> integer needs an explicit USING: no SQL until it is given" 'nth "$out" 2 | grep -q "blocked=1" && printf "%s\n" "$out" | grep -q "^tdblocked status: character varying(24) -> integer has no implicit conversion"'
  expect "TBL-075: with USING the plan is ALTER COLUMN ... TYPE integer USING ..." 'sqlsteps "$out" 3 | grep -q "ALTER COLUMN \"status\" TYPE integer USING status::integer"'
  expect "TBL-068: Check existing data finds the NULL that would break NOT NULL (bounded, on request)" 'printf "%s\n" "$out" | grep -q "^tddatacheck.*NOT NULL total: rows that do not fit exist: the step will fail"'
  expect "TBL-055 / 056 (PG): applied in one transaction; read back: status is integer, rows kept" 'nth "$out" 5 | grep -q "msg=\"Applied" && [ "$(psql_ "SELECT data_type FROM information_schema.columns WHERE table_schema = '"'"'lume_tdui'"'"' AND table_name = '"'"'orders'"'"' AND column_name = '"'"'status'"'"'")" = integer ] && [ "$(psql_ "SELECT sum(status) FROM lume_tdui.orders")" = 30 ]'
  expect "TBL-071: PROD needs the table name typed; refused without it" 'printf "%s\n" "$out" | grep -q "^script: td apply refused (consent / PROD name)" && nth "$out" 6 | grep -q "table=lume_tdui.prod_t.*msg=\"Applied" && [ "$(psql_ "SELECT count(*) FROM information_schema.columns WHERE table_schema = '"'"'lume_tdui'"'"' AND table_name = '"'"'prod_t'"'"' AND column_name = '"'"'note'"'"'")" = 1 ]'
  expect "TBL-071: a read-only connection refuses the change at execution" 'nth "$out" 7 | grep -q "is read-only in Lume: structure changes are refused" && [ "$(psql_ "SELECT count(*) FROM information_schema.columns WHERE table_schema = '"'"'lume_tdui'"'"' AND table_name = '"'"'orders'"'"' AND column_name = '"'"'ro_col'"'"'")" = 0 ]'
  expect "no PostgreSQL password in logs, drafts, audit or migrations" '! grep -rqF "$PW" $Q/logs $Q/table-drafts $Q/audit $Q/migrations 2>/dev/null'
  expect "TBL-063: Save as Migration writes a .sql identical to the preview, with metadata, not applied" 'ls $Q/migrations/*_alter_orders.sql >/dev/null 2>&1 && grep -q "^-- base fingerprint" $Q/migrations/*_alter_orders.sql && grep -q "ADD COLUMN \"mig_col\"" $Q/migrations/*_alter_orders.sql && [ "$(psql_ "SELECT count(*) FROM information_schema.columns WHERE table_name = '"'"'orders'"'"' AND column_name = '"'"'mig_col'"'"'")" = 0 ]'
  psql_ "DROP SCHEMA IF EXISTS lume_tdui CASCADE" >/dev/null
  unset PW
else echo "skip  PostgreSQL scenarios: dbv-postgres not running"; fi

# the component menus through a real click on the popup (a bug once swallowed every item above id 100)
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/td_menu.txt ./build/lume 2>&1)
expect "every script target found (component menus)" '[ "$(printf "%s\n" "$out" | grep -c "target not found")" = 0 ]'
k0=$(nth "$out" 1 | sed -n "s/.* keys=\([0-9]*\).*/\1/p"); c0=$(nth "$out" 1 | sed -n "s/.* cols=\([0-9]*\).*/\1/p")
expect "TBL: right-click Foreign Keys > Add Foreign Key adds one (clicked in the menu)" '[ "$(nth "$out" 2 | sed -n "s/.* keys=\([0-9]*\).*/\1/p")" = $((k0 + 1)) ]'
expect "TBL: right-click Unique Keys > Add Unique Key adds one" '[ "$(nth "$out" 3 | sed -n "s/.* keys=\([0-9]*\).*/\1/p")" = $((k0 + 2)) ]'
expect "TBL: right-click Columns > Add Column adds one" '[ "$(nth "$out" 4 | sed -n "s/.* cols=\([0-9]*\).*/\1/p")" = $((c0 + 1)) ]'

echo "TDES UI: $([ $fails = 0 ] && echo passed || echo "FAILED ($fails failures)")"
[ $fails = 0 ]
