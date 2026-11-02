/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * PostgreSQL adapter for the object source editor (OBJ-047 / 048 / 049 / 055 / 056 / 070)
 * ========================================================================================
 *
 * Source: pg_get_viewdef / pg_get_functiondef / pg_get_triggerdef. A routine is identified by
 * schema + name + identity argument types (oidvectortypes(proargtypes)), never by name alone:
 * calculate(integer) and calculate(text) are different objects (AT-03), resolved on the server
 * with to_regprocedure.
 *
 * Publishing: CREATE OR REPLACE inside one transaction when PostgreSQL allows it (views keep
 * their columns' names, types and order; functions keep their identity and return type;
 * triggers from 14). Otherwise an explicit DROP + CREATE, offered only on request, never with
 * CASCADE, followed by the statements that restore what the drop removes: grants (from the ACL),
 * owner, comment and, for materialized views, indexes.
 */
#include "objplan_int.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

static const char *cell(const ResultSet *rs, int r, int c) { const char *v = r < rs->nrows && c < rs->ncols ? rs_cell(rs, r, c) : NULL; return v ? v : ""; }

static ObjCaps pg_caps(int k, int ver) {
    ObjCaps c; memset(&c, 0, sizeof c);
    c.available = true;
    if (k == OBJK_OPERATOR || k == OBJK_EVENT_TRIGGER) { c.source_read = true; return c; }   /* read-only (OBJ-058) */
    if (k != OBJK_VIEW && k != OBJK_MATVIEW && k != OBJK_FUNCTION && k != OBJK_PROCEDURE && k != OBJK_TRIGGER) return c;
    if (k == OBJK_PROCEDURE && ver && ver < 110000) return c;   /* procedures exist from 11 */
    c.source_read = c.source_edit = c.format = c.static_validate = true;
    c.server_validate = k == OBJK_VIEW || k == OBJK_MATVIEW;   /* compiled as a temporary view, rolled back */
    c.replace_in_place = k == OBJK_VIEW || k == OBJK_FUNCTION || k == OBJK_PROCEDURE || (k == OBJK_TRIGGER && (!ver || ver >= 140000));
    c.transactional_ddl = true;
    c.dependency_graph = true;
    c.fingerprint = true;
    c.refresh = k == OBJK_MATVIEW;
    c.enable_disable = k == OBJK_TRIGGER;
    return c;
}

