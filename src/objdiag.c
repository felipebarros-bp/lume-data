/* SPDX-License-Identifier: AGPL-3.0-only */
/* Local diagnostics of an object source: see objdiag.h. */
#include "objdiag.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

typedef struct {
    ObjDiag *out; int n, max;
    const char *text; int len;
} Sink;

static void line_col(const char *t, int len, int off, int *line, int *col) {
    int l = 1, c = 1;
    for (int i = 0; i < off && i < len; i++) { if (t[i] == '\n') { l++; c = 1; } else c++; }
    *line = l; *col = c;
}

static ObjDiag *emit(Sink *s, int src, int sev, const char *code, int start, int end, int conf, const char *fmt, ...) {
    if (s->n >= s->max) return NULL;
    /* the same code at the same place once */
    for (int i = 0; i < s->n; i++) if (s->out[i].start == start && !strcmp(s->out[i].code, code)) return NULL;
    ObjDiag *d = &s->out[s->n++]; memset(d, 0, sizeof *d);
    d->source = src; d->severity = sev; snprintf(d->code, sizeof d->code, "%s", code);
    d->start = start; d->end = end < start ? start : end; d->confidence = conf;
    snprintf(d->unit, sizeof d->unit, "MAIN");
    if (start >= 0) line_col(s->text, s->len, start, &d->line, &d->col);
    va_list ap; va_start(ap, fmt); vsnprintf(d->message, sizeof d->message, fmt, ap); va_end(ap);
    return d;
}
static void fix(ObjDiag *d, const char *label, int a, int b, const char *txt) {
    if (!d) return;
    d->has_fix = true; snprintf(d->fix_label, sizeof d->fix_label, "%s", label);
    d->fix_start = a; d->fix_end = b; snprintf(d->fix_text, sizeof d->fix_text, "%s", txt);
}

char *obj_apply_fix(const char *text, int len, const ObjDiag *dg, int *out_len) {
    if (!dg->has_fix || dg->fix_start < 0 || dg->fix_end < dg->fix_start || dg->fix_end > len) return NULL;
    int fl = (int)strlen(dg->fix_text), n = len - (dg->fix_end - dg->fix_start) + fl;
    char *o = malloc((size_t)n + 1);
    memcpy(o, text, (size_t)dg->fix_start);
    memcpy(o + dg->fix_start, dg->fix_text, (size_t)fl);
    memcpy(o + dg->fix_start + fl, text + dg->fix_end, (size_t)(len - dg->fix_end));
    o[n] = 0; *out_len = n;
    return o;
}

/* significant tokens */
typedef struct { SqlTok *t; int n; const char *s; } Tk;
static void lex_sig(Tk *k, const char *s, int len, int d, bool keep_errors, Sink *sink, int base_off) {
    SqlTok *all = malloc(sizeof *all * (size_t)(len + 1));
    int n = sql_lex(s, len, d, all, len + 1), m = 0;
    for (int i = 0; i < n; i++) {
        if (keep_errors && all[i].unterminated) {
            const char *what = all[i].kind == TK_STRING ? "string" : all[i].kind == TK_COMMENT ? "comment" : all[i].kind == TK_BODY ? "dollar-quoted body" : all[i].kind == TK_QIDENT ? "quoted identifier" : "token";
            emit(sink, DSRC_PARSER, DIAG_ERROR, "LEX001", base_off + all[i].off, base_off + all[i].off + (all[i].len < 40 ? all[i].len : 1), 0, "Unterminated %s: it runs to the end of the text.", what);
        }
        if (all[i].kind != TK_WS && all[i].kind != TK_COMMENT) all[m++] = all[i];
    }
    k->t = all; k->n = m; k->s = s;
}
static bool K(const Tk *k, int i, const char *w) { return i >= 0 && i < k->n && tok_is(k->s, &k->t[i], w); }

/* Parenthesis balance: first unmatched ')' or the last unclosed '('. */
static void check_parens(Sink *s, const Tk *k, int base) {
    int stack[256], sp = 0;
    for (int i = 0; i < k->n; i++) {
        if (k->t[i].kind == TK_LPAREN) { if (sp < 256) stack[sp] = i; sp++; }
        else if (k->t[i].kind == TK_RPAREN) {
            if (sp == 0) { emit(s, DSRC_PARSER, DIAG_ERROR, "SYN001", base + k->t[i].off, base + k->t[i].off + 1, 0, "Unmatched ')'."); return; }
            sp--;
        }
    }
    if (sp > 0) { int i = stack[sp - 1 < 255 ? sp - 1 : 255]; emit(s, DSRC_PARSER, DIAG_ERROR, "SYN002", base + k->t[i].off, base + k->t[i].off + 1, 0, "'(' is never closed."); }
}

/* BEGIN / CASE / IF / LOOP against END in procedural code (approximate: keywords only). */
static void check_blocks(Sink *s, const Tk *k, int from, int base, bool plsql_like, const char *unit) {
    int open = 0, first_open = -1, ifs = 0, loops = 0;
    for (int i = from; i < k->n; i++) {
        if (K(k, i, "END")) {
            if (K(k, i + 1, "IF")) { ifs--; i++; continue; }
            if (K(k, i + 1, "LOOP")) { loops--; i++; continue; }
            if (K(k, i + 1, "CASE")) { open--; i++; continue; }
            if (K(k, i + 1, "WHILE") || K(k, i + 1, "REPEAT") || K(k, i + 1, "FOR")) { loops--; i++; continue; }
            open--;
            if (open < 0) { ObjDiag *d = emit(s, DSRC_PARSER, DIAG_WARNING, "BLK002", base + k->t[i].off, base + k->t[i].off + 3, 2, "END without a matching BEGIN or CASE."); if (d) snprintf(d->unit, sizeof d->unit, "%s", unit); return; }
            continue;
        }
        if (K(k, i, "BEGIN") && !K(k, i + 1, "TRANSACTION") && !K(k, i + 1, "TRAN")) { if (open == 0) first_open = i; open++; }
        else if (K(k, i, "CASE")) { if (open == 0) first_open = i; open++; }
        else if (plsql_like && K(k, i, "IF") && !K(k, i - 1, "END") && !K(k, i + 1, "EXISTS") && !K(k, i - 1, "ELSE")) ifs++;
        else if (plsql_like && K(k, i, "LOOP") && !K(k, i - 1, "END")) loops++;
    }
    if (open > 0 && first_open >= 0) { ObjDiag *d = emit(s, DSRC_PARSER, DIAG_WARNING, "BLK001", base + k->t[first_open].off, base + k->t[first_open].off + k->t[first_open].len, 2, "%d block(s) opened here are not closed by END.", open); if (d) snprintf(d->unit, sizeof d->unit, "%s", unit); }
    if (plsql_like && ifs > 0) emit(s, DSRC_PARSER, DIAG_WARNING, "BLK003", -1, -1, 2, "%d IF without END IF.", ifs);
    if (plsql_like && loops > 0) emit(s, DSRC_PARSER, DIAG_WARNING, "BLK004", -1, -1, 2, "%d LOOP without END LOOP.", loops);
}

