#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Object source editor end to end: SQLite always, PostgreSQL (dbv-postgres) when reachable,
# Oracle inside the dbv-oracle container. Passwords come from the test servers' .env files and
# are never printed.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
SRC="src/objmodel.c src/objplan.c src/obj_sqlite.c src/obj_pg.c src/obj_oracle.c src/obj_mysql.c src/objdiag.c src/objlang.c src/objsrc.c src/sqllex.c src/sqlfmt.c src/textdiff.c src/exec.c src/rs.c"
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if [ -z "$LUME_PG_CONN" ] && [ -f "$ENV_FILE" ]; then
  PG_PASSWORD=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  LUME_PG_CONN="host=localhost port=15432 dbname=syn user=postgres password=$PG_PASSWORD"
fi
export LUME_PG_CONN
# MySQL (BL-013): the lume-mysql test container, password from its private env file
MYENV="${LUME_MYSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mysql/.env}"
if [ -z "$LUME_MYSQL_LOC" ] && [ -f "$MYENV" ] && docker ps --format '{{.Names}}' | grep -qx lume-mysql; then
  MYSQL_PW=$(grep '^MYSQL_PW=' "$MYENV" | cut -d= -f2-)
  LUME_MYSQL_LOC=$(printf '127.0.0.1\n13306\nroot\n%s\nshop' "$MYSQL_PW"); export LUME_MYSQL_LOC
fi
if [ -z "$LUME_MARIADB_LOC" ] && [ -f "$MYENV" ] && docker ps --format '{{.Names}}' | grep -qx lume-mariadb; then
  MYSQL_PW=$(grep '^MYSQL_PW=' "$MYENV" | cut -d= -f2-)
  LUME_MARIADB_LOC=$(printf '127.0.0.1\n13307\nroot\n%s\nshop' "$MYSQL_PW"); export LUME_MARIADB_LOC
fi
# SQL Server (BL-014): the lume-mssql container, sa password from Isa's synthetic/mssql/.env (never printed)
MSENV="${LUME_MSSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mssql/.env}"
if [ -z "$LUME_MSSQL_LOC" ] && [ -f "$MSENV" ] && docker ps --format '{{.Names}}' | grep -qx lume-mssql; then
  MS_PW=$(grep '^MSSQL_PASSWORD=' "$MSENV" | cut -d= -f2- | tr -d '"')
  LUME_MSSQL_LOC=$(printf '127.0.0.1\n14330\nsa\n%s\nmaster' "$MS_PW"); export LUME_MSSQL_LOC
fi
# the driver mirrors MYSQL_FIELD: check it against the installed header when there is one
if [ -f /opt/homebrew/opt/mysql-client/include/mysql/mysql.h ]; then
  cc -std=gnu11 -I/opt/homebrew/opt/mysql-client/include/mysql -Isrc -o build/mysql_layout tests/mysql_layout.c && build/mysql_layout
fi
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/objects_db_check tests/objects_db_check.c $SRC src/db.c src/db_pg.c src/db_mysql.c src/db_mssql.c -lsqlite3 -lpthread -lm
build/objects_db_check
# Oracle: built for Linux in a gcc:8 container, run inside the database container
: "${ORA_CONTAINER:=dbv-oracle}"; : "${ORA_USER:=system}"; : "${ORA_SERVICE:=FREEPDB1}"
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
[ -n "$ORA_PASSWORD" ] || ORA_PASSWORD=$(grep '^ORACLE_PASSWORD=' "$OENV" 2>/dev/null | cut -d= -f2- | tr -d '"')
if [ -z "$ORA_PASSWORD" ] || ! docker ps --format '{{.Names}}' 2>/dev/null | grep -qx "$ORA_CONTAINER"; then echo "skip  Oracle: container or password not available"; exit 0; fi
mkdir -p build/objects_ora
docker run --rm -v "$PWD:/src:ro" -v "$PWD/build/objects_ora:/out" gcc:8 sh -c \
  "cd /src && gcc -std=gnu11 -O1 -Wall -DWITH_ORACLE -DNO_SQLITE -Isrc -Itests/shim -o /out/objects_db_check tests/objects_db_check.c $SRC src/db_oracle.c src/db_pg.c src/db_mysql.c src/db_mssql.c -ldl -lm -lpthread"
docker cp build/objects_ora/objects_db_check "$ORA_CONTAINER:/tmp/objects_db_check"
docker exec -e ORA_USER="$ORA_USER" -e ORA_PASSWORD="$ORA_PASSWORD" -e ORA_DSN="//localhost:1521/$ORA_SERVICE" \
  "$ORA_CONTAINER" sh -c 'export LD_LIBRARY_PATH=$ORACLE_HOME/lib; export LUME_ORACLE_LIB=$ORACLE_HOME/lib/libclntsh.so; /tmp/objects_db_check'
