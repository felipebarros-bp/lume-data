/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer core, headless (TBL-011..081, 098 / 099): golden DDL per dialect, quoting,
 * semantic diff (rename is RENAME), validation, risk flags, rollback classes, draft files, and
 * SQLite end to end in process: snapshot -> plan -> run (native ALTER and the rebuild with an
 * index, a trigger and a foreign key, then foreign_key_check).
 */
#include "tbl.h"
#include <sqlite3.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static int fails, oks;
#define EXPECT(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); } while (0)

static bool has(const TdPlan *p, const char *needle) { for (int i = 0; i < p->n; i++) if (strstr(p->steps[i].sql, needle)) return true; return false; }
static int idx(const TdPlan *p, const char *needle) { for (int i = 0; i < p->n; i++) if (strstr(p->steps[i].sql, needle)) return i; return -1; }
static bool warned(const TdPlan *p, const char *needle) { for (int i = 0; i < p->nwarn; i++) if (strstr(p->warnings[i], needle)) return true; return false; }

/* orders(id bigint PK, customer_id bigint FK customers(id), total numeric(10,2), status varchar(24) default 'open', created_at timestamp) */
static void orders(TdTable *t, int d) {
    td_init(t);
    snprintf(t->schema, sizeof t->schema, "%s", d == TD_ORACLE ? "APP" : d == TD_MSSQL ? "dbo" : d == TD_SQLITE ? "main" : "public");
    snprintf(t->name, sizeof t->name, "orders");
    TdCol *id = td_add_col(t, "id", d == TD_ORACLE ? "NUMBER(19)" : d == TD_SQLITE ? "INTEGER" : "bigint"); id->notnull = true;
    TdCol *cu = td_add_col(t, "customer_id", d == TD_ORACLE ? "NUMBER(19)" : d == TD_SQLITE ? "INTEGER" : "bigint"); cu->notnull = true;
    TdCol *to = td_add_col(t, "total", d == TD_ORACLE ? "NUMBER(10,2)" : d == TD_SQLITE ? "NUMERIC" : "numeric(10,2)");
    TdCol *st = td_add_col(t, "status", d == TD_ORACLE ? "VARCHAR2(24)" : "varchar(24)"); snprintf(st->def, sizeof st->def, "'open'");
    td_add_col(t, "created_at", d == TD_MSSQL ? "datetime2" : d == TD_ORACLE ? "TIMESTAMP" : "timestamp");
    TdKey *pk = td_add_key(t, TK_PK, "orders_pkey"); pk->cols[pk->ncols++] = id->id;
    TdKey *fk = td_add_key(t, TK_FK, "orders_customer_fk"); fk->cols[fk->ncols++] = cu->id;
    snprintf(fk->ref_schema, sizeof fk->ref_schema, "%s", t->schema); snprintf(fk->ref_table, sizeof fk->ref_table, "customers"); snprintf(fk->ref_cols[fk->nref++], 128, "id");
    snprintf(fk->on_delete, sizeof fk->on_delete, "CASCADE");
    TdKey *ix = td_add_key(t, TK_INDEX, "orders_status_idx"); ix->cols[ix->ncols++] = st->id;
    (void)to;
}

