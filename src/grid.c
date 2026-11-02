/* SPDX-License-Identifier: AGPL-3.0-only */
/* grid.c - headless result-grid model (see grid.h and docs/result-grid.md).
 *
 * A Grid wraps one ResultSet (rs.c) and adds everything the result grid does that is not
 * drawing: column layout (hide / show / move / freeze, persisted as a short string), local
 * sorting and filtering (RN-002: never re-queries the database), text search, rectangular
 * selection with a status-bar summary, display formatting of numbers / dates / booleans,
 * copy and file export (TSV, CSV, JSON, Markdown, SQL INSERT, XLSX), full-result streaming
 * export through the open cursor, and the derived sets "transpose" and "compare".
 *
 * Coordinates: the public API takes DISPLAY rows / columns. g->order maps a display row to
 * a result row (after filter + sort), g->cols maps a display column to a source column.
 * Sort keys, filters and the numeric flags are stored per SOURCE column so they survive
 * layout changes. Every change that moves rows or columns bumps g->gen so the UI (results.c)
 * can drop its caches.
 *
 * Ownership: the Grid borrows its ResultSet (the caller keeps it alive, frees it after
 * grid_free) and owns its index arrays. Strings returned by grid_copy* are malloc'd for the
 * caller. Derived sets (transpose / compare) are new ResultSets owned by the caller.
 *
 * Threading: UI thread only. Sorting and compare use file-level statics to pass context to
 * qsort (g_sort, g_key_numeric), so they are not reentrant. grid_export_full may block on
 * the driver while it fetches more rows. */
#include "grid.h"
#include "db.h"
#include <errno.h>
#include <time.h>
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* ---- helpers ----------------------------------------------------------- */

/* Parses s as a number (strtod syntax, trailing spaces allowed). False for NULL, "" or junk. */
static bool parse_num(const char *s, double *out) {
    if (!s || !*s) return false;
    char *end;
    *out = strtod(s, &end);
    while (*end == ' ') end++;
    return *end == 0;
}

/* Formats v compactly: integers without decimals (below 1e15), others with 10 significant
 * digits. Used by the selection summary. */
static void fmt_num(char *buf, size_t n, double v) {
    if (fabs(v) < 1e15 && v == floor(v)) snprintf(buf, n, "%.0f", v);
    else snprintf(buf, n, "%.10g", v);
}

/* Decides per source column whether to compare / align as numbers: from the driver kind when
 * known, otherwise by sampling the first 200 non-NULL values (all must parse as numbers). */
static void detect_numeric(Grid *g) {
    ResultSet *rs = g->rs;
    g->numeric = realloc(g->numeric, sizeof *g->numeric * (rs->ncols ? rs->ncols : 1));
    for (int c = 0; c < rs->ncols; c++) {
        int kind = rs->kinds[c];
        if (kind != CK_OTHER) { g->numeric[c] = kind == CK_NUMBER; continue; }
        /* unknown type: look at a sample of values */
        bool any = false, num = true;
        int sample = rs->nrows < 200 ? rs->nrows : 200;
        for (int i = 0; i < sample && num; i++) {
            const char *v = rs_cell(rs, i, c);
            if (!v) continue;
            any = true;
            double d;
            if (!parse_num(v, &d)) num = false;
        }
        g->numeric[c] = any && num;
    }
}

/* ---- sort -------------------------------------------------------------- */

/* Grid being sorted, for cmp_rows (qsort has no context pointer). */
static const Grid *g_sort;
/* Compares source column `col` of result rows a and b: NULLs last, numerically for numeric
 * columns when both parse, else case-insensitive text. */
static int cmp_cell(const Grid *g, int col, int a, int b) {
    const char *x = rs_cell(g->rs, a, col), *y = rs_cell(g->rs, b, col);
    if (!x || !y) return (x ? 0 : 1) - (y ? 0 : 1);     /* NULL last in ascending order */
    if (g->numeric[col]) {
        double dx, dy;
        if (parse_num(x, &dx) && parse_num(y, &dy)) return dx < dy ? -1 : dx > dy;
    }
    return strcasecmp(x, y);
}
/* qsort comparator over result-row indices using g_sort's sort keys in priority order; ties
 * keep the original row order so the sort is stable. */
static int cmp_rows(const void *pa, const void *pb) {
    int a = *(const int *)pa, b = *(const int *)pb;
    for (int k = 0; k < g_sort->nsort; k++) {
        int c = cmp_cell(g_sort, g_sort->sort_col[k], a, b);
        if (g_sort->sort_desc[k]) c = -c;
        if (c) return c;
    }
    return a - b;                                        /* stable: original order */
}

/* ---- filter ------------------------------------------------------------ */

/* Case-insensitive substring test; an empty needle always matches. */
static bool contains_ci(const char *hay, const char *needle) {
    size_t n = strlen(needle);
    if (!n) return true;
    for (const char *p = hay; *p; p++) if (strncasecmp(p, needle, n) == 0) return true;
    return false;
}

/* True when result row `row` passes one filter. NULL only matches IS NULL; comparisons are
 * numeric on numeric columns when both sides parse, else case-insensitive text. */
static bool filter_match(const Grid *g, const GridFilter *f, int row) {
    const char *v = rs_cell(g->rs, row, f->col);
    switch (f->op) {
    case OP_IS_NULL: return v == NULL;
    case OP_NOT_NULL: return v != NULL;
    default: break;
    }
    if (!v) return false;                                /* NULL never matches a value test */
    bool numeric = g->numeric[f->col];
    double a, b, x;
    switch (f->op) {
    case OP_CONTAINS: return contains_ci(v, f->value);
    case OP_STARTS: return strncasecmp(v, f->value, strlen(f->value)) == 0;
    case OP_EQUALS:
        if (numeric && parse_num(v, &x) && parse_num(f->value, &a)) return x == a;
        return strcasecmp(v, f->value) == 0;
    case OP_GT:
        if (numeric && parse_num(v, &x) && parse_num(f->value, &a)) return x > a;
        return strcasecmp(v, f->value) > 0;
    case OP_LT:
        if (numeric && parse_num(v, &x) && parse_num(f->value, &a)) return x < a;
        return strcasecmp(v, f->value) < 0;
    case OP_BETWEEN:
        if (numeric && parse_num(v, &x) && parse_num(f->value, &a) && parse_num(f->value2, &b)) return x >= a && x <= b;
        return strcasecmp(v, f->value) >= 0 && strcasecmp(v, f->value2) <= 0;
    }
    return true;
}

/* Recomputes g->order: keeps rows that pass the filters (AND, or OR with filter_any), sorts
 * them with the active keys, drops a selection now out of range, redoes the search and bumps
 * gen. Called after every filter / sort change and when rows arrive. */
static void apply(Grid *g) {
    ResultSet *rs = g->rs;
    g->order = realloc(g->order, sizeof *g->order * (rs->nrows ? rs->nrows : 1));
    g->nvis = 0;
    for (int i = 0; i < rs->nrows; i++) {
        bool ok = !g->filter_any || g->nfilters == 0;
        if (g->filter_any) { for (int k = 0; k < g->nfilters && !ok; k++) ok = filter_match(g, &g->filters[k], i); }
        else for (int k = 0; k < g->nfilters && ok; k++) ok = filter_match(g, &g->filters[k], i);
        if (ok) g->order[g->nvis++] = i;
    }
    if (g->nsort) { g_sort = g; qsort(g->order, g->nvis, sizeof *g->order, cmp_rows); }
    if (g->sel_r >= g->nvis || g->sel_c >= g->ncols) grid_select_none(g);
    if (g->search[0]) grid_search(g, g->search);
    g->gen++;
}

/* ---- columns ----------------------------------------------------------- */

/* A layout change moves search hits instead of searching every cell again (hiding 100 of
 * 200 columns with a search active took seconds otherwise): hits of hidden columns are
 * dropped, the rest get their new display column. Showing a column searches again, since
 * its hits were dropped. */
static void remap_hits(Grid *g, const int *old_cols, int old_n, bool full) {
    if (!g->search[0]) return;
    if (full) { grid_search(g, g->search); return; }
    int nc = g->rs->ncols;
    int *disp = malloc(sizeof *disp * (nc ? nc : 1));
    for (int c = 0; c < nc; c++) disp[c] = -1;
    for (int i = 0; i < g->ncols; i++) disp[g->cols[i]] = i;
    int keep = 0;
    for (int h = 0; h < g->nhits; h++) {
        int c = g->hits[h * 2 + 1];
        int src = c < old_n ? old_cols[c] : -1;
        int nd = src >= 0 && src < nc ? disp[src] : -1;
        if (nd < 0) continue;
        g->hits[keep * 2] = g->hits[h * 2]; g->hits[keep * 2 + 1] = nd; keep++;
    }
    g->nhits = keep;
    if (g->hit >= keep) g->hit = keep ? keep - 1 : -1;
    free(disp);
}

/* Rebuilds g->cols from g->hidden, keeping the current display order, dropping hidden columns
 * and appending newly shown ones at the end. shown = some column became visible (search hits
 * are recomputed instead of remapped). Clears the selection and bumps gen. */
static void rebuild_cols(Grid *g, bool shown) {
    /* keep the current display order, drop hidden columns, append newly shown ones */
    ResultSet *rs = g->rs;
    int *fresh = malloc(sizeof *fresh * (rs->ncols ? rs->ncols : 1));
    bool *placed = calloc(rs->ncols ? rs->ncols : 1, sizeof *placed);
    int n = 0;
    for (int i = 0; i < g->ncols; i++) {
        int src = g->cols[i];
        if (src < rs->ncols && !g->hidden[src] && !placed[src]) { fresh[n++] = src; placed[src] = true; }
    }
    for (int src = 0; src < rs->ncols; src++) if (!g->hidden[src] && !placed[src]) fresh[n++] = src;
    int *old = g->cols; int old_n = g->ncols;
    free(placed);
    g->cols = fresh; g->ncols = n;
    if (g->frozen > n) g->frozen = n;
    grid_select_none(g);
    remap_hits(g, old, old_n, shown);
    free(old);
    g->gen++;
}

/* Hides display column col (never the last visible one); adjusts the frozen count. */
void grid_col_hide(Grid *g, int col) {
    if (col < 0 || col >= g->ncols || g->ncols <= 1) return;
    g->hidden[g->cols[col]] = true;
    if (col < g->frozen) g->frozen--;
    rebuild_cols(g, false);
}

