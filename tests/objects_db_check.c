/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object source editor end to end on real databases (OBJ-078): load -> edit -> plan -> review
 * data -> apply -> read back, through the same provider / deployment service the UI uses.
 *   SQLite      always (a file in build/)
 *   PostgreSQL  when LUME_PG_CONN is set (the dbv-postgres test container, schema lume_obj)
 *   Oracle      when built with -DWITH_ORACLE and ORA_* are set (inside the dbv-oracle container)
 * Covers AT-01 / 02 / 03 / 04 / 05 / 07 and the rollback of a failed transactional plan. */
#include "db.h"
#include "objdiag.h"
#include "objengine.h"
#include "objsrc.h"
#include "sqllex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <time.h>
#include <pthread.h>

extern const DbDriver db_pg_driver;
extern const DbDriver db_mysql_driver;
extern const DbDriver db_mssql_driver;
#ifdef WITH_ORACLE
extern const DbDriver db_oracle_driver;
#endif

#ifdef NO_SQLITE
/* db.c needs SQLite, which the Linux build container lacks: the helpers the drivers use */
char *db_strdup(const char *s) { return s ? strdup(s) : NULL; }
void db_quote_ident_double(const char *ident, char *buf, size_t buflen) {
    size_t n = 0; buf[n++] = '"';
    for (const char *p = ident; *p && n < buflen - 4; p++) { if (*p == '.') { buf[n++] = '"'; buf[n++] = '.'; buf[n++] = '"'; continue; } if (*p == '"') buf[n++] = '"'; buf[n++] = *p; }
    buf[n++] = '"'; buf[n] = 0;
}
Db *db_open(const DbDriver *drv, const char *location, char *err, size_t errlen) {
    Db *db = calloc(1, sizeof *db); db->drv = drv; db->location = strdup(location);
    if (!drv->open(db, location, err, errlen)) { free(db->location); free(db); return NULL; }
    return db;
}
void db_close(Db *db) { if (!db) return; db->drv->close(db); free(db->location); free(db); }
void db_cancel(Db *db) { if (db && db->drv->cancel) db->drv->cancel(db); }   /* straight to the driver: no thread here */
#endif

static int fails, oks;
#define CHECK(cond, ...) do { if (cond) { oks++; printf("ok    "); } else { fails++; printf("FAIL  "); } printf(__VA_ARGS__); printf("\n"); fflush(stdout); } while (0)

static bool run(Db *db, const char *sql) {
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
    bool ok = rs.ok;
    if (!ok) printf("      setup: %s -> %s\n", sql, rs.error);
    rs_free(&rs);
    return ok;
}
static char *scalar(Db *db, const char *sql) {
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
    const char *v = rs.ok && rs.nrows ? rs_cell(&rs, 0, 0) : NULL;
    char *o = strdup(v ? v : rs.ok ? "" : rs.error);
    rs_free(&rs);
    return o;
}
static ObjKey mk(int kind, const char *schema, const char *name, const char *sig, const char *table);
static ObjKey mk_(int kind, const char *schema, const char *name) { return mk(kind, schema, name, NULL, NULL); }
static ObjKey mk(int kind, const char *schema, const char *name, const char *sig, const char *table) {
    ObjKey k; memset(&k, 0, sizeof k);
    snprintf(k.conn_id, sizeof k.conn_id, "t"); snprintf(k.schema, sizeof k.schema, "%s", schema); snprintf(k.name, sizeof k.name, "%s", name);
    k.kind = kind; snprintf(k.signature, sizeof k.signature, "%s", sig ? sig : ""); snprintf(k.table, sizeof k.table, "%s", table ? table : "");
    return k;
}
/* loads into *s (caller frees); false on failure */
static bool load(Db *db, const ObjEngine *e, const ObjKey *k, ObjSnapshot *s) {
    ObjJob *j = objsrc_load(db, e, k);
    bool ok = objsrc_wait(j, 20000);
    if (!ok) printf("      load: %s\n", j->err);
    *s = j->snap; memset(&j->snap, 0, sizeof j->snap);
    objsrc_free(j);
    return ok;
}
/* plan with the remote re-read now; returns the plan (caller frees) */
static ObjPlan plan_for(Db *db, const ObjEngine *e, const ObjKey *k, const ObjSnapshot *base, const char *text, bool destructive, const ObjColumn *cols, int ncols) {
    ObjSnapshot remote; load(db, e, k, &remote);
    ObjPlanInput in; memset(&in, 0, sizeof in);
    in.dialect = e->dialect; in.key = k; in.base = base; in.remote = &remote; in.local = text; in.local_len = (int)strlen(text);
    in.has_driver = true; in.allow_destructive = destructive; in.new_cols = cols; in.n_new_cols = ncols; in.server_version = base->server_version;
    ObjPlan p; objplan_make(&in, &p);
    objsnap_free(&remote);
    return p;
}
static ObjJob *apply(Db *db, const ObjEngine *e, const ObjKey *k, const ObjPlan *p, uint64_t fp) {
    ObjJob *j = objsrc_apply(db, e, k, p, fp);
    objsrc_wait(j, 30000);
    return j;
}

