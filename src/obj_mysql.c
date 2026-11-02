/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * MySQL, MariaDB and SQL Server adapters (OBJ-044 / 052 / 053 / 054)
 * ==================================================================
 *
 * Lume has no driver for these engines yet: the adapters edit, check and plan, and the plan is
 * export-only (has_driver = false: nothing can run). They are the reference for a future driver,
 * which only needs to add the catalog functions (load_sql / load_parse / list_*).
 *
 *   MySQL       ALTER PROCEDURE / FUNCTION cannot change a body: DROP + CREATE, with the definer,
 *               the grants lost with the routine, the window without it and the implicit commit
 *               stated; explicit consent required (AT-06). Views: CREATE OR REPLACE / ALTER VIEW.
 *   MariaDB     not a synonym of MySQL: CREATE OR REPLACE for routines and triggers when written.
 *   SQL Server  CREATE OR ALTER (2016 SP1+) or ALTER; GO only separates batches in the script.
 */
#include "objplan_int.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

static ObjCaps my_caps_common(int k, bool maria) {
    ObjCaps c; memset(&c, 0, sizeof c);
    if (k != OBJK_VIEW && k != OBJK_FUNCTION && k != OBJK_PROCEDURE && k != OBJK_TRIGGER) return c;
    c.source_edit = c.format = c.static_validate = true;
    c.replace_in_place = k == OBJK_VIEW || maria;
    return c;
}
/* with the driver: the source is read from the server, plans run (BL-013) */
static ObjCaps with_driver(ObjCaps c) { if (c.source_edit) { c.available = c.source_read = c.fingerprint = true; c.dependency_graph = false; } return c; }
static ObjCaps mysql_caps(int k, int ver) { return with_driver(my_caps_common(k, false)); }
static ObjCaps mariadb_caps(int k, int ver) {
    ObjCaps c = with_driver(my_caps_common(k, true));
    if (ver && ver < 100103 && k != OBJK_VIEW) c.replace_in_place = false;   /* OR REPLACE for routines from 10.1.3 */
    return c;
}

static void my_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    const ObjKey *key = in->key;
    const int d = in->dialect;
    bool maria = d == DIALECT_MARIADB || in->mariadb;
    char qn[600]; obj_qname(key, d, qn, sizeof qn);
    p->atomicity = ATOM_AUTOCOMMIT;
    if (in->tx_open) plan_block(p, "The session has an open transaction: MySQL DDL commits it implicitly.");
    if (key->kind == OBJK_VIEW) {
        if (!h->or_replace && !h->alter_only) plan_block(p, "Write CREATE OR REPLACE VIEW (or ALTER VIEW): the view exists.");
        snprintf(p->strategy, sizeof p->strategy, "%s VIEW", h->alter_only ? "ALTER" : "CREATE OR REPLACE");
        plan_create(p, in->local + cs, ce - cs); p->risk = RISK_NON_DESTRUCTIVE;
        plan_impact(p, "DEFINER and SQL SECURITY are taken from the text as written.");
        return;
    }
    if (maria && h->or_replace) {
        if (in->server_version && in->server_version < 100103) plan_block(p, "CREATE OR REPLACE %s needs MariaDB 10.1.3.", objkind_name(key->kind));
        snprintf(p->strategy, sizeof p->strategy, "CREATE OR REPLACE %s (MariaDB)", objkind_name(key->kind));
        plan_create(p, in->local + cs, ce - cs); p->risk = RISK_POTENTIAL;
        plan_impact(p, "MariaDB replaces the %s in one statement; check DEFINER and SQL SECURITY in the text.", objkind_word(key->kind));
        return;
    }
    p->destructive = true; p->risk = RISK_DESTRUCTIVE;
    snprintf(p->strategy, sizeof p->strategy, "DROP %s + CREATE (explicit)", objkind_name(key->kind));
    if (key->kind == OBJK_TRIGGER) plan_sql(p, "DROP TRIGGER %s", qn);
    else plan_sql(p, "DROP %s %s", objkind_name(key->kind), qn);
    plan_create(p, in->local + cs, ce - cs);
    plan_impact(p, "%s cannot change a %s body: it is dropped and created again.", maria ? "MariaDB without OR REPLACE" : "MySQL ALTER", objkind_word(key->kind));
    plan_impact(p, "Between the DROP and the CREATE the %s does not exist: calls fail in that window.", objkind_word(key->kind));
    plan_impact(p, "DEFINER and SQL SECURITY come from the text; privileges granted on the %s are lost and must be granted again.", objkind_word(key->kind));
    plan_impact(p, "Each DDL commits implicitly: a failed CREATE leaves the %s dropped.", objkind_word(key->kind));
    if (!in->allow_destructive) { p->destructive_available = true; plan_block(p, "Changing the body needs DROP + CREATE: review the explicit plan and its risks to continue."); }
}


