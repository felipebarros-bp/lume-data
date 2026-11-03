#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Schema Explorer v2 on screen (DBX-077..126): Schemas + Others on PostgreSQL with 170,009
# namespaces (database lume_scale in dbv-postgres, created by tests/nsx_pg_setup.sh), Oracle
# ALL_USERS pages, MySQL "Databases", SQLite's own tree, persistence, menus, idle CPU.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
mkdir -p build/ui
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: target not found\|^nsx: ' ; }
st() { printf '%s\n' "$1" | grep '^nsx mode' | sed -n "$2p"; }
menu() { printf '%s\n' "$1" | grep '^nsx menu' | sed -n "$2p"; }
props() { printf '%s\n' "$1" | grep '^nsx props' | sed -n "$2p"; }
cput() { ps -o time= -p $1 | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*100+$3; else print (($1*60+$2)*60+$3)*100+$4 }'; }

# ---- PostgreSQL, 170,009 namespaces ----
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then
  PGOPTIONS="-c client_min_messages=warning" sh tests/nsx_pg_setup.sh >/dev/null 2>&1 || echo "FAIL  lume_scale setup"
  Q=build/nsx-pg; rm -rf $Q; mkdir -p $Q
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  printf '[connection]\nid=scale\nname=scale\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=lume_scale\nuser=postgres\nenv=DEV\n' > $Q/connections.txt
  ( umask 077; printf 'scale\tpassword\t%s\n' "$PW" > $Q/secrets.txt )
  out=$(LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/nsx_pg.txt ./build/lume 2>&1)
  expect "every script target found (PostgreSQL)" '[ "$(missing "$out")" = 0 ]'
  expect "DBX-077 / 083: Schemas = current + schemas with known tables (7), Others (169,999) exact, no name page read at connect" 'st "$out" 1 | grep -q "featured=7 others=\"Others (169,999)\" .*loaded=0 pages=0 .*queries=2 " && st "$out" 1 | grep -q "list=\[abc:a,atendimento:a,clientes:a,privado:a,public:c,usr_165000:a,xpto:a\]"'
  expect "DBX-088: the tree holds rows for highlighted schemas only before Others opens (no 170k nodes)" 'st "$out" 1 | grep -q "schema_rows=7 "'
  expect "DBX-104: Schemas menu = Search / Refresh / View Options / Show All / Copy Loaded Names / Properties" 'menu "$out" 1 | grep -q "\[Search Schemas…\] \[Refresh\] \[View Options…\] \[Show All Schemas\] \[Copy Loaded Names (7)\] \[Properties\]"'
  expect "DBX-079 / 084: expanding Others reads one page (100 names), highlighted ones not repeated (95 rows)" 'st "$out" 2 | grep -q "shown=95 loaded=100 pages=1 has_more=1" && st "$out" 2 | grep -q "schema_rows=102 "'
  expect "DBX-105: Others menu = Search in Others / Load More / Refresh Others / Show All / View Options; no DDL" 'menu "$out" 2 | grep -q "\[Search in Others…\] \[Load More\] \[Refresh Others\] \[Show All Schemas\] \[View Options…\]" && ! menu "$out" 2 | grep -qi "drop\|delete"'
  expect "DBX-084: Load More = the next page only (200 names)" 'st "$out" 3 | grep -q "loaded=200 pages=2"'
  expect "DBX-085 / 120.3: search finds usr_165000 on the server; no page walked (still 2 pages)" 'st "$out" 4 | grep -q "hits=1 \[usr_165000\]" && st "$out" 4 | grep -q "pages=2 "'
  expect "DBX-081 / 121.4: Pin to Schemas: 8 highlighted, Others (169,998)" 'st "$out" 5 | grep -q "featured=8 others=\"Others (169,998)\"" && st "$out" 5 | grep -q "usr_000050:p"'
  expect "DBX-107: menu of a pinned schema offers Unpin / Move to Others, Console, Find Objects, Verify, Properties" 'menu "$out" 3 | grep -q "\[Open SQL Console\].*\[Find Objects…\].*\[Verify Visible Objects\].*\[Unpin (Move to Others)\] \[Copy Qualified Name\] \[Properties\]"'
  expect "DBX-081 / 113: Move to Others: back in Others (7 highlighted, Others (169,999))" 'st "$out" 6 | grep -q "featured=7 others=\"Others (169,999)\"" && ! st "$out" 6 | grep -q "usr_000050:"'
  expect "DBX-081.2: the current schema cannot be moved to Others (disabled item)" 'menu "$out" 4 | grep -q "\[-Move to Others (not for the current schema)\]"'
  expect "DBX-092 / 107: Verify Visible Objects on an empty schema: 'no visible objects for this session (not proof that it is empty)'" 'props "$out" 1 | grep -q "Visible objects: No visible objects for this session (not proof that it is empty)" && props "$out" 1 | grep -q "In Others: not highlighted (this says nothing about its objects)"'
  expect "DBX-078: a schema with tables: Has visible objects; shown under Schemas (known objects)" 'props "$out" 2 | grep -q "Has visible objects" && props "$out" 2 | grep -q "Shown under Schemas: known objects"'
  expect "DBX-117: Properties of Schemas = reads, timings, cancels, timeouts (no names)" 'props "$out" 3 | grep -q "Catalog reads: [0-9]* · ready in [0-9]* ms · last page [0-9]* ms" && props "$out" 3 | grep -q "Cancelled 0 · timed out 0"'
  expect "DBX-080 / 104.3: Show All Schemas = one list, paged (no Others node)" 'st "$out" 7 | grep -q "mode=all .*others=\"-\""'
  expect "DBX-087: Find Objects finds clientes.vw_clientes by name" 'st "$out" 8 | grep -q "hits=1 \[clientes.vw_clientes\]"'
  expect "DBX-120.4: no object query per listed schema: probes ran only for the 2 schemas asked" '[ "$(st "$out" 8 | sed "s/.*queries=\([0-9]*\).*/\1/")" -le 14 ]'
  expect "DBX-114: the log has page counts, never schema names" 'grep -q "explorer: Schemas page" $Q/logs/lume.log && ! grep -q "usr_0000\|usr_1650" $Q/logs/lume.log'
  expect "DBX-112.4: schemas.txt holds preferences only, no password" 'grep -q "pin=usr_000077" $Q/schemas.txt && ! grep -qF "$PW" $Q/schemas.txt && ! grep -qi "password" $Q/schemas.txt'
  cp $Q/schemas.txt $Q/schemas.before
  out2=$(LUME_PREF_DIR=$Q LUME_SCRIPT=tests/scripts/nsx_pg2.txt ./build/lume 2>&1)
  expect "DBX-112: after a restart the pin (usr_000077) and the suppression (usr_000050) persist" 'st "$out2" 1 | grep -q "usr_000077:p" && st "$out2" 1 | grep -q "others=\"Others (169,998)\"" && grep -q "suppress=usr_000050" $Q/schemas.before'
  expect "DBX-108.4: Reset for this connection clears the tree preferences only" 'st "$out2" 2 | grep -q "featured=7 " && ! grep -q "pin=usr_000077" $Q/schemas.txt && grep -q "^id=scale" $Q/connections.txt'
  # idle: Others closed / open, window idle: nothing recurring (DBX-125.4)
  printf 'wait 500\ndnode "scale"\nwait 3000\nnsx "scale" expand\nwait 9000\nquit\n' > $Q/idle.txt
  LUME_PREF_DIR=$Q LUME_SCRIPT=$Q/idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
  sleep 6; c0=$(cput $pid); sleep 3; c1=$(cput $pid); wait $pid
  cpu=$(awk -v a="$c0" -v b="$c1" 'BEGIN{printf "%.1f", (b-a)/3}')
  expect "DBX-125.4 / 120.5: idle with Others open: $cpu % CPU (bar 2 %)" 'awk -v c="$cpu" "BEGIN{exit !(c + 0 <= 2.0)}"'
  unset PW