/* ------------------------------------------------------------------------ */
static void sqlite_tests(void) {
    const char *path = "build/objects_check.db";
    unlink(path);
    { FILE *f = fopen(path, "w"); if (f) fclose(f); }   /* the driver opens existing files only; an empty file is a valid database */
    char err[256];
    Db *db = db_open(&db_sqlite_driver, path, err, sizeof err), *other = db ? db_open(&db_sqlite_driver, path, err, sizeof err) : NULL;
    if (!db || !other) { printf("FAIL  sqlite open: %s\n", err); fails++; return; }
    const ObjEngine *e = obj_engine_for_driver("sqlite");
    run(db, "CREATE TABLE sales(id INTEGER PRIMARY KEY, amount REAL, note TEXT)");
    run(db, "INSERT INTO sales VALUES (1, 10, 'a'), (2, 20, 'b')");
    run(db, "CREATE VIEW v_revenue AS SELECT id, amount FROM sales");
    run(db, "CREATE TABLE log(msg TEXT)");
    run(db, "CREATE TRIGGER v_rev_ins INSTEAD OF INSERT ON v_revenue BEGIN INSERT INTO log VALUES ('ins ' || new.id); END");
    run(db, "CREATE VIEW v_top AS SELECT * FROM v_revenue WHERE amount > 15");

    ObjKey k = mk(OBJK_VIEW, "main", "v_revenue", NULL, NULL);
    ObjSnapshot base; bool ok = load(db, e, &k, &base);
    CHECK(ok && base.exists && base.ddl && strstr(base.ddl, "CREATE VIEW v_revenue") && base.ncols == 2 && base.nrecreate == 1 && base.ndeps >= 1,
          "SQLite load: DDL as stored, 2 columns, INSTEAD OF trigger to recreate, dependent v_top (approximate)");
    /* listing */
    ObjJob *lj = objsrc_list(db, e, "main", "t"); objsrc_wait(lj, 5000);
    bool seen_view = false, seen_trg = false;
    for (int i = 0; i < lj->nitems; i++) { if (!strcmp(lj->items[i].key.name, "v_revenue")) seen_view = true; if (lj->items[i].key.kind == OBJK_TRIGGER && !strcmp(lj->items[i].key.table, "v_revenue")) seen_trg = true; }
    CHECK(lj->state == OJS_DONE && seen_view && seen_trg, "SQLite list: views and triggers with their table (%d objects)", lj->nitems);
    objsrc_free(lj);
    /* describe = server validation without running the query */
    const char *good = "SELECT id, amount * 2 AS amount, note FROM sales";
    ObjJob *dj = objsrc_describe(db, e, good, (int)strlen(good)); objsrc_wait(dj, 5000);
    CHECK(dj->state == OJS_DONE && dj->ncols == 3 && !strcmp(dj->cols[2].name, "note"), "SQLite validate: query compiled as a temporary view (3 columns), rolled back");
    objsrc_free(dj);
    const char *bad = "SELECT id, missing_col FROM sales";
    dj = objsrc_describe(db, e, bad, (int)strlen(bad)); objsrc_wait(dj, 5000);
    CHECK(dj->state == OJS_FAILED && strstr(dj->err, "missing_col"), "SQLite validate: unknown column reported by the server (%s)", dj->err);
    objsrc_free(dj);
    char *tmp = scalar(db, "SELECT count(*) FROM sqlite_temp_master WHERE name = 'lume_describe'");
    CHECK(!strcmp(tmp, "0"), "SQLite validate: no temporary view left behind (envelope rolled back)");
    free(tmp);

    /* AT-01: edit, plan (DROP / CREATE), apply, read back */
    const char *edited = "CREATE VIEW v_revenue AS\nSELECT id, amount * 2 AS amount\nFROM sales;";
    ObjPlan p = plan_for(db, e, &k, &base, edited, false, NULL, 0);
    CHECK(!p.blocked && p.nsql == 6 && strstr(p.sql[1], "DROP VIEW") && strstr(p.sql[4], "pragma_table_info"), "AT-01: plan DROP + CREATE + INSTEAD OF trigger, unblocked (%s)", p.block_reason);
    char *before = scalar(db, "SELECT sql FROM sqlite_master WHERE name = 'v_revenue'");
    CHECK(strstr(before, "amount * 2") == NULL, "AT-01: planning (Save Draft / review) left the database unchanged");
    free(before);
    ObjJob *aj = apply(db, e, &k, &p, base.fingerprint);
    char *after = scalar(db, "SELECT sql FROM sqlite_master WHERE name = 'v_revenue'");
    char *trg = scalar(db, "SELECT count(*) FROM sqlite_master WHERE type = 'trigger' AND name = 'v_rev_ins'");
    char *val = scalar(db, "SELECT amount FROM v_revenue WHERE id = 2");
    CHECK(aj->state == OJS_DONE && strstr(after, "amount * 2") && !strcmp(trg, "1") && !strcmp(val, "40.0") && aj->snap.exists && strstr(aj->snap.ddl, "amount * 2"),
          "AT-01: applied in one transaction, trigger re-created, data through the new view (%s), definition read back", val);
    free(after); free(trg); free(val);
    ObjSnapshot base2 = aj->snap; memset(&aj->snap, 0, sizeof aj->snap);
    objsrc_free(aj); objplan_free(&p); objsnap_free(&base);

    /* a failing CREATE rolls everything back: the view and its trigger are still there */
    const char *broken = "CREATE VIEW v_revenue AS SELECT id, no_such_column FROM sales";
    p = plan_for(db, e, &k, &base2, broken, false, NULL, 0);
    aj = apply(db, e, &k, &p, base2.fingerprint);
    if (aj->state != OJS_FAILED || aj->failed_at != 4) printf("      state %s, failed_at %d, blocked %d (%s), err: %s\n", objsrc_state_name(aj->state), aj->failed_at, p.blocked, p.block_reason, aj->err);
    after = scalar(db, "SELECT sql FROM sqlite_master WHERE name = 'v_revenue'");
    trg = scalar(db, "SELECT count(*) FROM sqlite_master WHERE type = 'trigger' AND name = 'v_rev_ins'");
    CHECK(aj->state == OJS_FAILED && aj->failed_at == 4 && strstr(aj->err, "no_such_column") && strstr(aj->err, "rolled back") && strstr(after, "amount * 2") && !strcmp(trg, "1"),
          "broken view (SQLite accepts it at CREATE): caught by the compile step %d inside the transaction, rolled back, view and trigger intact", aj->failed_at + 1);
    char *tx = scalar(db, "SELECT 1");   /* the connection is usable and not left in a transaction */
    CHECK(!strcmp(tx, "1") && run(db, "BEGIN") && run(db, "ROLLBACK"), "after the failure the session has no transaction left open");
    free(after); free(trg); free(tx);
    objsrc_free(aj); objplan_free(&p);

    /* AT-04: another session changes the view after the editor opened it */
    run(other, "DROP VIEW v_revenue"); run(other, "CREATE VIEW v_revenue AS SELECT id FROM sales");
    const char *mine = "CREATE VIEW v_revenue AS SELECT id, amount * 3 AS amount FROM sales";
    p = plan_for(db, e, &k, &base2, mine, false, NULL, 0);
    CHECK(p.blocked && p.conflict, "AT-04: the plan sees the remote change (conflict, blocked)");
    p.blocked = false;   /* even if the UI tried to run it anyway, the service re-checks */
    aj = apply(db, e, &k, &p, base2.fingerprint);
    after = scalar(db, "SELECT sql FROM sqlite_master WHERE name = 'v_revenue'");
    CHECK(aj->state == OJS_FAILED && aj->conflict && !strstr(after, "amount * 3"), "AT-04: the deployment re-reads the definition and publishes nothing (%s)", aj->err);
    free(after); objsrc_free(aj); objplan_free(&p); objsnap_free(&base2);


    /* OBJ-058: SQLite virtual tables with their module, read-only */
    run(db, "CREATE VIRTUAL TABLE notes_fts USING fts5(body)");
    lj = objsrc_list(db, e, "main", "t"); objsrc_wait(lj, 5000);
    bool vt = false; for (int i = 0; i < lj->nitems; i++) if (lj->items[i].key.kind == OBJK_VTABLE && !strcmp(lj->items[i].key.name, "notes_fts")) vt = true;
    objsrc_free(lj);
    ObjKey kvt = mk(OBJK_VTABLE, "main", "notes_fts", NULL, NULL);
    ObjSnapshot vs; load(db, e, &kvt, &vs);
    CHECK(vt && vs.system && strstr(vs.ro_reason, "module fts5") && strstr(vs.original, "USING fts5"), "OBJ-058: SQLite virtual table listed with its module (fts5), read-only");
    objsnap_free(&vs);
    db_close(db); db_close(other);
}

