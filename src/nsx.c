/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Namespace explorer model: see nsx.h.
 *
 * Layout: SQL builders and adapters (one block per engine), then the registry and preferences,
 * then the model (featured list, Others, count), then requests and their results.
 */
#include "nsx.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- SQL text builder ---- */

typedef struct Sb { char *p; int n, len; } Sb;
static void sb_add(Sb *b, const char *fmt, ...) {
    if (b->len >= b->n - 1) return;
    va_list ap; va_start(ap, fmt);
    int w = vsnprintf(b->p + b->len, (size_t)(b->n - b->len), fmt, ap);
    va_end(ap);
    if (w > 0) b->len += w;
    if (b->len > b->n - 1) b->len = b->n - 1;
}
enum { LIT_STD, LIT_MY, LIT_N };
/* a string literal: quotes doubled; MySQL also doubles backslashes; SQL Server gets N'' */
static void sb_lit(Sb *b, const char *s, int style) {
    sb_add(b, style == LIT_N ? "N'" : "'");
    for (; *s && b->len < b->n - 4; s++) {
        char c = *s;
        if (c == '\'') b->p[b->len++] = '\'';
        if (c == '\\' && style == LIT_MY) b->p[b->len++] = '\\';
        b->p[b->len++] = c;
    }
    b->p[b->len] = 0;
    sb_add(b, "'");
}
/* a LIKE pattern literal: % _ \ escaped with a backslash, then prefix% or %substring% */
static void sb_like(Sb *b, const char *s, bool substring, bool upper, int style) {
    char pat[300]; int o = 0;
    if (substring) pat[o++] = '%';
    for (; *s && o < (int)sizeof pat - 4; s++) {
        if (*s == '%' || *s == '_' || *s == '\\') pat[o++] = '\\';
        pat[o++] = upper ? (char)toupper((unsigned char)*s) : *s;
    }
    pat[o++] = '%'; pat[o] = 0;
    sb_lit(b, pat, style);
}

/* ---- Oracle (DBX-091..098) ---- */

