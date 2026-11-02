/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "db.h"
#include "rs.h"
#include <pthread.h>
#include <stdint.h>

/* Statement splitting and asynchronous execution (OUT-007, OUT-015, OUT-018). The worker
 * thread owns the connection while a job runs (db->busy); it runs the statements one by one
 * and leaves each ResultSet in the job for the main thread to pick up. Nothing here touches
 * the UI or the output log: the main thread polls and emits events. */

/* Lexical rules the splitter applies: PG adds E'...' escapes and $tag$ quoting; Oracle adds
 * q'[...]' quoting and PL/SQL units terminated by a line holding only "/". */
enum { SQL_DIALECT_GENERIC, SQL_DIALECT_PG, SQL_DIALECT_ORACLE };
typedef struct SqlStmt { int start, end; } SqlStmt;   /* byte offsets, trimmed, comment-only pieces dropped */
/* Splits a script into statements at top-level ';' (ignoring those inside strings, quoted
 * identifiers, comments and dialect-specific quoting). Writes at most `max` entries to out
 * and returns how many were written. len < 0 means NUL-terminated. */
int sql_split(const char *text, int len, int dialect, SqlStmt *out, int max);
/* Splitter dialect for a driver (by its name); GENERIC for SQLite and unknown drivers. */
int sql_dialect_for(const DbDriver *drv);
/* line / column (1-based) of a byte offset inside text */
void sql_line_col(const char *text, int len, int offset, int *line, int *col);

/* Status of one statement of a job. SKIPPED: not run because an earlier one failed with
 * stop_on_error; CANCELLED: cancelled while running, or not started after a cancel. */
enum { XS_PENDING, XS_RUNNING, XS_OK, XS_FAILED, XS_SKIPPED, XS_CANCELLED };
/* One statement of a job: its byte range in the job text, its status (written by the worker
 * with atomic stores), its ResultSet (owned by the job until taken), whether the caller took
 * the ResultSet, its duration in ms and its wall-clock start time. */
typedef struct ExecStmt { int start, end; volatile int status; ResultSet rs; bool taken; double ms; double t_start; /* wall clock, seconds */ } ExecStmt;

/* One run of a script. Created by exec_start, freed by exec_free (main thread only). */
typedef struct ExecJob {
    Db *db; char *text; int len; int fetch_size; bool stop_on_error;   /* connection (lent), owned copy of the script, rows per first fetch, abort after a failure */
    ExecStmt *stmts; int n;   /* one per split statement */
    volatile int cur;            /* statement being run */
    volatile int cancel_req;     /* 1 after exec_cancel(): the worker runs no further statement */
    volatile int done;   /* 1 once the worker has returned (db->busy is already cleared) */
    double started, finished;    /* monotonic seconds */
    pthread_t th; bool thread_ok, joined;   /* worker thread; thread_ok when it was created */
    int reported;                /* statements whose outcome the caller consumed (exec_next_done) */
    /* caller bookkeeping, untouched here */
    int exec_id, mode; uint32_t editor_rev; int sql_base; int started_reported;
    bool finally_last;           /* the last statement runs even after a failure / cancel (ROLLBACK of an envelope) */
} ExecJob;

/* splits and starts the worker; NULL when the connection is busy or there is nothing to run */
ExecJob *exec_start(Db *db, const char *sql, int len, int fetch_size, bool stop_on_error);
/* Runs already separated statements (no re-split), in order; finally_last: the last one always
 * runs (a ROLLBACK closing an envelope). NULL when the connection is busy. */
ExecJob *exec_start_list(Db *db, const char *const *sql, int n, int fetch_size, bool stop_on_error, bool finally_last);
/* True while the worker has not finished (false for NULL). */
bool exec_running(const ExecJob *j);
/* True between exec_cancel and the worker's return. */
bool exec_cancelling(const ExecJob *j);
void exec_cancel(ExecJob *j);                 /* driver cancel + no further statements; the state stays "cancelling" until the worker returns */
/* Milliseconds since the start, frozen at the finish time once done. */
double exec_elapsed_ms(const ExecJob *j);
int exec_next_done(ExecJob *j);               /* index of the next finished statement not yet consumed, or -1 */
bool exec_take_result(ExecJob *j, int i, ResultSet *out);   /* moves the ResultSet out; false when none / already taken */
int exec_overall(const ExecJob *j);           /* ES_* from outlog.h semantics: 3 completed, 4 failed, 5 cancelled */
void exec_free(ExecJob *j);                   /* cancels and joins when still running */
