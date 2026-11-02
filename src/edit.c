/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Table-editor transaction model: see edit.h for the rules (key + snapshot WHERE, atomic
 * apply, conflict diagnosis).
 *
 * Layout of this file:
 *   - set-up: which columns can be compared exactly (the conflict snapshot), key and
 *     read-only columns;
 *   - pending changes: CellEdit list, deleted-row flags and pending inserts appended to the
 *     ResultSet after new_from;
 *   - statements: SQL text generation (UPDATE / DELETE / INSERT) with literal quoting and
 *     masking of sensitive columns;
 *   - apply: runs the statements in one transaction, checks that every UPDATE / DELETE
 *     touched exactly one row, rolls back otherwise and explains the conflict.
 *
 * Threading: everything runs on the UI thread. edit_apply calls the driver directly and
 * blocks until the batch is done, so the caller must make sure the connection is idle
 * (no executor job owns it).
 */
#include "edit.h"
#include "json.h"
#include <ctype.h>
#include <errno.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* ---- set-up ------------------------------------------------------------ */

/* forward declaration: edit_validate uses it before the statement section */
static bool bare_number(const char *v);
/* True for the Oracle driver (checked by name, for the few dialect differences below). */
static bool is_oracle(const EditSet *e) { return e->drv && e->drv->name && strcmp(e->drv->name, "Oracle") == 0; }
/* True for the SQLite driver. */
static bool is_sqlite(const EditSet *e) { return e->drv && e->drv->name && strcmp(e->drv->name, "SQLite") == 0; }

/* Case-insensitive substring search (also used on column names). */
static bool type_has(const char *type, const char *word) {
    size_t n = strlen(word);
    for (const char *p = type; *p; p++) if (strncasecmp(p, word, n) == 0) return true;
    return false;
}

/* can "col = '<text as loaded>'" be trusted to match exactly the stored value? */
static bool comparable(const EditSet *e, int c, char *why, size_t n) {
    const char *type = e->rs->types[c] ? e->rs->types[c] : "";
    int kind = e->rs->kinds[c];
    if (kind == CK_BINARY) { snprintf(why, n, "binary"); return false; }
    if (kind == CK_OTHER) { snprintf(why, n, "no equality for %s", *type ? type : "unknown type"); return false; }
    if (kind == CK_JSON && strcasecmp(type, "json") == 0) { snprintf(why, n, "json has no equality (jsonb has)"); return false; }
    if (is_sqlite(e) && (type_has(type, "REAL") || type_has(type, "FLOA") || type_has(type, "DOUB"))) { snprintf(why, n, "floating point text is not exact on SQLite"); return false; }
    if (is_oracle(e) && (kind == CK_DATE || type_has(type, "BINARY_FLOAT") || type_has(type, "BINARY_DOUBLE"))) { snprintf(why, n, "Oracle %s text depends on the session format", kind == CK_DATE ? "dates" : "binary floats"); return false; }
    return true;
}

/* Decides which non-key columns go into the WHERE snapshot (e->snap), builds the note of the
 * excluded ones and spots a conventional version column. Called whenever the key changes. */
static void compute_snapshot(EditSet *e) {
    int nc = e->rs->ncols;
    free(e->snap);
    e->snap = calloc(nc ? nc : 1, sizeof *e->snap);
    e->nsnap = 0;
    e->snap_note[0] = 0;
    e->version_col = -1;
    for (int c = 0; c < nc; c++) {
        const char *name = e->rs->cols[c];
        if (strcasecmp(name, "version") == 0 || strcasecmp(name, "row_version") == 0 || strcasecmp(name, "lock_version") == 0) e->version_col = c;
    }
    for (int c = 0; c < nc; c++) {
        bool pk = false;
        for (int k = 0; k < e->npk; k++) if (e->pkcols[k] == c) pk = true;
        if (pk) continue;
        char why[80];
        if (comparable(e, c, why, sizeof why)) { e->snap[c] = true; e->nsnap++; }
        else { size_t l = strlen(e->snap_note); snprintf(e->snap_note + l, sizeof e->snap_note - l, "%s%s (%s)", l ? ", " : "", e->rs->cols[c], why); }
    }
}

