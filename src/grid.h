/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "rs.h"

/* Grid view model: everything the result grid does to a ResultSet that is not drawing.
 * Local operations only (RN-002): sorting, filtering and searching act on loaded rows and
 * never touch the database. No SDL, no UI; tests/grid_check.c exercises it headless.
 *
 * Column coordinates: every `col` argument of the public API is a DISPLAY column (after the
 * user hid or reordered columns). Sort keys and filters are stored by source column so they
 * survive layout changes. grid_col() maps display -> source.
 *
 * Ownership: a Grid borrows its ResultSet (keep it alive until grid_free) and owns its index
 * arrays. UI thread only; sorting is not reentrant. */

/* Filter operators (GridFilter.op). Text comparisons are case-insensitive; on numeric
 * columns =, >, <, between compare numbers. */
enum { OP_CONTAINS, OP_EQUALS, OP_STARTS, OP_GT, OP_LT, OP_BETWEEN, OP_IS_NULL, OP_NOT_NULL, OP_COUNT };
/* Selection mode: a cell rectangle, whole rows, or whole columns. */
enum { SEL_CELLS, SEL_ROWS, SEL_COLS };
/* Copy / export formats. */
enum { FMT_TSV, FMT_CSV, FMT_JSON, FMT_MARKDOWN, FMT_SQL, FMT_XLSX, FMT_COUNT };
#define GRID_MAX_SORT 4         /* sort keys at once */
#define GRID_MAX_FILTERS 8      /* filters at once */

/* One filter: source column, OP_* operator, value (and upper bound for OP_BETWEEN). */
typedef struct GridFilter { int col, op; char value[256], value2[256]; } GridFilter;

/* Display formatting (US-018). Never changes stored values: exports use raw text unless the
 * caller asks for displayed values. */
enum { DATE_AS_STORED, DATE_DMY, DATE_MDY, DATE_STYLES };
enum { TZ_AS_STORED, TZ_UTC, TZ_LOCAL, TZ_MODES };
typedef struct GridFormat {
    bool group;          /* thousands separator in numbers */
    int decimals;        /* -1 = as stored, else fixed decimals for numbers with a fraction */
    int date_style;      /* DATE_* for ISO dates / timestamps */
    int tz;              /* TZ_* for timestamps that carry an offset */
} GridFormat;

/* Export / copy options (US-024). */
/* How SQL NULL is written in CSV / TSV: empty field, the word NULL, or \N. */
enum { NULL_EMPTY, NULL_WORD, NULL_BACKSLASH_N };
typedef struct ExportOpts {
    int fmt;             /* FMT_* */
    bool headers;        /* CSV / TSV header line */
    char sep;            /* CSV delimiter (',' ';' '\t' '|') */
    bool quote_all;      /* CSV: quote every field, not only when needed */
    bool crlf;           /* line ending */
    int null_as;         /* NULL_* for CSV / TSV */
    bool displayed;      /* formatted values instead of raw text */
    bool guard;          /* formula-injection guard for CSV / TSV */
    const char *table;   /* SQL INSERT target */
    long max_mb;         /* full export only: stop at this size, keep the partial file (0 = none) */
    double max_secs;     /* full export only: stop after this time, keep the partial file (0 = none) */
} ExportOpts;

/* Grid state over one ResultSet. */
typedef struct Grid {
    ResultSet *rs;                 /* borrowed result set */
    bool *numeric;                 /* per SOURCE column: compare and align as numbers */
    int *order; int nvis;          /* display row -> result row, after filter + sort */
    int *cols; int ncols;          /* display column -> source column (hidden ones omitted) */
    bool *hidden;                  /* per source column */
    int frozen;                    /* leading display columns pinned at the left */
    int sort_col[GRID_MAX_SORT]; bool sort_desc[GRID_MAX_SORT]; int nsort;   /* source columns */
    GridFilter filters[GRID_MAX_FILTERS]; int nfilters;                       /* source columns */
    bool filter_any;               /* OR between filters instead of AND (US-011) */
    char search[128];              /* current search text, "" = none */
    int *hits; int nhits, hit;     /* search matches: (display row, display col) pairs */
    /* selection: cursor cell and anchor, both display coordinates; sel_r < 0 = none */
    int sel_r, sel_c, anc_r, anc_c, sel_mode;
    GridFormat fmt;                /* display formatting options */
    unsigned gen;                  /* bumped whenever rows, order or layout change */
} Grid;

