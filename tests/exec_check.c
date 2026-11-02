/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks of the splitter and the asynchronous executor on SQLite (OUT-007, 015,
 * 018, 012): statement boundaries per dialect, a cancel that reaches the running statement,
 * stop / continue on error, error codes and offsets, results handed over intact. */
#include "exec.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

static int failures = 0;
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%.160s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}
static double now_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static void piece(const char *t, SqlStmt s, char *out, size_t n) { snprintf(out, n, "%.*s", s.end - s.start, t + s.start); }
static void wait_done(ExecJob *j, int max_ms) { double t0 = now_ms(); while (exec_running(j) && now_ms() - t0 < max_ms) usleep(2000); }

int main(void) {
    SqlStmt p[32]; char b[256];
    const char *g = "select 1; select ';' -- ; not a boundary\n; /* ; neither */ select 3 ;; -- trailing comment only";
    int n = sql_split(g, -1, SQL_DIALECT_GENERIC, p, 32);
    piece(g, p[1], b, sizeof b);
    check(n == 3 && strcmp(b, "select ';' -- ; not a boundary") == 0, "generic: quotes and comments do not split; empty pieces dropped", b);
    const char *pg = "create function f() returns int as $body$ begin return 1; end; $body$ language plpgsql; select E'it\\'s;', 'a''b'; select $1;";
    n = sql_split(pg, -1, SQL_DIALECT_PG, p, 32);
    piece(pg, p[1], b, sizeof b);
    check(n == 3 && strcmp(b, "select E'it\\'s;', 'a''b'") == 0, "pg: dollar quotes, E'' escapes and $1 parameters", b);
    const char *ora = "begin\n  null;\nend;\n/\nselect q'[a;b]' from dual;\ncreate or replace procedure p is begin null; end;\n/\nselect 2 from dual";
    n = sql_split(ora, -1, SQL_DIALECT_ORACLE, p, 32);
    piece(ora, p[0], b, sizeof b);
    bool ok0 = strcmp(b, "begin\n  null;\nend;") == 0;
    piece(ora, p[2], b, sizeof b);
    check(n == 4 && ok0 && strncmp(b, "create or replace procedure", 27) == 0 && strstr(b, "end;") != NULL, "oracle: PL/SQL units end at a lone slash and keep their semicolons; q'[]' quoting", b);
    int line, col; sql_line_col(ora, -1, 11, &line, &col);
    check(line == 2 && col == 6, "line / column from a byte offset", NULL);

    const char *path = "/tmp/lume_exec_check.db";
    remove(path);
    { sqlite3 *h = NULL; sqlite3_open(path, &h); sqlite3_close(h); }
    char err[256];
    Db *db = db_open(&db_sqlite_driver, path, err, sizeof err);
    if (!db) { printf("open failed: %s\n", err); return 2; }
    /* script: stop on error */
    ExecJob *j = exec_start(db, "create table t (id integer primary key, v text);\ninsert into t (v) values ('a'), ('b');\nselect * from nope;\nselect count(*) from t;", -1, 100, true);
    check(j && j->n == 4 && db->busy == 1, "job started: 4 statements, connection marked busy", NULL);
    wait_done(j, 5000);
    check(!exec_running(j) && db->busy == 0, "job finished, connection released", NULL);
    int st[4]; for (int i = 0; i < 4; i++) st[i] = j->stmts[i].status;
    check(st[0] == XS_OK && st[1] == XS_OK && st[2] == XS_FAILED && st[3] == XS_SKIPPED && exec_overall(j) == 4, "stop on error: ok, ok, failed, skipped; overall failed", NULL);
    ResultSet rs;
    int d = exec_next_done(j); int d2 = exec_next_done(j); int d3 = exec_next_done(j);
    check(d == 0 && d2 == 1 && d3 == 2, "finished statements are consumed in order", NULL);
    check(exec_take_result(j, 1, &rs) && rs.ok && rs.affected == 2, "insert result: 2 rows affected", NULL);
    rs_free(&rs);
    check(exec_take_result(j, 2, &rs) && !rs.ok && rs.err_code == 1 && strstr(rs.error, "nope"), "failed statement carries the extended code and the message", rs.error);
    rs_free(&rs);
    check(!exec_take_result(j, 3, &rs), "a skipped statement has no result", NULL);
    exec_free(j);
    /* continue on error; a syntax error reports where it is (offset inside the statement) */
    j = exec_start(db, "select * from nope; select count(*) as c from t; select 1 frm t;", -1, 100, false);
    wait_done(j, 5000);
    check(j && j->stmts[0].status == XS_FAILED && j->stmts[1].status == XS_OK && exec_take_result(j, 1, &rs) && rs.has_rows && strcmp(rs_cell(&rs, 0, 0), "2") == 0, "continue on error: the second statement still runs and returns rows", NULL);
    rs_free(&rs);
#if SQLITE_VERSION_NUMBER >= 3038000   /* sqlite3_error_offset: SQLite 3.38+ */
    check(exec_take_result(j, 2, &rs) && !rs.ok && rs.err_offset == 13, "syntax error offset points at the bad token inside the statement", rs.error);
    { int l, c; sql_line_col(j->text + j->stmts[2].start, j->stmts[2].end - j->stmts[2].start, rs.err_offset, &l, &c); char dd[48]; snprintf(dd, sizeof dd, "line %d col %d", l, c); check(l == 1 && c == 14, "offset to line / column", dd); }
#else
    check(exec_take_result(j, 2, &rs) && !rs.ok, "a syntax error fails the statement", rs.error);
    printf("skip  error offset: SQLite %s has no sqlite3_error_offset (3.38+)\n", sqlite3_libversion());
#endif
    rs_free(&rs); exec_free(j);
    /* busy connection refuses a second job */
    j = exec_start(db, "with recursive c(x) as (select 1 union all select x + 1 from c limit 200000000) select count(*) from c;", -1, 100, true);
    check(j != NULL && exec_start(db, "select 1", -1, 100, true) == NULL, "a second job on a busy connection is refused", NULL);
    usleep(60000);
    double t0 = now_ms();
    check(exec_running(j) && !exec_cancelling(j), "long statement still running after 60 ms", NULL);
    exec_cancel(j);
    check(exec_cancelling(j) || !exec_running(j), "state is cancelling until the worker returns", NULL);
    wait_done(j, 5000);
    char dd[96]; snprintf(dd, sizeof dd, "%.0f ms after cancel, status %d, elapsed %.0f ms", now_ms() - t0, j->stmts[0].status, exec_elapsed_ms(j));
    check(!exec_running(j) && j->stmts[0].status == XS_CANCELLED && exec_overall(j) == 5 && now_ms() - t0 < 1500, "sqlite3_interrupt stops the statement; reported as cancelled, not failed", dd);
    check(exec_take_result(j, 0, &rs) && rs.cancelled && !rs.ok, "the cancelled statement's result says so", rs.error);
    rs_free(&rs); exec_free(j);
    /* cancel before the second statement runs */
    j = exec_start(db, "with recursive c(x) as (select 1 union all select x + 1 from c limit 200000000) select count(*) from c; select 1;", -1, 100, false);
    usleep(30000); exec_cancel(j); wait_done(j, 5000);
    check(j->stmts[0].status == XS_CANCELLED && j->stmts[1].status == XS_CANCELLED, "statements after a cancel never run", NULL);
    exec_free(j);
    db_close(db);
    remove(path);
    printf("%s (%d failures)\n", failures ? "EXEC: FAIL" : "EXEC: OK", failures);
    return failures ? 1 : 0;
}
