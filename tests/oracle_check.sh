#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds tests/oracle_check.c for Linux in a throwaway gcc:8 container (glibc 2.28, the same
# as Oracle Linux 8) and runs it inside the Oracle database container, which already ships
# libclntsh.so. The password comes from ORA_PASSWORD or the test servers' .env; never printed.
#   ORA_CONTAINER=dbv-oracle ORA_USER=system ORA_PASSWORD=... ORA_SERVICE=FREEPDB1 tests/oracle_check.sh
set -e
cd "$(dirname "$0")/.."
: "${ORA_CONTAINER:=dbv-oracle}"
: "${ORA_USER:=system}"
: "${ORA_SERVICE:=FREEPDB1}"
ENV_FILE="${LUME_ORA_ENV:-$HOME/workspace/tessarotto/test-servers/.env}"
[ -n "$ORA_PASSWORD" ] || ORA_PASSWORD=$(grep '^ORACLE_PASSWORD=' "$ENV_FILE" 2>/dev/null | cut -d= -f2- | tr -d '"')
[ -n "$ORA_PASSWORD" ] || { echo "ORA_PASSWORD is required" >&2; exit 2; }
mkdir -p build/oracle_check
docker run --rm -v "$PWD:/src:ro" -v "$PWD/build/oracle_check:/out" gcc:8 sh -c \
  'gcc -std=gnu11 -O1 -Wall -I/src/src -I/src/tests/shim -o /out/oracle_check /src/tests/oracle_check.c /src/src/db_oracle.c /src/src/exec.c /src/src/edit.c /src/src/grid.c /src/src/rs.c /src/src/json.c /src/src/geo.c -ldl -lm -lpthread'
docker cp build/oracle_check/oracle_check "$ORA_CONTAINER:/tmp/oracle_check"
docker exec -e ORA_USER="$ORA_USER" -e ORA_PASSWORD="$ORA_PASSWORD" -e ORA_DSN="//localhost:1521/$ORA_SERVICE" \
  "$ORA_CONTAINER" sh -c 'export LD_LIBRARY_PATH=$ORACLE_HOME/lib; export LUME_ORACLE_LIB=$ORACLE_HOME/lib/libclntsh.so; /tmp/oracle_check'