static bool known_name(const ObjDiagCtx *c, const char *name) {
    for (int i = 0; i < c->nknown; i++) {
        const char *k = c->known[i];
        if (!strcasecmp(k, name)) return true;
        const char *dot = strrchr(k, '.');
        if (dot && !strchr(name, '.') && !strcasecmp(dot + 1, name)) return true;
    }
    return false;
}

/* Inspections over a run of statements (a view query or a routine body). base: offset of tk text
 * in the whole document. */
static void inspect(Sink *s, const ObjDiagCtx *c, const Tk *k, int from, int base, bool is_view_query, int d, const char *const *params, int nparams) {
    /* CTE names are known inside the statement */
    char ctes[32][128]; int ncte = 0;
    for (int i = from; i + 2 < k->n; i++)
        if ((K(k, i, "WITH") || (k->t[i].kind == TK_COMMA && ncte > 0) || K(k, i, "RECURSIVE")) && (k->t[i + 1].kind == TK_WORD || k->t[i + 1].kind == TK_QIDENT) &&
            (K(k, i + 2, "AS") || k->t[i + 2].kind == TK_LPAREN) && ncte < 32)
            snprintf(ctes[ncte++], 128, "%.*s", k->t[i + 1].len, k->s + k->t[i + 1].off);
    for (int i = from; i < k->n; i++) {
        const SqlTok *t = &k->t[i];
        /* SELECT * in a view: the column list follows the tables */
        if (is_view_query && t->kind == TK_OP && t->len == 1 && k->s[t->off] == '*' &&
            (K(k, i - 1, "SELECT") || K(k, i - 1, "DISTINCT") || (i >= 2 && k->t[i - 1].kind == TK_DOT && (k->t[i - 2].kind == TK_WORD || k->t[i - 2].kind == TK_QIDENT) &&
                                                                  (K(k, i - 3, "SELECT") || k->t[i - 3].kind == TK_COMMA)))) {
            emit(s, DSRC_PARSER, DIAG_WARNING, "INS001", base + t->off, base + t->off + 1, 0,
                 "SELECT * in a view: the columns are fixed when the view is created; a later table change does not show and can break CREATE OR REPLACE. List the columns.");
        }
        /* DELETE / UPDATE without WHERE */
        if (K(k, i, "DELETE") || (K(k, i, "UPDATE") && !K(k, i - 1, "FOR") && !K(k, i - 1, "OF") && !K(k, i - 1, "ON") && !K(k, i - 1, "OR"))) {
            int depth = 0; bool where = false; int j = i + 1;
            for (; j < k->n; j++) {
                if (k->t[j].kind == TK_LPAREN) depth++;
                else if (k->t[j].kind == TK_RPAREN) { if (depth == 0) break; depth--; }
                else if (depth == 0 && k->t[j].kind == TK_SEMI) break;
                else if (depth == 0 && (K(k, j, "WHERE") || K(k, j, "CURRENT"))) { where = true; break; }
                else if (depth == 0 && (K(k, j, "END") || K(k, j, "BEGIN"))) break;
            }
            /* "UPDATE OF col ON t" in trigger headers and "BEFORE DELETE ON" are events, not statements */
            bool event = K(k, i + 1, "ON") || K(k, i + 1, "OR") || K(k, i + 1, "OF") || K(k, i - 1, "BEFORE") || K(k, i - 1, "AFTER") || K(k, i - 1, "OF") || K(k, i - 1, "INSTEAD") || K(k, i + 1, ",") || (i + 1 < k->n && k->t[i + 1].kind == TK_COMMA);
            if (!where && !event)
                emit(s, DSRC_PARSER, DIAG_WARNING, "INS002", base + t->off, base + t->off + t->len, 1, "%.*s without WHERE changes every row of the table.", t->len, k->s + t->off);
        }
        /* dynamic SQL (OBJ-032) */
        bool dyn = (K(k, i, "EXECUTE") && K(k, i + 1, "IMMEDIATE")) ||
                   (d == DIALECT_PG && K(k, i, "EXECUTE") && !K(k, i + 1, "FUNCTION") && !K(k, i + 1, "PROCEDURE") && !K(k, i - 1, "FOR") && !K(k, i - 1, "TO") && !K(k, i - 1, "ON")) ||
                   K(k, i, "sp_executesql") || (d == DIALECT_MSSQL && (K(k, i, "EXEC") || K(k, i, "EXECUTE")) && i + 1 < k->n && k->t[i + 1].kind == TK_LPAREN) ||
                   ((d == DIALECT_MYSQL || d == DIALECT_MARIADB) && K(k, i, "PREPARE"));
        if (dyn) emit(s, DSRC_PARSER, DIAG_INFO, "INS003", base + t->off, base + t->off + t->len, 0, "Dynamic SQL: the statement text is built at run time; Lume does not format or check what is inside it.");
        /* unknown tables (OBJ-027) */
        if (c->known && (K(k, i, "FROM") || K(k, i, "JOIN") || (K(k, i, "INTO") && !K(k, i - 1, "SELECT") && !(i >= 2 && k->t[i - 2].kind == TK_COMMA)) || (K(k, i, "UPDATE") && !K(k, i - 1, "FOR") && !K(k, i - 1, "OF") && !K(k, i + 1, "OF") && !K(k, i + 1, "ON")))) {
            int j = i + 1;
            if (j < k->n && (k->t[j].kind == TK_WORD || k->t[j].kind == TK_QIDENT) && !(j + 1 < k->n && k->t[j + 1].kind == TK_LPAREN) && !(K(k, j, "LATERAL") || K(k, j, "ONLY") || K(k, j, "SELECT") || K(k, j, "STRICT"))) {
                char nm[260]; int o = 0; int a = k->t[j].off, b = a;
                while (j < k->n && (k->t[j].kind == TK_WORD || k->t[j].kind == TK_QIDENT)) {
                    const SqlTok *q = &k->t[j]; const char *p = k->s + q->off; int l = q->len;
                    if (q->kind == TK_QIDENT && l >= 2) { p++; l -= 2; }
                    o += snprintf(nm + o, sizeof nm - (size_t)o, "%s%.*s", o ? "." : "", l, p);
                    b = q->off + q->len;
                    if (j + 2 < k->n && k->t[j + 1].kind == TK_DOT) j += 2; else { j++; break; }
                }
                if (j < k->n && k->t[j].kind == TK_LPAREN) continue;   /* a function call: FROM generate_series(...) */
                bool cte = false; for (int q = 0; q < ncte; q++) if (!strcasecmp(ctes[q], nm)) cte = true;
                bool param = false; for (int q = 0; q < nparams; q++) if (!strcasecmp(params[q], nm)) param = true;
                bool sys = !strncasecmp(nm, "pg_", 3) || !strncasecmp(nm, "information_schema", 18) || !strcasecmp(nm, "dual") || !strncasecmp(nm, "sys.", 4) ||
                           !strncasecmp(nm, "all_", 4) || !strncasecmp(nm, "user_", 5) || !strncasecmp(nm, "sqlite_", 7) || !strcasecmp(nm, "new") || !strcasecmp(nm, "old");
                if (!cte && !param && !sys && !known_name(c, nm))
                    emit(s, DSRC_METADATA, DIAG_WARNING, "REF001", base + a, base + b, 2, "Unknown table or view \"%s\" in the catalog loaded by Lume (it may be in another schema or created later).", nm);
            }
        }
    }
    /* parameters with the name of a column of a referenced table (shadowing) */
    if (c->columns && nparams > 0) {
        for (int q = 0; q < nparams; q++) {
            for (int i = 0; i < c->ncolumns; i++) {
                const char *dot = strrchr(c->columns[i], '.'); if (!dot) continue;
                if (strcasecmp(dot + 1, params[q])) continue;
                char tbl[200]; snprintf(tbl, sizeof tbl, "%.*s", (int)(dot - c->columns[i]), c->columns[i]);
                const char *tn = strrchr(tbl, '.') ? strrchr(tbl, '.') + 1 : tbl;
                bool used = false;
                for (int j = from; j + 1 < k->n && !used; j++)
                    if ((K(k, j, "FROM") || K(k, j, "JOIN") || K(k, j, "UPDATE") || K(k, j, "INTO")) && (k->t[j + 1].kind == TK_WORD || k->t[j + 1].kind == TK_QIDENT)) {
                        int jj = j + 1; while (jj + 2 < k->n && k->t[jj + 1].kind == TK_DOT) jj += 2;
                        if (tok_is(k->s, &k->t[jj], tn)) used = true;
                    }
                if (!used) continue;
                for (int j = from; j < k->n; j++) if (tok_is(k->s, &k->t[j], params[q]) && !(j > 0 && k->t[j - 1].kind == TK_DOT)) {
                    emit(s, DSRC_METADATA, DIAG_WARNING, "INS004", base + k->t[j].off, base + k->t[j].off + k->t[j].len, 1,
                         "Parameter \"%s\" has the name of column %s: inside queries the reference is ambiguous. Prefix the parameter (p_%s) or qualify the column.", params[q], c->columns[i], params[q]);
                    break;
                }
                break;
            }
        }
    }
}

