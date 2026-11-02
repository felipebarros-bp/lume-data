/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Read-only query policy for Lume Intelligence and the Report Studio (LI-029 / 080 / 081 / 097).
 *
 * Two layers, both required (a "starts with SELECT" check is not validation, LI-029.2):
 *
 *   1. sqlguard_check: lexical policy over the dialect's tokens (strings, quoted identifiers,
 *      comments and bodies are never read as SQL). Exactly one statement, SELECT / WITH only, no
 *      write or DDL keyword anywhere (data-modifying CTEs included), no SELECT ... INTO, no row
 *      locks, no MySQL executable comments, no function known to have side effects (sleep,
 *      file / network access, sequences, advisory locks, dynamic SQL, OS commands).
 *   2. sqlguard_wrap: the statement runs inside the engine's read-only transaction where one
 *      exists (PostgreSQL BEGIN READ ONLY + statement_timeout, MySQL / MariaDB START TRANSACTION
 *      READ ONLY, Oracle SET TRANSACTION READ ONLY, SQLite PRAGMA query_only), always rolled back.
 *      SQL Server has no read-only transaction: the note says so and recommends a read-only login.
 *
 * Neither layer replaces a read-only credential (LI-078 / 083): they stop what Lume itself would
 * send. Pure C, no SDL, no connection: usable from workers and tests.
 */
#include <stdbool.h>

#define SQLGUARD_MAX_LEN 65536

/* true when the statement may run; why (a short, non-sensitive reason) otherwise. dialect is a
 * sqllex DIALECT_* id. */
bool sqlguard_check(const char *sql, int dialect, char *why, int whyn);

/* The statements to run for one guarded query: out[] receives malloc'd strings (free with
 * sqlguard_free); returns the count, *result the index holding the query's rows, *finally_last
 * whether the last statement must run even after a failure (the rollback). timeout_s <= 0 = none
 * where the engine has a server-side timeout. note: the protections in place, for the plan / log. */
int sqlguard_wrap(int dialect, const char *sql, int timeout_s, char **out, int max, int *result, bool *finally_last, char *note, int nn);
void sqlguard_free(char **out, int n);
