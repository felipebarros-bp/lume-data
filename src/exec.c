/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Statement splitter and asynchronous executor: see exec.h.
 *
 * Splitter: a single left-to-right scan that tracks comments, string literals, quoted
 * identifiers and dialect-specific quoting (PostgreSQL E'...' and $tag$, Oracle q'[...]' and
 * PL/SQL units ending at a "/" line), and cuts at top-level ';'. It only returns byte ranges
 * into the caller's text; nothing is copied.
 *
 * Executor: exec_start copies the script, splits it and starts one worker thread per job.
 * The worker runs the statements in order through the driver's exec(), storing each outcome
 * in its ExecStmt and publishing the status with an atomic store. The main thread polls
 * (exec_next_done / exec_take_result) and turns outcomes into UI events; nothing here
 * touches the UI.
 *
 * Threading and ownership:
 *   - db->busy is 1 from exec_start until the worker returns; during that time only the
 *     worker calls the driver, except cancel() which the UI thread may call (exec_cancel);
 *   - a statement's ResultSet belongs to the job until exec_take_result moves it out; the
 *     main thread must not read it before its status is final;
 *   - exec_free (main thread) joins the worker and frees whatever was not taken.
 */
#include "exec.h"
#include <ctype.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>

/* Monotonic clock in seconds, for durations. */
static double mono_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

/* ---- splitter -------------------------------------------------------------- */

/* See exec.h. */
int sql_dialect_for(const DbDriver *drv) {
    if (!drv || !drv->name) return SQL_DIALECT_GENERIC;
    if (strcmp(drv->name, "PostgreSQL") == 0) return SQL_DIALECT_PG;
    if (strcmp(drv->name, "Oracle") == 0) return SQL_DIALECT_ORACLE;
    return SQL_DIALECT_GENERIC;
}

/* True when keyword w (case-insensitive) starts at t[i] as a whole word, i.e. not glued to
 * letters, digits or '_' on either side. */
static bool word_at(const char *t, int len, int i, const char *w) {
    int n = (int)strlen(w);
    if (i + n > len || strncasecmp(t + i, w, n) != 0) return false;
    if (i > 0 && (isalnum((unsigned char)t[i - 1]) || t[i - 1] == '_')) return false;
    if (i + n < len && (isalnum((unsigned char)t[i + n]) || t[i + n] == '_')) return false;
    return true;
}

/* an Oracle PL/SQL unit ends at a line holding only "/" (or at end of text), not at ';' */
/* True when the statement starting at t[i] is a PL/SQL unit: BEGIN, DECLARE, or
 * CREATE [OR REPLACE] FUNCTION / PROCEDURE / PACKAGE / TRIGGER / TYPE. */
static bool plsql_start(const char *t, int len, int i) {
    if (word_at(t, len, i, "BEGIN") || word_at(t, len, i, "DECLARE")) return true;
    if (!word_at(t, len, i, "CREATE")) return false;
    int j = i + 6;
    while (j < len && isspace((unsigned char)t[j])) j++;
    if (word_at(t, len, j, "OR")) { j += 2; while (j < len && isspace((unsigned char)t[j])) j++; if (word_at(t, len, j, "REPLACE")) j += 7; while (j < len && isspace((unsigned char)t[j])) j++; }
    static const char *const units[] = { "FUNCTION", "PROCEDURE", "PACKAGE", "TRIGGER", "TYPE", NULL };
    for (int k = 0; units[k]; k++) if (word_at(t, len, j, units[k])) return true;
    return false;
}

/* Appends t[s..e) to out after trimming whitespace and (unless keep_semicolon, used for
 * PL/SQL units) trailing ';'. Pieces with only comments or whitespace are dropped, and so
 * is everything once `max` entries are stored. */
static void add_stmt(const char *t, int s, int e, SqlStmt *out, int *n, int max, bool keep_semicolon) {
    while (s < e && isspace((unsigned char)t[s])) s++;
    while (e > s && (isspace((unsigned char)t[e - 1]) || (!keep_semicolon && t[e - 1] == ';'))) e--;
    /* comment-only pieces are not statements */
    bool code = false;
    for (int k = s; k < e && !code; k++) {
        if (t[k] == '-' && k + 1 < e && t[k + 1] == '-') { while (k < e && t[k] != '\n') k++; continue; }
        if (t[k] == '/' && k + 1 < e && t[k + 1] == '*') { k += 2; while (k + 1 < e && !(t[k] == '*' && t[k + 1] == '/')) k++; k++; continue; }
        if (!isspace((unsigned char)t[k])) code = true;
    }
    if (!code || *n >= max) return;
    out[*n].start = s; out[*n].end = e; (*n)++;
}

