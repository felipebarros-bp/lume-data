/* SPDX-License-Identifier: AGPL-3.0-only */
/* Conservative SQL formatter: see sqlfmt.h. Pure C. */
#include "sqlfmt.h"
#include "sqllex.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

void sql_style_default(SqlStyle *s) { s->kw_case = 0; s->indent = 4; s->tabs = false; s->clause_newlines = true; }

void fmt_free(FmtResult *r) { free(r->text); r->text = NULL; }

/* Growable output buffer. */
typedef struct { char *s; int n, cap; } Buf;
static void put(Buf *b, const char *s, int n) {
    if (b->n + n + 1 > b->cap) { b->cap = (b->n + n + 1) * 2 + 256; b->s = realloc(b->s, (size_t)b->cap); }
    memcpy(b->s + b->n, s, (size_t)n); b->n += n; b->s[b->n] = 0;
}
static void putc_(Buf *b, char c) { put(b, &c, 1); }
static bool ends_with_space(Buf *b) { return b->n == 0 || b->s[b->n - 1] == ' ' || b->s[b->n - 1] == '\n' || b->s[b->n - 1] == '\t' || b->s[b->n - 1] == '('; }
static void trim_trailing(Buf *b) { while (b->n > 0 && (b->s[b->n - 1] == ' ' || b->s[b->n - 1] == '\t')) b->n--; if (b->s) b->s[b->n] = 0; }
static void newline(Buf *b, const SqlStyle *st, int level) {
    trim_trailing(b);
    if (b->n > 0 && b->s[b->n - 1] != '\n') putc_(b, '\n');
    for (int i = 0; i < level; i++) { if (st->tabs) putc_(b, '\t'); else for (int k = 0; k < st->indent; k++) putc_(b, ' '); }
}

/* Clause keywords that start a new line; two-word ones are matched by their first word. */
static bool clause_start(const char *t, const SqlTok *tk, const SqlTok *next, const char *ntext) {
    static const char *one[] = { "SELECT", "FROM", "WHERE", "HAVING", "LIMIT", "OFFSET", "UNION", "INTERSECT", "EXCEPT", "VALUES", "SET", "RETURNING", "JOIN", "WINDOW", NULL };
    for (int i = 0; one[i]; i++) if (tok_is(t, tk, one[i])) return true;
    if ((tok_is(t, tk, "GROUP") || tok_is(t, tk, "ORDER") || tok_is(t, tk, "PARTITION")) && next && tok_is(ntext, next, "BY")) return !tok_is(t, tk, "PARTITION");
    if (tok_is(t, tk, "LEFT") || tok_is(t, tk, "RIGHT") || tok_is(t, tk, "INNER") || tok_is(t, tk, "FULL") || tok_is(t, tk, "CROSS")) return true;
    return false;
}

/* True when the significant tokens (not whitespace) of a and b are the same. */
bool sql_same_tokens(const char *a, int alen, const char *b, int blen, int d) {
    int ma = alen + 1, mb = blen + 1;
    SqlTok *ta = malloc(sizeof *ta * (size_t)ma), *tb = malloc(sizeof *tb * (size_t)mb);
    int na = sql_lex(a, alen, d, ta, ma), nb = sql_lex(b, blen, d, tb, mb);
    int i = 0, j = 0; bool same = true;
    for (;;) {
        while (i < na && ta[i].kind == TK_WS) i++;
        while (j < nb && tb[j].kind == TK_WS) j++;
        if (i >= na || j >= nb) { same = i >= na && j >= nb; break; }
        const SqlTok *x = &ta[i], *y = &tb[j];
        if (x->kind != y->kind || x->len != y->len) { same = false; break; }
        bool kw = x->kind == TK_WORD && sql_is_keyword(a + x->off, x->len);
        if (kw ? strncasecmp(a + x->off, b + y->off, (size_t)x->len) : memcmp(a + x->off, b + y->off, (size_t)x->len)) { same = false; break; }
        i++; j++;
    }
    free(ta); free(tb);
    return same;
}

