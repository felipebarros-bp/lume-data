/* SPDX-License-Identifier: AGPL-3.0-only */
/* rs.c - ResultSet storage (see rs.h).
 *
 * A ResultSet is the driver-independent result model the grid (grid.c) and the exporters
 * read from. Drivers (db_*.c) build it with rs_add_column / rs_add_row / rs_set while
 * executing; the UI reads cells with rs_cell. Cell texts are appended to one growing arena
 * and each cell stores "arena offset + 1" (0 means SQL NULL), so a million-cell result is a
 * handful of allocations instead of a million.
 *
 * Cursor ownership: when a driver leaves a server cursor open for "fetch more", rs->db and
 * rs->cursor point at it; rs_fetch_more / rs_release / rs_move forward to the driver's
 * fetch / release / rehome callbacks (rehome lets the driver update its back-pointer when
 * the set is moved to a new address).
 *
 * Threading: no locking. A set is filled on the query worker and then moved to the UI
 * thread; callers guarantee only one thread touches it at a time. */
#include "rs.h"
#include "db.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* Heap copy of a NUL-terminated string (no NULL check on malloc). */
static char *dup(const char *s) { size_t n = strlen(s) + 1; char *d = malloc(n); memcpy(d, s, n); return d; }

/* See rs.h. */
void rs_init(ResultSet *rs) { memset(rs, 0, sizeof *rs); rs->err_code = -1; rs->err_offset = -1; }

/* Lets the driver close its cursor (if any) and forgets it; the rows stay. */
void rs_release(ResultSet *rs) {
    if (rs->cursor && rs->db) rs->db->drv->release(rs->db, rs);
    rs->cursor = NULL;
}

/* Releases the cursor and every owned buffer, then reinitialises rs like rs_init. */
void rs_free(ResultSet *rs) {
    rs_release(rs);
    for (int i = 0; i < rs->ncols; i++) { free(rs->cols[i]); free(rs->types[i]); }
    free(rs->cols); free(rs->types); free(rs->kinds);
    free(rs->cells);
    free(rs->arena);
    free(rs->notices);
    memset(rs, 0, sizeof *rs);
    rs->err_code = -1; rs->err_offset = -1;
}

/* Shallow-moves all fields to dst and zeroes src; tells the driver about the new address
 * when a cursor is still open. dst's previous contents are overwritten, not freed. */
void rs_move(ResultSet *dst, ResultSet *src) {
    *dst = *src;
    memset(src, 0, sizeof *src);
    if (dst->cursor && dst->db && dst->db->drv->rehome) dst->db->drv->rehome(dst->db, dst);
}

/* Grows the three column arrays by one and stores copies of name/type. */
void rs_add_column(ResultSet *rs, const char *name, const char *type, int kind) {
    int n = rs->ncols + 1;
    rs->cols = realloc(rs->cols, sizeof *rs->cols * n);
    rs->types = realloc(rs->types, sizeof *rs->types * n);
    rs->kinds = realloc(rs->kinds, sizeof *rs->kinds * n);
    rs->cols[rs->ncols] = dup(name ? name : "");
    rs->types[rs->ncols] = dup(type ? type : "");
    rs->kinds[rs->ncols] = (uint8_t)kind;
    rs->ncols = n;
}

/* Appends an all-NULL row, doubling the cell table (starting at 256 rows) when full.
 * Returns the new row index, or -1 at RS_ROW_CAP. */
int rs_add_row(ResultSet *rs) {
    if (rs->nrows >= RS_ROW_CAP) return -1;
    if (rs->nrows == rs->cap_rows) {
        rs->cap_rows = rs->cap_rows ? rs->cap_rows * 2 : 256;
        rs->cells = realloc(rs->cells, sizeof *rs->cells * (size_t)rs->cap_rows * (rs->ncols ? rs->ncols : 1));
    }
    memset(rs->cells + (size_t)rs->nrows * rs->ncols, 0, sizeof *rs->cells * rs->ncols);
    return rs->nrows++;
}

