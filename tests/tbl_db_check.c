/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer against real servers (TBL-050 / 055 / 056 / 057 / 074..079 / 098 / 099):
 * PostgreSQL, MySQL, MariaDB, SQL Server and Oracle, through Lume's own drivers.
 *
 * For each engine: create two tables from designer plans, put rows in, then modify (rename, add,
 * widen, default, index, check), re-read the catalog after every apply, detect drift from another
 * session, and run a plan whose second step fails: transactional engines roll everything back,
 * Oracle / MySQL / MariaDB keep step 1 and the plan says so (never a false rollback).
 * Locations come from the environment (never printed); test tables are dropped at the end.
 */
#include "tbl.h"
#include "db.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

extern const DbDriver db_pg_driver, db_mysql_driver, db_mssql_driver, db_oracle_driver;
static int fails, oks;
#define EXPECT(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static bool exec1(Db *db, const char *sql, char *err) {
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
    bool ok = rs.ok;
    if (!ok && err) snprintf(err, 400, "%s", rs.error);
    rs_free(&rs);
    return ok;
}
static void quiet(Db *db, const char *sql) { exec1(db, sql, NULL); }
static char *scalar(Db *db, const char *sql) {
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
    const char *v = rs.ok && rs.nrows ? rs_cell(&rs, 0, 0) : NULL;
    char *o = strdup(v ? v : "");
    rs_free(&rs);
    return o;
}
static bool snap(int d, Db *db, const char *schema, const char *name, TdTable *t, char *err) {
    TdSql q; td_snapshot_sql(d, schema, name, &q);
    ResultSet r[TD_SNAP_MAX]; ResultSet *rp[TD_SNAP_MAX];
    bool ok = true;
    for (int i = 0; i < q.n; i++) {
        rs_init(&r[i]);
        db->drv->exec(db, q.sql[i], (int)strlen(q.sql[i]), 0, &r[i]);
        rp[i] = &r[i];
        if (!r[i].ok) { snprintf(err, 400, "catalog statement %d: %s", i + 1, r[i].error); ok = false; }
    }
    if (ok) ok = td_snapshot_parse(d, schema, name, rp, q.n, t, err, 400);
    for (int i = 0; i < q.n; i++) rs_free(&r[i]);
    td_sql_free(&q);
    return ok;
}
/* the executor's contract (as the designer runs it): returns the number of steps that completed */
static int apply(int d, Db *db, const TdPlan *p, char *err, bool *rolled_back) {
    *rolled_back = false; err[0] = 0;
    if (p->transactional && !exec1(db, d == TD_MSSQL ? "BEGIN TRANSACTION" : "BEGIN", err)) return 0;
    int done = 0;
    for (int i = 0; i < p->n; i++) {
        char e[400];
        if (!exec1(db, p->steps[i].sql, e)) {
            snprintf(err, 400, "step %d: %s", i + 1, e);
            if (p->transactional) {
                *rolled_back = exec1(db, "ROLLBACK", NULL);
                /* SQL Server may have rolled back by itself (the error doomed the transaction): confirmed, not assumed */
                if (!*rolled_back && d == TD_MSSQL) { char *tc = scalar(db, "SELECT @@TRANCOUNT"); *rolled_back = !strcmp(tc, "0"); free(tc); }
            }
            return done;
        }
        done++;
    }
    if (p->transactional && !exec1(db, "COMMIT", err)) return 0;
    return done;
}
static const TdCol *col_named(const TdTable *t, const char *n) { for (int i = 0; i < t->ncols; i++) if (!strcasecmp(t->cols[i].name, n)) return &t->cols[i]; return NULL; }
static int keys_of(const TdTable *t, int kind) { int n = 0; for (int i = 0; i < t->nkeys; i++) n += t->keys[i].kind == kind; return n; }
static const char *U(int d, const char *s) { static char b[8][64]; static int k; k = (k + 1) % 8; snprintf(b[k], 64, "%s", s); if (d == TD_ORACLE) for (char *p = b[k]; *p; p++) *p = (char)toupper((unsigned char)*p); return b[k]; }
static const char *T(int d, const char *kind) {   /* a type per engine */
    if (!strcmp(kind, "id")) return d == TD_ORACLE ? "NUMBER(19)" : d == TD_SQLITE ? "INTEGER" : "bigint";
    if (!strcmp(kind, "money")) return d == TD_ORACLE ? "NUMBER(10,2)" : d == TD_MYSQL || d == TD_MARIADB || d == TD_MSSQL ? "decimal(10,2)" : "numeric(10,2)";
    if (!strcmp(kind, "money2")) return d == TD_ORACLE ? "NUMBER(14,2)" : d == TD_MYSQL || d == TD_MARIADB || d == TD_MSSQL ? "decimal(14,2)" : "numeric(14,2)";
    if (!strcmp(kind, "text")) return d == TD_ORACLE ? "VARCHAR2(24)" : d == TD_MSSQL ? "nvarchar(24)" : "varchar(24)";
    if (!strcmp(kind, "note")) return d == TD_ORACLE ? "VARCHAR2(200)" : d == TD_MSSQL ? "nvarchar(200)" : "varchar(200)";
    return "?";
}