static void pg_plan(const ObjPlanInput *in, const ObjHeader *h, int cs, int ce, ObjPlan *p) {
    const ObjKey *key = in->key; const ObjSnapshot *base = in->base;
    const int d = DIALECT_PG;
    char qn[600]; obj_qname(key, d, qn, sizeof qn);
    char disp[700]; objkey_display(key, disp, sizeof disp);
    p->atomicity = ATOM_TRANSACTIONAL;
    bool drop_create = false; char why[300] = "";
    if (key->kind == OBJK_VIEW) {
        if (!h->or_replace) plan_block(p, "Write CREATE OR REPLACE VIEW: a plain CREATE fails because the view exists.");
        if (in->new_cols && base && base->ncols) {
            if (!pg_view_contract_ok(base->cols, base->ncols, in->new_cols, in->n_new_cols, why, sizeof why)) drop_create = true;
            else { plan_impact(p, "Column contract kept: %d column(s)%s.", base->ncols, in->n_new_cols > base->ncols ? ", new ones appended at the end" : ""); p->risk = RISK_NON_DESTRUCTIVE; }
        } else { p->risk = RISK_UNKNOWN; plan_pre(p, "Columns of the new query not described: PostgreSQL rejects an incompatible replace and the transaction rolls back."); }
    } else if (key->kind == OBJK_MATVIEW) {
        drop_create = true; snprintf(why, sizeof why, "a materialized view has no CREATE OR REPLACE");
    } else if (key->kind == OBJK_FUNCTION || key->kind == OBJK_PROCEDURE) {
        if (!h->or_replace) plan_block(p, "Write CREATE OR REPLACE %s: a plain CREATE fails because it exists.", objkind_name(key->kind));
        char loc[512] = "", cat[512] = "";
        bool sure = h->args_start >= 0 && pg_identity_types(in->local + h->args_start, h->args_end - h->args_start, key->kind == OBJK_PROCEDURE, loc, sizeof loc);
        pg_norm_signature(key->signature, cat, sizeof cat);
        snprintf(p->probe_signature, sizeof p->probe_signature, "%s(%s)", qn, sure ? loc : key->signature);
        if (!sure) { p->identity_unsure = true; plan_pre(p, "The argument list could not be read with certainty: the identity is resolved on the server before running."); }
        else if (strcmp(loc, cat))
            plan_block(p, "Signature changed: (%s) is a different overload than (%s). Publishing would create a new %s and leave %s unchanged.", loc, key->signature, objkind_word(key->kind), disp);
        else plan_pre(p, "Identity checked: %s(%s) must resolve to the object opened (oid %s) on the server before running.", key->name, loc, key->native_id[0] ? key->native_id : "?");
        if (key->kind == OBJK_FUNCTION && h->returns[0] && base && base->result_type[0]) {
            char a[300], b[300]; pg_norm_result(h->returns, a, sizeof a); pg_norm_result(base->result_type, b, sizeof b);
            if (strcmp(a, b)) { drop_create = true; snprintf(why, sizeof why, "the return type changes (%s -> %s), which CREATE OR REPLACE cannot do", base->result_type, h->returns); }
        }
        if (!drop_create) p->risk = RISK_NON_DESTRUCTIVE;
    } else if (key->kind == OBJK_TRIGGER) {
        if (h->or_replace && in->server_version && in->server_version < 140000)
            plan_block(p, "CREATE OR REPLACE TRIGGER needs PostgreSQL 14; this server is %d.%d. Write CREATE TRIGGER (Lume drops and re-creates it in one transaction).",
                       in->server_version / 10000, in->server_version % 10000 / 100);
        if (!h->or_replace) {
            snprintf(p->strategy, sizeof p->strategy, "DROP TRIGGER + CREATE TRIGGER in one transaction");
            char tbl[300], qt[300], qtrig[300];
            const char *t = key->table[0] ? key->table : h->table;
            if (!strchr(t, '.') && key->schema[0]) snprintf(tbl, sizeof tbl, "%s.%s", key->schema, t); else snprintf(tbl, sizeof tbl, "%s", t);
            obj_qtable(tbl, d, qt, sizeof qt);
            ObjKey k3; memset(&k3, 0, sizeof k3); snprintf(k3.name, sizeof k3.name, "%s", key->name); obj_qname(&k3, d, qtrig, sizeof qtrig);
            plan_sql(p, "BEGIN"); plan_sql(p, "DROP TRIGGER %s ON %s", qtrig, qt);
            plan_create(p, in->local + cs, ce - cs); plan_sql(p, "COMMIT");
            p->risk = RISK_POTENTIAL;
            plan_impact(p, "The trigger is dropped and created again inside one transaction: no write to %s can happen without it.", tbl);
            plan_deps_impact(p, base, "applying");
            return;
        }
        p->risk = RISK_NON_DESTRUCTIVE;
    }
    if (drop_create) {
        p->destructive = true; p->risk = RISK_DESTRUCTIVE;
        snprintf(p->strategy, sizeof p->strategy, "DROP + CREATE in one transaction (explicit)");
        plan_impact(p, "CREATE OR REPLACE is not possible: %s.", why);
        if (!in->allow_destructive) {
            p->destructive_available = true;
            plan_block(p, "CREATE OR REPLACE is not possible: %s. Review the explicit drop + create plan to continue.", why);
        }
        if (base && base->deps_known && base->ndeps > 0)
            plan_block(p, "%d object(s) depend on %s; DROP would fail without CASCADE, which Lume never uses. Change or drop them first.", base->ndeps, disp);
        plan_sql(p, "BEGIN");
        if (key->kind == OBJK_FUNCTION || key->kind == OBJK_PROCEDURE) plan_sql(p, "DROP %s %s(%s)", objkind_name(key->kind), qn, key->signature);
        else plan_sql(p, "DROP %s %s", objkind_name(key->kind), qn);
        plan_create(p, in->local + cs, ce - cs);
        if (base) for (int i = 0; i < base->nrecreate; i++) plan_sql(p, "%s", base->recreate[i]);
        plan_sql(p, "COMMIT");
        if (base && base->nrecreate) plan_impact(p, "%d statement(s) restore what the drop removes (grants, owner, comment, indexes).", base->nrecreate);
        if (key->kind == OBJK_MATVIEW) plan_impact(p, "The materialized view is rebuilt: its data is computed again by the new query.");
    } else {
        snprintf(p->strategy, sizeof p->strategy, "CREATE OR REPLACE %s in one transaction", objkind_name(key->kind));
        plan_sql(p, "BEGIN");
        plan_create(p, in->local + cs, ce - cs);
        plan_sql(p, "COMMIT");
        plan_impact(p, "Replaced in place: grants, owner and dependents are kept.");
    }
    plan_deps_impact(p, base, "applying");
}

/* ------------------------------------------------------------------------ */
/* catalog                                                                   */
/* ------------------------------------------------------------------------ */