/* a list adapter that tries to write */
static bool evil_list(const char *schema, ObjSqlBuf *o) { obj_sql_add(o, "SELECT 1; DELETE FROM sales"); return true; }
static void hostile_adapter(void) {
    char err[256];
    Db *db = db_open(&db_sqlite_driver, "build/objects_check.db", err, sizeof err);
    ObjEngine evil = obj_engine_sqlite; evil.id = "evil"; evil.list_sql = evil_list;
    ObjJob *j = objsrc_list(db, &evil, "main", "t");
    char *n = scalar(db, "SELECT count(*) FROM sales");
    CHECK(j->state == OJS_FAILED && strstr(j->err, "not a plain read") && !strcmp(n, "2"), "OBJ-080: catalog SQL that writes is refused by the provider; rows intact");
    free(n); objsrc_free(j); db_close(db);
}

/* ------------------------------------------------------------------------ */
static Db *pg_open(void) {
    const char *conn = getenv("LUME_PG_CONN");
    if (!conn) return NULL;
    char err[512];
    Db *db = db_open(&db_pg_driver, conn, err, sizeof err);
    if (!db) printf("      pg open: %s\n", err);
    return db;
}
struct killarg { const char *conn; char pid[32]; int delay_ms; };
static void *killer(void *a) {
    struct killarg *k = a;
    usleep((useconds_t)k->delay_ms * 1000);
    char err[256]; Db *db = db_open(&db_pg_driver, k->conn, err, sizeof err);
    if (db) { char sql[128]; snprintf(sql, sizeof sql, "SELECT pg_terminate_backend(%s)", k->pid); run(db, sql); db_close(db); }
    return NULL;
}