/* Parameter names of a routine header ("a int, OUT b text" -> a, b). */
static int param_names(const char *args, int len, int d, char names[][64], int max) {
    Tk k; Sink dummy = { 0 }; lex_sig(&k, args, len, d, false, &dummy, 0);
    int n = 0, s = 0;
    while (s < k.n && n < max) {
        int e = s, depth = 0;
        while (e < k.n && !(depth == 0 && k.t[e].kind == TK_COMMA)) { if (k.t[e].kind == TK_LPAREN) depth++; else if (k.t[e].kind == TK_RPAREN) depth--; e++; }
        int a = s;
        if (K(&k, a, "IN") || K(&k, a, "OUT") || K(&k, a, "INOUT") || K(&k, a, "VARIADIC")) a++;
        if (a + 1 < e && (k.t[a].kind == TK_WORD || k.t[a].kind == TK_QIDENT) && k.t[a + 1].kind != TK_DOT) {
            char w1[64], w2[64]; snprintf(w1, sizeof w1, "%.*s", k.t[a].len, args + k.t[a].off); snprintf(w2, sizeof w2, "%.*s", k.t[a + 1].len, args + k.t[a + 1].off);
            bool type2 = (!strcasecmp(w1, "double") && !strcasecmp(w2, "precision")) || (!strcasecmp(w1, "character") && !strcasecmp(w2, "varying")) ||
                         ((!strcasecmp(w1, "timestamp") || !strcasecmp(w1, "time")) && (!strcasecmp(w2, "with") || !strcasecmp(w2, "without")));
            if (!type2) snprintf(names[n++], 64, "%s", w1);
        }
        s = e + 1;
    }
    free(k.t);
    return n;
}

