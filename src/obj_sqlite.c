/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * SQLite adapter for the object source editor (OBJ-046 / 055)
 * ===========================================================
 *
 * SQLite stores views and triggers only, as the CREATE text the user wrote (sqlite_master.sql),
 * so the source is complete executable DDL. There is no OR REPLACE: an edit is DROP + CREATE in
 * one transaction, and dropping a view also drops its INSTEAD OF triggers, so they are read with
 * the view and re-created after it. Dependents are found by text match (approximate: SQLite
 * keeps no dependency catalog).
 */
#include "objplan_int.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *cell(const ResultSet *rs, int r, int c) { const char *v = r < rs->nrows && c < rs->ncols ? rs_cell(rs, r, c) : NULL; return v ? v : ""; }

static ObjCaps sqlite_caps(int k, int ver) {
    ObjCaps c; memset(&c, 0, sizeof c);
    c.available = true;
    if (k == OBJK_VTABLE) { c.source_read = true; return c; }   /* shown with its module, read-only (OBJ-058) */
    if (k != OBJK_VIEW && k != OBJK_TRIGGER) return c;   /* no routines, packages or types */
    c.source_read = c.source_edit = c.format = c.static_validate = true;
    c.server_validate = k == OBJK_VIEW;                  /* compiled as a temporary view, rolled back */
    c.transactional_ddl = true;
    c.fingerprint = true;
    return c;
}

static void sqlite_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    const ObjKey *key = in->key; const ObjSnapshot *base = in->base;
    char qn[600]; obj_qname(key, DIALECT_SQLITE, qn, sizeof qn);
    if (h->or_replace) plan_block(p, "SQLite has no CREATE OR REPLACE: write CREATE %s; Lume drops and re-creates it in one transaction.", objkind_name(key->kind));
    p->atomicity = ATOM_TRANSACTIONAL;
    p->risk = RISK_POTENTIAL;
    snprintf(p->strategy, sizeof p->strategy, "DROP + CREATE in one transaction");
    plan_sql(p, "BEGIN");
    plan_sql(p, "DROP %s %s", key->kind == OBJK_VIEW ? "VIEW" : "TRIGGER", qn);
    plan_create(p, in->local + cs, ce - cs);
    if (base) for (int i = 0; i < base->nrecreate; i++) plan_sql(p, "%s", base->recreate[i]);
    if (key->kind == OBJK_VIEW) {
        /* SQLite accepts a view naming missing tables or columns and fails only when it is used:
         * compile it here, inside the transaction, so a broken view is rolled back, not published */
        char nm[300], sch[300]; obj_qlit(key->name, nm, sizeof nm); obj_qlit(key->schema[0] ? key->schema : "main", sch, sizeof sch);
        plan_sql(p, "SELECT count(*) FROM pragma_table_info(%s, %s)", nm, sch);
        plan_pre(p, "The new view is compiled inside the transaction before COMMIT (SQLite would otherwise accept missing tables or columns).");
    }
    plan_sql(p, "COMMIT");
    plan_impact(p, "SQLite cannot replace a %s in place: it is dropped and created again; a failure rolls everything back.", objkind_word(key->kind));
    if (key->kind == OBJK_VIEW && base && base->nrecreate) plan_impact(p, "%d INSTEAD OF trigger(s) of the view are dropped with it and re-created after it.", base->nrecreate);
    if (key->kind == OBJK_VIEW) plan_impact(p, "Views and triggers that reference it are not checked by SQLite at DROP time; they work again once it exists.");
    plan_deps_impact(p, base, "applying");
}

/* "main".sqlite_master for the key's schema (attached databases have their own). */
static void master(const ObjKey *k, char *out, int n) {
    char q[300]; ObjKey s; memset(&s, 0, sizeof s);
    snprintf(s.name, sizeof s.name, "%s", k->schema[0] ? k->schema : "main");
    obj_qname(&s, DIALECT_SQLITE, q, sizeof q);
    snprintf(out, (size_t)n, "%s.sqlite_master", q);
}

static bool sqlite_load_sql(const ObjKey *k, ObjSqlBuf *o) {
    char m[400], nm[300], sch[300]; master(k, m, sizeof m); obj_qlit(k->name, nm, sizeof nm); obj_qlit(k->schema[0] ? k->schema : "main", sch, sizeof sch);
    const char *type = k->kind == OBJK_VIEW ? "view" : k->kind == OBJK_VTABLE ? "table" : "trigger";
    obj_sql_add(o, "SELECT type, name, tbl_name, sql FROM %s WHERE type = '%s' AND name = %s", m, type, nm);
    if (k->kind == OBJK_VIEW) {
        obj_sql_add(o, "SELECT name, type FROM pragma_table_info(%s, %s) ORDER BY cid", nm, sch);
        /* INSTEAD OF triggers live with the view and are dropped with it */
        obj_sql_add(o, "SELECT name, sql FROM %s WHERE type = 'trigger' AND tbl_name = %s ORDER BY name", m, nm);
    }
    /* dependents by text (SQLite has no dependency catalog) */
    obj_sql_add(o, "SELECT type, name FROM %s WHERE type IN ('view', 'trigger') AND name <> %s AND tbl_name <> %s AND instr(lower(sql), lower(%s)) > 0 ORDER BY type, name", m, nm, nm, nm);
    return true;
}

