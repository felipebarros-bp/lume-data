/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Object migration planner (OBJ-035 / 039 / 040 / 046..057 / 067 / 068 / 069)
 * ============================================================================
 *
 * Turns "the stored definition" + "the edited text" into the exact statements that would publish
 * the change, with the risk, the atomicity and every reason not to run it. Pure functions: no
 * connection, no UI. Engine rules (spec section 4):
 *
 *   SQLite      views / triggers have no OR REPLACE: DROP + CREATE inside one transaction, with
 *               the INSTEAD OF triggers a dropped view would lose recreated after it.
 *   PostgreSQL  CREATE OR REPLACE for views (column contract kept: same names / types / order,
 *               new columns only at the end), functions and procedures (same identity signature,
 *               same return type) and triggers (14+). Anything else is a DROP + CREATE that is
 *               offered only on explicit request, never with CASCADE, inside one transaction.
 *   Oracle      CREATE OR REPLACE; each DDL commits on its own; the compile status is read back
 *               after Apply by the deployment (VALID / INVALID is never assumed).
 *   MySQL       routine bodies need DROP + CREATE (definer, grants, downtime): explicit consent.
 *   MariaDB     CREATE OR REPLACE for routines / triggers when written so.
 *   SQL Server  CREATE OR ALTER (2016 SP1+) or ALTER.
 *   MySQL, MariaDB and SQL Server have no Lume driver: the plan is export-only.
 */
#include "objplan_int.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ------------------------------------------------------------------------ */
/* tokens                                                                    */
/* ------------------------------------------------------------------------ */

/* Significant tokens of a text (no whitespace / comments). */
typedef struct { SqlTok *t; int n; const char *s; int len; } Toks;
static void toks_lex(Toks *k, const char *s, int len, int d) {
    SqlTok *all = malloc(sizeof *all * (size_t)(len + 1));
    int n = sql_lex(s, len, d, all, len + 1), m = 0;
    for (int i = 0; i < n; i++) if (all[i].kind != TK_WS && all[i].kind != TK_COMMENT) all[m++] = all[i];
    k->t = all; k->n = m; k->s = s; k->len = len;
}
static bool kw(const Toks *k, int i, const char *w) { return i >= 0 && i < k->n && tok_is(k->s, &k->t[i], w); }
static bool is_name_tok(const Toks *k, int i) { return i < k->n && (k->t[i].kind == TK_WORD || k->t[i].kind == TK_QIDENT); }

/* Identifier text as the engine stores it: quotes removed; unquoted names folded (PostgreSQL
 * lower case, Oracle upper case, the others kept as written). */
static void ident_text(const Toks *k, int i, int d, char *out, int n) {
    const SqlTok *t = &k->t[i]; const char *p = k->s + t->off; int l = t->len, o = 0;
    if (t->kind == TK_QIDENT && l >= 2) {
        char close = p[0] == '[' ? ']' : p[0];
        for (int j = 1; j < l - 1 && o < n - 1; j++) { out[o++] = p[j]; if (p[j] == close && j + 1 < l - 1 && p[j + 1] == close) j++; }
    } else {
        for (int j = 0; j < l && o < n - 1; j++)
            out[o++] = d == DIALECT_PG ? (char)tolower((unsigned char)p[j]) : d == DIALECT_ORACLE ? (char)toupper((unsigned char)p[j]) : p[j];
    }
    out[o] = 0;
}
/* Names compare exactly when the engine folds them, case-insensitively otherwise. */
static bool same_ident(const char *a, const char *b, int d) {
    return d == DIALECT_PG || d == DIALECT_ORACLE ? !strcmp(a, b) : !strcasecmp(a, b);
}

/* Index of the token closing the parenthesis opened at i, or k->n. */
static int match_paren(const Toks *k, int i) {
    int depth = 0;
    for (int j = i; j < k->n; j++) {
        if (k->t[j].kind == TK_LPAREN) depth++;
        else if (k->t[j].kind == TK_RPAREN && --depth == 0) return j;
    }
    return k->n;
}

/* ------------------------------------------------------------------------ */
/* the CREATE header                                                         */
/* ------------------------------------------------------------------------ */