int obj_diagnose(const char *text, int len, const ObjDiagCtx *c, ObjDiag *out, int max) {
    Sink s = { out, 0, max, text, len };
    const int d = c->dialect;
    Tk k; lex_sig(&k, text, len, d, true, &s, 0);
    check_parens(&s, &k, 0);
    ObjHeader h;
    if (!obj_parse_header(text, len, d, &h)) {
        int at = k.n ? k.t[0].off : 0;
        emit(&s, DSRC_PARSER, DIAG_ERROR, "HDR001", at, at + (k.n ? k.t[0].len : 0), 0,
             "The text must start with CREATE %s ...: this is what is published (comments before it are fine).", c->key ? objkind_name(c->key->kind) : "");
        free(k.t);
        goto sort;
    }
    if (c->key) {
        const ObjKey *key = c->key;
        if (h.kind != key->kind)
            emit(&s, DSRC_PARSER, DIAG_ERROR, "HDR002", h.start, h.name_start, 0, "This creates a %s; the tab edits a %s.", objkind_word(h.kind), objkind_word(key->kind));
        bool same = (d == DIALECT_PG || d == DIALECT_ORACLE) ? !strcmp(h.name, key->name) : !strcasecmp(h.name, key->name);
        bool same_schema = !h.schema[0] || !key->schema[0] || ((d == DIALECT_PG || d == DIALECT_ORACLE) ? !strcmp(h.schema, key->schema) : !strcasecmp(h.schema, key->schema));
        if (!same || !same_schema) {
            char want[300];
            /* the fix keeps the user's qualification style, quoting only when needed */
            bool needq = false; for (const char *p = key->name; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_') ||
                (d == DIALECT_PG && isupper((unsigned char)*p)) || (d == DIALECT_ORACLE && islower((unsigned char)*p))) needq = true;
            if (h.schema[0] && key->schema[0]) snprintf(want, sizeof want, needq ? "%s.\"%s\"" : "%s.%s", key->schema, key->name);
            else snprintf(want, sizeof want, needq ? "\"%s\"" : "%s", key->name);
            ObjDiag *dg = emit(&s, DSRC_PARSER, DIAG_ERROR, "HDR003", h.name_start, h.name_end, 0,
                               "The name differs from the object being edited (%s%s%s): publishing would create another object. Rename is a separate operation.",
                               key->schema, key->schema[0] ? "." : "", key->name);
            fix(dg, "Restore the object name", h.name_start, h.name_end, want);
        }
    }
    if (h.if_not_exists) {
        /* find "IF NOT EXISTS" to remove it */
        for (int i = 0; i + 2 < k.n; i++) if (K(&k, i, "IF") && K(&k, i + 1, "NOT") && K(&k, i + 2, "EXISTS")) {
            ObjDiag *dg = emit(&s, DSRC_PARSER, DIAG_ERROR, "HDR004", k.t[i].off, k.t[i + 2].off + k.t[i + 2].len, 0, "IF NOT EXISTS keeps the current definition: the change would be silently ignored.");
            int e = k.t[i + 2].off + k.t[i + 2].len; while (e < len && text[e] == ' ') e++;
            fix(dg, "Remove IF NOT EXISTS", k.t[i].off, e, "");
            break;
        }
    }
    /* the verb the engine needs */
    int kind = c->key ? c->key->kind : h.kind;
    if (d == DIALECT_SQLITE && h.or_replace) {
        ObjDiag *dg = emit(&s, DSRC_PARSER, DIAG_ERROR, "DLC001", h.start, h.verb_end, 0, "SQLite has no CREATE OR REPLACE: write CREATE; Apply drops and re-creates it in one transaction.");
        fix(dg, "Use CREATE", h.start, h.verb_end, "CREATE");
    }
    if ((d == DIALECT_PG && kind != OBJK_MATVIEW && kind != OBJK_TRIGGER) || (d == DIALECT_ORACLE && kind != OBJK_MATVIEW)) {
        if (!h.or_replace && !h.alter_only) {
            ObjDiag *dg = emit(&s, DSRC_PARSER, DIAG_ERROR, "DLC002", h.start, h.verb_end, 0, "The object exists: write CREATE OR REPLACE (a plain CREATE fails).");
            fix(dg, "Use CREATE OR REPLACE", h.start, h.verb_end, "CREATE OR REPLACE");
        }
    }
    if (d == DIALECT_MSSQL && !h.or_alter && !h.alter_only) {
        ObjDiag *dg = emit(&s, DSRC_PARSER, DIAG_ERROR, "DLC003", h.start, h.verb_end, 0, "The object exists: write CREATE OR ALTER (SQL Server 2016 SP1+) or ALTER.");
        fix(dg, "Use CREATE OR ALTER", h.start, h.verb_end, "CREATE OR ALTER");
    }
    if (d == DIALECT_PG && h.or_replace && kind == OBJK_MATVIEW)
        emit(&s, DSRC_PARSER, DIAG_ERROR, "DLC004", h.start, h.verb_end, 0, "PostgreSQL has no CREATE OR REPLACE MATERIALIZED VIEW; Apply plans a drop and create.");
    if (h.extra_statements > 0)
        emit(&s, DSRC_PARSER, DIAG_ERROR, "HDR005", h.body_end, len, 0, "%d statement(s) after the definition: only the object definition can be published.", h.extra_statements);
    /* PostgreSQL routines: identity and return type */
    if (d == DIALECT_PG && c->key && (kind == OBJK_FUNCTION || kind == OBJK_PROCEDURE) && h.args_start >= 0) {
        char loc[512];
        if (pg_identity_types(text + h.args_start, h.args_end - h.args_start, kind == OBJK_PROCEDURE, loc, sizeof loc)) {
            /* compare with the catalog signature, both normalised */
            char cat[512] = ""; int o = 0; const char *p = c->key->signature;
            while (*p) { while (*p == ' ') p++; const char *e = p; int dp = 0; while (*e && !(dp == 0 && *e == ',')) { if (*e == '(') dp++; else if (*e == ')') dp--; e++; }
                         if (e > p) { char ty[200]; pg_norm_type(p, (int)(e - p), ty, sizeof ty); o += snprintf(cat + o, sizeof cat - (size_t)o, "%s%s", o ? ", " : "", ty); } p = *e ? e + 1 : e; }
            if (strcmp(loc, cat))
                emit(&s, DSRC_PARSER, DIAG_ERROR, "PG001", h.args_start, h.args_end, 1, "Argument types (%s) differ from the %s being edited (%s): this would create a new overload and leave the old one in place.", loc, objkind_word(kind), c->key->signature);
        }
        if (kind == OBJK_FUNCTION && c->base && c->base->result_type[0] && h.returns[0]) {
            char a[260], b[260]; pg_norm_type(h.returns, (int)strlen(h.returns), a, sizeof a); pg_norm_type(c->base->result_type, (int)strlen(c->base->result_type), b, sizeof b);
            if (strcasecmp(a, b) && strncasecmp(h.returns, "TABLE", 5) && strncasecmp(h.returns, "SETOF", 5))
                emit(&s, DSRC_PARSER, DIAG_WARNING, "PG002", -1, -1, 1, "Return type changes (%s -> %s): CREATE OR REPLACE cannot do it; Apply will offer an explicit drop and create.", c->base->result_type, h.returns);
        }
    }
    /* parameters (for shadowing / reference checks) */
    char params[32][64]; int np = 0;
    if (h.args_start >= 0) np = param_names(text + h.args_start, h.args_end - h.args_start, d, params, 32);
    const char *pp[32]; for (int i = 0; i < np; i++) pp[i] = params[i];
    /* body checks */
    bool view = kind == OBJK_VIEW || kind == OBJK_MATVIEW;
    if (view) {
        int from = 0; while (from < k.n && k.t[from].off < (h.select_start >= 0 ? h.select_start : len)) from++;
        inspect(&s, c, &k, from, 0, true, d, NULL, 0);
    } else if (d == DIALECT_PG) {
        /* code lives in $tag$ bodies (or a BEGIN ATOMIC body): check inside them */
        bool atomic = false; for (int i = 0; i + 1 < k.n; i++) if (K(&k, i, "BEGIN") && K(&k, i + 1, "ATOMIC")) atomic = true;
        if (atomic) { check_blocks(&s, &k, 0, 0, false, "MAIN"); inspect(&s, c, &k, 0, 0, false, d, pp, np); }
        bool plpgsql = false; for (int i = 0; i + 1 < k.n; i++) if (K(&k, i, "LANGUAGE") && (K(&k, i + 1, "plpgsql") || (k.t[i + 1].kind == TK_STRING && !strncasecmp(text + k.t[i + 1].off + 1, "plpgsql", 7)))) plpgsql = true;
        for (int i = 0; i < k.n; i++) if (k.t[i].kind == TK_BODY && !k.t[i].unterminated) {
            const char *b = text + k.t[i].off; int bl = k.t[i].len, tag = 1;
            while (tag < bl && b[tag] != '$') tag++;
            tag++;   /* "$tag$" */
            if (bl < 2 * tag) continue;
            Tk inner; lex_sig(&inner, b + tag, bl - 2 * tag, DIALECT_PG, true, &s, k.t[i].off + tag);
            check_parens(&s, &inner, k.t[i].off + tag);
            if (plpgsql) check_blocks(&s, &inner, 0, k.t[i].off + tag, true, "BODY");
            inspect(&s, c, &inner, 0, k.t[i].off + tag, false, d, pp, np);
            free(inner.t);
        }
    } else {
        int from = 0; while (from < k.n && k.t[from].off < h.name_end) from++;
        bool plsql = d == DIALECT_ORACLE || d == DIALECT_MYSQL || d == DIALECT_MARIADB;
        if (kind != OBJK_PKG_SPEC && kind != OBJK_PKG_BODY && kind != OBJK_TYPE_SPEC && kind != OBJK_TYPE_BODY)
            check_blocks(&s, &k, from, 0, plsql, "MAIN");
        inspect(&s, c, &k, from, 0, false, d, pp, np);
    }
    free(k.t);
sort:
    /* errors first, then warnings, info, hints; text order inside each */
    for (int i = 1; i < s.n; i++) {
        ObjDiag t = out[i]; int j = i - 1;
        while (j >= 0 && (out[j].severity > t.severity || (out[j].severity == t.severity && out[j].start > t.start))) { out[j + 1] = out[j]; j--; }
        out[j + 1] = t;
    }
    return s.n;
}