/* See edit.h. Every row loaded so far is "existing"; later rows are inserts. */
void edit_init(EditSet *e, ResultSet *rs, const DbDriver *drv, const char *qtable, const bool *numeric) {
    memset(e, 0, sizeof *e);
    e->rs = rs; e->drv = drv; e->numeric = numeric;
    snprintf(e->qtable, sizeof e->qtable, "%s", qtable ? qtable : "");
    e->new_from = rs->nrows;
    e->version_col = -1;
    e->nmeta = rs->ncols;
    e->readonly_why = calloc(rs->ncols ? rs->ncols : 1, sizeof *e->readonly_why);
    compute_snapshot(e);
}

/* See edit.h. */
void edit_set_key(EditSet *e, const int *pkcols, int npk) {
    free(e->pkcols);
    e->pkcols = npk ? malloc(sizeof *e->pkcols * npk) : NULL;
    if (npk) memcpy(e->pkcols, pkcols, sizeof *pkcols * npk);
    e->npk = npk;
    compute_snapshot(e);
}

/* See edit.h. Out-of-range columns are ignored. */
void edit_set_readonly(EditSet *e, int col, const char *why) {
    if (col < 0 || col >= e->nmeta) return;
    free(e->readonly_why[col]);
    e->readonly_why[col] = why ? strdup(why) : NULL;
}

/* See edit.h. Binary columns are always read-only, even without an explicit reason. */
const char *edit_col_readonly(const EditSet *e, int col) {
    if (col < 0 || col >= e->nmeta) return NULL;
    if (e->readonly_why[col]) return e->readonly_why[col];
    if (e->rs->kinds[col] == CK_BINARY) return "binary values cannot be edited here";
    return NULL;
}

/* See edit.h. Pending inserts are removed by cutting the ResultSet back to new_from. */
void edit_clear(EditSet *e) {
    for (int i = 0; i < e->nedits; i++) free(e->edits[i].value);
    e->nedits = 0;
    if (e->deleted) memset(e->deleted, 0, sizeof *e->deleted * e->deleted_cap);
    if (e->rs->nrows > e->new_from) e->rs->nrows = e->new_from;
    edit_stmts_free(e);
}

/* See edit.h. */
void edit_free(EditSet *e) {
    edit_clear(e);
    free(e->edits); free(e->deleted); free(e->pkcols); free(e->snap);
    for (int c = 0; c < e->nmeta; c++) free(e->readonly_why[c]);
    free(e->readonly_why);
    memset(e, 0, sizeof *e);
}

/* ---- pending changes ----------------------------------------------------- */

/* Linear search; the pending list is small (what one person typed). */
CellEdit *edit_find(EditSet *e, int row, int col) {
    for (int i = 0; i < e->nedits; i++) if (e->edits[i].row == row && e->edits[i].col == col) return &e->edits[i];
    return NULL;
}

/* See edit.h. Keeps the order of the remaining edits. */
void edit_remove(EditSet *e, int row, int col) {
    for (int i = 0; i < e->nedits; i++) if (e->edits[i].row == row && e->edits[i].col == col) {
        free(e->edits[i].value);
        memmove(&e->edits[i], &e->edits[i + 1], sizeof *e->edits * (e->nedits - i - 1));
        e->nedits--;
        return;
    }
}

/* See edit.h. A NULL value is treated as SQL NULL. */
void edit_put(EditSet *e, int row, int col, const char *value, bool is_null) {
    CellEdit *c = edit_find(e, row, col);
    if (!c) {
        if (e->nedits == e->cap_edits) { e->cap_edits = e->cap_edits ? e->cap_edits * 2 : 16; e->edits = realloc(e->edits, sizeof *e->edits * e->cap_edits); }
        c = &e->edits[e->nedits++];
        c->row = row; c->col = col; c->value = NULL;
    }
    free(c->value);
    c->value = is_null || !value ? NULL : strdup(value);
    c->is_null = is_null;
}

