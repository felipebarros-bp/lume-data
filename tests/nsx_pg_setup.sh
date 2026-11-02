#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Creates (once) the database lume_scale in the PostgreSQL test container: 170,000 empty schemas
# usr_000000..usr_169999 plus abc / xpto / atendimento / clientes with tables, and a limited role
# without USAGE on most of them. Only this database is touched.
set -e
q() { docker exec -i dbv-postgres psql -U postgres -v ON_ERROR_STOP=1 -q "$@"; }
if [ "$(q -tAc "SELECT 1 FROM pg_database WHERE datname = 'lume_scale'")" != 1 ]; then
  q -c "CREATE DATABASE lume_scale"
fi
n=$(q -d lume_scale -tAc "SELECT count(*) FROM pg_namespace WHERE nspname LIKE 'usr\_%'")
if [ "$n" -lt 170000 ]; then
  q -d lume_scale <<'SQL'
DO $$
DECLARE i int;
BEGIN
  FOR i IN 0..169999 LOOP
    EXECUTE format('CREATE SCHEMA IF NOT EXISTS %I', 'usr_' || lpad(i::text, 6, '0'));
    IF i % 5000 = 4999 THEN COMMIT; END IF;
  END LOOP;
END $$;
SQL
fi
q -d lume_scale <<'SQL'
CREATE SCHEMA IF NOT EXISTS abc;
CREATE SCHEMA IF NOT EXISTS xpto;
CREATE SCHEMA IF NOT EXISTS atendimento;
CREATE SCHEMA IF NOT EXISTS clientes;
CREATE SCHEMA IF NOT EXISTS privado;
CREATE TABLE IF NOT EXISTS abc.pedidos (id int PRIMARY KEY, total numeric);
CREATE TABLE IF NOT EXISTS xpto.itens (id int PRIMARY KEY, nome text);
CREATE TABLE IF NOT EXISTS atendimento.chamados (id int PRIMARY KEY);
CREATE TABLE IF NOT EXISTS clientes.cadastro (id int PRIMARY KEY, nome text);
CREATE OR REPLACE VIEW clientes.vw_clientes AS SELECT id, nome FROM clientes.cadastro;
CREATE TABLE IF NOT EXISTS usr_165000.t_far (id int);
CREATE TABLE IF NOT EXISTS privado.segredo (id int);
SQL
echo "lume_scale ready: $(q -d lume_scale -tAc "SELECT count(*) FROM pg_namespace") namespaces"
