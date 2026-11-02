/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Oracle adapter for the object source editor (OBJ-010 / 030 / 050 / 051 / 057)
 * ==============================================================================
 *
 * PL/SQL units (function, procedure, package spec / body, trigger, type spec / body) come from
 * ALL_SOURCE, line by line, as "PACKAGE BODY x AS ..." without CREATE: the editable DDL is
 * "CREATE OR REPLACE " + that text. Views and materialized views come from DBMS_METADATA (a CLOB,
 * never a truncated LONG). Specification and body are separate objects with their own status.
 *
 * Every DDL commits on its own. After Apply the status (ALL_OBJECTS.STATUS) and the compiler
 * errors (ALL_ERRORS) are read back: "the DDL ran" is never reported as "compiled" (AT-05).
 * A wrapped unit is shown read-only (its source is not readable).
 */
#include "objplan_int.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *cell(const ResultSet *rs, int r, int c) { const char *v = r < rs->nrows && c < rs->ncols ? rs_cell(rs, r, c) : NULL; return v ? v : ""; }

/* ALL_SOURCE / ALL_OBJECTS type name of a kind */
static const char *ora_type(int k) {
    switch (k) {
    case OBJK_VIEW: return "VIEW"; case OBJK_MATVIEW: return "MATERIALIZED VIEW";
    case OBJK_FUNCTION: return "FUNCTION"; case OBJK_PROCEDURE: return "PROCEDURE";
    case OBJK_PKG_SPEC: return "PACKAGE"; case OBJK_PKG_BODY: return "PACKAGE BODY";
    case OBJK_TRIGGER: return "TRIGGER"; case OBJK_TYPE_SPEC: return "TYPE"; case OBJK_TYPE_BODY: return "TYPE BODY";
    default: return "";
    }
}
static int kind_of(const char *t) {
    for (int k = 0; k < OBJK_COUNT; k++) if (!strcmp(ora_type(k), t)) return k;
    return -1;
}

static ObjCaps ora_caps(int k, int ver) {
    ObjCaps c; memset(&c, 0, sizeof c);
    c.available = true;
    if (k < 0 || k > OBJK_TYPE_BODY) return c;   /* no operator / event trigger / virtual table kinds here */
    bool plsql = k != OBJK_VIEW && k != OBJK_MATVIEW;
    c.source_read = c.source_edit = c.format = c.static_validate = true;
    c.server_compile = plsql;
    c.replace_in_place = k != OBJK_MATVIEW;
    c.dependency_graph = true;
    c.fingerprint = true;
    c.refresh = k == OBJK_MATVIEW;
    c.enable_disable = k == OBJK_TRIGGER;
    return c;
}

static void ora_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    const ObjKey *key = in->key; const ObjSnapshot *base = in->base;
    char qn[600]; obj_qname(key, DIALECT_ORACLE, qn, sizeof qn);
    p->atomicity = ATOM_AUTOCOMMIT;
    if (in->tx_open) plan_block(p, "The session has an open transaction: Oracle DDL would commit it. Commit or roll back first.");
    if (key->kind == OBJK_MATVIEW) {
        p->destructive = true; p->risk = RISK_DESTRUCTIVE;
        snprintf(p->strategy, sizeof p->strategy, "DROP MATERIALIZED VIEW + CREATE (explicit)");
        if (!in->allow_destructive) { p->destructive_available = true; plan_block(p, "A materialized view cannot be replaced in place: review the explicit drop + create plan."); }
        plan_sql(p, "DROP MATERIALIZED VIEW %s", qn);
        plan_create(p, in->local + cs, obj_trim_semicolon(in->local, cs, ce) - cs);
        plan_impact(p, "Between the DROP and the CREATE the materialized view does not exist; each DDL commits on its own.");
        if (base) for (int i = 0; i < base->nrecreate; i++) plan_sql(p, "%s", base->recreate[i]);
    } else {
        if (!h->or_replace) plan_block(p, "Write CREATE OR REPLACE: a plain CREATE fails with ORA-00955 because the object exists.");
        snprintf(p->strategy, sizeof p->strategy, "CREATE OR REPLACE %s, then the compile status is read", objkind_name(key->kind));
        int e2 = key->kind == OBJK_VIEW ? obj_trim_semicolon(in->local, cs, ce) : ce;   /* PL/SQL keeps its END; */
        plan_create(p, in->local + cs, e2 - cs);
        p->risk = (key->kind == OBJK_PKG_SPEC || key->kind == OBJK_TYPE_SPEC || (base && base->ndeps > 0)) ? RISK_POTENTIAL : RISK_NON_DESTRUCTIVE;
        if (key->kind == OBJK_PKG_SPEC) plan_impact(p, "Changing the specification invalidates the package body and every dependent until they recompile.");
        if (key->kind == OBJK_TYPE_SPEC) plan_impact(p, "A type with dependent types or tables cannot be replaced (ORA-02303).");
        plan_impact(p, "DDL commits immediately: there is no rollback. Grants are kept by CREATE OR REPLACE.");
        plan_pre(p, "After running, the status (VALID / INVALID) and the compilation errors are read from the catalog; success is never assumed from the DDL alone.");
    }
    plan_deps_impact(p, base, "applying");
}

