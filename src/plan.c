/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Execution plans: see plan.h.
 *
 * Parsers per format (each one keeps every attribute it saw as a property, the operator name
 * unchanged, and leaves a value UNAVAILABLE when the engine did not report it):
 *   PostgreSQL   EXPLAIN (FORMAT JSON) / (ANALYZE, BUFFERS, FORMAT JSON)
 *   SQLite       EXPLAIN QUERY PLAN rows (id, parent, notused, detail)
 *   SQL Server   Showplan XML (estimated / with runtime counters), a small strict XML reader:
 *                no DOCTYPE, no entity definitions, size-limited
 *   MySQL        EXPLAIN FORMAT=JSON (format 1 and 2) / EXPLAIN ANALYZE (TREE text)
 *   MariaDB      EXPLAIN FORMAT=JSON / ANALYZE FORMAT=JSON (its own mapping: r_rows, r_loops)
 *   Oracle       PLAN_TABLE rows after EXPLAIN PLAN / DBMS_XPLAN.DISPLAY_CURSOR text (ALLSTATS LAST)
 */
#include "plan.h"
#include "json.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int plan_dialect_for_driver(const char *d) {
    if (!d) return -1;
    if (!strcmp(d, "sqlite")) return PL_SQLITE;
    if (!strcmp(d, "postgres")) return PL_PG;
    if (!strcmp(d, "oracle")) return PL_ORACLE;
    if (!strcmp(d, "mysql")) return PL_MYSQL;
    if (!strcmp(d, "mariadb")) return PL_MARIADB;
    if (!strcmp(d, "sqlserver")) return PL_MSSQL;
    return -1;
}
const char *plan_dialect_name(int d) { static const char *n[] = { "SQLite", "PostgreSQL", "Oracle", "MySQL", "MariaDB", "SQL Server" }; return d >= 0 && d < PL_NDIALECT ? n[d] : "?"; }

PlanCaps plan_caps(int d) {
    PlanCaps c; memset(&c, 0, sizeof c);
    c.estimated = true; c.raw = true;
    switch (d) {
    case PL_SQLITE: c.actual = false; c.why_no_actual = "SQLite reports no per-operator runtime: only the estimated query plan (SCAN / SEARCH) exists"; break;
    case PL_PG: c.actual = c.costs = c.rows = c.timing = c.buffers = true; break;
    case PL_MSSQL: c.actual = c.costs = c.rows = c.timing = c.warnings = true; break;
    case PL_MYSQL: c.actual = c.costs = c.rows = c.timing = true; break;
    case PL_MARIADB: c.actual = c.rows = c.timing = true; c.costs = false; break;
    case PL_ORACLE: c.actual = c.costs = c.rows = c.timing = c.buffers = true; break;
    }
    return c;
}

/* ---- model ---- */

void plan_init(Plan *p) { memset(p, 0, sizeof *p); }
void plan_free(Plan *p) {
    for (int i = 0; i < p->n; i++) { for (int k = 0; k < p->nodes[i].nprops; k++) free(p->nodes[i].props[k].val); free(p->nodes[i].props); }
    free(p->nodes); free(p->sql); free(p->raw);
    memset(p, 0, sizeof *p);
}
int plan_add_node(Plan *p, int parent, const char *op) {
    if (p->n == p->cap) { p->cap = p->cap ? p->cap * 2 : 32; p->nodes = realloc(p->nodes, sizeof *p->nodes * (size_t)p->cap); }
    PlanNode *n = &p->nodes[p->n];
    memset(n, 0, sizeof *n);
    n->id = p->n; n->parent = parent; n->first_child = n->next_sibling = -1;
    snprintf(n->op, sizeof n->op, "%s", op ? op : "?");
    snprintf(n->pid, sizeof n->pid, "%d", p->n);
    if (parent >= 0) {
        PlanNode *pa = &p->nodes[parent];
        n->depth = pa->depth + 1;
        if (pa->first_child < 0) pa->first_child = n->id;
        else { int c = pa->first_child; while (p->nodes[c].next_sibling >= 0) c = p->nodes[c].next_sibling; p->nodes[c].next_sibling = n->id; }
        pa->nchildren++;
    }
    return p->n++;
}
void plan_prop(PlanNode *n, const char *k, const char *v) {
    if (!v) return;
    for (int i = 0; i < n->nprops; i++) if (!strcmp(n->props[i].key, k)) { free(n->props[i].val); n->props[i].val = strdup(v); return; }
    n->props = realloc(n->props, sizeof *n->props * (size_t)(n->nprops + 1));
    snprintf(n->props[n->nprops].key, sizeof n->props[0].key, "%s", k);
    n->props[n->nprops++].val = strdup(v);
}
const char *plan_prop_get(const PlanNode *n, const char *k) { for (int i = 0; i < n->nprops; i++) if (!strcmp(n->props[i].key, k)) return n->props[i].val; return NULL; }
void plan_num_fmt(PlanNum v, char *o, int n, int dec) {
    if (v.st == PV_UNAVAILABLE) snprintf(o, (size_t)n, "not available");
    else if (v.st == PV_NA) snprintf(o, (size_t)n, "n/a");
    else if (v.v == floor(v.v) && fabs(v.v) < 1e15) snprintf(o, (size_t)n, "%.0f", v.v);
    else snprintf(o, (size_t)n, "%.*f", dec, v.v);
}
static PlanNum num(double v) { PlanNum r = { PV_PRESENT, v }; return r; }
static PlanNum num_s(const char *s) { if (!s || !*s) return (PlanNum){ PV_UNAVAILABLE, 0 }; char *e; double v = strtod(s, &e); return e == s ? (PlanNum){ PV_UNAVAILABLE, 0 } : num(v); }
static void set_kind(PlanNode *n) {
    char o[160]; snprintf(o, sizeof o, "%s", n->op); for (char *c = o; *c; c++) *c = (char)tolower((unsigned char)*c);
    const char *k = "other";
    if (strstr(o, "seek") || strstr(o, "search") || strstr(o, "index only scan") || strstr(o, "index scan") || strstr(o, "unique scan") || strstr(o, "range scan") || strstr(o, "index lookup") || strstr(o, "by index rowid")) k = "seek";
    else if (strstr(o, "scan") || strstr(o, "table access full") || strstr(o, "full") || !strcmp(o, "all")) k = "scan";
    if (strstr(o, "join") || strstr(o, "nested loop") || strstr(o, "hash match") || strstr(o, "merge")) k = "join";
    else if (strstr(o, "sort") || strstr(o, "order by") || strstr(o, "filesort")) k = "sort";
    else if (strstr(o, "aggregate") || strstr(o, "group") || strstr(o, "hash match (aggregate)")) k = "aggregate";
    else if (strstr(o, "limit") || strstr(o, "top") || strstr(o, "count stopkey")) k = "limit";
    else if (strstr(o, "temp") || strstr(o, "materialize") || strstr(o, "spool") || strstr(o, "hash") ) { if (strcmp(k, "seek") && strcmp(k, "scan")) k = "temp"; }
    else if (strstr(o, "insert") || strstr(o, "update") || strstr(o, "delete") || strstr(o, "modify")) k = "modify";
    else if (strstr(o, "filter")) k = "filter";
    else if (strstr(o, "result") || strstr(o, "select statement") || !strcmp(o, "select") || strstr(o, "query plan") || strstr(o, "query_block")) k = "result";
    snprintf(n->kind, sizeof n->kind, "%s", k);
}

/* ---- capture ---- */