/* See edit.h. Comparison is with the text as loaded; a pending insert never matches. */
void edit_put_text(EditSet *e, int row, int col, const char *text) {
    const char *old = edit_row_new(e, row) ? NULL : rs_cell(e->rs, row, col);
    if (old && strcmp(old, text) == 0) edit_remove(e, row, col);
    else edit_put(e, row, col, text, false);
}

/* See edit.h. */
bool edit_row_deleted(const EditSet *e, int row) { return e->deleted && row < e->deleted_cap && e->deleted[row]; }
/* See edit.h. */
bool edit_row_new(const EditSet *e, int row) { return row >= e->new_from; }

/* See edit.h. */
int edit_row_state(const EditSet *e, int row) {
    if (row >= e->new_from) return 3;
    if (edit_row_deleted(e, row)) return 2;
    for (int i = 0; i < e->nedits; i++) if (e->edits[i].row == row) return 1;
    return 0;
}

/* See edit.h. The returned text belongs to the EditSet or the ResultSet; do not free. */
const char *edit_pending_value(const EditSet *e, int row, int col, int *state) {
    CellEdit *c = edit_find((EditSet *)e, row, col);
    if (c) { *state = c->is_null ? 2 : 1; return c->value; }
    if (edit_row_new(e, row)) { *state = 3; return NULL; }
    *state = 0;
    return rs_cell(e->rs, row, col);
}

/* See edit.h. */
int edit_pending_count(const EditSet *e) {
    int n = e->nedits;
    for (int i = 0; i < e->deleted_cap && i < e->rs->nrows; i++) if (e->deleted[i]) n++;
    n += e->rs->nrows - e->new_from;
    return n;
}

/* See edit.h. */
int edit_pending_rows(const EditSet *e) {
    int n = e->rs->nrows - e->new_from;
    for (int r = 0; r < e->new_from; r++) {
        bool t = edit_row_deleted(e, r);
        for (int i = 0; i < e->nedits && !t; i++) if (e->edits[i].row == r) t = true;
        n += t;
    }
    return n;
}

/* See edit.h. Returns false with a user-facing reason in `why` when the value cannot be
 * sent as is. Columns flagged CK_OTHER skip the numeric check (their text is passed through). */
bool edit_validate(const EditSet *e, int col, const char *val, char *why, size_t n) {
    const char *ro = edit_col_readonly(e, col);
    if (ro) { snprintf(why, n, "%s: %s", e->rs->cols[col], ro); return false; }
    int kind = e->rs->kinds[col];
    if (e->numeric && e->numeric[col] && kind != CK_OTHER) {
        if (!*val) { snprintf(why, n, "%s is numeric: type a number or use Set NULL", e->rs->cols[col]); return false; }
        char *end; strtod(val, &end); while (*end == ' ') end++;
        if (*end) { snprintf(why, n, "%s is numeric: '%s' is not a number", e->rs->cols[col], val); return false; }
        /* SQLite stores numbers as 64-bit integers or doubles: anything longer would be
         * silently rounded, so it is refused instead (US-050) */
        if (is_sqlite(e) && !bare_number(val)) { snprintf(why, n, "%s: SQLite cannot store this number exactly (64-bit integer or double only)", e->rs->cols[col]); return false; }
    }
    if (kind == CK_BOOL) {
        static const char *const ok[] = { "t", "f", "true", "false", "0", "1", "yes", "no" };
        bool good = false;
        for (int i = 0; i < 8; i++) if (strcasecmp(val, ok[i]) == 0) good = true;
        if (!good) { snprintf(why, n, "%s is boolean: use true / false", e->rs->cols[col]); return false; }
    }
    if (kind == CK_JSON) {
        Json j;
        if (!json_parse(&j, val, strlen(val))) { snprintf(why, n, "%s is JSON: %s at offset %zu", e->rs->cols[col], j.err, j.err_off); return false; }
        json_free(&j);
    }
    return true;
}

/* Grows the deleted-flags array to cover every row of the ResultSet (new slots false). */
static void ensure_deleted(EditSet *e) {
    if (e->deleted_cap >= e->rs->nrows) return;
    int cap = e->rs->nrows + 64;
    e->deleted = realloc(e->deleted, sizeof *e->deleted * cap);
    memset(e->deleted + e->deleted_cap, 0, sizeof *e->deleted * (cap - e->deleted_cap));
    e->deleted_cap = cap;
}

