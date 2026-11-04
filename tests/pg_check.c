/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless PostgreSQL checks for the result grid: streaming export through a live cursor
 * (US-025) with flat memory, type metadata (US-017) and the server-side select builder
 * (US-012). Connection string from LUME_PG_CONN (libpq conninfo); never printed. */
#include "db.h"
#include "grid.h"
#include "edit.h"
#include "exec.h"
#include <time.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

extern const DbDriver db_pg_driver;

char *db_strdup(const char *s) { return s ? SDL_strdup(s) : NULL; }
/* db.c is not linked here: the cancel goes straight to the driver (no thread) */
void db_cancel(Db *db) { if (db && db->drv->cancel) db->drv->cancel(db); }
void db_tables_free(DbTable *tables, int count) {
    for (int i = 0; i < count; i++) {
        for (int c = 0; c < tables[i].ncols; c++) { free(tables[i].cols[c].name); free(tables[i].cols[c].type); }
        free(tables[i].cols); free(tables[i].name);
    }
    free(tables);
}
void db_fks_free(DbFk *fks, int count) {
    for (int i = 0; i < count; i++) { free(fks[i].name); free(fks[i].from_col); free(fks[i].to_table); free(fks[i].to_col); }
    free(fks);
}
void db_quote_ident_double(const char *ident, char *buf, size_t buflen) {
    size_t n = 0;
    if (n < buflen - 1) buf[n++] = '"';
    for (const char *p = ident; *p && n < buflen - 4; p++) {
        if (*p == '.') { buf[n++] = '"'; buf[n++] = '.'; buf[n++] = '"'; continue; }
        if (*p == '"') buf[n++] = '"';
        buf[n++] = *p;
    }
    if (n < buflen - 1) buf[n++] = '"';
    buf[n] = 0;
}

