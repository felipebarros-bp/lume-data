/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer core: model, capabilities, identifiers, diff, validation, draft files.
 * See tbl.h. The planner is in tbl_plan.c, the catalog snapshots in tbl_snap.c.
 */
#include "tbl.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

int td_dialect_for_driver(const char *d) {
    if (!d) return -1;
    if (!strcmp(d, "sqlite")) return TD_SQLITE;
    if (!strcmp(d, "postgres")) return TD_PG;
    if (!strcmp(d, "oracle")) return TD_ORACLE;
    if (!strcmp(d, "mysql")) return TD_MYSQL;
    if (!strcmp(d, "mariadb")) return TD_MARIADB;
    if (!strcmp(d, "sqlserver")) return TD_MSSQL;
    return -1;
}
const char *td_dialect_name(int d) {
    static const char *n[] = { "SQLite", "PostgreSQL", "Oracle", "MySQL", "MariaDB", "SQL Server" };
    return d >= 0 && d < TD_NDIALECT ? n[d] : "?";
}

/* What each engine can do. Version 0 = unknown: features that depend on it are UNKNOWN, and the
 * planner refuses an UNKNOWN feature instead of guessing (TBL-080). */
TdCaps td_caps(int d, int v) {
    TdCaps c; memset(&c, 0, sizeof c);
    int Y = TCAP_YES, N = TCAP_NO, U = TCAP_UNKNOWN;
#define VER(min) (v == 0 ? U : v >= (min) ? Y : N)
    switch (d) {
    case TD_SQLITE:
        c.rename_column = VER(32500); c.drop_column = VER(33500); c.alter_type = N; c.comments = N; c.identity = Y; c.generated = VER(33100);
        c.generated_virtual = VER(33100); c.deferrable = Y; c.partial_index = Y; c.include_index = N; c.index_method = N; c.check = Y;
        c.transactional_ddl = Y; c.column_order = N; c.unlogged = N; c.temp = Y; c.engine = N; c.collation = Y; c.rename_constraint = N;
        c.rename_index = N; c.on_update_actions = Y; c.add_column_after = N; c.max_ident = 0;
        break;
    case TD_PG:
        c.rename_column = Y; c.drop_column = Y; c.alter_type = Y; c.comments = Y; c.identity = VER(100000); c.generated = VER(120000);
        c.generated_virtual = VER(180000); c.deferrable = Y; c.partial_index = Y; c.include_index = VER(110000); c.index_method = Y; c.check = Y;
        c.transactional_ddl = Y; c.column_order = N; c.unlogged = Y; c.temp = Y; c.engine = N; c.collation = Y; c.rename_constraint = Y;
        c.rename_index = Y; c.on_update_actions = Y; c.add_column_after = N; c.max_ident = 63;
        break;
    case TD_ORACLE:
        c.rename_column = Y; c.drop_column = Y; c.alter_type = Y; c.comments = Y; c.identity = VER(120000); c.generated = Y;
        c.generated_virtual = Y; c.deferrable = Y; c.partial_index = N; c.include_index = N; c.index_method = N; c.check = Y;
        c.transactional_ddl = N; c.column_order = N; c.unlogged = N; c.temp = Y; c.engine = N; c.collation = VER(120200); c.rename_constraint = Y;
        c.rename_index = Y; c.on_update_actions = N; c.add_column_after = N; c.max_ident = v == 0 ? 30 : v >= 120200 ? 128 : 30;
        break;
    case TD_MYSQL: case TD_MARIADB:
        c.rename_column = d == TD_MYSQL ? VER(80000) : VER(100500); c.drop_column = Y; c.alter_type = Y; c.comments = Y; c.identity = Y;
        c.generated = d == TD_MYSQL ? VER(50700) : VER(100200); c.generated_virtual = c.generated; c.deferrable = N; c.partial_index = N; c.include_index = N;
        c.index_method = N; c.check = d == TD_MYSQL ? VER(80016) : VER(100201); c.transactional_ddl = N; c.column_order = Y; c.unlogged = N; c.temp = Y;
        c.engine = Y; c.collation = Y; c.rename_constraint = N; c.rename_index = d == TD_MYSQL ? VER(50700) : VER(100500); c.on_update_actions = Y;
        c.add_column_after = Y; c.max_ident = 64;
        break;
    case TD_MSSQL:
        c.rename_column = Y; c.drop_column = Y; c.alter_type = Y; c.comments = Y; c.identity = Y; c.generated = Y; c.generated_virtual = Y;
        c.deferrable = N; c.partial_index = Y; c.include_index = Y; c.index_method = N; c.check = Y; c.transactional_ddl = Y; c.column_order = N;
        c.unlogged = N; c.temp = Y; c.engine = N; c.collation = Y; c.rename_constraint = Y; c.rename_index = Y; c.on_update_actions = Y;
        c.add_column_after = N; c.max_ident = 128;
        break;
    }
#undef VER
    return c;
}

const char *td_key_kind_name(int k) {
    static const char *n[] = { "Primary Key", "Unique", "Foreign Key", "Check", "Index" };
    return k >= 0 && k < TK_NKINDS ? n[k] : "?";
}

/* ---- model ---- */