/* See edit.h. Copied values come from the source row's pending state, so an edited but
 * unsaved value is copied as edited. */
int edit_row_add(EditSet *e, int copy_from) {
    int row = rs_add_row(e->rs);
    if (row < 0) return -1;
    for (int c = 0; c < e->rs->ncols; c++) rs_set(e->rs, row, c, NULL, 0);
    if (copy_from >= 0) {
        for (int c = 0; c < e->rs->ncols; c++) {
            bool pk = false;
            for (int k = 0; k < e->npk; k++) if (e->pkcols[k] == c) pk = true;
            if (pk || edit_col_readonly(e, c)) continue;      /* keys and computed columns take DEFAULT */
            int st; const char *val = edit_pending_value(e, copy_from, c, &st);
            if (st == 3) continue;
            edit_put(e, row, c, val, val == NULL);
        }
    }
    return row;
}

/* See edit.h. Dropping a pending insert removes its row from the ResultSet and shifts the
 * row numbers of later edits down by one. */
void edit_row_toggle_delete(EditSet *e, int row) {
    if (edit_row_new(e, row)) {
        for (int i = e->nedits - 1; i >= 0; i--) if (e->edits[i].row == row) { free(e->edits[i].value); memmove(&e->edits[i], &e->edits[i + 1], sizeof *e->edits * (e->nedits - i - 1)); e->nedits--; }
        for (int i = 0; i < e->nedits; i++) if (e->edits[i].row > row) e->edits[i].row--;
        int ncols = e->rs->ncols;
        memmove(e->rs->cells + (size_t)row * ncols, e->rs->cells + (size_t)(row + 1) * ncols, sizeof *e->rs->cells * (size_t)(e->rs->nrows - row - 1) * ncols);
        e->rs->nrows--;
        return;
    }
    ensure_deleted(e);
    e->deleted[row] = !e->deleted[row];
}

/* ---- statements ------------------------------------------------------------ */

/* Growable, always NUL-terminated string buffer. s is heap allocated (free it); a zeroed
 * SBuf is a valid empty buffer. */
typedef struct { char *s; size_t len, cap; } SBuf;
/* Empties the buffer, keeping its memory. */
static void sb_reset(SBuf *b) { b->len = 0; if (b->s) b->s[0] = 0; }
/* Appends s, growing the buffer as needed. */
static void sb_put(SBuf *b, const char *s) {
    size_t n = strlen(s);
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2 + 64; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0;
}

/* See edit.h. Substring match, case-insensitive. */
bool edit_sensitive_name(const char *col) {
    static const char *const words[] = { "password", "passwd", "secret", "token", "api_key", "apikey", "credential", "ssn", "cpf", "card_number", "cvv", NULL };
    for (int i = 0; words[i]; i++) if (type_has(col, words[i])) return true;
    return false;
}

/* a number is written bare only when the database will read it back exactly: up to 18
 * integer digits, or a decimal / exponent form with at most 15 significant digits;
 * anything longer is sent as a quoted literal the column type converts without loss */
static bool bare_number(const char *v) {
    const char *p = v;
    if (*p == '+' || *p == '-') p++;
    int digits = 0; bool frac = false, exp = false;
    for (; *p; p++) {
        if (isdigit((unsigned char)*p)) { if (!exp) digits++; }
        else if (*p == '.' && !frac && !exp) frac = true;
        else if ((*p == 'e' || *p == 'E') && !exp) { exp = true; if (p[1] == '+' || p[1] == '-') p++; }
        else return false;
    }
    if (!digits) return false;
    if (frac || exp) return digits <= 15;
    if (digits < 19) return true;
    errno = 0; strtoll(v, NULL, 10);
    return errno == 0;                       /* 19 digits: bare only when it fits in int64 */
}

/* bare == false writes every value as a quoted literal: in a WHERE comparison the database
 * converts it to the column's own type, so "f" = '0.1' matches a real 0.1 exactly while
 * "f" = 0.1 (a numeric / double constant) would not. Assignments use bare numbers. */