static void lits(const ObjKey *k, char *o, char *n, int sz) { obj_qlit(k->schema, o, sz); obj_qlit(k->name, n, sz); }

static void status_stmts(const ObjKey *k, ObjSqlBuf *o) {
    char ow[300], nm[300]; lits(k, ow, nm, sizeof ow);
    obj_sql_add(o, "SELECT status, TO_CHAR(last_ddl_time, 'YYYY-MM-DD HH24:MI:SS') FROM all_objects WHERE owner = %s AND object_name = %s AND object_type = '%s'", ow, nm, ora_type(k->kind));
    obj_sql_add(o, "SELECT line, position, text, attribute, message_number FROM all_errors WHERE owner = %s AND name = %s AND type = '%s' ORDER BY sequence", ow, nm, ora_type(k->kind));
}

static bool ora_load_sql(const ObjKey *k, ObjSqlBuf *o) {
    char ow[300], nm[300]; lits(k, ow, nm, sizeof ow);
    if (k->kind == OBJK_VIEW || k->kind == OBJK_MATVIEW)
        obj_sql_add(o, "SELECT DBMS_METADATA.GET_DDL('%s', %s, %s) FROM dual", k->kind == OBJK_VIEW ? "VIEW" : "MATERIALIZED_VIEW", nm, ow);
    else
        obj_sql_add(o, "SELECT text FROM all_source WHERE owner = %s AND name = %s AND type = '%s' ORDER BY line", ow, nm, ora_type(k->kind));
    status_stmts(k, o);
    /* dependents: a body depends on its own spec, which is not a dependent to report */
    const char *ref = k->kind == OBJK_PKG_BODY ? "PACKAGE" : k->kind == OBJK_TYPE_BODY ? "TYPE" : ora_type(k->kind);
    obj_sql_add(o, "SELECT owner || '.' || name, type FROM all_dependencies WHERE referenced_owner = %s AND referenced_name = %s AND referenced_type = '%s' "
                   "AND NOT (owner = %s AND name = %s) ORDER BY 1", ow, nm, ref, ow, nm);
    if (k->kind == OBJK_VIEW)
        obj_sql_add(o, "SELECT column_name, data_type FROM all_tab_columns WHERE owner = %s AND table_name = %s ORDER BY column_id", ow, nm);
    return true;
}

static bool ora_status_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s) {
    if (n < 2 || !rs[0]->ok || !rs[1]->ok) return false;
    snprintf(s->status, sizeof s->status, "%s", cell(rs[0], 0, 0));
    free(s->errors); s->errors = NULL; s->nerrors = 0;
    if (rs[1]->nrows) {
        s->errors = calloc((size_t)rs[1]->nrows, sizeof *s->errors);
        for (int i = 0; i < rs[1]->nrows; i++) {
            ObjError *e = &s->errors[s->nerrors++];
            e->line = atoi(cell(rs[1], i, 0)); e->col = atoi(cell(rs[1], i, 1));
            snprintf(e->text, sizeof e->text, "%s", cell(rs[1], i, 2));
            /* "PLS-00302: component 'X' must be declared": the code is the part before ':' */
            const char *colon = strchr(e->text, ':');
            if (colon && colon - e->text < (int)sizeof e->code) snprintf(e->code, sizeof e->code, "%.*s", (int)(colon - e->text), e->text);
            else snprintf(e->code, sizeof e->code, "%s-%s", cell(rs[1], i, 3), cell(rs[1], i, 4));
            snprintf(e->unit, sizeof e->unit, "%s", k->kind == OBJK_PKG_BODY || k->kind == OBJK_TYPE_BODY ? "BODY" : k->kind == OBJK_PKG_SPEC || k->kind == OBJK_TYPE_SPEC ? "SPEC" : "MAIN");
        }
    }
    return true;
}