void td_init(TdTable *t) { memset(t, 0, sizeof *t); t->next_id = 1; t->approx_rows = -1; t->can_alter = true; }
void td_free(TdTable *t) { free(t->cols); free(t->keys); memset(t, 0, sizeof *t); }
void td_copy(TdTable *d, const TdTable *s) {
    *d = *s;
    d->cols = s->ncols ? malloc(sizeof *d->cols * (size_t)s->ncols) : NULL; d->capcols = s->ncols;
    if (s->ncols) memcpy(d->cols, s->cols, sizeof *d->cols * (size_t)s->ncols);
    d->keys = s->nkeys ? malloc(sizeof *d->keys * (size_t)s->nkeys) : NULL; d->capkeys = s->nkeys;
    if (s->nkeys) memcpy(d->keys, s->keys, sizeof *d->keys * (size_t)s->nkeys);
}
bool td_col_same(const TdCol *a, const TdCol *b) {
    return a->id == b->id && !strcmp(a->name, b->name) && !strcasecmp(a->type, b->type) && a->notnull == b->notnull && !strcmp(a->def, b->def) && a->identity == b->identity &&
           !strcmp(a->gen, b->gen) && a->gen_stored == b->gen_stored && !strcmp(a->comment, b->comment) && !strcmp(a->collation, b->collation) && !strcmp(a->extra, b->extra);
}
bool td_key_same(const TdKey *a, const TdKey *b, bool with_name) {
    if (a->kind != b->kind || a->ncols != b->ncols || a->unique != b->unique || strcmp(a->method, b->method) || strcmp(a->where, b->where) || a->ninclude != b->ninclude ||
        strcmp(a->ref_schema, b->ref_schema) || strcmp(a->ref_table, b->ref_table) || a->nref != b->nref || strcmp(a->on_delete, b->on_delete) || strcmp(a->on_update, b->on_update) ||
        a->deferrable != b->deferrable || a->deferred != b->deferred || strcmp(a->check, b->check) || strcmp(a->raw, b->raw) || (with_name && strcmp(a->name, b->name)))
        return false;
    for (int i = 0; i < a->ncols; i++) if (a->cols[i] != b->cols[i] || a->desc[i] != b->desc[i]) return false;
    for (int i = 0; i < a->ninclude; i++) if (a->include[i] != b->include[i]) return false;
    for (int i = 0; i < a->nref; i++) if (strcmp(a->ref_cols[i], b->ref_cols[i])) return false;
    return true;
}
bool td_equal(const TdTable *a, const TdTable *b) {
    if (strcmp(a->schema, b->schema) || strcmp(a->name, b->name) || strcmp(a->comment, b->comment) || a->ncols != b->ncols || a->nkeys != b->nkeys ||
        a->temporary != b->temporary || a->unlogged != b->unlogged || strcmp(a->engine, b->engine) || strcmp(a->charset, b->charset) || strcmp(a->collation, b->collation))
        return false;
    for (int i = 0; i < a->ncols; i++) if (!td_col_same(&a->cols[i], &b->cols[i])) return false;
    for (int i = 0; i < a->nkeys; i++) if (a->keys[i].id != b->keys[i].id || !td_key_same(&a->keys[i], &b->keys[i], true)) return false;
    return true;
}
TdCol *td_col(TdTable *t, int id) { for (int i = 0; i < t->ncols; i++) if (t->cols[i].id == id) return &t->cols[i]; return NULL; }
const TdCol *td_col_c(const TdTable *t, int id) { for (int i = 0; i < t->ncols; i++) if (t->cols[i].id == id) return &t->cols[i]; return NULL; }
TdKey *td_key(TdTable *t, int id) { for (int i = 0; i < t->nkeys; i++) if (t->keys[i].id == id) return &t->keys[i]; return NULL; }
int td_col_index(const TdTable *t, int id) { for (int i = 0; i < t->ncols; i++) if (t->cols[i].id == id) return i; return -1; }
TdCol *td_add_col(TdTable *t, const char *name, const char *type) {
    if (t->ncols == t->capcols) { t->capcols = t->capcols ? t->capcols * 2 : 16; t->cols = realloc(t->cols, sizeof *t->cols * (size_t)t->capcols); }
    TdCol *c = &t->cols[t->ncols++];
    memset(c, 0, sizeof *c);
    c->id = t->next_id++;
    snprintf(c->name, sizeof c->name, "%s", name ? name : "");
    snprintf(c->type, sizeof c->type, "%s", type ? type : "");
    return c;
}
TdKey *td_add_key(TdTable *t, int kind, const char *name) {
    if (t->nkeys == t->capkeys) { t->capkeys = t->capkeys ? t->capkeys * 2 : 8; t->keys = realloc(t->keys, sizeof *t->keys * (size_t)t->capkeys); }
    TdKey *k = &t->keys[t->nkeys++];
    memset(k, 0, sizeof *k);
    k->id = t->next_id++; k->kind = kind;
    snprintf(k->name, sizeof k->name, "%s", name ? name : "");
    return k;
}
void td_del_key(TdTable *t, int id) {
    for (int i = 0; i < t->nkeys; i++) if (t->keys[i].id == id) { memmove(t->keys + i, t->keys + i + 1, sizeof *t->keys * (size_t)(t->nkeys - i - 1)); t->nkeys--; return; }
}
void td_del_col(TdTable *t, int id) {
    int i = td_col_index(t, id);
    if (i < 0) return;
    memmove(t->cols + i, t->cols + i + 1, sizeof *t->cols * (size_t)(t->ncols - i - 1)); t->ncols--;
    for (int k = 0; k < t->nkeys; ) {
        TdKey *K = &t->keys[k];
        int o = 0; for (int j = 0; j < K->ncols; j++) if (K->cols[j] != id) { K->cols[o] = K->cols[j]; K->desc[o] = K->desc[j]; if (K->kind == TK_FK && j < K->nref) snprintf(K->ref_cols[o], sizeof K->ref_cols[o], "%s", K->ref_cols[j]); o++; }
        if (K->kind == TK_FK) K->nref = o;
        K->ncols = o;
        int oi = 0; for (int j = 0; j < K->ninclude; j++) if (K->include[j] != id) K->include[oi++] = K->include[j];
        K->ninclude = oi;
        if (!K->ncols && K->kind != TK_CHECK && !K->raw[0]) td_del_key(t, K->id); else k++;
    }
}
bool td_move_col(TdTable *t, int id, int delta) {
    int i = td_col_index(t, id), j = i + delta;
    if (i < 0 || j < 0 || j >= t->ncols) return false;
    TdCol c = t->cols[i]; t->cols[i] = t->cols[j]; t->cols[j] = c;
    return true;
}
TdKey *td_pk(TdTable *t) { for (int i = 0; i < t->nkeys; i++) if (t->keys[i].kind == TK_PK) return &t->keys[i]; return NULL; }
bool td_col_in_kind(const TdTable *t, int col, int kind) {
    for (int i = 0; i < t->nkeys; i++) { if (t->keys[i].kind != kind) continue; for (int j = 0; j < t->keys[i].ncols; j++) if (t->keys[i].cols[j] == col) return true; }
    return false;
}
const char *td_default_type(int d) {
    switch (d) { case TD_SQLITE: return "TEXT"; case TD_PG: return "text"; case TD_ORACLE: return "VARCHAR2(255)"; case TD_MSSQL: return "nvarchar(255)"; default: return "varchar(255)"; }
}
const char *const *td_types(int d) {
    static const char *sq[] = { "INTEGER", "TEXT", "REAL", "NUMERIC", "BLOB", "BOOLEAN", "DATE", "DATETIME", NULL };
    static const char *pg[] = { "integer", "bigint", "smallint", "numeric(12,2)", "real", "double precision", "text", "varchar(255)", "char(1)", "boolean",
                                "date", "timestamp", "timestamptz", "time", "interval", "uuid", "json", "jsonb", "bytea", "inet", "serial", "bigserial", NULL };
    static const char *ora[] = { "NUMBER", "NUMBER(10)", "NUMBER(12,2)", "VARCHAR2(255)", "NVARCHAR2(255)", "CHAR(1)", "CLOB", "BLOB", "DATE", "TIMESTAMP",
                                 "TIMESTAMP WITH TIME ZONE", "RAW(16)", "BINARY_DOUBLE", "FLOAT", NULL };
    static const char *my[] = { "int", "bigint", "smallint", "tinyint", "decimal(12,2)", "double", "float", "varchar(255)", "char(1)", "text", "longtext",
                                "date", "datetime", "timestamp", "time", "json", "blob", "binary(16)", "enum('a','b')", NULL };
    static const char *ms[] = { "int", "bigint", "smallint", "tinyint", "bit", "decimal(12,2)", "float", "real", "nvarchar(255)", "varchar(255)", "nvarchar(max)",
                                "char(1)", "date", "datetime2", "datetimeoffset", "time", "uniqueidentifier", "varbinary(max)", "money", NULL };
    switch (d) { case TD_SQLITE: return sq; case TD_PG: return pg; case TD_ORACLE: return ora; case TD_MSSQL: return ms; default: return my; }
}
void td_unique_col_name(const TdTable *t, const char *base, char *out, int n) {
    for (int k = 1; k < 1000; k++) {
        if (k == 1) snprintf(out, (size_t)n, "%s", base); else snprintf(out, (size_t)n, "%s_%d", base, k);
        bool used = false; for (int i = 0; i < t->ncols && !used; i++) used = !strcasecmp(t->cols[i].name, out);
        if (!used) return;
    }
}
void td_key_name(const TdTable *t, const TdKey *k, const char *pattern, char *out, int n) {
    static const char *sfx[] = { "pk", "key", "fkey", "check", "idx" };
    char cols[200] = ""; int o = 0;
    for (int i = 0; i < k->ncols && o < (int)sizeof cols - 2; i++) { const TdCol *c = td_col_c(t, k->cols[i]); if (c) o += snprintf(cols + o, sizeof cols - (size_t)o, "%s%s", o ? "_" : "", c->name); }
    const char *p = pattern && pattern[0] ? pattern : k->kind == TK_PK ? "{table}_pkey" : "{table}_{cols}_{kind}";
    int w = 0;
    for (; *p && w < n - 1; p++) {
        const char *rep = NULL;
        if (!strncmp(p, "{table}", 7)) { rep = t->name; p += 6; }
        else if (!strncmp(p, "{cols}", 6)) { rep = cols[0] ? cols : "x"; p += 5; }
        else if (!strncmp(p, "{kind}", 6)) { rep = sfx[k->kind]; p += 5; }
        if (rep) w += snprintf(out + w, (size_t)(n - w), "%s", rep); else out[w++] = *p;
        if (w > n - 1) w = n - 1;
    }
    out[w] = 0;
}