/* ---- catalog (MySQL / MariaDB driver, BL-013) ---- */

static const char *cell(const ResultSet *rs, int r, int c) { const char *v = r < rs->nrows && c < rs->ncols ? rs_cell(rs, r, c) : NULL; return v ? v : ""; }
static void my_lit(const char *s, char *out, int n) {
    int o = 0; out[o++] = '\'';
    for (; *s && o < n - 3; s++) { if (*s == '\'' || *s == '\\') out[o++] = *s == '\\' ? '\\' : '\''; out[o++] = *s; }
    out[o++] = '\''; out[o] = 0;
}
static void schema_expr(const char *schema, char *out, int n) { if (schema && schema[0]) my_lit(schema, out, n); else snprintf(out, (size_t)n, "DATABASE()"); }

static bool my_load_sql(const ObjKey *k, ObjSqlBuf *o) {
    char q[600]; obj_qname(k, DIALECT_MYSQL, q, sizeof q);
    const char *what = k->kind == OBJK_VIEW ? "VIEW" : k->kind == OBJK_PROCEDURE ? "PROCEDURE" : k->kind == OBJK_FUNCTION ? "FUNCTION" : k->kind == OBJK_TRIGGER ? "TRIGGER" : NULL;
    if (!what) return false;
    obj_sql_add(o, "SHOW CREATE %s %s", what, q);
    char s[300], nm[300]; schema_expr(k->schema, s, sizeof s); my_lit(k->name, nm, sizeof nm);
    if (k->kind == OBJK_VIEW) {
        obj_sql_add(o, "SELECT column_name, column_type FROM information_schema.columns WHERE table_schema = %s AND table_name = %s ORDER BY ordinal_position", s, nm);
        /* views that read this one (MySQL 8.0.13+; MariaDB has no such table: the failure only means "not known") */
        obj_sql_add(o, "SELECT CONCAT(view_schema, '.', view_name), 'VIEW' FROM information_schema.view_table_usage WHERE table_schema = %s AND table_name = %s", s, nm);
    }
    return true;
}

/* "CREATE ... VIEW|PROCEDURE|..." -> "CREATE OR REPLACE ..." (the editable form) */
static char *with_or_replace(const char *ddl) {
    if (strncasecmp(ddl, "CREATE ", 7) || !strncasecmp(ddl + 7, "OR REPLACE", 10)) return strdup(ddl);
    size_t l = strlen(ddl); char *o = malloc(l + 12);
    memcpy(o, "CREATE OR REPLACE ", 18); memcpy(o + 18, ddl + 7, l - 7 + 1);
    return o;
}