/* ---- SQLite in process ---- */
static sqlite3 *db;
static void rs_from(sqlite3_stmt *st, ResultSet *rs) {
    rs_init(rs);
    int nc = sqlite3_column_count(st);
    for (int c = 0; c < nc; c++) rs_add_column(rs, sqlite3_column_name(st, c), "", 0);
    while (sqlite3_step(st) == SQLITE_ROW) { int r = rs_add_row(rs); for (int c = 0; c < nc; c++) rs_set(rs, r, c, (const char *)sqlite3_column_text(st, c), -1); }
    rs->ok = rs->has_rows = true;
}
static bool snap(const char *name, TdTable *t, char *err) {
    TdSql q; td_snapshot_sql(TD_SQLITE, "main", name, &q);
    ResultSet r[TD_SNAP_MAX]; ResultSet *rp[TD_SNAP_MAX];
    for (int i = 0; i < q.n; i++) { sqlite3_stmt *st; if (sqlite3_prepare_v2(db, q.sql[i], -1, &st, NULL) != SQLITE_OK) { snprintf(err, 300, "prepare %d: %s", i, sqlite3_errmsg(db)); td_sql_free(&q); return false; } rs_from(st, &r[i]); sqlite3_finalize(st); rp[i] = &r[i]; }
    bool ok = td_snapshot_parse(TD_SQLITE, "main", name, rp, q.n, t, err, 300);
    for (int i = 0; i < q.n; i++) rs_free(&r[i]);
    td_sql_free(&q);
    return ok;
}
static int count(const char *sql) { sqlite3_stmt *st; int n = -1; if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) { n = 0; while (sqlite3_step(st) == SQLITE_ROW) n = sqlite3_column_int(st, 0) + 0 * n; } sqlite3_finalize(st); return n; }
static int rows(const char *sql) { sqlite3_stmt *st; int n = 0; if (sqlite3_prepare_v2(db, sql, -1, &st, NULL) == SQLITE_OK) while (sqlite3_step(st) == SQLITE_ROW) n++; sqlite3_finalize(st); return n; }
/* the executor's contract: steps in order, inside BEGIN / COMMIT when transactional, ROLLBACK on failure; a
 * TS_DATA_CHECK pragma that returns rows fails the plan */
static bool run(const TdPlan *p, char *err) {
    if (p->fk_toggle) sqlite3_exec(db, "PRAGMA foreign_keys = OFF", NULL, NULL, NULL);
    if (p->transactional) sqlite3_exec(db, "BEGIN", NULL, NULL, NULL);
    for (int i = 0; i < p->n; i++) {
        if ((p->steps[i].flags & TS_DATA_CHECK) && !strncmp(p->steps[i].sql, "PRAGMA", 6)) {
            if (rows(p->steps[i].sql) > 0) { snprintf(err, 300, "step %d: foreign_key_check returned rows", i + 1); goto fail; }
            continue;
        }
        char *e = NULL;
        if (sqlite3_exec(db, p->steps[i].sql, NULL, NULL, &e) != SQLITE_OK) { snprintf(err, 300, "step %d: %s", i + 1, e ? e : "?"); sqlite3_free(e); goto fail; }
    }
    if (p->transactional) sqlite3_exec(db, "COMMIT", NULL, NULL, NULL);
    if (p->fk_toggle) sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
    return true;
fail:
    if (p->transactional) sqlite3_exec(db, "ROLLBACK", NULL, NULL, NULL);
    if (p->fk_toggle) sqlite3_exec(db, "PRAGMA foreign_keys = ON", NULL, NULL, NULL);
    return false;
}