/* ---- identifiers and literals (TBL-065.3: never concatenated unquoted) ---- */

void td_quote(int d, const char *s, char *out, int n) {
    char open = '"', close = '"';
    if (d == TD_MYSQL || d == TD_MARIADB) open = close = '`';
    else if (d == TD_MSSQL) { open = '['; close = ']'; }
    int o = 0;
    if (o < n - 1) out[o++] = open;
    for (; *s && o < n - 3; s++) { if (*s == close) out[o++] = close; out[o++] = *s; }
    out[o++] = close; out[o] = 0;
}
void td_qname(int d, const char *schema, const char *name, char *out, int n) {
    char a[300], b[300];
    td_quote(d, name, b, sizeof b);
    if (schema && schema[0]) { td_quote(d, schema, a, sizeof a); snprintf(out, (size_t)n, "%s.%s", a, b); }
    else snprintf(out, (size_t)n, "%s", b);
}
void td_lit(int d, const char *s, char *out, int n) {
    int o = 0;
    if (d == TD_MSSQL && o < n - 1) out[o++] = 'N';
    if (o < n - 1) out[o++] = '\'';
    for (; *s && o < n - 4; s++) {
        if (*s == '\'') out[o++] = '\'';
        if (*s == '\\' && (d == TD_MYSQL || d == TD_MARIADB)) out[o++] = '\\';
        out[o++] = *s;
    }
    out[o++] = '\''; out[o] = 0;
}

/* ---- diff (TBL-051): by stable id ---- */