/* ---- symbols of a routine (OBJ-017) ---- */

static int add_sym(ObjSymbol *out, int n, int max, const char *name, int nl, const char *type, int tl, bool param) {
    if (n >= max || nl <= 0 || nl >= (int)sizeof out[0].name) return n;
    for (int i = 0; i < n; i++) if ((int)strlen(out[i].name) == nl && !strncasecmp(out[i].name, name, (size_t)nl)) return n;
    ObjSymbol *s = &out[n]; memset(s, 0, sizeof *s);
    memcpy(s->name, name, (size_t)nl);
    if (tl > 0) snprintf(s->type, sizeof s->type, "%.*s", tl, type);
    s->parameter = param;
    return n + 1;
}
/* Declarations "name type ...;" from token i until BEGIN. */
static int scan_decls(const Tk *k, int i, ObjSymbol *out, int n, int max) {
    bool at_start = true;
    for (; i < k->n && !K(k, i, "BEGIN"); i++) {
        if (k->t[i].kind == TK_SEMI) { at_start = true; continue; }
        if (at_start && (k->t[i].kind == TK_WORD || k->t[i].kind == TK_QIDENT) && !sql_is_keyword(k->s + k->t[i].off, k->t[i].len) &&
            !K(k, i, "CURSOR") && !K(k, i, "TYPE") && !K(k, i, "PRAGMA") && !K(k, i, "FUNCTION") && !K(k, i, "PROCEDURE") && !K(k, i, "SUBTYPE")) {
            int te = i + 1; while (te < k->n && k->t[te].kind != TK_SEMI && !(k->t[te].kind == TK_OP && k->s[k->t[te].off] == ':') && !K(k, te, "DEFAULT") && !K(k, te, "NOT")) te++;
            int ta = i + 1 < k->n ? k->t[i + 1].off : 0, tb = te - 1 > i ? k->t[te - 1].off + k->t[te - 1].len : ta;
            n = add_sym(out, n, max, k->s + k->t[i].off, k->t[i].len, k->s + ta, tb - ta, false);
        }
        at_start = false;
    }
    return n;
}
int obj_symbols(const char *text, int len, int d, ObjSymbol *out, int max) {
    int n = 0;
    ObjHeader h;
    if (!obj_parse_header(text, len, d, &h)) return 0;
    if (h.args_start >= 0) {
        /* parameters: [mode] name type [DEFAULT ...] */
        Tk k; Sink dummy = { 0 }; lex_sig(&k, text + h.args_start, h.args_end - h.args_start, d, false, &dummy, 0);
        int s = 0;
        while (s < k.n) {
            int e = s, depth = 0;
            while (e < k.n && !(depth == 0 && k.t[e].kind == TK_COMMA)) { if (k.t[e].kind == TK_LPAREN) depth++; else if (k.t[e].kind == TK_RPAREN) depth--; e++; }
            int a = s;
            if (K(&k, a, "IN") || K(&k, a, "OUT") || K(&k, a, "INOUT") || K(&k, a, "VARIADIC")) a++;
            if (K(&k, a, "OUT")) a++;   /* Oracle IN OUT */
            int b = e; for (int j = a; j < e; j++) if (K(&k, j, "DEFAULT") || (k.t[j].kind == TK_OP && k.s[k.t[j].off] == '=')) { b = j; break; }
            bool outp = K(&k, s, "OUT") || (a + 1 < e && K(&k, a, "OUT"));
            if (a + 1 < b && (k.t[a].kind == TK_WORD || k.t[a].kind == TK_QIDENT)) {
                int t0 = a + 1;
                while (t0 < b - 1 && (K(&k, t0, "IN") || K(&k, t0, "OUT") || K(&k, t0, "NOCOPY"))) t0++;   /* PL/SQL: name IN OUT type */
                int ta = k.t[t0].off, tb = k.t[b - 1].off + k.t[b - 1].len;
                int before = n;
                n = add_sym(out, n, max, k.s + k.t[a].off, k.t[a].len, k.s + ta, tb - ta, true);
                if (n > before) out[n - 1].out_only = outp && !K(&k, s, "INOUT");
            } else if (a < b) {
                /* an unnamed parameter keeps its place in the $n numbering */
                if (n < max) { memset(&out[n], 0, sizeof out[n]); out[n].parameter = true; out[n].out_only = K(&k, s, "OUT"); n++; }
            }
            s = e + 1;
        }
        free(k.t);
    }
    /* variables */
    Tk k; Sink dummy = { 0 }; lex_sig(&k, text, len, d, false, &dummy, 0);
    if (d == DIALECT_PG) {
        for (int i = 0; i < k.n; i++) if (k.t[i].kind == TK_BODY) {
            const char *b = text + k.t[i].off; int bl = k.t[i].len, tag = 1;
            while (tag < bl && b[tag] != '$') tag++;
            tag++;
            if (bl < 2 * tag) continue;
            Tk in; lex_sig(&in, b + tag, bl - 2 * tag, d, false, &dummy, 0);
            for (int j = 0; j < in.n; j++) if (K(&in, j, "DECLARE")) n = scan_decls(&in, j + 1, out, n, max);
            free(in.t);
        }
    } else if (d == DIALECT_ORACLE) {
        /* PL/SQL: the declarations between IS / AS (after the header) and the first BEGIN */
        int from = 0; while (from < k.n && k.t[from].off < h.name_end) from++;
        for (int i = from; i < k.n; i++) if ((K(&k, i, "IS") || K(&k, i, "AS")) && h.kind != OBJK_VIEW) { n = scan_decls(&k, i + 1, out, n, max); break; }
        for (int i = from; i < k.n; i++) if (K(&k, i, "DECLARE")) n = scan_decls(&k, i + 1, out, n, max);
    } else {
        /* T-SQL / MySQL: DECLARE name type; one per statement */
        for (int i = 0; i + 1 < k.n; i++) if (K(&k, i, "DECLARE") && (k.t[i + 1].kind == TK_WORD || k.t[i + 1].kind == TK_QIDENT)) {
            int te = i + 2; while (te < k.n && k.t[te].kind != TK_SEMI && k.t[te].kind != TK_COMMA && !K(&k, te, "DEFAULT")) te++;
            int ta = i + 2 < k.n ? k.t[i + 2].off : 0, tb = te - 1 > i + 1 ? k.t[te - 1].off + k.t[te - 1].len : ta;
            n = add_sym(out, n, max, text + k.t[i + 1].off, k.t[i + 1].len, text + ta, tb - ta, false);
        }
    }
    free(k.t);
    return n;
}