/* Reads "a", "a.b" or "a.b.c" at *i into schema / name (the last part is the name). */
static bool read_qname(const Toks *k, int *i, int d, char *schema, char *name, int *start, int *end) {
    if (!is_name_tok(k, *i)) return false;
    char parts[3][128]; int np = 0;
    *start = k->t[*i].off;
    for (;;) {
        if (np < 3) ident_text(k, *i, d, parts[np++], 128);
        *end = k->t[*i].off + k->t[*i].len;
        if (*i + 2 < k->n + 1 && *i + 1 < k->n && k->t[*i + 1].kind == TK_DOT && is_name_tok(k, *i + 2)) *i += 2;
        else break;
    }
    (*i)++;
    snprintf(name, 128, "%s", parts[np - 1]);
    if (np >= 2) snprintf(schema, 128, "%s", parts[np - 2]); else schema[0] = 0;
    return true;
}

static bool routine_attr(const Toks *k, int i) {
    static const char *stop[] = { "LANGUAGE", "AS", "IMMUTABLE", "STABLE", "VOLATILE", "STRICT", "SECURITY", "COST", "ROWS",
        "SET", "PARALLEL", "LEAKPROOF", "WINDOW", "CALLED", "BEGIN", "IS", "DETERMINISTIC", "PIPELINED", "RESULT_CACHE",
        "AUTHID", "NOT", "CONTAINS", "READS", "MODIFIES", "COMMENT", "WITH", "NO", "TRANSFORM", "SUPPORT", "EXTERNAL",
        "ACCESSIBLE", "PARALLEL_ENABLE", "AGGREGATE", "RETURN", "SQL", "DEFAULT", NULL };
    if (k->t[i].kind == TK_SEMI || k->t[i].kind == TK_BODY || k->t[i].kind == TK_STRING) return true;
    for (int j = 0; stop[j]; j++) if (kw(k, i, stop[j])) return true;
    return false;
}

