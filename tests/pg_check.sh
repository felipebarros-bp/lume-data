#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Headless PostgreSQL grid checks against the dbv-postgres test container (localhost:15432, db syn).
# The password is read from the test servers' .env and never printed.
set -e
cd "$(dirname "$0")/.."
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
if [ -z "$LUME_PG_CONN" ]; then
  PG_PASSWORD=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  LUME_PG_CONN="host=localhost port=15432 dbname=syn user=postgres password=$PG_PASSWORD"
fi
export LUME_PG_CONN
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/pg_check tests/pg_check.c src/db_pg.c src/exec.c src/edit.c src/grid.c src/rs.c src/json.c src/geo.c tests/rs_stub.c -lm -lpthread
build/pg_check