/* Shows source column src again; it is appended at the right end. */
void grid_col_show(Grid *g, int src) {
    if (src < 0 || src >= g->rs->ncols || !g->hidden[src]) return;
    g->hidden[src] = false;
    rebuild_cols(g, true);
}

/* Unhides every column (existing order kept, previously hidden ones appended). */
void grid_col_show_all(Grid *g) {
    for (int c = 0; c < g->rs->ncols; c++) g->hidden[c] = false;
    rebuild_cols(g, true);
}

/* Number of hidden source columns. */
int grid_hidden_count(const Grid *g) {
    int n = 0;
    for (int c = 0; c < g->rs->ncols; c++) n += g->hidden[c];
    return n;
}

/* Moves display column `from` to position `to`, shifting the columns in between. */
void grid_col_move(Grid *g, int from, int to) {
    if (from < 0 || from >= g->ncols || to < 0 || to >= g->ncols || from == to) return;
    int src = g->cols[from];
    int *old = malloc(sizeof *old * g->ncols); memcpy(old, g->cols, sizeof *old * g->ncols);
    if (from < to) memmove(&g->cols[from], &g->cols[from + 1], sizeof *g->cols * (to - from));
    else memmove(&g->cols[to + 1], &g->cols[to], sizeof *g->cols * (from - to));
    g->cols[to] = src;
    grid_select_none(g);
    remap_hits(g, old, g->ncols, false);
    free(old);
    g->gen++;
}

/* Pins the first n display columns (clamped to 0..ncols). */
void grid_freeze(Grid *g, int n) {
    if (n < 0) n = 0;
    if (n > g->ncols) n = g->ncols;
    g->frozen = n;
    g->gen++;
}

/* Back to the driver layout: all columns visible, source order, nothing frozen. */
void grid_layout_reset(Grid *g) {
    for (int c = 0; c < g->rs->ncols; c++) g->hidden[c] = false;
    g->ncols = 0;              /* rebuild appends every column in source order */
    g->frozen = 0;
    rebuild_cols(g, true);
}

/* True when the layout is the driver's: source order, nothing hidden or frozen. */
bool grid_layout_default(const Grid *g) {
    if (g->frozen || g->ncols != g->rs->ncols) return false;
    for (int i = 0; i < g->ncols; i++) if (g->cols[i] != i) return false;
    return true;
}

/* Writes "order=a,b,c;hidden=x,y;frozen=n" (source column indices) into buf, truncating at n. */
void grid_layout_save(const Grid *g, char *buf, size_t n) {
    size_t p = 0;
    p += snprintf(buf + p, n - p, "order=");
    for (int i = 0; i < g->ncols && p < n; i++) p += snprintf(buf + p, n - p, "%s%d", i ? "," : "", g->cols[i]);
    if (p < n) p += snprintf(buf + p, n - p, ";hidden=");
    bool first = true;
    for (int c = 0; c < g->rs->ncols && p < n; c++) if (g->hidden[c]) { p += snprintf(buf + p, n - p, "%s%d", first ? "" : ",", c); first = false; }
    if (p < n) snprintf(buf + p, n - p, ";frozen=%d", g->frozen);
}

/* Value of "key=" in a ';'-separated layout string (pointer into s), or NULL. */
static const char *find_key(const char *s, const char *key) {
    size_t kl = strlen(key);
    for (const char *p = s; *p; p++) {
        if ((p == s || p[-1] == ';') && strncmp(p, key, kl) == 0 && p[kl] == '=') return p + kl + 1;
    }
    return NULL;
}

/* Restores a layout saved by grid_layout_save. Rejects strings whose order lists an unknown or
 * repeated column (layout unchanged, returns false). Columns missing from order are appended;
 * hidden entries that are also in order are ignored. */
bool grid_layout_load(Grid *g, const char *s) {
    const char *order = find_key(s, "order"), *hidden = find_key(s, "hidden"), *frozen = find_key(s, "frozen");
    if (!order) return false;
    int nc = g->rs->ncols;
    int *cols = malloc(sizeof *cols * (nc ? nc : 1));
    bool *seen = calloc(nc ? nc : 1, sizeof *seen);
    int n = 0;
    for (const char *p = order; *p && *p != ';';) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p || v < 0 || v >= nc || seen[v]) { free(cols); free(seen); return false; }
        cols[n++] = (int)v; seen[v] = true;
        p = end;
        if (*p == ',') p++;
    }
    for (int c = 0; c < nc; c++) g->hidden[c] = false;
    if (hidden) for (const char *p = hidden; *p && *p != ';';) {
        char *end;
        long v = strtol(p, &end, 10);
        if (end == p) break;
        if (v >= 0 && v < nc && !seen[v]) g->hidden[v] = true;
        p = end;
        if (*p == ',') p++;
    }
    free(g->cols); g->cols = cols; g->ncols = n;
    free(seen);
    g->frozen = frozen ? atoi(frozen) : 0;
    rebuild_cols(g, true);
    return true;
}

/* ---- lifecycle --------------------------------------------------------- */

/* See grid.h. Borrows rs; starts with the driver layout, no sort / filter / selection, and
 * numbers shown as stored. */
void grid_init(Grid *g, ResultSet *rs) {
    memset(g, 0, sizeof *g);
    g->rs = rs;
    g->fmt.decimals = -1;
    g->sel_r = g->sel_c = g->anc_r = g->anc_c = -1;
    g->hidden = calloc(rs->ncols ? rs->ncols : 1, sizeof *g->hidden);
    detect_numeric(g);
    rebuild_cols(g, true);
    apply(g);
}

/* Frees the Grid's own arrays (not the ResultSet) and zeroes it. */
void grid_free(Grid *g) {
    free(g->numeric); free(g->order); free(g->hits); free(g->cols); free(g->hidden);
    memset(g, 0, sizeof *g);
}

/* After rows were appended to the ResultSet: re-detects numeric columns and re-applies
 * filter, sort and search. */
void grid_refresh(Grid *g) { detect_numeric(g); apply(g); }

/* ---- sort API ---------------------------------------------------------- */

/* Position of source column src in the sort keys, or -1. */
static int sort_index(const Grid *g, int src) {
    for (int k = 0; k < g->nsort; k++) if (g->sort_col[k] == src) return k;
    return -1;
}

/* See grid.h: 1-based priority of display column col in the sort, 0 if unsorted; *desc
 * (optional) gets its direction. */
int grid_sort_priority(const Grid *g, int col, bool *desc) {
    if (col < 0 || col >= g->ncols) return 0;
    int k = sort_index(g, g->cols[col]);
    if (k < 0) return 0;
    if (desc) *desc = g->sort_desc[k];
    return k + 1;
}

/* Header click: cycles the column through ascending, descending, unsorted. Without additive
 * it becomes the only key; with additive it is added as the next key (up to GRID_MAX_SORT). */
void grid_sort_click(Grid *g, int col, bool additive) {
    if (col < 0 || col >= g->ncols) return;
    int src = g->cols[col];
    int k = sort_index(g, src);
    if (!additive && (g->nsort > 1 || k < 0)) { g->nsort = 0; k = -1; }
    if (k < 0) {
        if (g->nsort < GRID_MAX_SORT) { g->sort_col[g->nsort] = src; g->sort_desc[g->nsort] = false; g->nsort++; }
    } else if (!g->sort_desc[k]) g->sort_desc[k] = true;
    else {
        memmove(&g->sort_col[k], &g->sort_col[k + 1], sizeof *g->sort_col * (g->nsort - k - 1));
        memmove(&g->sort_desc[k], &g->sort_desc[k + 1], sizeof *g->sort_desc * (g->nsort - k - 1));
        g->nsort--;
    }
    apply(g);
}

/* Removes every sort key (back to result order). */
void grid_sort_clear(Grid *g) { g->nsort = 0; apply(g); }

/* ---- filter API -------------------------------------------------------- */

/* Short operator text for filter chips ("contains", "=", ...). Static string. */
const char *grid_op_name(int op) {
    static const char *const names[] = { "contains", "=", "starts with", ">", "<", "between", "is NULL", "is not NULL" };
    return op >= 0 && op < OP_COUNT ? names[op] : "?";
}

/* Adds a filter on display column col (stored by source column) and re-applies. Values are
 * copied (truncated to 255 bytes); value2 is the upper bound of BETWEEN. False when the
 * filter list is full or col is invalid. */
bool grid_filter_add(Grid *g, int col, int op, const char *value, const char *value2) {
    if (g->nfilters >= GRID_MAX_FILTERS || col < 0 || col >= g->ncols) return false;
    GridFilter *f = &g->filters[g->nfilters++];
    f->col = g->cols[col]; f->op = op;
    snprintf(f->value, sizeof f->value, "%s", value ? value : "");
    snprintf(f->value2, sizeof f->value2, "%s", value2 ? value2 : "");
    apply(g);
    return true;
}

/* Removes filter idx and re-applies. */
void grid_filter_remove(Grid *g, int idx) {
    if (idx < 0 || idx >= g->nfilters) return;
    memmove(&g->filters[idx], &g->filters[idx + 1], sizeof *g->filters * (g->nfilters - idx - 1));
    g->nfilters--;
    apply(g);
}

/* Removes all filters; grid_filter_mode switches AND / OR between filters. */
void grid_filter_clear(Grid *g) { g->nfilters = 0; apply(g); }
/* Switches between AND (any = false) and OR between filters; re-applies only on a change. */
void grid_filter_mode(Grid *g, bool any) { if (g->filter_any != any) { g->filter_any = any; apply(g); } }

/* Human-readable form of filter idx ("price between 1 and 5") for its chip. */
void grid_filter_describe(const Grid *g, int idx, char *buf, size_t n) {
    const GridFilter *f = &g->filters[idx];
    const char *col = g->rs->cols[f->col];
    if (f->op == OP_IS_NULL || f->op == OP_NOT_NULL) snprintf(buf, n, "%s %s", col, grid_op_name(f->op));
    else if (f->op == OP_BETWEEN) snprintf(buf, n, "%s between %s and %s", col, f->value, f->value2);
    else snprintf(buf, n, "%s %s %s", col, grid_op_name(f->op), f->value);
}

/* ---- search ------------------------------------------------------------ */

/* Stores needle (may be g->search itself) and collects every matching visible cell as a
 * (display row, display column) pair, case-insensitive, capped at 100,000 hits. NULL cells
 * never match. An empty needle clears the search. */