void plan_sql_free(PlanSql *s) { for (int i = 0; i < s->n; i++) free(s->sql[i]); s->n = 0; }
static void ps_add(PlanSql *s, const char *fmt, ...) {
    if (s->n >= PLAN_MAXSQL) return;
    va_list ap; va_start(ap, fmt);
    int len = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *b = malloc((size_t)len + 1);
    va_start(ap, fmt); vsnprintf(b, (size_t)len + 1, fmt, ap); va_end(ap);
    s->sql[s->n++] = b;
}
static const char *skip_ws_comments(const char *s) {
    for (;;) {
        while (*s && isspace((unsigned char)*s)) s++;
        if (s[0] == '-' && s[1] == '-') { while (*s && *s != '\n') s++; continue; }
        if (s[0] == '/' && s[1] == '*') { const char *e = strstr(s + 2, "*/"); s = e ? e + 2 : s + strlen(s); continue; }
        return s;
    }
}
bool plan_stmt_writes(const char *stmt) {
    const char *s = skip_ws_comments(stmt);
    while (*s == '(') s = skip_ws_comments(s + 1);
    static const char *w[] = { "INSERT", "UPDATE", "DELETE", "MERGE", "CREATE", "ALTER", "DROP", "TRUNCATE", "GRANT", "REVOKE", "CALL", "EXEC", "EXECUTE", "DO", "COPY", "REPLACE", "UPSERT", "VACUUM", "ANALYZE", "REINDEX", "REFRESH", "BEGIN", "SET", NULL };
    for (int i = 0; w[i]; i++) { size_t L = strlen(w[i]); if (!strncasecmp(s, w[i], L) && !isalnum((unsigned char)s[L]) && s[L] != '_') return true; }
    /* data-modifying CTE, SELECT INTO, row locks */
    char up[8192]; int k = 0; char q = 0;
    for (const char *p = s; *p && k < (int)sizeof up - 1; p++) { if (q) { if (*p == q) q = 0; continue; } if (*p == '\'' || *p == '"') { q = *p; continue; } up[k++] = (char)toupper((unsigned char)*p); }
    up[k] = 0;
    /* data-modifying CTE (WITH ... INSERT / UPDATE / DELETE), SELECT ... INTO, row locks: whole words */
    static const char *in[] = { "INSERT", "UPDATE", "DELETE", "MERGE", NULL };
    for (int i = 0; in[i]; i++) {
        size_t L = strlen(in[i]);
        for (const char *f = strstr(up, in[i]); f; f = strstr(f + 1, in[i]))
            if ((f == up || !(isalnum((unsigned char)f[-1]) || f[-1] == '_')) && !(isalnum((unsigned char)f[L]) || f[L] == '_')) return true;
    }
    if (strstr(up, " FOR SHARE")) return true;
    if (!strncmp(up, "SELECT", 6) && strstr(up, " INTO ")) return true;
    return false;
}
/* trailing ';' and spaces off */
static void trim_stmt(const char *in, char *out, int n) {
    snprintf(out, (size_t)n, "%s", skip_ws_comments(in));
    size_t L = strlen(out); while (L && (isspace((unsigned char)out[L - 1]) || out[L - 1] == ';')) out[--L] = 0;
}
bool plan_capture_sql(int d, int mode, const char *stmt0, int version, PlanSql *o, char *why, int wn) {
    memset(o, 0, sizeof *o);
    size_t L = strlen(stmt0) + 1;
    char *st = malloc(L + 64); trim_stmt(stmt0, st, (int)L);
    if (!st[0]) { snprintf(why, (size_t)wn, "No statement under the cursor"); free(st); return false; }
    PlanCaps caps = plan_caps(d);
    bool writes = plan_stmt_writes(st);
    if (mode == PM_ACTUAL && !caps.actual) { snprintf(why, (size_t)wn, "%s", caps.why_no_actual); free(st); return false; }
    switch (d) {
    case PL_SQLITE:
        ps_add(o, "EXPLAIN QUERY PLAN %s", st); o->result = 0;
        snprintf(o->note, sizeof o->note, "EXPLAIN QUERY PLAN compiles the statement without running it; it reports access paths only (no rows, costs or time)");
        break;
    case PL_PG:
        if (mode == PM_ESTIMATED) { ps_add(o, "EXPLAIN (VERBOSE, FORMAT JSON) %s", st); o->result = 0; snprintf(o->note, sizeof o->note, "EXPLAIN without ANALYZE plans the statement and does not run it"); }
        else {
            /* ANALYZE runs it: inside a transaction that is always rolled back (side effects outside the table data, like sequences or notifications, can remain) */
            ps_add(o, "BEGIN"); ps_add(o, "EXPLAIN (ANALYZE, BUFFERS, VERBOSE, FORMAT JSON) %s", st); ps_add(o, "ROLLBACK");
            o->result = 1; o->finally_last = true;
            snprintf(o->note, sizeof o->note, "EXPLAIN ANALYZE RUNS the statement (inside BEGIN ... ROLLBACK; sequences, functions with side effects and external effects are not undone)");
        }
        break;
    case PL_MSSQL:
        /* TEXTSIZE: the client library would otherwise cut the XML at its default text size */
        if (mode == PM_ESTIMATED) { ps_add(o, "SET TEXTSIZE 2147483647"); ps_add(o, "SET SHOWPLAN_XML ON"); ps_add(o, "%s", st); ps_add(o, "SET SHOWPLAN_XML OFF"); snprintf(o->note, sizeof o->note, "SHOWPLAN_XML compiles the statement without running it; the setting is switched off afterwards even on failure"); }
        else { ps_add(o, "SET TEXTSIZE 2147483647"); ps_add(o, "SET STATISTICS XML ON"); ps_add(o, "%s", st); ps_add(o, "SET STATISTICS XML OFF"); snprintf(o->note, sizeof o->note, "STATISTICS XML RUNS the statement and returns the plan with runtime counters (read after the rows); the setting is switched off afterwards"); }
        o->result = 2; o->finally_last = true; o->last_result = mode == PM_ACTUAL;
        if (writes && mode == PM_ACTUAL) { snprintf(why, (size_t)wn, "Actual plans of statements that write are refused on SQL Server (they would really run)"); free(st); plan_sql_free(o); return false; }
        break;
    case PL_MYSQL:
        if (mode == PM_ESTIMATED) { ps_add(o, "EXPLAIN FORMAT=JSON %s", st); snprintf(o->note, sizeof o->note, "EXPLAIN plans the statement without running it"); }
        else {
            if (writes) { snprintf(why, (size_t)wn, "EXPLAIN ANALYZE would run a statement that writes: refused on MySQL (DDL / DML commit implicitly)"); free(st); return false; }
            if (version && version < 80018) { snprintf(why, (size_t)wn, "EXPLAIN ANALYZE needs MySQL 8.0.18"); free(st); return false; }
            ps_add(o, "EXPLAIN ANALYZE %s", st); snprintf(o->note, sizeof o->note, "EXPLAIN ANALYZE RUNS the SELECT and reports actual time, rows and loops");
        }
        o->result = 0;
        break;
    case PL_MARIADB:
        if (mode == PM_ESTIMATED) { ps_add(o, "EXPLAIN FORMAT=JSON %s", st); snprintf(o->note, sizeof o->note, "EXPLAIN plans the statement without running it"); }
        else {
            if (writes) { snprintf(why, (size_t)wn, "ANALYZE would run a statement that writes: refused on MariaDB (it commits implicitly)"); free(st); return false; }
            ps_add(o, "ANALYZE FORMAT=JSON %s", st); snprintf(o->note, sizeof o->note, "ANALYZE RUNS the statement and reports r_rows, r_loops and r_total_time_ms");
        }
        o->result = 0;
        break;
    case PL_ORACLE: {
        static unsigned seq; char sid[32]; snprintf(sid, sizeof sid, "LUME_%u", ++seq);
        if (mode == PM_ESTIMATED) {
            ps_add(o, "EXPLAIN PLAN SET STATEMENT_ID = '%s' FOR %s", sid, st);
            ps_add(o, "SELECT id, parent_id, operation, options, object_owner, object_name, cardinality, cost, bytes, access_predicates, filter_predicates, time, object_type FROM plan_table WHERE statement_id = '%s' ORDER BY id", sid);
            ps_add(o, "DELETE FROM plan_table WHERE statement_id = '%s'", sid);
            o->result = 1; o->finally_last = true;
            snprintf(o->note, sizeof o->note, "EXPLAIN PLAN writes the plan into the session's PLAN_TABLE (removed afterwards) and does not run the statement; bind peeking does not apply");
        } else {
            if (writes) { snprintf(why, (size_t)wn, "An actual plan would run a statement that writes: refused on Oracle (DDL commits implicitly; DML would need its own transaction decision)"); free(st); return false; }
            /* the statement runs with row-source statistics, then the last cursor's plan is read */
            const char *s = st; while (*s && isspace((unsigned char)*s)) s++;
            if (!strncasecmp(s, "SELECT", 6)) ps_add(o, "SELECT /*+ gather_plan_statistics */%s", s + 6);
            else if (!strncasecmp(s, "WITH", 4)) ps_add(o, "%s", s);
            else { snprintf(why, (size_t)wn, "Actual plans on Oracle are offered for SELECT / WITH queries"); free(st); return false; }
            ps_add(o, "SELECT plan_table_output FROM TABLE(DBMS_XPLAN.DISPLAY_CURSOR(NULL, NULL, 'ALLSTATS LAST'))");
            o->result = 1;
            snprintf(o->note, sizeof o->note, "Runs the query with gather_plan_statistics, then reads DBMS_XPLAN.DISPLAY_CURSOR (needs SELECT on V$SQL_PLAN_STATISTICS_ALL, V$SQL, V$SESSION)");
        }
        break;
    }
    default: snprintf(why, (size_t)wn, "No execution plans for this engine"); free(st); return false;
    }
    free(st);
    return true;
}

/* ---- JSON helpers ---- */

static int jchild(const Json *j, int node, const char *key) {
    if (node < 0) return -1;
    size_t kl = strlen(key);
    for (int c = j->nodes[node].first_child; c >= 0; c = j->nodes[c].next_sibling)
        if (j->nodes[c].key_len == kl && !memcmp(j->src + j->nodes[c].key_off, key, kl)) return c;
    return -1;
}
static void jtext(const Json *j, int node, char *out, int n) {
    out[0] = 0;
    if (node < 0) return;
    const JsonNode *x = &j->nodes[node];
    if (x->type == JN_STRING) json_unescape(j->src + x->val_off, x->val_len, out, (size_t)n);
    else if (x->type == JN_NUMBER || x->type == JN_TRUE || x->type == JN_FALSE) snprintf(out, (size_t)n, "%.*s", (int)x->val_len, j->src + x->val_off);
    else if (x->type == JN_ARRAY) {   /* ["a","b"] -> a, b */
        int w = 0;
        for (int c = x->first_child; c >= 0 && w < n - 4; c = j->nodes[c].next_sibling) { char b[512]; jtext(j, c, b, sizeof b); w += snprintf(out + w, (size_t)(n - w), "%s%s", w ? ", " : "", b); }
    } else if (x->type == JN_OBJECT) snprintf(out, (size_t)n, "%.*s", (int)(x->val_len < (uint32_t)n - 1 ? x->val_len : (uint32_t)n - 1), j->src + x->val_off);
}
static PlanNum jnum(const Json *j, int node, const char *key) { char b[64]; int c = jchild(j, node, key); if (c < 0) return (PlanNum){ PV_UNAVAILABLE, 0 }; jtext(j, c, b, sizeof b); return num_s(b); }
static void jprops(const Json *j, int node, PlanNode *n, const char *skip) {
    for (int c = j->nodes[node].first_child; c >= 0; c = j->nodes[c].next_sibling) {
        char k[64]; json_unescape(j->src + j->nodes[c].key_off, j->nodes[c].key_len, k, sizeof k);
        if (skip && strstr(skip, k)) continue;
        if (j->nodes[c].type == JN_OBJECT || (j->nodes[c].type == JN_ARRAY && j->nodes[c].nchildren && j->nodes[j->nodes[c].first_child].type == JN_OBJECT)) continue;
        char v[1200]; jtext(j, c, v, sizeof v); plan_prop(n, k, v);
    }
}

/* ---- PostgreSQL ---- */

static void pg_node(Plan *p, const Json *j, int jn, int parent) {
    char op[128]; jtext(j, jchild(j, jn, "Node Type"), op, sizeof op);
    char jt[64]; jtext(j, jchild(j, jn, "Join Type"), jt, sizeof jt);
    char strat[64]; jtext(j, jchild(j, jn, "Strategy"), strat, sizeof strat);
    char full[200];
    if (jt[0] && strstr(op, "Join")) snprintf(full, sizeof full, "%s (%s)", op, jt);
    else if (strat[0] && !strcmp(op, "Aggregate")) snprintf(full, sizeof full, "%s (%s)", op, strat);
    else snprintf(full, sizeof full, "%s", op);
    int id = plan_add_node(p, parent, full);
    PlanNode *n = &p->nodes[id];
    jprops(j, jn, n, "Plans");
    char rel[128] = "", sch[128] = ""; jtext(j, jchild(j, jn, "Relation Name"), rel, sizeof rel); jtext(j, jchild(j, jn, "Schema"), sch, sizeof sch);
    if (rel[0]) snprintf(n->object, sizeof n->object, "%s%s%s", sch, sch[0] ? "." : "", rel);
    jtext(j, jchild(j, jn, "Index Name"), n->index, sizeof n->index);
    static const char *pk[] = { "Index Cond", "Recheck Cond", "Hash Cond", "Merge Cond", "Join Filter", "Filter", NULL };
    int w = 0;
    for (int i = 0; pk[i]; i++) { char b[600]; jtext(j, jchild(j, jn, pk[i]), b, sizeof b); if (b[0] && w < (int)sizeof n->predicate - 2) w += snprintf(n->predicate + w, sizeof n->predicate - (size_t)w, "%s%s: %s", w ? "; " : "", pk[i], b); }
    n->est_rows = jnum(j, jn, "Plan Rows"); n->est_cost = jnum(j, jn, "Total Cost"); n->est_startup = jnum(j, jn, "Startup Cost"); n->width = jnum(j, jn, "Plan Width");
    if (p->mode == PM_ACTUAL) {
        n->act_rows = jnum(j, jn, "Actual Rows"); n->loops = jnum(j, jn, "Actual Loops");
        n->act_ms = jnum(j, jn, "Actual Total Time"); n->act_first_ms = jnum(j, jn, "Actual Startup Time");
        n->removed = jnum(j, jn, "Rows Removed by Filter");
        if (n->act_rows.st != PV_PRESENT && jchild(j, jn, "Actual Loops") >= 0) { n->act_rows.st = PV_NA; }   /* never executed */
    } else { n->act_rows.st = n->loops.st = n->act_ms.st = PV_NA; }
    char sst[32]; jtext(j, jchild(j, jn, "Sort Space Type"), sst, sizeof sst);
    if (!strcmp(sst, "Disk")) snprintf(n->warning, sizeof n->warning, "Sort spilled to disk (Sort Space Type: Disk)");
    PlanNum hb = jnum(j, jn, "Hash Batches");
    if (hb.st == PV_PRESENT && hb.v > 1) snprintf(n->warning, sizeof n->warning, "Hash used %.0f batches (spilled to disk)", hb.v);
    set_kind(n);
    int plans = jchild(j, jn, "Plans");
    if (plans >= 0) for (int c = j->nodes[plans].first_child; c >= 0; c = j->nodes[c].next_sibling) pg_node(p, j, c, id);
}
static bool parse_pg(const char *txt, Plan *p, char *err, int en) {
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, txt, strlen(txt))) { snprintf(err, (size_t)en, "PostgreSQL plan JSON: %s at %zu", j.err, j.err_off); json_free(&j); return false; }
    int root = 0;
    if (j.nodes[0].type == JN_ARRAY) root = j.nodes[0].first_child;
    int pl = jchild(&j, root, "Plan");
    if (pl < 0) { snprintf(err, (size_t)en, "PostgreSQL plan JSON has no \"Plan\""); json_free(&j); return false; }
    p->planning_ms = jnum(&j, root, "Planning Time"); p->exec_ms = jnum(&j, root, "Execution Time");
    if (p->mode == PM_ESTIMATED) { p->planning_ms.st = PV_UNAVAILABLE; p->exec_ms.st = PV_NA; }
    pg_node(p, &j, pl, -1);
    if (p->n) p->total_cost = p->nodes[0].est_cost;
    json_free(&j);
    return true;
}