static int failures = 0;
static void check(bool ok, const char *what) { printf("%s  %s\n", ok ? "ok  " : "FAIL", what); if (!ok) failures++; }
static void check2(bool ok, const char *what, const char *detail) { printf("%s  %s%s%.200s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : ""); if (!ok) failures++; }

static long cancel_at;
static ExportProgress last;
static int prog_cancel(void *ud, const ExportProgress *p) { (void)ud; last = *p; return p->rows >= cancel_at ? EXPORT_CANCELLED : EXPORT_GO; }
typedef struct { Db *db2; char pid[16]; bool done; } KillCtx;
static int prog_kill(void *ud, const ExportProgress *p) {
    KillCtx *k = ud;
    if (!k->done && p->rows >= 4000) {
        char sql[128]; snprintf(sql, sizeof sql, "SELECT pg_terminate_backend(%s)", k->pid);
        ResultSet r; db_pg_driver.exec(k->db2, sql, -1, 0, &r); rs_free(&r);
        k->done = true;
    }
    return EXPORT_GO;
}

static bool run(Db *db, const char *sql) {
    ResultSet rs; db_pg_driver.exec(db, sql, -1, 0, &rs);
    bool ok = rs.ok; if (!ok) printf("      exec failed: %s\n      %s\n", rs.error, sql);
    rs_free(&rs); return ok;
}
static bool scalar_is(Db *db, const char *sql, const char *want) {
    ResultSet rs; db_pg_driver.exec(db, sql, -1, 0, &rs);
    const char *v = rs.ok && rs.nrows ? rs_cell(&rs, 0, 0) : "<error>";
    bool ok = want ? (v && strcmp(v, want) == 0) : v == NULL;
    if (!ok) printf("      got [%s] want [%s]\n", v ? v : "NULL", want ? want : "NULL");
    rs_free(&rs); return ok;
}
typedef struct { ResultSet rs; Grid g; EditSet e; DbTable t; } View;
static int vcol(View *v, const char *name) { for (int c = 0; c < v->rs.ncols; c++) if (strcmp(v->rs.cols[c], name) == 0) return c; return -1; }
static void view_open(View *v, Db *db, const char *table, const char *order) {
    char q[128], sql[512];
    db_pg_driver.quote_ident(table, q, sizeof q);
    db_pg_driver.select_sql(q, NULL, order, 100, sql, sizeof sql);
    db_pg_driver.exec(db, sql, -1, 0, &v->rs);
    grid_init(&v->g, &v->rs);
    edit_init(&v->e, &v->rs, &db_pg_driver, q, v->g.numeric);
    memset(&v->t, 0, sizeof v->t); v->t.name = (char *)table;
    char err[200];
    if (!db_pg_driver.list_columns(db, &v->t, err, sizeof err)) printf("      list_columns: %s\n", err);
    int pk[8], npk = 0;
    for (int c = 0; c < v->t.ncols; c++) {
        int src = vcol(v, v->t.cols[c].name);
        if (src < 0) continue;
        if (v->t.cols[c].pk && npk < 8) pk[npk++] = src;
        if (v->t.cols[c].generated) edit_set_readonly(&v->e, src, "generated column");
        if (v->t.cols[c].identity_always) edit_set_readonly(&v->e, src, "identity column (generated always)");
    }
    edit_set_key(&v->e, pk, npk);
}
static void view_close(View *v) {
    edit_free(&v->e); grid_free(&v->g); rs_free(&v->rs);
    for (int c = 0; c < v->t.ncols; c++) { free(v->t.cols[c].name); free(v->t.cols[c].type); }
    free(v->t.cols);
}
typedef struct { Db *db2; char pid[16]; int at; } CommitKill;
static void kill_before(void *ud, int i, const char *sql) {
    CommitKill *k = ud; (void)sql;
    if (i == k->at) { char s[128]; snprintf(s, sizeof s, "SELECT pg_terminate_backend(%s)", k->pid); run(k->db2, s); }
}

static double mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static void wait_job(ExecJob *j, int max_ms) { double t0 = mono_ms(); while (exec_running(j) && mono_ms() - t0 < max_ms) usleep(2000); }

static long count_lines(const char *path) {
    FILE *f = fopen(path, "r");
    if (!f) return -1;
    long n = 0; int c;
    while ((c = fgetc(f)) != EOF) if (c == '\n') n++;
    fclose(f);
    return n;
}

int main(void) {
    const char *conn = getenv("LUME_PG_CONN");
    if (!conn) { fprintf(stderr, "LUME_PG_CONN not set\n"); return 2; }
    Db db = { .drv = &db_pg_driver };
    char err[512];
    if (!db_pg_driver.open(&db, conn, err, sizeof err)) { fprintf(stderr, "open failed: %s\n", err); return 2; }

    ResultSet rs;
    db_pg_driver.exec(&db, "SELECT g AS n, 'v' || g AS s, now() AS ts, '{\"a\": [1, 2]}'::jsonb AS doc, g % 2 = 0 AS even FROM generate_series(1, 300000) g", -1, 500, &rs);
    check(rs.ok && rs.nrows == 500 && rs.cursor != NULL && !rs.complete, "first batch of 500 leaves the cursor open");
    check(rs.kinds[0] == CK_NUMBER && rs.kinds[2] == CK_DATE && rs.kinds[3] == CK_JSON && rs.kinds[4] == CK_BOOL, "kinds from PostgreSQL OIDs: number, timestamp, jsonb, bool");
    Grid g;
    grid_init(&g, &rs);
    grid_sort_click(&g, 0, false); grid_sort_click(&g, 0, false);   /* local desc: must not affect the stream */
    ExportOpts o = { .fmt = FMT_CSV, .headers = true, .sep = ',', .null_as = NULL_EMPTY };
    ExportReport rep;
    long n = grid_export_full(&g, "/tmp/lume_pg_full.csv", &o, 0, NULL, NULL, &rep);
    check(n == 300000 && rep.status == EXPORT_OK && rep.bytes > 10000000, "full export streams the whole cursor (300000 rows) and reports bytes");
    check(count_lines("/tmp/lume_pg_full.csv") == 300001, "file has header + 300000 lines");
    check(rs.nrows == 500 && rs_bytes(&rs) < 2u << 20, "loaded rows stay at 500 and memory stays under 2 MB: streamed blocks are dropped");
    check(rs.cursor == NULL && rs.cursor_lost && !rs.complete, "cursor consumed: result marked partial, not complete");
    FILE *f = fopen("/tmp/lume_pg_full.csv", "r");
    char line[512]; int ln = 0; bool first_ok = false, bool_ok = false;
    while (f && fgets(line, sizeof line, f) && ln < 3) { ln++; if (ln == 2 && strncmp(line, "1,v1,", 5) == 0) first_ok = true; if (ln == 2 && strstr(line, ",f\n")) bool_ok = true; }
    if (f) fclose(f);
    check(first_ok && bool_ok, "rows written in source order with raw values (bool as t/f)");
    remove("/tmp/lume_pg_full.csv");
    grid_free(&g); rs_free(&rs);

    /* row limit inside the loaded rows keeps the cursor usable afterwards; a limit that
     * streamed rows away releases it (no silent gap) */
    db_pg_driver.exec(&db, "SELECT g FROM generate_series(1, 5000) g", -1, 100, &rs);
    grid_init(&g, &rs);
    n = grid_export_full(&g, "/tmp/lume_pg_part.csv", &o, 50, NULL, NULL, &rep);
    check(n == 50 && rep.status == EXPORT_STOP_ROWS && rs.cursor != NULL && rs.nrows == 100, "row limit inside the loaded rows stops at 50 and keeps the cursor open");
    check(rs_fetch_more(&rs, 50) && rs.nrows == 150 && strcmp(rs_cell(&rs, 149, 0), "150") == 0, "fetch-more continues without a gap");
    n = grid_export_full(&g, "/tmp/lume_pg_part.csv", &o, 1000, NULL, NULL, &rep);
    check(n == 1000 && rep.status == EXPORT_STOP_ROWS && rep.cursor_released && rs.cursor == NULL && rs.cursor_lost && rs.nrows == 150, "row limit past the loaded rows releases the cursor and marks the set partial");
    check(!rs_fetch_more(&rs, 50) && rs.nrows == 150, "no fetch-more after rows were streamed away (would leave a gap)");
    remove("/tmp/lume_pg_part.csv");
    grid_free(&g); rs_free(&rs);

    /* US-047: cancel after several blocks; nothing left behind, cursor released, connection usable */
    db_pg_driver.exec(&db, "SELECT g FROM generate_series(1, 200000) g", -1, 100, &rs);
    grid_init(&g, &rs);
    cancel_at = 6000;
    remove("/tmp/lume_pg_cancel.csv"); remove("/tmp/lume_pg_cancel.csv.part");
    n = grid_export_full(&g, "/tmp/lume_pg_cancel.csv", &o, 0, prog_cancel, NULL, &rep);
    check(n == -2 && rep.status == EXPORT_CANCELLED && access("/tmp/lume_pg_cancel.csv", F_OK) != 0 && access("/tmp/lume_pg_cancel.csv.part", F_OK) != 0, "cancel after several blocks: no file and no .part left");
    check(rs.cursor == NULL && rs.cursor_lost && rep.cursor_released && rs.nrows == 100, "cancel reaches the cursor (PQcancel + drain): released, loaded rows kept, set marked partial");
    check(last.total == -1 && last.bytes > 0 && last.rows >= 6000, "progress reported rows, bytes and an unknown total");
    grid_free(&g); rs_free(&rs);
    db_pg_driver.exec(&db, "SELECT 1", -1, 0, &rs);
    check(rs.ok && rs.nrows == 1, "the connection is usable right after the cancel");
    rs_free(&rs);

    /* server-side select builder on a real relation */
    char q[256], sql[1024];
    db_pg_driver.quote_ident("information_schema.tables", q, sizeof q);
    db_pg_driver.select_sql(q, "\"table_schema\" = 'pg_catalog'", "\"table_name\" DESC", 5, sql, sizeof sql);
    db_pg_driver.exec(&db, sql, -1, 500, &rs);
    check(rs.ok && rs.nrows == 5 && rs.complete, "server-side WHERE + ORDER BY + LIMIT runs");
    int tn = -1;
    for (int c = 0; c < rs.ncols; c++) if (strcmp(rs.cols[c], "table_name") == 0) tn = c;
    check(tn >= 0 && strcmp(rs_cell(&rs, 0, tn), rs_cell(&rs, 1, tn)) > 0, "rows come back in the requested server order");
    rs_free(&rs);

    /* ---- editing model on PostgreSQL (US-048 .. US-050) ---- */
    Db dbe = { .drv = &db_pg_driver };
    if (db_pg_driver.open(&dbe, conn, err, sizeof err)) {
        run(&db, "DROP TABLE IF EXISTS lume_edit_check");
        run(&db, "CREATE TABLE lume_edit_check (id integer GENERATED ALWAYS AS IDENTITY PRIMARY KEY, name text UNIQUE, amount numeric(40, 2), "
                 "ts timestamp(6) with time zone, j json, jb jsonb, raw bytea, f real, secret_token text, doubled integer GENERATED ALWAYS AS (id * 2) STORED, note text)");
        run(&db, "INSERT INTO lume_edit_check (name, amount, ts, j, jb, raw, f, secret_token, note) VALUES "
                 "('Ana', 1.50, '2024-01-02 03:04:05.123456+00', '{\"a\": 1}', '{\"b\": 2}', '\\x0102', 0.1, 'tok', NULL), "
                 "('Bia', 2.25, NULL, NULL, NULL, NULL, 1.5, NULL, 'n2')");
        View v; ApplyResult ar; char d[600], why[200];
        view_open(&v, &db, "lume_edit_check", "\"id\"");
        edit_describe_strategy(&v.e, d, sizeof d);
        check2(v.e.npk == 1 && strstr(d, "j (json has no equality") && strstr(d, "raw (binary)") && !strstr(d, "jb (") && !strstr(d, " f (") , "PG strategy: json and bytea left out, jsonb and real compared", d);
        check2(edit_col_readonly(&v.e, vcol(&v, "id")) && edit_col_readonly(&v.e, vcol(&v, "doubled")) && !edit_col_readonly(&v.e, vcol(&v, "name")), "identity ALWAYS and generated STORED columns are read-only", edit_col_readonly(&v.e, vcol(&v, "doubled")));
        /* conflict on an unedited column from another session */
        edit_put(&v.e, 0, vcol(&v, "name"), "Ana Maria", false);
        run(&dbe, "UPDATE lume_edit_check SET note = 'elsewhere' WHERE id = 1");
        edit_apply(&v.e, &db, NULL, &ar);
        check2(ar.status == APPLY_CONFLICT && strstr(ar.conflict, "note: you saw NULL, now elsewhere") != NULL, "PG: change to an unedited column by another session is a conflict with a diagnosis", ar.conflict);
        check2(scalar_is(&db, "SELECT name FROM lume_edit_check WHERE id = 1", "Ana") && v.e.nedits == 1, "PG: nothing written, pending kept", NULL);
        view_close(&v);
        /* precision: 38-digit numeric and microsecond timestamp round trip; secret masked */
        view_open(&v, &db, "lume_edit_check", "\"id\"");
        check2(edit_validate(&v.e, vcol(&v, "amount"), "123456789012345678901234567890123456.78", why, sizeof why), "PG accepts a 38-digit numeric", why);
        edit_put(&v.e, 0, vcol(&v, "amount"), "123456789012345678901234567890123456.78", false);
        edit_put(&v.e, 0, vcol(&v, "ts"), "2030-12-31 23:59:59.654321+00", false);
        edit_put(&v.e, 0, vcol(&v, "secret_token"), "newtok", false);
        edit_build(&v.e);
        check2(strstr(v.e.stmts[0].sql, "'123456789012345678901234567890123456.78'") && strstr(v.e.stmts[0].shown, "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2") && !strstr(v.e.stmts[0].shown, "newtok") && strstr(v.e.stmts[0].sql, "\"ts\" = '2024-01-02 03:04:05.123456+00'"), "PG: big numeric sent quoted, secret masked in the shown SQL, timestamp snapshot keeps microseconds", v.e.stmts[0].shown);
        edit_apply(&v.e, &db, NULL, &ar);
        check2(ar.status == APPLY_OK && scalar_is(&db, "SELECT amount::text FROM lume_edit_check WHERE id = 1", "123456789012345678901234567890123456.78") && scalar_is(&db, "SELECT to_char(ts AT TIME ZONE 'UTC', 'YYYY-MM-DD HH24:MI:SS.US') FROM lume_edit_check WHERE id = 1", "2030-12-31 23:59:59.654321"), "PG: 38-digit numeric and microsecond timestamp round-trip exactly", ar.message);
        view_close(&v);
        /* atomicity: second statement violates UNIQUE */
        view_open(&v, &db, "lume_edit_check", "\"id\"");
        edit_put(&v.e, 0, vcol(&v, "note"), "first", false);
        edit_put(&v.e, 1, vcol(&v, "name"), "Ana", false);
        edit_apply(&v.e, &db, NULL, &ar);
        check2(ar.status == APPLY_STMT_FAILED && ar.stmt == 1 && scalar_is(&db, "SELECT note FROM lume_edit_check WHERE id = 1", "elsewhere"), "PG: constraint violation rolls the whole batch back", ar.message);
        view_close(&v);
        /* connection killed right before COMMIT: result unknown, never claimed */
        char pidq[64] = "0";
        { ResultSet r; db_pg_driver.exec(&db, "SELECT pg_backend_pid()", -1, 0, &r); if (r.nrows) snprintf(pidq, sizeof pidq, "%s", rs_cell(&r, 0, 0)); rs_free(&r); }
        view_open(&v, &db, "lume_edit_check", "\"id\"");
        edit_put(&v.e, 1, vcol(&v, "note"), "before-kill", false);
        CommitKill ck = { &dbe, "", 1 };
        snprintf(ck.pid, sizeof ck.pid, "%s", pidq);
        EditHooks hk = { kill_before, &ck };
        edit_apply(&v.e, &db, &hk, &ar);
        check2(ar.status == APPLY_UNKNOWN && strstr(ar.message, "may or may not") != NULL && v.e.nedits == 1, "PG: connection dropped at COMMIT: result reported as unknown, pending kept", ar.message);
        check2(scalar_is(&dbe, "SELECT note FROM lume_edit_check WHERE id = 2", "n2"), "(the server in fact discarded that transaction)", NULL);
        view_close(&v);
        run(&dbe, "DROP TABLE lume_edit_check");
        db_pg_driver.close(&dbe);
        /* db is dead now: reopen for what follows */
        db_pg_driver.close(&db);
        db = (Db){ .drv = &db_pg_driver };
        if (!db_pg_driver.open(&db, conn, err, sizeof err)) { printf("reopen failed: %s\n", err); return 2; }
    } else printf("skip  edit checks: second connection unavailable: %s\n", err);

    /* ---- permissions (US-060 adverse cases, US-058 / US-050 on a user without privileges) ----
     * A login role that may read the child table and update only its "note" column, and has no
     * access at all to the parent. Errors must surface with the server's text and SQLSTATE,
     * never as an empty result, a crash or a reported success. */
    {
        run(&db, "DROP TABLE IF EXISTS lume_perm_child, lume_perm_parent, lume_perm_bin");
        run(&db, "DO $$ BEGIN IF EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'lume_ro') THEN EXECUTE 'DROP OWNED BY lume_ro'; EXECUTE 'DROP ROLE lume_ro'; END IF; END $$");
        run(&db, "CREATE TABLE lume_perm_parent (id integer PRIMARY KEY, name text)");
        run(&db, "CREATE TABLE lume_perm_child (id integer PRIMARY KEY, parent_id integer REFERENCES lume_perm_parent(id), secret text, note text)");
        run(&db, "INSERT INTO lume_perm_parent VALUES (1, 'p1')");
        run(&db, "INSERT INTO lume_perm_child VALUES (10, 1, 's', 'n')");
        run(&db, "CREATE ROLE lume_ro LOGIN PASSWORD 'lume_ro_pw'");
        run(&db, "GRANT SELECT ON lume_perm_child TO lume_ro");
        run(&db, "GRANT UPDATE (note) ON lume_perm_child TO lume_ro");
        char roconn[700];
        snprintf(roconn, sizeof roconn, "%s user=lume_ro password=lume_ro_pw", conn);   /* libpq: the last keyword wins */
        Db dro = { .drv = &db_pg_driver };
        if (db_pg_driver.open(&dro, roconn, err, sizeof err)) {
            char q[128], sql[512];
            /* 1. preview of a table without SELECT */
            db_pg_driver.quote_ident("lume_perm_parent", q, sizeof q);
            db_pg_driver.select_sql(q, NULL, NULL, 200, sql, sizeof sql);
            ResultSet pr; db_pg_driver.exec(&dro, sql, -1, 200, &pr);
            check2(!pr.ok && strstr(pr.error, "permission denied") && strcmp(pr.err_state, "42501") == 0 && !pr.has_rows, "PG permissions: preview of a table without SELECT fails with the server message and SQLSTATE 42501", pr.error);
            rs_free(&pr);
            /* 2. foreign-key navigation: the constraint is listed, the jump is refused as an error */
            DbFk *fks = NULL; int nfks = 0; char ferr[200] = "";
            bool listed = db_pg_driver.list_fks && db_pg_driver.list_fks(&dro, "lume_perm_child", &fks, &nfks, ferr, sizeof ferr);
            check2(listed && nfks == 1 && strcmp(fks[0].to_table, "lume_perm_parent") == 0, "PG permissions: the FK of a readable table is listed", ferr);
            if (listed) db_fks_free(fks, nfks);
            db_pg_driver.select_sql(q, "\"id\" = '1'", NULL, 200, sql, sizeof sql);
            ResultSet fr; db_pg_driver.exec(&dro, sql, -1, 200, &fr);
            check2(!fr.ok && strcmp(fr.err_state, "42501") == 0, "PG permissions: navigating to the inaccessible parent is an error, not an empty result", fr.error);
            rs_free(&fr);
            check(scalar_is(&dro, "SELECT 1", "1"), "PG permissions: the connection stays usable after the refusals");
            /* composite key: columns paired in order (the old information_schema join crossed them) */
            run(&db, "DROP TABLE IF EXISTS lume_fk_c, lume_fk_p");
            run(&db, "CREATE TABLE lume_fk_p (a integer, b text, PRIMARY KEY (a, b))");
            run(&db, "CREATE TABLE lume_fk_c (id integer PRIMARY KEY, pb text, pa integer, FOREIGN KEY (pa, pb) REFERENCES lume_fk_p (a, b))");
            run(&db, "GRANT SELECT ON lume_fk_c TO lume_ro");
            DbFk *cf = NULL; int ncf = 0;
            bool cl = db_pg_driver.list_fks(&dro, "lume_fk_c", &cf, &ncf, ferr, sizeof ferr);
            check2(cl && ncf == 2 && strcmp(cf[0].from_col, "pa") == 0 && strcmp(cf[0].to_col, "a") == 0 && strcmp(cf[1].from_col, "pb") == 0 && strcmp(cf[1].to_col, "b") == 0 && strcmp(cf[0].name, cf[1].name) == 0,
                   "PG: composite FK listed as 2 column pairs in key order, for a user who owns neither table", ferr);
            if (cl) db_fks_free(cf, ncf);
            run(&db, "DROP TABLE lume_fk_c, lume_fk_p");
            /* 3. editing: the granted column applies, the other one fails and writes nothing */
            View v; ApplyResult ar;
            view_open(&v, &dro, "lume_perm_child", "\"id\"");
            edit_put(&v.e, 0, vcol(&v, "note"), "edited by ro", false);
            edit_apply(&v.e, &dro, NULL, &ar);
            check2(ar.status == APPLY_OK && scalar_is(&db, "SELECT note FROM lume_perm_child WHERE id = 10", "edited by ro"), "PG permissions: UPDATE on the granted column applies", ar.message);
            view_close(&v);
            view_open(&v, &dro, "lume_perm_child", "\"id\"");
            edit_put(&v.e, 0, vcol(&v, "note"), "second", false);
            edit_put(&v.e, 0, vcol(&v, "secret"), "stolen", false);
            edit_apply(&v.e, &dro, NULL, &ar);
            check2(ar.status == APPLY_STMT_FAILED && strstr(ar.message, "permission denied") && v.e.nedits == 2, "PG permissions: UPDATE touching a column without privilege fails, reported as not applied, pending kept", ar.message);
            check(scalar_is(&db, "SELECT note || '/' || secret FROM lume_perm_child WHERE id = 10", "edited by ro/s"), "PG permissions: nothing from the failed batch was written");
            view_close(&v);
            db_pg_driver.close(&dro);
        } else printf("FAIL  PG permissions: cannot log in as lume_ro: %s\n", err), failures++;
        run(&db, "DROP TABLE IF EXISTS lume_perm_child, lume_perm_parent");
        run(&db, "DROP OWNED BY lume_ro"); run(&db, "DROP ROLE lume_ro");

        /* bytea through SQL INSERT export (US-053): the PostgreSQL literal re-inserts the same bytes */
        run(&db, "CREATE TABLE lume_perm_bin (id integer PRIMARY KEY, b bytea)");
        run(&db, "INSERT INTO lume_perm_bin VALUES (1, '\\x00ff10'), (2, ''), (3, NULL)");
        ResultSet br; db_pg_driver.exec(&db, "SELECT id, b FROM lume_perm_bin ORDER BY id", -1, 0, &br);
        Grid bg; grid_init(&bg, &br); grid_select_all(&bg);
        char *ins = grid_copy(&bg, FMT_SQL, true, "lume_perm_bin_copy");
        run(&db, "CREATE TABLE lume_perm_bin_copy (id integer PRIMARY KEY, b bytea)");
        bool all = true;
        for (char *st = ins, *nl; st && *st; st = nl ? nl + 1 : NULL) { nl = strchr(st, '\n'); if (nl) *nl = 0; if (*st && !run(&db, st)) all = false; }
        check2(all && scalar_is(&db, "SELECT count(*) FROM (SELECT * FROM lume_perm_bin EXCEPT SELECT * FROM lume_perm_bin_copy) d", "0") && scalar_is(&db, "SELECT encode(b, 'hex') FROM lume_perm_bin_copy WHERE id = 1", "00ff10"), "PG: bytea exported as '\\x..'::bytea re-inserts the same bytes (empty and NULL kept)", ins);
        free(ins); grid_free(&bg); rs_free(&br);
        run(&db, "DROP TABLE lume_perm_bin, lume_perm_bin_copy");
    }

    /* ---- executor on PostgreSQL (OUT-007 / 012 / 013 / 017 / 018) ---- */
    {
        ExecJob *j = exec_start(&db, "DO $$ BEGIN RAISE NOTICE 'hello from plpgsql'; RAISE WARNING 'careful'; END $$;\nSELECT * FROM nope_table;\nSELECT 1;", -1, 100, false);
        check(j != NULL && j->n == 3, "pg: script split (dollar-quoted DO block stays one statement)");
        wait_job(j, 10000);
        ResultSet r;
        check(exec_take_result(j, 0, &r) && r.ok && r.notices && strstr(r.notices, "NOTICE: hello from plpgsql") && strstr(r.notices, "WARNING: careful"), "pg: NOTICE / WARNING messages captured during the statement");
        rs_free(&r);
        check(exec_take_result(j, 1, &r) && !r.ok && strcmp(r.err_state, "42P01") == 0 && r.err_offset == 14 && strstr(r.error, "nope_table"), "pg: SQLSTATE and statement position reported with the error");
        rs_free(&r);
        check(j->stmts[2].status == XS_OK, "pg: continue on error ran the third statement");
        exec_free(j);
        /* cancel reaches the server: pg_sleep is interrupted with SQLSTATE 57014 */
        j = exec_start(&db, "SELECT pg_sleep(20)", -1, 100, true);
        usleep(150000);
        double t0 = mono_ms();
        exec_cancel(j);
        wait_job(j, 10000);
        bool got = exec_take_result(j, 0, &r);
        char d[160]; snprintf(d, sizeof d, "%.0f ms after cancel, state %s, cancelled=%d", mono_ms() - t0, r.err_state, r.cancelled);
        check(got && !exec_running(j) && j->stmts[0].status == XS_CANCELLED && r.cancelled && strcmp(r.err_state, "57014") == 0 && mono_ms() - t0 < 3000, "pg: PQcancel stops pg_sleep(20); reported as cancelled (57014), not as an error");
        printf("      (%s)\n", d);
        rs_free(&r); exec_free(j);
        /* transaction state comes from the driver, never from the text */
        check(db_pg_driver.tx_state(&db) == 0, "pg: no transaction open at idle");
        j = exec_start(&db, "BEGIN; CREATE TEMP TABLE lume_tx_t (x int); INSERT INTO lume_tx_t VALUES (1);", -1, 100, true);
        wait_job(j, 10000); exec_free(j);
        check(db_pg_driver.tx_state(&db) == 1, "pg: after BEGIN the driver reports an open transaction");
        j = exec_start(&db, "ROLLBACK", -1, 100, true); wait_job(j, 10000); exec_free(j);
        check(db_pg_driver.tx_state(&db) == 0, "pg: after ROLLBACK the driver reports none");
        db_pg_driver.exec(&db, "SELECT 1", -1, 0, &r);
        check(r.ok, "pg: connection healthy after the cancel and the rollback");
        rs_free(&r);
    }

    /* US-047: the backend is terminated mid-stream from a second connection (last: it kills db) */
    Db db2 = { .drv = &db_pg_driver };
    if (db_pg_driver.open(&db2, conn, err, sizeof err)) {
        db_pg_driver.exec(&db, "SELECT pg_backend_pid()", -1, 0, &rs);
        KillCtx kc = { &db2, "", false };
        snprintf(kc.pid, sizeof kc.pid, "%s", rs.nrows ? rs_cell(&rs, 0, 0) : "0");
        rs_free(&rs);
        remove("/tmp/lume_pg_kill.csv"); remove("/tmp/lume_pg_kill.csv.part");
        db_pg_driver.exec(&db, "SELECT g FROM generate_series(1, 200000) g", -1, 100, &rs);
        grid_init(&g, &rs);
        n = grid_export_full(&g, "/tmp/lume_pg_kill.csv", &o, 0, prog_kill, &kc, &rep);
        check(n == -1 && rep.status == EXPORT_ERR_FETCH && strstr(rep.err, "fetch stopped") != NULL, "backend terminated mid-stream: reported as a fetch error, distinct from a write error");
        check(access("/tmp/lume_pg_kill.csv", F_OK) != 0 && access("/tmp/lume_pg_kill.csv.part", F_OK) != 0, "a failed export leaves no file and no .part");
        printf("      (%.90s)\n", rep.err);
        grid_free(&g); rs_free(&rs);
        db_pg_driver.exec(&db, "SELECT 1", -1, 0, &rs);
        check(!rs.ok && rs.error[0], "after the kill the connection reports its failure instead of pretending");
        rs_free(&rs);
        db_pg_driver.close(&db2);
    } else printf("skip  second connection unavailable: %s\n", err);

    db_pg_driver.close(&db);
    printf("%s (%d failures)\n", failures ? "PG GRID: FAIL" : "PG GRID: OK", failures);
    return failures ? 1 : 0;
}
