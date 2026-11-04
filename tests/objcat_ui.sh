#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Completion with every object the user may see, read in a second session. Oracle: schemas,
# SCHEMA. lists, other schemas' objects, a PUBLIC synonym, other schemas' columns arriving in the
# background, the INSERT column list, the disk cache on the next connect, the setting off, and the
# Explorer opening a package to its subprograms. PostgreSQL, MySQL and MariaDB: the same catalog
# (schemas, SCHEMA., columns in the background) and the Explorer's Sequences group.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
nth() { printf '%s\n' "$1" | grep "^$2" | sed -n "$3p"; }
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
if [ -n "$LUME_SKIP_ORACLE" ] || ! docker ps --format '{{.Names}}' | grep -qx dbv-oracle || [ ! -f "$OENV" ] || [ ! -e "$HOME/oracle/instantclient/libclntsh.dylib" ]; then
  echo "skip  Oracle catalog completion: dbv-oracle / Instant Client not available"
else
  OPW=$(grep '^ORACLE_PASSWORD=' "$OENV" | cut -d= -f2- | tr -d '"')
  ora() { docker exec -i dbv-oracle bash -c 'sqlplus -s system/"$1"@localhost:1521/FREEPDB1' -- "$OPW" >/dev/null 2>&1; }
  printf 'CREATE OR REPLACE PUBLIC SYNONYM LUME_AC_VISITS FOR SYN_FAT.T00001;\nCREATE OR REPLACE PACKAGE LUME_PKG AS\n  PROCEDURE P_LOG(P_MSG VARCHAR2);\n  FUNCTION F_TOTAL(P_A NUMBER, P_B NUMBER) RETURN NUMBER;\nEND;\n/\nexit\n' | ora
  O=build/objcat-ora; rm -rf $O; mkdir -p $O
  printf '[connection]\nid=ora1\nname=ora DEV\ndriver=oracle\nhost=localhost\nport=15210\nservice=FREEPDB1\nuser=system\nenv=DEV\n' > $O/connections.txt
  ( umask 077; printf 'ora1\tpassword\t%s\n' "$OPW" > $O/secrets.txt )

  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/objcat_ora.txt ./build/lume 2>&1)
  printf '%s\n' "$out" > build/objcat_ui.out
  expect "the object list is read in a second session (thousands of objects, several schemas)" 'nth "$out" objcatstate 1 | grep -qE "have=1 n=[0-9]{4,} schemas=[0-9]+ .*session=1"'
  expect "SYN_F proposes the schema SYN_FAT, with its object count" 'nth "$out" completestate 1 | grep -qE "\[SYN_FAT\|schema · [0-9,]+ objects ›\]"'
  expect "SYN_FAT. lists its tables" 'nth "$out" completestate 2 | grep -qE "\[T[0-9]+\|table\]"'
  expect "SYN_FAT.T0000 filters them" 'nth "$out" completestate 3 | grep -q "\[T00001|table\]"'
  expect "a PUBLIC synonym is proposed unqualified, with its target" 'nth "$out" completestate 4 | grep -q "\[LUME_AC_VISITS|synonym → SYN_FAT.T00001\]"'
  expect "SCHEMA. after FROM: no packages or procedures" '! nth "$out" completestate 5 | grep -qE "\|(package|procedure)\]"'
  expect "a.: Loading row while another schema's columns come" 'nth "$out" completestate 6 | grep -q "\[Loading columns of SYN_FAT.T00001…|\]"'
  expect "  ... then that table's columns, the detail being the type" 'nth "$out" completestate 7 | grep -qE "\[C001\|[A-Z]+"'
  expect "no alias: the table name as qualifier" 'nth "$out" completestate 8 | grep -q "\[C001|"'
  expect "a synonym's alias: the target's columns" 'nth "$out" completestate 9 | grep -q "\[C001|"'
  expect "INSERT INTO: every column first, then VALUES (" 'nth "$out" completestate 10 | grep -qE "\[all [0-9]+ columns, then VALUES \(\|"'
  expect "the catalog is cached on disk (names only)" 'test -s $O/objcat-ora1.tsv && grep -q "^TABLE	SYN_FAT	T00001	" $O/objcat-ora1.tsv && ! grep -qi "password" $O/objcat-ora1.tsv'
  expect "the window was never blocked: columns read in the catalog session" 'grep -q "object catalog: reading ora DEV in a second session" $O/logs/lume.log'

  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/objcat_ora_again.txt ./build/lume 2>&1)
  printf '%s\n' "$out" > build/objcat_ui_again.out
  expect "next connect: the cached catalog at once" 'grep -q "from the cache" $O/logs/lume.log && nth "$out" completestate 1 | grep -q "\[SYN_FAT|schema"'
  expect "  ... refreshed in the background" 'nth "$out" objcatstate 2 | grep -qE "have=1 n=[0-9]{4,}"'

  printf 'catalog_session=0\n' > $O/settings.txt
  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/objcat_off.txt ./build/lume 2>&1)
  expect "Settings off: no second session, no catalog proposals" 'nth "$out" objcatstate 1 | grep -q "have=0" && ! nth "$out" completestate 1 | grep -q "SYN_FAT|schema"'

  # the Explorer: a package opens to its subprograms (read by the catalog session)
  rm -f $O/settings.txt
  out=$(LUME_PREF_DIR=$O LUME_SCRIPT=tests/scripts/objcat_ora_pkg.txt ./build/lume 2>&1)
  printf '%s\n' "$out" > build/objcat_ui_pkg.out
  expect "Explorer: the Objects group lists Packages with LUME_PKG" 'nth "$out" "explorer rows" 1 | grep -q "|\.*Packages" && nth "$out" "explorer rows" 1 | grep -q "|\.*LUME_PKG"'
  expect "  ... opening the package shows Loading members first, or the members already (a local server answers within the frame)" 'nth "$out" "explorer rows" 2 | grep -qE "\|\.*Loading members|\|\.*P_LOG\(P_MSG\)"'
  expect "  ... then its procedure and function, with parameter names, in declaration order" 'nth "$out" "explorer rows" 3 | grep -q "|\.*LUME_PKG |\.*P_LOG(P_MSG) |\.*F_TOTAL(P_A, P_B)"'
  expect "  ... read by the catalog session" 'nth "$out" objcatstate 1 | grep -q "members_loaded=1"'
  printf 'DROP PUBLIC SYNONYM LUME_AC_VISITS;\nDROP PACKAGE LUME_PKG;\nexit\n' | ora