/* ---- SQLite ---- */

static bool parse_sqlite_rows(ResultSet *rs, Plan *p, char *err, int en) {
    if (!rs || rs->ncols < 4) { snprintf(err, (size_t)en, "EXPLAIN QUERY PLAN returned no plan"); return false; }
    int root = plan_add_node(p, -1, "QUERY PLAN");
    snprintf(p->nodes[root].kind, sizeof p->nodes[root].kind, "result");
    int map_id[4096], map_node[4096], nm = 0;
    for (int r = 0; r < rs->nrows; r++) {
        int id = atoi(rs_cell(rs, r, 0) ? rs_cell(rs, r, 0) : "0"), par = atoi(rs_cell(rs, r, 1) ? rs_cell(rs, r, 1) : "0");
        const char *det = rs_cell(rs, r, 3) ? rs_cell(rs, r, 3) : "";
        int parent = root;
        for (int i = 0; i < nm; i++) if (map_id[i] == par) parent = map_node[i];
        int n = plan_add_node(p, parent, det);
        PlanNode *x = &p->nodes[n];
        snprintf(x->pid, sizeof x->pid, "%d", id);
        plan_prop(x, "detail", det);
        /* SCAN t [USING [COVERING] INDEX i] / SEARCH t USING [COVERING] INDEX i (a=?) / USE TEMP B-TREE FOR ... */
        char w1[32] = "", w2[200] = ""; sscanf(det, "%31s %199s", w1, w2);
        if (!strcmp(w1, "SCAN") || !strcmp(w1, "SEARCH")) {
            if (!strcmp(w2, "TABLE")) sscanf(det, "%*s %*s %199s", w2);
            snprintf(x->object, sizeof x->object, "%s", w2);
            const char *ui = strstr(det, "INDEX ");
            if (ui) { sscanf(ui + 6, "%159s", x->index); }
            if (strstr(det, "COVERING INDEX")) plan_prop(x, "covering index", "yes");
            if (strstr(det, "INTEGER PRIMARY KEY")) snprintf(x->index, sizeof x->index, "INTEGER PRIMARY KEY");
            const char *pc = strchr(det, '('); if (pc) snprintf(x->predicate, sizeof x->predicate, "%s", pc);
            snprintf(x->kind, sizeof x->kind, "%s", !strcmp(w1, "SEARCH") ? "seek" : "scan");
        } else if (strstr(det, "TEMP B-TREE")) { snprintf(x->kind, sizeof x->kind, "temp"); plan_prop(x, "temp structure", det); }
        else set_kind(x);
        /* SQLite gives no rows, cost or time: not applicable, never zero (EPL-012.3) */
        x->est_rows.st = x->act_rows.st = x->est_cost.st = x->act_ms.st = x->loops.st = PV_NA;
        if (nm < 4096) { map_id[nm] = id; map_node[nm] = n; nm++; }
    }
    p->total_cost.st = PV_NA; p->exec_ms.st = PV_NA; p->planning_ms.st = PV_NA;
    snprintf(p->notes, sizeof p->notes, "SQLite's EXPLAIN QUERY PLAN lists access paths only: no rows, costs, percentages or time. Its format can change between SQLite versions; unknown lines are kept as written.");
    return true;
}

/* ---- SQL Server Showplan XML: a small strict reader ---- */

typedef struct XAttr { char name[48]; char val[1200]; } XAttr;
static void xdecode(const char *s, int len, char *out, int n) {
    int o = 0;
    for (int i = 0; i < len && o < n - 4; i++) {
        if (s[i] == '&') {
            const char *e = memchr(s + i, ';', (size_t)(len - i));
            if (e) {
                int el = (int)(e - (s + i));
                if (el == 5 && !strncmp(s + i, "&quot", 5)) out[o++] = '"';
                else if (el == 4 && !strncmp(s + i, "&amp", 4)) out[o++] = '&';
                else if (el == 3 && !strncmp(s + i, "&lt", 3)) out[o++] = '<';
                else if (el == 3 && !strncmp(s + i, "&gt", 3)) out[o++] = '>';
                else if (el == 5 && !strncmp(s + i, "&apos", 5)) out[o++] = '\'';
                else if (s[i + 1] == '#') { long c = s[i + 2] == 'x' ? strtol(s + i + 3, NULL, 16) : strtol(s + i + 2, NULL, 10); if (c > 0 && c < 128) out[o++] = (char)c; else out[o++] = '?'; }
                else { memcpy(out + o, s + i, (size_t)(el + 1 < n - o - 1 ? el + 1 : 0)); o += el + 1 < n - o - 1 ? el + 1 : 0; }
                i += el; continue;
            }
        }
        out[o++] = s[i];
    }
    out[o] = 0;
}
static const char *xattr(XAttr *a, int na, const char *name) { for (int i = 0; i < na; i++) if (!strcmp(a[i].name, name)) return a[i].val; return NULL; }
static bool parse_sqlplan(const char *x, Plan *p, char *err, int en) {
    size_t len = strlen(x);
    if (len > (32u << 20)) { snprintf(err, (size_t)en, "Showplan larger than 32 MB: refused"); return false; }
    if (strstr(x, "<!DOCTYPE") || strstr(x, "<!ENTITY")) { snprintf(err, (size_t)en, "Showplan with DOCTYPE / ENTITY declarations: refused (no external entities)"); return false; }
    if (!strstr(x, "ShowPlanXML")) { snprintf(err, (size_t)en, "Not a Showplan XML document"); return false; }
    char stack[256][48]; int relstack[256]; int depth = 0;
    int cur_root = -1;
    static XAttr at[64];
    const char *s = x;
    char missing[1024] = ""; int mw = 0;
    while ((s = strchr(s, '<'))) {
        if (!strncmp(s, "<?", 2)) { s = strstr(s, "?>"); if (!s) break; continue; }
        if (!strncmp(s, "<!--", 4)) { s = strstr(s, "-->"); if (!s) break; continue; }
        if (s[1] == '/') { if (depth > 0) depth--; s = strchr(s, '>'); if (!s) break; continue; }
        const char *e = s + 1; while (*e && !isspace((unsigned char)*e) && *e != '>' && *e != '/') e++;
        char nm[48]; int nl = (int)(e - s - 1); snprintf(nm, sizeof nm, "%.*s", nl < 47 ? nl : 47, s + 1);
        int na = 0;
        const char *q = e;
        for (;;) {
            while (*q && isspace((unsigned char)*q)) q++;
            if (!*q || *q == '>' || *q == '/') break;
            const char *ae = q; while (*ae && *ae != '=' && !isspace((unsigned char)*ae)) ae++;
            if (*ae != '=') { q = ae; continue; }
            char quote = ae[1]; if (quote != '"' && quote != '\'') break;
            const char *ve = strchr(ae + 2, quote); if (!ve) break;
            if (na < 64) { snprintf(at[na].name, sizeof at[na].name, "%.*s", (int)(ae - q < 47 ? ae - q : 47), q); xdecode(ae + 2, (int)(ve - ae - 2), at[na].val, sizeof at[na].val); na++; }
            q = ve + 1;
        }
        bool selfclose = *q == '/';
        const char *gt = strchr(q, '>'); if (!gt) break;
        int parent_rel = depth > 0 ? relstack[depth - 1] : -1;
        int this_rel = parent_rel;
        if (!strcmp(nm, "StmtSimple") || !strcmp(nm, "StmtCond") || !strcmp(nm, "StmtCursor")) {
            const char *ty = xattr(at, na, "StatementType");
            cur_root = plan_add_node(p, -1, ty ? ty : nm);
            snprintf(p->nodes[cur_root].kind, sizeof p->nodes[cur_root].kind, "result");
            snprintf(p->nodes[cur_root].pid, sizeof p->nodes[cur_root].pid, "stmt");   /* RelOp NodeIds start at 0 */
            p->nodes[cur_root].est_cost = num_s(xattr(at, na, "StatementSubTreeCost"));
            p->nodes[cur_root].est_rows = num_s(xattr(at, na, "StatementEstRows"));
            for (int i = 0; i < na; i++) plan_prop(&p->nodes[cur_root], at[i].name, at[i].val);
            if (p->total_cost.st != PV_PRESENT) p->total_cost = p->nodes[cur_root].est_cost;
            this_rel = cur_root;
        } else if (!strcmp(nm, "RelOp")) {
            const char *po = xattr(at, na, "PhysicalOp"), *lo = xattr(at, na, "LogicalOp");
            char op[128]; if (po && lo && strcmp(po, lo)) snprintf(op, sizeof op, "%s (%s)", po, lo); else snprintf(op, sizeof op, "%s", po ? po : lo ? lo : "RelOp");
            int id = plan_add_node(p, parent_rel >= 0 ? parent_rel : cur_root, op);
            PlanNode *n = &p->nodes[id];
            const char *nid = xattr(at, na, "NodeId"); if (nid) snprintf(n->pid, sizeof n->pid, "%s", nid);
            for (int i = 0; i < na; i++) plan_prop(n, at[i].name, at[i].val);
            n->est_rows = num_s(xattr(at, na, "EstimateRows")); n->est_cost = num_s(xattr(at, na, "EstimatedTotalSubtreeCost"));
            if (p->mode == PM_ESTIMATED) { n->act_rows.st = n->loops.st = n->act_ms.st = PV_NA; }
            set_kind(n);
            this_rel = id;
        } else if (parent_rel >= 0 && parent_rel < p->n) {
            PlanNode *n = &p->nodes[parent_rel];
            if (!strcmp(nm, "Object") && !n->object[0]) {
                const char *t = xattr(at, na, "Table"), *sc = xattr(at, na, "Schema"), *ix = xattr(at, na, "Index");
                if (t) snprintf(n->object, sizeof n->object, "%s%s%s", sc ? sc : "", sc ? "." : "", t);
                if (ix) snprintf(n->index, sizeof n->index, "%s", ix);
            } else if (!strcmp(nm, "ScalarOperator") && !n->predicate[0] && depth >= 1 && (!strcmp(stack[depth - 1], "Predicate") || !strcmp(stack[depth - 1], "SeekPredicates") || strstr(stack[depth - 1], "Predicate") || !strcmp(stack[depth - 1], "RangeExpressions") || !strcmp(stack[depth - 1], "ProbeResidual"))) {
                const char *ss = xattr(at, na, "ScalarString"); if (ss) snprintf(n->predicate, sizeof n->predicate, "%s", ss);
            } else if (!strcmp(nm, "RunTimeCountersPerThread")) {
                PlanNum ar = num_s(xattr(at, na, "ActualRows")), ae = num_s(xattr(at, na, "ActualExecutions")), am = num_s(xattr(at, na, "ActualElapsedms"));
                if (ar.st == PV_PRESENT) { n->act_rows.v = (n->act_rows.st == PV_PRESENT ? n->act_rows.v : 0) + ar.v; n->act_rows.st = PV_PRESENT; }
                if (ae.st == PV_PRESENT) { n->loops.v = (n->loops.st == PV_PRESENT ? n->loops.v : 0) + ae.v; n->loops.st = PV_PRESENT; }
                if (am.st == PV_PRESENT && (n->act_ms.st != PV_PRESENT || am.v > n->act_ms.v)) n->act_ms = am;
                /* counters the SQL Server tooltip shows, summed over threads */
                static const char *const sumk[] = { "ActualRowsRead", "ActualLogicalReads", "ActualPhysicalReads", "ActualCPUms", NULL };
                for (int k = 0; sumk[k]; k++) { PlanNum x = num_s(xattr(at, na, sumk[k])); if (x.st != PV_PRESENT) continue; const char *old = plan_prop_get(n, sumk[k]); char b[48]; snprintf(b, sizeof b, "%.15g", x.v + (old ? atof(old) : 0)); plan_prop(n, sumk[k], b); }
                const char *em = xattr(at, na, "ActualExecutionMode"); if (em && !plan_prop_get(n, "ActualExecutionMode")) plan_prop(n, "ActualExecutionMode", em);
            } else if (!strcmp(nm, "SpillToTempDb") || !strcmp(nm, "PlanAffectingConvert") || !strcmp(nm, "NoJoinPredicate") || !strcmp(nm, "ColumnsWithNoStatistics") || !strcmp(nm, "SortSpillDetails") || !strcmp(nm, "HashSpillDetails")) {
                char w[256]; snprintf(w, sizeof w, "%s%s%s", nm, xattr(at, na, "Expression") ? ": " : "", xattr(at, na, "Expression") ? xattr(at, na, "Expression") : "");
                snprintf(n->warning, sizeof n->warning, "%s (reported by SQL Server)", w);
            }
        }
        if (!strcmp(nm, "MissingIndexGroup") && cur_root >= 0 && !plan_prop_get(&p->nodes[cur_root], "MissingIndexImpact") && xattr(at, na, "Impact")) plan_prop(&p->nodes[cur_root], "MissingIndexImpact", xattr(at, na, "Impact"));
        if (!strcmp(nm, "MissingIndex") || (depth > 0 && !strcmp(nm, "Column") && !strcmp(stack[depth - 1], "ColumnGroup"))) {
            if (!strcmp(nm, "MissingIndex")) mw += snprintf(missing + mw, sizeof missing - (size_t)mw, "%sCREATE INDEX hint on %s.%s:", mw ? "\n" : "", xattr(at, na, "Schema") ? xattr(at, na, "Schema") : "", xattr(at, na, "Table") ? xattr(at, na, "Table") : "");
            else if (mw < (int)sizeof missing - 100) mw += snprintf(missing + mw, sizeof missing - (size_t)mw, " %s", xattr(at, na, "Name") ? xattr(at, na, "Name") : "");
        }
        if (!strcmp(nm, "ColumnGroup") && mw < (int)sizeof missing - 100) mw += snprintf(missing + mw, sizeof missing - (size_t)mw, " [%s]", xattr(at, na, "Usage") ? xattr(at, na, "Usage") : "");
        if (!selfclose && depth < 256) { snprintf(stack[depth], sizeof stack[0], "%s", nm); relstack[depth] = this_rel; depth++; }
        s = gt + 1;
    }
    if (!p->n) { snprintf(err, (size_t)en, "The Showplan has no statement / operator"); return false; }
    if (missing[0]) { plan_prop(&p->nodes[0], "MissingIndexes (SQL Server hint)", missing); }
    return true;
}