bool obj_parse_header(const char *text, int len, int d, ObjHeader *h) {
    memset(h, 0, sizeof *h);
    h->args_start = h->args_end = h->select_start = -1; h->kind = -1;
    Toks k; toks_lex(&k, text, len, d);
    int i = 0;
    if (kw(&k, 0, "CREATE")) {
        h->start = k.t[0].off; i = 1;
        if (kw(&k, i, "OR") && kw(&k, i + 1, "REPLACE")) { h->or_replace = true; i += 2; }
        else if (kw(&k, i, "OR") && kw(&k, i + 1, "ALTER")) { h->or_alter = true; i += 2; }
        h->verb_end = k.t[i - 1].off + k.t[i - 1].len;
    } else if (kw(&k, 0, "ALTER")) {
        h->start = k.t[0].off; h->alter_only = true; i = 1; h->verb_end = k.t[0].off + k.t[0].len;
    } else { free(k.t); return false; }
    /* modifiers before the object kind */
    for (;;) {
        if (kw(&k, i, "EDITIONABLE") || kw(&k, i, "NONEDITIONABLE") || kw(&k, i, "FORCE") || kw(&k, i, "NOFORCE") ||
            kw(&k, i, "TEMP") || kw(&k, i, "TEMPORARY") || kw(&k, i, "RECURSIVE") || kw(&k, i, "AGGREGATE") ||
            kw(&k, i, "CONSTRAINT") || kw(&k, i, "EDITIONING") || kw(&k, i, "SECURE")) { i++; continue; }
        if (kw(&k, i, "NO") && kw(&k, i + 1, "FORCE")) { i += 2; continue; }
        if ((kw(&k, i, "DEFINER") || kw(&k, i, "ALGORITHM")) && i + 2 < k.n && k.t[i + 1].kind == TK_OP) {
            i += 3;   /* DEFINER = user@host: user, @host are one or more tokens */
            while (i < k.n && (k.t[i].kind == TK_OP || (i > 0 && k.t[i - 1].kind == TK_OP && is_name_tok(&k, i)) || k.t[i].kind == TK_STRING)) i++;
            continue;
        }
        if (kw(&k, i, "SQL") && kw(&k, i + 1, "SECURITY")) { i += 3; continue; }
        break;
    }
    if (kw(&k, i, "VIEW")) { h->kind = OBJK_VIEW; i++; }
    else if (kw(&k, i, "MATERIALIZED") && kw(&k, i + 1, "VIEW")) { h->kind = OBJK_MATVIEW; i += 2; }
    else if (kw(&k, i, "FUNCTION")) { h->kind = OBJK_FUNCTION; i++; }
    else if (kw(&k, i, "PROCEDURE") || kw(&k, i, "PROC")) { h->kind = OBJK_PROCEDURE; i++; }
    else if (kw(&k, i, "PACKAGE") && kw(&k, i + 1, "BODY")) { h->kind = OBJK_PKG_BODY; i += 2; }
    else if (kw(&k, i, "PACKAGE")) { h->kind = OBJK_PKG_SPEC; i++; }
    else if (kw(&k, i, "TRIGGER")) { h->kind = OBJK_TRIGGER; i++; }
    else if (kw(&k, i, "TYPE") && kw(&k, i + 1, "BODY")) { h->kind = OBJK_TYPE_BODY; i += 2; }
    else if (kw(&k, i, "TYPE")) { h->kind = OBJK_TYPE_SPEC; i++; }
    else { free(k.t); return false; }
    if (kw(&k, i, "IF") && kw(&k, i + 1, "NOT") && kw(&k, i + 2, "EXISTS")) { h->if_not_exists = true; i += 3; }
    if (!read_qname(&k, &i, d, h->schema, h->name, &h->name_start, &h->name_end)) { free(k.t); return false; }
    h->found = true;
    /* routines: arguments and return type */
    if (h->kind == OBJK_FUNCTION || h->kind == OBJK_PROCEDURE) {
        if (i < k.n && k.t[i].kind == TK_LPAREN) {
            int c = match_paren(&k, i);
            h->args_start = k.t[i].off + 1; h->args_end = c < k.n ? k.t[c].off : len;
            i = c + 1;
        }
        if (kw(&k, i, "RETURNS") || (d == DIALECT_ORACLE && kw(&k, i, "RETURN"))) {
            int s = ++i, e = i;
            while (e < k.n) {
                if (k.t[e].kind == TK_LPAREN) { e = match_paren(&k, e) + 1; continue; }
                if (e > s && routine_attr(&k, e)) break;
                if (d == DIALECT_PG && e > s && kw(&k, e, "NULL")) break;
                e++;
            }
            if (e > s) {
                int a = k.t[s].off, b = k.t[e - 1].off + k.t[e - 1].len, l = b - a;
                if (l > (int)sizeof h->returns - 1) l = (int)sizeof h->returns - 1;
                memcpy(h->returns, text + a, (size_t)l); h->returns[l] = 0;
            }
            i = e;
        }
    }
    /* triggers: the table after ON */
    if (h->kind == OBJK_TRIGGER) {
        for (int j = i; j < k.n; j++) {
            if (kw(&k, j, "ON")) {
                int q = j + 1; char sch[128], nm[128]; int a, b;
                if (read_qname(&k, &q, d, sch, nm, &a, &b)) {
                    if (sch[0]) snprintf(h->table, sizeof h->table, "%s.%s", sch, nm); else snprintf(h->table, sizeof h->table, "%s", nm);
                }
                break;
            }
            if (kw(&k, j, "BEGIN") || kw(&k, j, "EXECUTE") || kw(&k, j, "DECLARE")) break;
        }
    }
    /* views: the query after AS */
    if (h->kind == OBJK_VIEW || h->kind == OBJK_MATVIEW) {
        int j = i;
        if (j < k.n && k.t[j].kind == TK_LPAREN) j = match_paren(&k, j) + 1;          /* column list */
        while (j < k.n && !kw(&k, j, "AS")) { if (k.t[j].kind == TK_LPAREN) j = match_paren(&k, j); j++; }   /* WITH (...) / SCHEMABINDING ... */
        if (j + 1 < k.n) h->select_start = k.t[j + 1].off;
    }
    /* where the definition ends and whether more statements follow */
    bool procedural = !(h->kind == OBJK_VIEW || h->kind == OBJK_MATVIEW) && d != DIALECT_PG;
    if (d == DIALECT_PG) for (int j = i; j < k.n; j++) if (kw(&k, j, "BEGIN") && kw(&k, j + 1, "ATOMIC")) procedural = true;
    if (!procedural) {
        int depth = 0, end = -1;
        for (int j = i; j < k.n; j++) {
            if (k.t[j].kind == TK_LPAREN) depth++;
            else if (k.t[j].kind == TK_RPAREN && depth) depth--;
            else if (depth == 0 && (k.t[j].kind == TK_SEMI || k.t[j].kind == TK_BATCH || (d == DIALECT_ORACLE && k.t[j].kind == TK_OP && k.t[j].len == 1 && text[k.t[j].off] == '/'))) {
                if (end < 0) { end = j; h->body_end = k.t[j].off; }
                else if (k.t[j].kind == TK_SEMI && k.t[j - 1].kind != TK_SEMI && j - 1 > end) h->extra_statements++;
                continue;
            }
            if (end >= 0 && j == end + 1) h->extra_statements++;   /* something after the first terminator */
        }
        if (end < 0) h->body_end = k.n ? k.t[k.n - 1].off + k.t[k.n - 1].len : len;
        /* count each extra statement once: "a; b; c" after the definition = 2 */
        if (end >= 0) {
            int extra = 0; bool in_stmt = false;
            for (int j = end + 1; j < k.n; j++) {
                bool term = k.t[j].kind == TK_SEMI || k.t[j].kind == TK_BATCH || (d == DIALECT_ORACLE && k.t[j].kind == TK_OP && k.t[j].len == 1 && text[k.t[j].off] == '/');
                if (term) in_stmt = false; else if (!in_stmt) { in_stmt = true; extra++; }
            }
            h->extra_statements = extra;
        }
    } else {
        /* procedural units: the whole text, without a trailing "/" line (Oracle), GO (SQL
         * Server) or the last ';' after END in PostgreSQL BEGIN ATOMIC bodies */
        int e = k.n - 1;
        while (e >= 0 && ((k.t[e].kind == TK_OP && k.t[e].len == 1 && text[k.t[e].off] == '/') || k.t[e].kind == TK_BATCH)) e--;
        if (d == DIALECT_PG && e >= 0 && k.t[e].kind == TK_SEMI) e--;
        h->body_end = e >= 0 ? k.t[e].off + k.t[e].len : len;
        /* Oracle: a unit ends at the first "/" line; anything after it is another statement */
        if (d == DIALECT_ORACLE)
            for (int j = i; j < k.n; j++)
                if (k.t[j].kind == TK_OP && k.t[j].len == 1 && text[k.t[j].off] == '/' && (k.t[j].off == 0 || text[k.t[j].off - 1] == '\n')) {
                    h->body_end = k.t[j - 1].off + k.t[j - 1].len;
                    bool more = false; for (int q = j + 1; q < k.n; q++) if (!(k.t[q].kind == TK_OP && text[k.t[q].off] == '/')) more = true;
                    h->extra_statements = more ? 1 : 0;
                    break;
                }
        if (d == DIALECT_MSSQL)
            for (int j = i; j < k.n; j++) if (k.t[j].kind == TK_BATCH) {
                h->body_end = j > 0 ? k.t[j - 1].off + k.t[j - 1].len : len;
                bool more = false; for (int q = j + 1; q < k.n; q++) if (k.t[q].kind != TK_BATCH) more = true;
                h->extra_statements = more ? 1 : 0;
                break;
            }
    }
    free(k.t);
    return true;
}