static void literal(const EditSet *e, int col, const char *val, SBuf *out, bool mask, bool bare) {
    if (mask && edit_sensitive_name(e->rs->cols[col])) { sb_put(out, "'\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2'"); return; }
    if (bare && e->numeric && e->numeric[col] && *val && bare_number(val)) { sb_put(out, val); return; }
    /* appended whole, any length: a value cut short would be written truncated (data loss) or,
     * in a WHERE, never match and look like a conflict */
    size_t need = out->len + 2 * strlen(val) + 3;
    if (need > out->cap) { out->cap = need * 2 + 64; out->s = realloc(out->s, out->cap); }
    char *w = out->s + out->len;
    *w++ = '\'';
    for (const char *q = val; *q; q++) { if (*q == '\'') *w++ = '\''; *w++ = *q; }
    *w++ = '\''; *w = 0;
    out->len = (size_t)(w - out->s);
}

/* Appends one comparison "col = 'value'" (or "col IS NULL") for a WHERE clause. */
static void put_cmp(const EditSet *e, SBuf *b, int col, const char *val, bool mask) {
    char q[300];
    e->drv->quote_ident(e->rs->cols[col], q, sizeof q);
    sb_put(b, q);
    if (!val) { sb_put(b, " IS NULL"); return; }
    sb_put(b, " = "); literal(e, col, val, b, mask, false);
}

/* WHERE: primary key, then the snapshot of every comparable column as loaded (US-048) */
static void put_where(const EditSet *e, SBuf *b, int row, bool mask) {
    sb_put(b, " WHERE ");
    for (int k = 0; k < e->npk; k++) { if (k) sb_put(b, " AND "); put_cmp(e, b, e->pkcols[k], rs_cell(e->rs, row, e->pkcols[k]), mask); }
    for (int c = 0; c < e->rs->ncols; c++) {
        if (!e->snap[c]) continue;
        sb_put(b, " AND "); put_cmp(e, b, c, rs_cell(e->rs, row, c), mask);
    }
}

/* Appends only the key comparison (no " WHERE "), for re-reading a row in diagnose(). */
static void put_where_key(const EditSet *e, SBuf *b, int row) {
    for (int k = 0; k < e->npk; k++) { if (k) sb_put(b, " AND "); put_cmp(e, b, e->pkcols[k], rs_cell(e->rs, row, e->pkcols[k]), false); }
}

/* See edit.h. */
void edit_stmts_free(EditSet *e) {
    for (int i = 0; i < e->nstmts; i++) { free(e->stmts[i].sql); free(e->stmts[i].shown); free(e->stmts[i].note); }
    free(e->stmts);
    e->stmts = NULL; e->nstmts = 0;
}

/* Appends a statement to e->stmts, copying the three strings. */
static void stmt_add(EditSet *e, const char *sql, const char *shown, const char *note, int kind, int row) {
    e->stmts = realloc(e->stmts, sizeof *e->stmts * (e->nstmts + 1));
    EditStmt *s = &e->stmts[e->nstmts++];
    s->sql = strdup(sql); s->shown = strdup(shown); s->note = strdup(note); s->kind = kind; s->row = row;
}

/* Writes into b the UPDATE for every pending edit of one loaded row (mask = hide sensitive
 * values, for the shown text) and fills note with "col: old -> new" pairs. */
static void build_update(EditSet *e, int row, SBuf *b, bool mask, char *note, size_t nn) {
    char q[300];
    sb_reset(b);
    sb_put(b, "UPDATE "); sb_put(b, e->qtable); sb_put(b, " SET ");
    note[0] = 0;
    bool first = true;
    for (int i = 0; i < e->nedits; i++) {
        CellEdit *c = &e->edits[i];
        if (c->row != row) continue;
        if (!first) sb_put(b, ", ");
        first = false;
        e->drv->quote_ident(e->rs->cols[c->col], q, sizeof q);
        sb_put(b, q); sb_put(b, " = ");
        if (c->is_null) sb_put(b, "NULL"); else literal(e, c->col, c->value, b, mask, true);
        const char *old = rs_cell(e->rs, row, c->col);
        bool sens = edit_sensitive_name(e->rs->cols[c->col]);
        size_t l = strlen(note);
        snprintf(note + l, nn - l, "%s%s: %s \xE2\x86\x92 %s", l ? "  \xC2\xB7  " : "", e->rs->cols[c->col],
                 sens ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : old ? old : "NULL", c->is_null ? "NULL" : sens ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : c->value);
    }
    put_where(e, b, row, mask);
}