/* ---- MySQL / MariaDB JSON ---- */

static void my_table(Plan *p, const Json *j, int t, int parent, bool maria) {
    char tn[128] = "", at[32] = "", key[128] = "", op[200];
    jtext(j, jchild(j, t, "table_name"), tn, sizeof tn); jtext(j, jchild(j, t, "access_type"), at, sizeof at); jtext(j, jchild(j, t, "key"), key, sizeof key);
    snprintf(op, sizeof op, "%s%s%s", at[0] ? at : "table", tn[0] ? " " : "", tn);
    int id = plan_add_node(p, parent, op);
    PlanNode *n = &p->nodes[id];
    jprops(j, t, n, "cost_info");
    snprintf(n->object, sizeof n->object, "%s", tn); snprintf(n->index, sizeof n->index, "%s", key);
    jtext(j, jchild(j, t, "attached_condition"), n->predicate, sizeof n->predicate);
    snprintf(n->kind, sizeof n->kind, "%s", !strcmp(at, "ALL") ? "scan" : !strcmp(at, "index") ? "scan" : at[0] ? "seek" : "other");
    if (maria) {
        n->est_rows = jnum(j, t, "rows");
        n->act_rows = jnum(j, t, "r_rows"); n->loops = jnum(j, t, "r_loops"); n->act_ms = jnum(j, t, "r_total_time_ms");
        n->est_cost.st = PV_UNAVAILABLE;
        if (p->mode == PM_ESTIMATED) n->act_rows.st = n->loops.st = n->act_ms.st = PV_NA;
    } else {
        n->est_rows = jnum(j, t, "rows_examined_per_scan");
        int ci = jchild(j, t, "cost_info");
        n->est_cost = ci >= 0 ? jnum(j, ci, "prefix_cost") : (PlanNum){ PV_UNAVAILABLE, 0 };
        if (ci >= 0) jprops(j, ci, n, NULL);
        n->act_rows.st = n->loops.st = n->act_ms.st = PV_NA;
    }
    if (n->kind[0] == 'o') set_kind(n);
}
/* query_block -> operations / nested_loop / table (format 1; MariaDB's own keys) */
static void my_block(Plan *p, const Json *j, int b, int parent, bool maria) {
    for (int c = j->nodes[b].first_child; c >= 0; c = j->nodes[c].next_sibling) {
        char k[64]; json_unescape(j->src + j->nodes[c].key_off, j->nodes[c].key_len, k, sizeof k);
        const JsonNode *x = &j->nodes[c];
        if (!strcmp(k, "table") && x->type == JN_OBJECT) my_table(p, j, c, parent, maria);
        else if (!strcmp(k, "nested_loop") && x->type == JN_ARRAY) {
            int nl = plan_add_node(p, parent, "nested loop"); snprintf(p->nodes[nl].kind, sizeof p->nodes[nl].kind, "join");
            for (int e = x->first_child; e >= 0; e = j->nodes[e].next_sibling) my_block(p, j, e, nl, maria);
        } else if (x->type == JN_OBJECT && (!strcmp(k, "ordering_operation") || !strcmp(k, "grouping_operation") || !strcmp(k, "duplicates_removal") || !strcmp(k, "filesort") || !strcmp(k, "temporary_table") ||
                                            !strcmp(k, "read_sorted_file") || !strcmp(k, "having_condition") || !strcmp(k, "windowing") || !strcmp(k, "materialized_from_subquery") || !strcmp(k, "query_block") || !strcmp(k, "union_result"))) {
            char op[96]; snprintf(op, sizeof op, "%s", k);
            int id = plan_add_node(p, parent, op);
            jprops(j, c, &p->nodes[id], NULL);
            if (jchild(j, c, "using_filesort") >= 0) { char v[16]; jtext(j, jchild(j, c, "using_filesort"), v, sizeof v); if (!strcmp(v, "true")) plan_prop(&p->nodes[id], "filesort", "yes"); }
            set_kind(&p->nodes[id]);
            if (maria) { p->nodes[id].act_rows = jnum(j, c, "r_rows"); p->nodes[id].loops = jnum(j, c, "r_loops"); p->nodes[id].act_ms = jnum(j, c, "r_total_time_ms"); }
            my_block(p, j, c, id, maria);
        } else if (x->type == JN_ARRAY && (!strcmp(k, "query_specifications") || !strcmp(k, "subqueries") || !strcmp(k, "attached_subqueries"))) {
            for (int e = x->first_child; e >= 0; e = j->nodes[e].next_sibling) my_block(p, j, e, parent, maria);
        }
    }
}
/* MySQL EXPLAIN FORMAT=JSON format version 2: an iterator tree with "inputs" */
static void my_v2(Plan *p, const Json *j, int o, int parent) {
    char op[200]; jtext(j, jchild(j, o, "operation"), op, sizeof op);
    int id = plan_add_node(p, parent, op[0] ? op : "operation");
    PlanNode *n = &p->nodes[id];
    jprops(j, o, n, "inputs");
    jtext(j, jchild(j, o, "table_name"), n->object, sizeof n->object);
    jtext(j, jchild(j, o, "index_name"), n->index, sizeof n->index);
    jtext(j, jchild(j, o, "condition"), n->predicate, sizeof n->predicate);
    n->est_rows = jnum(j, o, "estimated_rows"); n->est_cost = jnum(j, o, "estimated_total_cost");
    n->act_rows = jnum(j, o, "actual_rows"); n->loops = jnum(j, o, "actual_loops"); n->act_ms = jnum(j, o, "actual_last_row_ms");
    if (p->mode == PM_ESTIMATED) n->act_rows.st = n->loops.st = n->act_ms.st = PV_NA;
    char at[32]; jtext(j, jchild(j, o, "access_type"), at, sizeof at);
    if (!strcmp(at, "table")) snprintf(n->kind, sizeof n->kind, "scan"); else if (!strcmp(at, "index")) snprintf(n->kind, sizeof n->kind, "seek"); else set_kind(n);
    int in = jchild(j, o, "inputs");
    if (in >= 0) for (int c = j->nodes[in].first_child; c >= 0; c = j->nodes[c].next_sibling) my_v2(p, j, c, id);
}
static bool parse_my_json(const char *txt, Plan *p, bool maria, char *err, int en) {
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, txt, strlen(txt))) { snprintf(err, (size_t)en, "%s plan JSON: %s at %zu", maria ? "MariaDB" : "MySQL", j.err, j.err_off); json_free(&j); return false; }
    int qb = jchild(&j, 0, "query_block"), qp = jchild(&j, 0, "query_plan");
    if (qp >= 0 || jchild(&j, 0, "operation") >= 0) { my_v2(p, &j, qp >= 0 ? qp : 0, -1); snprintf(p->adapter, sizeof p->adapter, "mysql-json-2"); }
    else if (qb >= 0) {
        int root = plan_add_node(p, -1, "query_block");
        snprintf(p->nodes[root].kind, sizeof p->nodes[root].kind, "result");
        jprops(&j, qb, &p->nodes[root], NULL);
        int ci = jchild(&j, qb, "cost_info");
        if (ci >= 0 && !maria) { p->total_cost = jnum(&j, ci, "query_cost"); p->nodes[root].est_cost = p->total_cost; }
        if (maria) { p->exec_ms = jnum(&j, qb, "r_total_time_ms"); if (p->mode == PM_ESTIMATED) p->exec_ms.st = PV_NA; }
        my_block(p, &j, qb, root, maria);
        snprintf(p->adapter, sizeof p->adapter, maria ? "mariadb-json" : "mysql-json-1");
    } else { snprintf(err, (size_t)en, "No query_block / query_plan in the %s plan", maria ? "MariaDB" : "MySQL"); json_free(&j); return false; }
    if (maria) snprintf(p->notes, sizeof p->notes, "MariaDB reports no optimizer cost per table in JSON: costs are shown as not available. %s", p->mode == PM_ACTUAL ? "r_rows is an average per loop." : "");
    else if (p->mode == PM_ESTIMATED) snprintf(p->notes, sizeof p->notes, "MySQL estimated plan: rows are rows examined per scan; costs are the optimizer's units.");
    json_free(&j);
    return true;
}
/* MySQL EXPLAIN ANALYZE (TREE): "-> Op  (cost=.. rows=..) (actual time=a..b rows=r loops=l)", 4 spaces per level */
static bool parse_my_tree(const char *txt, Plan *p, char *err, int en) {
    int stack[256]; int lvl_of[256]; int sp = 0;
    const char *s = txt;
    while (*s) {
        const char *e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
        char line[4096]; snprintf(line, sizeof line, "%.*s", len < 4095 ? len : 4095, s);
        s += len + (e ? 1 : 0);
        char *arrow = strstr(line, "-> ");
        if (!arrow) continue;
        int indent = (int)(arrow - line) / 4;
        char *body = arrow + 3;
        char *cost = strstr(body, "  (cost="), *act = strstr(body, "(actual time="), *never = strstr(body, "(never executed)");
        char op[1024]; int oplen = (int)((cost ? cost : act ? act - 1 : never ? never - 1 : body + strlen(body)) - body);
        if (oplen < 0) oplen = (int)strlen(body);
        snprintf(op, sizeof op, "%.*s", oplen, body);
        while (sp > 0 && lvl_of[sp - 1] >= indent) sp--;
        int parent = sp > 0 ? stack[sp - 1] : -1;
        char opname[128]; snprintf(opname, sizeof opname, "%.*s", (int)strcspn(op, ":"), op);
        int id = plan_add_node(p, parent, opname);
        PlanNode *n = &p->nodes[id];
        plan_prop(n, "line", body);
        const char *colon = strchr(op, ':'); if (colon) snprintf(n->predicate, sizeof n->predicate, "%s", colon + 2);
        const char *on = strstr(op, " on "); if (on) { char t[128]; sscanf(on + 4, "%127s", t); snprintf(n->object, sizeof n->object, "%s", t); }
        const char *ui = strstr(op, "using "); if (ui) { char t[128]; sscanf(ui + 6, "%127s", t); snprintf(n->index, sizeof n->index, "%s", t); }
        if (cost) { double c = 0, r = 0; if (sscanf(cost, "  (cost=%lf rows=%lf", &c, &r) == 2) { n->est_cost = num(c); n->est_rows = num(r); } else if (sscanf(cost, "  (cost=%lf..%lf rows=%lf", &c, &c, &r) == 3) { n->est_cost = num(c); n->est_rows = num(r); } }
        if (act) { double a = 0, b = 0, r = 0, l = 0; if (sscanf(act, "(actual time=%lf..%lf rows=%lf loops=%lf", &a, &b, &r, &l) == 4) { n->act_first_ms = num(a); n->act_ms = num(b); n->act_rows = num(r); n->loops = num(l); } }
        if (never) { n->act_rows.st = PV_NA; plan_prop(n, "executed", "never"); }
        set_kind(n);
        if (sp < 256) { stack[sp] = id; lvl_of[sp] = indent; sp++; }
    }
    if (!p->n) { snprintf(err, (size_t)en, "EXPLAIN ANALYZE output has no '->' operator lines (format not recognized: shown as text)"); return false; }
    snprintf(p->adapter, sizeof p->adapter, "mysql-tree");
    snprintf(p->notes, sizeof p->notes, "MySQL EXPLAIN ANALYZE: actual time is per loop (first row..all rows, ms); rows is the average per loop.");
    return true;
}

