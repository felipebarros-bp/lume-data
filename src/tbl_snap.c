/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Visual Table Designer: catalog snapshots per dialect (TBL-050 / 037 / 048 / 057 / 081).
 * See tbl.h. Every statement here is a plain read of the catalog for ONE table (never the
 * schema, never a SELECT of the table's rows, never a COUNT); the parse never invents an
 * empty table: a missing result or an unknown table is an error.
 */
#include "tbl.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void td_sql_free(TdSql *s) { for (int i = 0; i < s->n; i++) free(s->sql[i]); s->n = 0; }
static void add(TdSql *s, const char *fmt, ...) {
    if (s->n >= TD_SNAP_MAX) return;
    char *b = malloc(8192);
    va_list ap; va_start(ap, fmt); vsnprintf(b, 8192, fmt, ap); va_end(ap);
    s->sql[s->n++] = b;
}
static const char *cell(ResultSet *const *rs, int i, int r, int c) {
    if (!rs[i] || r >= rs[i]->nrows || c >= rs[i]->ncols) return "";
    const char *v = rs_cell(rs[i], r, c);
    return v ? v : "";
}
static bool isnull(ResultSet *const *rs, int i, int r, int c) { return !rs[i] || r >= rs[i]->nrows || c >= rs[i]->ncols || !rs_cell(rs[i], r, c); }
static bool truthy(const char *v) { return !strcmp(v, "1") || !strcasecmp(v, "t") || !strcasecmp(v, "true") || !strcasecmp(v, "YES") || !strcasecmp(v, "Y"); }
/* a list "a\tb\tc" (or \x01) -> items */
static int split(const char *s, char sep, char out[][128], int max) {
    int n = 0;
    while (*s && n < max) {
        const char *e = strchr(s, sep); int len = e ? (int)(e - s) : (int)strlen(s);
        snprintf(out[n++], 128, "%.*s", len, s);
        if (!e) break;
        s = e + 1;
    }
    return n;
}
static int col_by_name(TdTable *t, const char *name) { for (int i = 0; i < t->ncols; i++) if (!strcmp(t->cols[i].name, name)) return t->cols[i].id; return 0; }
static void add_cols_by_name(TdTable *t, TdKey *k, const char *list, char sep) {
    char items[TD_MAXKC][128]; int n = split(list, sep, items, TD_MAXKC);
    for (int i = 0; i < n; i++) {
        char *nm = items[i]; bool desc = false;
        size_t L = strlen(nm); if (L > 5 && !strcmp(nm + L - 5, " DESC")) { nm[L - 5] = 0; desc = true; }
        int id = col_by_name(t, nm);
        if (!id) { snprintf(k->raw, sizeof k->raw, "expression column %s", nm); continue; }
        k->cols[k->ncols] = id; k->desc[k->ncols] = desc; k->ncols++;
    }
}
static uint64_t fp_rs(ResultSet *const *rs, int n) {
    uint64_t h = 1469598103934665603ULL;
    for (int i = 0; i < n; i++) {
        if (!rs[i]) continue;
        for (int r = 0; r < rs[i]->nrows; r++) for (int c = 0; c < rs[i]->ncols; c++) {
            const char *v = rs_cell(rs[i], r, c);
            for (const char *p = v ? v : "\x02"; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
            h ^= 0x1e; h *= 1099511628211ULL;
        }
        h ^= 0x1d; h *= 1099511628211ULL;
    }
    return h;
}
uint64_t td_fingerprint(const TdTable *t) {
    char *s = td_serialize(t);
    uint64_t h = 1469598103934665603ULL;
    for (const char *p = s; *p; p++) { h ^= (unsigned char)*p; h *= 1099511628211ULL; }
    free(s);
    return h;
}
static void ql(int d, const char *s, char *o, int n) { td_lit(d, s, o, n); }
static void action(const char *code, char *out, int n) {
    const char *a = "";
    if (!strcmp(code, "c") || !strcasecmp(code, "CASCADE")) a = "CASCADE";
    else if (!strcmp(code, "n") || !strcasecmp(code, "SET NULL") || !strcasecmp(code, "SET_NULL")) a = "SET NULL";
    else if (!strcmp(code, "d") || !strcasecmp(code, "SET DEFAULT") || !strcasecmp(code, "SET_DEFAULT")) a = "SET DEFAULT";
    else if (!strcmp(code, "r") || !strcasecmp(code, "RESTRICT")) a = "RESTRICT";
    snprintf(out, (size_t)n, "%s", a);   /* NO ACTION = "" (the engine's default) */
}
/* SQL Server stores "((0))", "('open')", "(getdate())": the outer parentheses go */
static void strip_parens(char *s) {
    for (;;) {
        size_t L = strlen(s);
        if (L < 2 || s[0] != '(' || s[L - 1] != ')') return;
        int depth = 0; bool whole = true;
        for (size_t i = 0; i < L; i++) { if (s[i] == '(') depth++; else if (s[i] == ')') { depth--; if (depth == 0 && i < L - 1) { whole = false; break; } } }
        if (!whole) return;
        memmove(s, s + 1, L - 2); s[L - 2] = 0;
    }
}

/* ======================= SQLite ======================= */

/* the column / table-constraint items of a CREATE TABLE text, at depth 1 */
static int sqlite_items(const char *sql, char items[][2048], int max) {
    const char *p = strchr(sql, '(');
    if (!p) return 0;
    int n = 0, depth = 0, w = 0; char q = 0;
    for (p++; *p && n < max; p++) {
        char c = *p;
        if (q) { if (c == q) q = 0; if (w < 2047) items[n][w++] = c; continue; }
        if (c == '\'' || c == '"' || c == '`' || c == '[') { q = c == '[' ? ']' : c; if (w < 2047) items[n][w++] = c; continue; }
        if (c == '(') depth++;
        if (c == ')') { if (depth == 0) { items[n][w] = 0; n++; break; } depth--; }
        if (c == ',' && depth == 0) { items[n][w] = 0; n++; w = 0; continue; }
        if (w < 2047) items[n][w++] = c;
    }
    for (int i = 0; i < n; i++) { char *s = items[i]; while (isspace((unsigned char)*s)) s++; memmove(items[i], s, strlen(s) + 1); }
    return n;
}
/* text of the parenthesised group starting at the first '(' at or after p */
static const char *paren_group(const char *p, char *out, int n) {
    p = strchr(p, '(');
    if (!p) { out[0] = 0; return NULL; }
    int depth = 0, w = 0; char q = 0;
    for (; *p; p++) {
        if (q) { if (*p == q) q = 0; if (w < n - 1) out[w++] = *p; continue; }
        if (*p == '\'' || *p == '"') q = *p;
        if (*p == '(') { if (depth++ == 0) continue; }
        if (*p == ')') { if (--depth == 0) { out[w] = 0; return p + 1; } }
        if (w < n - 1) out[w++] = *p;
    }
    out[w] = 0; return NULL;
}
static const char *find_kw(const char *s, const char *kw) {
    size_t L = strlen(kw); char q = 0;
    for (const char *p = s; *p; p++) {
        if (q) { if (*p == q) q = 0; continue; }
        if (*p == '\'' || *p == '"' || *p == '`') { q = *p; continue; }
        if (!strncasecmp(p, kw, L) && (p == s || !isalnum((unsigned char)p[-1])) && !isalnum((unsigned char)p[L]) && p[L] != '_') return p;
    }
    return NULL;
}
static void unquote(char *s) {
    size_t L = strlen(s);
    if (L >= 2 && ((s[0] == '"' && s[L - 1] == '"') || (s[0] == '`' && s[L - 1] == '`') || (s[0] == '[' && s[L - 1] == ']') || (s[0] == '\'' && s[L - 1] == '\''))) { memmove(s, s + 1, L - 2); s[L - 2] = 0; }
}
static void first_word(const char *s, char *out, int n) {
    while (isspace((unsigned char)*s)) s++;
    int w = 0;
    if (*s == '"' || *s == '`' || *s == '[') { char q = *s == '[' ? ']' : *s; out[w++] = *s++; while (*s && w < n - 2) { out[w++] = *s; if (*s++ == q) break; } }
    else while (*s && !isspace((unsigned char)*s) && *s != '(' && w < n - 1) out[w++] = *s++;
    out[w] = 0; unquote(out);
}

static void sqlite_sql(const char *s, const char *t, TdSql *o) {
    char ls[300], lt[300]; ql(TD_SQLITE, s, ls, sizeof ls); ql(TD_SQLITE, t, lt, sizeof lt);
    char qs[300]; td_quote(TD_SQLITE, s, qs, sizeof qs);
    add(o, "SELECT cid, name, type, \"notnull\", dflt_value, pk, hidden FROM pragma_table_xinfo(%s, %s) ORDER BY cid", lt, ls);
    add(o, "SELECT id, seq, \"table\", \"from\", \"to\", on_update, on_delete FROM pragma_foreign_key_list(%s, %s) ORDER BY id, seq", lt, ls);
    add(o, "SELECT il.name, il.\"unique\", il.origin, il.partial, ii.seqno, ii.name, ii.\"desc\" FROM pragma_index_list(%s, %s) il JOIN pragma_index_xinfo(il.name, %s) ii ON ii.key = 1 ORDER BY il.name, ii.seqno", lt, ls, ls);
    add(o, "SELECT type, name, sql FROM %s.sqlite_master WHERE tbl_name = %s AND type IN ('table', 'index', 'trigger') ORDER BY type, name", qs, lt);
    add(o, "SELECT sqlite_version(), (SELECT foreign_keys FROM pragma_foreign_keys)");
}
static bool sqlite_parse(const char *s, const char *tn, ResultSet *const *rs, int n, TdTable *t, char *err, int errn) {
    if (n < 5) { snprintf(err, (size_t)errn, "incomplete catalog read"); return false; }
    const char *tsql = NULL;
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) if (!strcmp(cell(rs, 3, r, 0), "table")) tsql = cell(rs, 3, r, 2);
    if (!rs[0] || !rs[0]->nrows || !tsql) { snprintf(err, (size_t)errn, "table %s.%s was not found (or is not visible)", s, tn); return false; }
    snprintf(t->schema, sizeof t->schema, "%s", s); snprintf(t->name, sizeof t->name, "%s", tn);
    t->exists = true;
    /* columns */
    int pkcols[TD_MAXKC] = { 0 }, npk = 0, pkpos[TD_MAXKC];
    for (int r = 0; r < rs[0]->nrows; r++) {
        int hidden = atoi(cell(rs, 0, r, 6));
        if (hidden == 1) continue;   /* a virtual table's hidden column */
        TdCol *c = td_add_col(t, cell(rs, 0, r, 1), cell(rs, 0, r, 2));
        c->notnull = truthy(cell(rs, 0, r, 3));
        if (!isnull(rs, 0, r, 4)) snprintf(c->def, sizeof c->def, "%s", cell(rs, 0, r, 4));
        int pk = atoi(cell(rs, 0, r, 5));
        if (pk > 0 && npk < TD_MAXKC) { pkcols[npk] = c->id; pkpos[npk] = pk; npk++; }
        if (hidden == 2 || hidden == 3) c->gen_stored = hidden == 3;
    }
    /* the CREATE text: generated expressions, collations, checks, AUTOINCREMENT, names */
    static char items[256][2048];
    int ni = sqlite_items(tsql, items, 256);
    const char *close = strrchr(tsql, ')');
    if (close && (find_kw(close, "WITHOUT ROWID") || find_kw(close, "STRICT"))) snprintf(t->extra, sizeof t->extra, "%s", close + 1);
    char pkname[128] = "";
    for (int i = 0; i < ni; i++) {
        char *it = items[i], cname[128] = "";
        const char *body = it;
        if (!strncasecmp(it, "CONSTRAINT", 10) && isspace((unsigned char)it[10])) { first_word(it + 10, cname, sizeof cname); body = find_kw(it + 10, cname) ? it + 10 : it; body += strspn(body, " \t\r\n"); body += strlen(cname) + (body[0] == '"' || body[0] == '`' || body[0] == '[' ? 2 : 0); while (isspace((unsigned char)*body)) body++; }
        if (!strncasecmp(body, "PRIMARY", 7)) { snprintf(pkname, sizeof pkname, "%s", cname); continue; }
        if (!strncasecmp(body, "UNIQUE", 6) || !strncasecmp(body, "FOREIGN", 7)) continue;   /* from the pragmas */
        if (!strncasecmp(body, "CHECK", 5)) { TdKey *k = td_add_key(t, TK_CHECK, cname); paren_group(body, k->check, sizeof k->check); continue; }
        /* a column item */
        char col[128]; first_word(it, col, sizeof col);
        int id = col_by_name(t, col); TdCol *c = id ? td_col(t, id) : NULL;
        if (!c) continue;
        const char *g = find_kw(it, "AS");
        if (g && (find_kw(it, "GENERATED") || c->gen_stored || g)) {
            const char *gp = find_kw(it, "GENERATED") ? find_kw(it, "AS") : g;
            char ex[512]; if (gp && paren_group(gp, ex, sizeof ex) && (find_kw(it, "GENERATED") || find_kw(it, "STORED") || find_kw(it, "VIRTUAL"))) { snprintf(c->gen, sizeof c->gen, "%s", ex); c->gen_stored = find_kw(it, "STORED") != NULL; }
        }
        const char *cl = find_kw(it, "COLLATE"); if (cl) first_word(cl + 7, c->collation, sizeof c->collation);
        if (find_kw(it, "AUTOINCREMENT")) c->identity = 1;
        const char *ck = find_kw(it, "CHECK");
        if (ck) { TdKey *k = td_add_key(t, TK_CHECK, ""); paren_group(ck, k->check, sizeof k->check); }
        if (find_kw(it, "PRIMARY") && !pkname[0]) { const char *cn = find_kw(it, "CONSTRAINT"); if (cn) first_word(cn + 10, pkname, sizeof pkname); }
    }
    if (npk) {
        TdKey *k = td_add_key(t, TK_PK, pkname);
        for (int pos = 1; pos <= npk; pos++) for (int j = 0; j < npk; j++) if (pkpos[j] == pos) k->cols[k->ncols++] = pkcols[j];
    }
    /* foreign keys (pragma ids group the columns) */
    for (int r = 0; rs[1] && r < rs[1]->nrows; ) {
        const char *fid = cell(rs, 1, r, 0);
        TdKey *k = td_add_key(t, TK_FK, "");
        snprintf(k->ref_table, sizeof k->ref_table, "%s", cell(rs, 1, r, 2));
        action(cell(rs, 1, r, 6), k->on_delete, sizeof k->on_delete); action(cell(rs, 1, r, 5), k->on_update, sizeof k->on_update);
        for (; r < rs[1]->nrows && !strcmp(cell(rs, 1, r, 0), fid); r++) {
            int id = col_by_name(t, cell(rs, 1, r, 3));
            if (k->ncols < TD_MAXKC) { k->cols[k->ncols++] = id; snprintf(k->ref_cols[k->nref++], 128, "%s", isnull(rs, 1, r, 4) ? "" : cell(rs, 1, r, 4)); }
        }
    }
    /* indexes: c = CREATE INDEX, u = UNIQUE constraint, pk = the primary key's */
    for (int r = 0; rs[2] && r < rs[2]->nrows; ) {
        char iname[128]; snprintf(iname, sizeof iname, "%s", cell(rs, 2, r, 0));
        const char *origin = cell(rs, 2, r, 2);
        bool uniq = truthy(cell(rs, 2, r, 1)), partial = truthy(cell(rs, 2, r, 3));
        TdKey *k = NULL;
        if (!strcmp(origin, "pk")) { for (; r < rs[2]->nrows && !strcmp(cell(rs, 2, r, 0), iname); r++) {} continue; }
        k = td_add_key(t, !strcmp(origin, "u") ? TK_UNIQUE : TK_INDEX, !strcmp(origin, "u") ? "" : iname);
        k->unique = uniq;
        for (; r < rs[2]->nrows && !strcmp(cell(rs, 2, r, 0), iname); r++) {
            if (isnull(rs, 2, r, 5)) { snprintf(k->raw, sizeof k->raw, "expression index"); continue; }
            int id = col_by_name(t, cell(rs, 2, r, 5));
            if (k->ncols < TD_MAXKC) { k->cols[k->ncols] = id; k->desc[k->ncols] = truthy(cell(rs, 2, r, 6)); k->ncols++; }
        }
        for (int s2 = 0; rs[3] && s2 < rs[3]->nrows; s2++) if (!strcmp(cell(rs, 3, s2, 0), "index") && !strcmp(cell(rs, 3, s2, 1), iname)) {
            const char *isql = cell(rs, 3, s2, 2);
            if (partial) { const char *w = find_kw(isql, "WHERE"); if (w) snprintf(k->where, sizeof k->where, "%s", w + 6); }
            if (k->raw[0]) snprintf(k->raw, sizeof k->raw, "%s", isql);
        }
    }
    /* triggers: recreated as they are by a rebuild */
    int dw = 0, tw = 0; t->deps[0] = 0; t->triggers[0] = 0;
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) if (!strcmp(cell(rs, 3, r, 0), "trigger")) {
        dw += snprintf(t->deps + dw, sizeof t->deps - (size_t)dw, "trigger %s\n", cell(rs, 3, r, 1));
        tw += snprintf(t->triggers + tw, sizeof t->triggers - (size_t)tw, "%s%s", tw ? "\n;;\n" : "", cell(rs, 3, r, 2));
    }
    if (rs[4] && rs[4]->nrows) { /* version, foreign_keys pragma: read by the UI from the result */ }
    t->fingerprint = fp_rs(rs, 4);
    return true;
}