void grid_search(Grid *g, const char *needle) {
    if (needle != g->search) snprintf(g->search, sizeof g->search, "%s", needle ? needle : "");
    g->nhits = 0; g->hit = -1;
    if (!g->search[0]) return;
    int cap = 0;
    for (int i = 0; i < g->nvis; i++) {
        for (int c = 0; c < g->ncols; c++) {
            const char *v = rs_cell(g->rs, g->order[i], g->cols[c]);
            if (!v || !contains_ci(v, g->search)) continue;     /* NULL cells never match */
            if (g->nhits == cap) { cap = cap ? cap * 2 : 64; g->hits = realloc(g->hits, sizeof *g->hits * 2 * cap); }
            g->hits[g->nhits * 2] = i; g->hits[g->nhits * 2 + 1] = c;
            g->nhits++;
            if (g->nhits >= 100000) return;                     /* enough to navigate */
        }
    }
}

/* Moves to the next (dir >= 0) or previous hit, wrapping, and returns its display cell in
 * *row / *col. False when there are no hits. */
bool grid_search_step(Grid *g, int dir, int *row, int *col) {
    if (!g->nhits) return false;
    if (g->hit < 0) g->hit = dir >= 0 ? 0 : g->nhits - 1;
    else g->hit = (g->hit + (dir >= 0 ? 1 : g->nhits - 1)) % g->nhits;
    *row = g->hits[g->hit * 2]; *col = g->hits[g->hit * 2 + 1];
    return true;
}

/* True when the display cell matches the current search (used while drawing). */
bool grid_is_hit(const Grid *g, int row, int col) {
    if (!g->search[0]) return false;
    const char *v = rs_cell(g->rs, g->order[row], g->cols[col]);
    return v && contains_ci(v, g->search);
}

/* ---- selection --------------------------------------------------------- */

/* Clears the selection and the anchor. */
void grid_select_none(Grid *g) { g->sel_r = g->sel_c = g->anc_r = g->anc_c = -1; g->sel_mode = SEL_CELLS; }

/* Selects a display cell; extend keeps the existing anchor (Shift+click / Shift+arrows). */
void grid_select_cell(Grid *g, int row, int col, bool extend) {
    if (row < 0 || row >= g->nvis || col < 0 || col >= g->ncols) return;
    g->sel_r = row; g->sel_c = col;
    if (!extend || g->anc_r < 0) { g->anc_r = row; g->anc_c = col; }
    g->sel_mode = SEL_CELLS;
}

/* Selects whole display rows from the anchor row to row (extend) or just row. */
void grid_select_row(Grid *g, int row, bool extend) {
    if (row < 0 || row >= g->nvis) return;
    g->sel_r = row; g->sel_c = 0;
    if (!extend || g->anc_r < 0 || g->sel_mode != SEL_ROWS) g->anc_r = row;
    g->anc_c = g->ncols - 1;
    g->sel_mode = SEL_ROWS;
}

/* Selects whole display columns from the anchor column to col (extend) or just col. */
void grid_select_col(Grid *g, int col, bool extend) {
    if (col < 0 || col >= g->ncols || !g->nvis) return;
    g->sel_c = col; g->sel_r = 0;
    if (!extend || g->anc_c < 0 || g->sel_mode != SEL_COLS) g->anc_c = col;
    g->anc_r = g->nvis - 1;
    g->sel_mode = SEL_COLS;
}

/* Selects every visible cell. */
void grid_select_all(Grid *g) {
    if (!g->nvis) return;
    g->sel_r = 0; g->sel_c = 0; g->anc_r = g->nvis - 1; g->anc_c = g->ncols - 1;
    g->sel_mode = SEL_CELLS;
}

/* Normalised selection rectangle in display coordinates (inclusive); row / column modes span
 * all columns / rows. False when nothing is selected. */
bool grid_sel_rect(const Grid *g, int *r0, int *c0, int *r1, int *c1) {
    if (g->sel_r < 0) return false;
    int ar = g->anc_r < 0 ? g->sel_r : g->anc_r, ac = g->anc_c < 0 ? g->sel_c : g->anc_c;
    *r0 = ar < g->sel_r ? ar : g->sel_r; *r1 = ar > g->sel_r ? ar : g->sel_r;
    *c0 = ac < g->sel_c ? ac : g->sel_c; *c1 = ac > g->sel_c ? ac : g->sel_c;
    if (g->sel_mode == SEL_ROWS) { *c0 = 0; *c1 = g->ncols - 1; }
    if (g->sel_mode == SEL_COLS) { *r0 = 0; *r1 = g->nvis - 1; }
    if (*c1 >= g->ncols) *c1 = g->ncols - 1;
    return true;
}

/* True when the display cell lies in the selection. */
bool grid_in_sel(const Grid *g, int row, int col) {
    int r0, c0, r1, c1;
    if (!grid_sel_rect(g, &r0, &c0, &r1, &c1)) return false;
    return row >= r0 && row <= r1 && col >= c0 && col <= c1;
}

/* Status-bar text for a selection of more than one cell: its size, non-NULL count, and for a
 * single numeric column sum / avg / min / max. Empty string otherwise. */
void grid_sel_summary(const Grid *g, char *buf, size_t n) {
    int r0, c0, r1, c1;
    buf[0] = 0;
    if (!grid_sel_rect(g, &r0, &c0, &r1, &c1)) return;
    long cells = (long)(r1 - r0 + 1) * (c1 - c0 + 1);
    if (cells <= 1) return;
    long nonnull = 0, nums = 0;
    double sum = 0, mn = INFINITY, mx = -INFINITY;
    for (int i = r0; i <= r1; i++) for (int c = c0; c <= c1; c++) {
        int src = g->cols[c];
        const char *v = rs_cell(g->rs, g->order[i], src);
        if (!v) continue;
        nonnull++;
        double d;
        if (g->numeric[src] && parse_num(v, &d)) { nums++; sum += d; if (d < mn) mn = d; if (d > mx) mx = d; }
    }
    size_t p = 0;
    if (g->sel_mode == SEL_ROWS) p += snprintf(buf + p, n - p, "%d row%s selected", r1 - r0 + 1, r1 - r0 ? "s" : "");
    else if (g->sel_mode == SEL_COLS) p += snprintf(buf + p, n - p, "%d column%s selected", c1 - c0 + 1, c1 - c0 ? "s" : "");
    else p += snprintf(buf + p, n - p, "%d\xC3\x97%d selection (%ld cells)", r1 - r0 + 1, c1 - c0 + 1, cells);
    p += snprintf(buf + p, n - p, "  \xC2\xB7  %ld non-null", nonnull);
    /* aggregates only make sense within one numeric column */
    if (nums > 0 && c0 == c1) {
        char fs[48], fa[48], fmn[48], fmx[48];
        double avg = sum / nums;
        fmt_num(fs, sizeof fs, sum); fmt_num(fmn, sizeof fmn, mn); fmt_num(fmx, sizeof fmx, mx);
        if (avg == floor(avg)) snprintf(fa, sizeof fa, "%.0f", avg); else snprintf(fa, sizeof fa, "%.2f", avg);
        p += snprintf(buf + p, n - p, "  \xC2\xB7  sum %s  \xC2\xB7  avg %s  \xC2\xB7  min %s  \xC2\xB7  max %s", fs, fa, fmn, fmx);
    } else if (nums > 0) {
        p += snprintf(buf + p, n - p, "  \xC2\xB7  select one column for sum / avg / min / max");
    }
    (void)p;
}

/* ---- values and display formatting (US-017 / US-018) ------------------- */

/* Raw text of a display cell; NULL for SQL NULL. Pointer into the ResultSet arena. */
const char *grid_cell(const Grid *g, int row, int col) { return rs_cell(g->rs, g->order[row], g->cols[col]); }

/* plain decimal number: [-]digits[.digits]; exponents and spaces are left alone */
static bool plain_number(const char *v, int *int_digits, const char **dot) {
    const char *p = v;
    if (*p == '-' || *p == '+') p++;
    int d = 0;
    while (*p >= '0' && *p <= '9') { p++; d++; }
    if (!d) return false;
    *int_digits = d;
    *dot = NULL;
    if (*p == '.') { *dot = p; p++; while (*p >= '0' && *p <= '9') p++; }
    return *p == 0;
}

/* Applies grouping / fixed decimals to a plain decimal number in v, writing into tmp. Fixed
 * decimals go through a double only for integer parts of up to 15 digits; grouping is done on
 * the text so long integers stay exact. Non-plain values are copied unchanged. */
static void format_number(const GridFormat *f, const char *v, char *tmp, size_t n) {
    int idig; const char *dot;
    if (!plain_number(v, &idig, &dot)) { snprintf(tmp, n, "%s", v); return; }
    char work[96];
    if (f->decimals >= 0 && idig <= 15) {
        /* fixed decimals through a double only when the integer part fits exactly */
        snprintf(work, sizeof work, "%.*f", f->decimals, strtod(v, NULL));
        v = work;
        plain_number(v, &idig, &dot);
    }
    if (!f->group || idig <= 3) { snprintf(tmp, n, "%s", v); return; }
    /* textual grouping keeps 20-digit integers exact */
    size_t p = 0;
    const char *s = v;
    if (*s == '-' || *s == '+') tmp[p++] = *s++;
    for (int i = 0; i < idig && p < n - 1; i++) {
        if (i && (idig - i) % 3 == 0) tmp[p++] = ',';
        tmp[p++] = s[i];
    }
    tmp[p] = 0;
    if (dot) snprintf(tmp + p, n - p, "%s", dot);
}

/* Parsed ISO date / timestamp: fields, up to 9 fractional digits kept as text, and the UTC
 * offset in minutes when one was given. */
typedef struct { int y, mo, d, h, mi, s; int frac_digits; char frac[10]; bool has_time, has_off; int off_min; } Dt;

/* Parses "YYYY-MM-DD[( |T)HH:MM[:SS][.frac][Z|+HH[:MM]|-HH[MM]]]". *rest points where parsing
 * stopped; returns true only when the whole string was consumed. */