/* ---- Oracle ---- */

static bool parse_ora_rows(ResultSet *rs, Plan *p, char *err, int en) {
    if (!rs || rs->ncols < 12 || !rs->nrows) { snprintf(err, (size_t)en, "PLAN_TABLE returned no rows for this statement"); return false; }
    int ids[4096], nodes[4096], nm = 0;
    for (int r = 0; r < rs->nrows; r++) {
#define C(i) (rs_cell(rs, r, i) ? rs_cell(rs, r, i) : "")
        int id = atoi(C(0)); const char *par = rs_cell(rs, r, 1);
        int parent = -1;
        if (par) for (int i = 0; i < nm; i++) if (ids[i] == atoi(par)) parent = nodes[i];
        char op[160]; snprintf(op, sizeof op, "%s%s%s", C(2), C(3)[0] ? " " : "", C(3));
        int n = plan_add_node(p, parent, op);
        PlanNode *x = &p->nodes[n];
        snprintf(x->pid, sizeof x->pid, "%d", id);
        if (C(5)[0]) { if (!strncmp(C(12), "INDEX", 5)) { snprintf(x->index, sizeof x->index, "%s", C(5)); } else snprintf(x->object, sizeof x->object, "%s%s%s", C(4), C(4)[0] ? "." : "", C(5)); }
        x->est_rows = num_s(C(6)); x->est_cost = num_s(C(7));
        int w = 0;
        if (C(9)[0]) w += snprintf(x->predicate + w, sizeof x->predicate - (size_t)w, "access: %s", C(9));
        if (C(10)[0]) snprintf(x->predicate + w, sizeof x->predicate - (size_t)w, "%sfilter: %s", w ? "; " : "", C(10));
        plan_prop(x, "bytes", C(8)); plan_prop(x, "time (s, optimizer estimate)", C(11)); plan_prop(x, "object type", C(12));
        x->act_rows.st = x->loops.st = x->act_ms.st = PV_NA;
        set_kind(x);
        if (nm < 4096) { ids[nm] = id; nodes[nm] = n; nm++; }
#undef C
    }
    if (p->n) p->total_cost = p->nodes[0].est_cost;
    snprintf(p->notes, sizeof p->notes, "Oracle EXPLAIN PLAN: the optimizer's plan without bind peeking; the runtime plan can differ (Explain Actual reads the executed cursor).");
    return true;
}
/* DBMS_XPLAN.DISPLAY_CURSOR text (ALLSTATS LAST) */
static bool parse_ora_text(const char *txt, Plan *p, char *err, int en) {
    char cols[16][32]; int nc = 0; bool in_table = false;
    int stack[256]; int lvl_of[256]; int sp = 0;
    const char *s = txt;
    char preds[8192] = ""; bool in_pred = false;
    while (*s) {
        const char *e = strchr(s, '\n'); int len = e ? (int)(e - s) : (int)strlen(s);
        char line[2048]; snprintf(line, sizeof line, "%.*s", len < 2047 ? len : 2047, s);
        s += len + (e ? 1 : 0);
        if (strstr(line, "Predicate Information")) { in_pred = true; in_table = false; continue; }
        if (in_pred) { size_t pl = strlen(preds); if (pl < sizeof preds - 400) snprintf(preds + pl, sizeof preds - pl, "%s\n", line); continue; }
        if (line[0] != '|') continue;
        if (strstr(line, "| Id") || strstr(line, "|  Id")) {   /* header */
            nc = 0; char *t = line + 1;
            while (*t && nc < 16) { char *bar = strchr(t, '|'); if (!bar) break; *bar = 0; char c[32]; snprintf(c, sizeof c, "%s", t); char *a = c; while (*a == ' ') a++; char *z = a + strlen(a); while (z > a && z[-1] == ' ') *--z = 0; snprintf(cols[nc++], sizeof cols[0], "%s", a); t = bar + 1; }
            in_table = true; continue;
        }
        if (!in_table || !nc) continue;
        char cells[16][256]; int k = 0; char *t = line + 1;
        while (*t && k < nc) { char *bar = strchr(t, '|'); if (!bar) break; *bar = 0; snprintf(cells[k++], sizeof cells[0], "%s", t); t = bar + 1; }
        if (k < 2) continue;
        int idc = -1, opc = -1, nmc = -1;
        for (int i = 0; i < nc; i++) { if (!strcmp(cols[i], "Id")) idc = i; if (!strcmp(cols[i], "Operation")) opc = i; if (!strcmp(cols[i], "Name")) nmc = i; }
        if (idc < 0 || opc < 0) continue;
        char *opraw = cells[opc]; if (*opraw == ' ') opraw++;
        int indent = (int)strspn(opraw, " ");
        char op[160]; snprintf(op, sizeof op, "%s", opraw + indent); { char *z = op + strlen(op); while (z > op && z[-1] == ' ') *--z = 0; }
        char idt[32]; snprintf(idt, sizeof idt, "%s", cells[idc]); char *ip = idt; while (*ip == ' ' || *ip == '*') ip++;
        while (sp > 0 && lvl_of[sp - 1] >= indent) sp--;
        int id = plan_add_node(p, sp > 0 ? stack[sp - 1] : -1, op);
        PlanNode *n = &p->nodes[id];
        snprintf(n->pid, sizeof n->pid, "%d", atoi(ip));
        if (nmc >= 0) { char nm[160]; snprintf(nm, sizeof nm, "%s", cells[nmc]); char *a = nm; while (*a == ' ') a++; char *z = a + strlen(a); while (z > a && z[-1] == ' ') *--z = 0; if (strstr(op, "INDEX")) snprintf(n->index, sizeof n->index, "%s", a); else snprintf(n->object, sizeof n->object, "%s", a); }
        for (int i = 0; i < k; i++) {
            char v[256]; snprintf(v, sizeof v, "%s", cells[i]); char *a = v; while (*a == ' ') a++; char *z = a + strlen(a); while (z > a && z[-1] == ' ') *--z = 0;
            plan_prop(n, cols[i], a);
            if (!strcmp(cols[i], "E-Rows")) n->est_rows = num_s(a);
            else if (!strcmp(cols[i], "A-Rows")) n->act_rows = num_s(a);
            else if (!strcmp(cols[i], "Starts")) n->loops = num_s(a);
            else if (!strcmp(cols[i], "Cost (%CPU)")) n->est_cost = num_s(a);
            else if (!strcmp(cols[i], "A-Time")) { int hh = 0, mm = 0; double ss = 0; if (sscanf(a, "%d:%d:%lf", &hh, &mm, &ss) == 3) n->act_ms = num((hh * 3600 + mm * 60 + ss) * 1000.0); }
        }
        set_kind(n);
        if (sp < 256) { stack[sp] = id; lvl_of[sp] = indent; sp++; }
    }
    if (!p->n) {
        if (strstr(txt, "cannot fetch plan") || strstr(txt, "ORA-") || strstr(txt, "User has no SELECT privilege")) snprintf(err, (size_t)en, "DBMS_XPLAN could not read the cursor: %.200s (needs SELECT on V$SQL_PLAN_STATISTICS_ALL, V$SQL, V$SESSION)", txt);
        else snprintf(err, (size_t)en, "No plan table in the DBMS_XPLAN output");
        return false;
    }
    /* "   2 - access("O"."C"=1)" -> node pid 2 */
    const char *q = preds;
    while (*q) {
        const char *e = strchr(q, '\n'); int len = e ? (int)(e - q) : (int)strlen(q);
        char ln[1200]; snprintf(ln, sizeof ln, "%.*s", len < 1199 ? len : 1199, q);
        q += len + (e ? 1 : 0);
        int pid = 0; char rest[1100] = "";
        if (sscanf(ln, " %d - %1099[^\n]", &pid, rest) == 2) for (int i = 0; i < p->n; i++) if (atoi(p->nodes[i].pid) == pid) { size_t L = strlen(p->nodes[i].predicate); snprintf(p->nodes[i].predicate + L, sizeof p->nodes[i].predicate - L, "%s%s", L ? "; " : "", rest); }
    }
    snprintf(p->notes, sizeof p->notes, "Oracle DISPLAY_CURSOR (ALLSTATS LAST): A-Rows and A-Time are actual for the last execution; Starts = executions of the operation; E-Rows is per start.");
    return true;
}

/* ---- entry ---- */