static bool my_load_parse_d(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn, bool maria) {
    memset(s, 0, sizeof *s);
    if (n < 1) { snprintf(err, (size_t)errn, "no result"); return false; }
    if (!rs[0]->ok) {
        long c = rs[0]->err_code;
        if (c == 1305 || c == 1146 || c == 1360) { s->exists = false; return true; }   /* no such routine / table / trigger */
        snprintf(err, (size_t)errn, "%s", rs[0]->error); return false;
    }
    s->exists = rs[0]->nrows > 0;
    if (!s->exists) return true;
    int col = k->kind == OBJK_TRIGGER ? 2 : k->kind == OBJK_VIEW ? 1 : 2;
    const char *ddl = cell(rs[0], 0, col);
    s->original = strdup(ddl);
    bool replace = k->kind == OBJK_VIEW || maria;   /* MariaDB: OR REPLACE for routines and triggers too */
    s->ddl = replace ? with_or_replace(ddl) : strdup(ddl);
    s->fingerprint = obj_hash(ddl, -1);
    if (k->kind == OBJK_VIEW && n > 1 && rs[1]->ok && rs[1]->nrows) {
        s->cols = calloc((size_t)rs[1]->nrows, sizeof *s->cols); s->ncols = rs[1]->nrows;
        for (int i = 0; i < rs[1]->nrows; i++) { snprintf(s->cols[i].name, sizeof s->cols[i].name, "%s", cell(rs[1], i, 0)); snprintf(s->cols[i].type, sizeof s->cols[i].type, "%s", cell(rs[1], i, 1)); }
    }
    if (k->kind == OBJK_VIEW && n > 2 && rs[2]->ok) {
        s->deps_known = true; s->deps = calloc((size_t)(rs[2]->nrows + 1), sizeof *s->deps);
        for (int i = 0; i < rs[2]->nrows; i++) { ObjDep *d = &s->deps[s->ndeps++]; snprintf(d->name, sizeof d->name, "%s", cell(rs[2], i, 0)); snprintf(d->kind, sizeof d->kind, "VIEW"); snprintf(d->how, sizeof d->how, "catalog"); }
    }
    return true;
}
static bool my_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) { return my_load_parse_d(k, rs, n, s, err, errn, false); }
static bool maria_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) { return my_load_parse_d(k, rs, n, s, err, errn, true); }

static bool my_list_sql(const char *schema, ObjSqlBuf *o) {
    char s[300]; schema_expr(schema, s, sizeof s);
    obj_sql_add(o, "SELECT 'view', table_name, '', '', table_schema, '' FROM information_schema.views WHERE table_schema = %s "
                   "UNION ALL SELECT LOWER(routine_type), routine_name, '', '', routine_schema, routine_comment FROM information_schema.routines WHERE routine_schema = %s "
                   "UNION ALL SELECT 'trigger', trigger_name, event_object_table, '', trigger_schema, '' FROM information_schema.triggers WHERE trigger_schema = %s ORDER BY 1, 2", s, s, s);
    return true;
}
static int my_list_parse(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max) {
    if (n < 1 || !rs[0]->ok) return 0;
    int m = 0;
    for (int i = 0; i < rs[0]->nrows && m < max; i++) {
        const char *kd = cell(rs[0], i, 0);
        ObjListItem *it = &out[m++]; memset(it, 0, sizeof *it);
        snprintf(it->key.conn_id, sizeof it->key.conn_id, "%s", conn_id);
        it->key.kind = !strcmp(kd, "view") ? OBJK_VIEW : !strcmp(kd, "procedure") ? OBJK_PROCEDURE : !strcmp(kd, "function") ? OBJK_FUNCTION : OBJK_TRIGGER;
        snprintf(it->key.name, sizeof it->key.name, "%s", cell(rs[0], i, 1));
        snprintf(it->key.table, sizeof it->key.table, "%s", cell(rs[0], i, 2));
        snprintf(it->key.schema, sizeof it->key.schema, "%s", cell(rs[0], i, 4));
        snprintf(it->comment, sizeof it->comment, "%s", cell(rs[0], i, 5));
    }
    return m;
}


/* ---- SQL Server catalog (db_mssql.c, BL-014) ---- */

static void ms_nlit(const char *v, char *out, int n) {
    int o = 0; out[o++] = 'N'; out[o++] = '\'';
    for (; *v && o < n - 3; v++) { if (*v == '\'') out[o++] = '\''; out[o++] = *v; }
    out[o++] = '\''; out[o] = 0;
}
static const char *ms_types(int k) { return k == OBJK_VIEW ? "'V'" : k == OBJK_PROCEDURE ? "'P'" : k == OBJK_FUNCTION ? "'FN', 'IF', 'TF'" : k == OBJK_TRIGGER ? "'TR'" : NULL; }