static bool parse_dt(const char *v, Dt *dt, const char **rest) {
    memset(dt, 0, sizeof *dt);
    const char *p = v;
    if (!(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && isdigit((unsigned char)p[3]) && p[4] == '-')) return false;
    dt->y = atoi(p); p += 5;
    if (!(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]) && p[2] == '-')) return false;
    dt->mo = atoi(p); p += 3;
    if (!(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]))) return false;
    dt->d = atoi(p); p += 2;
    if (dt->mo < 1 || dt->mo > 12 || dt->d < 1 || dt->d > 31) return false;
    if ((*p == 'T' || *p == ' ') && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2]) && p[3] == ':') {
        p++;
        dt->has_time = true;
        dt->h = atoi(p); p += 3;
        if (!(isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1]))) return false;
        dt->mi = atoi(p); p += 2;
        if (*p == ':' && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2])) { dt->s = atoi(p + 1); p += 3; }
        if (*p == '.' || *p == ',') {
            p++;
            while (isdigit((unsigned char)*p) && dt->frac_digits < 9) dt->frac[dt->frac_digits++] = *p++;
            while (isdigit((unsigned char)*p)) p++;
        }
        if (*p == 'Z') { dt->has_off = true; dt->off_min = 0; p++; }
        else if ((*p == '+' || *p == '-') && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2])) {
            int sign = *p == '-' ? -1 : 1;
            int oh = atoi(p + 1), om = 0;
            p += 3;
            if (*p == ':' && isdigit((unsigned char)p[1]) && isdigit((unsigned char)p[2])) { om = atoi(p + 1); p += 3; }
            else if (isdigit((unsigned char)p[0]) && isdigit((unsigned char)p[1])) { om = atoi(p); p += 2; }
            dt->has_off = true; dt->off_min = sign * (oh * 60 + om);
        }
    }
    *rest = p;
    return *p == 0;
}

/* Converts dt to the same instant expressed at UTC offset new_off_min (minutes). */
static void shift_dt(Dt *dt, int new_off_min) {
    /* move the wall-clock to another offset without touching the instant */
    struct tm tm = { 0 };
    tm.tm_year = dt->y - 1900; tm.tm_mon = dt->mo - 1; tm.tm_mday = dt->d;
    tm.tm_hour = dt->h; tm.tm_min = dt->mi; tm.tm_sec = dt->s;
    time_t t = timegm(&tm) - dt->off_min * 60 + new_off_min * 60;
    struct tm out;
    gmtime_r(&t, &out);
    dt->y = out.tm_year + 1900; dt->mo = out.tm_mon + 1; dt->d = out.tm_mday;
    dt->h = out.tm_hour; dt->mi = out.tm_min; dt->s = out.tm_sec;
    dt->off_min = new_off_min;
}

/* UTC offset (minutes) of the local time zone at the instant described by dt, so DST is right. */
static int local_offset_min(const Dt *dt) {
    struct tm tm = { 0 };
    tm.tm_year = dt->y - 1900; tm.tm_mon = dt->mo - 1; tm.tm_mday = dt->d;
    tm.tm_hour = dt->h; tm.tm_min = dt->mi; tm.tm_sec = dt->s;
    time_t t = timegm(&tm) - dt->off_min * 60;
    struct tm lt;
    localtime_r(&t, &lt);
#ifdef _WIN32   /* no tm_gmtoff: the local wall clock read back as if it were UTC, minus the instant */
    return (int)((timegm(&lt) - t) / 60);
#else
    return (int)(lt.tm_gmtoff / 60);
#endif
}

/* Re-renders an ISO date / timestamp per the date style and time-zone mode into tmp. Returns
 * false (caller keeps the stored text) when v is not a date or nothing would change. */
static bool format_datetime(const GridFormat *f, const char *v, char *tmp, size_t n) {
    Dt dt; const char *rest;
    if (!parse_dt(v, &dt, &rest)) return false;
    bool converted = false;
    if (dt.has_time && dt.has_off && f->tz != TZ_AS_STORED) {
        int want = f->tz == TZ_UTC ? 0 : local_offset_min(&dt);
        if (want != dt.off_min) { shift_dt(&dt, want); converted = true; }
        else if (f->tz == TZ_UTC) converted = true;
    }
    if (!converted && f->date_style == DATE_AS_STORED) return false;
    size_t p = 0;
    if (f->date_style == DATE_DMY) p += snprintf(tmp + p, n - p, "%02d/%02d/%04d", dt.d, dt.mo, dt.y);
    else if (f->date_style == DATE_MDY) p += snprintf(tmp + p, n - p, "%02d/%02d/%04d", dt.mo, dt.d, dt.y);
    else p += snprintf(tmp + p, n - p, "%04d-%02d-%02d", dt.y, dt.mo, dt.d);
    if (dt.has_time) {
        p += snprintf(tmp + p, n - p, " %02d:%02d:%02d", dt.h, dt.mi, dt.s);
        if (dt.frac_digits) p += snprintf(tmp + p, n - p, ".%s", dt.frac);
        if (dt.has_off) {
            int o = dt.off_min, ao = o < 0 ? -o : o;
            p += snprintf(tmp + p, n - p, "%c%02d:%02d", o < 0 ? '-' : '+', ao / 60, ao % 60);
        }
    }
    return true;
}

/* See grid.h: summary of a timestamp's offset and fractional precision for the value viewer. */
bool grid_describe_datetime(const char *v, char *buf, size_t n) {
    Dt dt; const char *rest;
    if (!v || !parse_dt(v, &dt, &rest)) return false;
    if (!dt.has_time) { snprintf(buf, n, "date, no time part"); return true; }
    char off[32];
    if (dt.has_off) { int o = dt.off_min, ao = o < 0 ? -o : o; snprintf(off, sizeof off, "offset %c%02d:%02d", o < 0 ? '-' : '+', ao / 60, ao % 60); }
    else snprintf(off, sizeof off, "no time zone (shown as stored)");
    const char *prec = dt.frac_digits == 0 ? "seconds" : dt.frac_digits <= 3 ? "milliseconds" : dt.frac_digits <= 6 ? "microseconds" : "nanoseconds";
    snprintf(buf, n, "%s, precision %s", off, prec);
    return true;
}

/* See grid.h. Display form of a value of source column src: 't' / 'f' booleans as words, then
 * number formatting for numeric columns, then date / time formatting. Returns v when unchanged,
 * tmp when reformatted, NULL for NULL. */
const char *grid_format_value(const Grid *g, int src, const char *v, char *tmp, size_t n) {
    if (!v) return NULL;
    int kind = g->rs->kinds[src];
    if (kind == CK_BOOL && (v[0] == 't' || v[0] == 'f') && !v[1]) { snprintf(tmp, n, "%s", v[0] == 't' ? "true" : "false"); return tmp; }
    const GridFormat *f = &g->fmt;
    if ((f->group || f->decimals >= 0) && g->numeric[src] && *v) {
        format_number(f, v, tmp, n);
        return strcmp(tmp, v) == 0 ? v : tmp;
    }
    if ((f->date_style != DATE_AS_STORED || f->tz != TZ_AS_STORED) && (kind == CK_DATE || kind == CK_TEXT || kind == CK_OTHER) && isdigit((unsigned char)v[0])) {
        if (format_datetime(f, v, tmp, n)) return tmp;
    }
    return v;
}

/* Same as grid_format_value for a display cell. */
const char *grid_display(const Grid *g, int row, int col, char *tmp, size_t n) {
    return grid_format_value(g, g->cols[col], grid_cell(g, row, col), tmp, n);
}

/* ---- serialisation ----------------------------------------------------- */

/* Growable output buffer for copies and export blocks; s is always NUL-terminated. */
typedef struct { char *s; size_t len, cap; } Buf;
/* Appends n bytes, growing the buffer (doubling the needed size). */
static void put(Buf *b, const char *s, size_t n) {
    if (b->len + n + 1 > b->cap) { b->cap = (b->len + n + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, s, n); b->len += n; b->s[b->len] = 0;
}
/* Appends a NUL-terminated string. */
static void puts_(Buf *b, const char *s) { put(b, s, strlen(s)); }
/* Appends the configured line ending. */
static void put_nl(Buf *b, const ExportOpts *o) { puts_(b, o->crlf ? "\r\n" : "\n"); }

/* True when a CSV field must be quoted (contains the separator, a quote or a line break). */
static bool needs_quote(const char *v, char sep) {
    for (const char *p = v; *p; p++) if (*p == sep || *p == '"' || *p == '\n' || *p == '\r') return true;
    return false;
}

/* Writes SQL NULL for CSV / TSV per null_as: nothing, NULL or \N. */
static void put_null(Buf *b, const ExportOpts *o) {
    if (o->null_as == NULL_WORD) puts_(b, "NULL");
    else if (o->null_as == NULL_BACKSLASH_N) puts_(b, "\\N");
}

/* US-046: formula-injection guard, see grid.h */
/* True when the guard option is on and v would start a spreadsheet formula after leading
 * spaces / control characters; numbers in numeric columns are exempt. */
static bool needs_guard(const char *v, bool numeric, const ExportOpts *o) {
    if (!o->guard) return false;
    const unsigned char *p = (const unsigned char *)v;
    while (*p && (*p == ' ' || *p < 0x20 || *p == 0x7F)) p++;
    if (!(*p == '=' || *p == '+' || *p == '-' || *p == '@')) return false;
    double d;
    if (numeric && parse_num(v, &d)) return false;
    return true;
}

/* Writes one CSV field: NULL per options, quoted when needed / quote_all / guarded (guarded
 * fields get a leading apostrophe inside the quotes), inner quotes doubled. */
static void put_csv(Buf *b, const char *v, const ExportOpts *o, bool numeric) {
    if (!v) { put_null(b, o); return; }
    bool dangerous = needs_guard(v, numeric, o);
    if (o->quote_all || needs_quote(v, o->sep) || dangerous) {
        puts_(b, "\"");
        if (dangerous) puts_(b, "'");
        for (const char *p = v; *p; p++) { if (*p == '"') puts_(b, "\""); put(b, p, 1); }
        puts_(b, "\"");
    } else puts_(b, v);
}

/* Writes one TSV field: escapes tab, newline, CR and backslash; guarded values get a leading
 * apostrophe. */
static void put_tsv(Buf *b, const char *v, const ExportOpts *o, bool numeric) {
    if (!v) { put_null(b, o); return; }
    if (needs_guard(v, numeric, o)) puts_(b, "'");
    for (const char *p = v; *p; p++) {
        if (*p == '\t') puts_(b, "\\t");
        else if (*p == '\n') puts_(b, "\\n");
        else if (*p == '\r') puts_(b, "\\r");
        else if (*p == '\\') puts_(b, "\\\\");
        else put(b, p, 1);
    }
}

/* Writes v as a JSON string literal. */
static void put_json_str(Buf *b, const char *v) {
    puts_(b, "\"");
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        char esc[8];
        switch (*p) {
        case '"': puts_(b, "\\\""); break;
        case '\\': puts_(b, "\\\\"); break;
        case '\n': puts_(b, "\\n"); break;
        case '\r': puts_(b, "\\r"); break;
        case '\t': puts_(b, "\\t"); break;
        default:
            if (*p < 0x20) { snprintf(esc, sizeof esc, "\\u%04x", *p); puts_(b, esc); }
            else put(b, (const char *)p, 1);
        }
    }
    puts_(b, "\"");
}