/* ---- refactorings (OBJ-021) ---- */

char *obj_extract_cte(const char *text, int len, int d, int s0, int s1, const char *name, char *why, int whyn) {
    while (s0 < s1 && isspace((unsigned char)text[s0])) s0++;
    while (s1 > s0 && isspace((unsigned char)text[s1 - 1])) s1--;
    if (s1 - s0 < 2 || text[s0] != '(' || text[s1 - 1] != ')') { snprintf(why, (size_t)whyn, "Select a parenthesized subquery: (SELECT ...)"); return NULL; }
    Tk k; Sink dummy = { 0 }; lex_sig(&k, text, len, d, false, &dummy, 0);
    int open = -1, close = -1;
    for (int i = 0; i < k.n; i++) { if (k.t[i].off == s0 && k.t[i].kind == TK_LPAREN) open = i; if (k.t[i].off == s1 - 1 && k.t[i].kind == TK_RPAREN) close = i; }
    if (open < 0 || close < 0 || !K(&k, open + 1, "SELECT")) { free(k.t); snprintf(why, (size_t)whyn, "The selection is not a (SELECT ...) subquery"); return NULL; }
    int depth = 0; for (int i = open; i <= close; i++) { if (k.t[i].kind == TK_LPAREN) depth++; else if (k.t[i].kind == TK_RPAREN && --depth == 0 && i != close) { free(k.t); snprintf(why, (size_t)whyn, "The parentheses do not enclose one subquery"); return NULL; } }
    /* only where a table name can stand: FROM / JOIN / a comma of a FROM list (IN (...) or a
     * scalar subquery would change meaning) */
    bool from_ctx = K(&k, open - 1, "FROM") || K(&k, open - 1, "JOIN") || K(&k, open - 1, "LATERAL");
    if (!from_ctx && open > 0 && k.t[open - 1].kind == TK_COMMA) {
        for (int i = open - 2, dp = 0; i >= 0; i--) { if (k.t[i].kind == TK_RPAREN) dp++; else if (k.t[i].kind == TK_LPAREN) { if (!dp) break; dp--; } else if (!dp && K(&k, i, "FROM")) { from_ctx = true; break; } else if (!dp && (K(&k, i, "SELECT") || K(&k, i, "WHERE"))) break; }
    }
    if (!from_ctx || K(&k, open - 1, "LATERAL")) { free(k.t); snprintf(why, (size_t)whyn, "Only a subquery used as a table (FROM / JOIN) can become a CTE without changing the query"); return NULL; }
    /* the query that holds it: the outermost SELECT / WITH of its statement */
    int q = -1, dp2 = 0;
    for (int i = open - 1; i >= 0; i--) {
        if (k.t[i].kind == TK_RPAREN) dp2++;
        else if (k.t[i].kind == TK_LPAREN) dp2 = dp2 ? dp2 - 1 : 0;
        else if (k.t[i].kind == TK_SEMI && !dp2) break;
        else if (!dp2 && (K(&k, i, "SELECT") || K(&k, i, "WITH"))) q = i;
        else if (!dp2 && K(&k, i, "AS") && q >= 0 && !(i + 1 < k.n && k.t[i + 1].kind == TK_LPAREN)) break;   /* CREATE VIEW ... AS <query> (a CTE's "name AS (" goes on) */
    }
    if (q < 0) { free(k.t); snprintf(why, (size_t)whyn, "No enclosing SELECT found"); return NULL; }
    for (const char *p = name; *p; p++) if (!(isalnum((unsigned char)*p) || *p == '_') || (p == name && isdigit((unsigned char)*p))) { free(k.t); snprintf(why, (size_t)whyn, "Not a valid name"); return NULL; }
    if (!name[0] || sql_is_keyword(name, (int)strlen(name))) { free(k.t); snprintf(why, (size_t)whyn, "Not a valid name"); return NULL; }
    int sub_a = k.t[open].off, sub_b = k.t[close].off + 1;
    size_t cap = (size_t)len + strlen(name) * 2 + 64; char *o = malloc(cap); size_t ol = 0;
#define PUT(p_, n_) do { memcpy(o + ol, (p_), (size_t)(n_)); ol += (size_t)(n_); } while (0)
    int qoff = k.t[q].off;
    PUT(text, qoff);
    if (K(&k, q, "WITH")) {
        int after = q + 1; if (K(&k, after, "RECURSIVE")) after++;
        int ins = k.t[after].off;
        PUT(text + qoff, ins - qoff);
        PUT(name, strlen(name)); PUT(" AS ", 4); PUT(text + sub_a, sub_b - sub_a); PUT(",\n", 2);
        PUT(text + ins, sub_a - ins);
    } else {
        PUT("WITH ", 5); PUT(name, strlen(name)); PUT(" AS ", 4); PUT(text + sub_a, sub_b - sub_a); PUT("\n", 1);
        PUT(text + qoff, sub_a - qoff);
    }
    PUT(name, strlen(name));
    PUT(text + sub_b, len - sub_b);
#undef PUT
    o[ol] = 0; free(k.t);
    return o;
}