/* See exec.h. Each branch below consumes one lexical element (comment, literal, quoted
 * name, dollar-quoted body) as a whole so a ';' inside it never splits. Unterminated
 * literals or comments simply run to the end of the text. */
int sql_split(const char *t, int len, int dialect, SqlStmt *out, int max) {
    if (len < 0) len = (int)strlen(t);
    int n = 0, start = 0, i = 0;
    bool unit = false;           /* inside an Oracle PL/SQL unit */
    bool at_stmt_start = true;
    while (i < len) {
        char c = t[i];
        if (at_stmt_start && !isspace((unsigned char)c)) {
            at_stmt_start = false;
            if (dialect == SQL_DIALECT_ORACLE && plsql_start(t, len, i)) unit = true;
        }
        if (c == '-' && i + 1 < len && t[i + 1] == '-') { while (i < len && t[i] != '\n') i++; continue; }
        if (c == '/' && i + 1 < len && t[i + 1] == '*') { i += 2; while (i + 1 < len && !(t[i] == '*' && t[i + 1] == '/')) i++; i += 2; continue; }
        /* string literal: '' doubles a quote; E'...' also allows backslash escapes (PG) */
        if (c == '\'' ) {
            bool escapes = dialect == SQL_DIALECT_PG && i > 0 && (t[i - 1] == 'E' || t[i - 1] == 'e') && (i < 2 || !isalnum((unsigned char)t[i - 2]));
            if (dialect == SQL_DIALECT_ORACLE && i > 0 && (t[i - 1] == 'q' || t[i - 1] == 'Q') && (i < 2 || !isalnum((unsigned char)t[i - 2])) && i + 1 < len) {
                /* q'[...]' alternative quoting */
                char d = t[i + 1], close = d == '[' ? ']' : d == '{' ? '}' : d == '(' ? ')' : d == '<' ? '>' : d;
                i += 2;
                while (i + 1 < len && !(t[i] == close && t[i + 1] == '\'')) i++;
                i += 2; continue;
            }
            i++;
            while (i < len) { if (escapes && t[i] == '\\') { i += 2; continue; } if (t[i] == '\'') { if (i + 1 < len && t[i + 1] == '\'') { i += 2; continue; } break; } i++; }
            i++; continue;
        }
        /* quoted identifier */
        if (c == '"') { i++; while (i < len && t[i] != '"') i++; i++; continue; }
        if (c == '$' && dialect == SQL_DIALECT_PG) {
            /* $tag$ ... $tag$ */
            int j = i + 1;
            while (j < len && (isalnum((unsigned char)t[j]) || t[j] == '_')) j++;
            if (j < len && t[j] == '$' && (j == i + 1 || !isdigit((unsigned char)t[i + 1]))) {
                int tl = j - i + 1;
                int k = j + 1;
                while (k + tl <= len && strncmp(t + k, t + i, tl) != 0) k++;
                i = k + tl; continue;
            }
        }
        if (unit) {
            /* a line that is only "/" ends the unit */
            if (c == '\n') {
                int j = i + 1;
                while (j < len && (t[j] == ' ' || t[j] == '\t')) j++;
                if (j < len && t[j] == '/' && (j + 1 >= len || t[j + 1] == '\n' || t[j + 1] == '\r')) {
                    add_stmt(t, start, j, out, &n, max, true);
                    start = j + 1; i = j + 1; unit = false; at_stmt_start = true; continue;
                }
            }
            i++; continue;
        }
        /* ordinary statement end; in Oracle a lone "/" line also ends a plain statement */
        if (c == ';') { add_stmt(t, start, i, out, &n, max, false); start = i + 1; at_stmt_start = true; i++; continue; }
        if (dialect == SQL_DIALECT_ORACLE && c == '/' && (i == 0 || t[i - 1] == '\n') && (i + 1 >= len || t[i + 1] == '\n' || t[i + 1] == '\r')) {
            add_stmt(t, start, i, out, &n, max, unit); start = i + 1; at_stmt_start = true; i++; continue;
        }
        i++;
    }
    /* whatever follows the last separator is the final statement */
    add_stmt(t, start, len, out, &n, max, unit);
    return n;
}

/* See exec.h. Columns count bytes, not characters. */
void sql_line_col(const char *text, int len, int offset, int *line, int *col) {
    if (len < 0) len = (int)strlen(text);
    int l = 1, c = 1;
    for (int i = 0; i < offset && i < len; i++) { if (text[i] == '\n') { l++; c = 1; } else c++; }
    *line = l; *col = c;
}

/* ---- executor --------------------------------------------------------------- */