else echo "skip  PostgreSQL scale scenarios: dbv-postgres not running"; fi

# ---- MySQL: Databases ----
MYENV="${LUME_MYSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mysql/.env}"
if docker ps --format '{{.Names}}' | grep -qx lume-mysql && [ -f "$MYENV" ]; then
  M=build/nsx-mysql; rm -rf $M; mkdir -p $M
  MYSQL_PW=$(grep '^MYSQL_PW=' "$MYENV" | cut -d= -f2-)
  printf '[connection]\nid=myshop\nname=shop MySQL\ndriver=mysql\nhost=127.0.0.1\nport=13306\ndatabase=shop\nuser=root\n' > $M/connections.txt
  ( umask 077; printf 'myshop\tpassword\t%s\n' "$MYSQL_PW" > $M/secrets.txt )
  out=$(LUME_PREF_DIR=$M LUME_SCRIPT=tests/scripts/nsx_my.txt ./build/lume 2>&1)
  expect "DBX-101: MySQL says Databases; a small catalog is shown whole, without Others" 'menu "$out" 1 | grep -q "\[Search Databases…\].*\[Show All Databases\]" && st "$out" 1 | grep -q "list=\[.*shop:c" && st "$out" 1 | grep -q "others=\"-\""'
  expect "DBX-102: SQLite keeps its own tree (no Schemas / Others model)" 'printf "%s\n" "$out" | grep -q "^nsx: no model for Demo (in-memory)"'
  unset MYSQL_PW