/* ======================= PostgreSQL ======================= */

static void pg_sql(const char *s, const char *tn, TdSql *o) {
    char qn[700], reg[800]; td_qname(TD_PG, s, tn, qn, sizeof qn);
    char lq[800]; ql(TD_PG, qn, lq, sizeof lq);
    snprintf(reg, sizeof reg, "to_regclass(%s)", lq);
    add(o, "SELECT c.relkind, c.relpersistence, coalesce(obj_description(c.oid, 'pg_class'), ''), coalesce(ts.spcname, ''), coalesce(array_to_string(c.reloptions, ', '), ''), "
           "c.reltuples::bigint, pg_has_role(c.relowner, 'USAGE'), c.relispartition, pg_get_userbyid(c.relowner), current_setting('server_version_num') "
           "FROM pg_class c LEFT JOIN pg_tablespace ts ON ts.oid = c.reltablespace WHERE c.oid = %s", reg);
    add(o, "SELECT a.attnum, a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, pg_get_expr(d.adbin, d.adrelid), a.attidentity, a.attgenerated, "
           "coalesce(col_description(a.attrelid, a.attnum), ''), CASE WHEN a.attcollation <> 0 AND a.attcollation <> t.typcollation THEN co.collname END "
           "FROM pg_attribute a JOIN pg_type t ON t.oid = a.atttypid LEFT JOIN pg_attrdef d ON d.adrelid = a.attrelid AND d.adnum = a.attnum "
           "LEFT JOIN pg_collation co ON co.oid = a.attcollation WHERE a.attrelid = %s AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum", reg);
    add(o, "SELECT con.conname, con.contype, (SELECT string_agg(a.attname, chr(9) ORDER BY k.ord) FROM unnest(con.conkey) WITH ORDINALITY k(n, ord) JOIN pg_attribute a ON a.attrelid = con.conrelid AND a.attnum = k.n), "
           "coalesce(fn.nspname, ''), coalesce(fc.relname, ''), coalesce((SELECT string_agg(fa.attname, chr(9) ORDER BY k.ord) FROM unnest(con.confkey) WITH ORDINALITY k(n, ord) JOIN pg_attribute fa ON fa.attrelid = con.confrelid AND fa.attnum = k.n), ''), "
           "con.confdeltype, con.confupdtype, con.condeferrable, con.condeferred, CASE WHEN con.contype = 'c' THEN pg_get_expr(con.conbin, con.conrelid) ELSE pg_get_constraintdef(con.oid) END "
           "FROM pg_constraint con LEFT JOIN pg_class fc ON fc.oid = con.confrelid LEFT JOIN pg_namespace fn ON fn.oid = fc.relnamespace "
           "WHERE con.conrelid = %s AND con.contype IN ('p', 'u', 'f', 'c', 'x') ORDER BY con.contype, con.conname", reg);
    add(o, "SELECT i.relname, ix.indisunique, am.amname, (SELECT string_agg(CASE WHEN k.n = 0 THEN chr(2) ELSE a.attname END || CASE WHEN k.ord <= ix.indnkeyatts AND (ix.indoption[k.ord - 1] & 1) = 1 THEN ' DESC' ELSE '' END, chr(9) ORDER BY k.ord) "
           "FROM unnest(ix.indkey::int2[]) WITH ORDINALITY k(n, ord) LEFT JOIN pg_attribute a ON a.attrelid = ix.indrelid AND a.attnum = k.n WHERE k.ord <= ix.indnkeyatts), "
           "coalesce((SELECT string_agg(a.attname, chr(9) ORDER BY k.ord) FROM unnest(ix.indkey::int2[]) WITH ORDINALITY k(n, ord) JOIN pg_attribute a ON a.attrelid = ix.indrelid AND a.attnum = k.n WHERE k.ord > ix.indnkeyatts), ''), "
           "coalesce(pg_get_expr(ix.indpred, ix.indrelid), ''), pg_get_indexdef(ix.indexrelid), ix.indexprs IS NOT NULL "
           "FROM pg_index ix JOIN pg_class i ON i.oid = ix.indexrelid JOIN pg_am am ON am.oid = i.relam "
           "WHERE ix.indrelid = %s AND NOT EXISTS (SELECT 1 FROM pg_constraint c WHERE c.conindid = ix.indexrelid AND c.conrelid = ix.indrelid AND c.contype IN ('p', 'u', 'x')) ORDER BY i.relname", reg);
    add(o, "SELECT x FROM (SELECT DISTINCT 'view ' || n.nspname || '.' || v.relname AS x FROM pg_depend d JOIN pg_rewrite r ON r.oid = d.objid JOIN pg_class v ON v.oid = r.ev_class "
           "JOIN pg_namespace n ON n.oid = v.relnamespace WHERE d.refobjid = %s AND v.oid <> d.refobjid "
           "UNION ALL SELECT 'foreign key ' || con.conname || ' on ' || con.conrelid::regclass::text FROM pg_constraint con WHERE con.confrelid = %s AND con.conrelid <> con.confrelid) z ORDER BY 1 LIMIT 200", reg, reg);
}
static bool pg_parse(const char *s, const char *tn, ResultSet *const *rs, int n, TdTable *t, char *err, int errn) {
    if (n < 5) { snprintf(err, (size_t)errn, "incomplete catalog read"); return false; }
    if (!rs[0] || !rs[0]->nrows) { snprintf(err, (size_t)errn, "table %s.%s was not found (or this user cannot see it)", s, tn); return false; }
    snprintf(t->schema, sizeof t->schema, "%s", s); snprintf(t->name, sizeof t->name, "%s", tn);
    t->exists = true;
    const char *kind = cell(rs, 0, 0, 0), *pers = cell(rs, 0, 0, 1);
    if (strcmp(kind, "r") && strcmp(kind, "p")) { snprintf(err, (size_t)errn, "%s.%s is not a table (relkind %s)", s, tn, kind); return false; }
    if (!strcmp(kind, "p")) snprintf(t->special, sizeof t->special, "a partitioned table: its partitions and partition key are not edited here");
    else if (truthy(cell(rs, 0, 0, 7))) snprintf(t->special, sizeof t->special, "a partition of another table: change the parent");
    t->unlogged = !strcmp(pers, "u"); t->temporary = !strcmp(pers, "t");
    snprintf(t->comment, sizeof t->comment, "%s", cell(rs, 0, 0, 2));
    snprintf(t->tablespace, sizeof t->tablespace, "%s", cell(rs, 0, 0, 3));
    if (cell(rs, 0, 0, 4)[0]) snprintf(t->extra, sizeof t->extra, "WITH (%s)", cell(rs, 0, 0, 4));
    t->approx_rows = atoll(cell(rs, 0, 0, 5));
    t->can_alter = truthy(cell(rs, 0, 0, 6));
    if (!t->can_alter) snprintf(t->alter_why, sizeof t->alter_why, "Only the owner of %s.%s (%s) or a member of that role can alter it", s, tn, cell(rs, 0, 0, 8));
    for (int r = 0; rs[1] && r < rs[1]->nrows; r++) {
        TdCol *c = td_add_col(t, cell(rs, 1, r, 1), cell(rs, 1, r, 2));
        c->notnull = truthy(cell(rs, 1, r, 3));
        const char *ident = cell(rs, 1, r, 5), *gen = cell(rs, 1, r, 6);
        if (!strcmp(ident, "a")) c->identity = 1; else if (!strcmp(ident, "d")) c->identity = 2;
        if (gen[0] == 's' || gen[0] == 'v') { snprintf(c->gen, sizeof c->gen, "%s", cell(rs, 1, r, 4)); c->gen_stored = gen[0] == 's'; }
        else if (!isnull(rs, 1, r, 4)) snprintf(c->def, sizeof c->def, "%s", cell(rs, 1, r, 4));
        snprintf(c->comment, sizeof c->comment, "%s", cell(rs, 1, r, 7));
        if (!isnull(rs, 1, r, 8)) snprintf(c->collation, sizeof c->collation, "%s", cell(rs, 1, r, 8));
    }
    if (!t->ncols) { snprintf(err, (size_t)errn, "no column of %s.%s is visible", s, tn); return false; }
    for (int r = 0; rs[2] && r < rs[2]->nrows; r++) {
        const char *ty = cell(rs, 2, r, 1);
        int kind2 = !strcmp(ty, "p") ? TK_PK : !strcmp(ty, "u") ? TK_UNIQUE : !strcmp(ty, "f") ? TK_FK : !strcmp(ty, "c") ? TK_CHECK : TK_INDEX;
        TdKey *k = td_add_key(t, kind2, cell(rs, 2, r, 0));
        if (!strcmp(ty, "x")) { snprintf(k->raw, sizeof k->raw, "%s", cell(rs, 2, r, 10)); k->kind = TK_CHECK; continue; }   /* exclusion: kept as is */
        add_cols_by_name(t, k, cell(rs, 2, r, 2), '\t');
        if (kind2 == TK_FK) {
            snprintf(k->ref_schema, sizeof k->ref_schema, "%s", cell(rs, 2, r, 3)); snprintf(k->ref_table, sizeof k->ref_table, "%s", cell(rs, 2, r, 4));
            char items[TD_MAXKC][128]; int m = split(cell(rs, 2, r, 5), '\t', items, TD_MAXKC);
            for (int i = 0; i < m; i++) snprintf(k->ref_cols[k->nref++], 128, "%s", items[i]);
            action(cell(rs, 2, r, 6), k->on_delete, sizeof k->on_delete); action(cell(rs, 2, r, 7), k->on_update, sizeof k->on_update);
            k->deferrable = truthy(cell(rs, 2, r, 8)); k->deferred = truthy(cell(rs, 2, r, 9));
        }
        if (kind2 == TK_CHECK) snprintf(k->check, sizeof k->check, "%s", cell(rs, 2, r, 10));
    }
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) {
        TdKey *k = td_add_key(t, TK_INDEX, cell(rs, 3, r, 0));
        k->unique = truthy(cell(rs, 3, r, 1));
        if (strcmp(cell(rs, 3, r, 2), "btree")) snprintf(k->method, sizeof k->method, "%s", cell(rs, 3, r, 2));
        if (truthy(cell(rs, 3, r, 8)) || strchr(cell(rs, 3, r, 3), 2)) { snprintf(k->raw, sizeof k->raw, "%s", cell(rs, 3, r, 7)); continue; }   /* expression index: kept as is */
        add_cols_by_name(t, k, cell(rs, 3, r, 3), '\t');
        char inc[TD_MAXKC][128]; int m = split(cell(rs, 3, r, 4), '\t', inc, TD_MAXKC);
        for (int i = 0; i < m; i++) { int id = col_by_name(t, inc[i]); if (id) k->include[k->ninclude++] = id; }
        snprintf(k->where, sizeof k->where, "%s", cell(rs, 3, r, 5));
        if (strstr(cell(rs, 3, r, 6), " WITH (") || strstr(cell(rs, 3, r, 6), " TABLESPACE ") || strstr(cell(rs, 3, r, 6), " NULLS ") || strstr(cell(rs, 3, r, 6), " COLLATE ")) snprintf(k->raw, sizeof k->raw, "%s", cell(rs, 3, r, 6));
    }
    int w = 0; t->deps[0] = 0;
    for (int r = 0; rs[4] && r < rs[4]->nrows && w < (int)sizeof t->deps - 1; r++) w += snprintf(t->deps + w, sizeof t->deps - (size_t)w, "%s\n", cell(rs, 4, r, 0));
    t->fingerprint = fp_rs(rs, 4);
    return true;
}