static void pg_tests(void) {
    Db *db = pg_open(), *other = pg_open();
    if (!db || !other) { printf("skip  PostgreSQL: LUME_PG_CONN not set\n"); return; }
    const ObjEngine *e = obj_engine_for_driver("postgres");
    run(db, "DROP SCHEMA IF EXISTS lume_obj CASCADE");
    run(db, "CREATE SCHEMA lume_obj");
    run(db, "DO $$ BEGIN IF NOT EXISTS (SELECT 1 FROM pg_roles WHERE rolname = 'lume_obj_reader') THEN CREATE ROLE lume_obj_reader; END IF; END $$");
    run(db, "CREATE TABLE lume_obj.sales(id int PRIMARY KEY, amount numeric(12,2), note text)");
    run(db, "INSERT INTO lume_obj.sales VALUES (1, 10, 'a'), (2, 20, 'b')");
    run(db, "CREATE VIEW lume_obj.v_sales AS SELECT id, amount FROM lume_obj.sales");
    run(db, "GRANT SELECT ON lume_obj.v_sales TO lume_obj_reader");
    run(db, "COMMENT ON VIEW lume_obj.v_sales IS 'sales, it''s public'");
    run(db, "CREATE FUNCTION lume_obj.calculate(p integer) RETURNS numeric LANGUAGE sql AS $f$ SELECT p * 2.0 $f$");
    run(db, "CREATE FUNCTION lume_obj.calculate(p text) RETURNS numeric LANGUAGE sql AS $f$ SELECT length(p)::numeric $f$");
    run(db, "CREATE MATERIALIZED VIEW lume_obj.mv_total AS SELECT sum(amount) AS total FROM lume_obj.sales");
    run(db, "CREATE INDEX mv_total_idx ON lume_obj.mv_total(total)");

    /* list: overloads are two objects with their signatures */
    ObjJob *lj = objsrc_list(db, e, "lume_obj", "t"); objsrc_wait(lj, 10000);
    ObjKey ki = { 0 }, kt = { 0 };
    for (int i = 0; i < lj->nitems; i++) if (lj->items[i].key.kind == OBJK_FUNCTION) { if (!strcmp(lj->items[i].key.signature, "integer")) ki = lj->items[i].key; else kt = lj->items[i].key; }
    CHECK(lj->state == OJS_DONE && ki.native_id[0] && kt.native_id[0] && strcmp(ki.native_id, kt.native_id), "PG list: calculate(integer) and calculate(text) are distinct objects (oids %s / %s)", ki.native_id, kt.native_id);
    objsrc_free(lj);

    /* AT-03 */
    ObjSnapshot fb; load(db, e, &ki, &fb);
    CHECK(fb.exists && strstr(fb.ddl, "calculate(p integer)") && !strcmp(fb.result_type, "numeric"), "PG load: function DDL from pg_get_functiondef, return type %s", fb.result_type);
    char *edited = malloc(strlen(fb.ddl) + 32); strcpy(edited, fb.ddl);
    char *x = strstr(edited, "p * 2.0"); if (x) memcpy(x, "p * 3.0", 7);
    ObjPlan p = plan_for(db, e, &ki, &fb, edited, false, NULL, 0);
    ObjJob *aj = apply(db, e, &ki, &p, fb.fingerprint);
    char *vi = scalar(db, "SELECT lume_obj.calculate(2)"), *vt = scalar(db, "SELECT lume_obj.calculate('abc'::text)");
    CHECK(aj->state == OJS_DONE && !strcmp(vi, "6.0") && !strcmp(vt, "3"), "AT-03: calculate(integer) replaced (%s), calculate(text) untouched (%s)", vi, vt);
    free(vi); free(vt); objsrc_free(aj); objplan_free(&p);
    /* identity drift between review and run: the probe refuses */
    run(other, "DROP FUNCTION lume_obj.calculate(integer)");
    run(other, "CREATE FUNCTION lume_obj.calculate(p integer) RETURNS numeric LANGUAGE sql AS $f$ SELECT p * 4.0 $f$");
    ObjSnapshot fb2; load(db, e, &ki, &fb2);                 /* the editor reloaded: same name and signature, new oid */
    p = plan_for(db, e, &ki, &fb2, edited, false, NULL, 0);
    aj = apply(db, e, &ki, &p, fb2.fingerprint);
    CHECK(aj->state == OJS_FAILED && strstr(aj->err, "another routine"), "AT-03: a routine re-created under the same signature (new oid) is refused (%s)", aj->err);
    objsrc_free(aj); objplan_free(&p); objsnap_free(&fb2); free(edited); objsnap_free(&fb);

    /* AT-02: incompatible view change */
    ObjKey kv = mk(OBJK_VIEW, "lume_obj", "v_sales", NULL, NULL);
    ObjSnapshot vb; load(db, e, &kv, &vb);
    CHECK(vb.exists && vb.ncols == 2 && !strcmp(vb.cols[1].type, "numeric(12,2)") && vb.nrecreate >= 3, "PG load view: columns with typmods, owner / grant / comment to restore (%d)", vb.nrecreate);
    const char *nv = "CREATE OR REPLACE VIEW lume_obj.v_sales AS\n SELECT id, amount::text AS amount FROM lume_obj.sales;";
    const char *q = strstr(nv, "SELECT");
    ObjJob *dj = objsrc_describe(db, e, q, (int)strlen(q)); objsrc_wait(dj, 10000);
    CHECK(dj->state == OJS_DONE && dj->ncols == 2 && !strcmp(dj->cols[1].type, "text"), "PG validate: temporary view describes the new query (amount text), rolled back");
    p = plan_for(db, e, &kv, &vb, nv, false, dj->cols, dj->ncols);
    CHECK(p.blocked && p.destructive_available && strstr(p.block_reason, "changes type"), "AT-02: no simple CREATE OR REPLACE; explicit plan required (%s)", p.block_reason);
    objplan_free(&p);
    p = plan_for(db, e, &kv, &vb, nv, true, dj->cols, dj->ncols);
    aj = apply(db, e, &kv, &p, vb.fingerprint);
    char *acl = scalar(db, "SELECT has_table_privilege('lume_obj_reader', 'lume_obj.v_sales', 'SELECT')::text");
    char *cm = scalar(db, "SELECT obj_description('lume_obj.v_sales'::regclass, 'pg_class')");
    char *ty = scalar(db, "SELECT format_type(atttypid, atttypmod) FROM pg_attribute WHERE attrelid = 'lume_obj.v_sales'::regclass AND attname = 'amount'");
    CHECK(aj->state == OJS_DONE && !strcmp(acl, "true") && !strcmp(cm, "sales, it's public") && !strcmp(ty, "text"),
          "AT-02: explicit DROP + CREATE applied in one transaction; grant and comment restored (OBJ-070)");
    free(acl); free(cm); free(ty); objsrc_free(aj); objplan_free(&p); objsrc_free(dj); objsnap_free(&vb);
    /* a dependent view blocks the drop (never CASCADE) */
    run(db, "CREATE VIEW lume_obj.v_report AS SELECT * FROM lume_obj.v_sales");
    load(db, e, &kv, &vb);
    ObjColumn c2[2] = { { "id", "integer" }, { "amount", "integer" } };
    p = plan_for(db, e, &kv, &vb, "CREATE OR REPLACE VIEW lume_obj.v_sales AS SELECT id, amount::int AS amount FROM lume_obj.sales", true, c2, 2);
    CHECK(p.blocked && strstr(p.block_reason, "CASCADE") && vb.ndeps == 1, "PG: dependent view v_report found in the catalog; drop refused without CASCADE");
    objplan_free(&p); objsnap_free(&vb);

    /* materialized view: indexes restored after the explicit drop + create */
    ObjKey km = mk(OBJK_MATVIEW, "lume_obj", "mv_total", NULL, NULL);
    ObjSnapshot mb; load(db, e, &km, &mb);
    const char *nm = "CREATE MATERIALIZED VIEW lume_obj.mv_total AS SELECT sum(amount) * 2 AS total FROM lume_obj.sales WITH DATA;";
    p = plan_for(db, e, &km, &mb, nm, true, NULL, 0);
    aj = apply(db, e, &km, &p, mb.fingerprint);
    char *idx = scalar(db, "SELECT count(*) FROM pg_indexes WHERE schemaname = 'lume_obj' AND indexname = 'mv_total_idx'");
    char *tot = scalar(db, "SELECT total FROM lume_obj.mv_total");
    CHECK(aj->state == OJS_DONE && !strcmp(idx, "1") && !strcmp(tot, "60.00"), "PG matview: rebuilt (total %s) and its index re-created", tot);
    free(idx); free(tot); objsrc_free(aj); objplan_free(&p); objsnap_free(&mb);

    /* trigger: CREATE OR REPLACE from PostgreSQL 14 */
    run(db, "CREATE FUNCTION lume_obj.audit() RETURNS trigger LANGUAGE plpgsql AS $f$ BEGIN RETURN NEW; END $f$");
    run(db, "CREATE TRIGGER trg_audit BEFORE INSERT ON lume_obj.sales FOR EACH ROW EXECUTE FUNCTION lume_obj.audit()");
    ObjKey kg = mk(OBJK_TRIGGER, "lume_obj", "trg_audit", NULL, "sales");
    ObjSnapshot gb; load(db, e, &kg, &gb);
    const char *ng = "CREATE OR REPLACE TRIGGER trg_audit BEFORE INSERT OR UPDATE ON lume_obj.sales FOR EACH ROW EXECUTE FUNCTION lume_obj.audit()";
    p = plan_for(db, e, &kg, &gb, ng, false, NULL, 0);
    aj = apply(db, e, &kg, &p, gb.fingerprint);
    CHECK(gb.exists && aj->state == OJS_DONE && strstr(aj->snap.original, "INSERT OR UPDATE"), "PG trigger: replaced in place (%s)", aj->state == OJS_DONE ? "server >= 14" : aj->err);
    objsrc_free(aj); objplan_free(&p); objsnap_free(&gb);


    /* OBJ-058: extension members and aggregates are listed, read-only, never with invented DDL */
    run(db, "CREATE AGGREGATE lume_obj.sum_sq(numeric) (SFUNC = numeric_add, STYPE = numeric)");
    lj = objsrc_list(db, e, "public", "t"); objsrc_wait(lj, 10000);
    ObjKey kx = { 0 }; bool ext_seen = false;
    for (int i = 0; i < lj->nitems; i++) if (!strcmp(lj->items[i].extension, "pg_stat_statements") && lj->items[i].key.kind == OBJK_FUNCTION) { ext_seen = true; kx = lj->items[i].key; }
    CHECK(ext_seen, "OBJ-058: functions owned by an extension are listed with its name (%s)", kx.name);
    objsrc_free(lj);
    ObjSnapshot xs; load(db, e, &kx, &xs);
    ObjPlan xp = plan_for(db, e, &kx, &xs, xs.ddl ? xs.ddl : "", false, NULL, 0);
    CHECK(xs.system && strstr(xs.ro_reason, "extension pg_stat_statements") && xp.blocked && strstr(xp.block_reason, "extension"), "OBJ-058: an extension member opens read-only and the planner refuses it (%s)", xp.block_reason);
    objplan_free(&xp); objsnap_free(&xs);
    lj = objsrc_list(db, e, "lume_obj", "t"); objsrc_wait(lj, 10000);
    ObjKey kag = { 0 }; for (int i = 0; i < lj->nitems; i++) if (!strcmp(lj->items[i].key.name, "sum_sq")) { kag = lj->items[i].key; CHECK(!strcmp(lj->items[i].status, "AGGREGATE"), "OBJ-058: aggregates are listed as such"); }
    objsrc_free(lj);
    ObjSnapshot as; load(db, e, &kag, &as);
    CHECK(as.exists && !as.ddl && strstr(as.missing, "aggregate") && as.system, "OBJ-058 / 069: an aggregate has no reconstructed DDL; the reason is stated (%s)", as.missing);
    objsnap_free(&as);

    /* OBJ-058: operators and event triggers: listed, shown from the catalog, read-only */
    run(db, "CREATE FUNCTION lume_obj.eqx(integer, integer) RETURNS boolean LANGUAGE sql IMMUTABLE AS $f$ SELECT $1 = $2 $f$");
    run(db, "CREATE OPERATOR lume_obj.=== (FUNCTION = lume_obj.eqx, LEFTARG = integer, RIGHTARG = integer)");
    run(db, "CREATE FUNCTION lume_obj.on_ddl() RETURNS event_trigger LANGUAGE plpgsql AS $f$ BEGIN END $f$");
    run(db, "DROP EVENT TRIGGER IF EXISTS lume_obj_evt");
    run(db, "CREATE EVENT TRIGGER lume_obj_evt ON ddl_command_end WHEN TAG IN ('CREATE TABLE', 'ALTER TABLE') EXECUTE FUNCTION lume_obj.on_ddl()");
    lj = objsrc_list(db, e, "lume_obj", "t"); objsrc_wait(lj, 10000);
    ObjKey kop = { 0 }; for (int i = 0; i < lj->nitems; i++) if (lj->items[i].key.kind == OBJK_OPERATOR) kop = lj->items[i].key;
    objsrc_free(lj);
    ObjSnapshot os_; load(db, e, &kop, &os_);
    CHECK(!strcmp(kop.name, "===") && !strcmp(kop.signature, "integer, integer") && os_.exists && os_.system && !os_.ddl && strstr(os_.original, "FUNCTION = lume_obj.eqx(integer,integer)") && strstr(os_.ro_reason, "operator"),
          "OBJ-058: operator === (integer, integer) listed and shown from pg_operator, read-only");
    objsnap_free(&os_);
    lj = objsrc_list(db, e, "public", "t"); objsrc_wait(lj, 10000);
    ObjKey kev = { 0 }; for (int i = 0; i < lj->nitems; i++) if (lj->items[i].key.kind == OBJK_EVENT_TRIGGER && !strcmp(lj->items[i].key.name, "lume_obj_evt")) kev = lj->items[i].key;
    objsrc_free(lj);
    ObjSnapshot es; load(db, e, &kev, &es);
    CHECK(kev.name[0] && es.exists && es.system && strstr(es.original, "ON ddl_command_end") && strstr(es.original, "WHEN TAG IN ('CREATE TABLE', 'ALTER TABLE')") && strstr(es.original, "EXECUTE FUNCTION lume_obj.on_ddl()"),
          "OBJ-058: event trigger listed in the current schema and shown with its event, tags and function, read-only");
    objsnap_free(&es);
    run(db, "DROP EVENT TRIGGER IF EXISTS lume_obj_evt");
    /* AT-07: the connection dies after a DDL committed on its own (autocommit plan) and before
     * the deployment finished: the outcome is UNKNOWN and nothing is retried */
    run(db, "CREATE VIEW lume_obj.v_lost AS SELECT 0 AS a");
    ObjKey kl = mk(OBJK_VIEW, "lume_obj", "v_lost", NULL, NULL);
    ObjSnapshot lb; load(db, e, &kl, &lb);
    ObjPlan lp; memset(&lp, 0, sizeof lp); lp.create_index = 0; lp.atomicity = ATOM_AUTOCOMMIT;
    lp.sql[lp.nsql++] = strdup("CREATE OR REPLACE VIEW lume_obj.v_lost AS SELECT 1 AS a");
    lp.sql[lp.nsql++] = strdup("SELECT pg_sleep(5)");
    char *pid = scalar(db, "SELECT pg_backend_pid()::text");
    struct killarg ka = { getenv("LUME_PG_CONN"), "", 1500 }; snprintf(ka.pid, sizeof ka.pid, "%s", pid); free(pid);
    pthread_t th; pthread_create(&th, NULL, killer, &ka);
    aj = objsrc_apply(db, e, &kl, &lp, lb.fingerprint);
    objsrc_wait(aj, 20000);
    pthread_join(th, NULL);
    char *now = scalar(other, "SELECT pg_get_viewdef('lume_obj.v_lost'::regclass)");
    CHECK(aj->state == OJS_UNKNOWN && aj->failed_at == 1 && aj->ran == 1 && strstr(aj->err, "unknown") && strstr(now, "1 AS a"),
          "AT-07: connection killed after the DDL ran -> state %s, statement %d, nothing retried (%s)", objsrc_state_name(aj->state), aj->failed_at + 1, aj->err);
    free(now); objsrc_free(aj); objplan_free(&lp); objsnap_free(&lb);
    run(other, "DROP SCHEMA IF EXISTS lume_obj CASCADE");
    db_close(db); db_close(other);
}

