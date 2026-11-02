/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "db.h"
#include "rs.h"

/* Editing model for table previews (US-032 .. US-035, US-048 .. US-050): pending cell
 * changes, deleted and inserted rows, statement generation and the atomic apply step. No UI
 * and no drawing; results.c owns one EditSet per tab and tests/edit_check.c drives it
 * against SQLite, tests/pg_check.c against PostgreSQL.
 *
 * Rows are addressed by primary key, never by position. The WHERE of every UPDATE / DELETE
 * carries the key plus a snapshot of every comparable column as it was loaded (NULL as IS
 * NULL), so a change by someone else to ANY column (edited or not) makes the statement match
 * nothing; the batch then rolls back and the row is re-read to say what differs. Columns the
 * driver cannot compare exactly (binary, json without equality, floating point on SQLite,
 * Oracle dates) are left out of the snapshot and named in the strategy line. */

/* A pending cell change. value is owned (NULL when is_null); row indexes the ResultSet,
 * so rows >= new_from belong to pending inserts. */
typedef struct CellEdit { int row, col; char *value; bool is_null; } CellEdit;
/* A generated statement: sql is what runs, shown the same text with sensitive values
 * masked, note a human summary ("price: 10 -> 12"); row is the ResultSet row it comes from.
 * All strings are owned by the EditSet. */
typedef struct EditStmt { char *sql, *shown, *note; int kind; int row; } EditStmt;   /* kind: 0 UPDATE 1 DELETE 2 INSERT */

/* Pending changes of one table preview. The ResultSet and `numeric` array are borrowed
 * from the result view and must outlive the EditSet. */
typedef struct EditSet {
    ResultSet *rs;   /* loaded rows; pending inserts are appended to it */
    const DbDriver *drv;   /* for quoting and select_sql */
    char qtable[256];   /* quoted table name used in the statements */
    const bool *numeric;            /* per source column (from the grid) */
    int *pkcols; int npk;   /* primary-key column indexes; npk == 0 disables editing */
    char **readonly_why; int nmeta; /* per column: NULL = editable */
    CellEdit *edits; int nedits, cap_edits;   /* pending cell changes, unordered */
    bool *deleted; int deleted_cap;   /* per loaded row: marked for DELETE */
    int new_from;                   /* rows >= new_from are pending inserts */
    bool *snap; int nsnap;          /* per column: part of the WHERE snapshot */
    char snap_note[240];            /* columns left out and why */
    int version_col;                /* -1 or a column named version / row_version / lock_version */
    EditStmt *stmts; int nstmts;   /* output of edit_build */
} EditSet;

/* Starts an empty EditSet over a loaded ResultSet. `numeric` (may be NULL) flags numeric
 * columns. Call edit_set_key next; free with edit_free. */
void edit_init(EditSet *e, ResultSet *rs, const DbDriver *drv, const char *qtable, const bool *numeric);
/* Sets the primary-key columns (copied) and recomputes the conflict snapshot. */
void edit_set_key(EditSet *e, const int *pkcols, int npk);
/* Marks a column read-only with a reason shown to the user (copied); NULL makes it editable. */
void edit_set_readonly(EditSet *e, int col, const char *why);
/* Frees everything the EditSet owns (not the ResultSet) and zeroes it. */
void edit_free(EditSet *e);
void edit_clear(EditSet *e);                        /* drops every pending change and inserted row */

/* Pending change of a cell, or NULL. The pointer is invalidated by the next put / remove. */
CellEdit *edit_find(EditSet *e, int row, int col);
/* Records (or replaces) a pending change; value is copied. */
void edit_put(EditSet *e, int row, int col, const char *value, bool is_null);
/* Drops the pending change of a cell, if any. */
void edit_remove(EditSet *e, int row, int col);
/* text typed into a cell: a value equal to the stored one drops the pending change (the cell is
 * unmodified again), anything else is a pending edit; '' stays distinct from NULL */
void edit_put_text(EditSet *e, int row, int col, const char *text);
/* True when a loaded row is marked for deletion. */
bool edit_row_deleted(const EditSet *e, int row);
/* True when the row is a pending insert. */
bool edit_row_new(const EditSet *e, int row);
int edit_row_state(const EditSet *e, int row);     /* 0 untouched, 1 edited, 2 deleted, 3 new */
/* value a cell would have after the pending change: *state 0 unchanged, 1 edited, 2 NULL by
 * edit, 3 DEFAULT (new row, untouched) */
const char *edit_pending_value(const EditSet *e, int row, int col, int *state);
int edit_pending_count(const EditSet *e);      /* cells + deleted + new rows */
int edit_pending_rows(const EditSet *e);       /* distinct rows touched */
const char *edit_col_readonly(const EditSet *e, int col);   /* reason, or NULL when editable */
/* type checks before anything reaches the database (US-033 / US-050): numbers, booleans,
 * JSON documents; binary columns are refused */
bool edit_validate(const EditSet *e, int col, const char *val, char *why, size_t n);
/* Appends an empty pending insert (all DEFAULT), or a copy of row copy_from (>= 0) without
 * its key and read-only columns. Returns -1 when the ResultSet is full. */
int edit_row_add(EditSet *e, int copy_from);        /* new row index; keys stay DEFAULT on a copy */
void edit_row_toggle_delete(EditSet *e, int row);   /* an unsent insert is simply dropped */

/* statements for the review dialog: sql runs, shown has sensitive literals masked */
/* Regenerates e->stmts from the pending changes: UPDATEs, then DELETEs, then INSERTs. */
void edit_build(EditSet *e);
/* Frees e->stmts. */
void edit_stmts_free(EditSet *e);
/* One line describing how conflicts are detected (key, snapshot columns, version column and
 * the columns left out), for the review dialog. */
void edit_describe_strategy(const EditSet *e, char *buf, size_t n);
/* True when a column name looks like it holds secrets (password, token, cpf, ...); such
 * values are masked in shown SQL, notes and conflict messages. */
bool edit_sensitive_name(const char *col);

/* edit_apply outcomes; edit_apply_status_name gives a short label for each. CONFLICT: an
 * UPDATE / DELETE matched a number of rows other than 1. */
enum { APPLY_OK, APPLY_NOTHING, APPLY_NO_CONNECTION, APPLY_TX_FAILED, APPLY_STMT_FAILED, APPLY_CONFLICT,
       APPLY_COMMIT_FAILED, APPLY_UNKNOWN };
/* Outcome of edit_apply: status (APPLY_*), index of the statement that stopped the batch
 * (-1 when none), its affected rows, a user-facing message and, for conflicts, what differs
 * in the row now. */
typedef struct ApplyResult { int status, stmt, affected; char message[400]; char conflict[800]; } ApplyResult;
/* test hook: called before statement i runs (i == nstmts means COMMIT) */
typedef struct EditHooks { void (*before)(void *ud, int i, const char *sql); void *ud; } EditHooks;
/* Runs the statements in one transaction. Anything but APPLY_OK leaves the pending changes
 * in place. APPLY_UNKNOWN means the COMMIT was sent and the connection died before the
 * answer: the caller must say so and never claim success or rollback. */
int edit_apply(EditSet *e, Db *db, const EditHooks *h, ApplyResult *out);
/* Short static label for an APPLY_* status ("applied", "conflict", ...); "?" when unknown. */
const char *edit_apply_status_name(int status);