static char *rs_text(ResultSet *rs) {
    if (!rs || !rs->nrows) return NULL;
    size_t cap = 1; for (int r = 0; r < rs->nrows; r++) { const char *v = rs_cell(rs, r, 0); cap += (v ? strlen(v) : 0) + 1; }
    char *t = malloc(cap); size_t w = 0; t[0] = 0;
    for (int r = 0; r < rs->nrows; r++) { const char *v = rs_cell(rs, r, 0); if (!v) continue; size_t L = strlen(v); memcpy(t + w, v, L); w += L; if (rs->nrows > 1) t[w++] = '\n'; }
    t[w] = 0;
    return t;
}
bool plan_parse(int d, int mode, ResultSet *const *rs, int n, int result, Plan *p, char *err, int en) {
    p->dialect = d; p->mode = mode;
    if (result >= n || !rs[result]) { snprintf(err, (size_t)en, "The plan statement returned nothing"); return false; }
    ResultSet *r = rs[result];
    bool ok = false;
    switch (d) {
    case PL_SQLITE: ok = parse_sqlite_rows(r, p, err, en); snprintf(p->raw_format, sizeof p->raw_format, "rows"); snprintf(p->adapter, sizeof p->adapter, "sqlite-eqp-1");
        { size_t cap = 64; for (int i = 0; i < r->nrows; i++) cap += strlen(rs_cell(r, i, 3) ? rs_cell(r, i, 3) : "") + 32; p->raw = malloc(cap); size_t w = 0; p->raw[0] = 0;
          for (int i = 0; i < r->nrows; i++) w += (size_t)snprintf(p->raw + w, cap - w, "%s|%s|%s\n", rs_cell(r, i, 0) ? rs_cell(r, i, 0) : "", rs_cell(r, i, 1) ? rs_cell(r, i, 1) : "", rs_cell(r, i, 3) ? rs_cell(r, i, 3) : ""); }
        break;
    case PL_PG: p->raw = rs_text(r); ok = p->raw && parse_pg(p->raw, p, err, en); snprintf(p->raw_format, sizeof p->raw_format, "json"); snprintf(p->adapter, sizeof p->adapter, "pg-json-1"); break;
    case PL_MSSQL: p->raw = rs_text(r); ok = p->raw && parse_sqlplan(p->raw, p, err, en); snprintf(p->raw_format, sizeof p->raw_format, "xml"); snprintf(p->adapter, sizeof p->adapter, "showplan-xml-1"); break;
    case PL_MYSQL: p->raw = rs_text(r); ok = p->raw && (mode == PM_ACTUAL ? parse_my_tree(p->raw, p, err, en) : parse_my_json(p->raw, p, false, err, en)); snprintf(p->raw_format, sizeof p->raw_format, mode == PM_ACTUAL ? "text" : "json"); break;
    case PL_MARIADB: p->raw = rs_text(r); ok = p->raw && parse_my_json(p->raw, p, true, err, en); snprintf(p->raw_format, sizeof p->raw_format, "json"); break;
    case PL_ORACLE:
        if (mode == PM_ESTIMATED) { ok = parse_ora_rows(r, p, err, en); snprintf(p->raw_format, sizeof p->raw_format, "rows"); snprintf(p->adapter, sizeof p->adapter, "oracle-plan-table-1"); }
        else { p->raw = rs_text(r); ok = p->raw && parse_ora_text(p->raw, p, err, en); snprintf(p->raw_format, sizeof p->raw_format, "text"); snprintf(p->adapter, sizeof p->adapter, "oracle-xplan-text-1"); }
        break;
    default: snprintf(err, (size_t)en, "No plan parser for this engine");
    }
    if (!ok && !err[0]) snprintf(err, (size_t)en, "The plan could not be read");
    return ok;
}
bool plan_import(const char *text, size_t len, const char *fmt, Plan *p, char *err, int en) {
    if (len > (32u << 20)) { snprintf(err, (size_t)en, "Plan file larger than 32 MB: refused"); return false; }
    char *t = malloc(len + 1); memcpy(t, text, len); t[len] = 0;
    for (size_t i = 0; i < len; i++) if (!t[i]) t[i] = ' ';
    const char *s = skip_ws_comments(t);
    char f[24]; snprintf(f, sizeof f, "%s", fmt ? fmt : "auto");
    if (!strcmp(f, "auto")) {
        if (strstr(s, "ShowPlanXML")) snprintf(f, sizeof f, "sqlplan");
        else if ((s[0] == '[' || s[0] == '{') && strstr(s, "\"Node Type\"")) snprintf(f, sizeof f, "pg-json");
        else if (s[0] == '{' && strstr(s, "\"r_rows\"")) snprintf(f, sizeof f, "mariadb-json");
        else if (s[0] == '{' && (strstr(s, "\"query_block\"") || strstr(s, "\"query_plan\""))) snprintf(f, sizeof f, "mysql-json");
        else if (strstr(s, "-> ")) snprintf(f, sizeof f, "mysql-tree");
        else if (strstr(s, "| Id") && strstr(s, "Operation")) snprintf(f, sizeof f, "oracle-text");
        else if (strstr(s, "SCAN ") || strstr(s, "SEARCH ")) snprintf(f, sizeof f, "sqlite-eqp");
        else { snprintf(err, (size_t)en, "The format could not be told apart: choose the parser explicitly"); free(t); return false; }
    }
    p->mode = PM_ESTIMATED;
    bool ok = false;
    if (!strcmp(f, "pg-json")) { p->dialect = PL_PG; p->mode = strstr(s, "\"Actual Rows\"") ? PM_ACTUAL : PM_ESTIMATED; ok = parse_pg(s, p, err, en); }
    else if (!strcmp(f, "sqlplan")) { p->dialect = PL_MSSQL; p->mode = strstr(s, "RunTimeCountersPerThread") ? PM_ACTUAL : PM_ESTIMATED; ok = parse_sqlplan(s, p, err, en); }
    else if (!strcmp(f, "mysql-json")) { p->dialect = PL_MYSQL; ok = parse_my_json(s, p, false, err, en); }
    else if (!strcmp(f, "mariadb-json")) { p->dialect = PL_MARIADB; p->mode = PM_ACTUAL; ok = parse_my_json(s, p, true, err, en); }
    else if (!strcmp(f, "mysql-tree")) { p->dialect = PL_MYSQL; p->mode = strstr(s, "actual time") ? PM_ACTUAL : PM_ESTIMATED; ok = parse_my_tree(s, p, err, en); }
    else if (!strcmp(f, "oracle-text")) { p->dialect = PL_ORACLE; p->mode = strstr(s, "A-Rows") ? PM_ACTUAL : PM_ESTIMATED; ok = parse_ora_text(s, p, err, en); }
    else if (!strcmp(f, "sqlite-eqp")) {   /* lines "id|parent|detail" or "|--SCAN t" from the sqlite3 shell */
        p->dialect = PL_SQLITE;
        ResultSet rs; rs_init(&rs); rs_add_column(&rs, "id", "", 0); rs_add_column(&rs, "parent", "", 0); rs_add_column(&rs, "notused", "", 0); rs_add_column(&rs, "detail", "", 0);
        const char *q = s; int seq = 1, lastid[64] = { 0 };
        while (*q) {
            const char *e = strchr(q, '\n'); int L = e ? (int)(e - q) : (int)strlen(q);
            char ln[1024]; snprintf(ln, sizeof ln, "%.*s", L < 1023 ? L : 1023, q); q += L + (e ? 1 : 0);
            int a, b; char det[1000];
            if (sscanf(ln, "%d|%d|%999[^\n]", &a, &b, det) == 3) { int r = rs_add_row(&rs); char x[16], y[16]; snprintf(x, sizeof x, "%d", a); snprintf(y, sizeof y, "%d", b); rs_set(&rs, r, 0, x, -1); rs_set(&rs, r, 1, y, -1); rs_set(&rs, r, 2, "0", -1); rs_set(&rs, r, 3, det, -1); continue; }
            const char *m = strstr(ln, "--"); if (!m) continue;
            int lvl = (int)(m - ln) / 3; if (lvl > 62) lvl = 62;
            int r = rs_add_row(&rs); char x[16], y[16]; snprintf(x, sizeof x, "%d", seq); snprintf(y, sizeof y, "%d", lvl ? lastid[lvl - 1] : 0);
            rs_set(&rs, r, 0, x, -1); rs_set(&rs, r, 1, y, -1); rs_set(&rs, r, 2, "0", -1); rs_set(&rs, r, 3, m + 2, -1); lastid[lvl] = seq++;
        }
        rs.ok = rs.has_rows = true;
        ok = rs.nrows > 0 && parse_sqlite_rows(&rs, p, err, en);
        if (!rs.nrows) snprintf(err, (size_t)en, "No EXPLAIN QUERY PLAN lines");
        rs_free(&rs);
    } else snprintf(err, (size_t)en, "Unknown plan format %s", f);
    if (ok) { p->raw = t; snprintf(p->raw_format, sizeof p->raw_format, "%s", f); } else free(t);
    if (ok) { size_t L = strlen(p->notes); snprintf(p->notes + L, sizeof p->notes - L, "%sImported snapshot: no connection, nothing was run.", L ? " " : ""); }
    return ok;
}

/* ---- layout: SQL Server style, the root on the left, data flowing from the right (EPL-024) ---- */

static float lay_leaf;
static void lay(Plan *p, int id, int depth, bool rtl) {
    PlanNode *n = &p->nodes[id];
    n->w = 190; n->h = 70;
    if (n->first_child < 0 || n->collapsed) {
        if (rtl) { n->x = depth * 240.0f; n->y = lay_leaf * 96.0f; } else { n->x = lay_leaf * 220.0f; n->y = depth * 110.0f; }
        lay_leaf += 1; return;
    }
    float first = -1, last = 0;
    for (int c = n->first_child; c >= 0; c = p->nodes[c].next_sibling) { lay(p, c, depth + 1, rtl); float v = rtl ? p->nodes[c].y : p->nodes[c].x; if (first < 0) first = v; last = v; }
    if (rtl) { n->x = depth * 240.0f; n->y = (first + last) / 2; } else { n->y = depth * 110.0f; n->x = (first + last) / 2; }
}
/* SQL Server Management Studio layout: the root on the left, each operator on the row of its first
 * input (not centred), siblings stacked downwards, every operator an icon with its captions below */
static float ss_row;
static void lay_ss(Plan *p, int id, int depth) {
    PlanNode *n = &p->nodes[id];
    n->w = 132; n->h = 112;
    n->x = depth * 182.0f;
    if (n->first_child < 0 || n->collapsed) { n->y = ss_row * 128.0f; ss_row += 1; return; }
    bool first = true;
    for (int c = n->first_child; c >= 0; c = p->nodes[c].next_sibling) { lay_ss(p, c, depth + 1); if (first) { n->y = p->nodes[c].y; first = false; } }
}
void plan_layout_rows(Plan *p) {
    ss_row = 0;
    for (int i = 0; i < p->n; i++) if (p->nodes[i].parent < 0) lay_ss(p, i, 0);
}
double plan_self_cost(const Plan *p, int i, bool *ok) {
    const PlanNode *n = &p->nodes[i];
    *ok = n->est_cost.st == PV_PRESENT;
    if (!*ok) return 0;
    double c = n->est_cost.v;
    for (int k = n->first_child; k >= 0; k = p->nodes[k].next_sibling) if (p->nodes[k].est_cost.st == PV_PRESENT) c -= p->nodes[k].est_cost.v;
    return c < 0 ? 0 : c;
}
void plan_layout(Plan *p, bool rtl) {
    lay_leaf = 0;
    for (int i = 0; i < p->n; i++) if (p->nodes[i].parent < 0) lay(p, i, 0, rtl);
}

/* ---- diagnostics: evidence first (EPL-035..041 / 046 / 056) ---- */