static void chg(TdChange *out, int max, int *n, int kind, int id, const char *fmt, const char *a, const char *b, const char *c) {
    if (*n >= max) { (*n)++; return; }
    out[*n].kind = kind; out[*n].id = id;
    snprintf(out[*n].text, sizeof out[*n].text, fmt, a ? a : "", b ? b : "", c ? c : "");
    (*n)++;
}
int td_diff(const TdTable *o, const TdTable *d, TdChange *out, int max) {
    int n = 0;
    char q[64];
    if (!o || !o->exists) {
        snprintf(q, sizeof q, "%d", d->ncols);
        chg(out, max, &n, TC_TABLE, 0, "[+] Create table %s with %s column(s)", d->name, q, NULL);
        for (int i = 0; i < d->nkeys; i++) chg(out, max, &n, TC_ADD_KEY, d->keys[i].id, "[+] %s %s", td_key_kind_name(d->keys[i].kind), d->keys[i].name[0] ? d->keys[i].name : "(named by the engine)", NULL);
        return n;
    }
    if (strcmp(o->name, d->name)) chg(out, max, &n, TC_RENAME_TABLE, 0, "[~] Rename table %s -> %s", o->name, d->name, NULL);
    if (strcmp(o->comment, d->comment)) chg(out, max, &n, TC_TABLE, 0, "[~] Table comment changed%s%s", "", "", NULL);
    if (strcmp(o->engine, d->engine) || strcmp(o->charset, d->charset) || strcmp(o->collation, d->collation)) chg(out, max, &n, TC_TABLE, 0, "[~] Table options: engine %s, charset %s, collation %s", d->engine, d->charset, d->collation);
    for (int i = 0; i < o->ncols; i++) {
        const TdCol *a = &o->cols[i], *b = td_col_c(d, a->id);
        if (!b) { chg(out, max, &n, TC_DROP_COL, a->id, "[-] Drop column %s (%s)", a->name, a->type, NULL); continue; }
        if (strcmp(a->name, b->name)) chg(out, max, &n, TC_RENAME_COL, a->id, "[~] Rename column %s -> %s", a->name, b->name, NULL);
        if (strcasecmp(a->type, b->type)) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] Change %s: %s -> %s", b->name, a->type, b->type);
        if (a->notnull != b->notnull) chg(out, max, &n, TC_ALTER_COL, a->id, b->notnull ? "[~] %s: NOT NULL%s%s" : "[~] %s: allow NULL%s%s", b->name, "", "");
        if (strcmp(a->def, b->def)) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] %s: default %s -> %s", b->name, a->def[0] ? a->def : "(none)", b->def[0] ? b->def : "(none)");
        if (a->identity != b->identity) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] %s: identity %s%s", b->name, b->identity ? "added" : "removed", "");
        if (strcmp(a->gen, b->gen) || a->gen_stored != b->gen_stored) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] %s: generated expression changed%s%s", b->name, "", "");
        if (strcmp(a->comment, b->comment)) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] %s: comment changed%s%s", b->name, "", "");
        if (strcmp(a->collation, b->collation)) chg(out, max, &n, TC_ALTER_COL, a->id, "[~] %s: collation %s -> %s", b->name, a->collation[0] ? a->collation : "(default)", b->collation[0] ? b->collation : "(default)");
    }
    /* order of existing columns */
    int oi = 0; bool reorder = false;
    for (int i = 0; i < d->ncols; i++) { if (!td_col_c(o, d->cols[i].id)) continue; while (oi < o->ncols && !td_col_c(d, o->cols[oi].id)) oi++; if (oi < o->ncols && o->cols[oi].id != d->cols[i].id) reorder = true; oi++; }
    if (reorder) chg(out, max, &n, TC_REORDER, 0, "[~] Column order of existing columns changed%s%s%s", "", "", "");
    for (int i = 0; i < d->ncols; i++) if (!td_col_c(o, d->cols[i].id)) chg(out, max, &n, TC_ADD_COL, d->cols[i].id, "[+] Add column %s %s", d->cols[i].name, d->cols[i].type, NULL);
    for (int i = 0; i < o->nkeys; i++) {
        const TdKey *a = &o->keys[i]; const TdKey *b = NULL;
        for (int j = 0; j < d->nkeys; j++) if (d->keys[j].id == a->id) b = &d->keys[j];
        if (!b) { chg(out, max, &n, TC_DROP_KEY, a->id, "[-] Drop %s %s", td_key_kind_name(a->kind), a->name, NULL); continue; }
        if (!td_key_same(a, b, false)) chg(out, max, &n, TC_ALTER_KEY, a->id, "[~] Change %s %s", td_key_kind_name(a->kind), b->name, NULL);
        else if (strcmp(a->name, b->name)) chg(out, max, &n, TC_RENAME_KEY, a->id, "[~] Rename %s %s -> %s", td_key_kind_name(a->kind), a->name, b->name);
    }
    for (int j = 0; j < d->nkeys; j++) {
        bool found = false; for (int i = 0; i < o->nkeys && !found; i++) found = o->keys[i].id == d->keys[j].id;
        if (!found) chg(out, max, &n, TC_ADD_KEY, d->keys[j].id, "[+] Add %s %s", td_key_kind_name(d->keys[j].kind), d->keys[j].name[0] ? d->keys[j].name : "(named by the engine)", NULL);
    }
    return n;
}

/* ---- validation (TBL-065 / 066 / 069) ---- */

static void issue(TdIssue *out, int max, int *n, int level, int col, int key, const char *field, const char *fmt, const char *a, const char *b) {
    if (*n >= max) { (*n)++; return; }
    out[*n].level = level; out[*n].col_id = col; out[*n].key_id = key;
    snprintf(out[*n].field, sizeof out[*n].field, "%s", field);
    snprintf(out[*n].text, sizeof out[*n].text, fmt, a ? a : "", b ? b : "");
    (*n)++;
}
/* quotes / parentheses balanced, no statement separator outside quotes: a static check of an
 * expression the user typed (DEFAULT, CHECK, generated, WHERE) */