/* Initialises g over rs (borrowed): driver layout, no sort / filter / selection. */
void grid_init(Grid *g, ResultSet *rs);
/* Frees the grid's own arrays (not the ResultSet). */
void grid_free(Grid *g);
void grid_refresh(Grid *g);              /* rows were appended: recompute everything */

/* columns (US-005) */
/* Display column -> source column, and that column's name / type / kind / numeric flag. */
static inline int grid_col(const Grid *g, int col) { return g->cols[col]; }
static inline const char *grid_col_name(const Grid *g, int col) { return g->rs->cols[g->cols[col]]; }
static inline const char *grid_col_type(const Grid *g, int col) { return g->rs->types[g->cols[col]]; }
static inline int grid_col_kind(const Grid *g, int col) { return g->rs->kinds[g->cols[col]]; }
static inline bool grid_numeric(const Grid *g, int col) { return g->numeric[g->cols[col]]; }
/* Hides a display column (the last visible one cannot be hidden). */
void grid_col_hide(Grid *g, int col);
void grid_col_show(Grid *g, int src);    /* source column */
/* Unhides every column; grid_hidden_count returns how many source columns are hidden. */
void grid_col_show_all(Grid *g);
int grid_hidden_count(const Grid *g);
void grid_col_move(Grid *g, int from, int to);   /* display indices; `to` is the new position */
/* Pins the first n display columns at the left (clamped). */
void grid_freeze(Grid *g, int n);
/* Restores the driver layout; grid_layout_default says whether the layout is that one. */
void grid_layout_reset(Grid *g);
bool grid_layout_default(const Grid *g);
/* "order=0,2,1;hidden=3;frozen=1" round trip for persistence */
void grid_layout_save(const Grid *g, char *buf, size_t n);
bool grid_layout_load(Grid *g, const char *s);   /* false (layout unchanged) when s is invalid for this set */

/* sorting */
void grid_sort_click(Grid *g, int col, bool additive);   /* asc -> desc -> none; additive adds a key */
void grid_sort_clear(Grid *g);                            /* back to result order */
int grid_sort_priority(const Grid *g, int col, bool *desc); /* 1-based or 0 */

/* filters */
/* Adds a filter on display column col (values copied) and re-applies; false when full. */
bool grid_filter_add(Grid *g, int col, int op, const char *value, const char *value2);
void grid_filter_mode(Grid *g, bool any);                 /* AND (false) or OR (true) */
void grid_filter_remove(Grid *g, int idx);                /* idx into g->filters */
void grid_filter_clear(Grid *g);
const char *grid_op_name(int op);                         /* static operator text */
/* Chip text for filter idx, e.g. "price between 1 and 5". */
void grid_filter_describe(const Grid *g, int idx, char *buf, size_t n);

/* search */
void grid_search(Grid *g, const char *needle);           /* "" clears */
/* Next (dir >= 0) / previous hit, wrapping; its display cell goes to *row / *col. False: no hits. */
bool grid_search_step(Grid *g, int dir, int *row, int *col);
/* True when the display cell matches the current search. */
bool grid_is_hit(const Grid *g, int row, int col);

/* selection */
/* Selection in display coordinates; extend keeps the anchor (Shift). Row / column variants
 * select whole rows / columns. */
void grid_select_cell(Grid *g, int row, int col, bool extend);
void grid_select_row(Grid *g, int row, bool extend);
void grid_select_col(Grid *g, int col, bool extend);
void grid_select_all(Grid *g);
void grid_select_none(Grid *g);
/* Inclusive selection rectangle in display coordinates; false when nothing is selected. */
bool grid_sel_rect(const Grid *g, int *r0, int *c0, int *r1, int *c1);
bool grid_in_sel(const Grid *g, int row, int col);
void grid_sel_summary(const Grid *g, char *buf, size_t n);  /* "12 cells · sum 42 · …" */

/* value access */
const char *grid_cell(const Grid *g, int row, int col);   /* display row / column; NULL = SQL NULL */
/* Formatted form: booleans as words, numbers and dates per g->fmt. Returns v itself when
 * nothing changes, tmp when it does (tmp should hold 64 bytes). NULL stays NULL. */
const char *grid_display(const Grid *g, int row, int col, char *tmp, size_t n);
/* Same for any value v of source column src_col (used by exports with `displayed`). */
const char *grid_format_value(const Grid *g, int src_col, const char *v, char *tmp, size_t n);
/* Describes what a timestamp carries: "offset +02:00, 3 fractional digits"; false when the
 * value is not an ISO date/time. */
bool grid_describe_datetime(const char *v, char *buf, size_t n);

/* text output: selection when there is one, else all visible rows; malloc'd */
/* grid_copy uses default options for fmt (table_name = INSERT target for FMT_SQL); the caller
 * frees the result. Not for FMT_XLSX. */
