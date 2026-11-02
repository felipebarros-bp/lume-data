/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object source provider and deployment service: see objsrc.h. */
#include "objsrc.h"
#include "objplan_int.h"
#include "sqllex.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

const char *objsrc_state_name(int s) {
    switch (s) { case OJS_RUNNING: return "running"; case OJS_DONE: return "done"; case OJS_FAILED: return "failed"; case OJS_CANCELLED: return "cancelled"; default: return "unknown"; }
}

bool objsrc_connection_lost(const char *e) {
    static const char *pat[] = { "server closed the connection", "connection to server", "no connection to the server", "terminating connection",
        "could not receive data", "could not send data", "SSL SYSCALL", "connection lost", "connection reset", "broken pipe",
        "ORA-03113", "ORA-03114", "ORA-03135", "ORA-12537", "ORA-12547", "DPI-1080", NULL };
    if (!e || !e[0]) return false;
    for (int i = 0; pat[i]; i++) if (strcasestr(e, pat[i])) return true;
    return false;
}

/* Catalog SQL from an adapter must be one plain read: a SELECT (or WITH ... SELECT) without any
 * writing keyword and without FOR UPDATE. Anything else is refused (OBJ-080). */
static bool catalog_read_only(const char *sql) {
    int len = (int)strlen(sql);
    SqlTok *t = malloc(sizeof *t * (size_t)(len + 1));
    int n = sql_lex(sql, len, DIALECT_GENERIC, t, len + 1), first = -1;
    bool ok = true;
    static const char *bad[] = { "INSERT", "UPDATE", "DELETE", "MERGE", "CREATE", "DROP", "ALTER", "TRUNCATE", "GRANT", "REVOKE",
                                 "COMMIT", "ROLLBACK", "CALL", "EXEC", "EXECUTE", "COPY", "LOCK", "VACUUM", "REINDEX", "ATTACH", "DETACH", "PRAGMA", NULL };
    for (int i = 0; i < n && ok; i++) {
        if (t[i].kind == TK_WS || t[i].kind == TK_COMMENT) continue;
        if (first < 0) {
            first = i;
            /* SHOW CREATE <kind> is a read (MySQL / MariaDB keep full DDL only there) */
            if (tok_is(sql, &t[i], "SHOW")) { int j = i + 1; while (j < n && t[j].kind == TK_WS) j++; if (j < n && tok_is(sql, &t[j], "CREATE")) { free(t); return !strchr(sql, ';'); } ok = false; continue; }
            if (!tok_is(sql, &t[i], "SELECT") && !tok_is(sql, &t[i], "WITH")) ok = false;
            continue;
        }
        if (t[i].kind == TK_SEMI) { for (int j = i + 1; j < n; j++) if (t[j].kind != TK_WS && t[j].kind != TK_COMMENT) ok = false; }
        if (t[i].kind == TK_WORD) {
            for (int b = 0; bad[b]; b++) if (tok_is(sql, &t[i], bad[b])) {
                /* "FOR UPDATE" locks; "pragma_table_info(...)" is a table-valued read in SQLite */
                if (!strcasecmp(bad[b], "UPDATE") || i + 1 >= n || t[i + 1].kind != TK_LPAREN) ok = false;
            }
        }
    }
    free(t);
    return ok && first >= 0;
}