static bool ms_load_sql(const ObjKey *k, ObjSqlBuf *o) {
    const char *types = ms_types(k->kind); if (!types) return false;
    char sch[300], nm[300], full[700], fl[800];
    if (k->schema[0]) ms_nlit(k->schema, sch, sizeof sch); else snprintf(sch, sizeof sch, "SCHEMA_NAME()");
    ms_nlit(k->name, nm, sizeof nm);
    obj_sql_add(o, "SELECT o.type, OBJECT_DEFINITION(o.object_id), CONVERT(varchar(30), o.modify_date, 126), CAST(o.object_id AS varchar(20)), COALESCE(OBJECT_NAME(o.parent_object_id), ''), s.name "
                   "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id WHERE s.name = %s AND o.name = %s AND o.type IN (%s)", sch, nm, types);
    snprintf(full, sizeof full, "%s.%s", k->schema[0] ? k->schema : "dbo", k->name); ms_nlit(full, fl, sizeof fl);
    obj_sql_add(o, "SELECT c.name, TYPE_NAME(c.user_type_id) FROM sys.columns c WHERE c.object_id = OBJECT_ID(%s) ORDER BY c.column_id", fl);
    obj_sql_add(o, "SELECT DISTINCT OBJECT_SCHEMA_NAME(d.referencing_id) + '.' + OBJECT_NAME(d.referencing_id), x.type_desc FROM sys.sql_expression_dependencies d "
                   "JOIN sys.objects x ON x.object_id = d.referencing_id WHERE d.referenced_id = OBJECT_ID(%s) AND d.referencing_id <> d.referenced_id", fl);
    return true;
}
static bool ms_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) {
    memset(s, 0, sizeof *s);
    if (n < 1 || !rs[0]->ok) { snprintf(err, (size_t)errn, "%s", n >= 1 ? rs[0]->error : "no result"); return false; }
    s->exists = rs[0]->nrows > 0;
    if (!s->exists) return true;
    const char *def = rs_cell(rs[0], 0, 1);
    char stamp[64]; snprintf(stamp, sizeof stamp, "%s", cell(rs[0], 0, 2));
    if (!def) {
        /* WITH ENCRYPTION: the server does not return the text */
        s->encrypted = true;
        snprintf(s->ro_reason, sizeof s->ro_reason, "created WITH ENCRYPTION: SQL Server does not return its source");
        snprintf(s->missing, sizeof s->missing, "the module is encrypted");
        char note[300]; snprintf(note, sizeof note, "-- %s %s.%s is encrypted (WITH ENCRYPTION): its source is not available.\n", objkind_name(k->kind), cell(rs[0], 0, 5), k->name);
        s->original = strdup(note); s->ddl = NULL;
        s->fingerprint = obj_hash(note, -1) ^ obj_hash(stamp, -1);
        return true;
    }
    s->original = strdup(def);
    /* the editable form publishes in place: CREATE -> CREATE OR ALTER (2016 SP1+) */
    ObjHeader h;
    if (obj_parse_header(def, (int)strlen(def), DIALECT_MSSQL, &h) && !h.or_alter && !h.alter_only) {
        size_t L = strlen(def) + 16; s->ddl = malloc(L);
        snprintf(s->ddl, L, "%.*sCREATE OR ALTER%s", h.start, def, def + h.verb_end);
    } else s->ddl = strdup(def);
    s->fingerprint = obj_hash(def, -1) ^ obj_hash(stamp, -1);
    snprintf(s->options, sizeof s->options, "%s", cell(rs[0], 0, 4)[0] ? cell(rs[0], 0, 4) : "");
    if (n > 1 && rs[1]->ok && rs[1]->nrows && k->kind == OBJK_VIEW) {
        s->cols = calloc((size_t)rs[1]->nrows, sizeof *s->cols); s->ncols = rs[1]->nrows;
        for (int i = 0; i < rs[1]->nrows; i++) { snprintf(s->cols[i].name, sizeof s->cols[i].name, "%s", cell(rs[1], i, 0)); snprintf(s->cols[i].type, sizeof s->cols[i].type, "%s", cell(rs[1], i, 1)); }
    }
    if (n > 2 && rs[2]->ok) {
        s->deps_known = true; s->deps = calloc((size_t)(rs[2]->nrows + 1), sizeof *s->deps);
        for (int i = 0; i < rs[2]->nrows; i++) { ObjDep *d = &s->deps[s->ndeps++]; snprintf(d->name, sizeof d->name, "%s", cell(rs[2], i, 0)); snprintf(d->kind, sizeof d->kind, "%s", cell(rs[2], i, 1)); snprintf(d->how, sizeof d->how, "catalog"); }
    }
    return true;
}
static bool ms_list_sql(const char *schema, ObjSqlBuf *o) {
    char sch[300]; if (schema && schema[0]) ms_nlit(schema, sch, sizeof sch); else snprintf(sch, sizeof sch, "SCHEMA_NAME()");
    obj_sql_add(o, "SELECT o.type, o.name, COALESCE(OBJECT_NAME(o.parent_object_id), ''), s.name, CASE WHEN m.definition IS NULL THEN 'ENCRYPTED' ELSE '' END FROM sys.objects o "
                   "JOIN sys.schemas s ON s.schema_id = o.schema_id LEFT JOIN sys.sql_modules m ON m.object_id = o.object_id "
                   "WHERE o.is_ms_shipped = 0 AND o.type IN ('V', 'P', 'FN', 'IF', 'TF', 'TR') AND s.name = %s ORDER BY o.type, o.name", sch);
    return true;
}
static int ms_list_parse(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max) {
    if (n < 1 || !rs[0]->ok) return 0;
    int m = 0;
    for (int i = 0; i < rs[0]->nrows && m < max; i++) {
        const char *t = cell(rs[0], i, 0);
        ObjListItem *it = &out[m++]; memset(it, 0, sizeof *it);
        snprintf(it->key.conn_id, sizeof it->key.conn_id, "%s", conn_id);
        it->key.kind = t[0] == 'V' ? OBJK_VIEW : t[0] == 'P' ? OBJK_PROCEDURE : !strncmp(t, "TR", 2) ? OBJK_TRIGGER : OBJK_FUNCTION;
        snprintf(it->key.name, sizeof it->key.name, "%s", cell(rs[0], i, 1));
        snprintf(it->key.table, sizeof it->key.table, "%s", cell(rs[0], i, 2));
        snprintf(it->key.schema, sizeof it->key.schema, "%s", cell(rs[0], i, 3));
        snprintf(it->status, sizeof it->status, "%s", cell(rs[0], i, 4));
        if (it->status[0]) it->system = true;
    }
    return m;
}