/* ------------------------------------------------------------------------ */
/* PostgreSQL identity signatures                                           */
/* ------------------------------------------------------------------------ */

void pg_norm_type(const char *t, int len, char *out, int n) {
    /* lower case, single spaces, typmods "(...)" removed, array brackets kept */
    char tmp[256]; int o = 0, depth = 0; bool sp = false;
    for (int i = 0; i < len && o < (int)sizeof tmp - 2; i++) {
        char c = t[i];
        if (c == '(') { depth++; continue; }
        if (c == ')') { if (depth) depth--; continue; }
        if (depth) continue;
        if (isspace((unsigned char)c)) { sp = o > 0; continue; }
        if (c == '"') continue;
        if (sp && c != '[' ) tmp[o++] = ' ';
        sp = false;
        tmp[o++] = (char)tolower((unsigned char)c);
    }
    tmp[o] = 0;
    /* "integer []" -> "integer[]" */
    char arr[16] = ""; char *br = strchr(tmp, '[');
    if (br) { snprintf(arr, sizeof arr, "[]"); while (br > tmp && br[-1] == ' ') br--; *br = 0; }
    /* drop "pg_catalog." / "public." for built-in names */
    const char *base = tmp;
    if (!strncmp(base, "pg_catalog.", 11)) base += 11;
    static const char *alias[][2] = {
        { "int", "integer" }, { "int4", "integer" }, { "int8", "bigint" }, { "int2", "smallint" }, { "bool", "boolean" },
        { "varchar", "character varying" }, { "char", "character" }, { "bpchar", "character" }, { "float8", "double precision" },
        { "float", "double precision" }, { "float4", "real" }, { "decimal", "numeric" }, { "timestamptz", "timestamp with time zone" },
        { "timestamp", "timestamp without time zone" }, { "timetz", "time with time zone" }, { "time", "time without time zone" },
        { "varbit", "bit varying" }, { "serial", "integer" }, { "bigserial", "bigint" }, { "serial4", "integer" }, { "serial8", "bigint" },
        { NULL, NULL } };
    for (int i = 0; alias[i][0]; i++) if (!strcmp(base, alias[i][0])) { base = alias[i][1]; break; }
    snprintf(out, (size_t)n, "%s%s", base, arr);
}