char *obj_named_params(const char *text, int len, int d, int *count, char *why, int whyn) {
    *count = 0;
    if (d != DIALECT_PG) { snprintf(why, (size_t)whyn, "Positional parameters ($1) are a PostgreSQL notation"); return NULL; }
    ObjSymbol sy[64]; int ns = obj_symbols(text, len, d, sy, 64);
    const char *names[64]; int np = 0;
    for (int i = 0; i < ns; i++) if (sy[i].parameter && !sy[i].out_only) names[np++] = sy[i].name;
    Tk k; Sink dummy = { 0 }; lex_sig(&k, text, len, d, false, &dummy, 0);
    size_t cap = (size_t)len * 2 + 64; char *o = malloc(cap); size_t ol = 0; int last = 0;
    for (int i = 0; i < k.n; i++) {
        if (k.t[i].kind != TK_BODY || k.t[i].unterminated) continue;
        const char *b = text + k.t[i].off; int bl = k.t[i].len, tag = 1;
        while (tag < bl && b[tag] != '$') tag++;
        tag++;
        int ba = k.t[i].off + tag, be = k.t[i].off + bl - tag;
        SqlTok *in = malloc(sizeof *in * (size_t)(be - ba + 1));
        int n = sql_lex(text + ba, be - ba, d, in, be - ba + 1);
        for (int j = 0; j + 1 < n; j++) {
            /* '$' then digits, adjacent, outside strings and comments */
            if (in[j].kind != TK_OTHER || text[ba + in[j].off] != '$' || in[j + 1].kind != TK_NUMBER || in[j + 1].off != in[j].off + 1) continue;
            int num = atoi(text + ba + in[j + 1].off);
            if (num < 1 || num > np || !names[num - 1][0]) { free(in); free(o); free(k.t); snprintf(why, (size_t)whyn, "$%d has no named parameter to replace it", num); return NULL; }
            int at = ba + in[j].off;
            memcpy(o + ol, text + last, (size_t)(at - last)); ol += (size_t)(at - last);
            size_t nl = strlen(names[num - 1]); memcpy(o + ol, names[num - 1], nl); ol += nl;
            last = ba + in[j + 1].off + in[j + 1].len; (*count)++;
            j++;
        }
        free(in);
    }
    memcpy(o + ol, text + last, (size_t)(len - last)); ol += (size_t)(len - last); o[ol] = 0;
    free(k.t);
    if (!*count) { free(o); snprintf(why, (size_t)whyn, "No $n reference in the body"); return NULL; }
    return o;
}