static void finding(PlanFinding *out, int max, int *n, int kind, int node, int conf, const char *title, const char *ev, const char *missing, const char *sug) {
    if (*n >= max) return;
    PlanFinding *f = &out[(*n)++];
    f->kind = kind; f->node = node; f->confidence = conf;
    snprintf(f->title, sizeof f->title, "%s", title); snprintf(f->evidence, sizeof f->evidence, "%s", ev);
    snprintf(f->missing, sizeof f->missing, "%s", missing ? missing : ""); snprintf(f->suggestion, sizeof f->suggestion, "%s", sug ? sug : "");
}
static double rows_total(const PlanNode *n, bool actual) {
    PlanNum r = actual ? n->act_rows : n->est_rows;
    if (r.st != PV_PRESENT) return -1;
    double loops = n->loops.st == PV_PRESENT && n->loops.v > 0 ? n->loops.v : 1;
    return r.v * (actual ? loops : 1);
}
int plan_diagnose(Plan *p, const PlanDiagOpts *o, PlanFinding *out, int max) {
    double ratio = o && o->card_ratio > 1 ? o->card_ratio : 10, minr = o && o->min_rows > 0 ? o->min_rows : 1000;
    int n = 0;
    char ev[600], t[200], sug[600];
    for (int i = 0; i < p->n; i++) {
        PlanNode *x = &p->nodes[i];
        /* cardinality: both measures, per loop where the engine reports per loop (PostgreSQL, MySQL, Oracle E-Rows per start) */
        if (x->est_rows.st == PV_PRESENT && x->act_rows.st == PV_PRESENT) {
            double e = x->est_rows.v, a = x->act_rows.v;
            if (p->dialect == PL_MSSQL && x->loops.st == PV_PRESENT && x->loops.v > 0) a = a / x->loops.v;   /* SQL Server: ActualRows over all executions, EstimateRows per execution */
            if (p->dialect == PL_ORACLE && x->loops.st == PV_PRESENT && x->loops.v > 0) a = a / x->loops.v;
            double hi = fmax(e, a), lo = fmax(fmin(e, a), 1);
            if (hi / lo >= ratio && hi >= 100) {
                x->flags |= PF_CARD;
                snprintf(t, sizeof t, "Row estimate off by %.0fx at node #%s (%s)", hi / lo, x->pid, x->op);
                snprintf(ev, sizeof ev, "estimated %.0f rows, actual %.0f%s%s; %s", e, a, x->loops.st == PV_PRESENT ? " per loop over " : "", x->loops.st == PV_PRESENT ? "" : "", x->object[0] ? x->object : "no table");
                if (x->loops.st == PV_PRESENT) { char lb[64]; snprintf(lb, sizeof lb, "%.0f loops", x->loops.v); size_t L = strlen(ev); snprintf(ev + L, sizeof ev - L, " (%s)", lb); }
                snprintf(sug, sizeof sug, "Statistics may be stale or the predicate correlated: refresh statistics after review (%s) - never run automatically", p->dialect == PL_PG ? "ANALYZE table" : p->dialect == PL_MSSQL ? "UPDATE STATISTICS table" : p->dialect == PL_ORACLE ? "DBMS_STATS.GATHER_TABLE_STATS" : p->dialect == PL_SQLITE ? "ANALYZE" : "ANALYZE TABLE");
                finding(out, max, &n, FD_CARDINALITY, i, 2, t, ev, "when the statistics were last gathered; whether columns of the predicate are correlated", sug);
            }
        }
        /* scans with a filter on a large input (EPL-037 / 056): a scan alone is not an error */
        if (!strcmp(x->kind, "scan") && x->object[0]) {
            double rows = rows_total(x, p->mode == PM_ACTUAL);
            if (rows < 0) rows = rows_total(x, false);
            double removed = x->removed.st == PV_PRESENT ? x->removed.v * (x->loops.st == PV_PRESENT ? x->loops.v : 1) : -1;
            bool filtered = x->predicate[0] != 0;
            if (p->dialect == PL_SQLITE) {
                {
                    x->flags |= PF_SCAN;
                    snprintf(t, sizeof t, "Full scan of %s (node #%s)", x->object, x->pid);
                    snprintf(ev, sizeof ev, "SQLite reports \"%s\"%s; it gives no row counts, so the size of the scan is unknown", x->op, x->index[0] ? "" : " without an index");
                    finding(out, max, &n, FD_SCAN, i, 0, t, ev, "the table's size and how selective the WHERE / JOIN columns are (a SCAN alone does not show that an index is needed)", "Check the WHERE / JOIN columns of this table; an index is a hypothesis to verify, not a conclusion");
                }
            } else if (filtered && (rows >= minr || removed >= minr)) {
                x->flags |= PF_SCAN | PF_HOT;
                snprintf(t, sizeof t, "Scan with a filter on %s (node #%s)", x->object, x->pid);
                if (removed >= 0) snprintf(ev, sizeof ev, "%s reads %s and discards %.0f row(s) with \"%.200s\"", x->op, x->object, removed, x->predicate);
                else snprintf(ev, sizeof ev, "%s on %s, %.0f row(s) %s, filter \"%.200s\"", x->op, x->object, rows, p->mode == PM_ACTUAL ? "actual" : "estimated", x->predicate);
                finding(out, max, &n, FD_INDEX, i, p->mode == PM_ACTUAL ? 1 : 0, t, ev, "existing indexes on the filtered columns; how often the table is written; whether the query is frequent", "Candidate: an index on the filtered column(s) (review the DDL; existing indexes and write cost first)");
            }
        }
        /* spills and sorts (EPL-038): only what the engine reported */
        if (x->warning[0]) {
            bool spill = strcasestr(x->warning, "spill") || strcasestr(x->warning, "disk") || strcasestr(x->warning, "batches");
            x->flags |= spill ? PF_SPILL : PF_WARN;
            snprintf(t, sizeof t, "%s at node #%s", spill ? "Spill to disk" : "Engine warning", x->pid);
            finding(out, max, &n, spill ? FD_SPILL : FD_WARNING, i, 2, t, x->warning, NULL, spill ? "More memory for the operation (work_mem / memory grant) or fewer rows into it; the engine reported it, it is not inferred" : "Read the warning in Properties");
        }
        if (!strcmp(x->kind, "temp") && p->dialect == PL_SQLITE && strstr(x->op, "TEMP B-TREE")) {
            x->flags |= PF_SORT;
            snprintf(t, sizeof t, "Temporary B-tree for %s (node #%s)", strstr(x->op, "ORDER BY") ? "ORDER BY" : strstr(x->op, "GROUP BY") ? "GROUP BY" : "DISTINCT", x->pid);
            finding(out, max, &n, FD_SORT, i, 1, t, x->op, "how many rows reach it", "An index matching the ORDER BY / GROUP BY could avoid it (hypothesis)");
        }
        if (!strcmp(x->kind, "join") && strstr(x->op, "Nested") && p->mode == PM_ACTUAL) {
            for (int c = x->first_child; c >= 0; c = p->nodes[c].next_sibling) {
                PlanNode *ch = &p->nodes[c];
                if (ch->loops.st == PV_PRESENT && ch->loops.v >= 1000) {
                    snprintf(t, sizeof t, "Inner side of a nested loop ran %.0f times (node #%s)", ch->loops.v, ch->pid);
                    snprintf(ev, sizeof ev, "%s under %s: %.0f loops", ch->op, x->op, ch->loops.v);
                    finding(out, max, &n, FD_NESTED, c, 1, t, ev, "whether the outer row estimate is right (see cardinality findings)", "A hash / merge join may suit large outer inputs; often a consequence of an underestimated outer side");
                }
            }
        }
    }
    const char *mi = p->n ? plan_prop_get(&p->nodes[0], "MissingIndexes (SQL Server hint)") : NULL;
    if (mi) finding(out, max, &n, FD_INDEX, 0, 1, "SQL Server suggested a missing index (native hint)", mi, "its impact estimate is the optimizer's; existing indexes and writes are not considered by the hint", "Review it as a native hint, not a verified improvement");
    return n;
}
void plan_summary(const Plan *p, char *o, int n) {
    char c[64], pm[64], em[64];
    plan_num_fmt(p->total_cost, c, sizeof c, 2); plan_num_fmt(p->planning_ms, pm, sizeof pm, 3); plan_num_fmt(p->exec_ms, em, sizeof em, 3);
    int w = snprintf(o, (size_t)n, "%s \xC2\xB7 %s \xC2\xB7 %d operator(s)", plan_dialect_name(p->dialect), p->mode == PM_ACTUAL ? "ACTUAL (the statement ran)" : "ESTIMATED (not run)", p->n);
    if (p->provider_version[0]) w += snprintf(o + w, (size_t)(n - w), " \xC2\xB7 server %s", p->provider_version);
    if (p->total_cost.st == PV_PRESENT) w += snprintf(o + w, (size_t)(n - w), " \xC2\xB7 total cost %s (%s optimizer units, not time)", c, plan_dialect_name(p->dialect));
    if (p->mode == PM_ACTUAL && p->exec_ms.st == PV_PRESENT) w += snprintf(o + w, (size_t)(n - w), " \xC2\xB7 execution %s ms", em);
    if (p->planning_ms.st == PV_PRESENT) snprintf(o + w, (size_t)(n - w), " \xC2\xB7 planning %s ms", pm);
}