/* First words of type names that can stand alone in an argument (so "date" alone is a type). */
static bool type_continues(const char *first, const char *second) {
    static const char *pairs[][2] = { { "double", "precision" }, { "character", "varying" }, { "char", "varying" }, { "bit", "varying" },
        { "time", "with" }, { "time", "without" }, { "timestamp", "with" }, { "timestamp", "without" }, { "interval", "year" },
        { "interval", "month" }, { "interval", "day" }, { "interval", "hour" }, { "interval", "minute" }, { "interval", "second" },
        { "national", "character" }, { NULL, NULL } };
    for (int i = 0; pairs[i][0]; i++) if (!strcasecmp(first, pairs[i][0]) && !strcasecmp(second, pairs[i][1])) return true;
    return false;
}

bool pg_identity_types(const char *args, int len, bool procedure, char *out, int n) {
    Toks k; toks_lex(&k, args, len, DIALECT_PG);
    out[0] = 0; int o = 0; bool ok = true;
    int s = 0;
    while (s < k.n) {
        /* one argument: tokens up to a top-level comma */
        int e = s, depth = 0;
        while (e < k.n && !(depth == 0 && k.t[e].kind == TK_COMMA)) {
            if (k.t[e].kind == TK_LPAREN) depth++; else if (k.t[e].kind == TK_RPAREN) depth--;
            e++;
        }
        int a = s, b = e;
        /* default value */
        for (int j = a; j < b; j++) if (kw(&k, j, "DEFAULT") || (k.t[j].kind == TK_OP && k.t[j].len == 1 && args[k.t[j].off] == '=')) { b = j; break; }
        /* mode */
        bool out_arg = false;
        if (kw(&k, a, "IN") || kw(&k, a, "VARIADIC")) a++;
        else if (kw(&k, a, "OUT")) { out_arg = true; a++; }
        else if (kw(&k, a, "INOUT")) a++;
        if (a >= b) { ok = false; break; }
        /* name: present when two or more "words" and the first does not start a multi-word type */
        int words = 0; for (int j = a; j < b; j++) if (is_name_tok(&k, j) && !(j > a && k.t[j - 1].kind == TK_DOT)) words++;
        bool named = false;
        if (words >= 2 && is_name_tok(&k, a) && !(a + 1 < b && k.t[a + 1].kind == TK_DOT)) {
            char w1[64], w2[64];
            snprintf(w1, sizeof w1, "%.*s", k.t[a].len, args + k.t[a].off);
            snprintf(w2, sizeof w2, "%.*s", k.t[a + 1].len, args + k.t[a + 1].off);
            named = !type_continues(w1, w2);
        }
        if (named) a++;
        if (!(out_arg && !procedure)) {
            if (a >= b) { ok = false; break; }
            char ty[256]; int ta = k.t[a].off, tb = k.t[b - 1].off + k.t[b - 1].len;
            pg_norm_type(args + ta, tb - ta, ty, sizeof ty);
            o += snprintf(out + o, (size_t)(n > o ? n - o : 0), "%s%s", o ? ", " : "", ty);
        }
        s = e + 1;
    }
    free(k.t);
    return ok;
}

/* "integer, text" from the catalog, each part normalised the same way. */
void pg_norm_signature(const char *sig, char *out, int n) {
    out[0] = 0; int o = 0;
    const char *p = sig;
    while (*p) {
        while (*p == ' ') p++;
        const char *e = p; int depth = 0;
        while (*e && !(depth == 0 && *e == ',')) { if (*e == '(') depth++; else if (*e == ')') depth--; e++; }
        if (e > p) {
            char ty[256]; pg_norm_type(p, (int)(e - p), ty, sizeof ty);
            o += snprintf(out + o, (size_t)(n > o ? n - o : 0), "%s%s", o ? ", " : "", ty);
        }
        p = *e ? e + 1 : e;
    }
}

/* Return types compared after normalisation ("SETOF int" == "SETOF integer"). TABLE(...)
 * results are compared as written, without spaces and case. */
void pg_norm_result(const char *r, char *out, int n) {
    while (isspace((unsigned char)*r)) r++;
    if (!strncasecmp(r, "SETOF", 5) && isspace((unsigned char)r[5])) {
        char t[200]; pg_norm_type(r + 6, (int)strlen(r + 6), t, sizeof t);
        snprintf(out, (size_t)n, "setof %s", t); return;
    }
    if (!strncasecmp(r, "TABLE", 5)) {
        int o = 0; for (const char *p = r; *p && o < n - 1; p++) if (!isspace((unsigned char)*p) && *p != '"') out[o++] = (char)tolower((unsigned char)*p);
        out[o] = 0; return;
    }
    pg_norm_type(r, (int)strlen(r), out, n);
}