static bool sqlite_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) {
    memset(s, 0, sizeof *s);
    if (n < 1 || !rs[0]->ok) { snprintf(err, (size_t)errn, "%s", n >= 1 ? rs[0]->error : "no result"); return false; }
    s->exists = rs[0]->nrows > 0;
    if (!s->exists) return true;
    const char *sql = cell(rs[0], 0, 3);
    s->original = strdup(sql); s->ddl = strdup(sql);
    s->fingerprint = obj_hash(sql, -1);
    if (k->kind == OBJK_TRIGGER) snprintf(s->options, sizeof s->options, "ON %s", cell(rs[0], 0, 2));
    if (k->kind == OBJK_VTABLE) {
        /* "CREATE VIRTUAL TABLE t USING fts5(...)": the module owns the definition */
        const char *u = strcasestr(sql, " USING "); char mod[64] = "?";
        if (u) { u += 7; while (*u == ' ') u++; int l = 0; while (u[l] && (isalnum((unsigned char)u[l]) || u[l] == '_') && l < 63) l++; snprintf(mod, sizeof mod, "%.*s", l, u); }
        s->system = true;
        snprintf(s->ro_reason, sizeof s->ro_reason, "a virtual table of module %s: its storage belongs to the module; change it by creating a new table", mod);
        snprintf(s->options, sizeof s->options, "module %s", mod);
    }
    int next = 1;
    if (k->kind == OBJK_VIEW) {
        if (n > 1 && rs[1]->ok && rs[1]->nrows) {
            s->cols = calloc((size_t)rs[1]->nrows, sizeof *s->cols); s->ncols = rs[1]->nrows;
            for (int i = 0; i < rs[1]->nrows; i++) { snprintf(s->cols[i].name, sizeof s->cols[i].name, "%s", cell(rs[1], i, 0)); snprintf(s->cols[i].type, sizeof s->cols[i].type, "%s", cell(rs[1], i, 1)); }
        }
        if (n > 2 && rs[2]->ok && rs[2]->nrows) {
            s->recreate = calloc((size_t)rs[2]->nrows, sizeof *s->recreate); s->nrecreate = rs[2]->nrows;
            for (int i = 0; i < rs[2]->nrows; i++) s->recreate[i] = strdup(cell(rs[2], i, 1));
        }
        next = 3;
    }
    if (n > next && rs[next]->ok) {
        s->deps_known = true;
        s->deps = calloc((size_t)(rs[next]->nrows ? rs[next]->nrows : 1), sizeof *s->deps);
        for (int i = 0; i < rs[next]->nrows; i++) {
            ObjDep *d = &s->deps[s->ndeps++];
            snprintf(d->kind, sizeof d->kind, "%s", cell(rs[next], i, 0)); snprintf(d->name, sizeof d->name, "%s", cell(rs[next], i, 1));
            snprintf(d->how, sizeof d->how, "approximate");
        }
    }
    return true;
}

static bool sqlite_list_sql(const char *schema, ObjSqlBuf *o) {
    ObjKey k; memset(&k, 0, sizeof k); snprintf(k.schema, sizeof k.schema, "%s", schema && schema[0] ? schema : "main");
    char m[400]; master(&k, m, sizeof m);
    obj_sql_add(o, "SELECT CASE WHEN type = 'table' THEN 'vtable' ELSE type END, name, tbl_name FROM %s WHERE (type IN ('view', 'trigger') OR (type = 'table' AND sql LIKE 'CREATE VIRTUAL TABLE%%')) "
                   "AND name NOT LIKE 'sqlite\\_%%' ESCAPE '\\' ORDER BY type DESC, name", m);
    return true;
}
static int sqlite_list_parse(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max) {
    if (n < 1 || !rs[0]->ok) return 0;
    int m = 0;
    for (int i = 0; i < rs[0]->nrows && m < max; i++) {
        ObjListItem *it = &out[m++]; memset(it, 0, sizeof *it);
        snprintf(it->key.conn_id, sizeof it->key.conn_id, "%s", conn_id);
        snprintf(it->key.schema, sizeof it->key.schema, "main");
        const char *ty = cell(rs[0], i, 0);
        it->key.kind = !strcmp(ty, "view") ? OBJK_VIEW : !strcmp(ty, "vtable") ? OBJK_VTABLE : OBJK_TRIGGER;
        if (it->key.kind == OBJK_VTABLE) it->system = true;
        snprintf(it->key.name, sizeof it->key.name, "%s", cell(rs[0], i, 1));
        if (it->key.kind == OBJK_TRIGGER) snprintf(it->key.table, sizeof it->key.table, "%s", cell(rs[0], i, 2));
    }
    return m;
}

/* The query compiled as a temporary view (inside BEGIN ... ROLLBACK added by the core). */
static bool sqlite_describe_sql(const char *q, int len, ObjSqlBuf *o) {
    obj_sql_add(o, "CREATE TEMP VIEW lume_describe AS %.*s", len, q);
    obj_sql_add(o, "SELECT name, type FROM pragma_table_info('lume_describe', 'temp') ORDER BY cid");
    return true;
}

const ObjEngine obj_engine_sqlite = {
    .api_version = OBJ_ENGINE_API, .id = "sqlite", .dialect = DIALECT_SQLITE, .has_driver = true,
    .caps = sqlite_caps, .plan = sqlite_plan,
    .load_sql = sqlite_load_sql, .load_parse = sqlite_load_parse,
    .list_sql = sqlite_list_sql, .list_parse = sqlite_list_parse,
    .describe_sql = sqlite_describe_sql,
};