/* Worker thread body: runs each statement of the job in order. Before each one it checks
 * cancel_req (remaining statements become CANCELLED); after each one it publishes the
 * status, turns a cancelled statement into a job-wide cancel, and with stop_on_error marks
 * the rest SKIPPED after a failure. On exit it clears db->busy, then sets done. */
static void *worker(void *p) {
    ExecJob *j = p;
    int last = j->finally_last ? j->n - 1 : -1;   /* runs even after a failure or a cancel (ROLLBACK) */
    for (int i = 0; i < j->n; i++) {
        ExecStmt *st = &j->stmts[i];
        if (i != last && __atomic_load_n(&j->cancel_req, __ATOMIC_SEQ_CST)) { __atomic_store_n(&st->status, XS_CANCELLED, __ATOMIC_SEQ_CST); continue; }
        __atomic_store_n(&j->cur, i, __ATOMIC_SEQ_CST);
        { struct timespec ts; clock_gettime(CLOCK_REALTIME, &ts); st->t_start = ts.tv_sec + ts.tv_nsec / 1e9; }
        __atomic_store_n(&st->status, XS_RUNNING, __ATOMIC_SEQ_CST);
        double t0 = mono_s();
        j->db->drv->exec(j->db, j->text + st->start, st->end - st->start, j->fetch_size, &st->rs);
        st->ms = (mono_s() - t0) * 1000.0;
        /* only the last statement may keep its cursor: the next one would take the connection
         * anyway, and a half-open cursor is reported as partial */
        if (i + 1 < j->n && st->rs.cursor) { rs_release(&st->rs); st->rs.cursor_lost = true; st->rs.complete = false; }
        int status = st->rs.ok ? XS_OK : st->rs.cancelled ? XS_CANCELLED : XS_FAILED;
        __atomic_store_n(&st->status, status, __ATOMIC_SEQ_CST);
        if (status == XS_CANCELLED) __atomic_store_n(&j->cancel_req, 1, __ATOMIC_SEQ_CST);
        if (status == XS_FAILED && j->stop_on_error) {
            for (int k = i + 1; k < j->n; k++) if (k != last) __atomic_store_n(&j->stmts[k].status, XS_SKIPPED, __ATOMIC_SEQ_CST);
            if (last > i) { i = last - 1; continue; }
            break;
        }
    }
    j->finished = mono_s();
    __atomic_store_n(&j->db->busy, 0, __ATOMIC_SEQ_CST);
    __atomic_store_n(&j->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}

/* See exec.h. Splits with the connection's dialect (at most 1024 statements), copies the
 * script so the editor can change meanwhile, marks the Db busy and starts the worker.
 * Main thread. The returned job is freed with exec_free. */
ExecJob *exec_start(Db *db, const char *sql, int len, int fetch_size, bool stop_on_error) {
    if (!db || __atomic_load_n(&db->busy, __ATOMIC_SEQ_CST)) return NULL;
    if (len < 0) len = (int)strlen(sql);
    SqlStmt parts[1024];
    int n = sql_split(sql, len, sql_dialect_for(db->drv), parts, 1024);
    if (n == 0) return NULL;
    ExecJob *j = calloc(1, sizeof *j);
    j->db = db; j->text = malloc(len + 1); memcpy(j->text, sql, len); j->text[len] = 0; j->len = len;
    j->fetch_size = fetch_size; j->stop_on_error = stop_on_error;
    j->stmts = calloc(n, sizeof *j->stmts); j->n = n;
    for (int i = 0; i < n; i++) { j->stmts[i].start = parts[i].start; j->stmts[i].end = parts[i].end; rs_init(&j->stmts[i].rs); }
    j->cur = -1;
    j->started = mono_s();
    __atomic_store_n(&db->busy, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&j->th, NULL, worker, j) != 0) {
        __atomic_store_n(&db->busy, 0, __ATOMIC_SEQ_CST);
        free(j->text); free(j->stmts); free(j);
        return NULL;
    }
    j->thread_ok = true;
    return j;
}

/* See exec.h. Statements are joined with newlines in the job text; each keeps its own range,
 * so nothing is split again (a trigger or PL/SQL unit with inner ';' stays one statement). */