/* JSON policy: numbers and booleans from typed columns are emitted bare, everything else as a
 * string; SQL NULL is JSON null. Big numbers stay exact because they are copied as text. */
static void put_json_value(const Grid *g, int src, const char *v, Buf *b) {
    if (!v) { puts_(b, "null"); return; }
    int kind = g->rs->kinds[src];
    double d;
    /* JSON has no NaN/Infinity: those (which strtod accepts) and hex/space forms go out as strings. */
    if ((kind == CK_NUMBER || (kind == CK_OTHER && g->numeric[src])) && parse_num(v, &d) && isfinite(d)
        && !strchr(v, ' ') && !strchr(v, ',') && !strpbrk(v, "xXnNiI")) { puts_(b, v); return; }
    if (kind == CK_BOOL && (v[0] == 't' || v[0] == 'f') && !v[1]) { puts_(b, v[0] == 't' ? "true" : "false"); return; }
    if (kind == CK_BOOL && (strcmp(v, "true") == 0 || strcmp(v, "false") == 0)) { puts_(b, v); return; }
    put_json_str(b, v);
}

/* Binary cells arrive as hex text ("\\x00ff" from SQLite / PostgreSQL, bare hex from an Oracle
 * RAW). A quoted string would re-insert as text, so the literal follows the source dialect:
 * X'00ff' (SQLite and the default), '\\x00ff'::bytea (PostgreSQL), HEXTORAW('00ff') (Oracle).
 * A value the driver did not load ("<blob N bytes>", "<lob>") is written as NULL with a
 * comment saying so, never as made-up bytes. */
static void put_sql_binary(const Grid *g, const char *v, Buf *b) {
    const char *hex = v;
    bool prefixed = hex[0] == '\\' && hex[1] == 'x';
    if (prefixed) hex += 2;
    size_t n = strlen(hex);
    bool ok = n % 2 == 0 && (n > 0 || prefixed);
    for (size_t i = 0; ok && i < n; i++) ok = isxdigit((unsigned char)hex[i]) != 0;
    if (!ok) {
        puts_(b, "NULL /* ");
        for (const char *p = v; *p && p - v < 80; p++) put(b, *p == '*' || *p == '/' || *p == '\n' ? "_" : p, 1);
        puts_(b, " not loaded */");
        return;
    }
    const char *dn = g->rs->db && g->rs->db->drv && g->rs->db->drv->name ? g->rs->db->drv->name : "";
    if (strcmp(dn, "PostgreSQL") == 0) { puts_(b, "'\\x"); put(b, hex, n); puts_(b, "'::bytea"); }
    else if (strcmp(dn, "Oracle") == 0) { puts_(b, "HEXTORAW('"); put(b, hex, n); puts_(b, "')"); }
    else { puts_(b, "X'"); put(b, hex, n); puts_(b, "'"); }
}

/* A column name goes out bare when it is a plain identifier, double-quoted otherwise (spaces,
 * punctuation, a leading digit or a common reserved word), with inner quotes doubled. */
static void put_sql_ident(Buf *b, const char *name) {
    static const char *const reserved[] = { "all", "and", "as", "by", "check", "column", "date", "default", "desc", "from",
        "group", "having", "in", "index", "is", "key", "level", "like", "limit", "not", "null", "number", "or", "order",
        "primary", "references", "select", "size", "table", "to", "user", "values", "where", NULL };
    bool plain = (isalpha((unsigned char)name[0]) || name[0] == '_');
    for (const char *p = name; plain && *p; p++) plain = isalnum((unsigned char)*p) || *p == '_';
    for (int i = 0; plain && reserved[i]; i++) if (strcasecmp(name, reserved[i]) == 0) plain = false;
    if (plain) { puts_(b, name); return; }
    puts_(b, "\"");
    for (const char *p = name; *p; p++) { if (*p == '"') puts_(b, "\""); put(b, p, 1); }
    puts_(b, "\"");
}

/* Writes v as a SQL literal for an INSERT: NULL, a dialect-specific binary literal, a bare
 * number for numeric columns, TRUE / FALSE for 't' / 'f' booleans, else a quoted string with
 * quotes doubled. */
static void put_sql_value(const Grid *g, int src, const char *v, Buf *b) {
    if (!v) { puts_(b, "NULL"); return; }
    if (g->rs->kinds[src] == CK_BINARY) { put_sql_binary(g, v, b); return; }
    double d;
    if (g->numeric[src] && parse_num(v, &d) && !strchr(v, ' ') && !strchr(v, ',')) { puts_(b, v); return; }
    if (g->rs->kinds[src] == CK_BOOL && (v[0] == 't' || v[0] == 'f') && !v[1]) { puts_(b, v[0] == 't' ? "TRUE" : "FALSE"); return; }
    puts_(b, "'");
    for (const char *p = v; *p; p++) { if (*p == '\'') puts_(b, "'"); put(b, p, 1); }
    puts_(b, "'");
}

/* Writes a Markdown table cell: pipes escaped, line breaks become spaces. NULL writes nothing. */
static void put_md(Buf *b, const char *v) {
    if (!v) return;
    for (const char *p = v; *p; p++) {
        if (*p == '|') puts_(b, "\\|");
        else if (*p == '\n' || *p == '\r') puts_(b, " ");
        else put(b, p, 1);
    }
}

/* Writes the header (when the format has one) for display columns c0..c1. */
static void serialise_header(const Grid *g, Buf *b, const ExportOpts *o, int c0, int c1) {
    ResultSet *rs = g->rs;
    int fmt = o->fmt;
    if (fmt == FMT_MARKDOWN) {
        puts_(b, "|");
        for (int c = c0; c <= c1; c++) { puts_(b, " "); put_md(b, rs->cols[g->cols[c]]); puts_(b, " |"); }
        put_nl(b, o);
        puts_(b, "|");
        for (int c = c0; c <= c1; c++) puts_(b, g->numeric[g->cols[c]] ? " ---: |" : " --- |");
        put_nl(b, o);
    } else if (o->headers && (fmt == FMT_TSV || fmt == FMT_CSV)) {
        for (int c = c0; c <= c1; c++) {
            if (c > c0) put(b, fmt == FMT_CSV ? &o->sep : "\t", 1);
            if (fmt == FMT_CSV) put_csv(b, rs->cols[g->cols[c]], o, false); else put_tsv(b, rs->cols[g->cols[c]], o, false);
        }
        put_nl(b, o);
    }
}

/* Writes rows (result row indices) for display columns c0..c1. JSON rows are written as
 * array items separated by commas; `first` says whether a comma must precede the first one. */
static void serialise_rows(const Grid *g, Buf *b, const ExportOpts *o, const int *rows, int n, int c0, int c1, bool first) {
    ResultSet *rs = g->rs;
    int fmt = o->fmt;
    char tmp[64];
    for (int i = 0; i < n; i++) {
        int row = rows[i];
        /* One line per row for CSV / TSV, one object per row for JSON (keys = column names), a table
         * row for Markdown, one INSERT statement per row for SQL. FMT_XLSX has no case here. */
        switch (fmt) {
        case FMT_TSV: case FMT_CSV:
            for (int c = c0; c <= c1; c++) {
                int src = g->cols[c];
                if (c > c0) put(b, fmt == FMT_CSV ? &o->sep : "\t", 1);
                const char *v = rs_cell(rs, row, src);
                if (o->displayed) v = grid_format_value(g, src, v, tmp, sizeof tmp);
                if (fmt == FMT_CSV) put_csv(b, v, o, g->numeric[src]); else put_tsv(b, v, o, g->numeric[src]);
            }
            put_nl(b, o);
            break;
        case FMT_JSON:
            if (!first || i) { puts_(b, ","); put_nl(b, o); }
            puts_(b, "  {");
            for (int c = c0; c <= c1; c++) {
                int src = g->cols[c];
                if (c > c0) puts_(b, ", ");
                put_json_str(b, rs->cols[src]); puts_(b, ": ");
                const char *v = rs_cell(rs, row, src);
                if (o->displayed) v = grid_format_value(g, src, v, tmp, sizeof tmp);
                put_json_value(g, src, v, b);
            }
            puts_(b, "}");
            break;
        case FMT_MARKDOWN:
            puts_(b, "|");
            for (int c = c0; c <= c1; c++) {
                int src = g->cols[c];
                const char *v = rs_cell(rs, row, src);
                if (o->displayed) v = grid_format_value(g, src, v, tmp, sizeof tmp);
                puts_(b, " "); put_md(b, v ? v : "NULL"); puts_(b, " |");
            }
            put_nl(b, o);
            break;
        case FMT_SQL:
            puts_(b, "INSERT INTO "); puts_(b, o->table && *o->table ? o->table : "table_name"); puts_(b, " (");
            for (int c = c0; c <= c1; c++) { if (c > c0) puts_(b, ", "); put_sql_ident(b, rs->cols[g->cols[c]]); }
            puts_(b, ") VALUES (");
            for (int c = c0; c <= c1; c++) { if (c > c0) puts_(b, ", "); put_sql_value(g, g->cols[c], rs_cell(rs, row, g->cols[c]), b); }
            puts_(b, ");");
            put_nl(b, o);
            break;
        }
    }
}

/* Copy defaults: ',' separator, LF, NULL as "NULL" in TSV and empty in CSV, no guard. */
static ExportOpts default_opts(int fmt, bool headers, const char *table) {
    ExportOpts o = { 0 };
    o.fmt = fmt; o.headers = headers; o.sep = ','; o.null_as = fmt == FMT_TSV ? NULL_WORD : NULL_EMPTY;
    o.table = table;
    return o;
}

/* See grid.h. Serialises the selection (or every visible row if none) with full options.
 * Returns a malloc'd string the caller frees; never NULL. */