/* ======================= Oracle ======================= */

static void ora_sql(const char *s, const char *tn, TdSql *o) {
    char ls[300], lt[300]; ql(TD_ORACLE, s, ls, sizeof ls); ql(TD_ORACLE, tn, lt, sizeof lt);
    add(o, "SELECT t.temporary, t.partitioned, NVL(t.iot_type, '-'), NVL(t.tablespace_name, ''), NVL(c.comments, ''), NVL(t.num_rows, -1), "
           "(SELECT COUNT(*) FROM all_external_tables e WHERE e.owner = t.owner AND e.table_name = t.table_name), "
           "CASE WHEN t.owner = USER THEN 1 WHEN EXISTS (SELECT 1 FROM session_privs WHERE privilege = 'ALTER ANY TABLE') THEN 1 "
           "WHEN EXISTS (SELECT 1 FROM all_tab_privs p WHERE p.table_schema = t.owner AND p.table_name = t.table_name AND p.privilege = 'ALTER') THEN 1 ELSE 0 END, t.duration "
           "FROM all_tables t LEFT JOIN all_tab_comments c ON c.owner = t.owner AND c.table_name = t.table_name WHERE t.owner = %s AND t.table_name = %s", ls, lt);
    add(o, "SELECT c.column_id, c.column_name, c.data_type, c.data_length, c.data_precision, c.data_scale, c.char_length, c.char_used, c.nullable, c.data_default_vc, "
           "c.identity_column, c.virtual_column, NVL(m.comments, ''), c.collation, c.default_on_null "
           "FROM all_tab_cols c LEFT JOIN all_col_comments m ON m.owner = c.owner AND m.table_name = c.table_name AND m.column_name = c.column_name "
           "WHERE c.owner = %s AND c.table_name = %s AND c.hidden_column = 'NO' ORDER BY c.column_id", ls, lt);
    add(o, "SELECT c.constraint_name, c.constraint_type, c.search_condition_vc, NVL(rc.owner, ''), NVL(rc.table_name, ''), c.delete_rule, c.deferrable, c.deferred, c.generated, "
           "(SELECT LISTAGG(cc.column_name, CHR(9)) WITHIN GROUP (ORDER BY cc.position) FROM all_cons_columns cc WHERE cc.owner = c.owner AND cc.constraint_name = c.constraint_name), "
           "(SELECT LISTAGG(rcc.column_name, CHR(9)) WITHIN GROUP (ORDER BY rcc.position) FROM all_cons_columns rcc WHERE rcc.owner = c.r_owner AND rcc.constraint_name = c.r_constraint_name) "
           "FROM all_constraints c LEFT JOIN all_constraints rc ON rc.owner = c.r_owner AND rc.constraint_name = c.r_constraint_name "
           "WHERE c.owner = %s AND c.table_name = %s AND c.constraint_type IN ('P', 'U', 'R', 'C') ORDER BY c.constraint_type, c.constraint_name", ls, lt);
    add(o, "SELECT i.index_name, i.uniqueness, i.index_type, (SELECT LISTAGG(ic.column_name || CASE WHEN ic.descend = 'DESC' THEN ' DESC' END, CHR(9)) WITHIN GROUP (ORDER BY ic.column_position) "
           "FROM all_ind_columns ic WHERE ic.index_owner = i.owner AND ic.index_name = i.index_name), "
           "CASE WHEN EXISTS (SELECT 1 FROM all_constraints c WHERE c.owner = i.table_owner AND c.table_name = i.table_name AND c.index_name = i.index_name AND c.constraint_type IN ('P', 'U')) THEN 1 ELSE 0 END, i.partitioned "
           "FROM all_indexes i WHERE i.table_owner = %s AND i.table_name = %s ORDER BY i.index_name", ls, lt);
    add(o, "SELECT x FROM (SELECT LOWER(d.type) || ' ' || d.owner || '.' || d.name AS x FROM all_dependencies d WHERE d.referenced_owner = %s AND d.referenced_name = %s AND d.referenced_type = 'TABLE' "
           "UNION ALL SELECT 'foreign key ' || c.constraint_name || ' on ' || c.owner || '.' || c.table_name FROM all_constraints c JOIN all_constraints p ON p.owner = c.r_owner AND p.constraint_name = c.r_constraint_name "
           "WHERE c.constraint_type = 'R' AND p.owner = %s AND p.table_name = %s AND c.table_name <> p.table_name) WHERE ROWNUM <= 200", ls, lt, ls, lt);
    add(o, "SELECT column_name, generation_type FROM all_tab_identity_cols WHERE owner = %s AND table_name = %s", ls, lt);
}
static void ora_type(ResultSet *const *rs, int r, char *out, int n) {
    const char *ty = cell(rs, 1, r, 2); long len = atol(cell(rs, 1, r, 3)), cl = atol(cell(rs, 1, r, 6));
    bool hasp = !isnull(rs, 1, r, 4), hass = !isnull(rs, 1, r, 5);
    long p = atol(cell(rs, 1, r, 4)), s = atol(cell(rs, 1, r, 5));
    const char *cu = cell(rs, 1, r, 7);
    if (!strcmp(ty, "NUMBER")) { if (hasp && hass && s) snprintf(out, (size_t)n, "NUMBER(%ld,%ld)", p, s); else if (hasp) snprintf(out, (size_t)n, "NUMBER(%ld)", p); else if (hass && s == 0) snprintf(out, (size_t)n, "INTEGER"); else snprintf(out, (size_t)n, "NUMBER"); }
    else if (!strcmp(ty, "VARCHAR2") || !strcmp(ty, "CHAR")) snprintf(out, (size_t)n, "%s(%ld%s)", ty, cl ? cl : len, !strcmp(cu, "C") ? " CHAR" : "");
    else if (!strcmp(ty, "NVARCHAR2") || !strcmp(ty, "NCHAR")) snprintf(out, (size_t)n, "%s(%ld)", ty, cl ? cl : len);
    else if (!strcmp(ty, "RAW")) snprintf(out, (size_t)n, "RAW(%ld)", len);
    else if (!strcmp(ty, "FLOAT") && hasp) snprintf(out, (size_t)n, "FLOAT(%ld)", p);
    else snprintf(out, (size_t)n, "%s", ty);   /* DATE, TIMESTAMP(6), CLOB ... as the catalog names them */
}
static void rtrim(char *s) { size_t L = strlen(s); while (L && isspace((unsigned char)s[L - 1])) s[--L] = 0; }
static bool ora_parse(const char *s, const char *tn, ResultSet *const *rs, int n, TdTable *t, char *err, int errn) {
    if (n < 6) { snprintf(err, (size_t)errn, "incomplete catalog read"); return false; }
    if (!rs[0] || !rs[0]->nrows) { snprintf(err, (size_t)errn, "table %s.%s was not found (or this user cannot see it)", s, tn); return false; }
    snprintf(t->schema, sizeof t->schema, "%s", s); snprintf(t->name, sizeof t->name, "%s", tn);
    t->exists = true;
    t->temporary = !strcmp(cell(rs, 0, 0, 0), "Y");
    if (!strcmp(cell(rs, 0, 0, 1), "YES")) snprintf(t->special, sizeof t->special, "a partitioned table: partitions are not edited here");
    else if (strcmp(cell(rs, 0, 0, 2), "-")) snprintf(t->special, sizeof t->special, "an index-organized table (%s)", cell(rs, 0, 0, 2));
    else if (atoi(cell(rs, 0, 0, 6))) snprintf(t->special, sizeof t->special, "an external table");
    snprintf(t->tablespace, sizeof t->tablespace, "%s", cell(rs, 0, 0, 3));
    snprintf(t->comment, sizeof t->comment, "%s", cell(rs, 0, 0, 4));
    t->approx_rows = atoll(cell(rs, 0, 0, 5));
    t->can_alter = truthy(cell(rs, 0, 0, 7));
    if (!t->can_alter) snprintf(t->alter_why, sizeof t->alter_why, "This user is not the owner of %s.%s and has no ALTER privilege on it", s, tn);
    for (int r = 0; rs[1] && r < rs[1]->nrows; r++) {
        char ty[128]; ora_type(rs, r, ty, sizeof ty);
        TdCol *c = td_add_col(t, cell(rs, 1, r, 1), ty);
        c->notnull = !strcmp(cell(rs, 1, r, 8), "N");
        char def[512]; snprintf(def, sizeof def, "%s", cell(rs, 1, r, 9)); rtrim(def);
        if (!strcmp(cell(rs, 1, r, 11), "YES")) { snprintf(c->gen, sizeof c->gen, "%s", def); c->gen_stored = false; }
        else if (!strcmp(cell(rs, 1, r, 10), "YES")) c->identity = 2;
        else if (def[0] && strcasecmp(def, "NULL")) snprintf(c->def, sizeof c->def, "%s", def);
        snprintf(c->comment, sizeof c->comment, "%s", cell(rs, 1, r, 12));
        if (!strcmp(cell(rs, 1, r, 14), "YES")) snprintf(c->extra, sizeof c->extra, "DEFAULT ON NULL");
    }
    if (!t->ncols) { snprintf(err, (size_t)errn, "no column of %s.%s is visible", s, tn); return false; }
    for (int r = 0; rs[5] && r < rs[5]->nrows; r++) { int id = col_by_name(t, cell(rs, 5, r, 0)); TdCol *c = td_col(t, id); if (c) c->identity = !strcmp(cell(rs, 5, r, 1), "ALWAYS") ? 1 : 2; }
    for (int r = 0; rs[2] && r < rs[2]->nrows; r++) {
        const char *ty = cell(rs, 2, r, 1), *cond = cell(rs, 2, r, 2);
        if (!strcmp(ty, "C") && !strcmp(cell(rs, 2, r, 8), "GENERATED NAME") && strstr(cond, " IS NOT NULL") && !strchr(cond, ' ' + 0 == 0 ? 0 : '(')) continue;   /* a NOT NULL column */
        int kind2 = !strcmp(ty, "P") ? TK_PK : !strcmp(ty, "U") ? TK_UNIQUE : !strcmp(ty, "R") ? TK_FK : TK_CHECK;
        TdKey *k = td_add_key(t, kind2, cell(rs, 2, r, 0));
        add_cols_by_name(t, k, cell(rs, 2, r, 9), '\t');
        if (kind2 == TK_CHECK) snprintf(k->check, sizeof k->check, "%s", cond);
        if (kind2 == TK_FK) {
            snprintf(k->ref_schema, sizeof k->ref_schema, "%s", cell(rs, 2, r, 3)); snprintf(k->ref_table, sizeof k->ref_table, "%s", cell(rs, 2, r, 4));
            char items[TD_MAXKC][128]; int m = split(cell(rs, 2, r, 10), '\t', items, TD_MAXKC);
            for (int i = 0; i < m; i++) snprintf(k->ref_cols[k->nref++], 128, "%s", items[i]);
            action(cell(rs, 2, r, 5), k->on_delete, sizeof k->on_delete);
            k->deferrable = !strcmp(cell(rs, 2, r, 6), "DEFERRABLE"); k->deferred = !strcmp(cell(rs, 2, r, 7), "DEFERRED");
        }
    }
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) {
        if (truthy(cell(rs, 3, r, 4))) continue;   /* the index of a PK / UNIQUE */
        TdKey *k = td_add_key(t, TK_INDEX, cell(rs, 3, r, 0));
        k->unique = !strcmp(cell(rs, 3, r, 1), "UNIQUE");
        const char *it = cell(rs, 3, r, 2);
        if (strcmp(it, "NORMAL") || !strcmp(cell(rs, 3, r, 5), "YES")) { snprintf(k->raw, sizeof k->raw, "%s index%s", it, !strcmp(cell(rs, 3, r, 5), "YES") ? ", partitioned" : ""); continue; }
        add_cols_by_name(t, k, cell(rs, 3, r, 3), '\t');
    }
    int w = 0; t->deps[0] = 0;
    for (int r = 0; rs[4] && r < rs[4]->nrows && w < (int)sizeof t->deps - 1; r++) w += snprintf(t->deps + w, sizeof t->deps - (size_t)w, "%s\n", cell(rs, 4, r, 0));
    t->fingerprint = fp_rs(rs, 6);
    return true;
}