/* identifier for generated DDL: bare when PostgreSQL would read it back the same, quoted otherwise */
static void pg_ident(const char *s, char *out, int n) {
    bool bare = s[0] && (islower((unsigned char)s[0]) || s[0] == '_') && !sql_is_keyword(s, (int)strlen(s));
    for (const char *p = s; *p && bare; p++) if (!(islower((unsigned char)*p) || isdigit((unsigned char)*p) || *p == '_' || *p == '$')) bare = false;
    if (bare) { snprintf(out, (size_t)n, "%s", s); return; }
    int o = 0; out[o++] = '"';
    for (const char *p = s; *p && o < n - 3; p++) { if (*p == '"') out[o++] = '"'; out[o++] = *p; }
    out[o++] = '"'; out[o] = 0;
}
static void pg_qual(const ObjKey *k, char *out, int n) {
    char a[300], b[300]; pg_ident(k->schema, a, sizeof a); pg_ident(k->name, b, sizeof b);
    if (k->schema[0]) snprintf(out, (size_t)n, "%s.%s", a, b); else snprintf(out, (size_t)n, "%s", b);
}
/* '"schema"."name"(sig)' as a literal for to_regprocedure */
static void regproc_lit(const ObjKey *k, char *out, int n) {
    char q[600], t[700]; obj_qname(k, DIALECT_PG, q, sizeof q);
    snprintf(t, sizeof t, "%s(%s)", q, k->signature);
    obj_qlit(t, out, n);
}
/* 'schema' and 'name' literals */
static void lits(const ObjKey *k, char *s, char *nm, int n) { obj_qlit(k->schema[0] ? k->schema : "public", s, n); obj_qlit(k->name, nm, n); }

static bool pg_load_sql(const ObjKey *k, ObjSqlBuf *o) {
    char s[300], nm[300]; lits(k, s, nm, sizeof s);
    if (k->kind == OBJK_VIEW || k->kind == OBJK_MATVIEW) {
        obj_sql_add(o, "SELECT c.oid::text, c.relkind::text, pg_get_viewdef(c.oid, true), pg_get_userbyid(c.relowner), coalesce(c.relacl::text, ''), "
                       "coalesce(array_to_string(c.reloptions, ', '), ''), coalesce(obj_description(c.oid, 'pg_class'), ''), current_setting('server_version_num'), "
                       "n.nspname IN ('pg_catalog', 'information_schema'), coalesce((SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_class'::regclass AND d.objid = c.oid AND d.deptype = 'e' LIMIT 1), '') "
                       "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = %s AND c.relname = %s AND c.relkind = '%c'",
                   s, nm, k->kind == OBJK_VIEW ? 'v' : 'm');
        obj_sql_add(o, "SELECT a.attname, format_type(a.atttypid, a.atttypmod) FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid "
                       "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE n.nspname = %s AND c.relname = %s AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum", s, nm);
        obj_sql_add(o, "SELECT DISTINCT dn.nspname || '.' || dc.relname, CASE dc.relkind WHEN 'm' THEN 'MATERIALIZED VIEW' ELSE 'VIEW' END "
                       "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_depend d ON d.refobjid = c.oid AND d.classid = 'pg_rewrite'::regclass "
                       "JOIN pg_rewrite r ON r.oid = d.objid JOIN pg_class dc ON dc.oid = r.ev_class JOIN pg_namespace dn ON dn.oid = dc.relnamespace "
                       "WHERE n.nspname = %s AND c.relname = %s AND dc.oid <> c.oid ORDER BY 1", s, nm);
        obj_sql_add(o, "SELECT indexdef FROM pg_indexes WHERE schemaname = %s AND tablename = %s ORDER BY indexname", s, nm);
        return true;
    }
    if (k->kind == OBJK_FUNCTION || k->kind == OBJK_PROCEDURE) {
        char rp[800]; regproc_lit(k, rp, sizeof rp);
        obj_sql_add(o, "SELECT p.oid::text, CASE WHEN p.prokind = 'a' THEN NULL ELSE pg_get_functiondef(p.oid) END, pg_get_userbyid(p.proowner), coalesce(p.proacl::text, ''), coalesce(pg_get_function_result(p.oid), ''), "
                       "p.prokind::text, coalesce(obj_description(p.oid, 'pg_proc'), ''), current_setting('server_version_num'), n.nspname IN ('pg_catalog', 'information_schema'), "
                       "coalesce((SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'e' LIMIT 1), '') "
                       "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE p.oid = to_regprocedure(%s) AND p.prokind IN ('f', 'p', 'a')", rp);
        obj_sql_add(o, "SELECT DISTINCT dn.nspname || '.' || dc.relname, 'VIEW' FROM pg_depend d JOIN pg_rewrite r ON r.oid = d.objid AND d.classid = 'pg_rewrite'::regclass "
                       "JOIN pg_class dc ON dc.oid = r.ev_class JOIN pg_namespace dn ON dn.oid = dc.relnamespace WHERE d.refobjid = to_regprocedure(%s) "
                       "UNION ALL SELECT t.tgname || ' ON ' || t.tgrelid::regclass::text, 'TRIGGER' FROM pg_trigger t WHERE t.tgfoid = to_regprocedure(%s) AND NOT t.tgisinternal ORDER BY 1", rp, rp);
        return true;
    }
    if (k->kind == OBJK_OPERATOR) {
        /* identity: schema.name(left, right), NONE for a missing operand */
        char sch[300], q[700], lit[900]; ObjKey ks; memset(&ks, 0, sizeof ks); snprintf(ks.name, sizeof ks.name, "%s", k->schema); obj_qname(&ks, DIALECT_PG, sch, sizeof sch);
        snprintf(q, sizeof q, "%s.%s(%s)", sch, k->name, k->signature);
        obj_qlit(q, lit, sizeof lit);
        obj_sql_add(o, "SELECT o.oid::text, o.oprname, CASE WHEN o.oprleft = 0 THEN 'NONE' ELSE format_type(o.oprleft, NULL) END, CASE WHEN o.oprright = 0 THEN 'NONE' ELSE format_type(o.oprright, NULL) END, "
                       "o.oprcode::regprocedure::text, format_type(o.oprresult, NULL), coalesce(NULLIF(o.oprcom, 0)::regoperator::text, ''), coalesce(NULLIF(o.oprnegate, 0)::regoperator::text, ''), "
                       "pg_get_userbyid(o.oprowner), coalesce(obj_description(o.oid, 'pg_operator'), ''), coalesce((SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_operator'::regclass AND d.objid = o.oid AND d.deptype = 'e' LIMIT 1), ''), current_setting('server_version_num') "
                       "FROM pg_operator o WHERE o.oid = to_regoperator(%s)", lit);
        return true;
    }
    if (k->kind == OBJK_EVENT_TRIGGER) {
        obj_sql_add(o, "SELECT oid::text, evtevent, evtfoid::regprocedure::text, evtenabled::text, coalesce(array_to_string(evttags, ','), ''), pg_get_userbyid(evtowner), current_setting('server_version_num') "
                       "FROM pg_event_trigger WHERE evtname = %s", nm);
        return true;
    }
    if (k->kind == OBJK_TRIGGER) {
        char tb[300]; const char *t = strrchr(k->table, '.') ? strrchr(k->table, '.') + 1 : k->table; obj_qlit(t, tb, sizeof tb);
        obj_sql_add(o, "SELECT t.oid::text, pg_get_triggerdef(t.oid, true), t.tgenabled::text, coalesce(obj_description(t.oid, 'pg_trigger'), ''), current_setting('server_version_num') "
                       "FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
                       "WHERE NOT t.tgisinternal AND n.nspname = %s AND c.relname = %s AND t.tgname = %s", s, tb, nm);
        return true;
    }
    return false;
}

