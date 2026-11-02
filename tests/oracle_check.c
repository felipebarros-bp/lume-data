/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless check of the Oracle driver: opens a connection, lists tables and columns, runs DDL,
 * DML and a SELECT, prints what it saw. Built without SDL (tests/shim) so it can run inside
 * a Linux container that has the Oracle client library. See tests/oracle_check.sh. */
#include "db.h"
#include "edit.h"
#include "exec.h"
#include "grid.h"
#include <time.h>
#include <unistd.h>
#include <stdarg.h>

extern const DbDriver db_oracle_driver;

/* helpers normally provided by db.c (which needs sqlite3) */
char *db_strdup(const char *s) { return s ? SDL_strdup(s) : NULL; }
void db_tables_free(DbTable *tables, int count) {
    for (int i = 0; i < count; i++) {
        free(tables[i].name);
        for (int c = 0; c < tables[i].ncols; c++) { free(tables[i].cols[c].name); free(tables[i].cols[c].type); }
        free(tables[i].cols);
    }
    free(tables);
}
/* Same as src/db.c (the container build cannot link db.c, which needs SDL): keep the two in
 * step, "OWNER.TABLE" is quoted per part. A copy without the split once made a cross-schema
 * test pass for the wrong reason. */
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
static double mono_ms(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec * 1000.0 + ts.tv_nsec / 1e6; }
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
}