/* ======================= MySQL / MariaDB ======================= */

static void my_sql(int d, const char *s, const char *tn, TdSql *o) {
    char ls[300], lt[300]; ql(d, s, ls, sizeof ls); ql(d, tn, lt, sizeof lt);
    add(o, "SELECT t.engine, t.table_collation, t.table_comment, t.create_options, t.table_rows, t.table_type, ca.character_set_name, VERSION() "
           "FROM information_schema.tables t LEFT JOIN information_schema.collation_character_set_applicability ca ON ca.collation_name = t.table_collation "
           "WHERE t.table_schema = %s AND t.table_name = %s", ls, lt);
    add(o, "SELECT ordinal_position, column_name, column_type, is_nullable, column_default, extra, generation_expression, column_comment, collation_name "
           "FROM information_schema.columns WHERE table_schema = %s AND table_name = %s ORDER BY ordinal_position", ls, lt);
    add(o, "SELECT tc.constraint_name, tc.constraint_type, (SELECT GROUP_CONCAT(k.column_name ORDER BY k.ordinal_position SEPARATOR '\\t') FROM information_schema.key_column_usage k "
           "WHERE k.constraint_schema = tc.constraint_schema AND k.constraint_name = tc.constraint_name AND k.table_name = tc.table_name), "
           "rc.unique_constraint_schema, rc.referenced_table_name, (SELECT GROUP_CONCAT(k.referenced_column_name ORDER BY k.ordinal_position SEPARATOR '\\t') FROM information_schema.key_column_usage k "
           "WHERE k.constraint_schema = tc.constraint_schema AND k.constraint_name = tc.constraint_name AND k.table_name = tc.table_name), rc.delete_rule, rc.update_rule "
           "FROM information_schema.table_constraints tc LEFT JOIN information_schema.referential_constraints rc ON rc.constraint_schema = tc.constraint_schema AND rc.constraint_name = tc.constraint_name AND rc.table_name = tc.table_name "
           "WHERE tc.table_schema = %s AND tc.table_name = %s AND tc.constraint_type IN ('PRIMARY KEY', 'UNIQUE', 'FOREIGN KEY') ORDER BY tc.constraint_type, tc.constraint_name", ls, lt);
    add(o, "SELECT index_name, non_unique, GROUP_CONCAT(CONCAT(IFNULL(column_name, CHAR(2)), IF(collation = 'D', ' DESC', '')) ORDER BY seq_in_index SEPARATOR '\\t'), index_type, MAX(sub_part IS NOT NULL) "
           "FROM information_schema.statistics WHERE table_schema = %s AND table_name = %s GROUP BY index_name, non_unique, index_type ORDER BY index_name", ls, lt);
    if (d == TD_MARIADB)
        add(o, "SELECT constraint_name, check_clause FROM information_schema.check_constraints WHERE constraint_schema = %s AND table_name = %s AND level = 'Table' ORDER BY constraint_name", ls, lt);
    else
        add(o, "SELECT cc.constraint_name, cc.check_clause FROM information_schema.check_constraints cc JOIN information_schema.table_constraints tc ON tc.constraint_schema = cc.constraint_schema "
               "AND tc.constraint_name = cc.constraint_name WHERE tc.table_schema = %s AND tc.table_name = %s AND tc.constraint_type = 'CHECK' ORDER BY cc.constraint_name", ls, lt);
    add(o, "SELECT CONCAT('foreign key ', constraint_name, ' on ', constraint_schema, '.', table_name) FROM information_schema.referential_constraints "
           "WHERE unique_constraint_schema = %s AND referenced_table_name = %s AND table_name <> referenced_table_name ORDER BY 1 LIMIT 200", ls, lt);
}
static bool my_parse(int d, const char *s, const char *tn, ResultSet *const *rs, int n, TdTable *t, char *err, int errn) {
    if (n < 6) { snprintf(err, (size_t)errn, "incomplete catalog read"); return false; }
    if (!rs[0] || !rs[0]->nrows) { snprintf(err, (size_t)errn, "table %s.%s was not found (or this user cannot see it)", s, tn); return false; }
    if (strcmp(cell(rs, 0, 0, 5), "BASE TABLE")) { snprintf(err, (size_t)errn, "%s.%s is not a base table (%s)", s, tn, cell(rs, 0, 0, 5)); return false; }
    snprintf(t->schema, sizeof t->schema, "%s", s); snprintf(t->name, sizeof t->name, "%s", tn);
    t->exists = true;
    snprintf(t->engine, sizeof t->engine, "%s", cell(rs, 0, 0, 0));
    snprintf(t->collation, sizeof t->collation, "%s", cell(rs, 0, 0, 1));
    snprintf(t->comment, sizeof t->comment, "%s", cell(rs, 0, 0, 2));
    snprintf(t->charset, sizeof t->charset, "%s", cell(rs, 0, 0, 6));
    const char *opts = cell(rs, 0, 0, 3);
    if (strstr(opts, "partitioned")) snprintf(t->special, sizeof t->special, "a partitioned table: partitions are not edited here");
    else if (opts[0]) snprintf(t->extra, sizeof t->extra, "%s", opts);
    t->approx_rows = isnull(rs, 0, 0, 4) ? -1 : atoll(cell(rs, 0, 0, 4));
    bool maria = d == TD_MARIADB || strstr(cell(rs, 0, 0, 7), "MariaDB");
    for (int r = 0; rs[1] && r < rs[1]->nrows; r++) {
        TdCol *c = td_add_col(t, cell(rs, 1, r, 1), cell(rs, 1, r, 2));
        c->notnull = !strcmp(cell(rs, 1, r, 3), "NO");
        const char *ex = cell(rs, 1, r, 5);
        char rest[256] = ""; int rw = 0;
        if (strcasestr(ex, "auto_increment")) c->identity = 2;
        if (strcasestr(ex, "VIRTUAL GENERATED") || strcasestr(ex, "STORED GENERATED") || (maria && (strcasestr(ex, "VIRTUAL") || strcasestr(ex, "PERSISTENT")))) {
            snprintf(c->gen, sizeof c->gen, "%s", cell(rs, 1, r, 6)); c->gen_stored = strcasestr(ex, "STORED") || strcasestr(ex, "PERSISTENT");
        } else if (!isnull(rs, 1, r, 4)) {
            const char *dv = cell(rs, 1, r, 4);
            if (maria) { if (strcasecmp(dv, "NULL")) snprintf(c->def, sizeof c->def, "%s", dv); }   /* MariaDB: already SQL text */
            else if (strcasestr(ex, "DEFAULT_GENERATED")) snprintf(c->def, sizeof c->def, "%s", dv);   /* an expression */
            else {
                bool num = dv[0] && strspn(dv, "-0123456789.") == strlen(dv);
                if (num || !strncasecmp(dv, "b'", 2)) snprintf(c->def, sizeof c->def, "%s", dv);
                else td_lit(d, dv, c->def, sizeof c->def);   /* a literal: quoted back */
            }
        }
        const char *ou = strcasestr(ex, "on update ");
        if (ou) { rw += snprintf(rest + rw, sizeof rest - (size_t)rw, "ON UPDATE %s", ou + 10); char *sp = strchr(rest + 10, ' '); if (sp) *sp = 0; }
        if (rest[0]) snprintf(c->extra, sizeof c->extra, "%s", rest);
        snprintf(c->comment, sizeof c->comment, "%s", cell(rs, 1, r, 7));
        if (!isnull(rs, 1, r, 8) && strcmp(cell(rs, 1, r, 8), t->collation)) snprintf(c->collation, sizeof c->collation, "%s", cell(rs, 1, r, 8));
    }
    if (!t->ncols) { snprintf(err, (size_t)errn, "no column of %s.%s is visible", s, tn); return false; }
    char uniq[64][128]; int nu = 0;   /* UNIQUE constraints and foreign keys: MySQL keeps an index of the same name */
    for (int r = 0; rs[2] && r < rs[2]->nrows; r++) {
        const char *ty = cell(rs, 2, r, 1);
        int kind2 = !strcmp(ty, "PRIMARY KEY") ? TK_PK : !strcmp(ty, "UNIQUE") ? TK_UNIQUE : TK_FK;
        TdKey *k = td_add_key(t, kind2, kind2 == TK_PK ? "" : cell(rs, 2, r, 0));
        add_cols_by_name(t, k, cell(rs, 2, r, 2), '\t');
        if ((kind2 == TK_UNIQUE || kind2 == TK_FK) && nu < 64) snprintf(uniq[nu++], 128, "%s", cell(rs, 2, r, 0));
        if (kind2 == TK_FK) {
            snprintf(k->ref_schema, sizeof k->ref_schema, "%s", cell(rs, 2, r, 3)); snprintf(k->ref_table, sizeof k->ref_table, "%s", cell(rs, 2, r, 4));
            char items[TD_MAXKC][128]; int m = split(cell(rs, 2, r, 5), '\t', items, TD_MAXKC);
            for (int i = 0; i < m; i++) snprintf(k->ref_cols[k->nref++], 128, "%s", items[i]);
            action(cell(rs, 2, r, 6), k->on_delete, sizeof k->on_delete); action(cell(rs, 2, r, 7), k->on_update, sizeof k->on_update);
        }
    }
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) {
        const char *iname = cell(rs, 3, r, 0);
        if (!strcmp(iname, "PRIMARY")) continue;
        bool backs = false; for (int i = 0; i < nu; i++) backs |= !strcmp(uniq[i], iname);
        if (backs) continue;
        TdKey *k = td_add_key(t, TK_INDEX, iname);
        k->unique = !strcmp(cell(rs, 3, r, 1), "0");
        const char *ty = cell(rs, 3, r, 3);
        if (strcmp(ty, "BTREE") || truthy(cell(rs, 3, r, 4)) || strchr(cell(rs, 3, r, 2), 2)) { snprintf(k->raw, sizeof k->raw, "%s index%s", ty, truthy(cell(rs, 3, r, 4)) ? " with column prefixes" : strchr(cell(rs, 3, r, 2), 2) ? " on expressions" : ""); continue; }
        add_cols_by_name(t, k, cell(rs, 3, r, 2), '\t');
    }
    for (int r = 0; rs[4] && r < rs[4]->nrows; r++) {
        TdKey *k = td_add_key(t, TK_CHECK, cell(rs, 4, r, 0));
        snprintf(k->check, sizeof k->check, "%s", cell(rs, 4, r, 1));
    }
    int w = 0; t->deps[0] = 0;
    for (int r = 0; rs[5] && r < rs[5]->nrows && w < (int)sizeof t->deps - 1; r++) w += snprintf(t->deps + w, sizeof t->deps - (size_t)w, "%s\n", cell(rs, 5, r, 0));
    t->deps_partial = true;   /* views that select from it are not listed on MySQL / MariaDB */
    t->fingerprint = fp_rs(rs, 5);
    return true;
}