bool pg_view_contract_ok(const ObjColumn *before, int nb, const ObjColumn *now, int nn, char *why, int n) {
    for (int i = 0; i < nb; i++) {
        if (i >= nn) { snprintf(why, (size_t)n, "column \"%s\" would be removed (PostgreSQL cannot drop a column with CREATE OR REPLACE VIEW)", before[i].name); return false; }
        if (strcmp(before[i].name, now[i].name)) {
            snprintf(why, (size_t)n, "column %d changes name: \"%s\" -> \"%s\" (names and order must stay)", i + 1, before[i].name, now[i].name); return false;
        }
        char a[128], b[128];
        pg_norm_type(before[i].type, (int)strlen(before[i].type), a, sizeof a);
        pg_norm_type(now[i].type, (int)strlen(now[i].type), b, sizeof b);
        if (a[0] && b[0] && strcmp(a, b)) {
            snprintf(why, (size_t)n, "column \"%s\" changes type: %s -> %s", before[i].name, before[i].type, now[i].type); return false;
        }
    }
    why[0] = 0;
    return true;
}

/* ------------------------------------------------------------------------ */
/* plan building helpers (shared with the adapters, objplan_int.h)           */
/* ------------------------------------------------------------------------ */

void plan_sql(ObjPlan *p, const char *fmt, ...) {
    if (p->nsql >= PLAN_MAX_SQL) return;
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *s = malloc((size_t)need + 1); vsnprintf(s, (size_t)need + 1, fmt, ap2); va_end(ap2);
    p->sql[p->nsql++] = s;
}
void plan_raw(ObjPlan *p, const char *s, int len) {
    if (p->nsql >= PLAN_MAX_SQL) return;
    char *c = malloc((size_t)len + 1); memcpy(c, s, (size_t)len); c[len] = 0; p->sql[p->nsql++] = c;
}
void plan_create(ObjPlan *p, const char *s, int len) { p->create_index = p->nsql; plan_raw(p, s, len); }
void plan_impact(ObjPlan *p, const char *fmt, ...) {
    if (p->nimpact >= PLAN_MAX_NOTES) return;
    va_list ap; va_start(ap, fmt); vsnprintf(p->impact[p->nimpact++], sizeof p->impact[0], fmt, ap); va_end(ap);
}
void plan_pre(ObjPlan *p, const char *fmt, ...) {
    if (p->npre >= PLAN_MAX_NOTES) return;
    va_list ap; va_start(ap, fmt); vsnprintf(p->pre[p->npre++], sizeof p->pre[0], fmt, ap); va_end(ap);
}
void plan_block(ObjPlan *p, const char *fmt, ...) {
    if (p->blocked) return;
    p->blocked = true;
    va_list ap; va_start(ap, fmt); vsnprintf(p->block_reason, sizeof p->block_reason, fmt, ap); va_end(ap);
}

static void quote_with(const char *s, char open, char close, char *out, int n) {
    int o = 0;
    if (n < 3) { if (n > 0) out[0] = 0; return; }
    out[o++] = open;
    for (; *s && o < n - 3; s++) { if (*s == close) out[o++] = close; out[o++] = *s; }
    out[o++] = close; out[o] = 0;
}
void obj_qname(const ObjKey *k, int d, char *out, int n) {
    char open = '"', close = '"';
    if (d == DIALECT_MYSQL || d == DIALECT_MARIADB) open = close = '`';
    else if (d == DIALECT_MSSQL) { open = '['; close = ']'; }
    char a[300], b[300];
    quote_with(k->name, open, close, a, sizeof a);
    if (k->schema[0]) { quote_with(k->schema, open, close, b, sizeof b); snprintf(out, (size_t)n, "%s.%s", b, a); }
    else snprintf(out, (size_t)n, "%s", a);
}
void obj_qtable(const char *t, int d, char *out, int n) {
    ObjKey k; memset(&k, 0, sizeof k);
    const char *dot = strchr(t, '.');
    if (dot) { snprintf(k.schema, sizeof k.schema, "%.*s", (int)(dot - t), t); snprintf(k.name, sizeof k.name, "%s", dot + 1); }
    else snprintf(k.name, sizeof k.name, "%s", t);
    obj_qname(&k, d, out, n);
}
void obj_qlit(const char *s, char *out, int n) { quote_with(s, '\'', '\'', out, n); }
int obj_trim_semicolon(const char *s, int a, int e) {
    while (e > a && (s[e - 1] == ';' || isspace((unsigned char)s[e - 1]))) e--;
    return e;
}