static bool scalar_is(Db *db, const char *sql, const char *want) {
    ResultSet rs; db->drv->exec(db, sql, -1, 0, &rs);
    const char *v = rs.ok && rs.nrows ? rs_cell(&rs, 0, 0) : "<error>";
    bool ok = want ? (v && strcmp(v, want) == 0) : v == NULL;
    if (!ok) printf("      got [%s] want [%s] %s\n", v ? v : "NULL", want ? want : "NULL", rs.ok ? "" : rs.error);
    rs_free(&rs); return ok;
}
typedef struct { ResultSet rs; Grid g; EditSet e; DbTable t; } View;
static int vcol(View *v, const char *name) { for (int c = 0; c < v->rs.ncols; c++) if (strcmp(v->rs.cols[c], name) == 0) return c; return -1; }
static void view_open(View *v, Db *db, const char *table) {
    char q[128], sql[512], err[200];
    db->drv->quote_ident(table, q, sizeof q);
    db->drv->select_sql(q, NULL, "\"ID\"", 100, sql, sizeof sql);
    db->drv->exec(db, sql, -1, 0, &v->rs);
    if (!v->rs.ok) printf("      preview failed: %s\n", v->rs.error);
    grid_init(&v->g, &v->rs);
    edit_init(&v->e, &v->rs, db->drv, q, v->g.numeric);
    memset(&v->t, 0, sizeof v->t); v->t.name = (char *)table;
    if (!db->drv->list_columns(db, &v->t, err, sizeof err)) printf("      list_columns: %s\n", err);
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

static void run(Db *db, const char *sql, bool expect_rows) {
    ResultSet rs;
    db->drv->exec(db, sql, -1, 0, &rs);
    char detail[600];
    if (rs.ok && rs.has_rows) snprintf(detail, sizeof detail, "%d rows, %d cols, exec %.1f ms, fetch %.1f ms", rs.nrows, rs.ncols, rs.exec_ms, rs.fetch_ms);
    else if (rs.ok) snprintf(detail, sizeof detail, "%d affected, %.1f ms", rs.affected, rs.exec_ms);
    else snprintf(detail, sizeof detail, "%s", rs.error);
    check(rs.ok && rs.has_rows == expect_rows, sql, detail);
    if (rs.ok && rs.has_rows) {
        printf("     ");
        for (int c = 0; c < rs.ncols; c++) printf(" %s (%s)%s", rs.cols[c], rs.types[c], c + 1 < rs.ncols ? " |" : "\n");
        for (int i = 0; i < rs.nrows && i < 5; i++) {
            printf("     ");
            for (int c = 0; c < rs.ncols; c++) { const char *v = rs_cell(&rs, i, c); printf(" %s%s", v ? v : "NULL", c + 1 < rs.ncols ? " |" : "\n"); }
        }
    }
    rs_free(&rs);
}

/* setup / cleanup statement whose failure is expected or irrelevant (e.g. dropping what may not exist) */
static bool quiet(Db *db, const char *sql) { ResultSet rs; db->drv->exec(db, sql, -1, 0, &rs); bool ok = rs.ok; rs_free(&rs); return ok; }

/* cursor: fetch in batches through the driver's fetch() */
static void run_batched(Db *db, const char *sql, int batch, int expect_total) {
    ResultSet rs;
    db->drv->exec(db, sql, -1, batch, &rs);
    char detail[200];
    snprintf(detail, sizeof detail, "first batch %d rows, cursor %s", rs.nrows, rs.cursor ? "open" : "closed");
    check(rs.ok && rs.nrows == batch && rs.cursor != NULL, "exec with limit keeps a cursor", detail);
    int fetches = 0;
    while (rs_fetch_more(&rs, batch)) fetches++;
    snprintf(detail, sizeof detail, "%d rows after %d more fetches, complete=%d", rs.nrows, fetches, rs.complete);
    check(rs.nrows == expect_total && rs.complete && !rs.cursor, "fetch more until complete", detail);
    rs_free(&rs);
}

int main(void) {
    const char *user = getenv("ORA_USER"), *pass = getenv("ORA_PASSWORD"), *dsn = getenv("ORA_DSN");
    if (!user || !pass || !dsn) { fprintf(stderr, "set ORA_USER, ORA_PASSWORD, ORA_DSN (//host:port/service)\n"); return 2; }
    char location[1024];
    snprintf(location, sizeof location, "%s\n%s\n%s", user, pass, dsn);

    Db db = { &db_oracle_driver, NULL, NULL, location };
    char err[256] = "";
    bool opened = db.drv->open(&db, location, err, sizeof err);
    check(opened, "open", err);
    if (!opened) return 1;

    DbTable *tables = NULL; int n = 0;
    bool listed = db.drv->list_tables(&db, &tables, &n, err, sizeof err);
    char detail[128];
    snprintf(detail, sizeof detail, "%d tables/views", n);
    check(listed, "list_tables", listed ? detail : err);

    run(&db, "CREATE TABLE lume_check_tmp (id NUMBER PRIMARY KEY, name VARCHAR2(40), amount NUMBER(10,2), created DATE)", false);
    run(&db, "INSERT INTO lume_check_tmp VALUES (1, 'Juliette', 1250.5, SYSDATE)", false);
    run(&db, "INSERT INTO lume_check_tmp VALUES (2, NULL, NULL, NULL);", false);   /* trailing ';' must be stripped */
    run(&db, "SELECT id, name, amount, created FROM lume_check_tmp ORDER BY id", true);
    run_batched(&db, "SELECT level AS n FROM dual CONNECT BY level <= 2500", 1000, 2500);

    db_tables_free(tables, n);
    listed = db.drv->list_tables(&db, &tables, &n, err, sizeof err);
    DbTable *t = NULL;
    for (int i = 0; i < n; i++) if (strcmp(tables[i].name, "LUME_CHECK_TMP") == 0) t = &tables[i];
    check(t != NULL, "new table appears in list_tables", "");
    if (t) {
        bool cols = db.drv->list_columns(&db, t, err, sizeof err);
        snprintf(detail, sizeof detail, "%d columns", t->ncols);
        check(cols && t->ncols == 4, "list_columns", cols ? detail : err);
        for (int c = 0; c < t->ncols; c++) printf("      %s %s%s%s\n", t->cols[c].name, t->cols[c].type, t->cols[c].pk ? " PK" : "", t->cols[c].notnull ? " NOT NULL" : "");
        check(t->cols[0].pk && !t->cols[1].pk, "primary key detected", "");
        char q[128], sql[256];
        db.drv->quote_ident(t->name, q, sizeof q);
        db.drv->select_sql(q, NULL, NULL, 1, sql, sizeof sql);
        run(&db, sql, true);
    }
    {   /* a missing table must fail cleanly with the ORA- message, not crash or pretend */
        ResultSet rs; db.drv->exec(&db, "SELECT * FROM nope_not_here", -1, 0, &rs);
        check(!rs.ok && strstr(rs.error, "ORA-00942") != NULL, "unknown table reported as an ORA-00942 error", rs.error);
        rs_free(&rs);
    }
    run(&db, "BEGIN NULL; END;", false);
    run(&db, "DROP TABLE lume_check_tmp", false);

    db_tables_free(tables, n);

    /* ---- capabilities: what this driver does not offer is reported, not assumed (US-060) ---- */
    printf("%s  server-side sort / filter: %s\n", db.drv->caps & DB_CAP_SERVER_SORT ? "ok  " : "skip", db.drv->caps & DB_CAP_SERVER_SORT ? "supported" : "Not supported");
    if (db.drv->cancel) {
        /* OCIBreak from another thread interrupts a running query: ORA-01013 (OUT-018). The test
         * container disables out-of-band breaks (sqlnet.ora DISABLE_OOB=ON), so the break is
         * delivered in-band and only noticed while the server polls: a CPU-bound query, not a sleep */
        ExecJob *j = exec_start(&db, "SELECT COUNT(*) FROM (SELECT 1 FROM dual CONNECT BY level <= 2000) a, (SELECT 1 FROM dual CONNECT BY level <= 2000) b, (SELECT 1 FROM dual CONNECT BY level <= 2000) c", -1, 100, true);
        usleep(300000);
        double t0 = mono_ms();
        exec_cancel(j);
        while (exec_running(j) && mono_ms() - t0 < 10000) usleep(5000);
        ResultSet r; bool got = exec_take_result(j, 0, &r);
        char d[200]; snprintf(d, sizeof d, "%.0f ms after cancel, status %d, %s", mono_ms() - t0, j->stmts[0].status, got ? r.error : "no result");
        check(got && !exec_running(j) && j->stmts[0].status == XS_CANCELLED && r.cancelled && r.err_code == 1013 && mono_ms() - t0 < 5000, "cancel of a running statement: OCIBreak stops a running query, reported as cancelled (ORA-01013)", d);
        if (got) rs_free(&r);
        exec_free(j);
        ResultSet r2; db.drv->exec(&db, "SELECT 1 FROM dual", -1, 0, &r2);
        check(r2.ok, "connection usable after the cancel", r2.ok ? "" : r2.error);
        rs_free(&r2);
        /* a sleeping block never polls for an in-band break: the second route (ALTER SYSTEM
         * CANCEL SQL from a helper session) must stop it */
        j = exec_start(&db, "BEGIN DBMS_SESSION.SLEEP(20); END;", -1, 100, true);
        usleep(300000);
        t0 = mono_ms();
        exec_cancel(j);
        while (exec_running(j) && mono_ms() - t0 < 10000) usleep(5000);
        got = exec_take_result(j, 0, &r);
        snprintf(d, sizeof d, "%.0f ms after cancel, status %d, %s", mono_ms() - t0, j->stmts[0].status, got ? r.error : "no result");
        check(got && !exec_running(j) && j->stmts[0].status == XS_CANCELLED && r.cancelled && mono_ms() - t0 < 5000, "cancel of a sleeping PL/SQL block through the helper session (ALTER SYSTEM CANCEL SQL)", d);
        if (got) rs_free(&r);
        exec_free(j);
        db.drv->exec(&db, "SELECT 1 FROM dual", -1, 0, &r2);
        check(r2.ok, "connection usable after the second cancel", r2.ok ? "" : r2.error);
        rs_free(&r2);
    } else printf("skip  cancel of a running statement: Not supported\n");
    printf("%s  foreign-key listing: %s\n", db.drv->list_fks ? "ok  " : "skip", db.drv->list_fks ? "supported" : "Not supported on Oracle");
    if (db.drv->list_fks) {
        run(&db, "CREATE TABLE lume_fk_parent (a NUMBER, b VARCHAR2(10), v VARCHAR2(10), PRIMARY KEY (a, b))", false);
        run(&db, "CREATE TABLE lume_fk_child (id NUMBER PRIMARY KEY, pa NUMBER, pb VARCHAR2(10), CONSTRAINT lume_fk_c FOREIGN KEY (pa, pb) REFERENCES lume_fk_parent (a, b))", false);
        DbFk *fks = NULL; int nf = 0;
        bool ok = db.drv->list_fks(&db, "LUME_FK_CHILD", &fks, &nf, err, sizeof err);
        char detail[200]; snprintf(detail, sizeof detail, "%d columns%s%s", nf, nf ? ": " : "", nf ? fks[0].name : err);
        check(ok && nf == 2 && strcmp(fks[0].name, fks[1].name) == 0 && strcmp(fks[0].from_col, "PA") == 0 && strcmp(fks[1].to_col, "B") == 0 && strcmp(fks[0].to_table, "LUME_FK_PARENT") == 0, "composite FK listed as two columns of one constraint", detail);
        if (ok) { for (int i = 0; i < nf; i++) { free(fks[i].name); free(fks[i].from_col); free(fks[i].to_table); free(fks[i].to_col); } free(fks); }
        run(&db, "INSERT INTO lume_fk_parent VALUES (1, 'x', 'one')", false);
        char q[128], sql[512];
        db.drv->quote_ident("LUME_FK_PARENT", q, sizeof q);
        db.drv->select_sql(q, "\"A\" = 1 AND \"B\" = 'x'", NULL, 100, sql, sizeof sql);
        run(&db, sql, true);
        run(&db, "DROP TABLE lume_fk_child", false);
        run(&db, "DROP TABLE lume_fk_parent", false);
        /* quoted mixed-case identifiers survive discovery, quoting and the filtered select */
        run(&db, "CREATE TABLE \"Mixed Parent\" (\"Id\" NUMBER PRIMARY KEY, v VARCHAR2(10))", false);
        run(&db, "CREATE TABLE \"Mixed Child\" (id NUMBER PRIMARY KEY, \"Parent Id\" NUMBER REFERENCES \"Mixed Parent\" (\"Id\"))", false);
        run(&db, "INSERT INTO \"Mixed Parent\" VALUES (7, 'seven')", false);
        fks = NULL; nf = 0;
        ok = db.drv->list_fks(&db, "Mixed Child", &fks, &nf, err, sizeof err);
        check(ok && nf == 1 && strcmp(fks[0].from_col, "Parent Id") == 0 && strcmp(fks[0].to_table, "Mixed Parent") == 0 && strcmp(fks[0].to_col, "Id") == 0, "quoted mixed-case FK discovered with exact names", ok ? fks[0].to_table : err);
        if (ok && nf == 1) {
            char qt[128], qc[128], where[200];
            db.drv->quote_ident(fks[0].to_table, qt, sizeof qt); db.drv->quote_ident(fks[0].to_col, qc, sizeof qc);
            snprintf(where, sizeof where, "%s = 7", qc);
            db.drv->select_sql(qt, where, NULL, 100, sql, sizeof sql);
            run(&db, sql, true);
            for (int i = 0; i < nf; i++) { free(fks[i].name); free(fks[i].from_col); free(fks[i].to_table); free(fks[i].to_col); } free(fks);
        }
        /* an object the user cannot see fails at navigation time with the server's reason, never silently */
        db.drv->quote_ident("SYS.NOPE_PRIVATE", q, sizeof q);
        db.drv->select_sql(q, NULL, NULL, 1, sql, sizeof sql);
        { ResultSet rr; db.drv->exec(&db, sql, -1, 0, &rr); check(!rr.ok && strstr(rr.error, "ORA-00942") != NULL, "navigation to an inaccessible object reports ORA-00942", rr.error); rs_free(&rr); }
        run(&db, "DROP TABLE \"Mixed Child\"", false);
        run(&db, "DROP TABLE \"Mixed Parent\"", false);
    }
    /* ---- permissions (US-060 adverse cases, US-058 / US-050) ----
     * LUME_OWN owns a parent and a child table; LUME_RO may read the child and update only its
     * NOTE column, and cannot see the parent at all. Every refusal must surface as the ORA- error
     * (never an empty result, a crash or a reported success); the catalog must still describe a
     * granted table of another schema (columns, key) so that editing works where it is allowed. */
    {
        quiet(&db, "DROP USER lume_ro CASCADE"); quiet(&db, "DROP USER lume_own CASCADE");
        bool setup = quiet(&db, "CREATE USER lume_own IDENTIFIED BY \"Lume_own_pw1\" QUOTA UNLIMITED ON users")
                  && quiet(&db, "CREATE USER lume_ro IDENTIFIED BY \"Lume_ro_pw1\"")
                  && quiet(&db, "GRANT CREATE SESSION TO lume_ro")
                  && quiet(&db, "CREATE TABLE lume_own.lume_perm_parent (id NUMBER PRIMARY KEY, name VARCHAR2(20))")
                  && quiet(&db, "CREATE TABLE lume_own.lume_perm_child (id NUMBER PRIMARY KEY, parent_id NUMBER CONSTRAINT lume_perm_fk REFERENCES lume_own.lume_perm_parent (id), secret VARCHAR2(20), note VARCHAR2(40))")
                  && quiet(&db, "INSERT INTO lume_own.lume_perm_parent VALUES (1, 'p1')")
                  && quiet(&db, "INSERT INTO lume_own.lume_perm_child VALUES (10, 1, 's', 'n')")
                  && quiet(&db, "COMMIT")
                  && quiet(&db, "GRANT SELECT ON lume_own.lume_perm_child TO lume_ro")
                  && quiet(&db, "GRANT UPDATE (note) ON lume_own.lume_perm_child TO lume_ro");
        check(setup, "permissions: owner + restricted user created", "");
        char roloc[600];
        const char *dsn_part = strrchr(location, '\n');
        snprintf(roloc, sizeof roloc, "LUME_RO\nLume_ro_pw1%s", dsn_part ? dsn_part : "");
        Db dro = { &db_oracle_driver, NULL, NULL, roloc };
        char rerr[256] = "";
        if (setup && dro.drv->open(&dro, roloc, rerr, sizeof rerr)) {
            char q[128], sql[512];
            dro.drv->quote_ident("LUME_OWN.LUME_PERM_PARENT", q, sizeof q);
            dro.drv->select_sql(q, NULL, NULL, 100, sql, sizeof sql);
            ResultSet pr; dro.drv->exec(&dro, sql, -1, 100, &pr);
            check(!pr.ok && strstr(pr.error, "ORA-00942") && !pr.has_rows, "permissions: preview of a table without SELECT fails with ORA-00942", pr.error);
            rs_free(&pr);
            DbFk *fks = NULL; int nf = 0;
            bool ok = dro.drv->list_fks(&dro, "LUME_OWN.LUME_PERM_CHILD", &fks, &nf, rerr, sizeof rerr);
            check(ok && nf == 1 && strcmp(fks[0].from_col, "PARENT_ID") == 0 && fks[0].to_table[0] == 0, "permissions: FK to an invisible table is still listed, marked not visible (empty target)", ok && nf ? fks[0].name : rerr);
            if (ok) { for (int i = 0; i < nf; i++) { free(fks[i].name); free(fks[i].from_col); free(fks[i].to_table); free(fks[i].to_col); } free(fks); }
            View v; ApplyResult ar;
            view_open(&v, &dro, "LUME_OWN.LUME_PERM_CHILD");
            check(v.t.ncols == 4 && v.e.npk == 1, "permissions: columns and primary key of a granted table in another schema are found", "");
            edit_put(&v.e, 0, vcol(&v, "NOTE"), "edited by ro", false);
            edit_apply(&v.e, &dro, NULL, &ar);
            check(ar.status == APPLY_OK && scalar_is(&db, "SELECT note FROM lume_own.lume_perm_child WHERE id = 10", "edited by ro"), "permissions: UPDATE on the granted column applies", ar.message);
            view_close(&v);
            view_open(&v, &dro, "LUME_OWN.LUME_PERM_CHILD");
            edit_put(&v.e, 0, vcol(&v, "NOTE"), "second", false);
            edit_put(&v.e, 0, vcol(&v, "SECRET"), "stolen", false);
            edit_apply(&v.e, &dro, NULL, &ar);
            /* ORA-01031 before 23ai, ORA-41900 ("missing UPDATE privilege") from 23ai on */
            check(ar.status == APPLY_STMT_FAILED && (strstr(ar.message, "ORA-01031") || strstr(ar.message, "ORA-41900")) && v.e.nedits == 2, "permissions: UPDATE of a column without privilege fails (ORA-01031 / ORA-41900 on 23ai), not applied, pending kept", ar.message);
            check(scalar_is(&db, "SELECT note || '/' || secret FROM lume_own.lume_perm_child WHERE id = 10", "edited by ro/s"), "permissions: nothing from the failed batch was written", "");
            view_close(&v);
            check(scalar_is(&dro, "SELECT 1 FROM dual", "1"), "permissions: the session stays usable after the refusals", "");
            dro.drv->close(&dro);
        } else check(false, "permissions: log in as LUME_RO", rerr);
        quiet(&db, "DROP USER lume_ro CASCADE"); quiet(&db, "DROP USER lume_own CASCADE");

        /* RAW through SQL INSERT export (US-053): HEXTORAW re-inserts the same bytes */
        quiet(&db, "DROP TABLE lume_raw_tmp"); quiet(&db, "DROP TABLE lume_raw_copy");
        quiet(&db, "CREATE TABLE lume_raw_tmp (id NUMBER PRIMARY KEY, b RAW(16))");
        quiet(&db, "CREATE TABLE lume_raw_copy (id NUMBER PRIMARY KEY, b RAW(16))");
        quiet(&db, "INSERT INTO lume_raw_tmp VALUES (1, HEXTORAW('00FF10'))");
        quiet(&db, "INSERT INTO lume_raw_tmp VALUES (2, NULL)");
        ResultSet br; db.drv->exec(&db, "SELECT id, b FROM lume_raw_tmp ORDER BY id", -1, 0, &br);
        Grid bg; grid_init(&bg, &br); grid_select_all(&bg);
        char *ins = grid_copy(&bg, FMT_SQL, true, "lume_raw_copy");
        bool all = true;
        for (char *st = ins, *nl; st && *st; st = nl ? nl + 1 : NULL) { nl = strchr(st, '\n'); if (nl) *nl = 0; size_t l = strlen(st); if (l && st[l - 1] == ';') st[l - 1] = 0; if (*st && !quiet(&db, st)) all = false; }
        check(all && br.kinds[1] == CK_BINARY && scalar_is(&db, "SELECT RAWTOHEX(b) FROM lume_raw_copy WHERE id = 1", "00FF10") && scalar_is(&db, "SELECT COUNT(*) FROM lume_raw_copy WHERE id = 2 AND b IS NULL", "1"), "RAW exported as HEXTORAW('..') re-inserts the same bytes, NULL kept", ins);
        free(ins); grid_free(&bg); rs_free(&br);
        quiet(&db, "DROP TABLE lume_raw_tmp"); quiet(&db, "DROP TABLE lume_raw_copy");
    }

    printf("ok    explicit transactions: implicit on Oracle (tx_begin NULL), COMMIT / ROLLBACK used\n");

    /* ---- editing model with a second session (US-048 .. US-050) ---- */
    Db db2 = { &db_oracle_driver, NULL, NULL, location };
    if (db2.drv->open(&db2, location, err, sizeof err)) {
        run(&db, "CREATE TABLE lume_edit_tmp (id NUMBER GENERATED ALWAYS AS IDENTITY PRIMARY KEY, name VARCHAR2(40) UNIQUE, amount NUMBER(38,2), d DATE, note VARCHAR2(40), twice NUMBER GENERATED ALWAYS AS (amount * 2) VIRTUAL)", false);
        run(&db, "INSERT INTO lume_edit_tmp (name, amount, d, note) VALUES ('Ana', 1.5, SYSDATE, NULL)", false);
        run(&db, "INSERT INTO lume_edit_tmp (name, amount, d, note) VALUES ('Bia', 2.25, NULL, 'n2')", false);
        run(&db, "COMMIT", false);
        View v; ApplyResult ar; char d[600];
        view_open(&v, &db, "LUME_EDIT_TMP");
        edit_describe_strategy(&v.e, d, sizeof d);
        check(v.e.npk == 1 && strstr(d, "D (Oracle dates") != NULL, "strategy: key + snapshot; DATE left out (session format)", d);
        check(edit_col_readonly(&v.e, vcol(&v, "ID")) != NULL && edit_col_readonly(&v.e, vcol(&v, "TWICE")) != NULL && edit_col_readonly(&v.e, vcol(&v, "NAME")) == NULL, "identity ALWAYS and virtual columns are read-only", edit_col_readonly(&v.e, vcol(&v, "TWICE")));
        edit_put(&v.e, 0, vcol(&v, "NAME"), "Ana Maria", false);
        run(&db2, "UPDATE lume_edit_tmp SET note = 'elsewhere' WHERE id = 1", false);
        run(&db2, "COMMIT", false);
        edit_apply(&v.e, &db, NULL, &ar);
        check(ar.status == APPLY_CONFLICT && strstr(ar.conflict, "NOTE: you saw NULL, now elsewhere") != NULL, "change by another session to an unedited column: conflict with diagnosis", ar.conflict);
        check(scalar_is(&db, "SELECT name FROM lume_edit_tmp WHERE id = 1", "Ana") && v.e.nedits == 1, "nothing written, pending kept", "");
        view_close(&v);
        view_open(&v, &db, "LUME_EDIT_TMP");
        edit_put(&v.e, 0, vcol(&v, "AMOUNT"), "123456789012345678901234567890123456.78", false);
        edit_apply(&v.e, &db, NULL, &ar);
        check(ar.status == APPLY_OK && scalar_is(&db, "SELECT TO_CHAR(amount, 'FM999999999999999999999999999999999999990.00') FROM lume_edit_tmp WHERE id = 1", "123456789012345678901234567890123456.78"), "38-digit NUMBER round-trips exactly", ar.message);
        view_close(&v);
        view_open(&v, &db, "LUME_EDIT_TMP");
        edit_put(&v.e, 0, vcol(&v, "NOTE"), "first", false);
        edit_put(&v.e, 1, vcol(&v, "NAME"), "Ana", false);
        edit_apply(&v.e, &db, NULL, &ar);
        check(ar.status == APPLY_STMT_FAILED && ar.stmt == 1 && scalar_is(&db, "SELECT note FROM lume_edit_tmp WHERE id = 1", "elsewhere"), "UNIQUE violation on the second statement rolls the batch back", ar.message);
        view_close(&v);
        run(&db, "DROP TABLE lume_edit_tmp", false);
        db2.drv->close(&db2);
    } else check(false, "second session for the edit checks", err);

    db.drv->close(&db);
    printf("%s (%d failures)\n", failures ? "ORACLE DRIVER: FAIL" : "ORACLE DRIVER: OK", failures);
    return failures ? 1 : 0;
}