/* ======================= SQL Server ======================= */

static void ms_sql(const char *s, const char *tn, TdSql *o) {
    char qn[700], lq[800]; td_qname(TD_MSSQL, s, tn, qn, sizeof qn); td_lit(TD_MSSQL, qn, lq, sizeof lq);
    char oid[900]; snprintf(oid, sizeof oid, "OBJECT_ID(%s)", lq);
    add(o, "SELECT t.temporal_type, t.is_memory_optimized, CAST(ep.value AS nvarchar(4000)), t.is_external, HAS_PERMS_BY_NAME(%s, 'OBJECT', 'ALTER'), "
           "(SELECT SUM(p.rows) FROM sys.partitions p WHERE p.object_id = t.object_id AND p.index_id IN (0, 1)), "
           "(SELECT COUNT(*) FROM sys.partitions p WHERE p.object_id = t.object_id AND p.index_id IN (0, 1)), CAST(SERVERPROPERTY('ProductVersion') AS nvarchar(64)) "
           "FROM sys.tables t LEFT JOIN sys.extended_properties ep ON ep.class = 1 AND ep.major_id = t.object_id AND ep.minor_id = 0 AND ep.name = N'MS_Description' WHERE t.object_id = %s", lq, oid);
    add(o, "SELECT c.column_id, c.name, ty.name, c.max_length, c.precision, c.scale, c.is_nullable, c.is_identity, c.is_computed, cc.definition, cc.is_persisted, dc.name, dc.definition, "
           "CAST(ep.value AS nvarchar(4000)), c.collation_name, CAST(DATABASEPROPERTYEX(DB_NAME(), 'Collation') AS nvarchar(128)), ty.is_user_defined, c.is_sparse "
           "FROM sys.columns c JOIN sys.types ty ON ty.user_type_id = c.user_type_id LEFT JOIN sys.computed_columns cc ON cc.object_id = c.object_id AND cc.column_id = c.column_id "
           "LEFT JOIN sys.default_constraints dc ON dc.parent_object_id = c.object_id AND dc.parent_column_id = c.column_id "
           "LEFT JOIN sys.extended_properties ep ON ep.class = 1 AND ep.major_id = c.object_id AND ep.minor_id = c.column_id AND ep.name = N'MS_Description' "
           "WHERE c.object_id = %s ORDER BY c.column_id", oid);
    /* sys.* columns come in different collations: every text column of the UNION is cast to one */
#define MSC(x) "CAST(" x " AS nvarchar(4000)) COLLATE DATABASE_DEFAULT"
    add(o, "SELECT " MSC("kc.name") ", " MSC("kc.type") ", " MSC("(SELECT STRING_AGG(col.name + CASE WHEN ic.is_descending_key = 1 THEN ' DESC' ELSE '' END, CHAR(9)) WITHIN GROUP (ORDER BY ic.key_ordinal) "
           "FROM sys.index_columns ic JOIN sys.columns col ON col.object_id = ic.object_id AND col.column_id = ic.column_id WHERE ic.object_id = kc.parent_object_id AND ic.index_id = kc.unique_index_id AND ic.is_included_column = 0)") ", "
           MSC("''") ", " MSC("''") ", " MSC("''") ", " MSC("''") ", " MSC("''") ", 0 FROM sys.key_constraints kc WHERE kc.parent_object_id = %s "
           "UNION ALL SELECT " MSC("fk.name") ", " MSC("'F'") ", " MSC("(SELECT STRING_AGG(pc.name, CHAR(9)) WITHIN GROUP (ORDER BY fkc.constraint_column_id) FROM sys.foreign_key_columns fkc JOIN sys.columns pc ON pc.object_id = fkc.parent_object_id AND pc.column_id = fkc.parent_column_id WHERE fkc.constraint_object_id = fk.object_id)") ", "
           MSC("OBJECT_SCHEMA_NAME(fk.referenced_object_id)") ", " MSC("OBJECT_NAME(fk.referenced_object_id)") ", " MSC("(SELECT STRING_AGG(rc.name, CHAR(9)) WITHIN GROUP (ORDER BY fkc.constraint_column_id) FROM sys.foreign_key_columns fkc JOIN sys.columns rc ON rc.object_id = fkc.referenced_object_id AND rc.column_id = fkc.referenced_column_id WHERE fkc.constraint_object_id = fk.object_id)") ", "
           MSC("fk.delete_referential_action_desc") ", " MSC("fk.update_referential_action_desc") ", CAST(fk.is_disabled AS int) FROM sys.foreign_keys fk WHERE fk.parent_object_id = %s "
           "UNION ALL SELECT " MSC("ck.name") ", " MSC("'C'") ", " MSC("''") ", " MSC("''") ", " MSC("''") ", " MSC("''") ", " MSC("''") ", " MSC("ck.definition") ", CAST(ck.is_disabled AS int) FROM sys.check_constraints ck WHERE ck.parent_object_id = %s ORDER BY 2, 1", oid, oid, oid);
#undef MSC
    add(o, "SELECT i.name, i.is_unique, i.type_desc, STRING_AGG(CASE WHEN ic.is_included_column = 0 THEN c.name + CASE WHEN ic.is_descending_key = 1 THEN ' DESC' ELSE '' END END, CHAR(9)) WITHIN GROUP (ORDER BY ic.key_ordinal), "
           "STRING_AGG(CASE WHEN ic.is_included_column = 1 THEN c.name END, CHAR(9)), ISNULL(i.filter_definition, '') "
           "FROM sys.indexes i JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id JOIN sys.columns c ON c.object_id = ic.object_id AND c.column_id = ic.column_id "
           "WHERE i.object_id = %s AND i.type > 0 AND i.is_primary_key = 0 AND i.is_unique_constraint = 0 GROUP BY i.name, i.is_unique, i.type_desc, i.filter_definition ORDER BY i.name", oid);
    add(o, "SELECT TOP 200 x FROM (SELECT DISTINCT 'view ' + OBJECT_SCHEMA_NAME(d.referencing_id) + '.' + OBJECT_NAME(d.referencing_id) AS x FROM sys.sql_expression_dependencies d WHERE d.referenced_id = %s "
           "UNION ALL SELECT 'foreign key ' + fk.name + ' on ' + OBJECT_SCHEMA_NAME(fk.parent_object_id) + '.' + OBJECT_NAME(fk.parent_object_id) FROM sys.foreign_keys fk WHERE fk.referenced_object_id = %s AND fk.parent_object_id <> fk.referenced_object_id) z ORDER BY x", oid, oid);
}
static void ms_type(ResultSet *const *rs, int r, char *out, int n) {
    const char *ty = cell(rs, 1, r, 2); int ml = atoi(cell(rs, 1, r, 3)), p = atoi(cell(rs, 1, r, 4)), s = atoi(cell(rs, 1, r, 5));
    if (!strcmp(ty, "nvarchar") || !strcmp(ty, "nchar")) { if (ml < 0) snprintf(out, (size_t)n, "%s(max)", ty); else snprintf(out, (size_t)n, "%s(%d)", ty, ml / 2); }
    else if (!strcmp(ty, "varchar") || !strcmp(ty, "char") || !strcmp(ty, "varbinary") || !strcmp(ty, "binary")) { if (ml < 0) snprintf(out, (size_t)n, "%s(max)", ty); else snprintf(out, (size_t)n, "%s(%d)", ty, ml); }
    else if (!strcmp(ty, "decimal") || !strcmp(ty, "numeric")) snprintf(out, (size_t)n, "%s(%d,%d)", ty, p, s);
    else if (!strcmp(ty, "datetime2") || !strcmp(ty, "datetimeoffset") || !strcmp(ty, "time")) { if (s != 7) snprintf(out, (size_t)n, "%s(%d)", ty, s); else snprintf(out, (size_t)n, "%s", ty); }
    else snprintf(out, (size_t)n, "%s", ty);
}
static bool ms_parse(const char *s, const char *tn, ResultSet *const *rs, int n, TdTable *t, char *err, int errn) {
    if (n < 5) { snprintf(err, (size_t)errn, "incomplete catalog read"); return false; }
    if (!rs[0] || !rs[0]->nrows) { snprintf(err, (size_t)errn, "table %s.%s was not found (or this user cannot see it)", s, tn); return false; }
    snprintf(t->schema, sizeof t->schema, "%s", s); snprintf(t->name, sizeof t->name, "%s", tn);
    t->exists = true;
    if (atoi(cell(rs, 0, 0, 0)) == 2) snprintf(t->special, sizeof t->special, "a system-versioned temporal table: change it with its history table from a console");
    else if (truthy(cell(rs, 0, 0, 1))) snprintf(t->special, sizeof t->special, "a memory-optimized table");
    else if (truthy(cell(rs, 0, 0, 3))) snprintf(t->special, sizeof t->special, "an external table");
    else if (atoi(cell(rs, 0, 0, 6)) > 1) snprintf(t->special, sizeof t->special, "a partitioned table: partitions are not edited here");
    snprintf(t->comment, sizeof t->comment, "%s", cell(rs, 0, 0, 2));
    t->can_alter = truthy(cell(rs, 0, 0, 4));
    if (!t->can_alter) snprintf(t->alter_why, sizeof t->alter_why, "This login has no ALTER permission on %s.%s", s, tn);
    t->approx_rows = isnull(rs, 0, 0, 5) ? -1 : atoll(cell(rs, 0, 0, 5));
    for (int r = 0; rs[1] && r < rs[1]->nrows; r++) {
        char ty[128]; ms_type(rs, r, ty, sizeof ty);
        TdCol *c = td_add_col(t, cell(rs, 1, r, 1), ty);
        c->notnull = !truthy(cell(rs, 1, r, 6));
        if (truthy(cell(rs, 1, r, 7))) c->identity = 2;
        if (truthy(cell(rs, 1, r, 8))) { snprintf(c->gen, sizeof c->gen, "%s", cell(rs, 1, r, 9)); strip_parens(c->gen); c->gen_stored = truthy(cell(rs, 1, r, 10)); c->type[0] = 0; }
        if (!isnull(rs, 1, r, 11)) { snprintf(c->def_name, sizeof c->def_name, "%s", cell(rs, 1, r, 11)); snprintf(c->def, sizeof c->def, "%s", cell(rs, 1, r, 12)); strip_parens(c->def); }
        snprintf(c->comment, sizeof c->comment, "%s", cell(rs, 1, r, 13));
        if (!isnull(rs, 1, r, 14) && strcmp(cell(rs, 1, r, 14), cell(rs, 1, r, 15))) snprintf(c->collation, sizeof c->collation, "%s", cell(rs, 1, r, 14));
        if (truthy(cell(rs, 1, r, 16))) snprintf(c->extra, sizeof c->extra, "user-defined type");
        if (truthy(cell(rs, 1, r, 17))) snprintf(c->extra, sizeof c->extra, "SPARSE");
    }
    if (!t->ncols) { snprintf(err, (size_t)errn, "no column of %s.%s is visible", s, tn); return false; }
    for (int r = 0; rs[2] && r < rs[2]->nrows; r++) {
        const char *ty = cell(rs, 2, r, 1);
        int kind2 = !strcmp(ty, "PK") ? TK_PK : !strcmp(ty, "UQ") ? TK_UNIQUE : !strcmp(ty, "F") ? TK_FK : TK_CHECK;
        TdKey *k = td_add_key(t, kind2, cell(rs, 2, r, 0));
        add_cols_by_name(t, k, cell(rs, 2, r, 2), '\t');
        if (kind2 == TK_FK) {
            snprintf(k->ref_schema, sizeof k->ref_schema, "%s", cell(rs, 2, r, 3)); snprintf(k->ref_table, sizeof k->ref_table, "%s", cell(rs, 2, r, 4));
            char items[TD_MAXKC][128]; int m = split(cell(rs, 2, r, 5), '\t', items, TD_MAXKC);
            for (int i = 0; i < m; i++) snprintf(k->ref_cols[k->nref++], 128, "%s", items[i]);
            char a1[32], a2[32]; snprintf(a1, sizeof a1, "%s", cell(rs, 2, r, 6)); snprintf(a2, sizeof a2, "%s", cell(rs, 2, r, 7));
            action(a1, k->on_delete, sizeof k->on_delete); action(a2, k->on_update, sizeof k->on_update);
        }
        if (kind2 == TK_CHECK) { snprintf(k->check, sizeof k->check, "%s", cell(rs, 2, r, 7)); strip_parens(k->check); }
        if (truthy(cell(rs, 2, r, 8))) snprintf(k->raw, sizeof k->raw, "disabled (NOCHECK) %s", kind2 == TK_FK ? "foreign key" : "check");
    }
    for (int r = 0; rs[3] && r < rs[3]->nrows; r++) {
        TdKey *k = td_add_key(t, TK_INDEX, cell(rs, 3, r, 0));
        k->unique = truthy(cell(rs, 3, r, 1));
        const char *ty = cell(rs, 3, r, 2);
        if (strcmp(ty, "NONCLUSTERED")) { snprintf(k->raw, sizeof k->raw, "%s index", ty); continue; }
        add_cols_by_name(t, k, cell(rs, 3, r, 3), '\t');
        char inc[TD_MAXKC][128]; int m = split(cell(rs, 3, r, 4), '\t', inc, TD_MAXKC);
        for (int i = 0; i < m; i++) { int id = col_by_name(t, inc[i]); if (id) k->include[k->ninclude++] = id; }
        snprintf(k->where, sizeof k->where, "%s", cell(rs, 3, r, 5)); strip_parens(k->where);
    }
    int w = 0; t->deps[0] = 0;
    for (int r = 0; rs[4] && r < rs[4]->nrows && w < (int)sizeof t->deps - 1; r++) w += snprintf(t->deps + w, sizeof t->deps - (size_t)w, "%s\n", cell(rs, 4, r, 0));
    t->fingerprint = fp_rs(rs, 4);
    return true;
}