/* text without leading / trailing whitespace */
static void trim_range(const char *s, int *a, int *b) {
    while (*a < *b && isspace((unsigned char)s[*a])) (*a)++;
    while (*b > *a && isspace((unsigned char)s[*b - 1])) (*b)--;
}
static bool same_text(const char *a, int al, const char *b, int bl) {
    int x0 = 0, y0 = 0; trim_range(a, &x0, &al); trim_range(b, &y0, &bl);
    /* a trailing ';' or '/' is not a change */
    while (al > x0 && (a[al - 1] == ';' || a[al - 1] == '/' || isspace((unsigned char)a[al - 1]))) al--;
    while (bl > y0 && (b[bl - 1] == ';' || b[bl - 1] == '/' || isspace((unsigned char)b[bl - 1]))) bl--;
    return al - x0 == bl - y0 && !memcmp(a + x0, b + y0, (size_t)(al - x0));
}

void plan_deps_impact(ObjPlan *p, const ObjSnapshot *b, const char *what) {
    if (!b) return;
    if (!b->deps_known) { plan_impact(p, "Dependent objects: not known for this engine; check them before %s.", what); return; }
    if (b->ndeps == 0) { plan_impact(p, "No dependent objects found in the catalog."); return; }
    char list[200] = ""; int o = 0;
    for (int i = 0; i < b->ndeps && i < 4; i++) o += snprintf(list + o, sizeof list - (size_t)o, "%s%s", i ? ", " : "", b->deps[i].name);
    plan_impact(p, "%d dependent object(s): %s%s.", b->ndeps, list, b->ndeps > 4 ? ", ..." : "");
}

/* ------------------------------------------------------------------------ */
/* the planner core: checks every engine shares, then the adapter            */
/* ------------------------------------------------------------------------ */

bool objplan_make(const ObjPlanInput *in, ObjPlan *p) {
    memset(p, 0, sizeof *p);
    p->create_index = -1; p->risk = RISK_UNKNOWN; p->atomicity = ATOM_UNKNOWN;
    if (!in || !in->key || !in->local) return false;
    const int d = in->dialect; const ObjKey *key = in->key; const ObjSnapshot *base = in->base;
    const ObjEngine *eng = obj_engine_for_dialect(d);
    char disp[700]; objkey_display(key, disp, sizeof disp);
    ObjCaps caps = eng ? eng->caps(key->kind, in->server_version) : (ObjCaps){ 0 };
    if (!eng || !caps.source_edit || !eng->plan) {
        plan_block(p, "%s: editing a %s is not supported on %s.", disp, objkind_word(key->kind), dialect_name(d));
        return true;
    }
    p->script_only = !(eng->has_driver && in->has_driver);

    /* the edited text must be one complete definition of this very object (OBJ-068 / 069) */
    ObjHeader h;
    if (!obj_parse_header(in->local, in->local_len, d, &h)) {
        plan_block(p, "The text is not a complete CREATE statement for the %s. Lume does not publish incomplete DDL%s%s.",
                   objkind_word(key->kind), base && base->missing[0] ? ": " : "", base && base->missing[0] ? base->missing : "");
        return true;
    }
    if (h.kind != key->kind) { plan_block(p, "The text defines a %s, but this tab edits %s.", objkind_word(h.kind), disp); return true; }
    if (!same_ident(h.name, key->name, d) || (h.schema[0] && key->schema[0] && !same_ident(h.schema, key->schema, d))) {
        plan_block(p, "The text defines %s%s%s, not %s. Renaming is a separate, reviewed operation.", h.schema, h.schema[0] ? "." : "", h.name, disp);
        return true;
    }
    if (key->kind == OBJK_TRIGGER && key->table[0] && h.table[0]) {
        const char *kt = strrchr(key->table, '.') ? strrchr(key->table, '.') + 1 : key->table;
        const char *ht = strrchr(h.table, '.') ? strrchr(h.table, '.') + 1 : h.table;
        if (!same_ident(kt, ht, d)) { plan_block(p, "The trigger would move from table %s to %s; that is a drop and a new trigger, not an edit.", key->table, h.table); return true; }
    }
    if (h.extra_statements > 0) {
        plan_block(p, "The text holds %d more statement(s) after the definition. Only the object definition is published; move the rest to a query console.", h.extra_statements);
        return true;
    }
    if (h.if_not_exists) { plan_block(p, "IF NOT EXISTS would silently keep the current definition. Remove it."); return true; }
    int cs = h.start, ce = h.body_end; trim_range(in->local, &cs, &ce);
    /* a read-only object is the first thing to say (extension member, aggregate, encrypted) */
    if (base && (base->system || base->encrypted)) plan_block(p, "%s is %s: its source cannot be published.", disp, base->ro_reason[0] ? base->ro_reason : base->system ? "a system object" : "encrypted");
    if (base && ((base->ddl && same_text(in->local, in->local_len, base->ddl, (int)strlen(base->ddl))) ||
                 (base->original && same_text(in->local, in->local_len, base->original, (int)strlen(base->original))))) {
        p->no_changes = true;
        plan_block(p, "No changes: the text is the stored definition.");
    }
    /* concurrent change (AT-04) */
    if (in->remote && base) {
        if (!in->remote->exists) { p->conflict = true; plan_block(p, "%s no longer exists on the server (dropped by another session). Re-create it as a new object or discard the draft.", disp); }
        else if (in->remote->fingerprint != base->fingerprint) { p->conflict = true; plan_block(p, "The definition changed on the server since you opened it. Review the three-way comparison and decide before publishing."); }
        else plan_pre(p, "Remote definition re-read: unchanged since it was opened (fingerprint match).");
    } else plan_pre(p, "The remote definition is re-read and compared just before running.");
    if (in->read_only) plan_block(p, "The connection is read-only%s%s: nothing will be written. The draft is kept and can be exported.", in->ro_reason ? ": " : "", in->ro_reason ? in->ro_reason : "");

    /* BEGIN / COMMIT of a deployment inside the user's own transaction would commit their work
     * with it (PostgreSQL) or fail (SQLite); Oracle / MySQL DDL commits it implicitly */
    if (in->tx_open) plan_block(p, "The session has an open transaction: publishing would commit or mix with it. Commit or roll back first.");
    eng->plan(in, &h, cs, ce, p);

    if (p->script_only && !p->blocked) plan_block(p, "Lume has no %s driver: the script can be exported and run with the engine's own tools.", dialect_name(d));
    return true;
}