char *grid_copy_opts(const Grid *g, const ExportOpts *o) {
    int r0, c0, r1, c1;
    if (!grid_sel_rect(g, &r0, &c0, &r1, &c1)) { r0 = 0; c0 = 0; r1 = g->nvis - 1; c1 = g->ncols - 1; }
    Buf b = { NULL, 0, 0 };
    put(&b, "", 0);
    if (r1 >= r0 && c1 >= c0) {
        if (o->fmt == FMT_JSON) { puts_(&b, "["); put_nl(&b, o); }
        serialise_header(g, &b, o, c0, c1);
        serialise_rows(g, &b, o, g->order + r0, r1 - r0 + 1, c0, c1, true);
        if (o->fmt == FMT_JSON) { put_nl(&b, o); puts_(&b, "]"); put_nl(&b, o); }
    }
    /* a single cell copies without the trailing newline so it pastes inline */
    if (r0 == r1 && c0 == c1 && (o->fmt == FMT_TSV || o->fmt == FMT_CSV)) {
        while (b.len && (b.s[b.len - 1] == '\n' || b.s[b.len - 1] == '\r')) b.s[--b.len] = 0;
    }
    return b.s;
}

/* See grid.h: grid_copy_opts with default options for fmt. */
char *grid_copy(const Grid *g, int fmt, bool headers, const char *table) {
    ExportOpts o = default_opts(fmt, headers, table);
    return grid_copy_opts(g, &o);
}

/* Rows serialised per write in file exports. */
#define EXPORT_BLOCK 2000

/* Files are written as "<path>.part" and renamed at the end (US-047): a failed or cancelled
 * export never leaves a file that looks complete. Every block is flushed and checked so a
 * full disk surfaces as the OS error, not as a silent short file. */
/* Opens "<path>.part" for writing (its name goes into tmp); fills err and returns NULL on failure. */
static FILE *out_open(const char *path, char *tmp, size_t n, char *err, size_t errn) {
    snprintf(tmp, n, "%s.part", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) snprintf(err, errn, "cannot create %s: %s", tmp, strerror(errno));
    return f;
}
/* Writes and empties the buffer, adding to *bytes; checks fwrite / fflush / ferror. */
static bool out_flush(FILE *f, Buf *b, unsigned long long *bytes, char *err, size_t errn) {
    bool ok = true;
    if (b->len) { ok = fwrite(b->s, 1, b->len, f) == b->len; if (ok) *bytes += b->len; }
    b->len = 0; b->s[0] = 0;
    if (!ok || fflush(f) != 0 || ferror(f)) { snprintf(err, errn, "write failed: %s", strerror(errno ? errno : EIO)); return false; }
    return true;
}
/* Closes the file and renames .part over path when keep is true and everything succeeded;
 * otherwise removes the .part file. Returns true only when the final file is in place. */
static bool out_close(FILE *f, const char *tmp, const char *path, bool keep, char *err, size_t errn) {
    bool ok = fclose(f) == 0;
    if (!ok && keep && !err[0]) snprintf(err, errn, "write failed on close: %s", strerror(errno));
    if (!keep || !ok) { remove(tmp); return false; }
    if (rename(tmp, path) != 0) { snprintf(err, errn, "cannot replace %s: %s", path, strerror(errno)); remove(tmp); return false; }
    return true;
}

/* Display names of EXPORT_* statuses, for messages. */
const char *grid_export_status_name(int status) {
    static const char *const names[] = { "ok", "row limit", "size limit", "time limit", "cancelled", "cannot open", "write error", "fetch error" };
    return status >= 0 && status < EXPORT_STATUS_COUNT ? names[status] : "?";
}

/* See grid.h. Writes the selection (scope 0, or all visible rows if none is selected) or every
 * visible row (scope 1) in display order, block by block. Not for FMT_XLSX (see
 * grid_export_xlsx). Returns rows written or -1 with err set. */
long grid_export(const Grid *g, const char *path, const ExportOpts *o, int scope, char *err, size_t errn) {
    int r0 = 0, c0 = 0, r1 = g->nvis - 1, c1 = g->ncols - 1;
    if (scope == 0 && !grid_sel_rect(g, &r0, &c0, &r1, &c1)) { r0 = 0; c0 = 0; r1 = g->nvis - 1; c1 = g->ncols - 1; }
    char tmp[1100];
    err[0] = 0;
    FILE *f = out_open(path, tmp, sizeof tmp, err, errn);
    if (!f) return -1;
    long rows = 0;
    unsigned long long bytes = 0;
    bool ok = true;
    Buf b = { NULL, 0, 0 };
    put(&b, "", 0);
    if (o->fmt == FMT_JSON) { puts_(&b, "["); put_nl(&b, o); }
    serialise_header(g, &b, o, c0, c1);
    /* stream in blocks of rows so a large export never materialises as one string */
    for (int i = r0; i <= r1 && ok; i += EXPORT_BLOCK) {
        int n = r1 - i + 1 < EXPORT_BLOCK ? r1 - i + 1 : EXPORT_BLOCK;
        serialise_rows(g, &b, o, g->order + i, n, c0, c1, i == r0);
        ok = out_flush(f, &b, &bytes, err, errn);
        rows += n;
    }
    if (ok) {
        if (o->fmt == FMT_JSON) { put_nl(&b, o); puts_(&b, "]"); put_nl(&b, o); }
        ok = out_flush(f, &b, &bytes, err, errn);
    }
    free(b.s);
    if (!out_close(f, tmp, path, ok, err, errn)) return -1;
    return rows;
}

/* Monotonic clock in seconds, for export time limits. */
static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

/* Hard caps on a full export (see grid.h); tests may lower them. */
static double hard_max_secs = EXPORT_HARD_MAX_SECS;
static unsigned long long hard_max_bytes = EXPORT_HARD_MAX_BYTES;
/* See grid.h: tests only; 0 or less restores the default cap. */
void grid_export_set_hard_caps(double secs, unsigned long long bytes) {
    hard_max_secs = secs > 0 ? secs : EXPORT_HARD_MAX_SECS;
    hard_max_bytes = bytes > 0 ? bytes : EXPORT_HARD_MAX_BYTES;
}

/* See grid.h. Exports every row (source order, no local filter / sort) of the loaded set and
 * then of the open cursor, all display columns, checking limits after each block. */
long grid_export_full(Grid *g, const char *path, const ExportOpts *o, long max_rows,
                      int (*progress)(void *ud, const ExportProgress *p), void *ud, ExportReport *rep) {
    ResultSet *rs = g->rs;
    int c0 = 0, c1 = g->ncols - 1;
    /* Setup: open the .part file and write the header. */
    memset(rep, 0, sizeof *rep);
    char tmp[1100];
    FILE *f = out_open(path, tmp, sizeof tmp, rep->err, sizeof rep->err);
    if (!f) { rep->status = EXPORT_ERR_OPEN; return -1; }
    double t0 = mono_s();
    long rows = 0;
    int status = EXPORT_GO;
    bool first = true, ok = true;
    ExportProgress pr = { 0, 0, rs->cursor ? -1 : rs->nrows, 0 };
    Buf b = { NULL, 0, 0 };
    put(&b, "", 0);
    if (o->fmt == FMT_JSON) { puts_(&b, "["); put_nl(&b, o); }
    serialise_header(g, &b, o, c0, c1);
    int *idx = malloc(sizeof *idx * EXPORT_BLOCK);
/* After each block: report progress to the caller and check the caller's stop request, the
 * option limits, the hard caps and the row limit, in that order. */
#define REPORT_BLOCK() do { \
        pr.rows = rows; pr.bytes = rep->bytes; pr.secs = mono_s() - t0; \
        if (progress) { int r_ = progress(ud, &pr); if (r_ != EXPORT_GO) status = r_; } \
        if (status == EXPORT_GO && o->max_secs > 0 && pr.secs >= o->max_secs) status = EXPORT_STOP_TIME; \
        if (status == EXPORT_GO && o->max_mb > 0 && rep->bytes >= (unsigned long long)o->max_mb << 20) status = EXPORT_STOP_BYTES; \
        if (status == EXPORT_GO && pr.secs >= hard_max_secs) { status = EXPORT_STOP_TIME; rep->hard_cap = true; } \
        if (status == EXPORT_GO && rep->bytes >= hard_max_bytes) { status = EXPORT_STOP_BYTES; rep->hard_cap = true; } \
        if (status == EXPORT_GO && max_rows > 0 && rows >= max_rows) status = EXPORT_STOP_ROWS; \
    } while (0)
    /* loaded rows first, in source order */
    for (int i = 0; i < rs->nrows && status == EXPORT_GO && ok; i += EXPORT_BLOCK) {
        int n = rs->nrows - i < EXPORT_BLOCK ? rs->nrows - i : EXPORT_BLOCK;
        if (max_rows > 0 && rows + n > max_rows) n = (int)(max_rows - rows);
        for (int k = 0; k < n; k++) idx[k] = i + k;
        serialise_rows(g, &b, o, idx, n, c0, c1, first);
        first = first && n == 0;
        ok = out_flush(f, &b, &rep->bytes, rep->err, sizeof rep->err);
        rows += n;
        REPORT_BLOCK();
    }
    /* then the rest of the cursor, block by block, dropping each block after writing it */
    int base = rs->nrows;
    size_t arena_base = rs->arena_len;
    long streamed = 0;
    while (ok && status == EXPORT_GO && rs->cursor) {
        int want = EXPORT_BLOCK;
        if (max_rows > 0 && rows + want > max_rows) want = (int)(max_rows - rows);
        bool more = rs_fetch_more(rs, want);
        int n = rs->nrows - base;
        if (n > 0) {
            for (int k = 0; k < n; k++) idx[k] = base + k;
            serialise_rows(g, &b, o, idx, n, c0, c1, first);
            first = first && n == 0;
            ok = out_flush(f, &b, &rep->bytes, rep->err, sizeof rep->err);
            rows += n;
            rs->nrows = base; rs->arena_len = arena_base;      /* drop the block: memory stays flat */
            streamed += n;
        }
        if (!rs->cursor && !rs->complete) {
            /* the driver stopped before the end: connection lost or statement error */
            ok = false;
            snprintf(rep->err, sizeof rep->err, "fetch stopped after %ld rows: %s", rows, rs->error[0] ? rs->error : "the cursor ended before the result was complete (connection lost?)");
            for (char *q = rep->err; *q; q++) if (*q == '\n' || *q == '\t' || *q == '\r') *q = ' ';
            rep->status = EXPORT_ERR_FETCH;
            break;
        }
        if (n > 0) REPORT_BLOCK();
        if (!more && !rs->cursor) break;
    }