int main(void) {
    TdTable t, o, d2; TdPlan p; TdPlanOpts op; memset(&op, 0, sizeof op);
    char *s;

    /* ---- CREATE golden (TBL-001 / 026 / 029 / 033 / 052 / 065) ---- */
    orders(&t, TD_PG);
    td_plan(TD_PG, NULL, &t, &op, &p);
    s = td_plan_script(TD_PG, &p);
    EXPECT(!p.blocked && strstr(s, "CREATE TABLE \"public\".\"orders\" (\n    \"id\" bigint NOT NULL,\n    \"customer_id\" bigint NOT NULL,\n    \"total\" numeric(10,2),\n    \"status\" varchar(24) DEFAULT 'open',\n    \"created_at\" timestamp,\n"
                                   "    CONSTRAINT \"orders_pkey\" PRIMARY KEY (\"id\"),\n    CONSTRAINT \"orders_customer_fk\" FOREIGN KEY (\"customer_id\") REFERENCES \"public\".\"customers\" (\"id\") ON DELETE CASCADE\n)"),
           "TBL-001 / 052 (PG): CREATE TABLE qualified, quoted, PK + FK + default:\n%s", s);
    EXPECT(strstr(s, "CREATE INDEX \"orders_status_idx\" ON \"public\".\"orders\" (\"status\")") && strstr(s, "BEGIN;") && strstr(s, "COMMIT;") && p.rollback == TR_TRANSACTIONAL, "TBL-033 / 060 (PG): index after the table, one transaction, transactionally reversible");
    free(s); td_plan_free(&p); td_free(&t);
    orders(&t, TD_MYSQL);
    snprintf(t.engine, sizeof t.engine, "InnoDB"); snprintf(t.comment, sizeof t.comment, "it's orders");
    td_plan(TD_MYSQL, NULL, &t, &op, &p); s = td_plan_script(TD_MYSQL, &p);
    EXPECT(strstr(s, "CREATE TABLE `public`.`orders` (") && strstr(s, "`status` varchar(24) DEFAULT 'open'") && strstr(s, ") ENGINE = InnoDB COMMENT = 'it''s orders'") && !strstr(s, "BEGIN") && p.rollback == TR_COMPENSATE && strstr(p.compensate, "DROP TABLE `public`.`orders`"),
           "TBL-046 / 060 / 077 (MySQL): backticks, ENGINE, COMMENT escaped; no fake transaction; DROP TABLE proposed as compensation");
    EXPECT(warned(&p, "commits implicitly"), "TBL-077: implicit commit stated in the review");
    free(s); td_plan_free(&p); td_free(&t);
    orders(&t, TD_MSSQL);
    td_col(&t, 1)->identity = 2; snprintf(td_col(&t, 4)->comment, 512, "estado");
    td_plan(TD_MSSQL, NULL, &t, &op, &p); s = td_plan_script(TD_MSSQL, &p);
    EXPECT(strstr(s, "CREATE TABLE [dbo].[orders] (\n    [id] bigint IDENTITY(1,1) NOT NULL") && strstr(s, "BEGIN TRANSACTION;") && strstr(s, "EXEC sys.sp_addextendedproperty @name = N'MS_Description', @value = N'estado'") && strstr(s, "@level2name = N'status'"),
           "TBL-019 / 021 / 079 (SQL Server): brackets, IDENTITY, MS_Description, transaction");
    free(s); td_plan_free(&p); td_free(&t);
    orders(&t, TD_ORACLE);
    td_col(&t, 1)->identity = 1;
    td_plan(TD_ORACLE, NULL, &t, &op, &p); s = td_plan_script(TD_ORACLE, &p);
    EXPECT(strstr(s, "CREATE TABLE \"APP\".\"orders\" (\n    \"id\" NUMBER(19) GENERATED ALWAYS AS IDENTITY NOT NULL") && strstr(s, "CREATE INDEX \"APP\".\"orders_status_idx\" ON \"APP\".\"orders\"") && p.rollback != TR_TRANSACTIONAL,
           "TBL-076 (Oracle): identity, schema on the index name, never transactional");
    EXPECT(warned(&p, "Oracle commits before and after every DDL statement"), "TBL-076: the review says Oracle DDL commits implicitly");
    free(s); td_plan_free(&p); td_free(&t);

    /* ---- ALTER: rename is RENAME (TBL-051 / Gherkin) ---- */
    orders(&o, TD_PG); o.exists = true;
    td_copy(&t, &o);
    snprintf(td_col(&t, 4)->name, 128, "state");
    TdChange ch[32]; int nc = td_diff(&o, &t, ch, 32);
    EXPECT(nc == 1 && ch[0].kind == TC_RENAME_COL && strstr(ch[0].text, "status -> state"), "TBL-051: the diff says RENAME_COLUMN status -> state");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(p.n == 1 && !strcmp(p.steps[0].sql, "ALTER TABLE \"public\".\"orders\" RENAME COLUMN \"status\" TO \"state\"") && !has(&p, "DROP COLUMN") && !has(&p, "ADD COLUMN"), "Gherkin: rename without DROP + ADD");
    td_plan_free(&p); td_free(&t);

    /* narrowing: numeric(12,2) -> numeric(3,0) (Gherkin "Rejeitar alteração perigosa") */
    td_copy(&t, &o);
    snprintf(td_col(&t, 3)->type, 128, "numeric(3,0)");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(!p.blocked && (p.flags & TS_DESTRUCTIVE) && p.risk == RISK_HIGH && strstr(p.steps[0].why, "less precision or scale"), "TBL-054 / 069: numeric(10,2) -> numeric(3,0) is destructive, high risk: %s", p.n ? p.steps[0].why : "");
    TdIssue is[32]; int ni = td_validate(TD_PG, 0, &o, &t, is, 32);
    bool w1 = false; for (int i = 0; i < ni; i++) w1 |= is[i].level == TV_WARNING && strstr(is[i].text, "values may fail or be rounded");
    EXPECT(w1, "TBL-069: validation warns of loss before the review");
    td_plan_free(&p); td_free(&t);

    /* incompatible type without USING blocks; with USING it plans */
    td_copy(&t, &o);
    snprintf(td_col(&t, 4)->type, 128, "integer");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(p.blocked && strstr(p.block_reason, "USING expression"), "TBL-075 / 069: varchar -> integer asks for an explicit USING: %s", p.block_reason);
    td_plan_free(&p);
    op.nusing = 1; op.using_col[0] = 4; snprintf(op.using_expr[0], 256, "\"status\"::integer");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(!p.blocked && has(&p, "ALTER COLUMN \"status\" TYPE integer USING \"status\"::integer"), "TBL-075: USING appears when the conversion needs it");
    int i1 = idx(&p, "ALTER COLUMN \"status\" DROP DEFAULT"), i2 = idx(&p, "TYPE integer"), i3 = idx(&p, "ALTER COLUMN \"status\" SET DEFAULT 'open'");
    EXPECT(i1 >= 0 && i2 > i1 && i3 > i2, "TBL-075 (found live): the old default is dropped before the type change and set again after (PostgreSQL casts it with the column)");
    td_plan_free(&p); td_free(&t); memset(&op, 0, sizeof op);

    /* drop a column used by the FK: the FK goes first, the drop is destructive (TBL-013 / 037 / 055) */
    td_copy(&t, &o);
    td_del_col(&t, 2);
    td_plan(TD_PG, &o, &t, &op, &p);
    int ifk = idx(&p, "DROP CONSTRAINT \"orders_customer_fk\""), icol = idx(&p, "DROP COLUMN \"customer_id\"");
    EXPECT(ifk >= 0 && icol > ifk && (p.steps[icol].flags & TS_DESTRUCTIVE) && !has(&p, "CASCADE\n") && !has(&p, "DROP COLUMN \"customer_id\" CASCADE"), "TBL-013 / 037 / 055: FK dropped before its column; destructive; never CASCADE");
    td_plan_free(&p); td_free(&t);

    /* NOT NULL on an existing column + new unique: data checks (TBL-015 / 028 / 068) */
    td_copy(&t, &o);
    td_col(&t, 3)->notnull = true;
    TdKey *u = td_add_key(&t, TK_UNIQUE, "orders_total_key"); u->cols[u->ncols++] = 3;
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(has(&p, "ALTER COLUMN \"total\" SET NOT NULL") && has(&p, "ADD CONSTRAINT \"orders_total_key\" UNIQUE (\"total\")") && (p.flags & TS_DATA_CHECK) && warned(&p, "ACCESS EXCLUSIVE"), "TBL-015 / 028 / 075: NOT NULL and UNIQUE flagged as data checks; PG lock stated");
    td_plan_free(&p); td_free(&t);

    /* lock_timeout (TBL-070) */
    td_copy(&t, &o); snprintf(td_col(&t, 5)->comment, 512, "when");
    snprintf(op.lock_timeout, sizeof op.lock_timeout, "5s");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(p.n == 2 && !strcmp(p.steps[0].sql, "SET LOCAL lock_timeout = '5s'") && !strcmp(p.steps[1].sql, "COMMENT ON COLUMN \"public\".\"orders\".\"created_at\" IS 'when'") && p.risk == RISK_LOW, "TBL-021 / 070: comment alone = one COMMENT (no rebuild), lock_timeout first");
    td_plan_free(&p); td_free(&t); memset(&op, 0, sizeof op);

    /* rename an index and a constraint natively (TBL-036) */
    td_copy(&t, &o);
    snprintf(td_key(&t, 8)->name, 128, "ix_orders_status"); snprintf(td_key(&t, 7)->name, 128, "fk_orders_customer");
    td_plan(TD_PG, &o, &t, &op, &p);
    EXPECT(has(&p, "ALTER INDEX \"public\".\"orders_status_idx\" RENAME TO \"ix_orders_status\"") && has(&p, "RENAME CONSTRAINT \"orders_customer_fk\" TO \"fk_orders_customer\"") && !has(&p, "DROP"), "TBL-036 (PG): renames are renames");
    td_plan_free(&p); td_free(&t);

    /* MySQL: MODIFY restates the column with its unmodelled attributes (TBL-081) */
    td_free(&o); orders(&o, TD_MYSQL); o.exists = true; snprintf(td_col(&o, 5)->extra, 256, "ON UPDATE CURRENT_TIMESTAMP");
    td_copy(&t, &o); snprintf(td_col(&t, 5)->type, 128, "datetime");
    td_plan(TD_MYSQL, &o, &t, &op, &p);
    EXPECT(has(&p, "MODIFY COLUMN `created_at` datetime ON UPDATE CURRENT_TIMESTAMP") && (p.flags & TS_IMPLICIT_COMMIT), "TBL-081 (MySQL): MODIFY keeps ON UPDATE CURRENT_TIMESTAMP");
    td_plan_free(&p); td_free(&t);
    td_copy(&t, &o); snprintf(td_key(&t, 7)->name, 128, "fk2");
    td_plan(TD_MYSQL, &o, &t, &op, &p);
    EXPECT(has(&p, "DROP FOREIGN KEY `orders_customer_fk`") && has(&p, "ADD CONSTRAINT `fk2` FOREIGN KEY"), "TBL-036 (MySQL): a foreign key cannot be renamed: drop + add, said so");
    td_plan_free(&p); td_free(&t);
    op.version = 50700; td_copy(&t, &o); snprintf(td_col(&t, 4)->name, 128, "state");
    td_plan(TD_MYSQL, &o, &t, &op, &p);
    EXPECT(p.blocked && strstr(p.block_reason, "Renaming a column is not supported"), "TBL-080: MySQL 5.7 has no RENAME COLUMN: blocked, never DROP + ADD: %s", p.block_reason);
    td_plan_free(&p); td_free(&t); memset(&op, 0, sizeof op);
    td_copy(&t, &o); snprintf(td_col(&t, 4)->name, 128, "state");
    td_plan(TD_MYSQL, &o, &t, &op, &p);
    EXPECT(p.blocked && strstr(p.block_reason, "not confirmed"), "TBL-080: unknown version = unknown capability, not a guess");
    td_plan_free(&p); td_free(&t);

    /* SQL Server: DEFAULT constraint dropped and recreated around a type change (TBL-079) */
    td_free(&o); orders(&o, TD_MSSQL); o.exists = true; snprintf(td_col(&o, 4)->def_name, 128, "DF_orders_status");
    td_copy(&t, &o); snprintf(td_col(&t, 4)->type, 128, "varchar(40)");
    td_plan(TD_MSSQL, &o, &t, &op, &p);
    int a1 = idx(&p, "DROP CONSTRAINT [DF_orders_status]"), a2 = idx(&p, "ALTER COLUMN [status] varchar(40) NULL"), a3 = idx(&p, "ADD CONSTRAINT [DF_orders_status] DEFAULT 'open' FOR [status]");
    EXPECT(a1 >= 0 && a2 > a1 && a3 > a2 && warned(&p, "Sch-M"), "TBL-079 (SQL Server): DEFAULT constraint dropped, column altered, default restored; Sch-M lock stated");
    td_plan_free(&p); td_free(&t);
    td_copy(&t, &o); snprintf(td_col(&t, 4)->name, 128, "state");
    td_plan(TD_MSSQL, &o, &t, &op, &p);
    EXPECT(p.n == 1 && !strcmp(p.steps[0].sql, "EXEC sp_rename N'dbo.orders.status', N'state', N'COLUMN'"), "TBL-079: column rename with sp_rename");
    td_plan_free(&p); td_free(&t);

    /* Oracle: MODIFY and the commit warning (TBL-076) */
    td_free(&o); orders(&o, TD_ORACLE); o.exists = true;
    td_copy(&t, &o); snprintf(td_col(&t, 4)->type, 128, "VARCHAR2(40)"); td_col(&t, 4)->notnull = true; td_add_col(&t, "note", "VARCHAR2(200)");
    td_plan(TD_ORACLE, &o, &t, &op, &p);
    EXPECT(has(&p, "ALTER TABLE \"APP\".\"orders\" MODIFY (\"status\" VARCHAR2(40) NOT NULL)") && has(&p, "ALTER TABLE \"APP\".\"orders\" ADD (\"note\" VARCHAR2(200))") && p.risk == RISK_HIGH && p.rollback == TR_COMPENSATE && strstr(p.compensate, "DROP COLUMN \"note\""),
           "TBL-060 / 076 (Oracle): MODIFY / ADD; several commits = high risk; compensating script proposed, not run");
    td_plan_free(&p); td_free(&t);

    /* validation (TBL-065 / 066) */
    td_free(&o); orders(&o, TD_PG);
    td_add_col(&o, "status", "text");
    TdKey *k2 = td_add_key(&o, TK_PK, "second_pk"); k2->cols[k2->ncols++] = 2;
    TdKey *fk2 = td_add_key(&o, TK_FK, "bad_fk"); fk2->cols[fk2->ncols++] = 1; fk2->cols[fk2->ncols++] = 2; snprintf(fk2->ref_table, 128, "x"); snprintf(fk2->ref_cols[fk2->nref++], 128, "id");
    TdKey *ck = td_add_key(&o, TK_CHECK, "c1"); snprintf(ck->check, sizeof ck->check, "total > 0; DROP TABLE x");
    char longname[80]; memset(longname, 'a', 70); longname[70] = 0; td_add_col(&o, longname, "int");
    ni = td_validate(TD_PG, 0, NULL, &o, is, 32);
    int dup = 0, pk2 = 0, fkc = 0, semi = 0, len = 0;
    for (int i = 0; i < ni; i++) { dup |= !!strstr(is[i].text, "Two columns are named status"); pk2 |= !!strstr(is[i].text, "Only one primary key"); fkc |= !!strstr(is[i].text, "as many referenced columns"); semi |= !!strstr(is[i].text, "';' would end"); len |= !!strstr(is[i].text, "longer than 63"); }
    EXPECT(dup && pk2 && fkc && semi && len, "TBL-065 / 066: duplicate name, two PKs, FK column count, ';' in CHECK, PG 63-char limit");
    td_plan(TD_PG, NULL, &o, &op, &p);
    EXPECT(p.blocked && p.n == 0, "TBL-052.3: an invalid draft has no SQL and Apply is disabled");
    td_plan_free(&p); td_free(&o);

    /* draft file round trip (TBL-009) */
    orders(&o, TD_PG); snprintf(o.comment, sizeof o.comment, "line1\nline2 \\ back");
    char *ser = td_serialize(&o);
    EXPECT(td_deserialize(ser, &d2) && td_equal(&o, &d2) && d2.next_id == o.next_id && !strstr(ser, "password"), "TBL-009: draft file round trip (no credentials)");
    free(ser); td_free(&d2); td_free(&o);

    /* ---- SQLite in process (TBL-074 / 099) ---- */
    sqlite3_open(":memory:", &db);
    sqlite3_exec(db, "PRAGMA foreign_keys = ON;"
                     "CREATE TABLE customers (id INTEGER PRIMARY KEY, name TEXT NOT NULL);"
                     "CREATE TABLE orders (id INTEGER PRIMARY KEY AUTOINCREMENT, customer_id INTEGER NOT NULL REFERENCES customers(id) ON DELETE CASCADE, total NUMERIC DEFAULT 0, "
                     "status TEXT DEFAULT 'open' COLLATE NOCASE, CONSTRAINT positive CHECK (total >= 0));"
                     "CREATE INDEX orders_status_idx ON orders(status);"
                     "CREATE INDEX orders_open ON orders(customer_id) WHERE status = 'open';"
                     "CREATE TABLE audit (msg TEXT);"
                     "CREATE TRIGGER orders_ins AFTER INSERT ON orders BEGIN INSERT INTO audit VALUES ('new ' || NEW.id); END;"
                     "INSERT INTO customers VALUES (1, 'Ana'), (2, 'Bia');"
                     "INSERT INTO orders (customer_id, total, status) VALUES (1, 10, 'open'), (2, 20, 'paid');", NULL, NULL, NULL);
    char err[300] = "";
    bool ok = snap("orders", &o, err);
    TdKey *pk = td_pk(&o);
    int nck = 0, nfk = 0, nix = 0; const TdKey *partial = NULL;
    for (int i = 0; i < o.nkeys; i++) { nck += o.keys[i].kind == TK_CHECK; nfk += o.keys[i].kind == TK_FK; if (o.keys[i].kind == TK_INDEX) { nix++; if (o.keys[i].where[0]) partial = &o.keys[i]; } }
    EXPECT(ok && o.ncols == 4 && pk && pk->ncols == 1 && td_col(&o, pk->cols[0])->identity && nck == 1 && nfk == 1 && nix == 2 && partial && strstr(partial->where, "status = 'open'"),
           "TBL-050 (SQLite): snapshot with AUTOINCREMENT PK, FK, named CHECK, two indexes (one partial) %s", err);
    EXPECT(!strcmp(td_col(&o, 4)->collation, "NOCASE") && !strcmp(td_col(&o, 4)->def, "'open'") && strstr(o.triggers, "CREATE TRIGGER orders_ins"), "TBL-050: collation, default text and the trigger SQL are captured");
    /* native: add a nullable column + rename a column (SQLite >= 3.25) */
    op.version = sqlite3_libversion_number() / 1000 * 100 + sqlite3_libversion_number() % 1000 / 1000;
    op.version = (sqlite3_libversion_number() / 1000000) * 10000 + (sqlite3_libversion_number() / 1000 % 1000) * 100;
    td_copy(&t, &o); td_add_col(&t, "note", "TEXT"); snprintf(td_col(&t, 3)->name, 128, "amount");
    td_plan(TD_SQLITE, &o, &t, &op, &p);
    EXPECT(!p.rebuild && p.n == 2 && has(&p, "RENAME COLUMN \"total\" TO \"amount\"") && has(&p, "ADD COLUMN \"note\" TEXT"), "TBL-074: rename + add are native ALTERs (no rebuild)");
    EXPECT(run(&p, err) && count("SELECT count(*) FROM orders WHERE amount = 20") == 1, "TBL-074: applied; data kept %s", err);
    td_plan_free(&p); td_free(&t); td_free(&o);
    /* rebuild: change a type + NOT NULL, drop the CHECK; the index, the trigger and the FK survive */
    ok = snap("orders", &o, err);
    td_copy(&t, &o);
    TdCol *am = NULL; for (int i = 0; i < t.ncols; i++) if (!strcmp(t.cols[i].name, "amount")) am = &t.cols[i];
    snprintf(am->type, 128, "REAL"); am->notnull = true;
    for (int i = 0; i < t.nkeys; i++) if (t.keys[i].kind == TK_CHECK) { td_del_key(&t, t.keys[i].id); break; }
    op.sqlite_fk_on = true;
    td_plan(TD_SQLITE, &o, &t, &op, &p);
    s = td_plan_script(TD_SQLITE, &p);
    EXPECT(!p.blocked && p.rebuild && p.fk_toggle && strstr(s, "PRAGMA foreign_keys = OFF;\nBEGIN;\nCREATE TABLE \"main\".\"_lume_new_orders\"") && strstr(s, "INSERT INTO \"main\".\"_lume_new_orders\"") && strstr(s, "DROP TABLE \"main\".\"orders\"") &&
           strstr(s, "RENAME TO \"orders\"") && strstr(s, "CREATE INDEX \"main\".\"orders_open\" ON \"orders\" (\"customer_id\") WHERE status = 'open'") && strstr(s, "CREATE TRIGGER orders_ins") && strstr(s, "foreign_key_check"),
           "TBL-074 / Gherkin 'SQLite com rebuild': the copy-and-rebuild plan is shown:\n%s", s);
    free(s);
    EXPECT(run(&p, err), "TBL-074: rebuild applied %s", err);
    TdTable after; snap("orders", &after, err);
    int ix2 = 0, ck2 = 0; for (int i = 0; i < after.nkeys; i++) { ix2 += after.keys[i].kind == TK_INDEX; ck2 += after.keys[i].kind == TK_CHECK; }
    EXPECT(count("SELECT count(*) FROM orders") == 2 && ix2 == 2 && ck2 == 0 && strstr(after.triggers, "orders_ins") && td_pk(&after) && rows("PRAGMA foreign_key_list(orders)") == 1 && rows("PRAGMA integrity_check") == 1,
           "TBL-099: after the rebuild: rows, 2 indexes, trigger, FK kept; CHECK gone; integrity ok");
    int aud = count("SELECT count(*) FROM audit");
    sqlite3_exec(db, "INSERT INTO orders (customer_id, amount, status) VALUES (1, 5, 'x')", NULL, NULL, NULL);
    EXPECT(count("SELECT count(*) FROM audit") == aud + 1, "TBL-099: the recreated trigger fires");
    td_plan_free(&p); td_free(&t); td_free(&after);
    /* a rebuild that would break a FK: foreign_key_check fails, everything rolls back */
    sqlite3_exec(db, "PRAGMA foreign_keys = OFF; INSERT INTO orders (customer_id, amount, status) VALUES (99, 1, 'orphan'); PRAGMA foreign_keys = ON;", NULL, NULL, NULL);
    td_free(&o); snap("orders", &o, err);
    td_copy(&t, &o); for (int i = 0; i < t.ncols; i++) if (!strcmp(t.cols[i].name, "note")) snprintf(t.cols[i].type, 128, "VARCHAR(10)");
    td_plan(TD_SQLITE, &o, &t, &op, &p);
    int before = count("SELECT count(*) FROM sqlite_master");
    bool r2 = run(&p, err);
    TdTable again; snap("orders", &again, err);
    EXPECT(!r2 && strstr(err, "foreign_key_check") && count("SELECT count(*) FROM sqlite_master") == before && !strcmp(td_col(&again, td_col(&o, 6) ? 6 : again.cols[again.ncols - 1].id)->type, "TEXT"),
           "TBL-099 / 074.3: an orphan row makes foreign_key_check fail: ROLLBACK, nothing changed, never 'success'");
    td_plan_free(&p); td_free(&t); td_free(&again);
    /* attributes Lume does not model block a rebuild (TBL-081) */
    sqlite3_exec(db, "CREATE TABLE kv (k TEXT PRIMARY KEY, v TEXT) WITHOUT ROWID", NULL, NULL, NULL);
    td_free(&o); snap("kv", &o, err); td_copy(&t, &o); t.cols[1].notnull = true;
    td_plan(TD_SQLITE, &o, &t, &op, &p);
    EXPECT(p.blocked && strstr(p.block_reason, "WITHOUT ROWID"), "TBL-081: WITHOUT ROWID would be lost by a rebuild: blocked (%s)", p.block_reason);
    td_plan_free(&p); td_free(&t); td_free(&o);
    /* missing table: an error, never an empty editor (TBL-002.3) */
    EXPECT(!snap("nope", &o, err) && strstr(err, "was not found"), "TBL-002.3: an unknown table is an error, not an empty designer");
    sqlite3_close(db);

    printf("TBL: %s (%d ok, %d failures)\n", fails ? "FAILED" : "passed", oks, fails);
    return fails ? 1 : 0;
}