/* GRANT statements from an aclitem[] text, skipping the owner's own entry (OBJ-070). */
static void acl_grants(ObjSnapshot *s, const char *acl, const char *owner, const char *on_what) {
    static const struct { char c; const char *priv; } map[] = { { 'r', "SELECT" }, { 'a', "INSERT" }, { 'w', "UPDATE" }, { 'd', "DELETE" }, { 'D', "TRUNCATE" },
        { 'x', "REFERENCES" }, { 't', "TRIGGER" }, { 'X', "EXECUTE" }, { 'U', "USAGE" }, { 'm', "MAINTAIN" }, { 0, NULL } };
    const char *p = acl;
    if (*p == '{') p++;
    while (*p && *p != '}') {
        /* item: grantee=privs/grantor, grantee may be quoted or empty (PUBLIC) */
        char grantee[160] = ""; int g = 0;
        bool quoted = *p == '"';
        if (quoted) { p++; while (*p && !(*p == '"' && p[1] != '"')) { if (*p == '"') p++; if (g < 159) grantee[g++] = *p; p++; } if (*p == '"') p++; }
        else while (*p && *p != '=' && *p != ',' && *p != '}') { if (g < 159) grantee[g++] = *p; p++; }
        grantee[g] = 0;
        if (*p != '=') break;
        p++;
        char privs[64]; int np = 0; while (*p && *p != '/' && *p != ',' && *p != '}' && np < 63) privs[np++] = *p++; privs[np] = 0;
        while (*p && *p != ',' && *p != '}') { if (*p == '"') { p++; while (*p && *p != '"') p++; } if (*p) p++; }
        if (*p == ',') p++;
        if (owner && !strcmp(grantee, owner)) continue;
        char who[200]; if (!grantee[0]) snprintf(who, sizeof who, "PUBLIC"); else pg_ident(grantee, who, sizeof who);
        char plain[200] = "", withopt[200] = ""; int a = 0, b = 0;
        for (int i = 0; privs[i]; i++)
            for (int m = 0; map[m].c; m++) if (privs[i] == map[m].c) {
                bool opt = privs[i + 1] == '*';
                if (opt) b += snprintf(withopt + b, sizeof withopt - (size_t)b, "%s%s", b ? ", " : "", map[m].priv);
                else a += snprintf(plain + a, sizeof plain - (size_t)a, "%s%s", a ? ", " : "", map[m].priv);
            }
        char st[600];
        if (plain[0]) { snprintf(st, sizeof st, "GRANT %s ON %s TO %s", plain, on_what, who); s->recreate = realloc(s->recreate, sizeof *s->recreate * (size_t)(s->nrecreate + 1)); s->recreate[s->nrecreate++] = strdup(st); }
        if (withopt[0]) { snprintf(st, sizeof st, "GRANT %s ON %s TO %s WITH GRANT OPTION", withopt, on_what, who); s->recreate = realloc(s->recreate, sizeof *s->recreate * (size_t)(s->nrecreate + 1)); s->recreate[s->nrecreate++] = strdup(st); }
    }
}
static void add_recreate(ObjSnapshot *s, const char *fmt, const char *a, const char *b) {
    char st[2400]; snprintf(st, sizeof st, fmt, a, b);
    s->recreate = realloc(s->recreate, sizeof *s->recreate * (size_t)(s->nrecreate + 1)); s->recreate[s->nrecreate++] = strdup(st);
}

