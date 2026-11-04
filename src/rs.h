/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* ResultSet model: the grid's data, independent of any UI or driver. Cells live in one arena
 * (one allocation per batch, not per cell); the original text of every value is kept as the
 * driver produced it. See docs/result-grid.md.
 *
 * Ownership: a ResultSet owns its column names, type names, cell table, arena and notices;
 * rs_free() releases them all. While a cursor is open (db/cursor set) the driver that
 * filled the set also holds state for it; rs_release()/rs_free() hand it back. Not
 * thread-safe: a set is filled by the query worker and only then handed to the UI (via
 * rs_move), never shared concurrently. */

/* Cell kinds, used for alignment, formatting, sorting and the value viewer. */
enum { CK_TEXT, CK_NUMBER, CK_BOOL, CK_DATE, CK_BINARY, CK_JSON, CK_OTHER };

typedef struct Db Db;

/* One query result: column metadata, rows as offsets into a text arena, and execution status. */
typedef struct ResultSet {
    int ncols;
    char **cols;            /* column names in driver order; duplicates are kept */
    char **types;           /* driver type names ("" when unknown) */
    uint8_t *kinds;         /* CK_* from driver metadata; CK_OTHER = unknown, decide per value */
    int nrows, cap_rows;    /* rows filled / rows allocated in cells */
    uint32_t *cells;        /* nrows * ncols; arena offset + 1, 0 = SQL NULL */
    char *arena; size_t arena_len, arena_cap;   /* NUL-terminated cell texts, back to back */
    bool ok, has_rows;      /* ok: statement succeeded; has_rows: it produced a row set (vs. DML/DDL) */
    bool truncated;         /* the fetch limit stopped the fetch: more rows may exist */
    bool complete;          /* the source is exhausted: nrows is the real total */
    bool cursor_lost;       /* more rows existed but the driver had to close the cursor */
    int affected;           /* rows affected by DML, as the driver reports it */
    double exec_ms, fetch_ms;   /* wall time executing and fetching, in milliseconds */
    char error[512];        /* driver error message; "" on success */
    /* error diagnostics as far as the driver provides them (OUT-012): absent = "" / -1 */
    char err_state[8]; long err_code; char err_detail[256], err_hint[256]; int err_offset;   /* byte offset into the statement, -1 none */
    char *notices;          /* server messages (NOTICE / warnings) during this execution, newline-separated; NULL none */
    bool cancelled;         /* the driver stopped because cancel() was called */
    /* open cursor for fetch-more, owned by the driver that filled this set */
    Db *db; void *cursor;
} ResultSet;

#define RS_ROW_CAP 2000000   /* hard limit of rows held in memory per set */

/* Zeroes rs and marks the diagnostics as absent (err_code / err_offset = -1). */
void rs_init(ResultSet *rs);
void rs_free(ResultSet *rs);                      /* releases the cursor too */
void rs_move(ResultSet *dst, ResultSet *src);     /* moves ownership; src becomes empty */
/* Appends a column (name/type copied; NULL becomes ""). kind is a CK_* value. Must be
 * called before any row is added, since the cell table is laid out by ncols. */
void rs_add_column(ResultSet *rs, const char *name, const char *type, int kind);
int rs_add_row(ResultSet *rs);                    /* row index, or -1 when RS_ROW_CAP was hit */
/* Stores a copy of text (len bytes, or strlen when len < 0) in the arena for one cell. */
void rs_set(ResultSet *rs, int row, int col, const char *text, int len);   /* text NULL = SQL NULL */
/* Appends src's rows to dst (same column count): the arena is copied once and the cell offsets
 * rebased, no per-cell copy. Returns the rows appended (fewer than src->nrows past RS_ROW_CAP). */
int rs_append(ResultSet *dst, const ResultSet *src);
/* Room for `rows` more rows and `bytes` more arena up front (fewer reallocations while a block fills). */
void rs_reserve(ResultSet *rs, int rows, size_t bytes);
/* The cell's NUL-terminated text, owned by the set and valid until the arena next grows
 * (rs_set / fetch-more) or the set is freed. */
const char *rs_cell(const ResultSet *rs, int row, int col);               /* NULL = SQL NULL */
/* Maps a driver type name (case-insensitive substring match) to a CK_* kind; CK_OTHER
 * when empty or unrecognised. */
int rs_kind_from_type(const char *type);          /* heuristic on a type name */
/* Asks the owning driver to append up to n more rows from the open cursor. Returns false at
 * the end of the data or when no cursor is open. Runs on the caller's thread (blocking I/O). */
bool rs_fetch_more(ResultSet *rs, int n);         /* through the cursor; false when none */
void rs_release(ResultSet *rs);                   /* closes the cursor, keeps the rows */
void rs_trim(ResultSet *rs);                      /* releases growth slack once the cursor is closed */
/* Approximate memory footprint in bytes: allocated cell table plus arena capacity
 * (column names and notices are not counted). */
size_t rs_bytes(const ResultSet *rs);             /* memory held by rows + arena */