char *grid_copy(const Grid *g, int fmt, bool headers, const char *table_name);
char *grid_copy_opts(const Grid *g, const ExportOpts *o);
/* file export (US-023 / US-024): scope 0 = selection, 1 = all visible rows. Streams to the
 * file in blocks through "<path>.part", renamed only when every write succeeded (a failed
 * write removes it and reports the OS error). Returns rows written or -1 with err filled.
 * CSV / TSV guard policy (US-046): with o->guard, a value whose first character after any
 * spaces or control characters is one of = + - @ gets a leading apostrophe (and quotes in
 * CSV) so a spreadsheet reads it as text; numbers in numeric columns are exempt. The guard
 * changes the data (an apostrophe is added) and is therefore a visible option. */
long grid_export(const Grid *g, const char *path, const ExportOpts *o, int scope, char *err, size_t errn);
/* XLSX (US-026): a valid workbook written directly as a zip (stored entries, CRC-32) with
 * inline strings, so no spreadsheet formula is ever created from a cell; numbers from
 * numeric columns are written as numbers. Stops at the format limits (1,048,576 rows,
 * 16,384 columns) and reports it in err. Returns rows written or -1. */
long grid_export_xlsx(const Grid *g, const char *path, int scope, bool headers, bool displayed, char *err, size_t errn);
/* Derived sets (US-038 / US-040): the caller owns the returned ResultSet. */
void grid_transpose(const Grid *g, int r0, int r1, ResultSet *out);          /* display rows r0..r1 */
/* Aligns two grids on one key column each; `out` gets a status column ("same", "changed",
 * "only in A", "only in B"), the key, then every other column of A with "a -> b" when
 * changed. Reports counts and whether keys were unique. */
typedef struct CompareStats { int same, changed, only_a, only_b; bool keys_unique; } CompareStats;
void grid_compare(const Grid *a, const Grid *b, int key_a, int key_b, ResultSet *out, CompareStats *st);
/* Full-result export (US-025 / US-047): writes every loaded row in source order (local sort
 * and filters do not apply), then keeps pulling from the open cursor in blocks that are
 * written and dropped, so memory stays flat. The file is written as "<path>.part" and renamed
 * at the end; a cancelled or failed export leaves nothing behind. progress() is called per
 * block and returns EXPORT_GO or a stop reason (EXPORT_STOP_BYTES, EXPORT_STOP_TIME keep the
 * partial file and say so; EXPORT_CANCELLED removes it). max_rows <= 0 = no row limit. The
 * hard caps below apply whatever the caller asks. Once rows were streamed and dropped the
 * cursor cannot continue (rows in between are gone), so it is released and the set is
 * marked partial; a stop inside the loaded rows keeps it open.
 * Returns rows written (also when stopped), -1 on error, -2 when cancelled; rep says which. */
enum { EXPORT_GO, EXPORT_OK = EXPORT_GO, EXPORT_STOP_ROWS, EXPORT_STOP_BYTES, EXPORT_STOP_TIME, EXPORT_CANCELLED,
       EXPORT_ERR_OPEN, EXPORT_ERR_WRITE, EXPORT_ERR_FETCH, EXPORT_STATUS_COUNT };
#define EXPORT_HARD_MAX_SECS 3600               /* one hour */
#define EXPORT_HARD_MAX_BYTES (16ull << 30)     /* 16 GiB */
/* The size / time limits in ExportOpts and the hard caps are enforced here, after every block,
 * not by the caller's progress callback; a stop by a hard cap sets rep->hard_cap. */
/* Progress passed to the callback after each block (bytes written, elapsed seconds). Report:
 * final status (EXPORT_*), rows / bytes / seconds, whether the cursor was released, whether a
 * hard cap stopped it, and an error message. */
typedef struct ExportProgress { long rows; unsigned long long bytes; long total; double secs; } ExportProgress;   /* total -1 = unknown */
typedef struct ExportReport { int status; long rows; unsigned long long bytes; double secs; bool cursor_released, hard_cap; char err[256]; } ExportReport;
/* Tests only: lowers the hard caps so the production path can be exercised; 0 restores the default. */
void grid_export_set_hard_caps(double secs, unsigned long long bytes);
/* Short static name of an EXPORT_* status for messages. */
const char *grid_export_status_name(int status);
long grid_export_full(Grid *g, const char *path, const ExportOpts *o, long max_rows,
                      int (*progress)(void *ud, const ExportProgress *p), void *ud, ExportReport *rep);