static ObjJob *job_new(int kind, Db *db, const ObjEngine *e) {
    ObjJob *j = calloc(1, sizeof *j);
    j->kind = kind; j->db = db; j->eng = e; j->state = OJS_RUNNING; j->failed_at = -1; j->failed_offset = -1;
    j->plan.create_index = -1;
    j->started = now_s();
    return j;
}
static void fail(ObjJob *j, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(j->err, sizeof j->err, fmt, ap); va_end(ap);
    j->state = OJS_FAILED;
}
/* starts the statements of b (read-only checked unless allow_write); false + state FAILED otherwise */
static bool start(ObjJob *j, ObjSqlBuf *b, bool check, bool finally_last) {
    if (b->n == 0) { fail(j, "Nothing to run for %s on %s.", objkind_word(j->key.kind), j->eng->id); return false; }
    if (check) for (int i = 0; i < b->n; i++) if (!catalog_read_only(b->sql[i])) {
        fail(j, "Refused: catalog SQL from the %s adapter is not a plain read (statement %d).", j->eng->id, i + 1);
        return false;
    }
    if (!j->db) { fail(j, "Not connected."); return false; }
    j->ex = exec_start_list(j->db, (const char *const *)b->sql, b->n, 0, true, finally_last);
    if (!j->ex) { fail(j, "The connection is busy with another execution; try again when it finishes."); return false; }
    return true;
}
/* moves every result of a finished exec job out; returns how many */
static int take_all(ExecJob *ex, ResultSet *rs, int max) {
    int n = ex->n < max ? ex->n : max;
    for (int i = 0; i < n; i++) { rs_init(&rs[i]); exec_take_result(ex, i, &rs[i]); }
    return n;
}

static void start_run(ObjJob *j);

ObjJob *objsrc_load(Db *db, const ObjEngine *e, const ObjKey *k) {
    ObjJob *j = job_new(OJ_LOAD, db, e); j->key = *k;
    if (!e || !e->load_sql || !e->load_parse) { fail(j, "Reading the source is not supported on %s.", e ? dialect_name(e->dialect) : "this engine"); return j; }
    ObjSqlBuf b = { 0 };
    if (!e->load_sql(k, &b)) { obj_sql_free(&b); fail(j, "Reading a %s is not supported on %s.", objkind_word(k->kind), dialect_name(e->dialect)); return j; }
    start(j, &b, true, false);
    obj_sql_free(&b);
    return j;
}

ObjJob *objsrc_list(Db *db, const ObjEngine *e, const char *schema, const char *conn_id) {
    ObjJob *j = job_new(OJ_LIST, db, e);
    snprintf(j->key.schema, sizeof j->key.schema, "%s", schema ? schema : "");
    snprintf(j->key.conn_id, sizeof j->key.conn_id, "%s", conn_id ? conn_id : "");
    if (!e || !e->list_sql) { fail(j, "Listing objects is not supported on %s.", e ? dialect_name(e->dialect) : "this engine"); return j; }
    ObjSqlBuf b = { 0 }; e->list_sql(schema, &b);
    start(j, &b, true, false);
    obj_sql_free(&b);
    return j;
}

ObjJob *objsrc_describe(Db *db, const ObjEngine *e, const char *q, int len) {
    ObjJob *j = job_new(OJ_DESCRIBE, db, e);
    ObjCaps c = e ? e->caps(OBJK_VIEW, 0) : (ObjCaps){ 0 };
    if (!e || !e->describe_sql || !c.transactional_ddl) { fail(j, "Server validation of a view query is not offered on %s.", e ? dialect_name(e->dialect) : "this engine"); return j; }
    if (j->db && j->db->drv->tx_state && j->db->drv->tx_state(j->db) == 1) { fail(j, "The session has an open transaction: commit or roll back before validating on the server."); return j; }
    /* the core's envelope: whatever the adapter writes is rolled back, always */
    ObjSqlBuf inner = { 0 }; e->describe_sql(q, len, &inner);
    ObjSqlBuf b = { 0 };
    obj_sql_add(&b, "BEGIN");
    for (int i = 0; i < inner.n && b.n < OBJ_SQL_MAX - 1; i++) obj_sql_add(&b, "%s", inner.sql[i]);
    obj_sql_add(&b, "ROLLBACK");
    obj_sql_free(&inner);
    start(j, &b, false, true);
    obj_sql_free(&b);
    return j;
}