/* See edit.h. Rows marked deleted get no UPDATE even when they also have edits. */
void edit_build(EditSet *e) {
    edit_stmts_free(e);
    char q[300], note[400], shown_note[400];
    SBuf b = { 0 }, shown = { 0 };
    for (int row = 0; row < e->new_from; row++) {
        if (edit_row_deleted(e, row)) continue;
        bool any = false;
        for (int i = 0; i < e->nedits; i++) if (e->edits[i].row == row) any = true;
        if (!any) continue;
        build_update(e, row, &b, false, note, sizeof note);
        build_update(e, row, &shown, true, shown_note, sizeof shown_note);
        stmt_add(e, b.s, shown.s, note, 0, row);
    }
    /* DELETEs: key + snapshot, like the UPDATEs */
    for (int row = 0; row < e->new_from && row < e->deleted_cap; row++) {
        if (!e->deleted[row]) continue;
        sb_reset(&b); sb_reset(&shown);
        sb_put(&b, "DELETE FROM "); sb_put(&b, e->qtable); put_where(e, &b, row, false);
        sb_put(&shown, "DELETE FROM "); sb_put(&shown, e->qtable); put_where(e, &shown, row, true);
        const char *k = e->npk ? rs_cell(e->rs, row, e->pkcols[0]) : NULL;
        snprintf(note, sizeof note, "row with %s = %s  \xC2\xB7  cannot be undone after commit", e->npk ? e->rs->cols[e->pkcols[0]] : "?", k ? k : "NULL");
        stmt_add(e, b.s, shown.s, note, 1, row);
    }
    /* INSERTs: only the columns the user filled; the others take their DEFAULT */
    for (int row = e->new_from; row < e->rs->nrows; row++) {
        SBuf vals = { 0 }, svals = { 0 };
        int n = 0;
        sb_reset(&b); sb_reset(&shown);
        sb_put(&b, "INSERT INTO "); sb_put(&b, e->qtable); sb_put(&b, " (");
        note[0] = 0;
        for (int c = 0; c < e->rs->ncols; c++) {
            CellEdit *ce = edit_find(e, row, c);
            if (!ce) continue;
            if (n) { sb_put(&b, ", "); sb_put(&vals, ", "); sb_put(&svals, ", "); }
            e->drv->quote_ident(e->rs->cols[c], q, sizeof q);
            sb_put(&b, q);
            if (ce->is_null) { sb_put(&vals, "NULL"); sb_put(&svals, "NULL"); }
            else { literal(e, c, ce->value, &vals, false, true); literal(e, c, ce->value, &svals, true, true); }
            size_t l = strlen(note);
            snprintf(note + l, sizeof note - l, "%s%s = %s", l ? "  \xC2\xB7  " : "", e->rs->cols[c], ce->is_null ? "NULL" : edit_sensitive_name(e->rs->cols[c]) ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : ce->value);
            n++;
        }
        if (n) {
            sb_put(&shown, b.s);
            sb_put(&b, ") VALUES ("); sb_put(&b, vals.s); sb_put(&b, ")");
            sb_put(&shown, ") VALUES ("); sb_put(&shown, svals.s); sb_put(&shown, ")");
        } else {
            sb_reset(&b);
            sb_put(&b, "INSERT INTO "); sb_put(&b, e->qtable); sb_put(&b, is_oracle(e) ? " () VALUES ()" : " DEFAULT VALUES");
            sb_put(&shown, b.s);
            snprintf(note, sizeof note, "every column takes its default");
        }
        stmt_add(e, b.s, shown.s, note, 2, row);
        free(vals.s); free(svals.s);
    }
    free(b.s); free(shown.s);
}