static const char *expr_problem(const char *e) {
    int depth = 0; char q = 0;
    for (const char *p = e; *p; p++) {
        if (q) { if (*p == q) { if (p[1] == q) p++; else q = 0; } continue; }
        if (*p == '\'' || *p == '"') q = *p;
        else if (*p == '(') depth++;
        else if (*p == ')') { if (--depth < 0) return "unbalanced parentheses"; }
        else if (*p == ';') return "a ';' would end the statement";
        else if (p[0] == '-' && p[1] == '-') return "a '--' comment would hide the rest of the statement";
    }
    if (q) return "an unterminated quote";
    if (depth) return "unbalanced parentheses";
    return NULL;
}
/* numeric(p,s) / varchar(n): base type and up to two numbers */
static void type_parts(const char *t, char *base, int bn, long *a, long *b) {
    *a = *b = -1;
    const char *p = strchr(t, '(');
    int len = p ? (int)(p - t) : (int)strlen(t);
    while (len > 0 && t[len - 1] == ' ') len--;
    snprintf(base, (size_t)bn, "%.*s", len, t);
    for (char *x = base; *x; x++) *x = (char)tolower((unsigned char)*x);
    if (p) { if (!strncasecmp(p + 1, "max", 3)) *a = 1L << 30; else { *a = strtol(p + 1, (char **)&p, 10); if (*p == ',') *b = strtol(p + 1, NULL, 10); } }
}
static bool is_text(const char *b) { return strstr(b, "char") || strstr(b, "text") || !strcmp(b, "clob") || strstr(b, "string"); }
static bool is_num(const char *b) { return strstr(b, "int") || !strcmp(b, "numeric") || !strcmp(b, "decimal") || !strcmp(b, "number") || !strcmp(b, "real") || strstr(b, "float") || strstr(b, "double") || !strcmp(b, "money"); }
static int int_rank(const char *b) {
    if (!strcmp(b, "tinyint")) return 1; if (!strcmp(b, "smallint") || !strcmp(b, "int2")) return 2; if (!strcmp(b, "mediumint")) return 3;
    if (!strcmp(b, "int") || !strcmp(b, "integer") || !strcmp(b, "int4")) return 4; if (!strcmp(b, "bigint") || !strcmp(b, "int8")) return 5; return 0;
}
/* narrowing / incompatible conversion (TBL-069): "" when it widens or keeps */
void td_type_risk(const char *from, const char *to, char *out, int n) {
    out[0] = 0;
    char fb[64], tb[64]; long fa, fs, ta, ts;
    type_parts(from, fb, sizeof fb, &fa, &fs); type_parts(to, tb, sizeof tb, &ta, &ts);
    if (!strcasecmp(from, to)) return;
    if (is_text(fb) && is_text(tb)) { if (fa > 0 && ta > 0 && ta < fa) snprintf(out, (size_t)n, "shorter text (%ld -> %ld): longer values fail or are truncated", fa, ta); else if (fa < 0 && ta > 0) snprintf(out, (size_t)n, "unbounded text to length %ld: longer values fail or are truncated", ta); return; }
    if (is_num(fb) && is_num(tb)) {
        int r1 = int_rank(fb), r2 = int_rank(tb);
        if (r1 && r2) { if (r2 < r1) snprintf(out, (size_t)n, "smaller integer type (%s -> %s): out-of-range values fail", fb, tb); return; }
        if (fa > 0 && ta > 0 && (ta - (ts > 0 ? ts : 0) < fa - (fs > 0 ? fs : 0) || (ts >= 0 ? ts : 0) < (fs >= 0 ? fs : 0))) { snprintf(out, (size_t)n, "less precision or scale (%s -> %s): values may fail or be rounded", from, to); return; }
        if (r1 && !r2) return;
        if (!r1 && r2) { snprintf(out, (size_t)n, "decimal to integer (%s -> %s): fractions are lost", from, to); return; }
        return;
    }
    snprintf(out, (size_t)n, "conversion %s -> %s: existing values may not convert", from, to);
}
static const char *tb_of(const char *t) { static char b[64]; long x, y; type_parts(t, b, sizeof b, &x, &y); return b; }
static bool name_eq(int d, const char *a, const char *b) { return d == TD_PG || d == TD_SQLITE ? !strcmp(a, b) || (d == TD_SQLITE && !strcasecmp(a, b)) : !strcasecmp(a, b); }