ObjJob *objsrc_apply(Db *db, const ObjEngine *e, const ObjKey *k, const ObjPlan *p, uint64_t expect_fp) {
    ObjJob *j = job_new(OJ_APPLY, db, e); j->key = *k; j->expect_fp = expect_fp;
    if (!e || !e->has_driver) { fail(j, "Lume has no %s driver: export the script instead.", e ? dialect_name(e->dialect) : ""); return j; }
    if (p->blocked) { fail(j, "Refused: %s", p->block_reason); return j; }
    if (p->nsql == 0) { fail(j, "The plan has no statement."); return j; }
    if (j->db && j->db->drv->tx_state && j->db->drv->tx_state(j->db) == 1) { fail(j, "The session has an open transaction: commit or roll back before publishing."); return j; }
    /* own copy of the plan */
    j->plan = *p;
    for (int i = 0; i < p->nsql; i++) j->plan.sql[i] = strdup(p->sql[i]);
    j->transactional = p->atomicity == ATOM_TRANSACTIONAL && p->nsql >= 2 && !strcasecmp(p->sql[0], "BEGIN");
    /* precheck: the definition read again (and the routine identity on PostgreSQL) */
    j->phase = OJP_PRECHECK;
    ObjSqlBuf b = { 0 };
    if (e->load_sql) e->load_sql(k, &b);
    if (p->probe_signature[0] && e->dialect == DIALECT_PG && b.n < OBJ_SQL_MAX) {
        char lit[800]; obj_qlit(p->probe_signature, lit, sizeof lit);
        obj_sql_add(&b, "SELECT coalesce(to_regprocedure(%s)::oid::text, '')", lit);
    }
    if (b.n == 0) { obj_sql_free(&b); start_run(j); return j; }
    start(j, &b, true, false);
    obj_sql_free(&b);
    return j;
}

/* starts the run phase: the plan, plus a ROLLBACK that always runs for transactional plans */
static void start_run(ObjJob *j) {
    j->phase = OJP_RUN;
    const char *list[PLAN_MAX_SQL + 1]; int n = 0;
    for (int i = 0; i < j->plan.nsql; i++) list[n++] = j->plan.sql[i];
    if (j->transactional) list[n++] = "ROLLBACK";   /* after COMMIT a no-op; after a failure it undoes everything */
    j->ex = exec_start_list(j->db, list, n, 0, true, j->transactional);
    if (!j->ex) fail(j, "The connection became busy before publishing; nothing ran.");
}
static void start_refresh(ObjJob *j) {
    j->phase = OJP_REFRESH;
    ObjSqlBuf b = { 0 };
    if (j->eng->load_sql) j->eng->load_sql(&j->key, &b);
    if (b.n == 0) { j->phase = OJP_END; j->state = OJS_DONE; return; }
    j->ex = exec_start_list(j->db, (const char *const *)b.sql, b.n, 0, false, false);
    obj_sql_free(&b);
    if (!j->ex) { j->phase = OJP_END; j->state = OJS_DONE; snprintf(j->err, sizeof j->err, "Applied; the definition could not be read back (connection busy)."); }
}