/* Copies the text plus a terminator to the arena end (arena doubles from 64 KiB) and records
 * its offset + 1 in the cell; a NULL text marks the cell as SQL NULL. Does not bounds-check
 * row/col. */
void rs_set(ResultSet *rs, int row, int col, const char *text, int len) {
    if (!text) { rs->cells[(size_t)row * rs->ncols + col] = 0; return; }
    if (len < 0) len = (int)strlen(text);
    if (rs->arena_len + len + 1 > rs->arena_cap) {
        size_t want = rs->arena_cap ? rs->arena_cap * 2 : 64 * 1024;
        while (want < rs->arena_len + len + 1) want *= 2;
        rs->arena = realloc(rs->arena, want);
        rs->arena_cap = want;
    }
    memcpy(rs->arena + rs->arena_len, text, len);
    rs->arena[rs->arena_len + len] = 0;
    rs->cells[(size_t)row * rs->ncols + col] = (uint32_t)rs->arena_len + 1;
    rs->arena_len += len + 1;
}

/* Translates the stored offset back to a pointer; NULL for SQL NULL. */
const char *rs_cell(const ResultSet *rs, int row, int col) {
    uint32_t off = rs->cells[(size_t)row * rs->ncols + col];
    return off ? rs->arena + off - 1 : NULL;
}

/* Lower-cases the first 63 characters of the type name and checks keyword families in
 * priority order: numeric, boolean, date/time, JSON, binary, text. */
int rs_kind_from_type(const char *type) {
    if (!type || !*type) return CK_OTHER;
    char t[64];
    size_t i = 0;
    for (; type[i] && i < sizeof t - 1; i++) t[i] = (char)tolower((unsigned char)type[i]);
    t[i] = 0;
    if (strstr(t, "int") || strstr(t, "numeric") || strstr(t, "decimal") || strstr(t, "number") || strstr(t, "real") ||
        strstr(t, "double") || strstr(t, "float") || strstr(t, "money") || strcmp(t, "oid") == 0 || strstr(t, "serial")) return CK_NUMBER;
    if (strcmp(t, "bool") == 0 || strcmp(t, "boolean") == 0) return CK_BOOL;
    if (strstr(t, "date") || strstr(t, "time")) return CK_DATE;
    if (strstr(t, "json")) return CK_JSON;
    if (strstr(t, "blob") || strstr(t, "bytea") || strstr(t, "binary") || strstr(t, "raw")) return CK_BINARY;
    if (strstr(t, "char") || strstr(t, "text") || strstr(t, "clob") || strstr(t, "string") || strcmp(t, "name") == 0 || strstr(t, "uuid")) return CK_TEXT;
    return CK_OTHER;
}

/* Delegates to the driver's fetch callback. */
bool rs_fetch_more(ResultSet *rs, int n) {
    if (!rs->cursor || !rs->db) return false;
    return rs->db->drv->fetch(rs->db, rs, n);
}

/* Shrinks arena and cell table to their used size; no-op while a cursor is open because
 * more rows may still arrive. Failed reallocs keep the old buffers. */
void rs_trim(ResultSet *rs) {
    /* growth doubled the buffers while fetching; once the set is final, give the slack back */
    if (rs->cursor) return;
    if (rs->arena_cap > rs->arena_len + 1) { char *a = realloc(rs->arena, rs->arena_len ? rs->arena_len : 1); if (a) { rs->arena = a; rs->arena_cap = rs->arena_len ? rs->arena_len : 1; } }
    if (rs->cap_rows > rs->nrows) {
        size_t n = (size_t)(rs->nrows ? rs->nrows : 1) * (rs->ncols ? rs->ncols : 1);
        uint32_t *c = realloc(rs->cells, sizeof *rs->cells * n);
        if (c) { rs->cells = c; rs->cap_rows = rs->nrows ? rs->nrows : 1; }
    }
}

/* See rs.h. */
size_t rs_bytes(const ResultSet *rs) { return (size_t)rs->cap_rows * rs->ncols * sizeof *rs->cells + rs->arena_cap; }