ExecJob *exec_start_list(Db *db, const char *const *sql, int n, int fetch_size, bool stop_on_error, bool finally_last) {
    if (!db || n <= 0 || __atomic_load_n(&db->busy, __ATOMIC_SEQ_CST)) return NULL;
    size_t len = 0; for (int i = 0; i < n; i++) len += strlen(sql[i]) + 1;
    ExecJob *j = calloc(1, sizeof *j);
    j->db = db; j->text = malloc(len + 1); j->len = (int)len;
    j->fetch_size = fetch_size; j->stop_on_error = stop_on_error; j->finally_last = finally_last;
    j->stmts = calloc((size_t)n, sizeof *j->stmts); j->n = n;
    size_t o = 0;
    for (int i = 0; i < n; i++) {
        size_t l = strlen(sql[i]);
        memcpy(j->text + o, sql[i], l);
        j->stmts[i].start = (int)o; j->stmts[i].end = (int)(o + l); rs_init(&j->stmts[i].rs);
        o += l; j->text[o++] = '\n';
    }
    j->text[o] = 0;
    j->cur = -1;
    j->started = mono_s();
    __atomic_store_n(&db->busy, 1, __ATOMIC_SEQ_CST);
    if (pthread_create(&j->th, NULL, worker, j) != 0) {
        __atomic_store_n(&db->busy, 0, __ATOMIC_SEQ_CST);
        free(j->text); free(j->stmts); free(j);
        return NULL;
    }
    j->thread_ok = true;
    return j;
}

/* See exec.h. */
bool exec_running(const ExecJob *j) { return j && !__atomic_load_n(&j->done, __ATOMIC_SEQ_CST); }
/* See exec.h. */
bool exec_cancelling(const ExecJob *j) { return exec_running(j) && __atomic_load_n(&j->cancel_req, __ATOMIC_SEQ_CST); }

/* See exec.h. Main thread; the driver's cancel() is the one call allowed while the worker
 * owns the connection. */
void exec_cancel(ExecJob *j) {
    if (!j || !exec_running(j)) return;
    __atomic_store_n(&j->cancel_req, 1, __ATOMIC_SEQ_CST);
    db_cancel(j->db);   /* on its own thread; reaches the statement in flight, whose outcome tells whether it worked */
}

/* See exec.h. */
double exec_elapsed_ms(const ExecJob *j) { return ((exec_running(j) ? mono_s() : j->finished) - j->started) * 1000.0; }

/* See exec.h. Statements are reported strictly in order: a running statement blocks the
 * report of later ones. Each index is returned once. */
int exec_next_done(ExecJob *j) {
    for (int i = j->reported; i < j->n; i++) {
        int st = __atomic_load_n(&j->stmts[i].status, __ATOMIC_SEQ_CST);
        if (st == XS_PENDING || st == XS_RUNNING) return -1;
        j->reported = i + 1;
        return i;
    }
    return -1;
}

/* See exec.h. `out` receives ownership (free it with rs_free); a statement cancelled before
 * it started has no result. */
bool exec_take_result(ExecJob *j, int i, ResultSet *out) {
    if (i < 0 || i >= j->n || j->stmts[i].taken) return false;
    int st = __atomic_load_n(&j->stmts[i].status, __ATOMIC_SEQ_CST);
    if (st == XS_PENDING || st == XS_RUNNING || st == XS_SKIPPED) return false;
    if (st == XS_CANCELLED && !j->stmts[i].rs.db) return false;   /* never ran */
    rs_move(out, &j->stmts[i].rs);
    j->stmts[i].taken = true;
    return true;
}

/* See exec.h. Cancelled wins over failed, failed over completed. */
int exec_overall(const ExecJob *j) {
    bool failed = false, cancelled = false;
    for (int i = 0; i < j->n; i++) {
        int st = __atomic_load_n(&j->stmts[i].status, __ATOMIC_SEQ_CST);
        if (st == XS_FAILED) failed = true;
        if (st == XS_CANCELLED) cancelled = true;
    }
    return cancelled ? 5 : failed ? 4 : 3;
}

/* See exec.h. Blocks until the worker returns (a cancel is sent first). */
void exec_free(ExecJob *j) {
    if (!j) return;
    if (j->thread_ok) {
        exec_cancel(j);
        /* a worker blocked in the driver on a network that died (a dropped VPN) returns only when
         * TCP gives up, minutes later: after a few seconds it is left behind with its connection
         * and the job's memory (the window must not freeze on quit or disconnect) */
        double t0 = mono_s();
        while (!__atomic_load_n(&j->done, __ATOMIC_SEQ_CST) && mono_s() - t0 < 8.0) { struct timespec ts = { 0, 50000000 }; nanosleep(&ts, NULL); }
        if (!__atomic_load_n(&j->done, __ATOMIC_SEQ_CST)) {
            SDL_Log("a statement did not stop in 8 s (the network died under it?): its worker and session are left behind");
            __atomic_store_n(&j->db->abandoned, 1, __ATOMIC_SEQ_CST); pthread_detach(j->th); return;
        }
        pthread_join(j->th, NULL); j->joined = true;
    }
    for (int i = 0; i < j->n; i++) if (!j->stmts[i].taken) rs_free(&j->stmts[i].rs);
    free(j->stmts); free(j->text); free(j);
}