int td_validate(int d, int ver, const TdTable *o, const TdTable *t, TdIssue *out, int max) {
    int n = 0;
    TdCaps caps = td_caps(d, ver);
    char tmp[64];
    if (!t->name[0]) issue(out, max, &n, TV_ERROR, 0, 0, "table", "The table needs a name%s%s", NULL, NULL);
    if (caps.max_ident && (int)strlen(t->name) > caps.max_ident) { snprintf(tmp, sizeof tmp, "%d", caps.max_ident); issue(out, max, &n, TV_ERROR, 0, 0, "table", "Table name longer than %s characters (%s limit)", tmp, td_dialect_name(d)); }
    if (!t->ncols) issue(out, max, &n, TV_ERROR, 0, 0, "columns", "A table needs at least one column%s%s", NULL, NULL);
    if (t->special[0]) issue(out, max, &n, TV_ERROR, 0, 0, "table", "Read-only: %s%s", t->special, NULL);
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *c = &t->cols[i];
        const TdCol *oc = o ? td_col_c(o, c->id) : NULL;
        if (!c->name[0]) issue(out, max, &n, TV_ERROR, c->id, 0, "name", "Column %s has no name%s", c->type, NULL);
        else if (caps.max_ident && (int)strlen(c->name) > caps.max_ident) { snprintf(tmp, sizeof tmp, "%d", caps.max_ident); issue(out, max, &n, TV_ERROR, c->id, 0, "name", "Column name %s is longer than %s characters", c->name, tmp); }
        for (int j = 0; j < i; j++) if (c->name[0] && name_eq(d, t->cols[j].name, c->name)) issue(out, max, &n, TV_ERROR, c->id, 0, "name", "Two columns are named %s%s", c->name, NULL);
        if (!c->type[0] && !c->gen[0]) issue(out, max, &n, TV_ERROR, c->id, 0, "type", "Column %s has no data type%s", c->name, NULL);
        const char *ep;
        if (c->def[0] && (ep = expr_problem(c->def))) issue(out, max, &n, TV_ERROR, c->id, 0, "default", "Default of %s: %s", c->name, ep);
        if (c->gen[0] && (ep = expr_problem(c->gen))) issue(out, max, &n, TV_ERROR, c->id, 0, "generated", "Expression of %s: %s", c->name, ep);
        if (c->gen[0] && c->def[0]) issue(out, max, &n, TV_ERROR, c->id, 0, "default", "%s is generated: it cannot also have a default%s", c->name, NULL);
        if (c->gen[0] && caps.generated == TCAP_NO) issue(out, max, &n, TV_ERROR, c->id, 0, "generated", "%s does not support generated columns%s", td_dialect_name(d), NULL);
        if (c->gen[0] && !c->gen_stored && caps.generated_virtual == TCAP_NO) issue(out, max, &n, TV_ERROR, c->id, 0, "generated", "%s supports STORED generated columns only%s", td_dialect_name(d), NULL);
        if (c->identity) {
            char b[64]; long x, y; type_parts(c->type, b, sizeof b, &x, &y);
            if (!int_rank(b) && strcmp(b, "number") && strcmp(b, "numeric") && strcmp(b, "decimal")) issue(out, max, &n, TV_ERROR, c->id, 0, "identity", "Identity needs an integer type (%s is %s)", c->name, c->type);
            if (c->def[0]) issue(out, max, &n, TV_ERROR, c->id, 0, "identity", "%s is an identity: it cannot also have a default (identity is not a sequence default)%s", c->name, NULL);
            if (caps.identity == TCAP_NO) issue(out, max, &n, TV_ERROR, c->id, 0, "identity", "This %s version has no identity columns%s", td_dialect_name(d), NULL);
        }
        if (c->comment[0] && caps.comments == TCAP_NO) issue(out, max, &n, TV_WARNING, c->id, 0, "comment", "%s has no column comments: the comment of %s is not saved", td_dialect_name(d), c->name);
        if (c->collation[0] && caps.collation == TCAP_NO) issue(out, max, &n, TV_ERROR, c->id, 0, "collation", "Collations are not supported here%s%s", NULL, NULL);
        if (oc) {
            char risk[200]; td_type_risk(oc->type, c->type, risk, sizeof risk);
            if (risk[0]) issue(out, max, &n, TV_WARNING, c->id, 0, "type", "%s: %s", c->name, risk);
            if (strcasecmp(oc->type, c->type) && c->def[0] && !strcmp(c->def, oc->def) && c->def[0] == '\'' && !is_text(tb_of(c->type)))
                issue(out, max, &n, TV_WARNING, c->id, 0, "default", "%s: the default %s is text and may not fit the new type", c->name, c->def);
            if (c->notnull && !oc->notnull) issue(out, max, &n, TV_WARNING, c->id, 0, "notnull", "%s becomes NOT NULL: the change fails if a row has NULL (optional check: Check existing data)%s", c->name, NULL);
            if (oc->extra[0] && strcasecmp(oc->type, c->type)) issue(out, max, &n, TV_WARNING, c->id, 0, "type", "%s has attributes Lume does not model (%s): check the generated statement", c->name, oc->extra);
        }
    }
    int npk = 0;
    for (int i = 0; i < t->nkeys; i++) {
        const TdKey *k = &t->keys[i];
        if (k->raw[0]) continue;   /* not modelled: preserved as is */
        if (k->kind == TK_PK) npk++;
        if (k->name[0] && caps.max_ident && (int)strlen(k->name) > caps.max_ident) { snprintf(tmp, sizeof tmp, "%d", caps.max_ident); issue(out, max, &n, TV_ERROR, 0, k->id, "name", "Name %s is longer than %s characters", k->name, tmp); }
        for (int j = 0; j < i; j++) if (k->name[0] && t->keys[j].name[0] && name_eq(d, t->keys[j].name, k->name) && !(t->keys[j].kind == TK_INDEX && t->keys[j].backs_constraint))
            issue(out, max, &n, TV_ERROR, 0, k->id, "name", "Two constraints / indexes are named %s%s", k->name, NULL);
        if (k->kind != TK_CHECK && !k->ncols) issue(out, max, &n, TV_ERROR, 0, k->id, "columns", "%s %s has no column", td_key_kind_name(k->kind), k->name);
        if (k->kind == TK_PK)
            for (int j = 0; j < k->ncols; j++) { const TdCol *c = td_col_c(t, k->cols[j]); if (c && !c->notnull && d != TD_SQLITE) issue(out, max, &n, TV_INFO, c->id, k->id, "notnull", "%s is in the primary key: it will be NOT NULL%s", c->name, NULL); }
        if (k->kind == TK_FK) {
            if (!k->ref_table[0]) issue(out, max, &n, TV_ERROR, 0, k->id, "ref", "Foreign key %s: choose the referenced table%s", k->name, NULL);
            if (k->nref != k->ncols) issue(out, max, &n, TV_ERROR, 0, k->id, "ref", "Foreign key %s: as many referenced columns as columns are needed%s", k->name, NULL);
            for (int j = 0; j < k->nref; j++) if (!k->ref_cols[j][0]) issue(out, max, &n, TV_ERROR, 0, k->id, "ref", "Foreign key %s: a referenced column is empty%s", k->name, NULL);
            if ((k->deferrable || k->deferred) && caps.deferrable == TCAP_NO) issue(out, max, &n, TV_ERROR, 0, k->id, "deferrable", "%s has no deferrable constraints%s", td_dialect_name(d), NULL);
            if (k->on_update[0] && strcmp(k->on_update, "NO ACTION") && caps.on_update_actions == TCAP_NO) issue(out, max, &n, TV_ERROR, 0, k->id, "on_update", "%s has no ON UPDATE actions%s", td_dialect_name(d), NULL);
            if (!strcmp(k->on_delete, "SET NULL")) for (int j = 0; j < k->ncols; j++) { const TdCol *c = td_col_c(t, k->cols[j]); if (c && c->notnull) issue(out, max, &n, TV_ERROR, c->id, k->id, "on_delete", "ON DELETE SET NULL but %s is NOT NULL%s", c->name, NULL); }
            if (!strcmp(k->ref_table, t->name) && !strcmp(k->ref_schema, t->schema))
                for (int j = 0; j < k->nref; j++) { bool f = false; for (int c = 0; c < t->ncols; c++) f |= !strcmp(t->cols[c].name, k->ref_cols[j]); if (!f) issue(out, max, &n, TV_ERROR, 0, k->id, "ref", "Foreign key %s: column %s is not in this table", k->name, k->ref_cols[j]); }
        }
        if (k->kind == TK_CHECK) {
            const char *ep;
            if (!k->check[0]) issue(out, max, &n, TV_ERROR, 0, k->id, "check", "Check %s has no expression%s", k->name, NULL);
            else if ((ep = expr_problem(k->check))) issue(out, max, &n, TV_ERROR, 0, k->id, "check", "Check %s: %s", k->name, ep);
            if (caps.check == TCAP_NO) issue(out, max, &n, TV_ERROR, 0, k->id, "check", "This %s version ignores CHECK constraints%s", td_dialect_name(d), NULL);
            if (o && o->exists) { bool was = false; for (int j = 0; j < o->nkeys; j++) was |= o->keys[j].id == k->id; if (!was) issue(out, max, &n, TV_WARNING, 0, k->id, "check", "New check %s is verified against every existing row when added%s", k->name, NULL); }
        }
        if (k->kind == TK_INDEX) {
            const char *ep;
            if (k->where[0] && caps.partial_index == TCAP_NO) issue(out, max, &n, TV_ERROR, 0, k->id, "where", "%s has no partial / filtered indexes%s", td_dialect_name(d), NULL);
            if (k->where[0] && (ep = expr_problem(k->where))) issue(out, max, &n, TV_ERROR, 0, k->id, "where", "Condition of %s: %s", k->name, ep);
            if (k->ninclude && caps.include_index != TCAP_YES) issue(out, max, &n, TV_ERROR, 0, k->id, "include", "INCLUDE columns are not supported%s%s", caps.include_index == TCAP_UNKNOWN ? " (server version unknown)" : "", NULL);
            if (k->method[0] && caps.index_method != TCAP_YES) issue(out, max, &n, TV_ERROR, 0, k->id, "method", "Index methods are not supported on %s%s", td_dialect_name(d), NULL);
            /* the same columns as another index / the PK: redundant (TBL-026.2 / 033.3) */
            for (int j = 0; j < t->nkeys; j++) {
                const TdKey *x = &t->keys[j];
                if (x->id == k->id || x->ncols != k->ncols || (x->kind != TK_INDEX && x->kind != TK_PK && x->kind != TK_UNIQUE) || x->where[0] || k->where[0]) continue;
                if (!memcmp(x->cols, k->cols, sizeof k->cols[0] * (size_t)k->ncols) && (x->kind != TK_INDEX || j < i)) { issue(out, max, &n, TV_WARNING, 0, k->id, "columns", "Index %s repeats the columns of %s", k->name, x->name[0] ? x->name : td_key_kind_name(x->kind)); break; }
            }
        }
        if (k->kind == TK_UNIQUE && o && o->exists) { bool was = false; for (int j = 0; j < o->nkeys; j++) was |= o->keys[j].id == k->id; if (!was) issue(out, max, &n, TV_WARNING, 0, k->id, "columns", "New unique %s fails if existing rows have duplicates (optional check: Check existing data)%s", k->name, NULL); }
    }
    if (npk > 1) issue(out, max, &n, TV_ERROR, 0, 0, "pk", "Only one primary key per table%s%s", NULL, NULL);
    if (o && o->exists && strcmp(o->schema, t->schema)) issue(out, max, &n, TV_ERROR, 0, 0, "schema", "Moving a table to another schema is not part of this designer%s%s", NULL, NULL);
    return n;
}