static void ora_scope(char *o, int n) {
    snprintf(o, (size_t)n, "SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA'), USER, SYS_CONTEXT('USERENV', 'CON_NAME') FROM dual");
}
static void ora_page(const NsPageReq *r, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT * FROM (SELECT username, %s FROM all_users WHERE 1 = 1",
           r->no_sysflag ? "0 AS sys" : "CASE WHEN oracle_maintained = 'Y' THEN 1 ELSE 0 END AS sys");
    if (r->after && r->after[0]) { sb_add(&b, " AND username > "); sb_lit(&b, r->after, LIT_STD); }
    if (r->pattern && r->pattern[0]) {
        /* prefix: direct on USERNAME (index friendly); a substring is an explicit, costlier request */
        if (r->substring) { sb_add(&b, " AND UPPER(username) LIKE "); sb_like(&b, r->pattern, true, true, LIT_STD); }
        else { sb_add(&b, " AND username LIKE "); sb_like(&b, r->pattern, false, true, LIT_STD); }
        sb_add(&b, " ESCAPE '\\'");
    }
    if (!r->include_system && !r->no_sysflag) sb_add(&b, " AND oracle_maintained = 'N'");
    sb_add(&b, " ORDER BY username) WHERE ROWNUM <= %d", r->limit);
}
static void ora_names_in(const char *const *names, int k, bool nosys, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT username, %s FROM all_users WHERE username IN (", nosys ? "0" : "CASE WHEN oracle_maintained = 'Y' THEN 1 ELSE 0 END");
    for (int i = 0; i < k; i++) { if (i) sb_add(&b, ", "); sb_lit(&b, names[i], LIT_STD); }
    sb_add(&b, ")");
}
static void ora_probe(const char *ns, int kinds, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT 1 FROM all_objects WHERE owner = "); sb_lit(&b, ns, LIT_STD);
    sb_add(&b, " AND subobject_name IS NULL");
    if (kinds != NSK_ALL) {
        sb_add(&b, " AND object_type IN ('#'");
        if (kinds & NSK_TABLES) sb_add(&b, ", 'TABLE'");
        if (kinds & NSK_VIEWS) sb_add(&b, ", 'VIEW', 'MATERIALIZED VIEW'");
        if (kinds & NSK_ROUTINES) sb_add(&b, ", 'PROCEDURE', 'FUNCTION', 'PACKAGE', 'TYPE'");
        if (kinds & NSK_OTHER) sb_add(&b, ", 'SEQUENCE', 'SYNONYM', 'TRIGGER', 'INDEX'");
        sb_add(&b, ")");
    }
    sb_add(&b, " AND ROWNUM = 1");
}
static void ora_tables(const char *ns, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT * FROM (SELECT owner || '.' || table_name, 0 FROM all_tables WHERE owner = "); sb_lit(&b, ns, LIT_STD);
    sb_add(&b, " AND nested = 'NO' AND secondary = 'N' UNION ALL SELECT owner || '.' || view_name, 1 FROM all_views WHERE owner = "); sb_lit(&b, ns, LIT_STD);
    sb_add(&b, " ORDER BY 2, 1) WHERE ROWNUM <= %d", limit);
}
static void ora_objsearch(const char *ns, const char *pat, bool sub, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT * FROM (SELECT owner, object_type, object_name FROM all_objects WHERE subobject_name IS NULL"
               " AND object_type IN ('TABLE', 'VIEW', 'MATERIALIZED VIEW', 'PROCEDURE', 'FUNCTION', 'PACKAGE', 'TRIGGER', 'TYPE', 'SYNONYM', 'SEQUENCE')");
    if (ns && ns[0]) { sb_add(&b, " AND owner = "); sb_lit(&b, ns, LIT_STD); }
    sb_add(&b, sub ? " AND UPPER(object_name) LIKE " : " AND object_name LIKE "); sb_like(&b, pat, sub, true, LIT_STD); sb_add(&b, " ESCAPE '\\'");
    sb_add(&b, " ORDER BY object_name, owner, object_type) WHERE ROWNUM <= %d", limit);
}
static void ora_discover(int limit, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT * FROM (SELECT owner FROM all_objects WHERE subobject_name IS NULL GROUP BY owner ORDER BY owner) WHERE ROWNUM <= %d", limit);
}

/* ---- PostgreSQL (DBX-099) ---- */

#define PG_SYS "(nspname IN ('pg_catalog', 'information_schema') OR nspname LIKE 'pg\\_%')"
static void pg_scope(char *o, int n) { snprintf(o, (size_t)n, "SELECT current_schema(), current_user, current_database()"); }
static void pg_page(const NsPageReq *r, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT nspname, CASE WHEN %s THEN 1 ELSE 0 END FROM pg_namespace WHERE has_schema_privilege(oid, 'USAGE')", PG_SYS);
    if (r->after && r->after[0]) { sb_add(&b, " AND nspname > "); sb_lit(&b, r->after, LIT_STD); }
    if (r->pattern && r->pattern[0]) { sb_add(&b, " AND nspname ILIKE "); sb_like(&b, r->pattern, r->substring, false, LIT_STD); }
    if (!r->include_system) sb_add(&b, " AND NOT %s", PG_SYS);
    sb_add(&b, " ORDER BY nspname LIMIT %d", r->limit);
}
static void pg_names_in(const char *const *names, int k, bool nosys, char *o, int n) {
    (void)nosys;
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT nspname, CASE WHEN %s THEN 1 ELSE 0 END FROM pg_namespace WHERE has_schema_privilege(oid, 'USAGE') AND nspname IN (", PG_SYS);
    for (int i = 0; i < k; i++) { if (i) sb_add(&b, ", "); sb_lit(&b, names[i], LIT_STD); }
    sb_add(&b, ")");
}
static void pg_count(bool sys, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT count(*) FROM pg_namespace WHERE has_schema_privilege(oid, 'USAGE')%s", sys ? "" : " AND NOT " PG_SYS);
}
static void pg_probe(const char *ns, int kinds, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT 1 WHERE EXISTS (SELECT 1 FROM pg_class c JOIN pg_namespace s ON s.oid = c.relnamespace WHERE s.nspname = "); sb_lit(&b, ns, LIT_STD);
    sb_add(&b, " AND c.relkind IN ('#'");
    if (kinds & NSK_TABLES) sb_add(&b, ", 'r', 'p', 'f'");
    if (kinds & NSK_VIEWS) sb_add(&b, ", 'v', 'm'");
    if (kinds & NSK_OTHER) sb_add(&b, ", 'S'");
    sb_add(&b, ") AND has_table_privilege(c.oid, 'SELECT'))");
    if (kinds & NSK_ROUTINES) {
        sb_add(&b, " OR EXISTS (SELECT 1 FROM pg_proc p JOIN pg_namespace s ON s.oid = p.pronamespace WHERE s.nspname = "); sb_lit(&b, ns, LIT_STD);
        sb_add(&b, " AND has_function_privilege(p.oid, 'EXECUTE'))");
    }
}
static void pg_tables(const char *ns, int limit, char *o, int n) {
    /* the same names as the driver's list_tables: bare in public, "schema.table" elsewhere */
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT CASE WHEN table_schema = 'public' THEN table_name ELSE table_schema || '.' || table_name END, CASE WHEN table_type = 'VIEW' THEN 1 ELSE 0 END "
               "FROM information_schema.tables WHERE table_schema = "); sb_lit(&b, ns, LIT_STD);
    sb_add(&b, " ORDER BY 2, lower(table_name) LIMIT %d", limit);
}
static void pg_objsearch(const char *ns, const char *pat, bool sub, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT s, t, nm FROM (SELECT n.nspname AS s, CASE c.relkind WHEN 'v' THEN 'VIEW' WHEN 'm' THEN 'MATERIALIZED VIEW' WHEN 'f' THEN 'FOREIGN TABLE' WHEN 'S' THEN 'SEQUENCE' ELSE 'TABLE' END AS t, c.relname AS nm "
               "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f', 'S') AND has_schema_privilege(n.oid, 'USAGE') AND c.relname ILIKE ");
    sb_like(&b, pat, sub, false, LIT_STD);
    if (ns && ns[0]) { sb_add(&b, " AND n.nspname = "); sb_lit(&b, ns, LIT_STD); }
    else sb_add(&b, "%s", " AND NOT (n.nspname IN ('pg_catalog', 'information_schema') OR n.nspname LIKE 'pg\\_%')");
    sb_add(&b, " UNION ALL SELECT n.nspname, CASE WHEN p.prokind = 'p' THEN 'PROCEDURE' ELSE 'FUNCTION' END, p.proname FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace "
               "WHERE has_schema_privilege(n.oid, 'USAGE') AND p.proname ILIKE ");
    sb_like(&b, pat, sub, false, LIT_STD);
    if (ns && ns[0]) { sb_add(&b, " AND n.nspname = "); sb_lit(&b, ns, LIT_STD); }
    else sb_add(&b, "%s", " AND NOT (n.nspname IN ('pg_catalog', 'information_schema') OR n.nspname LIKE 'pg\\_%')");
    sb_add(&b, ") x ORDER BY nm, s, t LIMIT %d", limit);
}
static void pg_discover(int limit, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT DISTINCT table_schema FROM information_schema.tables WHERE table_schema NOT IN ('pg_catalog', 'information_schema') ORDER BY 1 LIMIT %d", limit);
}

/* ---- MySQL / MariaDB: databases (DBX-101) ---- */

#define MY_SYS "schema_name IN ('mysql', 'information_schema', 'performance_schema', 'sys')"
static void my_scope(char *o, int n) { snprintf(o, (size_t)n, "SELECT DATABASE(), CURRENT_USER(), ''"); }
static void my_page(const NsPageReq *r, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT schema_name, CASE WHEN " MY_SYS " THEN 1 ELSE 0 END FROM information_schema.schemata WHERE 1 = 1");
    if (r->after && r->after[0]) { sb_add(&b, " AND schema_name > "); sb_lit(&b, r->after, LIT_MY); }
    if (r->pattern && r->pattern[0]) { sb_add(&b, " AND schema_name LIKE "); sb_like(&b, r->pattern, r->substring, false, LIT_MY); }
    if (!r->include_system) sb_add(&b, " AND NOT " MY_SYS);
    sb_add(&b, " ORDER BY schema_name LIMIT %d", r->limit);
}
static void my_names_in(const char *const *names, int k, bool nosys, char *o, int n) {
    (void)nosys;
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT schema_name, CASE WHEN " MY_SYS " THEN 1 ELSE 0 END FROM information_schema.schemata WHERE schema_name IN (");
    for (int i = 0; i < k; i++) { if (i) sb_add(&b, ", "); sb_lit(&b, names[i], LIT_MY); }
    sb_add(&b, ")");
}
static void my_count(bool sys, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT COUNT(*) FROM information_schema.schemata%s", sys ? "" : " WHERE NOT " MY_SYS);
}
static void my_probe(const char *ns, int kinds, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT 1 FROM information_schema.tables WHERE table_schema = "); sb_lit(&b, ns, LIT_MY);
    if ((kinds & (NSK_TABLES | NSK_VIEWS)) != (NSK_TABLES | NSK_VIEWS)) sb_add(&b, (kinds & NSK_VIEWS) ? " AND table_type = 'VIEW'" : (kinds & NSK_TABLES) ? " AND table_type <> 'VIEW'" : " AND 1 = 0");
    if (kinds & NSK_ROUTINES) { sb_add(&b, " UNION ALL SELECT 1 FROM information_schema.routines WHERE routine_schema = "); sb_lit(&b, ns, LIT_MY); }
    sb_add(&b, " LIMIT 1");
}
static void my_tables(const char *ns, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT CONCAT(table_schema, '.', table_name), CASE WHEN table_type LIKE '%%VIEW' THEN 1 ELSE 0 END FROM information_schema.tables WHERE table_schema = "); sb_lit(&b, ns, LIT_MY);
    sb_add(&b, " ORDER BY 2, table_name LIMIT %d", limit);
}
static void my_objsearch(const char *ns, const char *pat, bool sub, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT s, t, nm FROM (SELECT table_schema AS s, CASE WHEN table_type LIKE '%%VIEW' THEN 'VIEW' ELSE 'TABLE' END AS t, table_name AS nm FROM information_schema.tables WHERE table_name LIKE ");
    sb_like(&b, pat, sub, false, LIT_MY);
    if (ns && ns[0]) { sb_add(&b, " AND table_schema = "); sb_lit(&b, ns, LIT_MY); } else sb_add(&b, " AND NOT table_schema IN ('mysql', 'information_schema', 'performance_schema', 'sys')");
    sb_add(&b, " UNION ALL SELECT routine_schema, routine_type, routine_name FROM information_schema.routines WHERE routine_name LIKE ");
    sb_like(&b, pat, sub, false, LIT_MY);
    if (ns && ns[0]) { sb_add(&b, " AND routine_schema = "); sb_lit(&b, ns, LIT_MY); }
    sb_add(&b, ") x ORDER BY nm, s, t LIMIT %d", limit);
}
static void my_discover(int limit, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT DISTINCT table_schema FROM information_schema.tables WHERE table_schema NOT IN ('mysql', 'information_schema', 'performance_schema', 'sys') ORDER BY 1 LIMIT %d", limit);
}

/* ---- SQL Server: schemas of the current database (DBX-100) ---- */

#define MS_SYS "(name IN ('sys', 'INFORMATION_SCHEMA', 'guest') OR schema_id BETWEEN 16384 AND 16399)"
static void ms_scope(char *o, int n) { snprintf(o, (size_t)n, "SELECT SCHEMA_NAME(), USER_NAME(), DB_NAME()"); }
static void ms_page(const NsPageReq *r, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT TOP (%d) name, CASE WHEN " MS_SYS " THEN 1 ELSE 0 END FROM sys.schemas WHERE 1 = 1", r->limit);
    if (r->after && r->after[0]) { sb_add(&b, " AND name > "); sb_lit(&b, r->after, LIT_N); }
    if (r->pattern && r->pattern[0]) { sb_add(&b, " AND name LIKE "); sb_like(&b, r->pattern, r->substring, false, LIT_N); sb_add(&b, " ESCAPE '\\'"); }
    if (!r->include_system) sb_add(&b, " AND NOT " MS_SYS);
    sb_add(&b, " ORDER BY name");
}
static void ms_names_in(const char *const *names, int k, bool nosys, char *o, int n) {
    (void)nosys;
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT name, CASE WHEN " MS_SYS " THEN 1 ELSE 0 END FROM sys.schemas WHERE name IN (");
    for (int i = 0; i < k; i++) { if (i) sb_add(&b, ", "); sb_lit(&b, names[i], LIT_N); }
    sb_add(&b, ")");
}
static void ms_count(bool sys, char *o, int n) { snprintf(o, (size_t)n, "SELECT COUNT(*) FROM sys.schemas%s", sys ? "" : " WHERE NOT " MS_SYS); }
static void ms_probe(const char *ns, int kinds, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT TOP 1 1 FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id WHERE s.name = "); sb_lit(&b, ns, LIT_N);
    sb_add(&b, " AND o.is_ms_shipped = 0 AND o.parent_object_id = 0");
    if (kinds != NSK_ALL) {
        sb_add(&b, " AND o.type IN ('#'");
        if (kinds & NSK_TABLES) sb_add(&b, ", 'U'");
        if (kinds & NSK_VIEWS) sb_add(&b, ", 'V'");
        if (kinds & NSK_ROUTINES) sb_add(&b, ", 'P', 'FN', 'IF', 'TF'");
        if (kinds & NSK_OTHER) sb_add(&b, ", 'SN', 'SO'");
        sb_add(&b, ")");
    }
}
static void ms_tables(const char *ns, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT TOP (%d) s.name + '.' + o.name, CASE WHEN o.type = 'V' THEN 1 ELSE 0 END FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id WHERE s.name = ", limit);
    sb_lit(&b, ns, LIT_N);
    sb_add(&b, " AND o.type IN ('U', 'V') AND o.is_ms_shipped = 0 ORDER BY 2, o.name");
}
static void ms_objsearch(const char *ns, const char *pat, bool sub, int limit, char *o, int n) {
    Sb b = { o, n, 0 };
    sb_add(&b, "SELECT TOP (%d) s.name, o.type_desc, o.name FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
               "WHERE o.is_ms_shipped = 0 AND o.parent_object_id = 0 AND o.type IN ('U', 'V', 'P', 'FN', 'IF', 'TF', 'SN', 'SO') AND o.name LIKE ", limit);
    sb_like(&b, pat, sub, false, LIT_N); sb_add(&b, " ESCAPE '\\'");
    if (ns && ns[0]) { sb_add(&b, " AND s.name = "); sb_lit(&b, ns, LIT_N); }
    sb_add(&b, " ORDER BY o.name, s.name");
}
static void ms_discover(int limit, char *o, int n) {
    snprintf(o, (size_t)n, "SELECT DISTINCT TOP (%d) s.name FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id WHERE o.is_ms_shipped = 0 ORDER BY s.name", limit);
}

static const NsAdapter adapters[] = {
    { "oracle", "Schemas", { true, true, true, true, false, true, true, true, false },
      ora_scope, ora_page, ora_names_in, NULL, ora_probe, ora_tables, ora_objsearch, ora_discover },
    { "postgres", "Schemas", { true, true, true, true, true, true, true, true, true },
      pg_scope, pg_page, pg_names_in, pg_count, pg_probe, pg_tables, pg_objsearch, pg_discover },
    { "mysql", "Databases", { true, true, true, true, true, true, true, true, false },
      my_scope, my_page, my_names_in, my_count, my_probe, my_tables, my_objsearch, my_discover },
    { "mariadb", "Databases", { true, true, true, true, true, true, true, true, false },
      my_scope, my_page, my_names_in, my_count, my_probe, my_tables, my_objsearch, my_discover },
    { "sqlserver", "Schemas", { true, true, true, true, true, true, true, true, true },
      ms_scope, ms_page, ms_names_in, ms_count, ms_probe, ms_tables, ms_objsearch, ms_discover },
};

const NsAdapter *nsx_adapter(const char *driver) {
    if (!driver) return NULL;
    for (size_t i = 0; i < sizeof adapters / sizeof adapters[0]; i++) if (!strcmp(adapters[i].driver, driver)) return &adapters[i];
    return NULL;   /* SQLite: main / temp / attached keep the existing tree (DBX-102) */
}

/* ---- small helpers ---- */

static char *xstrdup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }
static int list_find(char **l, int n, const char *s) { for (int i = 0; i < n; i++) if (!strcmp(l[i], s)) return i; return -1; }
static void list_add(char ***l, int *n, const char *s) {
    if (list_find(*l, *n, s) >= 0) return;
    *l = realloc(*l, sizeof **l * (size_t)(*n + 1)); (*l)[(*n)++] = xstrdup(s);
}
static void list_del(char **l, int *n, const char *s) {
    int i = list_find(l, *n, s); if (i < 0) return;
    free(l[i]); memmove(l + i, l + i + 1, sizeof *l * (size_t)(*n - i - 1)); (*n)--;
}
static void list_free(char ***l, int *n) { for (int i = 0; i < *n; i++) free((*l)[i]); free(*l); *l = NULL; *n = 0; }
static uint64_t fnv(uint64_t h, const char *s) { for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } h ^= 0xff; h *= 1099511628211ULL; return h; }

/* ---- registry and preferences ---- */

static NsState *reg[64]; static int nreg;
static char prefs_dir[1024];

/* Settings > Schema Explorer: catalog reads and a schema's table listing, for every connection */
static int g_read_ms = 5000, g_tables_ms = NSX_TABLES_BUDGET_MS;
void nsx_set_timeouts(int read_ms, int tables_ms) {
    if (read_ms >= 500) g_read_ms = read_ms;
    if (tables_ms >= 1000) g_tables_ms = tables_ms;
}
static void prefs_default(NsPrefs *p) {
    p->mode = NSM_COMPACT; p->auto_cap = 20; p->page_size = 100; p->timeout_ms = 0; p->kinds = NSK_ALL;
    p->show_system = false; p->auto_recent = true; p->auto_known = true; p->discover = false;
}
static NsState *state_new(const char *conn_id) {
    if (nreg >= 64) return NULL;
    NsState *s = calloc(1, sizeof *s);
    snprintf(s->conn_id, sizeof s->conn_id, "%s", conn_id);
    prefs_default(&s->prefs);
    reg[nreg++] = s;
    return s;
}
void nsx_set_dir(const char *d) { snprintf(prefs_dir, sizeof prefs_dir, "%s", d ? d : ""); }
NsState *nsx_find(const char *conn_id) {
    for (int i = 0; i < nreg; i++) if (!strcmp(reg[i]->conn_id, conn_id)) return reg[i];
    return NULL;
}
NsState *nsx_get(const char *conn_id, const char *driver) {
    const NsAdapter *ad = nsx_adapter(driver);
    if (!ad || !conn_id) return NULL;
    NsState *s = nsx_find(conn_id);
    if (!s) s = state_new(conn_id);
    if (s) s->ad = ad;
    return s;
}

static void pager_clear(NsPager *p) {
    for (int i = 0; i < p->n; i++) free(p->names[i]);
    free(p->names); free(p->sys);
    uint32_t g = p->gen;
    memset(p, 0, sizeof *p);
    p->gen = g + 1;   /* a response of the old listing is dropped */
}
static void info_tables_free(NsInfo *in) {
    for (int i = 0; i < in->ntables; i++) free(in->tables[i].name);
    free(in->tables); in->tables = NULL; in->ntables = 0; in->tables_loaded = in->tables_loading = in->tables_more = false; in->tables_err[0] = 0;
}
static void info_clear(NsState *s) {
    for (int i = 0; i < s->ninfo; i++) info_tables_free(&s->info[i]);
    free(s->info); s->info = NULL; s->ninfo = s->capinfo = 0;
}
static void search_clear_hits(NsSearch *q) { free(q->hits); q->hits = NULL; q->nhits = 0; }
static void req_free(NsReq *r) { free(r->sql); free(r->aux); r->sql = r->aux = NULL; }
static void queue_clear(NsState *s) { for (int i = 0; i < s->nq; i++) req_free(&s->queue[i]); s->nq = 0; }

static void session_clear(NsState *s) {
    queue_clear(s);
    if (s->has_running) { req_free(&s->running); s->has_running = false; }
    pager_clear(&s->pager);
    info_clear(s);
    list_free(&s->anchors, &s->nanchors);
    list_free(&s->known, &s->nknown);
    search_clear_hits(&s->search); s->search.gen++; s->search.pending = s->search.loading = false;
    s->scope_known = s->scope_loading = s->scope_failed = false;
    s->total_known = false; s->count_loading = false; s->count_gen++;
    s->discover_done = s->discover_loading = false;
    s->rev++;
}
void nsx_disconnected(const char *conn_id) { NsState *s = nsx_find(conn_id); if (s) session_clear(s); }

static void state_free(NsState *s) {
    session_clear(s);
    list_free(&s->prefs.pins, &s->prefs.npins); list_free(&s->prefs.suppressed, &s->prefs.nsup); list_free(&s->prefs.recent, &s->prefs.nrecent);
    free(s->featured);
    free(s);
}
void nsx_shutdown(void) { for (int i = 0; i < nreg; i++) state_free(reg[i]); nreg = 0; }

static const char *mode_name(int m) { return m == NSM_ALL ? "all" : m == NSM_PINNED ? "pinned" : "compact"; }

/* schemas.txt: per connection, the tree preferences only (no credentials, no cached metadata) */
void nsx_save(void) {
    if (!prefs_dir[0]) return;
    char path[1200], tmp[1210];
    snprintf(path, sizeof path, "%s%sschemas.txt", prefs_dir, prefs_dir[strlen(prefs_dir) - 1] == '/' ? "" : "/");
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) return;
    fprintf(f, "# Lume Database Explorer: schema tree preferences per connection (no credentials, no cached metadata)\n");
    for (int i = 0; i < nreg; i++) {
        NsState *s = reg[i]; NsPrefs *p = &s->prefs; NsPrefs d; prefs_default(&d);
        bool custom = p->npins || p->nsup || p->nrecent || p->mode != d.mode || p->auto_cap != d.auto_cap || p->page_size != d.page_size || p->timeout_ms != d.timeout_ms ||
                      p->kinds != d.kinds || p->show_system != d.show_system || p->auto_recent != d.auto_recent || p->auto_known != d.auto_known || p->discover != d.discover;
        if (!custom) continue;
        fprintf(f, "[%s]\nmode=%s\nauto_cap=%d\npage_size=%d\ntimeout_ms=%d\nkinds=%d\nsystem=%d\nauto_recent=%d\nauto_known=%d\ndiscover=%d\n", s->conn_id, mode_name(p->mode),
                p->auto_cap, p->page_size, p->timeout_ms, p->kinds, p->show_system, p->auto_recent, p->auto_known, p->discover);
        for (int k = 0; k < p->npins; k++) fprintf(f, "pin=%s\n", p->pins[k]);
        for (int k = 0; k < p->nsup; k++) fprintf(f, "suppress=%s\n", p->suppressed[k]);
        for (int k = 0; k < p->nrecent; k++) fprintf(f, "recent=%s\n", p->recent[k]);
    }
    fclose(f);
    rename(tmp, path);
}
void nsx_load(void) {
    if (!prefs_dir[0]) return;
    char path[1200];
    snprintf(path, sizeof path, "%s%sschemas.txt", prefs_dir, prefs_dir[strlen(prefs_dir) - 1] == '/' ? "" : "/");
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[600]; NsState *s = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (line[0] == '#' || !line[0]) continue;
        if (line[0] == '[') { char id[64] = ""; sscanf(line, "[%63[^]]", id); s = nsx_find(id); if (!s) s = state_new(id); continue; }
        if (!s) continue;
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; const char *k = line, *v = eq + 1; NsPrefs *p = &s->prefs;
        if (!strcmp(k, "mode")) p->mode = !strcmp(v, "all") ? NSM_ALL : !strcmp(v, "pinned") ? NSM_PINNED : NSM_COMPACT;
        else if (!strcmp(k, "auto_cap")) p->auto_cap = atoi(v);
        else if (!strcmp(k, "page_size")) p->page_size = atoi(v) >= 10 ? atoi(v) : 100;
        else if (!strcmp(k, "timeout_ms")) p->timeout_ms = atoi(v) >= 500 && atoi(v) != 5000 ? atoi(v) : 0;   /* 5000: the old fixed default, now Settings' */
        else if (!strcmp(k, "kinds")) p->kinds = atoi(v) & NSK_ALL ? atoi(v) & NSK_ALL : NSK_ALL;
        else if (!strcmp(k, "system")) p->show_system = atoi(v) != 0;
        else if (!strcmp(k, "auto_recent")) p->auto_recent = atoi(v) != 0;
        else if (!strcmp(k, "auto_known")) p->auto_known = atoi(v) != 0;
        else if (!strcmp(k, "discover")) p->discover = atoi(v) != 0;
        else if (!strcmp(k, "pin") && v[0]) list_add(&p->pins, &p->npins, v);
        else if (!strcmp(k, "suppress") && v[0]) list_add(&p->suppressed, &p->nsup, v);
        else if (!strcmp(k, "recent") && v[0]) list_add(&p->recent, &p->nrecent, v);
    }
    fclose(f);
}

/* ---- requests ---- */

static int req_timeout(NsState *s, int kind) {
    int read = s->prefs.timeout_ms > 0 ? s->prefs.timeout_ms : g_read_ms;   /* a connection's own value wins */
    switch (kind) {
    case NSR_COUNT: return 2000;                 /* only when cheap: past its budget the count stays unknown */
    case NSR_DISCOVER: return 10000;
    case NSR_TABLES: return read > g_tables_ms ? read : g_tables_ms;
    case NSR_SCOPE: case NSR_NAMES: return read > 5000 ? read : 5000;
    default: return read;
    }
}
/* queues a request unless an identical one is queued or running (coalescing, DBX-119) */
static bool push(NsState *s, int kind, uint32_t gen, const char *key, char *sql, int limit, char *aux) {
    key = key ? key : "";
    bool dup = s->has_running && s->running.kind == kind && s->running.gen == gen && !strcmp(s->running.key, key);
    for (int i = 0; !dup && i < s->nq; i++) dup = s->queue[i].kind == kind && s->queue[i].gen == gen && !strcmp(s->queue[i].key, key);
    if (dup || s->nq >= NSX_QUEUE) { free(sql); free(aux); return false; }
    NsReq *r = &s->queue[s->nq++];
    memset(r, 0, sizeof *r);
    r->kind = kind; r->gen = gen; snprintf(r->key, sizeof r->key, "%s", key); r->sql = sql; r->limit = limit; r->aux = aux;
    r->timeout_ms = req_timeout(s, kind);
    return true;
}
static char *sqlbuf(void) { return calloc(1, 16384); }

static void queue_names_check(NsState *s) {
    /* do the pinned / recent / suppressed names exist in this scope? one query for all (DBX-121) */
    const char *names[96]; int k = 0;
    for (int i = 0; i < s->prefs.npins && k < 96; i++) names[k++] = s->prefs.pins[i];
    for (int i = 0; i < s->prefs.nrecent && k < 96; i++) if (list_find(s->prefs.pins, s->prefs.npins, s->prefs.recent[i]) < 0) names[k++] = s->prefs.recent[i];
    for (int i = 0; i < s->prefs.nsup && k < 96; i++) {
        bool seen = false; for (int j = 0; j < k && !seen; j++) seen = !strcmp(names[j], s->prefs.suppressed[i]);
        if (!seen) names[k++] = s->prefs.suppressed[i];
    }
    if (!k) return;
    char *sql = sqlbuf(); s->ad->names_in_sql(names, k, s->sysflag_unavailable, sql, 16384);
    size_t len = 1; for (int i = 0; i < k; i++) len += strlen(names[i]) + 1;
    char *aux = calloc(1, len); for (int i = 0; i < k; i++) { strcat(aux, names[i]); strcat(aux, "\n"); }
    push(s, NSR_NAMES, s->count_gen, "", sql, 0, aux);
}
static void queue_count(NsState *s) {
    if (!s->ad->caps.cheap_count || !s->ad->count_sql) return;   /* never a costly COUNT for a label (DBX-121) */
    char *sql = sqlbuf(); s->ad->count_sql(s->prefs.show_system, sql, 16384);
    s->count_loading = true;
    push(s, NSR_COUNT, s->count_gen, "", sql, 1, NULL);
}
static void queue_page(NsState *s, int limit) {
    NsPager *p = &s->pager;
    if (p->loading || (p->started && !p->has_more)) return;
    NsPageReq r = { p->cursor, "", false, limit + 1, s->prefs.show_system, s->sysflag_unavailable };
    char *sql = sqlbuf(); s->ad->page_sql(&r, sql, 16384);
    p->started = true; p->loading = true; p->failed = false; p->error[0] = 0;
    char key[32]; snprintf(key, sizeof key, "%d", p->n);
    push(s, NSR_PAGE, p->gen, key, sql, limit, NULL);
}

void nsx_connected(NsState *s, const char *session_schema, double now_ms) {
    session_clear(s);
    s->connected_at = now_ms;
    s->scope_loading = true;
    if (session_schema && session_schema[0]) snprintf(s->current, sizeof s->current, "%s", session_schema);
    char *sql = sqlbuf(); s->ad->scope_sql(sql, 16384);
    push(s, NSR_SCOPE, s->count_gen, "", sql, 1, NULL);
    s->rev++;
}

void nsx_note_known(NsState *s, const char *const *names, int n) {
    list_free(&s->known, &s->nknown);
    for (int i = 0; i < n; i++) if (names[i] && names[i][0]) list_add(&s->known, &s->nknown, names[i]);
    s->rev++;
}

void nsx_refresh(NsState *s) {
    /* the highlights and the start of discovery; never the whole catalog (DBX-104 / 111) */
    double t = s->connected_at;
    char cur[128]; snprintf(cur, sizeof cur, "%s", s->current);
    uint64_t h = s->scope_hash;
    session_clear(s);
    s->scope_hash = h;
    nsx_connected(s, cur, t);
}
void nsx_refresh_others(NsState *s) {
    int pages = s->pager.pages;
    bool was = s->pager.started;
    pager_clear(&s->pager);
    if (was) { s->pager.want = pages > 0 ? pages : 1; queue_page(s, s->prefs.page_size); }
    s->rev++;
}
void nsx_refresh_schema(NsState *s, const char *name) {
    NsInfo *in = nsx_info(s, name, false);
    if (in) { info_tables_free(in); in->vis = NSV_UNKNOWN; in->note[0] = 0; in->checked_ms = 0; }
    s->rev++;
}

/* ---- info ---- */

NsInfo *nsx_info(NsState *s, const char *name, bool create) {
    for (int i = 0; i < s->ninfo; i++) if (!strcmp(s->info[i].name, name)) return &s->info[i];
    if (!create) return NULL;
    if (s->ninfo == s->capinfo) { s->capinfo = s->capinfo ? s->capinfo * 2 : 32; s->info = realloc(s->info, sizeof *s->info * (size_t)s->capinfo); }
    NsInfo *in = &s->info[s->ninfo++];
    memset(in, 0, sizeof *in);
    snprintf(in->name, sizeof in->name, "%s", name);
    return in;
}
const char *nsx_vis_text(int v) {
    switch (v) {
    case NSV_HAS: return "Has visible objects";
    case NSV_NONE: return "No visible objects for this session (not proof that it is empty)";
    case NSV_ERROR: return "Could not verify";
    case NSV_LOADING: return "Checking...";
    default: return "Not verified";
    }
}

/* ---- the featured list (DBX-077 / 081 / 086) ---- */

static bool suppressed(NsState *s, const char *n) { return list_find(s->prefs.suppressed, s->prefs.nsup, n) >= 0; }
static NsItem *feat_find(NsState *s, const char *n) { for (int i = 0; i < s->nfeatured; i++) if (!strcmp(s->featured[i].name, n)) return &s->featured[i]; return NULL; }
static void feat_add(NsState *s, const char *n, int flag) {
    NsItem *it = feat_find(s, n);
    if (it) { it->flags |= flag; return; }
    if (s->nfeatured == s->capfeat) { s->capfeat = s->capfeat ? s->capfeat * 2 : 32; s->featured = realloc(s->featured, sizeof *s->featured * (size_t)s->capfeat); }
    it = &s->featured[s->nfeatured++];
    snprintf(it->name, sizeof it->name, "%s", n); it->flags = flag;
}
static int feat_cmp(const void *a, const void *b) { return strcmp(((const NsItem *)a)->name, ((const NsItem *)b)->name); }
static bool is_system(NsState *s, const char *n) { NsInfo *in = nsx_info(s, n, false); return in && in->system; }
static bool missing(NsState *s, const char *n) { NsInfo *in = nsx_info(s, n, false); return in && in->exists_known && !in->exists; }
/* the whole (small) catalog is loaded and fits under the cap: everything is shown directly */
static bool small_catalog(NsState *s) {
    return s->total_known && s->total <= s->prefs.auto_cap && s->pager.started && !s->pager.has_more && !s->pager.loading && !s->pager.failed;
}

int nsx_featured(NsState *s) {
    s->nfeatured = 0;
    if (s->current[0]) feat_add(s, s->current, NSF_CURRENT);   /* never hidden (DBX-081.2) */
    for (int i = 0; i < s->prefs.npins; i++) feat_add(s, s->prefs.pins[i], NSF_PINNED);
    if (s->prefs.mode != NSM_PINNED) {
        int autos = 0, cap = s->prefs.auto_cap;
        if (s->prefs.auto_recent)
            for (int i = 0; i < s->prefs.nrecent; i++) {
                const char *n = s->prefs.recent[i];
                if (suppressed(s, n) || missing(s, n) || (!s->prefs.show_system && is_system(s, n))) continue;
                NsItem *it = feat_find(s, n);
                if (it) it->flags |= NSF_RECENT;
                else if (autos < cap) { feat_add(s, n, NSF_RECENT); autos++; }
            }
        if (small_catalog(s))
            for (int i = 0; i < s->pager.n; i++) if (!suppressed(s, s->pager.names[i])) feat_add(s, s->pager.names[i], NSF_SMALL);
        if (s->prefs.auto_known) {
            /* candidates already known: owners of tables in the connection's list, probes and the
             * opt-in discovery; never a scan to rank (DBX-086.3) */
            for (int i = 0; i < s->nknown; i++) {
                const char *n = s->known[i];
                if (suppressed(s, n) || (!s->prefs.show_system && is_system(s, n))) continue;
                NsItem *it = feat_find(s, n);
                if (it) it->flags |= NSF_AUTO; else if (autos < cap) { feat_add(s, n, NSF_AUTO); autos++; }
            }
            for (int i = 0; i < s->ninfo; i++) {
                NsInfo *in = &s->info[i];
                if (in->vis != NSV_HAS || suppressed(s, in->name) || (!s->prefs.show_system && in->system)) continue;
                NsItem *it = feat_find(s, in->name);
                if (it) it->flags |= NSF_AUTO; else if (autos < cap) { feat_add(s, in->name, NSF_AUTO); autos++; }
            }
        }
    }
    qsort(s->featured, (size_t)s->nfeatured, sizeof *s->featured, feat_cmp);   /* stable, deterministic order */
    return s->nfeatured;
}
bool nsx_is_featured(NsState *s, const char *n) { return feat_find(s, n) != NULL; }
int nsx_featured_flags(NsState *s, const char *n) { NsItem *it = feat_find(s, n); return it ? it->flags : 0; }

long long nsx_others_exact(NsState *s) {
    if (!s->total_known) return -1;
    /* every name of this scope and filter is loaded: Others is what the list holds, exactly */
    if (s->pager.started && !s->pager.has_more && !s->pager.loading && !s->pager.failed) return nsx_others_names(s, NULL, 0);
    long long shown = 0;
    for (int i = 0; i < s->nfeatured; i++) {
        NsItem *it = &s->featured[i];
        NsInfo *in = nsx_info(s, it->name, false);
        bool exists;
        if (it->flags & (NSF_SMALL | NSF_AUTO)) exists = true;
        else if (it->flags & NSF_CURRENT && !(it->flags & NSF_PINNED)) exists = s->scope_known;
        else if (in && in->exists_known) exists = in->exists;
        else if (it->flags & NSF_CURRENT) exists = s->scope_known;
        else return -1;   /* a pin we could not check: the number would be a guess */
        if (!exists) continue;
        if (!s->prefs.show_system && in && in->system) continue;   /* not in the filtered total */
        shown++;
    }
    long long n = s->total - shown;
    return n >= 0 ? n : -1;
}
bool nsx_others_shown(NsState *s) {
    if (s->prefs.mode == NSM_ALL) return false;
    return !(nsx_others_exact(s) == 0 && s->nanchors == 0);   /* hidden only when nothing else can exist */
}
void nsx_others_label(NsState *s, char *out, int n) {
    long long k = nsx_others_exact(s);
    if (k < 0) { snprintf(out, (size_t)n, "Others"); return; }
    char d[32]; snprintf(d, sizeof d, "%lld", k);
    char g[48]; int len = (int)strlen(d), o = 0;
    for (int i = 0; i < len; i++) { if (i && (len - i) % 3 == 0) g[o++] = ','; g[o++] = d[i]; }
    g[o] = 0;
    snprintf(out, (size_t)n, "Others (%s)", g);
}
static bool in_pager(NsState *s, const char *n) {
    for (int i = 0; i < s->pager.n; i++) if (!strcmp(s->pager.names[i], n)) return true;
    return false;
}
int nsx_others_names(NsState *s, const char **out, int max) {
    int k = 0;
    for (int i = 0; i < s->nanchors; i++) if (!feat_find(s, s->anchors[i])) { if (k < max && out) out[k] = s->anchors[i]; k++; }
    for (int i = 0; i < s->pager.n; i++) {
        const char *n = s->pager.names[i];
        if (feat_find(s, n)) continue;
        if (!s->prefs.show_system && s->pager.sys[i]) continue;
        if (k < max && out) out[k] = n;
        k++;
    }
    return k;
}

/* ---- actions ---- */

void nsx_expand_others(NsState *s) {
    if (!s->pager.started) queue_page(s, s->prefs.page_size);   /* names only, one page (DBX-079) */
    s->rev++;
}
void nsx_load_more(NsState *s) { queue_page(s, s->prefs.page_size); s->rev++; }

static void anchor_if_needed(NsState *s, const char *n) {
    if (in_pager(s, n) || list_find(s->anchors, s->nanchors, n) >= 0) return;
    list_add(&s->anchors, &s->nanchors, n);
}
void nsx_pin(NsState *s, const char *n) {
    if (!n || !n[0]) return;
    list_add(&s->prefs.pins, &s->prefs.npins, n);
    list_del(s->prefs.suppressed, &s->prefs.nsup, n);
    NsInfo *in = nsx_info(s, n, false);
    if (!in || !in->exists_known) queue_names_check(s);   /* keeps Others (N) exact when it was */
    nsx_save();
    s->rev++;
}
bool nsx_can_unpin(NsState *s, const char *n) { return n && strcmp(n, s->current) != 0; }
void nsx_unpin(NsState *s, const char *n) {
    if (!nsx_can_unpin(s, n)) return;   /* the session's schema stays visible (DBX-081.2) */
    list_del(s->prefs.pins, &s->prefs.npins, n);
    list_add(&s->prefs.suppressed, &s->prefs.nsup, n);   /* no automatic promotion until pinned again (DBX-086.5) */
    anchor_if_needed(s, n);                               /* stays reachable in Others */
    nsx_save();
    s->rev++;
}
void nsx_note_recent(NsState *s, const char *n) {
    if (!n || !n[0]) return;
    list_del(s->prefs.recent, &s->prefs.nrecent, n);
    s->prefs.recent = realloc(s->prefs.recent, sizeof *s->prefs.recent * (size_t)(s->prefs.nrecent + 1));
    memmove(s->prefs.recent + 1, s->prefs.recent, sizeof *s->prefs.recent * (size_t)s->prefs.nrecent);
    s->prefs.recent[0] = xstrdup(n); s->prefs.nrecent++;
    while (s->prefs.nrecent > 10) { free(s->prefs.recent[--s->prefs.nrecent]); }
    NsInfo *in = nsx_info(s, n, true); in->exists_known = true; in->exists = true;   /* the user just opened it */
    nsx_save();
    s->rev++;
}
void nsx_reveal(NsState *s, const char *n) {
    nsx_featured(s);
    if (!feat_find(s, n)) anchor_if_needed(s, n);
    s->rev++;
}
static void filter_changed(NsState *s) {
    pager_clear(&s->pager);
    list_free(&s->anchors, &s->nanchors);
    s->total_known = false; s->count_loading = false; s->count_gen++;
    if (s->scope_known) { queue_count(s); queue_names_check(s); }
    s->rev++;
}
void nsx_set_mode(NsState *s, int mode) {
    s->prefs.mode = mode;
    if (mode == NSM_ALL && !s->pager.started) queue_page(s, s->prefs.page_size);
    nsx_save(); s->rev++;
}
void nsx_set_system(NsState *s, bool on) {
    if (s->prefs.show_system == on) return;
    s->prefs.show_system = on;
    filter_changed(s);
    nsx_save();
}
void nsx_set_kinds(NsState *s, int kinds) {
    kinds &= NSK_ALL; if (!kinds) kinds = NSK_ALL;
    if (s->prefs.kinds == kinds) return;
    s->prefs.kinds = kinds;
    /* only classifications of the old scope are dropped; nothing is re-checked by itself (DBX-089.2) */
    for (int i = 0; i < s->ninfo; i++) if (s->info[i].vis != NSV_LOADING) { s->info[i].vis = NSV_UNKNOWN; s->info[i].note[0] = 0; s->info[i].checked_ms = 0; }
    s->discover_done = false;
    nsx_save(); s->rev++;
}
void nsx_set_option(NsState *s, const char *k, int v) {
    NsPrefs *p = &s->prefs;
    if (!strcmp(k, "auto_cap")) p->auto_cap = v < 0 ? 0 : v > 200 ? 200 : v;
    else if (!strcmp(k, "page_size")) p->page_size = v < 10 ? 10 : v > 1000 ? 1000 : v;
    else if (!strcmp(k, "timeout_ms")) p->timeout_ms = v <= 0 ? 0 : v < 500 ? 500 : v > 120000 ? 120000 : v;   /* 0: follow Settings */
    else if (!strcmp(k, "auto_recent")) p->auto_recent = v != 0;
    else if (!strcmp(k, "auto_known")) p->auto_known = v != 0;
    else if (!strcmp(k, "discover")) { p->discover = v != 0; if (p->discover && s->scope_known) nsx_discover(s); }
    else return;
    nsx_save(); s->rev++;
}
void nsx_reset_prefs(NsState *s) {
    list_free(&s->prefs.pins, &s->prefs.npins); list_free(&s->prefs.suppressed, &s->prefs.nsup); list_free(&s->prefs.recent, &s->prefs.nrecent);
    bool sys = s->prefs.show_system;
    prefs_default(&s->prefs);
    list_free(&s->anchors, &s->nanchors);
    if (sys) filter_changed(s);
    nsx_save(); s->rev++;
}

void nsx_search(NsState *s, const char *text, bool substring, bool objects, const char *ns, uint64_t now) {
    NsSearch *q = &s->search;
    snprintf(q->text, sizeof q->text, "%s", text ? text : "");
    q->substring = substring; q->objects = objects; snprintf(q->ns, sizeof q->ns, "%s", ns ? ns : "");
    q->gen++;                       /* any answer still on its way is obsolete */
    q->failed = q->partial = q->done = false; q->error[0] = 0;
    if (!q->text[0]) { q->pending = false; q->loading = false; search_clear_hits(q); s->rev++; return; }
    q->pending = true; q->due = now + 250;   /* debounce */
    s->rev++;
}
void nsx_search_clear(NsState *s) { nsx_search(s, "", false, false, "", 0); }

void nsx_probe(NsState *s, const char *n) {
    if (!s->ad->caps.visibility_probe) return;
    NsInfo *in = nsx_info(s, n, true);
    in->vis = NSV_LOADING;
    char *sql = sqlbuf(); s->ad->probe_sql(n, s->prefs.kinds, sql, 16384);
    push(s, NSR_PROBE, s->count_gen, n, sql, 1, NULL);
    s->rev++;
}
void nsx_load_tables(NsState *s, const char *n) {
    NsInfo *in = nsx_info(s, n, true);
    if (in->tables_loaded || in->tables_loading) return;
    in->tables_loading = true; in->tables_err[0] = 0; in->tables_cancel = false; in->tables_secs = 0;
    char *sql = sqlbuf(); s->ad->tables_sql(n, 5001, sql, 16384);   /* 5000 shown; more = "use Find Objects" */
    push(s, NSR_TABLES, s->count_gen, n, sql, 5000, NULL);
    s->rev++;
}
void nsx_cancel_tables(NsState *s, const char *n) {
    NsInfo *in = nsx_info(s, n, false);
    if (!in || !in->tables_loading) return;
    for (int i = 0; i < s->nq; i++) {
        if (s->queue[i].kind != NSR_TABLES || strcmp(s->queue[i].key, n)) continue;
        free(s->queue[i].sql); free(s->queue[i].aux);
        memmove(s->queue + i, s->queue + i + 1, sizeof *s->queue * (size_t)(s->nq - i - 1)); s->nq--;
        in->tables_loading = false; snprintf(in->tables_err, sizeof in->tables_err, "cancelled");
        s->rev++;
        return;
    }
    if (s->has_running && s->running.kind == NSR_TABLES && !strcmp(s->running.key, n)) in->tables_cancel = true;
}
void nsx_discover(NsState *s) {
    if (!s->ad->caps.discovery || s->discover_loading) return;
    s->discover_loading = true; s->discover_note[0] = 0;
    char *sql = sqlbuf(); s->ad->discover_sql(s->prefs.auto_cap > 0 ? s->prefs.auto_cap * 5 : 100, sql, 16384);
    push(s, NSR_DISCOVER, s->count_gen, "", sql, 0, NULL);
    s->rev++;
}

int nsx_copy_loaded(NsState *s, char *out, int n) {
    int o = 0, k = 0;
    out[0] = 0;
    nsx_featured(s);
    for (int i = 0; i < s->nfeatured && o < n - 1; i++, k++) o += snprintf(out + o, (size_t)(n - o), "%s\n", s->featured[i].name);
    for (int i = 0; i < s->nanchors && o < n - 1; i++) if (!feat_find(s, s->anchors[i])) { o += snprintf(out + o, (size_t)(n - o), "%s\n", s->anchors[i]); k++; }
    for (int i = 0; i < s->pager.n && o < n - 1; i++) {
        if (feat_find(s, s->pager.names[i]) || (!s->prefs.show_system && s->pager.sys[i])) continue;
        o += snprintf(out + o, (size_t)(n - o), "%s\n", s->pager.names[i]); k++;
    }
    return k;
}

/* ---- execution ---- */

bool nsx_busy(NsState *s) { return s->has_running || s->nq > 0 || s->search.pending; }
uint64_t nsx_next_due(NsState *s) { return s->search.pending ? s->search.due : 0; }

const NsReq *nsx_next_request(NsState *s, uint64_t now) {
    if (s->has_running) return NULL;   /* one catalog statement at a time per connection (DBX-119) */
    NsSearch *q = &s->search;
    if (q->pending && now >= q->due) {
        q->pending = false; q->loading = true;
        char *sql = sqlbuf();
        int limit = 200;
        if (q->objects) s->ad->objsearch_sql(q->ns, q->text, q->substring, limit + 1, sql, 16384);
        else { NsPageReq r = { "", q->text, q->substring, limit + 1, true, s->sysflag_unavailable }; s->ad->page_sql(&r, sql, 16384); }   /* system ones too, marked (DBX-124.4) */
        memset(&s->running, 0, sizeof s->running);
        s->running.kind = q->objects ? NSR_OBJSEARCH : NSR_SEARCH; s->running.gen = q->gen; s->running.sql = sql; s->running.limit = limit;
        s->running.timeout_ms = req_timeout(s, NSR_SEARCH); s->running.started = now;
        s->has_running = true; s->m.queries++;
        return &s->running;
    }
    if (!s->nq) return NULL;
    s->running = s->queue[0];
    memmove(s->queue, s->queue + 1, sizeof *s->queue * (size_t)(s->nq - 1)); s->nq--;
    s->running.started = now;
    s->has_running = true; s->m.queries++;
    return &s->running;
}

bool nsx_running_stale(NsState *s) {
    if (!s->has_running) return false;
    NsReq *r = &s->running;
    if (r->kind == NSR_SEARCH || r->kind == NSR_OBJSEARCH) return r->gen != s->search.gen;
    if (r->kind == NSR_PAGE) return r->gen != s->pager.gen;
    if (r->kind == NSR_COUNT || r->kind == NSR_NAMES) return r->gen != s->count_gen;
    if (r->kind == NSR_TABLES) { NsInfo *in = nsx_info(s, r->key, false); return in && in->tables_cancel; }
    return false;
}

void nsx_error_hint(const char *err, char *out, int n) {
    const char *e = err && err[0] ? err : "unknown error";
    const char *hint = "";
    if (strstr(e, "ORA-00942")) hint = "a catalog view is not accessible to this user (ORA-00942)";
    else if (strstr(e, "ORA-01031")) hint = "insufficient privileges for this catalog query (ORA-01031)";
    else if (strstr(e, "ORA-00904")) hint = "this server's catalog lacks a column the query uses (ORA-00904)";
    else if (strstr(e, "timed out")) hint = "the query took longer than its budget";
    else if (strstr(e, "ORA-03113") || strstr(e, "ORA-03114") || strstr(e, "server closed") || strstr(e, "Lost connection") || strstr(e, "no connection") || strstr(e, "connection lost"))
        hint = "the connection was lost";
    else if (strcasestr(e, "permission denied")) hint = "permission denied on the catalog";
    char first[160]; snprintf(first, sizeof first, "%s", e); first[strcspn(first, "\r\n")] = 0;
    if (hint[0] && !strstr(hint, first)) snprintf(out, (size_t)n, "%s: %s", hint, first);
    else snprintf(out, (size_t)n, "%s", first);
}

static const char *cell(const ResultSet *rs, int r, int c) { const char *v = rs && r < rs->nrows && c < rs->ncols ? rs_cell(rs, r, c) : NULL; return v ? v : ""; }

void nsx_complete(NsState *s, const ResultSet *rs, const char *err, bool timed_out, bool cancelled, double ms, uint64_t now) {
    if (!s->has_running) return;
    NsReq r = s->running; s->has_running = false;
    bool stale = false;
    s->running.sql = s->running.aux = NULL;
    if (r.kind == NSR_SEARCH || r.kind == NSR_OBJSEARCH) stale = r.gen != s->search.gen;
    else if (r.kind == NSR_PAGE) stale = r.gen != s->pager.gen;
    else if (r.kind == NSR_COUNT || r.kind == NSR_NAMES) stale = r.gen != s->count_gen;
    if (timed_out) s->m.timeouts++;
    if (cancelled) s->m.cancels++;
    if (stale) { s->m.skipped_stale++; req_free(&r); return; }   /* generation check (DBX-119.1) */
    bool ok = rs && !err && !timed_out && !cancelled;
    char why[240] = "";
    if (!ok) { s->m.failures += !cancelled; if (timed_out) snprintf(why, sizeof why, "timed out after %d ms", r.timeout_ms); else if (cancelled) snprintf(why, sizeof why, "cancelled"); else nsx_error_hint(err, why, sizeof why); }
    if (timed_out) { char t[240]; nsx_error_hint(why, t, sizeof t); snprintf(why, sizeof why, "%s", t); }

    switch (r.kind) {
    case NSR_SCOPE: {
        s->scope_loading = false;
        if (!ok) {
            /* degraded mode (DBX-118): pins, recent and Others without a count; search still works */
            s->scope_failed = true; snprintf(s->scope_error, sizeof s->scope_error, "%s", why);
            s->m.useful_ms = (double)now - s->connected_at;
            break;
        }
        snprintf(s->current, sizeof s->current, "%s", cell(rs, 0, 0));
        snprintf(s->user, sizeof s->user, "%s", cell(rs, 0, 1));
        snprintf(s->container, sizeof s->container, "%s", cell(rs, 0, 2));
        uint64_t h = fnv(fnv(fnv(1469598103934665603ULL, s->conn_id), s->user), s->container);
        if (s->scope_hash && h != s->scope_hash) {   /* another user / PDB / database: nothing carries over (DBX-095 / 110) */
            info_clear(s); pager_clear(&s->pager); list_free(&s->anchors, &s->nanchors); s->total_known = false; s->count_gen++;
            search_clear_hits(&s->search); s->search.gen++;
        }
        s->scope_hash = h; s->scope_known = true; s->scope_failed = false; s->scope_error[0] = 0;
        if (s->current[0]) { NsInfo *in = nsx_info(s, s->current, true); in->exists_known = in->exists = true; }
        s->m.useful_ms = (double)now - s->connected_at;
        queue_names_check(s);
        queue_count(s);
        if (s->prefs.discover) nsx_discover(s);
        if (s->prefs.mode == NSM_ALL) queue_page(s, s->prefs.page_size);
        break;
    }
    case NSR_NAMES: {
        if (!ok) {
            /* Oracle before 12c: no ORACLE_MAINTAINED; documented fallback: no system flag */
            if (err && strstr(err, "ORA-00904") && !s->sysflag_unavailable) { s->sysflag_unavailable = true; queue_names_check(s); }
            break;
        }
        char *save = NULL;
        for (char *n = strtok_r(r.aux, "\n", &save); n; n = strtok_r(NULL, "\n", &save)) {
            NsInfo *in = nsx_info(s, n, true);
            in->exists_known = true; in->exists = false;
            for (int i = 0; i < rs->nrows; i++) if (!strcmp(cell(rs, i, 0), n)) { in->exists = true; in->system = !strcmp(cell(rs, i, 1), "1"); }
        }
        break;
    }
    case NSR_COUNT:
        s->count_loading = false;
        if (!ok) { s->count_failed = true; break; }   /* the label stays "Others" */
        s->total = atoll(cell(rs, 0, 0)); s->total_known = rs->nrows == 1; s->count_failed = false;
        /* a small catalog is read whole and shown directly (no Others to open) */
        if (s->total_known && s->total <= s->prefs.auto_cap && s->ad->caps.paged_names && !s->pager.started) queue_page(s, s->prefs.auto_cap > 0 ? s->prefs.auto_cap : 1);
        break;
    case NSR_PAGE: {
        NsPager *p = &s->pager;
        p->loading = false;
        s->m.last_page_ms = ms;
        if (!ok) {
            if (err && strstr(err, "ORA-00904") && !s->sysflag_unavailable) { s->sysflag_unavailable = true; p->started = false; queue_page(s, r.limit); break; }
            p->failed = true; snprintf(p->error, sizeof p->error, "%s", why);
            break;
        }
        int take = rs->nrows > r.limit ? r.limit : rs->nrows;
        p->has_more = rs->nrows > r.limit;
        if (p->n + take > p->cap) { p->cap = (p->n + take) * 2; p->names = realloc(p->names, sizeof *p->names * (size_t)p->cap); p->sys = realloc(p->sys, (size_t)p->cap); }
        for (int i = 0; i < take; i++) {
            const char *n = cell(rs, i, 0);
            p->names[p->n] = xstrdup(n); p->sys[p->n] = !strcmp(cell(rs, i, 1), "1"); p->n++;
            list_del(s->anchors, &s->nanchors, n);   /* reached by the pages: no longer anchored */
        }
        if (take) snprintf(p->cursor, sizeof p->cursor, "%s", p->names[p->n - 1]);
        p->pages++; s->m.pages++; s->m.names_loaded = p->n;
        if (!p->has_more && !s->total_known) { s->total = p->n; s->total_known = true; }   /* the end was reached: exact */
        if (p->want > p->pages && p->has_more) queue_page(s, s->prefs.page_size);           /* Refresh Others */
        break;
    }
    case NSR_SEARCH: case NSR_OBJSEARCH: {
        NsSearch *q = &s->search;
        q->loading = false; q->done = true;
        s->m.last_search_ms = ms;
        search_clear_hits(q);
        if (!ok) { q->failed = true; snprintf(q->error, sizeof q->error, "%s", why); break; }
        int take = rs->nrows > r.limit ? r.limit : rs->nrows;
        q->partial = rs->nrows > r.limit;   /* more matches than shown: said so (DBX-085.5) */
        q->hits = calloc((size_t)(take ? take : 1), sizeof *q->hits); q->nhits = take;
        for (int i = 0; i < take; i++) {
            NsHit *h = &q->hits[i];
            if (r.kind == NSR_SEARCH) { snprintf(h->schema, sizeof h->schema, "%s", cell(rs, i, 0)); snprintf(h->type, sizeof h->type, "SCHEMA"); h->system = !strcmp(cell(rs, i, 1), "1"); }
            else { snprintf(h->schema, sizeof h->schema, "%s", cell(rs, i, 0)); snprintf(h->type, sizeof h->type, "%s", cell(rs, i, 1)); snprintf(h->name, sizeof h->name, "%s", cell(rs, i, 2)); }
        }
        break;
    }
    case NSR_PROBE: {
        NsInfo *in = nsx_info(s, r.key, true);
        in->checked_ms = now;
        if (ok) { in->vis = rs->nrows > 0 ? NSV_HAS : NSV_NONE; in->note[0] = 0; in->exists_known = true; in->exists = true; }
        else if (timed_out || cancelled) { in->vis = NSV_UNKNOWN; snprintf(in->note, sizeof in->note, "Not verified: %s", why); }   /* never "empty" (DBX-124.2) */
        else { in->vis = NSV_ERROR; snprintf(in->note, sizeof in->note, "%s", why); }
        break;
    }
    case NSR_TABLES: {
        NsInfo *in = nsx_info(s, r.key, true);
        in->tables_loading = false; in->tables_cancel = false;
        if (!ok) { snprintf(in->tables_err, sizeof in->tables_err, "%s", why); break; }
        int take = rs->nrows > r.limit ? r.limit : rs->nrows;
        in->tables = calloc((size_t)(take ? take : 1), sizeof *in->tables); in->ntables = take;
        for (int i = 0; i < take; i++) { in->tables[i].name = xstrdup(cell(rs, i, 0)); in->tables[i].is_view = !strcmp(cell(rs, i, 1), "1"); }
        in->tables_more = rs->nrows > r.limit;
        in->tables_loaded = true;
        static uint32_t serial; in->tables_serial = ++serial;
        if (take) { in->vis = NSV_HAS; in->checked_ms = now; }   /* evidence; zero tables proves nothing */
        break;
    }
    case NSR_DISCOVER:
        s->discover_loading = false; s->discover_done = true;
        if (!ok) { snprintf(s->discover_note, sizeof s->discover_note, "Discovery stopped: %s. Highlights may be partial.", why); break; }
        for (int i = 0; i < rs->nrows; i++) { NsInfo *in = nsx_info(s, cell(rs, i, 0), true); in->vis = NSV_HAS; in->checked_ms = now; in->exists_known = in->exists = true; }
        snprintf(s->discover_note, sizeof s->discover_note, "Discovery read %d owner%s (bounded)", rs->nrows, rs->nrows == 1 ? "" : "s");
        break;
    }
    req_free(&r);
    s->rev++;
}

/* ---- diagnostics ---- */

int nsx_dump(NsState *s, char *out, int n, bool with_names) {
    nsx_featured(s);
    char lbl[48]; nsx_others_label(s, lbl, sizeof lbl);
    int o = snprintf(out, (size_t)n, "nsx mode=%s featured=%d others=\"%s\" shown=%d loaded=%d pages=%d has_more=%d total=%lld anchors=%d scope=%s%s queries=%d stale=%d timeouts=%d",
                     mode_name(s->prefs.mode), s->nfeatured, nsx_others_shown(s) ? lbl : "-", nsx_others_names(s, NULL, 0), s->pager.n, s->pager.pages, s->pager.has_more,
                     s->total_known ? s->total : -1LL, s->nanchors, s->scope_known ? "known" : s->scope_failed ? "failed" : "loading", s->sysflag_unavailable ? " nosysflag" : "",
                     s->m.queries, s->m.skipped_stale, s->m.timeouts);
    if (with_names && o < n) {
        o += snprintf(out + o, (size_t)(n - o), " list=[");
        for (int i = 0; i < s->nfeatured && o < n; i++) {
            int f = s->featured[i].flags;
            o += snprintf(out + o, (size_t)(n - o), "%s%s:%s%s%s%s%s", i ? "," : "", s->featured[i].name, f & NSF_CURRENT ? "c" : "", f & NSF_PINNED ? "p" : "", f & NSF_RECENT ? "r" : "", f & NSF_AUTO ? "a" : "", f & NSF_SMALL ? "s" : "");
        }
        const char *on[6]; int k = nsx_others_names(s, on, 6);
        if (o < n) o += snprintf(out + o, (size_t)(n - o), "] others_first=[");
        for (int i = 0; i < k && i < 6 && o < n; i++) o += snprintf(out + o, (size_t)(n - o), "%s%s", i ? "," : "", on[i]);
        if (o < n) o += snprintf(out + o, (size_t)(n - o), "] search=%s hits=%d%s%s", s->search.loading ? "loading" : s->search.pending ? "pending" : s->search.done ? "done" : "-",
                                 s->search.nhits, s->search.partial ? " partial" : "", s->search.failed ? " failed" : "");
        if (s->pager.failed && o < n) o += snprintf(out + o, (size_t)(n - o), " page_error=\"%s\"", s->pager.error);
        if (s->search.failed && o < n) o += snprintf(out + o, (size_t)(n - o), " search_error=\"%s\"", s->search.error);
        for (int i = 0; i < s->search.nhits && i < 5 && o < n; i++)
            o += snprintf(out + o, (size_t)(n - o), "%s%s%s%s", i ? "," : " [", s->search.hits[i].schema, s->search.hits[i].name[0] ? "." : "", s->search.hits[i].name);
        if (s->search.nhits && o < n) o += snprintf(out + o, (size_t)(n - o), "]");
    }
    if (o >= n) o = n - 1;
    return o;
}