static bool pg_load_parse(const ObjKey *k, ResultSet *const *rs, int n, ObjSnapshot *s, char *err, int errn) {
    memset(s, 0, sizeof *s);
    for (int i = 0; i < n; i++) if (!rs[i]->ok) { snprintf(err, (size_t)errn, "%s", rs[i]->error); return false; }
    if (n < 1) { snprintf(err, (size_t)errn, "no result"); return false; }
    const ResultSet *m = rs[0];
    s->exists = m->nrows > 0;
    if (!s->exists) return true;
    char q[600]; pg_qual(k, q, sizeof q);
    if (k->kind == OBJK_VIEW || k->kind == OBJK_MATVIEW) {
        const char *def = cell(m, 0, 2);
        snprintf(s->owner, sizeof s->owner, "%s", cell(m, 0, 3)); snprintf(s->acl, sizeof s->acl, "%s", cell(m, 0, 4));
        snprintf(s->options, sizeof s->options, "%s", cell(m, 0, 5)); s->server_version = atoi(cell(m, 0, 7)); s->system = !strcmp(cell(m, 0, 8), "t");
        if (cell(m, 0, 9)[0]) { s->system = true; snprintf(s->ro_reason, sizeof s->ro_reason, "owned by extension %s: it changes with the extension (ALTER EXTENSION ... UPDATE), not by editing", cell(m, 0, 9)); }
        s->original = strdup(def);
        size_t L = strlen(def) + strlen(q) + strlen(s->options) + 96;
        s->ddl = malloc(L);
        if (k->kind == OBJK_VIEW) {
            if (s->options[0]) snprintf(s->ddl, L, "CREATE OR REPLACE VIEW %s WITH (%s) AS\n%s", q, s->options, def);
            else snprintf(s->ddl, L, "CREATE OR REPLACE VIEW %s AS\n%s", q, def);
        } else {
            /* pg_get_viewdef ends with ';' */
            int dl = (int)strlen(def); while (dl > 0 && (def[dl - 1] == ';' || isspace((unsigned char)def[dl - 1]))) dl--;
            if (s->options[0]) snprintf(s->ddl, L, "CREATE MATERIALIZED VIEW %s WITH (%s) AS\n%.*s\nWITH DATA;", q, s->options, dl, def);
            else snprintf(s->ddl, L, "CREATE MATERIALIZED VIEW %s AS\n%.*s\nWITH DATA;", q, dl, def);
        }
        char fp[1024]; snprintf(fp, sizeof fp, "%s|%s", s->options, cell(m, 0, 1));
        s->fingerprint = obj_hash(def, -1) ^ obj_hash(fp, -1);
        if (n > 1 && rs[1]->nrows) {
            s->cols = calloc((size_t)rs[1]->nrows, sizeof *s->cols); s->ncols = rs[1]->nrows;
            for (int i = 0; i < rs[1]->nrows; i++) { snprintf(s->cols[i].name, sizeof s->cols[i].name, "%s", cell(rs[1], i, 0)); snprintf(s->cols[i].type, sizeof s->cols[i].type, "%s", cell(rs[1], i, 1)); }
        }
        if (n > 2) {
            s->deps_known = true; s->deps = calloc((size_t)(rs[2]->nrows + 1), sizeof *s->deps);
            for (int i = 0; i < rs[2]->nrows; i++) { ObjDep *dp = &s->deps[s->ndeps++]; snprintf(dp->name, sizeof dp->name, "%s", cell(rs[2], i, 0)); snprintf(dp->kind, sizeof dp->kind, "%s", cell(rs[2], i, 1)); snprintf(dp->how, sizeof dp->how, "rewrite rule"); }
        }
        /* what a DROP would lose: indexes (materialized views), owner, grants, comment */
        if (n > 3) for (int i = 0; i < rs[3]->nrows; i++) add_recreate(s, "%s%s", cell(rs[3], i, 0), "");
        const char *what = k->kind == OBJK_VIEW ? "VIEW" : "MATERIALIZED VIEW";
        char on[700]; snprintf(on, sizeof on, "%s", q);
        char own[200]; pg_ident(s->owner, own, sizeof own);
        { char st[1000]; snprintf(st, sizeof st, "ALTER %s %s OWNER TO %s", what, q, own); add_recreate(s, "%s%s", st, ""); }
        if (s->acl[0]) acl_grants(s, s->acl, s->owner, on);
        if (cell(m, 0, 6)[0]) { char lit[2000]; obj_qlit(cell(m, 0, 6), lit, sizeof lit); char st[2200]; snprintf(st, sizeof st, "COMMENT ON %s %s IS %s", what, q, lit); add_recreate(s, "%s%s", st, ""); }
        return true;
    }
    if (k->kind == OBJK_FUNCTION || k->kind == OBJK_PROCEDURE) {
        const char *def = cell(m, 0, 1);
        snprintf(s->owner, sizeof s->owner, "%s", cell(m, 0, 2)); snprintf(s->acl, sizeof s->acl, "%s", cell(m, 0, 3));
        snprintf(s->result_type, sizeof s->result_type, "%s", cell(m, 0, 4)); s->server_version = atoi(cell(m, 0, 7)); s->system = !strcmp(cell(m, 0, 8), "t");
        if (!strcmp(cell(m, 0, 5), "a")) {
            /* PostgreSQL does not rebuild aggregate definitions: no invented DDL (OBJ-058 / 069) */
            s->system = true;
            snprintf(s->missing, sizeof s->missing, "PostgreSQL does not reconstruct aggregate definitions (pg_get_functiondef)");
            snprintf(s->ro_reason, sizeof s->ro_reason, "an aggregate: its definition is not available from the catalog as executable DDL");
            char note[400]; snprintf(note, sizeof note, "-- AGGREGATE %s(%s) returns %s\n-- %s.\n", q, k->signature, s->result_type, s->missing);
            s->original = strdup(note); s->ddl = NULL;
            s->fingerprint = obj_hash(note, -1);
        } else {
            s->original = strdup(def); s->ddl = strdup(def);
            s->fingerprint = obj_hash(def, -1);
        }
        if (cell(m, 0, 9)[0]) { s->system = true; snprintf(s->ro_reason, sizeof s->ro_reason, "owned by extension %s: it changes with the extension (ALTER EXTENSION ... UPDATE), not by editing", cell(m, 0, 9)); }
        if (n > 1) {
            s->deps_known = true; s->deps = calloc((size_t)(rs[1]->nrows + 1), sizeof *s->deps);
            for (int i = 0; i < rs[1]->nrows; i++) { ObjDep *dp = &s->deps[s->ndeps++]; snprintf(dp->name, sizeof dp->name, "%s", cell(rs[1], i, 0)); snprintf(dp->kind, sizeof dp->kind, "%s", cell(rs[1], i, 1)); snprintf(dp->how, sizeof dp->how, "catalog"); }
        }
        char on[900]; snprintf(on, sizeof on, "%s %s(%s)", objkind_name(k->kind), q, k->signature);
        char own[200]; pg_ident(s->owner, own, sizeof own);
        { char st[1200]; snprintf(st, sizeof st, "ALTER %s OWNER TO %s", on, own); add_recreate(s, "%s%s", st, ""); }
        if (s->acl[0]) acl_grants(s, s->acl, s->owner, on);
        if (cell(m, 0, 6)[0]) { char lit[2000]; obj_qlit(cell(m, 0, 6), lit, sizeof lit); char st[3000]; snprintf(st, sizeof st, "COMMENT ON %s IS %s", on, lit); add_recreate(s, "%s%s", st, ""); }
        return true;
    }
    if (k->kind == OBJK_OPERATOR || k->kind == OBJK_EVENT_TRIGGER) {
        /* shown from the catalog's own fields (as pg_dump writes them), read-only */
        char txt[2400];
        if (k->kind == OBJK_OPERATOR) {
            int o2 = snprintf(txt, sizeof txt, "-- operator %s, read from pg_operator (read-only)\nCREATE OPERATOR %s.%s (\n    FUNCTION = %s", cell(m, 0, 1), k->schema, cell(m, 0, 1), cell(m, 0, 4));
            if (strcmp(cell(m, 0, 2), "NONE")) o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, ",\n    LEFTARG = %s", cell(m, 0, 2));
            if (strcmp(cell(m, 0, 3), "NONE")) o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, ",\n    RIGHTARG = %s", cell(m, 0, 3));
            if (cell(m, 0, 6)[0]) o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, ",\n    COMMUTATOR = OPERATOR(%s)", cell(m, 0, 6));
            if (cell(m, 0, 7)[0]) o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, ",\n    NEGATOR = OPERATOR(%s)", cell(m, 0, 7));
            snprintf(txt + o2, sizeof txt - (size_t)o2, "\n);  -- returns %s\n", cell(m, 0, 5));
            snprintf(s->owner, sizeof s->owner, "%s", cell(m, 0, 8)); snprintf(s->result_type, sizeof s->result_type, "%s", cell(m, 0, 5)); s->server_version = atoi(cell(m, 0, 11));
            if (cell(m, 0, 10)[0]) snprintf(s->ro_reason, sizeof s->ro_reason, "owned by extension %s: it changes with the extension", cell(m, 0, 10));
            else snprintf(s->ro_reason, sizeof s->ro_reason, "an operator: changing it means DROP / CREATE OPERATOR, which Lume does not plan");
        } else {
            int o2 = snprintf(txt, sizeof txt, "-- event trigger, read from pg_event_trigger (read-only); enabled: %s\nCREATE EVENT TRIGGER %s ON %s", cell(m, 0, 3), k->name, cell(m, 0, 1));
            const char *tags = cell(m, 0, 4);
            if (tags[0]) {
                o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, "\n    WHEN TAG IN ('");
                for (const char *t = tags; *t && o2 < (int)sizeof txt - 8; t++) { if (*t == ',') o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, "', '"); else txt[o2++] = *t; }
                o2 += snprintf(txt + o2, sizeof txt - (size_t)o2, "')");
            }
            snprintf(txt + o2, sizeof txt - (size_t)o2, "\n    EXECUTE FUNCTION %s;\n", cell(m, 0, 2));
            snprintf(s->owner, sizeof s->owner, "%s", cell(m, 0, 5)); s->server_version = atoi(cell(m, 0, 6));
            s->enabled = strcmp(cell(m, 0, 3), "D") != 0;
            snprintf(s->ro_reason, sizeof s->ro_reason, "an event trigger (database-wide): changing it is not planned by Lume");
        }
        s->original = strdup(txt); s->ddl = NULL;
        snprintf(s->missing, sizeof s->missing, "shown from the catalog; not published from here");
        s->system = true; s->deps_known = false;
        s->fingerprint = obj_hash(txt, -1);
        return true;
    }
    if (k->kind == OBJK_TRIGGER) {
        const char *def = cell(m, 0, 1);
        s->server_version = atoi(cell(m, 0, 4));
        s->enabled = strcmp(cell(m, 0, 2), "D") != 0;
        s->original = strdup(def);
        /* from 14 the editable form replaces in place */
        if (s->server_version >= 140000 && !strncmp(def, "CREATE TRIGGER", 14)) {
            s->ddl = malloc(strlen(def) + 16); snprintf(s->ddl, strlen(def) + 16, "CREATE OR REPLACE TRIGGER%s", def + 14);
        } else s->ddl = strdup(def);
        s->fingerprint = obj_hash(def, -1) ^ (s->enabled ? 1 : 0);
        s->deps_known = true;
        return true;
    }
    return false;
}

