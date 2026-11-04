/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object catalog for completion: see objcat.h. */
#include "objcat.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- queries ---- */

#define ORA_TYPES "('TABLE','VIEW','MATERIALIZED VIEW','PROCEDURE','FUNCTION','PACKAGE','SEQUENCE','TYPE')"
#define ORA_MAINT "(SELECT username FROM all_users WHERE oracle_maintained = 'Y')"
/* before 12c there is no oracle_maintained: the schemas Oracle installs, by name */
#define ORA_MAINT_OLD "('SYS','SYSTEM','OUTLN','DBSNMP','XDB','MDSYS','CTXSYS','ORDSYS','ORDDATA','OLAPSYS','WMSYS','EXFSYS','APEX_PUBLIC_USER','FLOWS_FILES','ANONYMOUS','APPQOSSYS','DIP','ORACLE_OCM','SI_INFORMTN_SCHEMA','MDDATA','SPATIAL_CSW_ADMIN_USR','SPATIAL_WFS_ADMIN_USR','LBACSYS','DVSYS','AUDSYS','GSMADMIN_INTERNAL','OJVMSYS','DBSFWUSER','REMOTE_SCHEDULER_AGENT','SYSBACKUP','SYSDG','SYSKM','SYSRAC','GGSYS','XS$NULL','ORDPLUGINS')"
static const char *const ORA[][2] = {
    { "SELECT SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') FROM dual", NULL },
    { "SELECT o.owner, o.object_name, o.object_type, NULL FROM all_objects o WHERE o.object_type IN " ORA_TYPES
      " AND o.object_name NOT LIKE 'BIN$%' AND o.owner NOT IN " ORA_MAINT,
      "SELECT o.owner, o.object_name, o.object_type, NULL FROM all_objects o WHERE o.object_type IN " ORA_TYPES
      " AND o.object_name NOT LIKE 'BIN$%' AND o.owner NOT IN " ORA_MAINT_OLD },
    /* synonyms with their target; those pointing into Oracle's own schemas (thousands of PUBLIC ones) are left out */
    { "SELECT s.owner, s.synonym_name, 'SYNONYM', s.table_owner || '.' || s.table_name FROM all_synonyms s"
      " WHERE s.db_link IS NULL AND s.table_owner NOT IN " ORA_MAINT " AND s.owner NOT IN " ORA_MAINT,
      "SELECT s.owner, s.synonym_name, 'SYNONYM', s.table_owner || '.' || s.table_name FROM all_synonyms s"
      " WHERE s.db_link IS NULL AND s.table_owner NOT IN " ORA_MAINT_OLD " AND s.owner NOT IN " ORA_MAINT_OLD },
};
#define PG_SCHEMAS "n.nspname NOT IN ('pg_catalog', 'information_schema') AND n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp%' AND has_schema_privilege(n.oid, 'USAGE')"
static const char *const PG[][2] = {
    { "SELECT current_schema()", NULL },
    { "SELECT n.nspname, c.relname, CASE c.relkind WHEN 'v' THEN 'VIEW' WHEN 'm' THEN 'MATERIALIZED VIEW' WHEN 'S' THEN 'SEQUENCE' ELSE 'TABLE' END, NULL"
      " FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace WHERE c.relkind IN ('r', 'p', 'v', 'm', 'f', 'S') AND " PG_SCHEMAS, NULL },
    { "SELECT DISTINCT n.nspname, p.proname, CASE p.prokind WHEN 'p' THEN 'PROCEDURE' ELSE 'FUNCTION' END, NULL"
      " FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE " PG_SCHEMAS,
      "SELECT DISTINCT n.nspname, p.proname, 'FUNCTION', NULL FROM pg_proc p JOIN pg_namespace n ON n.oid = p.pronamespace WHERE " PG_SCHEMAS },   /* before 11: no prokind */
};
#define MY_SCHEMAS "('mysql', 'information_schema', 'performance_schema', 'sys')"
static const char *const MY[][2] = {
    { "SELECT DATABASE()", NULL },
    { "SELECT table_schema, table_name, CASE table_type WHEN 'VIEW' THEN 'VIEW' ELSE 'TABLE' END, NULL FROM information_schema.tables WHERE table_schema NOT IN " MY_SCHEMAS, NULL },
    { "SELECT routine_schema, routine_name, routine_type, NULL FROM information_schema.routines WHERE routine_schema NOT IN " MY_SCHEMAS, NULL },
};
static const char *const MS[][2] = {
    { "SELECT SCHEMA_NAME()", NULL },
    { "SELECT s.name, o.name, CASE o.type WHEN 'U' THEN 'TABLE' WHEN 'V' THEN 'VIEW' WHEN 'P' THEN 'PROCEDURE' WHEN 'SN' THEN 'SYNONYM' WHEN 'SO' THEN 'SEQUENCE' ELSE 'FUNCTION' END,"
      " REPLACE(REPLACE(sy.base_object_name, '[', ''), ']', '') FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id LEFT JOIN sys.synonyms sy ON sy.object_id = o.object_id"
      " WHERE o.type IN ('U', 'V', 'P', 'FN', 'IF', 'TF', 'SN', 'SO') AND o.is_ms_shipped = 0", NULL },
};
#define NQ(t) ((int)(sizeof t / sizeof t[0]))

