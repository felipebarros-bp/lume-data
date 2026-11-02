#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Visual Table Designer against real servers: PostgreSQL (dbv-postgres), MySQL / MariaDB / SQL Server
# (lume-* test containers), Oracle (dbv-oracle via the Instant Client). Passwords come from the test
# .env files and are never printed; servers that are not running are skipped (and said so).
cd "$(dirname "$0")/.."
mkdir -p build
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if [ -z "$LUME_PG_CONN" ] && [ -f "$ENV_FILE" ] && docker exec dbv-postgres true 2>/dev/null; then
  PG_PASSWORD=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  LUME_PG_CONN="host=localhost port=15432 dbname=syn user=postgres password=$PG_PASSWORD"; export LUME_PG_CONN
fi
MYENV="${LUME_MYSQL_ENV:-/private/tmp/claude-501/-Users-isabella-workspace-tessarotto/fca9806a-3181-4d53-8793-f86a7d31d145/scratchpad/mysql.env}"
if [ -f "$MYENV" ]; then
  MYSQL_PW=$(grep '^MYSQL_PW=' "$MYENV" | cut -d= -f2-)
  docker ps --format '{{.Names}}' | grep -qx lume-mysql && { LUME_MYSQL_LOC=$(printf '127.0.0.1\n13306\nroot\n%s\nshop' "$MYSQL_PW"); export LUME_MYSQL_LOC; }
  docker ps --format '{{.Names}}' | grep -qx lume-mariadb && { LUME_MARIADB_LOC=$(printf '127.0.0.1\n13307\nroot\n%s\nshop' "$MYSQL_PW"); export LUME_MARIADB_LOC; }
fi
MSENV="${LUME_MSSQL_ENV:-$HOME/workspace/tessarotto/test-servers/mssql/.env}"
if [ -f "$MSENV" ] && docker ps --format '{{.Names}}' | grep -qx lume-mssql; then
  MS_PW=$(grep '^MSSQL_PASSWORD=' "$MSENV" | cut -d= -f2- | tr -d '"')
  LUME_MSSQL_LOC=$(printf '127.0.0.1\n14330\nsa\n%s\nmaster' "$MS_PW"); export LUME_MSSQL_LOC
fi
OENV="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
if [ -z "$LUME_SKIP_ORACLE" ] && [ -f "$OENV" ] && docker ps --format '{{.Names}}' | grep -qx dbv-oracle && [ -e "$HOME/oracle/instantclient/libclntsh.dylib" ]; then
  OPW=$(grep '^ORACLE_PASSWORD=' "$OENV" | cut -d= -f2- | tr -d '"')
  LUME_ORA_LOC=$(printf 'system\n%s\n//localhost:15210/FREEPDB1' "$OPW"); export LUME_ORA_LOC
fi
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/tbl_db_check \
  tests/tbl_db_check.c src/tbl.c src/tbl_plan.c src/tbl_snap.c src/rs.c src/db.c src/db_pg.c src/db_mysql.c src/db_mssql.c src/db_oracle.c tests/rs_stub.c -lsqlite3 -ldl -lm -lpthread || exit 1
build/tbl_db_check