#undef REPORT_BLOCK
    free(idx);
    /* Wrap up: mark the set partial if rows were streamed away, close the JSON array, keep or
     * remove the file and fill the report. */
    if (!ok && rep->status == 0) rep->status = EXPORT_ERR_WRITE;
    /* rows streamed away are not in memory any more: the loaded set is partial, and a cursor
     * that was stopped in the middle cannot continue without a gap, so it is released */
    if (streamed > 0 || (!rs->cursor && rs->nrows == base && rows > base)) {
        if (rs->cursor) { rs_release(rs); rep->cursor_released = true; }
        rs->complete = false; rs->truncated = true; rs->cursor_lost = true;
    }
    bool cancelled = status == EXPORT_CANCELLED;
    if (ok && !cancelled) {
        if (o->fmt == FMT_JSON) { put_nl(&b, o); puts_(&b, "]"); put_nl(&b, o); }
        ok = out_flush(f, &b, &rep->bytes, rep->err, sizeof rep->err);
        if (!ok) rep->status = EXPORT_ERR_WRITE;
    }
    free(b.s);
    rep->rows = rows;
    rep->secs = mono_s() - t0;
    bool kept = out_close(f, tmp, path, ok && !cancelled, rep->err, sizeof rep->err);
    if (cancelled) { rep->status = EXPORT_CANCELLED; return -2; }
    if (!ok || !kept) { if (rep->status == 0) rep->status = EXPORT_ERR_WRITE; return -1; }
    rep->status = status;
    return rows;
}

/* ---- XLSX (US-026) ----------------------------------------------------- */

/* CRC-32 (IEEE) table for ZIP entries, built on first use. */
static uint32_t crc_table[256];
/* Fills crc_table once (reflected polynomial 0xEDB88320). */
static void crc_init(void) {
    if (crc_table[1]) return;
    for (uint32_t i = 0; i < 256; i++) { uint32_t c = i; for (int k = 0; k < 8; k++) c = c & 1 ? 0xEDB88320u ^ (c >> 1) : c >> 1; crc_table[i] = c; }
}
/* Continues a CRC-32 over n more bytes (start with 0). */
static uint32_t crc32_update(uint32_t crc, const unsigned char *p, size_t n) {
    crc = ~crc;
    for (size_t i = 0; i < n; i++) crc = crc_table[(crc ^ p[i]) & 0xFF] ^ (crc >> 8);
    return ~crc;
}

/* One ZIP entry: name, local header offset, CRC and size (stored, so compressed = raw size).
 * Zip: the output file and up to 8 entries; failed records a short write. */
typedef struct { const char *name; long offset; uint32_t crc, size; } ZipEntry;
typedef struct { FILE *f; ZipEntry entries[8]; int n; long pos; bool failed; } Zip;

/* Little-endian writers for ZIP headers. */
static void put_le16(FILE *f, unsigned v) { fputc(v & 0xFF, f); fputc((v >> 8) & 0xFF, f); }
/* 32-bit variant. */
static void put_le32(FILE *f, uint32_t v) { for (int i = 0; i < 4; i++) fputc((v >> (8 * i)) & 0xFF, f); }

/* begins a stored entry; the caller streams bytes with zip_write and ends with zip_end */
static void zip_begin(Zip *z, const char *name) {
    ZipEntry *e = &z->entries[z->n];
    e->name = name; e->offset = ftell(z->f); e->crc = 0; e->size = 0;
    size_t nl = strlen(name);
    put_le32(z->f, 0x04034b50); put_le16(z->f, 20); put_le16(z->f, 0x0808);   /* data descriptor follows */
    put_le16(z->f, 0); put_le16(z->f, 0); put_le16(z->f, 0);
    put_le32(z->f, 0); put_le32(z->f, 0); put_le32(z->f, 0);
    put_le16(z->f, (unsigned)nl); put_le16(z->f, 0);
    fwrite(name, 1, nl, z->f);
}
/* Writes entry bytes, updating its CRC and size. */
static void zip_write(Zip *z, const char *s, size_t n) {
    ZipEntry *e = &z->entries[z->n];
    e->crc = crc32_update(e->crc, (const unsigned char *)s, n);
    e->size += (uint32_t)n;
    if (fwrite(s, 1, n, z->f) != n) z->failed = true;
}
/* Writes a NUL-terminated string to the current entry. */
static void zip_puts(Zip *z, const char *s) { zip_write(z, s, strlen(s)); }
/* Ends the current entry with its data descriptor (CRC and sizes). */
static void zip_end(Zip *z) {
    ZipEntry *e = &z->entries[z->n++];
    put_le32(z->f, 0x08074b50); put_le32(z->f, e->crc); put_le32(z->f, e->size); put_le32(z->f, e->size);
}
/* Writes the central directory and end-of-central-directory record. No ZIP64: sizes and
 * offsets are 32-bit. */
static void zip_finish(Zip *z) {
    long cd = ftell(z->f);
    for (int i = 0; i < z->n; i++) {
        ZipEntry *e = &z->entries[i];
        size_t nl = strlen(e->name);
        put_le32(z->f, 0x02014b50); put_le16(z->f, 20); put_le16(z->f, 20); put_le16(z->f, 0x0808); put_le16(z->f, 0);
        put_le16(z->f, 0); put_le16(z->f, 0);
        put_le32(z->f, e->crc); put_le32(z->f, e->size); put_le32(z->f, e->size);
        put_le16(z->f, (unsigned)nl); put_le16(z->f, 0); put_le16(z->f, 0); put_le16(z->f, 0); put_le16(z->f, 0);
        put_le32(z->f, 0); put_le32(z->f, (uint32_t)e->offset);
        fwrite(e->name, 1, nl, z->f);
    }
    long end = ftell(z->f);
    put_le32(z->f, 0x06054b50); put_le16(z->f, 0); put_le16(z->f, 0);
    put_le16(z->f, (unsigned)z->n); put_le16(z->f, (unsigned)z->n);
    put_le32(z->f, (uint32_t)(end - cd)); put_le32(z->f, (uint32_t)cd); put_le16(z->f, 0);
}

/* Writes v XML-escaped into the current entry, dropping control characters XML 1.0 forbids. */
static void xml_escape(Zip *z, const char *v) {
    for (const unsigned char *p = (const unsigned char *)v; *p; p++) {
        switch (*p) {
        case '&': zip_puts(z, "&amp;"); break;
        case '<': zip_puts(z, "&lt;"); break;
        case '>': zip_puts(z, "&gt;"); break;
        case '"': zip_puts(z, "&quot;"); break;
        default:
            if (*p < 0x20 && *p != '\n' && *p != '\t' && *p != '\r') break;   /* not representable in XML 1.0 */
            zip_write(z, (const char *)p, 1);
        }
    }
}

/* Spreadsheet column letters for a 0-based index. */
static void col_letters(int c, char *out) {   /* 0 -> A, 25 -> Z, 26 -> AA */
    char tmp[8]; int n = 0;
    c++;
    while (c > 0) { int r = (c - 1) % 26; tmp[n++] = (char)('A' + r); c = (c - 1) / 26; }
    for (int i = 0; i < n; i++) out[i] = tmp[n - 1 - i];
    out[n] = 0;
}

/* Sheet size limits of the XLSX format. */
#define XLSX_MAX_ROWS 1048576
#define XLSX_MAX_COLS 16384

/* See grid.h. Writes a minimal workbook (content types, relationships, workbook, one sheet)
 * as a stored ZIP through "<path>.part". scope 0 = selection (or all if none), 1 = all visible
 * rows. NULL cells are omitted; with displayed every value is written as formatted text.
 * On success err is either empty or says the export stopped at a format limit. */
long grid_export_xlsx(const Grid *g, const char *path, int scope, bool headers, bool displayed, char *err, size_t errn) {
    int r0 = 0, c0 = 0, r1 = g->nvis - 1, c1 = g->ncols - 1;
    if (scope == 0 && !grid_sel_rect(g, &r0, &c0, &r1, &c1)) { r0 = 0; c0 = 0; r1 = g->nvis - 1; c1 = g->ncols - 1; }
    char part[1100];
    err[0] = 0;
    FILE *f = out_open(path, part, sizeof part, err, errn);
    if (!f) return -1;
    crc_init();
    /* Fixed package parts. */
    Zip z = { f, { { 0 } }, 0, 0, false };
    zip_begin(&z, "[Content_Types].xml");
    zip_puts(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Types xmlns=\"http://schemas.openxmlformats.org/package/2006/content-types\">"
        "<Default Extension=\"rels\" ContentType=\"application/vnd.openxmlformats-package.relationships+xml\"/>"
        "<Default Extension=\"xml\" ContentType=\"application/xml\"/>"
        "<Override PartName=\"/xl/workbook.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.sheet.main+xml\"/>"
        "<Override PartName=\"/xl/worksheets/sheet1.xml\" ContentType=\"application/vnd.openxmlformats-officedocument.spreadsheetml.worksheet+xml\"/>"
        "</Types>");
    zip_end(&z);
    zip_begin(&z, "_rels/.rels");
    zip_puts(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/officeDocument\" Target=\"xl/workbook.xml\"/>"
        "</Relationships>");
    zip_end(&z);
    zip_begin(&z, "xl/workbook.xml");
    zip_puts(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<workbook xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\" xmlns:r=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships\">"
        "<sheets><sheet name=\"Results\" sheetId=\"1\" r:id=\"rId1\"/></sheets></workbook>");
    zip_end(&z);
    zip_begin(&z, "xl/_rels/workbook.xml.rels");
    zip_puts(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<Relationships xmlns=\"http://schemas.openxmlformats.org/package/2006/relationships\">"
        "<Relationship Id=\"rId1\" Type=\"http://schemas.openxmlformats.org/officeDocument/2006/relationships/worksheet\" Target=\"worksheets/sheet1.xml\"/>"
        "</Relationships>");
    zip_end(&z);
    /* The sheet: optional header row, then one row per exported grid row. */
    zip_begin(&z, "xl/worksheets/sheet1.xml");
    zip_puts(&z, "<?xml version=\"1.0\" encoding=\"UTF-8\" standalone=\"yes\"?>"
        "<worksheet xmlns=\"http://schemas.openxmlformats.org/spreadsheetml/2006/main\"><sheetData>");
    long rows = 0, xrow = 1;
    bool cut_cols = c1 - c0 + 1 > XLSX_MAX_COLS, cut_rows = false;
    if (cut_cols) c1 = c0 + XLSX_MAX_COLS - 1;
    char ref[16], letters[8], tmp[64], num[64];
    if (headers) {
        snprintf(num, sizeof num, "<row r=\"%ld\">", xrow); zip_puts(&z, num);
        for (int c = c0; c <= c1; c++) {
            col_letters(c - c0, letters);
            snprintf(ref, sizeof ref, "%s%ld", letters, xrow);
            zip_puts(&z, "<c r=\""); zip_puts(&z, ref); zip_puts(&z, "\" t=\"inlineStr\"><is><t>");
            xml_escape(&z, g->rs->cols[g->cols[c]]);
            zip_puts(&z, "</t></is></c>");
        }
        zip_puts(&z, "</row>");
        xrow++;
    }
    for (int i = r0; i <= r1; i++) {
        if (xrow > XLSX_MAX_ROWS) { cut_rows = true; break; }
        snprintf(num, sizeof num, "<row r=\"%ld\">", xrow); zip_puts(&z, num);
        for (int c = c0; c <= c1; c++) {
            int src = g->cols[c];
            const char *v = rs_cell(g->rs, g->order[i], src);
            if (!v) continue;                              /* NULL: no cell at all */
            if (displayed) v = grid_format_value(g, src, v, tmp, sizeof tmp);
            col_letters(c - c0, letters);
            snprintf(ref, sizeof ref, "%s%ld", letters, xrow);
            double d;
            if (g->numeric[src] && !displayed && parse_num(v, &d) && !strchr(v, ' ') && strlen(v) < 40) {
                zip_puts(&z, "<c r=\""); zip_puts(&z, ref); zip_puts(&z, "\"><v>"); zip_puts(&z, v); zip_puts(&z, "</v></c>");
            } else if (g->rs->kinds[src] == CK_BOOL && (v[0] == 't' || v[0] == 'f') && !v[1]) {
                zip_puts(&z, "<c r=\""); zip_puts(&z, ref); zip_puts(&z, "\" t=\"b\"><v>"); zip_puts(&z, v[0] == 't' ? "1" : "0"); zip_puts(&z, "</v></c>");
            } else {
                /* inline string: never evaluated as a formula, whatever it starts with */
                zip_puts(&z, "<c r=\""); zip_puts(&z, ref); zip_puts(&z, "\" t=\"inlineStr\"><is><t xml:space=\"preserve\">");
                xml_escape(&z, v);
                zip_puts(&z, "</t></is></c>");
            }
        }
        zip_puts(&z, "</row>");
        xrow++; rows++;
    }
    zip_puts(&z, "</sheetData></worksheet>");
    zip_end(&z);
    zip_finish(&z);
    if (z.failed || fflush(f) != 0 || ferror(f)) { snprintf(err, errn, "write failed: %s", strerror(errno ? errno : EIO)); z.failed = true; }
    if (!out_close(f, part, path, !z.failed, err, errn)) return -1;
    if (cut_rows || cut_cols) snprintf(err, errn, "stopped at the XLSX limit (%s)", cut_rows ? "1,048,576 rows" : "16,384 columns");
    else err[0] = 0;
    return rows;
}