/* quotes s as a SQL string literal body (a ' becomes '') into out */
static void lit(const char *s, char *out, size_t n) {
    size_t k = 0;
    for (; *s && k + 2 < n; s++) { if (*s == '\'') out[k++] = '\''; out[k++] = *s; }
    out[k] = 0;
}
/* See objcat.h. Oracle: ALL_PROCEDURES lists each subprogram of the specification (an overload per
 * row); a member is a function when ALL_ARGUMENTS has its return value (position 0). */
bool objcat_members_sql(const char *driver, const char *schema, const char *name, char *out, size_t n) {
    if (!driver || strcmp(driver, "oracle") || !schema || !name) return false;
    if (!out || !n) return true;   /* only asking whether the driver has packages */
    char s[300], nm[300]; lit(schema, s, sizeof s); lit(name, nm, sizeof nm);
    snprintf(out, n,
        "SELECT p.procedure_name,"
        " CASE WHEN EXISTS (SELECT 1 FROM all_arguments a WHERE a.owner = p.owner AND a.package_name = p.object_name AND a.subprogram_id = p.subprogram_id"
        " AND a.position = 0 AND a.data_level = 0) THEN 'FUNCTION' ELSE 'PROCEDURE' END,"
        " (SELECT LISTAGG(a.argument_name, ', ') WITHIN GROUP (ORDER BY a.position) FROM all_arguments a WHERE a.owner = p.owner AND a.package_name = p.object_name"
        " AND a.subprogram_id = p.subprogram_id AND a.position > 0 AND a.data_level = 0)"
        " FROM all_procedures p WHERE p.owner = '%s' AND p.object_name = '%s' AND p.object_type = 'PACKAGE' AND p.procedure_name IS NOT NULL"
        " ORDER BY p.subprogram_id", s, nm);
    return true;
}
void objcat_members_free(OcMembers *ms) {
    if (!ms) return;
    for (int i = 0; i < ms->n; i++) { free(ms->m[i].name); free(ms->m[i].args); }
    free(ms->m); ms->m = NULL; ms->n = 0;
}
const char *objcat_sql(const char *drv, int i, bool alt) {
    const char *const (*t)[2] = NULL; int n = 0;
    if (!drv || i < 0) return NULL;
    if (!strcmp(drv, "oracle")) { t = ORA; n = NQ(ORA); }
    else if (!strcmp(drv, "postgres")) { t = PG; n = NQ(PG); }
    else if (!strcmp(drv, "mysql") || !strcmp(drv, "mariadb")) { t = MY; n = NQ(MY); }
    else if (!strcmp(drv, "sqlserver") || !strcmp(drv, "mssql")) { t = MS; n = NQ(MS); }
    if (!t || i >= n) return NULL;
    return t[i][alt ? 1 : 0];
}

static const char *const KIND_WORD[OC_N] = { "TABLE", "VIEW", "MATERIALIZED VIEW", "SYNONYM", "PROCEDURE", "FUNCTION", "PACKAGE", "SEQUENCE", "TYPE" };
static const char *const KIND_NAME[OC_N] = { "table", "view", "materialized view", "synonym", "procedure", "function", "package", "sequence", "type" };
int objcat_kind(const char *w) {
    if (!w) return -1;
    for (int k = 0; k < OC_N; k++) if (!strcasecmp(w, KIND_WORD[k])) return k;
    return -1;
}
const char *objcat_kind_name(int k) { return k >= 0 && k < OC_N ? KIND_NAME[k] : "object"; }

/* ---- data ---- */