/* See edit.h. */
void edit_describe_strategy(const EditSet *e, char *buf, size_t n) {
    if (!e->npk) { snprintf(buf, n, "no primary key: editing is off"); return; }
    size_t l = (size_t)snprintf(buf, n, "Conflict check: primary key (%d col%s) + snapshot of %d loaded column%s", e->npk, e->npk == 1 ? "" : "s", e->nsnap, e->nsnap == 1 ? "" : "s");
    if (e->version_col >= 0 && l < n) l += (size_t)snprintf(buf + l, n - l, " incl. version column %s", e->rs->cols[e->version_col]);
    if (e->snap_note[0] && l < n) snprintf(buf + l, n - l, "; not compared: %s", e->snap_note);
}

/* ---- apply ----------------------------------------------------------------- */

/* See edit.h. Indexed by the APPLY_* enum order. */
const char *edit_apply_status_name(int status) {
    static const char *const names[] = { "applied", "nothing to apply", "no connection", "transaction not opened", "statement failed", "conflict", "commit failed", "result unknown" };
    return status >= 0 && status < 8 ? names[status] : "?";
}

/* Executes one statement synchronously and reports success, affected rows and the error
 * text flattened to one line. affected / err may be NULL. */
static bool run(Db *db, const char *sql, int *affected, char *err, size_t n) {
    ResultSet rs;
    db->drv->exec(db, sql, -1, 0, &rs);
    bool ok = rs.ok;
    if (affected) *affected = rs.affected;
    if (err) { snprintf(err, n, "%s", rs.error); for (char *q = err; *q; q++) if (*q == '\n' || *q == '\t') *q = ' '; }
    rs_free(&rs);
    return ok;
}

/* True when the connection still answers a trivial query; tells a dead connection apart
 * from a statement-level failure. */
static bool probe(Db *db) {
    bool ora = db->drv->name && strcmp(db->drv->name, "Oracle") == 0;
    return run(db, ora ? "SELECT 1 FROM DUAL" : "SELECT 1", NULL, NULL, 0);
}

/* re-reads the row by key and says what differs from the loaded snapshot (US-048) */
static void diagnose(EditSet *e, Db *db, int row, char *out, size_t n) {
    SBuf w = { 0 };
    put_where_key(e, &w, row);
    char sql[2400];
    e->drv->select_sql(e->qtable, w.s, NULL, 2, sql, sizeof sql);
    free(w.s);
    ResultSet rs;
    db->drv->exec(db, sql, -1, 0, &rs);
    if (!rs.ok) { snprintf(out, n, "could not re-read the row: %s", rs.error); rs_free(&rs); return; }
    if (rs.nrows == 0) snprintf(out, n, "the row no longer exists (deleted by someone else)");
    else if (rs.nrows > 1) snprintf(out, n, "%d rows match the key: it is not unique on the server", rs.nrows);
    else {
        size_t l = (size_t)snprintf(out, n, "changed by someone else: ");
        int listed = 0;
        for (int c = 0; c < e->rs->ncols && l < n; c++) {
            int k = -1;
            for (int j = 0; j < rs.ncols; j++) if (strcasecmp(rs.cols[j], e->rs->cols[c]) == 0) { k = j; break; }
            if (k < 0) continue;
            const char *was = rs_cell(e->rs, row, c), *now = rs_cell(&rs, 0, k);
            if ((!was && !now) || (was && now && strcmp(was, now) == 0)) continue;
            bool sens = edit_sensitive_name(e->rs->cols[c]);
            l += (size_t)snprintf(out + l, n - l, "%s%s: you saw %s, now %s", listed ? "; " : "", e->rs->cols[c],
                                  sens ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : was ? was : "NULL", sens ? "\xE2\x80\xA2\xE2\x80\xA2\xE2\x80\xA2" : now ? now : "NULL");
            listed++;
            if (listed == 6) { snprintf(out + l, n - l, "; \xE2\x80\xA6"); break; }
        }
        if (!listed) snprintf(out + l, n - l, "a column that is not compared (%s) or a concurrent transaction", e->snap_note[0] ? e->snap_note : "none");
    }
    rs_free(&rs);
}

/* forward declaration: the batch body, run with autocommit off (see edit_apply) */
static int apply_batch(EditSet *e, Db *db, const EditHooks *h, ApplyResult *out);