/* ======================= entry ======================= */

bool td_snapshot_sql(int d, const char *schema, const char *name, TdSql *out) {
    memset(out, 0, sizeof *out);
    switch (d) {
    case TD_SQLITE: sqlite_sql(schema && schema[0] ? schema : "main", name, out); return true;
    case TD_PG: pg_sql(schema, name, out); return true;
    case TD_ORACLE: ora_sql(schema, name, out); return true;
    case TD_MYSQL: case TD_MARIADB: my_sql(d, schema, name, out); return true;
    case TD_MSSQL: ms_sql(schema, name, out); return true;
    }
    return false;
}
bool td_snapshot_parse(int d, const char *schema, const char *name, ResultSet *const *rs, int n, TdTable *out, char *err, int errn) {
    td_init(out);
    bool ok = false;
    switch (d) {
    case TD_SQLITE: ok = sqlite_parse(schema && schema[0] ? schema : "main", name, rs, n, out, err, errn); break;
    case TD_PG: ok = pg_parse(schema, name, rs, n, out, err, errn); break;
    case TD_ORACLE: ok = ora_parse(schema, name, rs, n, out, err, errn); break;
    case TD_MYSQL: case TD_MARIADB: ok = my_parse(d, schema, name, rs, n, out, err, errn); break;
    case TD_MSSQL: ok = ms_parse(schema, name, rs, n, out, err, errn); break;
    default: snprintf(err, (size_t)errn, "no table designer for this engine");
    }
    if (!ok) { td_free(out); td_init(out); }
    return ok;
}