static ObjCaps mssql_caps(int k, int ver) {
    ObjCaps c; memset(&c, 0, sizeof c);
    if (k != OBJK_VIEW && k != OBJK_FUNCTION && k != OBJK_PROCEDURE && k != OBJK_TRIGGER) return c;
    c.available = c.source_read = c.source_edit = c.format = c.static_validate = c.fingerprint = c.dependency_graph = true;
    c.replace_in_place = true;
    c.transactional_ddl = true;
    return c;
}
static void mssql_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    const ObjKey *key = in->key;
    p->atomicity = ATOM_TRANSACTIONAL;
    if (!h->or_alter && !h->alter_only) plan_block(p, "Write CREATE OR ALTER (or ALTER): the object exists.");
    if (h->or_alter && in->server_version && in->server_version < 13) plan_block(p, "CREATE OR ALTER needs SQL Server 2016 SP1; use ALTER.");
    snprintf(p->strategy, sizeof p->strategy, "%s %s", h->or_alter ? "CREATE OR ALTER" : "ALTER", objkind_name(key->kind));
    plan_create(p, in->local + cs, ce - cs); p->risk = RISK_NON_DESTRUCTIVE;
    plan_impact(p, "Permissions and the object id are kept by ALTER.");
    if (in->base && in->base->encrypted) plan_impact(p, "WITH ENCRYPTION: the stored text is not readable.");
}

const ObjEngine obj_engine_mysql = { .api_version = OBJ_ENGINE_API, .id = "mysql", .dialect = DIALECT_MYSQL, .has_driver = true, .caps = mysql_caps, .plan = my_plan,
    .load_sql = my_load_sql, .load_parse = my_load_parse, .list_sql = my_list_sql, .list_parse = my_list_parse };
const ObjEngine obj_engine_mariadb = { .api_version = OBJ_ENGINE_API, .id = "mariadb", .dialect = DIALECT_MARIADB, .has_driver = true, .caps = mariadb_caps, .plan = my_plan,
    .load_sql = my_load_sql, .load_parse = maria_load_parse, .list_sql = my_list_sql, .list_parse = my_list_parse };
const ObjEngine obj_engine_mssql = { .api_version = OBJ_ENGINE_API, .id = "sqlserver", .dialect = DIALECT_MSSQL, .has_driver = true, .caps = mssql_caps, .plan = mssql_plan,
    .load_sql = ms_load_sql, .load_parse = ms_load_parse, .list_sql = ms_list_sql, .list_parse = ms_list_parse };