/* ---- draft files (TBL-009) ---- */

static void esc(char **buf, size_t *len, size_t *cap, const char *key, const char *v) {
    size_t need = strlen(key) + strlen(v) * 2 + 4;
    if (*len + need >= *cap) { *cap = (*cap + need) * 2; *buf = realloc(*buf, *cap); }
    *len += (size_t)snprintf(*buf + *len, *cap - *len, "%s=", key);
    for (const char *p = v; *p; p++) {
        if (*p == '\n') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = 'n'; }
        else if (*p == '\\') { (*buf)[(*len)++] = '\\'; (*buf)[(*len)++] = '\\'; }
        else (*buf)[(*len)++] = *p;
    }
    (*buf)[(*len)++] = '\n'; (*buf)[*len] = 0;
}
static void unesc(const char *v, char *out, int n) {
    int o = 0;
    for (; *v && o < n - 1; v++) { if (*v == '\\' && v[1] == 'n') { out[o++] = '\n'; v++; } else if (*v == '\\' && v[1] == '\\') { out[o++] = '\\'; v++; } else out[o++] = *v; }
    out[o] = 0;
}
char *td_serialize(const TdTable *t) {
    size_t cap = 4096, len = 0; char *b = malloc(cap); b[0] = 0;
    char num[64];
    esc(&b, &len, &cap, "lume_table_draft", "1");
    esc(&b, &len, &cap, "schema", t->schema); esc(&b, &len, &cap, "name", t->name); esc(&b, &len, &cap, "comment", t->comment);
    snprintf(num, sizeof num, "%d %d %d %d", t->exists, t->temporary, t->unlogged, t->next_id); esc(&b, &len, &cap, "flags", num);
    esc(&b, &len, &cap, "engine", t->engine); esc(&b, &len, &cap, "charset", t->charset); esc(&b, &len, &cap, "collation", t->collation);
    snprintf(num, sizeof num, "%016llx", (unsigned long long)t->fingerprint); esc(&b, &len, &cap, "fingerprint", num);
    for (int i = 0; i < t->ncols; i++) {
        const TdCol *c = &t->cols[i];
        snprintf(num, sizeof num, "%d %d %d %d", c->id, c->notnull, c->identity, c->gen_stored); esc(&b, &len, &cap, "col", num);
        esc(&b, &len, &cap, ".name", c->name); esc(&b, &len, &cap, ".type", c->type); esc(&b, &len, &cap, ".def", c->def);
        esc(&b, &len, &cap, ".gen", c->gen); esc(&b, &len, &cap, ".comment", c->comment); esc(&b, &len, &cap, ".collation", c->collation);
        esc(&b, &len, &cap, ".def_name", c->def_name); esc(&b, &len, &cap, ".extra", c->extra);
    }
    for (int i = 0; i < t->nkeys; i++) {
        const TdKey *k = &t->keys[i];
        snprintf(num, sizeof num, "%d %d %d %d %d %d", k->id, k->kind, k->unique, k->deferrable, k->deferred, k->backs_constraint); esc(&b, &len, &cap, "key", num);
        esc(&b, &len, &cap, ".name", k->name);
        char list[600]; int o = 0; list[0] = 0;
        for (int j = 0; j < k->ncols; j++) o += snprintf(list + o, sizeof list - (size_t)o, "%s%d%s", j ? "," : "", k->cols[j], k->desc[j] ? "d" : "");
        esc(&b, &len, &cap, ".cols", list);
        o = 0; list[0] = 0; for (int j = 0; j < k->ninclude; j++) o += snprintf(list + o, sizeof list - (size_t)o, "%s%d", j ? "," : "", k->include[j]);
        esc(&b, &len, &cap, ".include", list);
        esc(&b, &len, &cap, ".method", k->method); esc(&b, &len, &cap, ".where", k->where);
        esc(&b, &len, &cap, ".ref_schema", k->ref_schema); esc(&b, &len, &cap, ".ref_table", k->ref_table);
        for (int j = 0; j < k->nref; j++) esc(&b, &len, &cap, ".ref_col", k->ref_cols[j]);
        esc(&b, &len, &cap, ".on_delete", k->on_delete); esc(&b, &len, &cap, ".on_update", k->on_update);
        esc(&b, &len, &cap, ".check", k->check); esc(&b, &len, &cap, ".raw", k->raw);
    }
    return b;
}
bool td_deserialize(const char *text, TdTable *t) {
    td_init(t);
    if (!text || strncmp(text, "lume_table_draft=1", 18)) return false;
    TdCol *c = NULL; TdKey *k = NULL;
    const char *p = text;
    char line[4200], v[4200];
    while (*p) {
        const char *e = strchr(p, '\n'); int len = e ? (int)(e - p) : (int)strlen(p);
        snprintf(line, sizeof line, "%.*s", len, p);
        p += len + (e ? 1 : 0);
        char *eq = strchr(line, '='); if (!eq) continue;
        *eq = 0; unesc(eq + 1, v, sizeof v);
        const char *key = line;
        if (!strcmp(key, "schema")) snprintf(t->schema, sizeof t->schema, "%s", v);
        else if (!strcmp(key, "name") && !c && !k) snprintf(t->name, sizeof t->name, "%s", v);
        else if (!strcmp(key, "comment")) snprintf(t->comment, sizeof t->comment, "%s", v);
        else if (!strcmp(key, "flags")) { int a = 0, b2 = 0, cc = 0, d = 1; sscanf(v, "%d %d %d %d", &a, &b2, &cc, &d); t->exists = a; t->temporary = b2; t->unlogged = cc; t->next_id = d; }
        else if (!strcmp(key, "engine")) snprintf(t->engine, sizeof t->engine, "%s", v);
        else if (!strcmp(key, "charset")) snprintf(t->charset, sizeof t->charset, "%s", v);
        else if (!strcmp(key, "collation") && !c) snprintf(t->collation, sizeof t->collation, "%s", v);
        else if (!strcmp(key, "fingerprint")) t->fingerprint = strtoull(v, NULL, 16);
        else if (!strcmp(key, "col")) {
            int id = 0, nn = 0, idn = 0, gs = 0; sscanf(v, "%d %d %d %d", &id, &nn, &idn, &gs);
            int keep = t->next_id; c = td_add_col(t, "", ""); c->id = id; t->next_id = keep; c->notnull = nn; c->identity = idn; c->gen_stored = gs; k = NULL;
        } else if (!strcmp(key, "key")) {
            int id = 0, kind = 0, u = 0, df = 0, dd = 0, bc = 0; sscanf(v, "%d %d %d %d %d %d", &id, &kind, &u, &df, &dd, &bc);
            int keep = t->next_id; k = td_add_key(t, kind, ""); k->id = id; t->next_id = keep; k->unique = u; k->deferrable = df; k->deferred = dd; k->backs_constraint = bc; c = NULL;
        } else if (c && key[0] == '.') {
            if (!strcmp(key, ".name")) snprintf(c->name, sizeof c->name, "%s", v);
            else if (!strcmp(key, ".type")) snprintf(c->type, sizeof c->type, "%s", v);
            else if (!strcmp(key, ".def")) snprintf(c->def, sizeof c->def, "%s", v);
            else if (!strcmp(key, ".gen")) snprintf(c->gen, sizeof c->gen, "%s", v);
            else if (!strcmp(key, ".comment")) snprintf(c->comment, sizeof c->comment, "%s", v);
            else if (!strcmp(key, ".collation")) snprintf(c->collation, sizeof c->collation, "%s", v);
            else if (!strcmp(key, ".def_name")) snprintf(c->def_name, sizeof c->def_name, "%s", v);
            else if (!strcmp(key, ".extra")) snprintf(c->extra, sizeof c->extra, "%s", v);
        } else if (k && key[0] == '.') {
            if (!strcmp(key, ".name")) snprintf(k->name, sizeof k->name, "%s", v);
            else if (!strcmp(key, ".cols")) { k->ncols = 0; for (char *s = v; *s && k->ncols < TD_MAXKC; ) { k->cols[k->ncols] = (int)strtol(s, &s, 10); k->desc[k->ncols] = *s == 'd'; if (*s == 'd') s++; k->ncols++; if (*s == ',') s++; else break; } }
            else if (!strcmp(key, ".include")) { k->ninclude = 0; for (char *s = v; *s && k->ninclude < TD_MAXKC; ) { k->include[k->ninclude++] = (int)strtol(s, &s, 10); if (*s == ',') s++; else break; } }
            else if (!strcmp(key, ".method")) snprintf(k->method, sizeof k->method, "%s", v);
            else if (!strcmp(key, ".where")) snprintf(k->where, sizeof k->where, "%s", v);
            else if (!strcmp(key, ".ref_schema")) snprintf(k->ref_schema, sizeof k->ref_schema, "%s", v);
            else if (!strcmp(key, ".ref_table")) snprintf(k->ref_table, sizeof k->ref_table, "%s", v);
            else if (!strcmp(key, ".ref_col") && k->nref < TD_MAXKC) snprintf(k->ref_cols[k->nref++], sizeof k->ref_cols[0], "%s", v);
            else if (!strcmp(key, ".on_delete")) snprintf(k->on_delete, sizeof k->on_delete, "%s", v);
            else if (!strcmp(key, ".on_update")) snprintf(k->on_update, sizeof k->on_update, "%s", v);
            else if (!strcmp(key, ".check")) snprintf(k->check, sizeof k->check, "%s", v);
            else if (!strcmp(key, ".raw")) snprintf(k->raw, sizeof k->raw, "%s", v);
        }
    }
    return true;
}