/* ------------------------------------------------------------------------ */
/* MySQL (BL-013 / AT-06): the driver, then objects end to end */
struct mykill { Db *db; int delay_ms; };
static void *my_killer(void *a) { struct mykill *k = a; usleep((useconds_t)k->delay_ms * 1000); k->db->drv->cancel(k->db); return NULL; }

static void mysql_tests(const char *env, const char *engine, bool maria) {
    const char *loc = getenv(env);
    if (!loc) { printf("skip  %s: %s not set\n", engine, env); return; }
    printf("---- %s\n", engine);
    char err[512]; Db *db = db_open(&db_mysql_driver, loc, err, sizeof err), *other = db ? db_open(&db_mysql_driver, loc, err, sizeof err) : NULL;
    if (!db || !other) { printf("FAIL  mysql open: %s\n", err); fails++; return; }
    const ObjEngine *e = obj_engine_for_driver(engine);
    run(db, "DROP TABLE IF EXISTS sales"); run(db, "DROP VIEW IF EXISTS v_sales"); run(db, "DROP PROCEDURE IF EXISTS p_bump"); run(db, "DROP FUNCTION IF EXISTS f_twice");
    run(db, "CREATE TABLE sales(id INT PRIMARY KEY, amount DECIMAL(12,2), note VARCHAR(40))");
    run(db, "INSERT INTO sales VALUES (1, 10, 'a'), (2, 20, 'b'), (3, 30, 'c')");
    /* driver: streaming with fetch-more, types, errors, catalog, cancel */
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, "SELECT id, amount, note FROM sales ORDER BY id", -1, 2, &rs);
    bool first = rs.ok && rs.nrows == 2 && rs.cursor && rs.kinds[0] == CK_NUMBER && !strcmp(rs.types[1], "decimal");
    bool more = db->drv->fetch(db, &rs, 10);
    CHECK(first && more && rs.nrows == 3 && !strcmp(rs_cell(&rs, 2, 2), "c") && !rs.cursor, "MySQL driver: 2 rows then fetch-more to the end; int / decimal types (%s)", rs.ok ? "ok" : rs.error);
    rs_free(&rs);
    db->drv->exec(db, "SELECT * FROM no_such_table", -1, 0, &rs);
    CHECK(!rs.ok && rs.err_code == 1146 && !strcmp(rs.err_state, "42S02"), "MySQL driver: error with code and SQLSTATE (%ld %s)", rs.err_code, rs.err_state);
    rs_free(&rs);
    DbTable *tb = NULL; int nt = 0; db->drv->list_tables(db, &tb, &nt, err, sizeof err);
    bool st = false; for (int i = 0; i < nt; i++) if (!strcmp(tb[i].name, "sales")) { db->drv->list_columns(db, &tb[i], err, sizeof err); st = tb[i].ncols == 3 && tb[i].cols[0].pk; }
    CHECK(st, "MySQL driver: tables and columns from information_schema (primary key seen)");
    db_tables_free(tb, nt);
    struct mykill mk = { db, 700 }; pthread_t th; pthread_create(&th, NULL, my_killer, &mk);
    db->drv->exec(db, "SELECT SLEEP(5)", -1, 0, &rs);
    pthread_join(th, NULL);
    bool cancelled = rs.cancelled || (rs.ok && rs.nrows == 1 && !strcmp(rs_cell(&rs, 0, 0), "1"));   /* SLEEP returns 1 when interrupted */
    CHECK(cancelled && rs.exec_ms < 4000, "MySQL driver: cancel (KILL QUERY from a second session) stops a running statement in %.0f ms", rs.exec_ms);
    rs_free(&rs);

    run(db, "CREATE VIEW v_sales AS SELECT id, amount FROM sales");
    run(db, "CREATE PROCEDURE p_bump(IN x INT) BEGIN UPDATE sales SET amount = amount + x WHERE id = 1; END");
    run(db, "CREATE FUNCTION f_twice(x INT) RETURNS INT DETERMINISTIC RETURN x * 2");
    ObjJob *lj = objsrc_list(db, e, "", "t"); objsrc_wait(lj, 10000);
    int seen = 0; for (int i = 0; i < lj->nitems; i++) if (!strcmp(lj->items[i].key.name, "v_sales") || !strcmp(lj->items[i].key.name, "p_bump") || !strcmp(lj->items[i].key.name, "f_twice")) seen++;
    CHECK(lj->state == OJS_DONE && seen == 3, "MySQL objects listed from information_schema (view, procedure, function)");
    objsrc_free(lj);
    /* AT-06: changing a procedure body */
    ObjKey kp = mk_(OBJK_PROCEDURE, "shop", "p_bump");
    ObjSnapshot pb; load(db, e, &kp, &pb);
    CHECK(pb.exists && strstr(pb.original, "CREATE DEFINER=") && strstr(pb.ddl, "PROCEDURE `p_bump`"), "%s load: SHOW CREATE PROCEDURE (definer included)", engine);
    char *ed = malloc(strlen(pb.ddl) + 8); strcpy(ed, pb.ddl); char *x = strstr(ed, "amount + x"); if (x) memcpy(x, "amount * x", 10);
    ObjPlan p;
    if (!maria) {
        p = plan_for(db, e, &kp, &pb, ed, false, NULL, 0);
        bool warn = false; for (int i = 0; i < p.nimpact; i++) if (strstr(p.impact[i], "DEFINER") || strstr(p.impact[i], "privileges")) warn = true;
        CHECK(p.blocked && p.destructive_available && warn && !p.script_only, "AT-06: body change needs DROP + CREATE: warns about definer / grants / downtime and asks for explicit consent");
        objplan_free(&p);
        p = plan_for(db, e, &kp, &pb, ed, true, NULL, 0);
    } else {
        /* OBJ-054: MariaDB is not MySQL: CREATE OR REPLACE PROCEDURE, in place, no DROP */
        p = plan_for(db, e, &kp, &pb, ed, false, NULL, 0);
        CHECK(strstr(pb.ddl, "CREATE OR REPLACE ") && !p.blocked && !p.destructive && p.nsql == 1 && strstr(p.strategy, "MariaDB"), "OBJ-054: MariaDB body change = CREATE OR REPLACE PROCEDURE in place, no DROP, no destructive consent (%s)", p.block_reason);
    }
    ObjJob *aj = apply(db, e, &kp, &p, pb.fingerprint);
    run(db, "CALL p_bump(3)");
    char *amt = scalar(db, "SELECT amount FROM sales WHERE id = 1");
    CHECK(aj->state == OJS_DONE && !strcmp(amt, "30.00") && strstr(aj->snap.original, "amount * x"), "%s: the new body was published and runs (10 * 3 = %s)", maria ? "MariaDB" : "AT-06", amt);
    free(amt); free(ed); objsrc_free(aj); objplan_free(&p); objsnap_free(&pb);
    /* views: CREATE OR REPLACE in place */
    ObjKey kv = mk_(OBJK_VIEW, "shop", "v_sales");
    ObjSnapshot vb; load(db, e, &kv, &vb);
    const char *nv = "CREATE OR REPLACE VIEW `v_sales` AS SELECT id, amount * 2 AS amount FROM sales";
    p = plan_for(db, e, &kv, &vb, nv, false, NULL, 0);
    aj = apply(db, e, &kv, &p, vb.fingerprint);
    char *v2 = scalar(db, "SELECT amount FROM v_sales WHERE id = 2");
    CHECK(vb.exists && strstr(vb.ddl, "CREATE OR REPLACE ") && aj->state == OJS_DONE && !strcmp(v2, "40.00"), "MySQL view: CREATE OR REPLACE applied in place (%s)", v2);
    free(v2); objsrc_free(aj); objplan_free(&p); objsnap_free(&vb);
    /* AT-04 on MySQL: another session changes the procedure after it was opened */
    load(db, e, &kp, &pb);
    run(other, "DROP PROCEDURE p_bump"); run(other, "CREATE PROCEDURE p_bump(IN x INT) BEGIN SELECT x; END");
    p = plan_for(db, e, &kp, &pb, "CREATE PROCEDURE p_bump(IN x INT) BEGIN SELECT x + 1; END", true, NULL, 0);
    CHECK(p.blocked && p.conflict, "MySQL: a procedure changed by another session is a conflict (nothing runs)");
    objplan_free(&p); objsnap_free(&pb);
    db_close(db); db_close(other);
}