static void finish_apply_phase(ObjJob *j) {
    ResultSet rs[PLAN_MAX_SQL + 2]; int n = take_all(j->ex, rs, PLAN_MAX_SQL + 2);
    bool cancelled = exec_overall(j->ex) == 5;
    if (j->phase == OJP_PRECHECK) {
        int nload = n - (j->plan.probe_signature[0] && j->eng->dialect == DIALECT_PG ? 1 : 0);
        ResultSet *ptr[PLAN_MAX_SQL + 2]; for (int i = 0; i < n; i++) ptr[i] = &rs[i];
        ObjSnapshot now; char err[400] = "";
        if (cancelled) { j->state = OJS_CANCELLED; snprintf(j->err, sizeof j->err, "Cancelled before anything ran."); }
        else if (!j->eng->load_parse(&j->key, ptr, nload, &now, err, sizeof err)) {
            if (objsrc_connection_lost(err)) fail(j, "The connection was lost before publishing; nothing ran. %s", err);
            else fail(j, "The definition could not be read again before publishing: %s", err);
        } else {
            if (!now.exists) { j->conflict = true; fail(j, "Refused: %s no longer exists on the server.", j->key.name); }
            else if (now.fingerprint != j->expect_fp) { j->conflict = true; fail(j, "Refused: the definition changed on the server after the review. Compare again (three-way) before publishing."); }
            else if (nload < n) {
                const char *oid = rs[n - 1].ok && rs[n - 1].nrows ? rs_cell(&rs[n - 1], 0, 0) : NULL;
                if (!oid || !oid[0]) fail(j, "Refused: %s does not resolve to an existing routine; publishing would create a new one.", j->plan.probe_signature);
                else if (j->key.native_id[0] && strcmp(oid, j->key.native_id)) fail(j, "Refused: %s resolves to another routine (oid %s, edited %s).", j->plan.probe_signature, oid, j->key.native_id);
            }
            objsnap_free(&now);
        }
        for (int i = 0; i < n; i++) rs_free(&rs[i]);
        exec_free(j->ex); j->ex = NULL;
        if (j->state == OJS_RUNNING) start_run(j);
        return;
    }
    if (j->phase == OJP_RUN) {
        j->run_ms = exec_elapsed_ms(j->ex);
        int last_plan = j->plan.nsql;   /* index past the plan's own statements */
        bool lost = false;
        for (int i = 0; i < last_plan && i < n; i++) {
            int st = j->ex->stmts[i].status;
            if (st == XS_OK) { j->ran++; if (i > 0 || !j->transactional) j->ddl_ran = true; continue; }
            /* Oracle: "success with compilation error" means the DDL ran */
            if (st == XS_FAILED && strstr(rs[i].error, "ORA-24344")) { j->ran++; j->ddl_ran = true; j->compiled_invalid = true; continue; }
            if (st == XS_FAILED || st == XS_CANCELLED) {
                j->failed_at = i;
                snprintf(j->failed_error, sizeof j->failed_error, "%s", rs[i].error[0] ? rs[i].error : st == XS_CANCELLED ? "cancelled" : "failed");
                j->failed_offset = rs[i].err_offset;
                lost = objsrc_connection_lost(rs[i].error);
            }
            break;
        }
        bool all = j->ran == last_plan;
        if (all) {
            j->state = OJS_RUNNING;   /* read the definition back */
        } else if (lost && (!j->transactional || j->failed_at == last_plan - 1)) {
            /* a COMMIT or a non-transactional DDL may or may not have reached the server */
            j->state = OJS_UNKNOWN;
            snprintf(j->err, sizeof j->err, "The connection was lost during statement %d: whether the change was applied is unknown. Nothing will be retried; reconnect and compare with the server.", j->failed_at + 1);
        } else if (cancelled && !j->transactional && j->ddl_ran) {
            j->state = OJS_UNKNOWN; snprintf(j->err, sizeof j->err, "Cancelled after %d statement(s) had run; each DDL commits on its own.", j->ran);
        } else {
            j->state = cancelled ? OJS_CANCELLED : OJS_FAILED;
            snprintf(j->err, sizeof j->err, "%s at statement %d%s: %s", cancelled ? "Cancelled" : "Failed", j->failed_at + 1,
                     j->transactional ? " (rolled back: nothing changed)" : j->ddl_ran ? " (earlier statements were committed)" : " (nothing changed)", j->failed_error);
        }
        for (int i = 0; i < n; i++) rs_free(&rs[i]);
        exec_free(j->ex); j->ex = NULL;
        if (j->state == OJS_RUNNING) start_refresh(j);
        return;
    }
    if (j->phase == OJP_REFRESH) {
        ResultSet *ptr[PLAN_MAX_SQL + 2]; for (int i = 0; i < n; i++) ptr[i] = &rs[i];
        char err[400] = "";
        if (!j->eng->load_parse(&j->key, ptr, n, &j->snap, err, sizeof err)) snprintf(j->err, sizeof j->err, "Applied; reading it back failed: %s", err);
        if (j->snap.status[0] && strcasecmp(j->snap.status, "VALID")) j->compiled_invalid = true;
        if (j->snap.nerrors) j->compiled_invalid = true;
        for (int i = 0; i < n; i++) rs_free(&rs[i]);
        exec_free(j->ex); j->ex = NULL;
        j->phase = OJP_END; j->state = OJS_DONE;
    }
}