else echo "skip  MySQL scenarios: lume-mysql not running"; fi

# ---- SQL Server: sys.schemas of the current database ----
MSENV="${LUME_MSSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mssql/.env}"
if docker ps --format '{{.Names}}' | grep -qx lume-mssql && [ -f "$MSENV" ]; then
  S=build/nsx-mssql; rm -rf $S; mkdir -p $S
  MS_PW=$(grep '^MSSQL_PASSWORD=' "$MSENV" | cut -d= -f2- | tr -d '"')
  printf '[connection]\nid=ms1\nname=ms\ndriver=sqlserver\nhost=127.0.0.1\nport=14330\ndatabase=master\nuser=sa\n' > $S/connections.txt
  ( umask 077; printf 'ms1\tpassword\t%s\n' "$MS_PW" > $S/secrets.txt )
  out=$(LUME_PREF_DIR=$S LUME_SCRIPT=tests/scripts/nsx_ms.txt ./build/lume 2>&1)
  expect "every script target found (SQL Server)" '[ "$(missing "$out")" = 0 ]'
  expect "DBX-100: SQL Server: dbo current in master; small catalog shown whole (exact count)" 'st "$out" 1 | grep -q "dbo:c" && st "$out" 1 | grep -q "scope=known" && st "$out" 1 | grep -q "total=[0-9]"'
  expect "DBX-082 / 100: Show system schemas adds sys / INFORMATION_SCHEMA / fixed roles (count grows)" '[ "$(st "$out" 2 | sed "s/.*total=\([0-9]*\).*/\1/")" -gt "$(st "$out" 1 | sed "s/.*total=\([0-9]*\).*/\1/")" ]'
  expect "DBX-100.3: server-side search over sys.schemas (db_* roles found)" 'st "$out" 3 | grep -q "search=done hits=[1-9].*\[db_"'
  expect "DBX-078: dbo Properties show the container (master)" 'props "$out" 1 | grep -q "Container / database: master"'
  expect "no SQL Server password in logs or preferences" '! grep -rqF "$MS_PW" $S/logs $S/schemas.txt 2>/dev/null'
  unset MS_PW
else echo "skip  SQL Server scenarios: lume-mssql not running"; fi