/* ------------------------------------------------------------------------ */
/* SQL Server (BL-014 / OBJ-052): the FreeTDS driver, then objects end to end */
static void mssql_tests(void) {
    const char *loc0 = getenv("LUME_MSSQL_LOC");
    if (!loc0) { printf("skip  SQL Server: LUME_MSSQL_LOC not set\n"); return; }
    printf("---- sqlserver\n");
    char err[512]; Db *adm = db_open(&db_mssql_driver, loc0, err, sizeof err);
    if (!adm) { printf("FAIL  sqlserver open: %s\n", err); fails++; return; }
    run(adm, "IF DB_ID('lume_obj') IS NULL CREATE DATABASE lume_obj");
    db_close(adm);
    char loc[1100]; snprintf(loc, sizeof loc, "%.*slume_obj", (int)(strrchr(loc0, '\n') - loc0 + 1), loc0);   /* same login, database lume_obj */
    Db *db = db_open(&db_mssql_driver, loc, err, sizeof err), *other = db ? db_open(&db_mssql_driver, loc, err, sizeof err) : NULL;
    if (!db || !other) { printf("FAIL  sqlserver open lume_obj: %s\n", err); fails++; return; }
    const ObjEngine *e = obj_engine_for_driver("sqlserver");
    run(db, "DROP VIEW IF EXISTS dbo.v_sales"); run(db, "DROP PROCEDURE IF EXISTS dbo.p_bump"); run(db, "DROP PROCEDURE IF EXISTS dbo.p_secret"); run(db, "DROP TABLE IF EXISTS dbo.sales");
    run(db, "CREATE TABLE dbo.sales(id int PRIMARY KEY, amount decimal(12,2), note nvarchar(40))");
    run(db, "INSERT INTO dbo.sales VALUES (1, 10, N'a'), (2, 20, N'b'), (3, 30, N'c')");
    ResultSet rs; rs_init(&rs);
    db->drv->exec(db, "SELECT id, amount, note FROM dbo.sales ORDER BY id", -1, 2, &rs);
    bool first = rs.ok && rs.nrows == 2 && rs.cursor;
    bool more = db->drv->fetch(db, &rs, 10);
    CHECK(first && more && rs.nrows == 3 && !strcmp(rs_cell(&rs, 2, 2), "c") && !rs.cursor && !strcmp(rs.types[1], "decimal"), "SQL Server driver: 2 rows then fetch-more to the end; decimal / nvarchar (%s)", rs.ok ? "ok" : rs.error);
    rs_free(&rs);
    db->drv->exec(db, "SELECT * FROM dbo.no_such_table", -1, 0, &rs);
    CHECK(!rs.ok && rs.err_code == 208, "SQL Server driver: error number 208 with the server's message (%s)", rs.error);
    rs_free(&rs);
    DbTable *tb = NULL; int nt = 0; db->drv->list_tables(db, &tb, &nt, err, sizeof err);
    bool st = false; for (int i = 0; i < nt; i++) if (!strcmp(tb[i].name, "dbo.sales")) { db->drv->list_columns(db, &tb[i], err, sizeof err); st = tb[i].ncols == 3 && tb[i].cols[0].pk; }
    CHECK(st, "SQL Server driver: tables (schema-qualified) and columns from sys.* (primary key seen)");
    db_tables_free(tb, nt);
    struct mykill mk = { db, 700 }; pthread_t th; pthread_create(&th, NULL, my_killer, &mk);
    struct timespec w0, w1; clock_gettime(CLOCK_MONOTONIC, &w0);
    db->drv->exec(db, "WAITFOR DELAY '00:00:05'", -1, 0, &rs);
    clock_gettime(CLOCK_MONOTONIC, &w1);
    pthread_join(th, NULL);
    double wall = (w1.tv_sec - w0.tv_sec) * 1000.0 + (w1.tv_nsec - w0.tv_nsec) / 1e6;
    CHECK(wall >= 600 && wall < 4000, "SQL Server driver: cancel (TDS attention) stops WAITFOR after %.0f ms of wall time (sent at 700 ms; uncancelled: 5000)", wall);
    rs_free(&rs);
    char *alive = scalar(db, "SELECT 1");
    CHECK(!strcmp(alive, "1"), "SQL Server driver: the connection works after the cancel");
    free(alive);

    run(db, "CREATE VIEW dbo.v_sales AS SELECT id, amount FROM dbo.sales");
    run(db, "CREATE PROCEDURE dbo.p_bump @x int AS BEGIN SET NOCOUNT ON; UPDATE dbo.sales SET amount = amount + @x WHERE id = 1; END");
    run(db, "CREATE PROCEDURE dbo.p_secret WITH ENCRYPTION AS SELECT 1 AS one");
    ObjJob *lj = objsrc_list(db, e, "dbo", "t"); objsrc_wait(lj, 20000);
    int seen = 0; bool enc = false;
    for (int i = 0; i < lj->nitems; i++) { if (!strcmp(lj->items[i].key.name, "v_sales") || !strcmp(lj->items[i].key.name, "p_bump")) seen++; if (!strcmp(lj->items[i].key.name, "p_secret") && !strcmp(lj->items[i].status, "ENCRYPTED")) enc = true; }
    CHECK(lj->state == OJS_DONE && seen == 2 && enc, "SQL Server objects listed from sys.objects; the encrypted procedure is marked");
    objsrc_free(lj);
    ObjKey kv = mk_(OBJK_VIEW, "dbo", "v_sales");
    ObjSnapshot vb; load(db, e, &kv, &vb);
    CHECK(vb.exists && !strncmp(vb.ddl, "CREATE OR ALTER VIEW", 20) && vb.ncols == 2, "OBJ-052: the view opens as CREATE OR ALTER VIEW (OBJECT_DEFINITION), with its columns");
    const char *nv = "CREATE OR ALTER VIEW dbo.v_sales AS SELECT id, amount * 2 AS amount FROM dbo.sales\nGO\n";
    ObjPlan p = plan_for(db, e, &kv, &vb, nv, false, NULL, 0);
    ObjJob *aj = apply(db, e, &kv, &p, vb.fingerprint);
    char *v2 = scalar(db, "SELECT amount FROM dbo.v_sales WHERE id = 2");
    CHECK(!p.blocked && p.nsql == 1 && !strstr(p.sql[0], "GO") && aj->state == OJS_DONE && !strcmp(v2, "40.00"), "OBJ-052: CREATE OR ALTER VIEW published (GO stays out of the batch); the view returns %s", v2);
    free(v2); objsrc_free(aj); objplan_free(&p); objsnap_free(&vb);
    ObjKey kp = mk_(OBJK_PROCEDURE, "dbo", "p_bump");
    ObjSnapshot pb; load(db, e, &kp, &pb);
    char *ed = malloc(strlen(pb.ddl) + 8); strcpy(ed, pb.ddl); char *x = strstr(ed, "amount + @x"); if (x) memcpy(x, "amount * @x", 11);
    p = plan_for(db, e, &kp, &pb, ed, false, NULL, 0);
    aj = apply(db, e, &kp, &p, pb.fingerprint);
    run(db, "EXEC dbo.p_bump 3");
    char *amt = scalar(db, "SELECT amount FROM dbo.sales WHERE id = 1");
    CHECK(!p.destructive && aj->state == OJS_DONE && !strcmp(amt, "30.00"), "OBJ-052: procedure altered in place (CREATE OR ALTER keeps permissions and object id); new body runs (10 * 3 = %s)", amt);
    free(amt); free(ed); objsrc_free(aj); objplan_free(&p); objsnap_free(&pb);
    ObjKey ks = mk_(OBJK_PROCEDURE, "dbo", "p_secret");
    ObjSnapshot sb; load(db, e, &ks, &sb);
    p = plan_for(db, e, &ks, &sb, "CREATE OR ALTER PROCEDURE dbo.p_secret AS SELECT 2 AS two", false, NULL, 0);
    CHECK(sb.encrypted && !sb.ddl && p.blocked && strstr(p.block_reason, "ENCRYPTION"), "OBJ-052 / 058: WITH ENCRYPTION: no source, read-only, the planner refuses (%s)", p.block_reason);
    objplan_free(&p); objsnap_free(&sb);
    load(db, e, &kp, &pb);
    run(other, "ALTER PROCEDURE dbo.p_bump @x int AS SELECT @x AS x");
    p = plan_for(db, e, &kp, &pb, "CREATE OR ALTER PROCEDURE dbo.p_bump @x int AS SELECT @x + 1 AS x", false, NULL, 0);
    CHECK(p.blocked && p.conflict, "SQL Server: a procedure altered by another session is a conflict (nothing runs)");
    objplan_free(&p); objsnap_free(&pb);
    db_close(db); db_close(other);
}