fi

# ---- PostgreSQL ----
PGENV="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
PGPW=$(grep '^PG_PASSWORD=' "$PGENV" 2>/dev/null | cut -d= -f2- | tr -d '"')
if docker ps --format '{{.Names}}' | grep -qx dbv-postgres && [ -n "$PGPW" ]; then
  P=build/objcat-pg; rm -rf $P; mkdir -p $P
  printf '[connection]\nid=pg1\nname=pg DEV\ndriver=postgres\nhost=localhost\nport=15432\ndatabase=syn\nuser=postgres\nenv=DEV\n' > $P/connections.txt
  ( umask 077; printf 'pg1\tpassword\t%s\n' "$PGPW" > $P/secrets.txt )
  docker exec -e PGPASSWORD="$PGPW" dbv-postgres psql -U postgres -d syn -qc "CREATE SEQUENCE IF NOT EXISTS public.lume_seq" >/dev/null 2>&1
  out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/objcat_pg.txt ./build/lume 2>&1)
  printf '%s\n' "$out" > build/objcat_ui_pg.out
  expect "PostgreSQL: the object list is read in a second session (every schema)" 'nth "$out" objcatstate 1 | grep -qE "have=1 n=[0-9]{4,} schemas=[0-9]+ home=\"public\" session=1"'
  expect "PostgreSQL: syn_m proposes the schema syn_mid, with its object count" 'nth "$out" completestate 1 | grep -qE "\[syn_mid\|schema · [0-9,]+ objects ›\]"'
  expect "PostgreSQL: syn_mid. lists its tables" 'nth "$out" completestate 2 | grep -q "\[t00001|table\]"'
  expect "PostgreSQL: a.: the Loading row, or the columns already (a local server answers within the frame)" 'nth "$out" completestate 3 | grep -qE "\[Loading columns of syn_mid.t00001…\||\[c002\|numeric"'
  expect "PostgreSQL:   ... then that table's columns" 'nth "$out" completestate 4 | grep -q "\[c002|numeric"'
  expect "PostgreSQL: the Explorer's Objects group of public lists the catalog's Sequences" 'nth "$out" "explorer rows" 1 | grep -q "|\.*Sequences |\.*lume_seq"'
  docker exec -e PGPASSWORD="$PGPW" dbv-postgres psql -U postgres -d syn -qc "DROP SEQUENCE IF EXISTS public.lume_seq" >/dev/null 2>&1
else echo "skip  PostgreSQL catalog: dbv-postgres not available"; fi

# ---- MySQL and MariaDB ----
MYENV="${LUME_MYSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mysql/.env}"
MYPW=$(grep '^MYSQL_PW=' "$MYENV" 2>/dev/null | cut -d= -f2- | tr -d '"')
for pair in "mysql lume-mysql 13306" "mariadb lume-mariadb 13307"; do
  set -- $pair; drv=$1; ctr=$2; port=$3
  if ! docker ps --format '{{.Names}}' | grep -qx $ctr || [ -z "$MYPW" ]; then echo "skip  $drv catalog: $ctr not available"; continue; fi
  M=build/objcat-$drv; rm -rf $M; mkdir -p $M
  printf '[connection]\nid=my1\nname=my DEV\ndriver=%s\nhost=127.0.0.1\nport=%s\ndatabase=shop\nuser=root\nenv=DEV\n' "$drv" "$port" > $M/connections.txt
  ( umask 077; printf 'my1\tpassword\t%s\n' "$MYPW" > $M/secrets.txt )
  out=$(LUME_PREF_DIR=$M LUME_SCRIPT=tests/scripts/objcat_my.txt ./build/lume 2>&1)
  printf '%s\n' "$out" > build/objcat_ui_$drv.out
  expect "$drv: the object list is read in a second session, the database as the home schema" 'nth "$out" objcatstate 1 | grep -qE "have=1 n=[1-9][0-9]* schemas=[1-9][0-9]* home=\"shop\" session=1"'
  expect "$drv: sho proposes the database shop as a schema" 'nth "$out" completestate 1 | grep -qE "\[shop\|schema · [0-9,]+ objects ›\]"'
  expect "$drv: shop. lists its tables and views" 'nth "$out" completestate 2 | grep -q "\[pl_customers|table\]" && nth "$out" completestate 2 | grep -q "\[v_sales|view\]"'
  expect "$drv: a. proposes the columns" 'nth "$out" completestate 3 | grep -qE "open=1 n=[1-9]" && ! nth "$out" completestate 3 | grep -q "Loading"'
done

if [ $fails = 0 ]; then echo "OBJCAT UI: all passed"; else echo "OBJCAT UI: FAILED ($fails failures)"; fi
exit $fails