static void scenario(int d, Db *db, Db *other, const char *schema) {
    const char *dn = td_dialect_name(d);
    char err[400] = "", sql[600], qo[400], qc[400];
    char ORD[64], CUS[64]; snprintf(ORD, sizeof ORD, "%s", U(d, "lume_td_orders")); snprintf(CUS, sizeof CUS, "%s", U(d, "lume_td_customers"));
    td_qname(d, schema, ORD, qo, sizeof qo); td_qname(d, schema, CUS, qc, sizeof qc);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qo); quiet(db, sql);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qc); quiet(db, sql);
    TdPlanOpts op; memset(&op, 0, sizeof op);
    char *ver = scalar(db, d == TD_PG ? "SHOW server_version_num" : d == TD_ORACLE ? "SELECT version FROM product_component_version WHERE ROWNUM = 1" : d == TD_MSSQL ? "SELECT CAST(SERVERPROPERTY('ProductVersion') AS nvarchar(64))" : "SELECT VERSION()");
    if (d == TD_PG) op.version = atoi(ver) / 10000 * 10000 + atoi(ver) % 10000 / 100 * 100;
    else { int a = 0, b = 0; sscanf(ver, "%d.%d", &a, &b); op.version = a * 10000 + b * 100; }
    free(ver);

    /* customers (id PK, name NOT NULL) */
    TdTable c; td_init(&c); snprintf(c.schema, sizeof c.schema, "%s", schema); snprintf(c.name, sizeof c.name, "%s", CUS);
    TdCol *cid = td_add_col(&c, U(d, "id"), T(d, "id")); cid->notnull = true;
    TdCol *cn = td_add_col(&c, U(d, "name"), T(d, "note")); cn->notnull = true;
    TdKey *cpk = td_add_key(&c, TK_PK, U(d, "lume_td_customers_pk")); cpk->cols[cpk->ncols++] = cid->id;
    TdPlan p; td_plan(d, NULL, &c, &op, &p);
    bool rb; int n = apply(d, db, &p, err, &rb);
    EXPECT(!p.blocked && n == p.n, "%s: CREATE customers from the designer plan (%d step(s)) %s%s", dn, p.n, err, p.block_reason);
    td_plan_free(&p);

    /* orders: identity PK, FK with ON DELETE CASCADE, default, index, comments */
    TdTable o; td_init(&o); snprintf(o.schema, sizeof o.schema, "%s", schema); snprintf(o.name, sizeof o.name, "%s", ORD);
    snprintf(o.comment, sizeof o.comment, "Orders of the designer test");
    TdCol *oid = td_add_col(&o, U(d, "id"), T(d, "id")); oid->notnull = true; oid->identity = 2;
    TdCol *ocu = td_add_col(&o, U(d, "customer_id"), T(d, "id")); ocu->notnull = true;
    TdCol *otot = td_add_col(&o, U(d, "total"), T(d, "money")); snprintf(otot->comment, sizeof otot->comment, "gross total");
    TdCol *ost = td_add_col(&o, U(d, "status"), T(d, "text")); snprintf(ost->def, sizeof ost->def, "'open'");
    TdKey *opk = td_add_key(&o, TK_PK, U(d, "lume_td_orders_pk")); opk->cols[opk->ncols++] = oid->id;
    TdKey *ofk = td_add_key(&o, TK_FK, U(d, "lume_td_orders_cust_fk")); ofk->cols[ofk->ncols++] = ocu->id;
    snprintf(ofk->ref_schema, sizeof ofk->ref_schema, "%s", schema); snprintf(ofk->ref_table, sizeof ofk->ref_table, "%s", CUS); snprintf(ofk->ref_cols[ofk->nref++], 128, "%s", U(d, "id"));
    snprintf(ofk->on_delete, sizeof ofk->on_delete, "CASCADE");
    TdKey *oix = td_add_key(&o, TK_INDEX, U(d, "lume_td_orders_status_ix")); oix->cols[oix->ncols++] = ost->id;
    td_plan(d, NULL, &o, &op, &p);
    n = apply(d, db, &p, err, &rb);
    EXPECT(!p.blocked && n == p.n, "%s: CREATE orders (identity, FK CASCADE, default, index, comments) %s%s", dn, err, p.block_reason);
    td_plan_free(&p);

    /* rows */
    char ins[600];
    snprintf(ins, sizeof ins, "INSERT INTO %s (%s, %s) VALUES (1, 'Ana')", qc, U(d, "id"), U(d, "name")); quiet(db, ins);
    snprintf(ins, sizeof ins, "INSERT INTO %s (%s, %s) VALUES (1, 10.5)", qo, U(d, "customer_id"), U(d, "total")); quiet(db, ins);
    snprintf(ins, sizeof ins, "INSERT INTO %s (%s, %s) VALUES (1, 20.25)", qo, U(d, "customer_id"), U(d, "total")); quiet(db, ins);
    if (d == TD_ORACLE) quiet(db, "COMMIT");

    /* snapshot (TBL-050) */
    TdTable s1; bool ok = snap(d, db, schema, ORD, &s1, err);
    const TdCol *sid = ok ? col_named(&s1, U(d, "id")) : NULL, *sst = ok ? col_named(&s1, U(d, "status")) : NULL, *stot = ok ? col_named(&s1, U(d, "total")) : NULL;
    if (!ok || s1.ncols != 4 || !sid || !sst || !strstr(sst->def, "open") || keys_of(&s1, TK_INDEX) != 1 || strcmp(s1.comment, "Orders of the designer test") || !stot || strcmp(stot->comment, "gross total")) {
        printf("      snapshot: ok=%d ncols=%d id_identity=%d status_def=[%s] pk=%d fk=%d ix=%d comment=[%s] total_comment=[%s]\n", ok, s1.ncols, sid ? sid->identity : -1, sst ? sst->def : "-",
               keys_of(&s1, TK_PK), keys_of(&s1, TK_FK), keys_of(&s1, TK_INDEX), s1.comment, stot ? stot->comment : "-");
    }
    EXPECT(ok && s1.ncols == 4 && sid && sid->identity && sst && strstr(sst->def, "open") && keys_of(&s1, TK_PK) == 1 && keys_of(&s1, TK_FK) == 1 && keys_of(&s1, TK_INDEX) == 1 && !strcmp(s1.comment, "Orders of the designer test") && stot && !strcmp(stot->comment, "gross total"),
           "%s: snapshot reads back identity, default, PK, FK, index and comments %s", dn, err);
    const TdKey *sfk = NULL; for (int i = 0; ok && i < s1.nkeys; i++) if (s1.keys[i].kind == TK_FK) sfk = &s1.keys[i];
    EXPECT(sfk && !strcmp(sfk->on_delete, "CASCADE") && !strcasecmp(sfk->ref_table, CUS) && sfk->nref == 1, "%s: the FK target and ON DELETE CASCADE are read back", dn);
    EXPECT(s1.can_alter && !s1.special[0], "%s: the session may alter it (privileges read from the catalog)", dn);

    /* modify: rename status -> state, widen total, add note, add a check, new default (TBL-002 / 051 / 055) */
    TdTable m; td_copy(&m, &s1);
    TdCol *mst = (TdCol *)col_named(&m, U(d, "status")); if (!mst) { EXPECT(false, "%s: no status column to modify", dn); td_free(&m); return; } snprintf(mst->name, sizeof mst->name, "%s", U(d, "state")); snprintf(mst->def, sizeof mst->def, "'new'");
    TdCol *mtot = (TdCol *)col_named(&m, U(d, "total")); snprintf(mtot->type, sizeof mtot->type, "%s", T(d, "money2"));
    td_add_col(&m, U(d, "note"), T(d, "note"));
    TdKey *ck = td_add_key(&m, TK_CHECK, U(d, "lume_td_orders_total_ck")); snprintf(ck->check, sizeof ck->check, "%s >= 0", U(d, "total"));
    td_plan(d, &s1, &m, &op, &p);
    bool rename_native = false; for (int i = 0; i < p.n; i++) rename_native |= strstr(p.steps[i].sql, "RENAME COLUMN") || strstr(p.steps[i].sql, "sp_rename");
    n = apply(d, db, &p, err, &rb);
    EXPECT(!p.blocked && n == p.n && rename_native, "%s: rename + widen + add + check + default applied in %d ordered step(s) %s%s", dn, p.n, err, p.block_reason);
    TdTable s2; ok = snap(d, db, schema, ORD, &s2, err);
    snprintf(sql, sizeof sql, "SELECT COUNT(*) FROM %s WHERE %s = 'open'", qo, U(d, "state"));
    char *cnt = scalar(db, sql);
    const TdCol *s2st = ok ? col_named(&s2, U(d, "state")) : NULL;
    EXPECT(ok && s2st && !col_named(&s2, U(d, "status")) && col_named(&s2, U(d, "note")) && keys_of(&s2, TK_CHECK) >= 1 && !strcmp(cnt, "2") && strstr(s2st->def, "new"),
           "%s: re-read after Apply: renamed (both rows kept, %s), note added, check present, new default %s", dn, cnt, err);
    free(cnt);
    td_plan_free(&p); td_free(&m);

    /* nothing left to do: an empty plan */
    td_plan(d, &s2, &s2, &op, &p);
    EXPECT(p.n == 0 && !p.blocked, "%s: the re-read table equals itself: no statement (snapshot is faithful)", dn);
    td_plan_free(&p);

    /* drift (TBL-057): another session changes the table after the snapshot */
    char alt[600]; snprintf(alt, sizeof alt, d == TD_ORACLE ? "ALTER TABLE %s ADD (%s NUMBER)" : d == TD_MSSQL ? "ALTER TABLE %s ADD %s int" : "ALTER TABLE %s ADD %s int", qo, U(d, "x_drift"));
    bool okd = exec1(other ? other : db, alt, err);
    TdTable s3; snap(d, db, schema, ORD, &s3, err);
    EXPECT(okd && s3.fingerprint != s2.fingerprint && col_named(&s3, U(d, "x_drift")), "%s: a change from another session changes the fingerprint (Apply is refused until Reload)", dn);

    /* a plan whose second step fails (TBL-056 / 099, Gherkin "Oracle com falha parcial") */
    m.ncols = 0; td_copy(&m, &s3);
    td_add_col(&m, U(d, "step_one"), T(d, "note"));
    TdKey *bad = td_add_key(&m, TK_CHECK, U(d, "lume_td_never")); snprintf(bad->check, sizeof bad->check, "%s < 0", U(d, "total"));   /* existing rows violate it: step 2 fails */
    td_plan(d, &s3, &m, &op, &p);
    n = apply(d, db, &p, err, &rb);
    TdTable s4; snap(d, db, schema, ORD, &s4, err);
    bool one = col_named(&s4, U(d, "step_one")) != NULL, two = false; for (int i = 0; i < s4.nkeys; i++) two |= !strcasecmp(s4.keys[i].name, U(d, "lume_td_never"));
    if (!(n == 1 && (p.transactional ? rb && !one && !two : one && !two))) printf("      partial: n=%d steps=%d rolled_back=%d one=%d two=%d err=%s\n", n, p.n, rb, one, two, err);
    if (p.transactional) EXPECT(n == 1 && rb && !one && !two && p.rollback == TR_TRANSACTIONAL, "%s: step 2 failed inside the transaction: ROLLBACK, nothing applied", dn);
    else EXPECT(n == 1 && one && !two && p.rollback != TR_TRANSACTIONAL && (p.flags & TS_IMPLICIT_COMMIT), "%s: step 2 failed: step 1 stays applied and the plan never promised a rollback", dn);
    td_plan_free(&p); td_free(&m);

    /* drop a column (destructive) */
    td_copy(&m, &s4); if (col_named(&m, U(d, "note"))) td_del_col(&m, col_named(&m, U(d, "note"))->id);
    td_plan(d, &s4, &m, &op, &p);
    bool destr = (p.flags & TS_DESTRUCTIVE) != 0;
    n = apply(d, db, &p, err, &rb);
    TdTable s5; snap(d, db, schema, ORD, &s5, err);
    EXPECT(destr && n == p.n && !col_named(&s5, U(d, "note")), "%s: DROP COLUMN flagged destructive and applied %s", dn, err);
    td_plan_free(&p); td_free(&m);

    td_free(&c); td_free(&o); td_free(&s1); td_free(&s2); td_free(&s3); td_free(&s4); td_free(&s5);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qo); quiet(db, sql);
    snprintf(sql, sizeof sql, "DROP TABLE %s", qc); quiet(db, sql);
}