/* ------------------------------------------------------------------------ */
#ifdef WITH_ORACLE
static void oracle_tests(void) {
    const char *u = getenv("ORA_USER"), *pw = getenv("ORA_PASSWORD"), *dsn = getenv("ORA_DSN");
    if (!u || !pw || !dsn) { printf("skip  Oracle: ORA_* not set\n"); return; }
    char loc[512]; snprintf(loc, sizeof loc, "%s\n%s\n%s", u, pw, dsn);   /* the driver's location: user, password, DSN on separate lines */
    char err[512]; Db *db = db_open(&db_oracle_driver, loc, err, sizeof err);
    if (!db) { printf("FAIL  oracle open: %s\n", err); fails++; return; }
    const ObjEngine *e = obj_engine_for_driver("oracle");
    char owner[64]; char *o = scalar(db, "SELECT USER FROM dual"); snprintf(owner, sizeof owner, "%s", o); free(o);
    run(db, "BEGIN EXECUTE IMMEDIATE 'DROP PACKAGE lume_pkg'; EXCEPTION WHEN OTHERS THEN NULL; END;");
    run(db, "CREATE OR REPLACE PACKAGE lume_pkg AS FUNCTION f(x NUMBER) RETURN NUMBER; END lume_pkg;");
    run(db, "CREATE OR REPLACE PACKAGE BODY lume_pkg AS FUNCTION f(x NUMBER) RETURN NUMBER IS BEGIN RETURN x * 2; END; END lume_pkg;");
    ObjJob *lj = objsrc_list(db, e, owner, "t"); objsrc_wait(lj, 20000);
    int pk = 0; for (int i = 0; i < lj->nitems; i++) if (!strcmp(lj->items[i].key.name, "LUME_PKG")) pk++;
    CHECK(lj->state == OJS_DONE && pk == 2, "Oracle list: LUME_PKG spec and body are two objects (%d)", pk);
    objsrc_free(lj);
    ObjKey kb = mk(OBJK_PKG_BODY, owner, "LUME_PKG", NULL, NULL);
    ObjSnapshot bb; load(db, e, &kb, &bb);
    CHECK(bb.exists && !strncmp(bb.ddl, "CREATE OR REPLACE PACKAGE BODY", 30) && !strcmp(bb.status, "VALID"), "Oracle load: body from ALL_SOURCE, status %s", bb.status);
    /* AT-05: compile error -> INVALID + real errors, never "success" */
    const char *badb = "CREATE OR REPLACE PACKAGE BODY lume_pkg AS\nFUNCTION f(x NUMBER) RETURN NUMBER IS\nBEGIN\n  RETURN x * undefined_thing;\nEND;\nEND lume_pkg;";
    ObjPlan p = plan_for(db, e, &kb, &bb, badb, false, NULL, 0);
    ObjJob *aj = apply(db, e, &kb, &p, bb.fingerprint);
    bool pls = false; for (int i = 0; i < aj->snap.nerrors; i++) if (strstr(aj->snap.errors[i].code, "PLS-") && aj->snap.errors[i].line == 4) pls = true;
    CHECK(aj->compiled_invalid && !strcmp(aj->snap.status, "INVALID") && pls, "AT-05: DDL ran, status read back INVALID with PLS- error on line 4 (%d error(s))", aj->snap.nerrors);
    ObjSnapshot inval = aj->snap; memset(&aj->snap, 0, sizeof aj->snap);
    objsrc_free(aj); objplan_free(&p);
    const char *good = "CREATE OR REPLACE PACKAGE BODY lume_pkg AS\nFUNCTION f(x NUMBER) RETURN NUMBER IS\nBEGIN\n  RETURN x * 3;\nEND;\nEND lume_pkg;";
    p = plan_for(db, e, &kb, &inval, good, false, NULL, 0);
    aj = apply(db, e, &kb, &p, inval.fingerprint);
    char *v = scalar(db, "SELECT lume_pkg.f(2) FROM dual");
    CHECK(aj->state == OJS_DONE && !aj->compiled_invalid && !strcmp(aj->snap.status, "VALID") && !strcmp(v, "6"), "Oracle: fixed body compiles VALID, f(2) = %s", v);
    free(v); objsrc_free(aj); objplan_free(&p); objsnap_free(&inval); objsnap_free(&bb);
    run(db, "DROP PACKAGE lume_pkg");
    db_close(db);
}
#endif

int main(void) {
#ifndef NO_SQLITE
    sqlite_tests();
    hostile_adapter();
#endif
#ifndef WITH_ORACLE
    pg_tests();
    mysql_tests("LUME_MYSQL_LOC", "mysql", false);
    mysql_tests("LUME_MARIADB_LOC", "mariadb", true);
    mssql_tests();
#else
    oracle_tests();
#endif
    printf("%s: %d ok, %d failed\n", fails ? "OBJECTS DB FAILED" : "OBJECTS DB OK", oks, fails);
    return fails ? 1 : 0;
}