/* Formats one statement range [s, e) of text into b. Returns false when it holds a procedural
 * block it must not touch (the caller then copies the range verbatim). */
static bool format_range(const char *text, int s, int e, int d, const SqlStyle *st, Buf *b, bool *had_body) {
    int len = e - s;
    SqlTok *tk = malloc(sizeof *tk * (size_t)(len + 1));
    int n = sql_lex(text + s, len, d, tk, len + 1);
    const char *t = text + s;
    /* procedural block outside a delimited body: leave it alone */
    for (int i = 0; i < n; i++) if (tok_is(t, &tk[i], "BEGIN") || tok_is(t, &tk[i], "DECLARE")) {
        /* "BEGIN" as a transaction statement on its own is fine; inside CREATE it is a block */
        bool create = false; for (int k = 0; k < i; k++) if (tok_is(t, &tk[k], "CREATE")) create = true;
        if (create || d == DIALECT_ORACLE || d == DIALECT_MSSQL) { free(tk); return false; }
    }
    int depth = 0, prev = -1;   /* prev: index of the previous significant token */
    for (int i = 0; i < n; i++) {
        SqlTok *k = &tk[i];
        if (k->kind == TK_WS) {
            /* keep blank lines between statements / after comments as one newline */
            continue;
        }
        int nx = i + 1; while (nx < n && tk[nx].kind == TK_WS) nx++;
        const SqlTok *next = nx < n ? &tk[nx] : NULL;
        if (k->kind == TK_BODY) *had_body = true;
        /* line breaks */
        bool at_start = b->n == 0 || b->s[b->n - 1] == '\n';
        if (st->clause_newlines && k->kind == TK_WORD && prev >= 0 && clause_start(t, k, next, t)) {
            /* JOIN right after LEFT / INNER ... stays on the same line */
            bool after_join_word = prev >= 0 && (tok_is(t, &tk[prev], "LEFT") || tok_is(t, &tk[prev], "RIGHT") || tok_is(t, &tk[prev], "INNER") || tok_is(t, &tk[prev], "FULL") || tok_is(t, &tk[prev], "CROSS") || tok_is(t, &tk[prev], "OUTER"));
            bool union_select = prev >= 0 && (tok_is(t, &tk[prev], "UNION") || tok_is(t, &tk[prev], "ALL") || tok_is(t, &tk[prev], "AS")) && tok_is(t, k, "SELECT");
            if (!after_join_word && !union_select && !(tok_is(t, k, "SELECT") && tk[prev].kind == TK_LPAREN)) { newline(b, st, depth); at_start = true; }
        }
        /* spacing */
        if (!at_start) {
            bool no_space_before = k->kind == TK_COMMA || k->kind == TK_RPAREN || k->kind == TK_SEMI || k->kind == TK_DOT ||
                                   (k->kind == TK_LPAREN && prev >= 0 && (tk[prev].kind == TK_WORD || tk[prev].kind == TK_QIDENT) && !sql_is_keyword(t + tk[prev].off, tk[prev].len));
            bool after_tight = prev >= 0 && (tk[prev].kind == TK_DOT || tk[prev].kind == TK_LPAREN);
            if (k->kind == TK_OP && k->len == 2 && !memcmp(t + k->off, "::", 2)) no_space_before = true;
            if (prev >= 0 && tk[prev].kind == TK_OP && tk[prev].len == 2 && !memcmp(t + tk[prev].off, "::", 2)) after_tight = true;
            if (!no_space_before && !after_tight && !ends_with_space(b)) putc_(b, ' ');
        }
        /* the token itself */
        if (k->kind == TK_WORD && sql_is_keyword(t + k->off, k->len) && st->kw_case != 2) {
            for (int c = 0; c < k->len; c++) putc_(b, st->kw_case == 0 ? (char)toupper((unsigned char)t[k->off + c]) : (char)tolower((unsigned char)t[k->off + c]));
        } else put(b, t + k->off, k->len);
        if (k->kind == TK_LPAREN) depth++;
        if (k->kind == TK_RPAREN && depth > 0) depth--;
        /* a line comment must keep its line end, or the next token would become comment text */
        if (k->kind == TK_COMMENT && k->len >= 2 && t[k->off] != '/') newline(b, st, depth);
        if (k->kind == TK_SEMI) putc_(b, '\n');
        prev = i;
    }
    trim_trailing(b);
    free(tk);
    return true;
}