static void engine(const char *env, const DbDriver *drv, int d, const char *schema) {
    const char *loc = getenv(env);
    if (!loc || !*loc) { printf("skip  %s: %s not set\n", td_dialect_name(d), env); return; }
    char err[512] = "";
    Db *db = db_open(drv, loc, err, sizeof err), *other = db ? db_open(drv, loc, err, sizeof err) : NULL;
    if (!db) { EXPECT(false, "%s: connect: %s", td_dialect_name(d), err); return; }
    if (d == TD_PG) quiet(db, "CREATE SCHEMA IF NOT EXISTS lume_td");
    scenario(d, db, other, schema);
    db_close(db); if (other) db_close(other);
}

int main(void) {
    engine("LUME_PG_CONN", &db_pg_driver, TD_PG, "lume_td");
    engine("LUME_MYSQL_LOC", &db_mysql_driver, TD_MYSQL, "shop");
    engine("LUME_MARIADB_LOC", &db_mysql_driver, TD_MARIADB, "shop");
    engine("LUME_MSSQL_LOC", &db_mssql_driver, TD_MSSQL, "dbo");
    engine("LUME_ORA_LOC", &db_oracle_driver, TD_ORACLE, "SYSTEM");
    printf("TBL DB: %s (%d ok, %d failures)\n", fails ? "FAILED" : "passed", oks, fails);
    return fails ? 1 : 0;
}