static bool ora_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) {
    memset(s, 0, sizeof *s);
    if (n < 1) { snprintf(err, (size_t)errn, "no result"); return false; }
    if (!rs[0]->ok) {
        /* DBMS_METADATA raises ORA-31603 when the object does not exist (or is not visible) */
        if (strstr(rs[0]->error, "ORA-31603")) { s->exists = false; return true; }
        snprintf(err, (size_t)errn, "%s", rs[0]->error); return false;
    }
    if (k->kind == OBJK_VIEW || k->kind == OBJK_MATVIEW) {
        s->exists = rs[0]->nrows > 0;
        if (!s->exists) return true;
        const char *ddl = cell(rs[0], 0, 0);
        while (isspace((unsigned char)*ddl)) ddl++;
        s->original = strdup(ddl);
        /* GET_DDL writes CREATE OR REPLACE [FORCE] [EDITIONABLE] VIEW for views */
        s->ddl = strdup(ddl);
    } else {
        s->exists = rs[0]->nrows > 0;
        if (!s->exists) return true;
        size_t L = 1; for (int i = 0; i < rs[0]->nrows; i++) L += strlen(cell(rs[0], i, 0));
        char *src = malloc(L); size_t o = 0;
        for (int i = 0; i < rs[0]->nrows; i++) { const char *t = cell(rs[0], i, 0); size_t l = strlen(t); memcpy(src + o, t, l); o += l; }
        while (o > 0 && isspace((unsigned char)src[o - 1])) o--;
        src[o] = 0;
        s->original = src;
        s->ddl = malloc(o + 20); snprintf(s->ddl, o + 20, "CREATE OR REPLACE %s", src);
        /* "PACKAGE BODY x wrapped": the source is obfuscated */
        char first[200]; snprintf(first, sizeof first, "%.*s", (int)(strchr(src, '\n') ? strchr(src, '\n') - src : (long)o), src);
        for (char *p = first; *p; p++) *p = (char)tolower((unsigned char)*p);
        if (strstr(first, " wrapped")) s->encrypted = true;
    }
    if (n >= 3) ora_status_parse(k, rs + 1, 2, s);
    char fp[64] = ""; if (n >= 2 && rs[1]->ok) snprintf(fp, sizeof fp, "%s", cell(rs[1], 0, 1));
    s->fingerprint = obj_hash(s->original, -1) ^ obj_hash(fp, -1);
    if (n >= 4 && rs[3]->ok) {
        s->deps_known = true; s->deps = calloc((size_t)(rs[3]->nrows + 1), sizeof *s->deps);
        for (int i = 0; i < rs[3]->nrows; i++) { ObjDep *d = &s->deps[s->ndeps++]; snprintf(d->name, sizeof d->name, "%s", cell(rs[3], i, 0)); snprintf(d->kind, sizeof d->kind, "%s", cell(rs[3], i, 1)); snprintf(d->how, sizeof d->how, "catalog"); }
    }
    if (n >= 5 && rs[4]->ok && rs[4]->nrows) {
        s->cols = calloc((size_t)rs[4]->nrows, sizeof *s->cols); s->ncols = rs[4]->nrows;
        for (int i = 0; i < rs[4]->nrows; i++) { snprintf(s->cols[i].name, sizeof s->cols[i].name, "%s", cell(rs[4], i, 0)); snprintf(s->cols[i].type, sizeof s->cols[i].type, "%s", cell(rs[4], i, 1)); }
    }
    return true;
}

static bool ora_status_sql(const ObjKey *k, ObjSqlBuf *o) { status_stmts(k, o); return true; }

static bool ora_list_sql(const char *schema, ObjSqlBuf *o) {
    char ow[300];
    if (schema && schema[0]) obj_qlit(schema, ow, sizeof ow); else snprintf(ow, sizeof ow, "SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA')");
    obj_sql_add(o, "SELECT o.object_type, o.object_name, o.status, NVL(t.table_name, ''), o.owner FROM all_objects o "
                   "LEFT JOIN all_triggers t ON t.owner = o.owner AND t.trigger_name = o.object_name AND o.object_type = 'TRIGGER' "
                   "WHERE o.owner = %s AND o.object_type IN ('VIEW', 'MATERIALIZED VIEW', 'FUNCTION', 'PROCEDURE', 'PACKAGE', 'PACKAGE BODY', 'TRIGGER', 'TYPE', 'TYPE BODY') "
                   "ORDER BY o.object_type, o.object_name", ow);
    return true;
}
static int ora_list_parse(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max) {
    if (n < 1 || !rs[0]->ok) return 0;
    int m = 0;
    for (int i = 0; i < rs[0]->nrows && m < max; i++) {
        int k = kind_of(cell(rs[0], i, 0)); if (k < 0) continue;
        ObjListItem *it = &out[m++]; memset(it, 0, sizeof *it);
        snprintf(it->key.conn_id, sizeof it->key.conn_id, "%s", conn_id);
        it->key.kind = k;
        snprintf(it->key.name, sizeof it->key.name, "%s", cell(rs[0], i, 1));
        snprintf(it->status, sizeof it->status, "%s", cell(rs[0], i, 2));
        snprintf(it->key.table, sizeof it->key.table, "%s", cell(rs[0], i, 3));
        snprintf(it->key.schema, sizeof it->key.schema, "%s", cell(rs[0], i, 4));
    }
    return m;
}

const ObjEngine obj_engine_oracle = {
    .api_version = OBJ_ENGINE_API, .id = "oracle", .dialect = DIALECT_ORACLE, .has_driver = true,
    .caps = ora_caps, .plan = ora_plan,
    .load_sql = ora_load_sql, .load_parse = ora_load_parse,
    .list_sql = ora_list_sql, .list_parse = ora_list_parse,
    .status_sql = ora_status_sql, .status_parse = ora_status_parse,
};