/* Statement boundaries: offsets just after each top-level ';' (outside strings / bodies). */
static int statement_ends(const char *text, int len, int d, int *ends, int max) {
    SqlTok *tk = malloc(sizeof *tk * (size_t)(len + 1));
    int n = sql_lex(text, len, d, tk, len + 1), m = 0, depth = 0;
    for (int i = 0; i < n && m < max; i++) {
        if (tk[i].kind == TK_LPAREN) depth++;
        else if (tk[i].kind == TK_RPAREN && depth) depth--;
        else if ((tk[i].kind == TK_SEMI && !depth) || tk[i].kind == TK_BATCH) ends[m++] = tk[i].off + tk[i].len;
    }
    free(tk);
    return m;
}

bool sql_format(const char *text, int len, int d, const SqlStyle *st, int sel_start, int sel_end, FmtResult *out) {
    memset(out, 0, sizeof *out);
    int s = 0, e = len;
    if (sel_start >= 0 && sel_end > sel_start) {
        /* widen to statement boundaries */
        int ends[4096]; int m = statement_ends(text, len, d, ends, 4096);
        s = 0; for (int i = 0; i < m; i++) if (ends[i] <= sel_start) s = ends[i];
        e = len; for (int i = 0; i < m; i++) if (ends[i] >= sel_end) { e = ends[i]; break; }
        while (s < e && isspace((unsigned char)text[s])) s++;
        if (s != sel_start || e != sel_end) snprintf(out->note, sizeof out->note, "Selection widened to whole statements. ");
    }
    out->range_start = s; out->range_end = e;
    Buf b = { 0 };
    put(&b, text, s);   /* untouched prefix */
    Buf mid = { 0 }; bool had_body = false;
    bool ok = format_range(text, s, e, d, st, &mid, &had_body);
    if (!ok) {
        free(mid.s); free(b.s);
        out->text = malloc((size_t)len + 1); memcpy(out->text, text, (size_t)len); out->text[len] = 0; out->len = len;
        out->refused = true;
        snprintf(out->note + strlen(out->note), sizeof out->note - strlen(out->note), "Not formatted: a procedural block (BEGIN ... END) has no reliable %s parser here; the text is kept as written.", dialect_name(d));
        return false;
    }
    /* keep a single trailing newline policy: what the range had */
    if (mid.n && e < len && mid.s[mid.n - 1] != '\n' && text[e] != '\n') putc_(&mid, '\n');
    put(&b, mid.s ? mid.s : "", mid.n);
    put(&b, text + e, len - e);   /* untouched suffix */
    free(mid.s);
    if (!sql_same_tokens(text, len, b.s, b.n, d)) {
        free(b.s);
        out->text = malloc((size_t)len + 1); memcpy(out->text, text, (size_t)len); out->text[len] = 0; out->len = len;
        out->refused = true;
        snprintf(out->note + strlen(out->note), sizeof out->note - strlen(out->note), "Not formatted: the result would not be token-equivalent to the source (a formatter limitation); nothing changed.");
        return false;
    }
    out->text = b.s; out->len = b.n;
    out->changed = b.n != len || memcmp(b.s, text, (size_t)len) != 0;
    if (had_body) snprintf(out->note + strlen(out->note), sizeof out->note - strlen(out->note), "The procedural body ($$ ... $$) is kept as written. ");
    if (!out->note[0]) snprintf(out->note, sizeof out->note, out->changed ? "Formatted." : "Already formatted.");
    return true;
}