bool objsrc_poll(ObjJob *j) {
    if (!j) return true;
    if (j->state != OJS_RUNNING) return true;
    if (!j->ex || exec_running(j->ex)) return false;
    if (j->kind == OJ_APPLY) { finish_apply_phase(j); return j->state != OJS_RUNNING; }
    ResultSet rs[OBJ_SQL_MAX + 2]; int n = take_all(j->ex, rs, OBJ_SQL_MAX + 2);
    ResultSet *ptr[OBJ_SQL_MAX + 2]; for (int i = 0; i < n; i++) ptr[i] = &rs[i];
    if (exec_overall(j->ex) == 5) { j->state = OJS_CANCELLED; snprintf(j->err, sizeof j->err, "Cancelled."); }
    else if (j->kind == OJ_LOAD) {
        char err[400] = "";
        if (!j->eng->load_parse(&j->key, ptr, n, &j->snap, err, sizeof err)) fail(j, "%s", err);
        else j->state = OJS_DONE;
    } else if (j->kind == OJ_LIST) {
        int cap = n && rs[0].ok ? rs[0].nrows : 0;
        j->items = calloc((size_t)(cap ? cap : 1), sizeof *j->items);
        if (n && !rs[0].ok) fail(j, "%s", rs[0].error);
        else {
            j->nitems = j->eng->list_parse(ptr, n, j->key.conn_id, j->items, cap);
            for (int i = 0; i < j->nitems; i++) if (!j->items[i].key.schema[0]) snprintf(j->items[i].key.schema, sizeof j->items[i].key.schema, "%s", j->key.schema);
            j->state = OJS_DONE;
        }
    } else if (j->kind == OJ_DESCRIBE) {
        /* BEGIN, CREATE TEMP VIEW, SELECT columns, ROLLBACK: the server's error on the CREATE
         * is the validation result */
        for (int i = 1; i + 1 < n; i++) if (!rs[i].ok) { fail(j, "%s", rs[i].error); j->failed_offset = rs[i].err_offset; break; }
        if (j->state == OJS_RUNNING) {
            const ResultSet *c = &rs[n - 2];
            j->cols = calloc((size_t)(c->nrows ? c->nrows : 1), sizeof *j->cols); j->ncols = c->nrows;
            for (int i = 0; i < c->nrows; i++) {
                const char *a = rs_cell(c, i, 0), *b = rs_cell(c, i, 1);
                snprintf(j->cols[i].name, sizeof j->cols[i].name, "%s", a ? a : ""); snprintf(j->cols[i].type, sizeof j->cols[i].type, "%s", b ? b : "");
            }
            j->state = OJS_DONE;
        }
    }
    for (int i = 0; i < n; i++) rs_free(&rs[i]);
    exec_free(j->ex); j->ex = NULL;
    return true;
}

void objsrc_cancel(ObjJob *j) { if (j && j->ex) exec_cancel(j->ex); }

void objsrc_free(ObjJob *j) {
    if (!j) return;
    if (j->ex) exec_free(j->ex);
    objsnap_free(&j->snap); free(j->items); free(j->cols);
    for (int i = 0; i < j->plan.nsql; i++) free(j->plan.sql[i]);
    free(j);
}

bool objsrc_wait(ObjJob *j, int timeout_ms) {
    double t0 = now_s();
    while (!objsrc_poll(j)) {
        if ((now_s() - t0) * 1000 > timeout_ms) { objsrc_cancel(j); while (!objsrc_poll(j)) { struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL); } return false; }
        struct timespec ts = { 0, 2000000 }; nanosleep(&ts, NULL);
    }
    return j->state == OJS_DONE;
}