/* ------------------------------------------------------------------------ */
/* engine registry                                                           */
/* ------------------------------------------------------------------------ */

static const ObjEngine *engines[DIALECT_MAX];
static int nengines; static bool builtins;
static void ensure_builtins(void) {
    if (builtins) return;
    builtins = true;
    const ObjEngine *b[] = { &obj_engine_sqlite, &obj_engine_pg, &obj_engine_oracle, &obj_engine_mysql, &obj_engine_mariadb, &obj_engine_mssql };
    for (int i = 0; i < 6; i++) obj_engine_register(b[i]);
}
bool obj_engine_register(const ObjEngine *e) {
    ensure_builtins();
    if (!e || e->api_version != OBJ_ENGINE_API || !e->id || !e->caps) return false;
    for (int i = 0; i < nengines; i++) if (!strcmp(engines[i]->id, e->id)) { engines[i] = e; return true; }   /* a module may replace an adapter */
    if (nengines >= DIALECT_MAX) return false;
    engines[nengines++] = e;
    return true;
}
const ObjEngine *obj_engine_for_dialect(int d) {
    ensure_builtins();
    for (int i = 0; i < nengines; i++) if (engines[i]->dialect == d) return engines[i];
    return NULL;
}
const ObjEngine *obj_engine_for_driver(const char *id) {
    ensure_builtins();
    if (!id) return NULL;
    for (int i = 0; i < nengines; i++) if (!strcmp(engines[i]->id, id)) return engines[i];
    return NULL;
}
int obj_engine_count(void) { ensure_builtins(); return nengines; }
const ObjEngine *obj_engine_at(int i) { ensure_builtins(); return i >= 0 && i < nengines ? engines[i] : NULL; }

void obj_sql_add(ObjSqlBuf *b, const char *fmt, ...) {
    if (b->n >= OBJ_SQL_MAX) return;
    va_list ap, ap2; va_start(ap, fmt); va_copy(ap2, ap);
    int need = vsnprintf(NULL, 0, fmt, ap); va_end(ap);
    char *s = malloc((size_t)need + 1); vsnprintf(s, (size_t)need + 1, fmt, ap2); va_end(ap2);
    b->sql[b->n++] = s;
}
void obj_sql_free(ObjSqlBuf *b) { for (int i = 0; i < b->n; i++) free(b->sql[i]); b->n = 0; }