/* See edit.h. Builds the statements, switches the driver's autocommit off for the batch
 * (Oracle) and restores it afterwards whatever the outcome. Blocking, UI thread. */
int edit_apply(EditSet *e, Db *db, const EditHooks *h, ApplyResult *out) {
    memset(out, 0, sizeof *out);
    out->stmt = -1;
    if (!db) { out->status = APPLY_NO_CONNECTION; snprintf(out->message, sizeof out->message, "connection closed: nothing was sent"); return out->status; }
    edit_build(e);
    if (!e->nstmts) { out->status = APPLY_NOTHING; snprintf(out->message, sizeof out->message, "nothing to apply"); return out->status; }
    const DbDriver *drv = db->drv;
    if (drv->set_autocommit) drv->set_autocommit(db, false);
    int st = apply_batch(e, db, h, out);
    if (drv->set_autocommit) drv->set_autocommit(db, true);
    return st;
}

/* Steps: open the transaction (tx_begin), run each statement and require exactly one row
 * for UPDATE / DELETE, on any failure ROLLBACK and classify (statement failed, conflict,
 * connection lost), then COMMIT and classify a COMMIT failure. */
static int apply_batch(EditSet *e, Db *db, const EditHooks *h, ApplyResult *out) {
    const DbDriver *drv = db->drv;
    char err[300];
    if (drv->tx_begin && !run(db, drv->tx_begin, NULL, err, sizeof err)) {
        out->status = APPLY_TX_FAILED;
        snprintf(out->message, sizeof out->message, "could not open a transaction: %s. Nothing was sent", err);
        return out->status;
    }
    for (int i = 0; i < e->nstmts; i++) {
        if (h && h->before) h->before(h->ud, i, e->stmts[i].sql);
        int affected = 0;
        bool ok = run(db, e->stmts[i].sql, &affected, err, sizeof err);
        if (ok && e->stmts[i].kind != 2 && affected == 1) continue;
        out->stmt = i; out->affected = affected;
        if (ok && e->stmts[i].kind == 2) continue;
        bool rb = run(db, "ROLLBACK", NULL, NULL, 0);
        if (!ok) {
            out->status = APPLY_STMT_FAILED;
            snprintf(out->message, sizeof out->message, "statement %d failed: %s. Rolled back%s", i + 1, err,
                     rb ? ": nothing was written" : !probe(db) ? " by the server (connection lost before commit): nothing was written" : "; the ROLLBACK itself failed, check the table");
            if (!rb && probe(db)) out->status = APPLY_UNKNOWN;
            return out->status;
        }
        out->status = APPLY_CONFLICT;
        snprintf(out->message, sizeof out->message, "statement %d matched %d row%s instead of 1. Rolled back: nothing was written", i + 1, affected, affected == 1 ? "" : "s");
        if (rb) diagnose(e, db, e->stmts[i].row, out->conflict, sizeof out->conflict);
        else snprintf(out->conflict, sizeof out->conflict, "rollback failed: %s", probe(db) ? "check the table" : "connection lost; the server discards the open transaction");
        return out->status;
    }
    /* every statement matched: commit */
    if (h && h->before) h->before(h->ud, e->nstmts, "COMMIT");
    if (!run(db, "COMMIT", NULL, err, sizeof err)) {
        if (!probe(db)) {
            out->status = APPLY_UNKNOWN;
            snprintf(out->message, sizeof out->message, "COMMIT was sent and the connection dropped before the answer (%s). The changes may or may not be in the database: check the table before applying again. Pending changes were kept", err);
            return out->status;
        }
        if (strcmp(drv->name, "SQLite") == 0) run(db, "ROLLBACK", NULL, NULL, 0);
        out->status = APPLY_COMMIT_FAILED;
        snprintf(out->message, sizeof out->message, "COMMIT failed: %s. The server did not keep the transaction: nothing was written", err);
        return out->status;
    }
    out->status = APPLY_OK;
    snprintf(out->message, sizeof out->message, "applied %d statement%s in one transaction and committed", e->nstmts, e->nstmts == 1 ? "" : "s");
    return out->status;
}
