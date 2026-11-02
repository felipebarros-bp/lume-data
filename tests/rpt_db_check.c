/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Report Studio golden dataset on real servers (LI-096 / 029): the 3000 deterministic orders of
 * rpt_check loaded into PostgreSQL, MySQL, MariaDB, SQL Server and Oracle; every golden query is
 * compiled per dialect by sem.c and run exactly as the app runs it (sqlguard_wrap: read-only
 * transaction, rollback) through Lume's own drivers; values must match the exact expectation.
 * Second layer of the policy: a write that slipped past the lexical guard (a volatile function, say)
 * is refused by the engine itself inside the read-only transaction.
 * Servers that are not running are skipped (and said so). Passwords come from the environment set
 * by rpt_db_check.sh and are never printed.
 */
#define main rpt_check_main            /* reuse the dataset, catalog and expectations of rpt_check */
#include "rpt_check.c"
#undef main
#include "db.h"
#include "rs.h"

extern const DbDriver db_pg_driver, db_mysql_driver, db_mssql_driver, db_oracle_driver;

static bool exec1(Db *db, const char *sql, char *err) {
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
    bool ok = rs.ok;
    if (!ok && err) snprintf(err, 400, "%s", rs.error);
    rs_free(&rs);
    return ok;
}
static void load(int d, Db *db, const char *qt, char *err) {
    char sql[300];
    snprintf(sql, sizeof sql, "DROP TABLE %s", qt); exec1(db, sql, NULL);
    const char *num = d == DIALECT_ORACLE ? "NUMBER(12,2)" : "NUMERIC(12,2)";
    const char *txt = d == DIALECT_ORACLE ? "VARCHAR2(40)" : "VARCHAR(40)";
    char create[600];
    if (d == DIALECT_ORACLE) snprintf(create, sizeof create, "CREATE TABLE %s (\"id\" NUMBER PRIMARY KEY, \"ordered_at\" DATE, \"region\" %s, \"status\" %s, \"gross_total\" %s, \"net_total\" %s, \"customer_id\" NUMBER, \"customer_email\" %s)", qt, txt, txt, num, num, txt);
    else { char c[8][80]; static const char *const n[] = { "id", "ordered_at", "region", "status", "gross_total", "net_total", "customer_id", "customer_email" }; for (int i = 0; i < 8; i++) sem_quote_ident(d, n[i], c[i], sizeof c[i]);
        snprintf(create, sizeof create, "CREATE TABLE %s (%s INT PRIMARY KEY, %s DATE, %s %s, %s %s, %s %s, %s %s, %s INT, %s %s)", qt, c[0], c[1], c[2], txt, c[3], txt, c[4], num, c[5], num, c[6], c[7], txt); }
    if (!exec1(db, create, err)) return;
    int batch = d == DIALECT_ORACLE ? 200 : 500;
    char *buf = malloc(1 << 20);
    for (int s = 0; s < NROWS; s += batch) {
        size_t w = 0;
        if (d == DIALECT_ORACLE) w += (size_t)sprintf(buf + w, "INSERT ALL");
        else w += (size_t)sprintf(buf + w, "INSERT INTO %s VALUES ", qt);
        for (int i = s; i < s + batch && i < NROWS; i++) {
            const Row *r = &rows[i];
            char day[40]; if (d == DIALECT_ORACLE) snprintf(day, sizeof day, "DATE '%s'", r->day); else snprintf(day, sizeof day, "'%s'", r->day);
            if (d == DIALECT_ORACLE) w += (size_t)sprintf(buf + w, " INTO %s VALUES (%d, %s, '%s', '%s', %ld.%02ld, %ld.%02ld, %d, 'c%d@example.com')", qt, i + 1, day, r->region, r->status, r->gross_c / 100, r->gross_c % 100, r->net_c / 100, r->net_c % 100, r->customer, r->customer);
            else w += (size_t)sprintf(buf + w, "%s(%d, %s, '%s', '%s', %ld.%02ld, %ld.%02ld, %d, 'c%d@example.com')", i > s ? ", " : "", i + 1, day, r->region, r->status, r->gross_c / 100, r->gross_c % 100, r->net_c / 100, r->net_c % 100, r->customer, r->customer);
        }
        if (d == DIALECT_ORACLE) w += (size_t)sprintf(buf + w, " SELECT 1 FROM dual");
        if (!exec1(db, buf, err)) { free(buf); return; }
    }
    free(buf);
    if (d != DIALECT_MYSQL && d != DIALECT_MARIADB) exec1(db, "COMMIT", NULL);
}
/* runs the wrapped statements like the executor (stop on error, the rollback always) */
static bool run_wrapped(int d, Db *db, const char *sql, ResultSet *out, char *err) {
    char *st[6]; int res; bool fin; char note[300];
    int n = sqlguard_wrap(d, sql, 30, st, 6, &res, &fin, note, sizeof note);
    bool ok = true;
    for (int i = 0; i < n; i++) {
        if (!ok && !(fin && i == n - 1)) continue;
        ResultSet rs; rs_init(&rs);
        db->drv->exec(db, st[i], (int)strlen(st[i]), 0, &rs);
        if (!rs.ok && ok && i != n - 1) { ok = false; snprintf(err, 400, "%s", rs.error); }
        if (i == res && rs.ok) rs_move(out, &rs);
        rs_free(&rs);
    }
    sqlguard_free(st, n);
    return ok;
}
static int compare(const ResultSet *rs, int grain, bool region) {
    int bad = 0, seen = 0;
    for (int i = 0; i < rs->nrows; i++) {
        int k = 0; const char *p = grain >= 0 ? rs_cell(rs, i, k++) : ""; const char *dd = region ? rs_cell(rs, i, k++) : ""; const char *v = rs_cell(rs, i, k);
        bool found = false;
        for (int e = 0; e < nexp; e++) if (!strcmp(ex[e].p, p ? p : "") && !strcmp(ex[e].d, dd ? dd : "")) { found = true; seen++; if (!v || fabs(atof(v) - ex[e].v) > 0.005) { bad++; printf("      mismatch %s/%s got %s want %.4f\n", p, dd, v ? v : "NULL", ex[e].v); } }
        if (!found) { bad++; printf("      unexpected row %s/%s\n", p ? p : "", dd ? dd : ""); }
    }
    return bad + (nexp - seen);
}
static void engine(const char *env, const DbDriver *drv, int d, const char *name, const char *schema) {
    const char *loc = getenv(env);
    if (!loc || !*loc) { printf("skip  %s: %s not set (server not running or no credentials)\n", name, env); return; }
    char err[512] = "";
    Db *db = db_open(drv, loc, err, sizeof err);
    if (!db) { EXPECT(false, "%s: connect: %s", name, err); return; }
    if (d == DIALECT_PG) exec1(db, "CREATE SCHEMA IF NOT EXISTS lume_rpt", NULL);
    SemCatalog c; sem_init(&c); sem_parse(&c, CATALOG, strlen(CATALOG));
    snprintf(c.ds[0].schema, sizeof c.ds[0].schema, "%s", schema);
    char qs[80], qt[200], tb[80]; sem_quote_ident(d, schema, qs, sizeof qs); sem_quote_ident(d, "rpt_sales", tb, sizeof tb); snprintf(qt, sizeof qt, "%s.%s", qs, tb);
    err[0] = 0; load(d, db, qt, err);
    EXPECT(!err[0], "%s: golden dataset loaded (%d rows) %s", name, NROWS, err);
    /* Oracle: a read-only transaction whose snapshot predates the table's DDL fails with ORA-01466
     * (correct behaviour); let the SCN move past the just-created table first */
    if (d == DIALECT_ORACLE) sleep(3);
    if (err[0]) { db_close(db); return; }
    for (int g = 0; g < NG; g++) {
        const GQ *q = &GOLDEN[g];
        SemQuery sq = { q->metric, 0, q->dim, q->grain, { "", "" }, NULL, NULL, 0 };
        snprintf(sq.period.from, 11, "%s", q->from); snprintf(sq.period.to, 11, "%s", q->to);
        char sql[4096], why[300];
        if (!sem_compile(&c, &sq, d, sql, sizeof sql, why, sizeof why)) { EXPECT(false, "%s: compile %s: %s", name, q->what, why); continue; }
        expect_q(q->mi, q->grain, q->dim[0], q->from, q->to);
        ResultSet rs; rs_init(&rs); err[0] = 0;
        bool ok = sqlguard_check(sql, d, why, sizeof why) && run_wrapped(d, db, sql, &rs, err);
        int bad = ok ? compare(&rs, q->grain, q->dim[0]) : -1;
        EXPECT(ok && bad == 0, "%s: LI-096 %s = exact values (%d rows, read-only transaction) %s%s", name, q->what, rs.nrows, err, ok ? "" : why);
        if (!ok) printf("      %s\n", sql);
        rs_free(&rs);
    }
    /* layer 2: a write inside the wrapper (as if a volatile function slipped through) is refused by the engine */
    if (d != DIALECT_MSSQL) {
        char del[300]; char idc[40]; sem_quote_ident(d, "id", idc, sizeof idc);
        snprintf(del, sizeof del, "DELETE FROM %s WHERE %s = 1", qt, idc);
        char *st[6]; int res; bool fin; char note[300];
        int n = sqlguard_wrap(d, del, 30, st, 6, &res, &fin, note, sizeof note);
        bool refused = false;
        for (int i = 0; i < n; i++) { char e2[400] = ""; bool okx = exec1(db, st[i], e2); if (getenv("RPT_DEBUG")) printf("      [%s] %s -> %s\n", name, st[i], okx ? "ok" : e2); if (i == res && !okx) refused = true; }
        sqlguard_free(st, n);
        char cnt[300]; snprintf(cnt, sizeof cnt, "SELECT COUNT(*) FROM %s", qt);
        ResultSet rs; rs_init(&rs); db->drv->exec(db, cnt, (int)strlen(cnt), 0, &rs);
        bool intact = rs.ok && rs.nrows && atoi(rs_cell(&rs, 0, 0)) == NROWS;
        rs_free(&rs);
        EXPECT(refused && intact, "%s: LI-029 the engine's read-only transaction refuses a write that bypassed the lexical policy (rows intact)", name);
    } else printf("note  %s: no read-only transaction exists; the lexical policy and a read-only login are the protection\n", name);
    char drop[300]; snprintf(drop, sizeof drop, "DROP TABLE %s", qt); exec1(db, drop, NULL);
    db_close(db);
}

int main(void) {
    gen();
    engine("LUME_PG_CONN", &db_pg_driver, DIALECT_PG, "PostgreSQL", "lume_rpt");
    engine("LUME_MYSQL_LOC", &db_mysql_driver, DIALECT_MYSQL, "MySQL", "shop");
    engine("LUME_MARIADB_LOC", &db_mysql_driver, DIALECT_MARIADB, "MariaDB", "shop");
    engine("LUME_MSSQL_LOC", &db_mssql_driver, DIALECT_MSSQL, "SQL Server", "dbo");
    engine("LUME_ORA_LOC", &db_oracle_driver, DIALECT_ORACLE, "Oracle", "SYSTEM");
    printf("RPT DB: %s (%d ok, %d failures)\n", fails ? "FAILED" : "passed", oks, fails);
    return fails ? 1 : 0;
}