unsigned objcat_mask(const char *s, int n) {
    unsigned m = 0;
    for (int i = 0; i < n && s[i]; i++) {
        unsigned char ch = (unsigned char)tolower((unsigned char)s[i]);
        if (ch >= 'a' && ch <= 'z') m |= 1u << (ch - 'a');
        else if (ch >= '0' && ch <= '9') m |= 1u << 26;
        else if (ch == '_') m |= 1u << 27;
        else m |= 1u << 28;
    }
    return m;
}
static char *dup(const char *s) { return s ? strdup(s) : NULL; }
void objcat_add(ObjCat *c, const char *schema, const char *name, int kind, const char *target) {
    if (!name || !name[0] || kind < 0 || kind >= OC_N) return;
    if (c->n == c->cap) {
        int nc = c->cap ? c->cap * 2 : 1024;
        OcObj *no = realloc(c->o, sizeof *no * (size_t)nc); if (!no) return;
        c->o = no; c->cap = nc;
    }
    OcObj *o = &c->o[c->n++];
    o->schema = dup(schema ? schema : ""); o->name = dup(name); o->target = target && target[0] ? dup(target) : NULL; o->kind = (unsigned char)kind; o->mask = objcat_mask(name, 1 << 30);
}
static int cmp_obj(const void *a, const void *b) {
    const OcObj *x = a, *y = b;
    int r = strcasecmp(x->schema, y->schema); if (r) return r;
    r = strcmp(x->schema, y->schema); if (r) return r;
    r = strcmp(x->name, y->name); if (r) return r;
    /* a materialized view is also listed as its container table: the view first, kept */
    int px = x->kind == OC_MVIEW ? -1 : x->kind, py = y->kind == OC_MVIEW ? -1 : y->kind;
    return px - py;
}
static void obj_free(OcObj *o) { free(o->schema); free(o->name); free(o->target); }
void objcat_finish(ObjCat *c) {
    if (c->n) qsort(c->o, (size_t)c->n, sizeof *c->o, cmp_obj);
    int w = 0;
    for (int i = 0; i < c->n; i++) {
        if (w && !strcmp(c->o[w - 1].schema, c->o[i].schema) && !strcmp(c->o[w - 1].name, c->o[i].name)) { obj_free(&c->o[i]); continue; }   /* same object, another kind row (package + its type...) */
        c->o[w++] = c->o[i];
    }
    c->n = w;
    for (int i = 0; i < c->ns; i++) free(c->s[i].name);
    free(c->s); c->s = NULL; c->ns = 0;
    int cap = 0;
    for (int i = 0; i < c->n;) {
        int j = i; while (j < c->n && !strcmp(c->o[j].schema, c->o[i].schema)) j++;
        if (c->ns == cap) { cap = cap ? cap * 2 : 64; OcSchema *ns = realloc(c->s, sizeof *ns * (size_t)cap); if (!ns) break; c->s = ns; }
        c->s[c->ns].name = dup(c->o[i].schema); c->s[c->ns].first = i; c->s[c->ns].n = j - i; c->ns++;
        i = j;
    }
}
void objcat_free(ObjCat *c) {
    for (int i = 0; i < c->n; i++) obj_free(&c->o[i]);
    for (int i = 0; i < c->ns; i++) free(c->s[i].name);
    free(c->o); free(c->s);
    memset(c, 0, sizeof *c);
}
const OcSchema *objcat_schema(const ObjCat *c, const char *name, int n) {
    if (!c || n <= 0) return NULL;
    const OcSchema *ci = NULL;
    for (int i = 0; i < c->ns; i++) {
        if ((int)strlen(c->s[i].name) != n) continue;
        if (!strncmp(c->s[i].name, name, (size_t)n)) return &c->s[i];   /* exact case first (PostgreSQL "Sales" and "sales") */
        if (!ci && !strncasecmp(c->s[i].name, name, (size_t)n)) ci = &c->s[i];
    }
    return ci;
}

/* ---- disk cache ---- */

static bool clean(const char *s) { for (; s && *s; s++) if (*s == '\t' || *s == '\n' || *s == '\r') return false; return true; }
bool objcat_save(const ObjCat *c, const char *path) {
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w"); if (!f) return false;
    fprintf(f, "# lume objcat 1\nhome\t%s\n", clean(c->home) ? c->home : "");
    for (int i = 0; i < c->n; i++) {
        const OcObj *o = &c->o[i];
        if (!clean(o->schema) || !clean(o->name) || !clean(o->target)) continue;   /* a name with a tab or newline is not cached (it is still proposed this session) */
        fprintf(f, "%s\t%s\t%s\t%s\n", KIND_WORD[o->kind], o->schema, o->name, o->target ? o->target : "");
    }
    bool ok = !ferror(f);
    if (fclose(f) != 0) ok = false;
    if (ok) ok = rename(tmp, path) == 0; else remove(tmp);
    return ok;
}
bool objcat_load(ObjCat *c, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return false;
    char *line = NULL; size_t cap = 0; long len; bool first = true, ok = false;
    while ((len = (long)getline(&line, &cap, f)) > 0) {
        while (len > 0 && (line[len - 1] == '\n' || line[len - 1] == '\r')) line[--len] = 0;
        if (first) { first = false; if (strcmp(line, "# lume objcat 1")) break; ok = true; continue; }
        if (!strncmp(line, "home\t", 5)) { snprintf(c->home, sizeof c->home, "%s", line + 5); continue; }
        char *f1 = line, *f2 = strchr(f1, '\t'); if (!f2) continue; *f2++ = 0;
        char *f3 = strchr(f2, '\t'); if (!f3) continue; *f3++ = 0;
        char *f4 = strchr(f3, '\t'); if (f4) *f4++ = 0;
        objcat_add(c, f2, f3, objcat_kind(f1), f4);
    }
    free(line); fclose(f);
    if (ok) objcat_finish(c); else objcat_free(c);
    return ok;
}