char *obj_qualify_columns(const char *text, int len, int d, const char *const *cols, int ncols, int *count, char *why, int whyn) {
    *count = 0;
    if (!cols || !ncols) { snprintf(why, (size_t)whyn, "The catalog columns are not loaded for this connection"); return NULL; }
    Tk k; Sink dummy = { 0 }; lex_sig(&k, text, len, d, false, &dummy, 0);
    /* tables in scope: FROM / JOIN name [AS] alias */
    struct { char table[128]; char alias[128]; } sc[16]; int ns = 0;
    char aliases[32][80]; int na = 0;   /* names the query defines (column aliases, CTEs): never qualified */
    for (int i = 0; i < k.n; i++) {
        if ((K(&k, i, "FROM") || K(&k, i, "JOIN") || (k.t[i].kind == TK_COMMA && ns > 0 && i > 0 && !K(&k, i + 1, "SELECT"))) && i + 1 < k.n && (k.t[i + 1].kind == TK_WORD || k.t[i + 1].kind == TK_QIDENT) && !K(&k, i + 1, "SELECT") && ns < 16) {
            int j = i + 1; char full[128]; int fo = 0;
            while (j < k.n && (k.t[j].kind == TK_WORD || k.t[j].kind == TK_QIDENT)) {
                fo += snprintf(full + fo, sizeof full - (size_t)fo, "%s%.*s", fo ? "." : "", k.t[j].len, text + k.t[j].off);
                if (j + 2 < k.n && k.t[j + 1].kind == TK_DOT) j += 2; else { j++; break; }
            }
            if (j < k.n && k.t[j].kind == TK_LPAREN) continue;   /* a function in FROM */
            const char *bare = strrchr(full, '.') ? strrchr(full, '.') + 1 : full;
            snprintf(sc[ns].table, sizeof sc[ns].table, "%s", bare);
            int al = j; if (K(&k, al, "AS")) al++;
            if (al < k.n && k.t[al].kind == TK_WORD && !sql_is_keyword(text + k.t[al].off, k.t[al].len)) snprintf(sc[ns].alias, sizeof sc[ns].alias, "%.*s", k.t[al].len, text + k.t[al].off);
            else snprintf(sc[ns].alias, sizeof sc[ns].alias, "%s", bare);
            if (i > 0 && k.t[i].kind == TK_COMMA) { /* only inside a FROM list */ bool inside = false; for (int q = i - 1; q >= 0; q--) { if (K(&k, q, "FROM")) { inside = true; break; } if (K(&k, q, "SELECT") || K(&k, q, "WHERE")) break; } if (!inside) continue; }
            ns++;
        }
        if (K(&k, i, "AS") && i + 1 < k.n && k.t[i + 1].kind == TK_WORD && na < 32) snprintf(aliases[na++], 80, "%.*s", k.t[i + 1].len, text + k.t[i + 1].off);
    }
    if (!ns) { free(k.t); snprintf(why, (size_t)whyn, "No FROM / JOIN table in the text"); return NULL; }
    size_t cap = (size_t)len * 2 + 64; char *o = malloc(cap); size_t ol = 0; int last = 0;
    for (int i = 0; i < k.n; i++) {
        const SqlTok *t = &k.t[i];
        if (t->kind != TK_WORD || sql_is_keyword(text + t->off, t->len)) continue;
        if ((i > 0 && k.t[i - 1].kind == TK_DOT) || (i + 1 < k.n && (k.t[i + 1].kind == TK_DOT || k.t[i + 1].kind == TK_LPAREN))) continue;
        if (K(&k, i - 1, "AS") || K(&k, i - 1, "FROM") || K(&k, i - 1, "JOIN") || K(&k, i - 1, "VIEW") || K(&k, i - 1, "TABLE") || K(&k, i - 1, "INTO") || K(&k, i - 1, "UPDATE")) continue;
        bool skip = false;
        for (int q = 0; q < ns && !skip; q++) if ((int)strlen(sc[q].alias) == t->len && !strncasecmp(sc[q].alias, text + t->off, (size_t)t->len)) skip = true;
        for (int q = 0; q < na && !skip; q++) if ((int)strlen(aliases[q]) == t->len && !strncasecmp(aliases[q], text + t->off, (size_t)t->len)) skip = true;
        if (skip) continue;
        /* the table (in scope) that has this column: exactly one, or nothing is done */
        int owner = -1, hits = 0;
        for (int q = 0; q < ns; q++)
            for (int c = 0; c < ncols; c++) {
                const char *dot = strrchr(cols[c], '.'); if (!dot) continue;
                char tb[160]; snprintf(tb, sizeof tb, "%.*s", (int)(dot - cols[c]), cols[c]);
                const char *tbare = strrchr(tb, '.') ? strrchr(tb, '.') + 1 : tb;
                if (!strcasecmp(tbare, sc[q].table) && (int)strlen(dot + 1) == t->len && !strncasecmp(dot + 1, text + t->off, (size_t)t->len)) { if (owner != q) hits++; owner = q; break; }
            }
        if (hits != 1) continue;
        size_t need = ol + (size_t)(t->off - last) + strlen(sc[owner].alias) + 2;
        if (need + (size_t)t->len >= cap) { cap = (need + (size_t)t->len) * 2; o = realloc(o, cap); }
        memcpy(o + ol, text + last, (size_t)(t->off - last)); ol += (size_t)(t->off - last);
        ol += (size_t)sprintf(o + ol, "%s.", sc[owner].alias);
        last = t->off; (*count)++;
    }
    if (cap < ol + (size_t)(len - last) + 1) { cap = ol + (size_t)(len - last) + 1; o = realloc(o, cap); }
    memcpy(o + ol, text + last, (size_t)(len - last)); ol += (size_t)(len - last); o[ol] = 0;
    free(k.t);
    if (!*count) { free(o); snprintf(why, (size_t)whyn, "Nothing to qualify: every column is qualified, unknown, or in more than one table"); return NULL; }
    return o;
}