/* ---- derived sets (US-038 transposed, US-040 compare) ------------------ */

/* See grid.h. Builds a set with one row per visible column: its name, type, and the values of
 * display rows r0..r1 as columns "row N". out is initialised here; the caller frees it. */
void grid_transpose(const Grid *g, int r0, int r1, ResultSet *out) {
    rs_init(out);
    rs_add_column(out, "column", "", CK_TEXT);
    rs_add_column(out, "type", "", CK_TEXT);
    for (int i = r0; i <= r1; i++) {
        char name[32];
        snprintf(name, sizeof name, "row %d", i + 1);
        rs_add_column(out, name, "", CK_OTHER);
    }
    out->ok = true; out->has_rows = true; out->complete = true;
    for (int c = 0; c < g->ncols; c++) {
        int row = rs_add_row(out);
        if (row < 0) break;
        rs_set(out, row, 0, grid_col_name(g, c), -1);
        rs_set(out, row, 1, grid_col_type(g, c), -1);
        for (int i = r0; i <= r1; i++) rs_set(out, row, 2 + (i - r0), grid_cell(g, i, c), -1);
    }
}

/* Key value of a display row, for sorting and merging in grid_compare. */
typedef struct { const char *key; int row; } KeyRow;
/* Whether both key columns are numeric (context for key_cmp; not reentrant). */
static bool g_key_numeric;
/* Compares two keys: NULLs last (two NULLs are equal), numerically when g_key_numeric and both
 * parse, else byte-wise. */
static int key_cmp(const char *x, const char *y) {
    if (!x || !y) return (x ? 0 : 1) - (y ? 0 : 1);
    if (g_key_numeric) { double dx, dy; if (parse_num(x, &dx) && parse_num(y, &dy)) return dx < dy ? -1 : dx > dy; }
    return strcmp(x, y);
}
/* qsort comparator: by key, then by display row. */
static int cmp_keyrow(const void *pa, const void *pb) {
    const KeyRow *a = pa, *b = pb;
    int c = key_cmp(a->key, b->key);
    return c ? c : a->row - b->row;
}
typedef struct { int kind; int ia, ib; } Pair;   /* kind: 0 changed, 1 only A, 2 only B, 3 same */

/* Writes a compared cell: the value when A and B agree, else "a -> b" (drawn with an arrow; NULL spelled out). */
static void put_diff(ResultSet *out, int row, int col, const char *a, const char *b) {
    if ((!a && !b) || (a && b && strcmp(a, b) == 0)) { rs_set(out, row, col, a, -1); return; }
    char buf[1024];
    snprintf(buf, sizeof buf, "%s \xE2\x86\x92 %s", a ? a : "NULL", b ? b : "NULL");
    rs_set(out, row, col, buf, -1);
}

/* See grid.h. Sorts both visible row sets by their key, merges them, and writes the pairs
 * grouped as changed, only in A, only in B, same. Columns of A are matched to B by name;
 * those missing in B are marked "(A only)" and copied as is. out is initialised here and owned
 * by the caller; st gets the counts. */
void grid_compare(const Grid *a, const Grid *b, int key_a, int key_b, ResultSet *out, CompareStats *st) {
    memset(st, 0, sizeof *st);
    st->keys_unique = true;
    rs_init(out);
    rs_add_column(out, "diff", "", CK_TEXT);
    rs_add_column(out, grid_col_name(a, key_a), grid_col_type(a, key_a), grid_col_kind(a, key_a));
    /* other columns of A matched to B by name */
    int nc = 0;
    int *acols = malloc(sizeof *acols * (a->ncols + 1)), *bcols = malloc(sizeof *bcols * (a->ncols + 1));
    for (int c = 0; c < a->ncols; c++) {
        if (c == key_a) continue;
        int match = -1;
        for (int d = 0; d < b->ncols; d++) if (strcmp(grid_col_name(a, c), grid_col_name(b, d)) == 0) { match = d; break; }
        acols[nc] = c; bcols[nc] = match; nc++;
        char name[256];
        snprintf(name, sizeof name, "%s%s", grid_col_name(a, c), match < 0 ? " (A only)" : "");
        rs_add_column(out, name, grid_col_type(a, c), CK_OTHER);
    }
    out->ok = true; out->has_rows = true; out->complete = true;
    /* Key lists of both sides, sorted; duplicates make keys_unique false. */
    KeyRow *ka = malloc(sizeof *ka * (a->nvis + 1)), *kb = malloc(sizeof *kb * (b->nvis + 1));
    for (int i = 0; i < a->nvis; i++) { ka[i].key = grid_cell(a, i, key_a); ka[i].row = i; }
    for (int i = 0; i < b->nvis; i++) { kb[i].key = grid_cell(b, i, key_b); kb[i].row = i; }
    g_key_numeric = grid_numeric(a, key_a) && grid_numeric(b, key_b);
    qsort(ka, a->nvis, sizeof *ka, cmp_keyrow);
    qsort(kb, b->nvis, sizeof *kb, cmp_keyrow);
    for (int i = 1; i < a->nvis; i++) if (ka[i].key && ka[i - 1].key && key_cmp(ka[i].key, ka[i - 1].key) == 0) st->keys_unique = false;
    for (int i = 1; i < b->nvis; i++) if (kb[i].key && kb[i - 1].key && key_cmp(kb[i].key, kb[i - 1].key) == 0) st->keys_unique = false;
    /* merge into pairs, then emit grouped: changed, only in A, only in B, same */
    Pair *pairs = malloc(sizeof *pairs * (a->nvis + b->nvis + 1));
    int np = 0, i = 0, j = 0;
    while (i < a->nvis || j < b->nvis) {
        int cmp;
        if (i >= a->nvis) cmp = 1; else if (j >= b->nvis) cmp = -1;
        else cmp = key_cmp(ka[i].key, kb[j].key);
        if (cmp == 0) {
            bool changed = false;
            for (int k = 0; k < nc && !changed; k++) {
                if (bcols[k] < 0) continue;
                const char *va = grid_cell(a, ka[i].row, acols[k]), *vb = grid_cell(b, kb[j].row, bcols[k]);
                if ((va && !vb) || (!va && vb) || (va && vb && strcmp(va, vb) != 0)) changed = true;
            }
            pairs[np].kind = changed ? 0 : 3; pairs[np].ia = ka[i].row; pairs[np].ib = kb[j].row; np++;
            if (changed) st->changed++; else st->same++;
            i++; j++;
        } else if (cmp < 0) { pairs[np].kind = 1; pairs[np].ia = ka[i].row; pairs[np].ib = -1; np++; st->only_a++; i++; }
        else { pairs[np].kind = 2; pairs[np].ia = -1; pairs[np].ib = kb[j].row; np++; st->only_b++; j++; }
    }
    /* Emit grouped by kind: changed, only in A, only in B, same. */
    for (int kind = 0; kind < 4; kind++) for (int q = 0; q < np; q++) {
        if (pairs[q].kind != kind) continue;
        int row = rs_add_row(out);
        if (row < 0) break;
        static const char *const names[] = { "changed", "only in A", "only in B", "same" };
        rs_set(out, row, 0, names[kind], -1);
        rs_set(out, row, 1, pairs[q].ia >= 0 ? grid_cell(a, pairs[q].ia, key_a) : grid_cell(b, pairs[q].ib, key_b), -1);
        for (int k = 0; k < nc; k++) {
            const char *va = pairs[q].ia >= 0 ? grid_cell(a, pairs[q].ia, acols[k]) : NULL;
            const char *vb = pairs[q].ib >= 0 && bcols[k] >= 0 ? grid_cell(b, pairs[q].ib, bcols[k]) : NULL;
            if (kind == 1 || bcols[k] < 0) rs_set(out, row, 2 + k, va, -1);
            else if (kind == 2) rs_set(out, row, 2 + k, vb, -1);
            else put_diff(out, row, 2 + k, va, vb);
        }
    }
    free(pairs);
    free(ka); free(kb); free(acols); free(bcols);
}