# ---- Oracle: ALL_USERS ----
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
if [ -z "$LUME_SKIP_ORACLE" ] && docker ps --format '{{.Names}}' | grep -qx dbv-oracle && [ -f "$OENV" ] && [ -e "$HOME/oracle/instantclient/libclntsh.dylib" ]; then
  O=build/nsx-ora; rm -rf $O; mkdir -p $O
  OPW=$(grep '^ORACLE_PASSWORD=' "$OENV" | cut -d= -f2- | tr -d '"')
  printf '[connection]\nid=ora1\nname=ora DEV\ndriver=oracle\nhost=localhost\nport=15210\nservice=FREEPDB1\nuser=system\nenv=DEV\n' > $O/connections.txt
  ( umask 077; printf 'ora1\tpassword\t%s\n' "$OPW" > $O/secrets.txt )
  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/nsx_ora.txt ./build/lume 2>&1)
  expect "every script target found (Oracle)" '[ "$(missing "$out")" = 0 ]'
  expect "DBX-083 / 093: Oracle connects with the scope read only: SYSTEM current, Others without a count" 'st "$out" 1 | grep -q "featured=1 others=\"Others\" .*loaded=0 pages=0 .*queries=1 " && st "$out" 1 | grep -q "SYSTEM:c"'
  expect "DBX-094: Others = ALL_USERS pages of 10 (keyset), with system accounts shown" 'st "$out" 2 | grep -q "loaded=10 pages=1 has_more=1" && st "$out" 3 | grep -q "loaded=20 pages=2 "'
  expect "DBX-085: search SYS on the server, system accounts included and marked" 'st "$out" 4 | grep -q "search=done hits=[1-9][0-9]* \[SYS"'
  expect "DBX-082 / 047: ORACLE_MAINTAINED accounts hidden: the 7 others fit, shown directly, no Others" 'st "$out" 5 | grep -q "featured=8 others=\"-\"" && st "$out" 5 | grep -q "SYN_FAT:s"'
  expect "DBX-091 / 107: Verify Visible Objects on SYSTEM (ALL_OBJECTS, one owner)" 'props "$out" 1 | grep -q "Has visible objects"'
  expect "DBX-095: the container (FREEPDB1) is part of the scope" 'props "$out" 2 | grep -q "in FREEPDB1"'
  expect "no Oracle password in logs or preferences" '! grep -rqF "$OPW" $O/logs $O/schemas.txt 2>/dev/null'
  # a schema whose table listing is slow (a real 60 s statement): it waits past the 5 s budget of
  # other catalog reads, shows the elapsed time and Cancel; Cancel stops it; Retry starts it again
  rm -rf $O/logs $O/schemas.txt
  out=$(LUME_TEST_SLOW_TABLES_S=60 LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/nsx_ora_slow.txt ./build/lume 2>&1)
  syn() { printf '%s\n' "$out" | grep '^explorer rows' | sed -n "$1p" | grep -oE 'SYN_FAT \|\.\.\.[^|]*\|\.\.\.[^|]*'; }
  expect "every script target found (Oracle, slow listing)" '[ "$(missing "$out")" = 0 ]'
  expect "a slow table listing keeps running past 5 s: \"Loading… N s\" and Cancel under the schema ($(syn 1))" 'syn 1 | grep -qE "Loading… [4-9] s \|\.\.\.Cancel"'
  expect "Run in the editor meanwhile says the Explorer holds the session and where to cancel" 'grep -q "status: The Explorer is still listing tables on this connection" $O/logs/lume.log'
  expect "Cancel stops the running statement (well before its 60 s): \"Listing cancelled\" and Retry, never an empty schema" 'syn 2 | grep -q "Listing cancelled |...Retry"'
  expect "Retry lists the schema again" 'syn 3 | grep -q "Loading… |...Cancel"'
  unset OPW
else echo "skip  Oracle scenarios: dbv-oracle / Instant Client not available"; fi

echo "NSX UI: $([ $fails = 0 ] && echo passed || echo "FAILED ($fails failures)")"
[ $fails = 0 ]