/* column names compared with '=' / IN / ranges in a predicate: identifiers before an operator */
static int pred_cols(const char *pred, const char *table, char cols[][96], int max) {
    int n = 0;
    const char *s = pred;
    while (*s && n < max) {
        while (*s && !(isalpha((unsigned char)*s) || *s == '_' || *s == '"' || *s == '[' || *s == '`')) { if (*s == '\'') { s++; while (*s && *s != '\'') s++; } if (*s) s++; }
        if (!*s) break;
        char id[200]; int k = 0;
        while (*s && (isalnum((unsigned char)*s) || *s == '_' || *s == '.' || *s == '"' || *s == '[' || *s == ']' || *s == '`') && k < 199) id[k++] = *s++;
        id[k] = 0;
        while (*s == ' ' || *s == ')') s++;
        bool cmp = *s == '=' || *s == '<' || *s == '>' || !strncasecmp(s, "IN ", 3) || !strncasecmp(s, "IN(", 3) || !strncasecmp(s, "BETWEEN", 7) || !strncasecmp(s, "IS ", 3) || !strncasecmp(s, "LIKE", 4) || !strncmp(s, "::", 2);
        if (!strncmp(s, "::", 2)) { s += 2; while (isalnum((unsigned char)*s) || *s == '_' || *s == ' ') s++; cmp = *s == '=' || *s == '<' || *s == '>'; }
        char *last = strrchr(id, '.'); const char *nm = last ? last + 1 : id;
        char clean[96]; int c = 0; for (const char *q = nm; *q && c < 95; q++) if (*q != '"' && *q != '[' && *q != ']' && *q != '`') clean[c++] = *q; clean[c] = 0;
        static const char *kw[] = { "AND", "OR", "NOT", "NULL", "access", "filter", "Filter", "Index", "Cond", "Recheck", "Hash", "Join", "text", "integer", "numeric", "bigint", "character", "varying", "SEARCH", "USING", "INDEX", NULL };
        bool isk = false; for (int i = 0; kw[i]; i++) isk |= !strcasecmp(clean, kw[i]);
        if (cmp && clean[0] && !isk && !isdigit((unsigned char)clean[0])) {
            bool dup = false; for (int i = 0; i < n; i++) dup |= !strcasecmp(cols[i], clean);
            bool eq = *s == '=' || !strncasecmp(s, "IN", 2) || !strncasecmp(s, "IS ", 3);
            if (!dup && eq) { memmove(cols[1], cols[0], sizeof cols[0] * (size_t)n); snprintf(cols[0], 96, "%s", clean); n++; }   /* equality first, ranges after */
            else if (!dup) snprintf(cols[n++], 96, "%s", clean);
        }
        (void)table;
    }
    return n;
}
bool plan_index_ddl(const Plan *p, const PlanFinding *f, char *o, int n, char *why, int wn) {
    o[0] = 0;
    if (f->kind != FD_INDEX || f->node < 0 || f->node >= p->n) { snprintf(why, (size_t)wn, "No index candidate for this finding"); return false; }
    const PlanNode *x = &p->nodes[f->node];
    if (!x->object[0]) { snprintf(why, (size_t)wn, "The operator has no table"); return false; }
    char cols[8][96]; int nc = pred_cols(x->predicate, x->object, cols, 8);
    if (!nc) { snprintf(why, (size_t)wn, "The predicate has no plain column comparison (expressions / functions need a human decision)"); return false; }
    char tbl[260]; snprintf(tbl, sizeof tbl, "%s", x->object);
    char base[100]; const char *dot = strrchr(tbl, '.'); snprintf(base, sizeof base, "%s", dot ? dot + 1 : tbl);
    for (char *c = base; *c; c++) if (*c == '"' || *c == '[' || *c == ']') *c = '_';
    char nm[200]; snprintf(nm, sizeof nm, "ix_%s_%s", base, cols[0]);
    char q1 = p->dialect == PL_MYSQL || p->dialect == PL_MARIADB ? '`' : '"', q2 = q1; if (p->dialect == PL_MSSQL) { q1 = '['; q2 = ']'; }
    int w = snprintf(o, (size_t)n, "CREATE INDEX %c%s%c ON ", q1, nm, q2);
    /* table: each part quoted */
    char tb[300]; snprintf(tb, sizeof tb, "%s", tbl);
    char *part = strtok(tb, ".");
    bool first = true;
    while (part) { char clean[160]; int c = 0; for (char *q = part; *q && c < 159; q++) if (*q != '"' && *q != '[' && *q != ']' && *q != '`') clean[c++] = *q; clean[c] = 0; w += snprintf(o + w, (size_t)(n - w), "%s%c%s%c", first ? "" : ".", q1, clean, q2); first = false; part = strtok(NULL, "."); }
    w += snprintf(o + w, (size_t)(n - w), " (");
    for (int i = 0; i < nc; i++) w += snprintf(o + w, (size_t)(n - w), "%s%c%s%c", i ? ", " : "", q1, cols[i], q2);
    snprintf(o + w, (size_t)(n - w), ")");
    snprintf(why, (size_t)wn, "From the predicate of node #%s (%s): an inference, not a verified improvement. %s", x->pid, x->op,
             p->dialect == PL_PG ? "On a busy table consider CREATE INDEX CONCURRENTLY (cannot run inside a transaction)." : p->dialect == PL_MSSQL ? "ONLINE = ON needs an edition that supports it." : p->dialect == PL_SQLITE ? "SQLite: check ANALYZE output and the query's selectivity first." : "");
    return true;
}

/* ---- comparison ---- */

void plan_compare(const Plan *a, const Plan *b, PlanDiff *d) {
    memset(d, 0, sizeof *d);
    d->same_engine = a->dialect == b->dialect;
    int w = 0;
    int n = a->n > b->n ? a->n : b->n;
    for (int i = 0; i < n; i++) {
        const PlanNode *x = i < a->n ? &a->nodes[i] : NULL, *y = i < b->n ? &b->nodes[i] : NULL;
        if (x && !y) { d->only_a++; continue; }
        if (y && !x) { d->only_b++; continue; }
        if (strcmp(x->op, y->op) || strcmp(x->object, y->object) || strcmp(x->index, y->index)) {
            d->changed_ops++;
            if (w < (int)sizeof d->text - 300) w += snprintf(d->text + w, sizeof d->text - (size_t)w, "position %d: %s %s -> %s %s\n", i + 1, x->op, x->object, y->op, y->object);
        }
        if (x->est_rows.st == PV_PRESENT && y->est_rows.st == PV_PRESENT && fabs(x->est_rows.v - y->est_rows.v) > fmax(1, 0.5 * fmax(x->est_rows.v, y->est_rows.v))) d->card_changes++;
    }
    if (!d->same_engine) w += snprintf(d->text + w, sizeof d->text - (size_t)w, "Different engines: costs are not comparable\n");
    if (a->n != b->n) w += snprintf(d->text + w, sizeof d->text - (size_t)w, "Operators: %d -> %d (matching by position: not a proof that operators correspond)\n", a->n, b->n);
}

/* ---- context for the LLM: sanitized by default ---- */

void plan_mask_literals(const char *in, char *out, int n) {
    int o = 0;
    for (const char *s = in; *s && o < n - 2; s++) {
        if (*s == '\'') { out[o++] = '?'; s++; while (*s && !(*s == '\'' && s[1] != '\'')) { if (*s == '\'' ) s++; s++; } if (!*s) break; continue; }
        if (isdigit((unsigned char)*s) && (s == in || !(isalnum((unsigned char)s[-1]) || s[-1] == '_'))) { out[o++] = '?'; while (isdigit((unsigned char)s[1]) || s[1] == '.') s++; continue; }
        out[o++] = *s;
    }
    out[o] = 0;
}
typedef struct NameMap { char real[16][160]; int n; } NameMap;
static const char *alias(NameMap *m, const char *real, const char *prefix, char *buf, int n) {
    if (!real[0]) return "";
    for (int i = 0; i < m->n; i++) if (!strcmp(m->real[i], real)) { snprintf(buf, (size_t)n, "%s_%d", prefix, i + 1); return buf; }
    if (m->n < 16) snprintf(m->real[m->n++], 160, "%s", real);
    snprintf(buf, (size_t)n, "%s_%d", prefix, m->n);
    return buf;
}
char *plan_context(const Plan *p, const PlanShare *s) {
    size_t cap = 4096 + (size_t)p->n * 1600 + (p->sql ? strlen(p->sql) * 2 : 0);
    char *o = malloc(cap); size_t w = 0;
    NameMap tm = { 0 }, im = { 0 };
    w += (size_t)snprintf(o + w, cap - w, "engine: %s\nmode: %s\noperators: %d\n", plan_dialect_name(p->dialect), p->mode == PM_ACTUAL ? "actual (executed)" : "estimated (not executed)", p->n);
    if (p->notes[0]) w += (size_t)snprintf(o + w, cap - w, "engine notes: %s\n", p->notes);
    if (s->sql && p->sql) {
        char *m = malloc(strlen(p->sql) + 1);
        if (s->literals) snprintf(m, strlen(p->sql) + 1, "%s", p->sql); else plan_mask_literals(p->sql, m, (int)strlen(p->sql) + 1);
        w += (size_t)snprintf(o + w, cap - w, "sql:\n%s\n", m);
        free(m);
    }
    w += (size_t)snprintf(o + w, cap - w, "nodes (id | parent | operator | table | index | est_rows | act_rows | loops | cost | time_ms | predicate | warning):\n");
    for (int i = 0; i < p->n; i++) {
        const PlanNode *x = &p->nodes[i];
        if (s->subtree >= 0) { int k = i; bool in = false; while (k >= 0) { if (k == s->subtree) { in = true; break; } k = p->nodes[k].parent; } int a = s->subtree; bool anc = false; while (a >= 0) { if (a == i) anc = true; a = p->nodes[a].parent; } if (!in && !anc) continue; }
        char er[32], ar[32], lp[32], cs[32], ms[32], tb[64], ix[64];
        plan_num_fmt(x->est_rows, er, sizeof er, 1); plan_num_fmt(x->act_rows, ar, sizeof ar, 1); plan_num_fmt(x->loops, lp, sizeof lp, 0); plan_num_fmt(x->est_cost, cs, sizeof cs, 2); plan_num_fmt(x->act_ms, ms, sizeof ms, 3);
        char pred[1100]; if (s->literals) snprintf(pred, sizeof pred, "%s", x->predicate); else plan_mask_literals(x->predicate, pred, sizeof pred);
        const char *t = s->names ? x->object : alias(&tm, x->object, "table", tb, sizeof tb), *ix2 = s->names ? x->index : alias(&im, x->index, "index", ix, sizeof ix);
        if (!s->names) for (int k = 0; k < tm.n; k++) { char *f; char al[32]; snprintf(al, sizeof al, "table_%d", k + 1); while ((f = strstr(pred, tm.real[k])) && tm.real[k][0]) { size_t L = strlen(tm.real[k]); memmove(f + strlen(al), f + L, strlen(f + L) + 1); memcpy(f, al, strlen(al)); } }
        w += (size_t)snprintf(o + w, cap - w, "#%s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s | %s\n", x->pid, x->parent >= 0 ? p->nodes[x->parent].pid : "-", x->op, t, ix2, er, ar, lp, cs, ms, pred, x->warning);
        if (w > cap - 2000) break;
    }
    if (s->raw && p->raw && w < cap - 100) { size_t room = cap - w - 64; w += (size_t)snprintf(o + w, cap - w, "raw plan:\n%.*s\n", (int)(strlen(p->raw) < room ? strlen(p->raw) : room), p->raw); }
    return o;
}

char *plan_report_md(const Plan *p, const PlanFinding *f, int nf) {
    size_t cap = 8192 + (size_t)p->n * 600 + (size_t)nf * 1600;
    char *o = malloc(cap); size_t w = 0;
    char sum[600]; plan_summary(p, sum, sizeof sum);
    w += (size_t)snprintf(o + w, cap - w, "# Execution plan report\n\n%s\n\n", sum);
    if (p->notes[0]) w += (size_t)snprintf(o + w, cap - w, "> %s\n\n", p->notes);
    w += (size_t)snprintf(o + w, cap - w, "## Findings (evidence, not guarantees)\n\n");
    if (!nf) w += (size_t)snprintf(o + w, cap - w, "No finding: the rules found nothing to point at (which is not a proof that the query is optimal).\n\n");
    static const char *cf[] = { "low", "medium", "high" };
    for (int i = 0; i < nf; i++) w += (size_t)snprintf(o + w, cap - w, "- **%s** (confidence %s)\n  - Evidence: %s\n  - Missing to confirm: %s\n  - Suggestion: %s\n", f[i].title, cf[f[i].confidence], f[i].evidence, f[i].missing[0] ? f[i].missing : "-", f[i].suggestion);
    w += (size_t)snprintf(o + w, cap - w, "\n## Operators\n\n| # | Operator | Table | Index | Est. rows | Act. rows | Loops | Cost |\n|---|---|---|---|---|---|---|---|\n");
    for (int i = 0; i < p->n && w < cap - 700; i++) {
        const PlanNode *x = &p->nodes[i]; char er[32], ar[32], lp[32], cs[32];
        plan_num_fmt(x->est_rows, er, sizeof er, 1); plan_num_fmt(x->act_rows, ar, sizeof ar, 1); plan_num_fmt(x->loops, lp, sizeof lp, 0); plan_num_fmt(x->est_cost, cs, sizeof cs, 2);
        w += (size_t)snprintf(o + w, cap - w, "| %s | %*s%s | %s | %s | %s | %s | %s | %s |\n", x->pid, x->depth * 2, "", x->op, x->object, x->index, er, ar, lp, cs);
    }
    w += (size_t)snprintf(o + w, cap - w, "\nCosts are %s optimizer units (not milliseconds, not comparable across engines). SQL text and connection details are not included.\n", plan_dialect_name(p->dialect));
    return o;
}