static bool pg_list_sql(const char *schema, ObjSqlBuf *o) {
    char s[300];
    if (schema && schema[0]) obj_qlit(schema, s, sizeof s); else snprintf(s, sizeof s, "current_schema()");
    obj_sql_add(o,
        "SELECT CASE c.relkind WHEN 'v' THEN 'view' ELSE 'matview' END, c.relname, '', '', c.oid::text, n.nspname, '', '', '', '' FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relkind IN ('v', 'm') "
        "UNION ALL SELECT CASE p.prokind WHEN 'p' THEN 'procedure' WHEN 'a' THEN 'aggregate' ELSE 'function' END, p.proname, oidvectortypes(p.proargtypes), '', p.oid::text, n.nspname, "
        "pg_get_function_arguments(p.oid), coalesce(pg_get_function_result(p.oid), ''), coalesce(obj_description(p.oid, 'pg_proc'), ''), "
        "coalesce((SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_proc'::regclass AND d.objid = p.oid AND d.deptype = 'e' LIMIT 1), '') "
        "FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE n.nspname = %s AND p.prokind IN ('f', 'p', 'a') "
        "UNION ALL SELECT 'operator', o.oprname, CASE WHEN o.oprleft = 0 THEN 'NONE' ELSE format_type(o.oprleft, NULL) END || ', ' || CASE WHEN o.oprright = 0 THEN 'NONE' ELSE format_type(o.oprright, NULL) END, "
        "'', o.oid::text, n.nspname, '', format_type(o.oprresult, NULL), coalesce(obj_description(o.oid, 'pg_operator'), ''), coalesce((SELECT e.extname FROM pg_depend d JOIN pg_extension e ON e.oid = d.refobjid WHERE d.classid = 'pg_operator'::regclass AND d.objid = o.oid AND d.deptype = 'e' LIMIT 1), '') "
        "FROM pg_operator o JOIN pg_namespace n ON n.oid = o.oprnamespace WHERE n.nspname = %s "
        "UNION ALL SELECT 'event trigger', e.evtname, '', '', e.oid::text, current_schema(), '', '', '', '' FROM pg_event_trigger e WHERE %s = current_schema() "
        "UNION ALL SELECT 'trigger', t.tgname, '', c.relname, t.oid::text, n.nspname, '', '', '', '' FROM pg_trigger t JOIN pg_class c ON c.oid = t.tgrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND NOT t.tgisinternal ORDER BY 1, 2, 3", s, s, s, s, s);
    return true;
}
static int pg_list_parse(ResultSet *const *rs, int n, const char *conn_id, ObjListItem *out, int max) {
    if (n < 1 || !rs[0]->ok) return 0;
    int m = 0;
    for (int i = 0; i < rs[0]->nrows && m < max; i++) {
        const char *kind = cell(rs[0], i, 0);
        ObjListItem *it = &out[m++]; memset(it, 0, sizeof *it);
        snprintf(it->key.conn_id, sizeof it->key.conn_id, "%s", conn_id);
        it->key.kind = !strcmp(kind, "view") ? OBJK_VIEW : !strcmp(kind, "matview") ? OBJK_MATVIEW : !strcmp(kind, "procedure") ? OBJK_PROCEDURE :
                       !strcmp(kind, "function") || !strcmp(kind, "aggregate") ? OBJK_FUNCTION : !strcmp(kind, "operator") ? OBJK_OPERATOR : !strcmp(kind, "event trigger") ? OBJK_EVENT_TRIGGER : OBJK_TRIGGER;
        if (it->key.kind == OBJK_OPERATOR || it->key.kind == OBJK_EVENT_TRIGGER) it->system = true;
        snprintf(it->key.name, sizeof it->key.name, "%s", cell(rs[0], i, 1));
        snprintf(it->key.signature, sizeof it->key.signature, "%s", cell(rs[0], i, 2));
        snprintf(it->key.table, sizeof it->key.table, "%s", cell(rs[0], i, 3));
        snprintf(it->key.native_id, sizeof it->key.native_id, "%s", cell(rs[0], i, 4));
        snprintf(it->key.schema, sizeof it->key.schema, "%s", cell(rs[0], i, 5));
        snprintf(it->args, sizeof it->args, "%s", cell(rs[0], i, 6));
        snprintf(it->returns, sizeof it->returns, "%s", cell(rs[0], i, 7));
        snprintf(it->comment, sizeof it->comment, "%s", cell(rs[0], i, 8));
        snprintf(it->extension, sizeof it->extension, "%s", cell(rs[0], i, 9));
        if (!strcmp(kind, "aggregate")) { it->key.kind = OBJK_FUNCTION; snprintf(it->status, sizeof it->status, "AGGREGATE"); it->system = true; }
        if (it->extension[0]) it->system = true;
    }
    return m;
}

/* The query compiled as a temporary view (the core wraps it in BEGIN ... ROLLBACK): validates
 * names and types on the server and reads the exact column types, without running the query. */
static bool pg_describe_sql(const char *q, int len, ObjSqlBuf *o) {
    while (len > 0 && (q[len - 1] == ';' || isspace((unsigned char)q[len - 1]))) len--;
    obj_sql_add(o, "CREATE TEMP VIEW lume_describe AS %.*s", len, q);
    obj_sql_add(o, "SELECT a.attname, format_type(a.atttypid, a.atttypmod) FROM pg_attribute a WHERE a.attrelid = 'pg_temp.lume_describe'::regclass AND a.attnum > 0 ORDER BY a.attnum");
    return true;
}

const ObjEngine obj_engine_pg = {
    .api_version = OBJ_ENGINE_API, .id = "postgres", .dialect = DIALECT_PG, .has_driver = true,
    .caps = pg_caps, .plan = pg_plan,
    .load_sql = pg_load_sql, .load_parse = pg_load_parse,
    .list_sql = pg_list_sql, .list_parse = pg_list_parse,
    .describe_sql = pg_describe_sql,
};
