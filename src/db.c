/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Driver abstraction helpers and the SQLite driver.
 *
 * The first part holds what every driver shares: string duplication, ANSI identifier
 * quoting, freeing of catalog arrays and db_open / db_close (see db.h). The second part is
 * the SQLite driver, linked directly against libsqlite3.
 *
 * SQLite specifics:
 *   - db->handle is the sqlite3 *;
 *   - exec() runs a whole script with sqlite3_prepare_v2 / tail, statement by statement;
 *     the last row-returning statement is kept, and when the row limit stops it, its
 *     sqlite3_stmt stays open as rs->cursor for fetch();
 *   - blobs are rendered as "\x..." hex text, the same shape as PostgreSQL bytea;
 *   - cancel uses sqlite3_interrupt, the only call that is safe from another thread.
 *
 * Threading: exec / fetch / release run on the executor's worker thread (exec.c); the
 * catalog calls run on the UI thread when the connection is idle.
 */
#include "db.h"
#include <sqlite3.h>

/* NULL-tolerant strdup; the result is freed with free(). */
static char *dupstr(const char *s) { return s ? SDL_strdup(s) : NULL; }
/* Exported dupstr for the other drivers. */
char *db_strdup(const char *s) { return dupstr(s); }

/* See db.h. Each '.' closes the current quoted part and opens the next one. */
void db_quote_ident_double(const char *ident, char *buf, size_t buflen) {
    size_t n = 0;
    if (n < buflen - 1) buf[n++] = '"';
    for (const char *p = ident; *p && n < buflen - 4; p++) {
        if (*p == '.') { buf[n++] = '"'; buf[n++] = '.'; buf[n++] = '"'; continue; }
        if (*p == '"') buf[n++] = '"';
        buf[n++] = *p;
    }
    if (n < buflen - 1) buf[n++] = '"';
    buf[n] = 0;
}

/* See db.h. */
void db_fks_free(DbFk *fks, int count) {
    for (int i = 0; i < count; i++) { free(fks[i].name); free(fks[i].from_col); free(fks[i].to_table); free(fks[i].to_col); }
    free(fks);
}

/* See db.h. */
void db_tables_free(DbTable *tables, int count) {
    for (int i = 0; i < count; i++) {
        free(tables[i].name);
        for (int c = 0; c < tables[i].ncols; c++) { free(tables[i].cols[c].name); free(tables[i].cols[c].type); }
        free(tables[i].cols);
    }
    free(tables);
}

/* See db.h. The label is the part after the last '/' (or '\' on Windows); conn_connect
 * replaces it with the connection's name. */
Db *db_open(const DbDriver *drv, const char *location, char *err, size_t errlen) {
    Db *db = calloc(1, sizeof *db);
    db->drv = drv;
    db->location = dupstr(location);
    if (!drv->open(db, location, err, errlen)) { free(db->location); free(db); return NULL; }
    if (strcmp(location, ":memory:") == 0) {
        db->label = dupstr("memory");
    } else {
        const char *base = strrchr(location, '/');
#ifdef _WIN32
        const char *b2 = strrchr(location, '\\');
        if (b2 && (!base || b2 > base)) base = b2;
#endif
        db->label = dupstr(base ? base + 1 : location);
    }
    return db;
}

/* ---- closing and cancelling without blocking the caller (see db.h) ---- */

static void close_now(Db *db) {
    db->drv->close(db);
    free(db->label);
    free(db->location);
    free(db);
}
typedef struct CloseJob { Db *db; volatile int done; } CloseJob;
static int close_worker(void *ud) {
    CloseJob *cj = ud;
    Uint64 t0 = SDL_GetTicks();   /* a cancel in flight still uses the handle: let it finish first */
    while (__atomic_load_n(&cj->db->cancels, __ATOMIC_SEQ_CST) && SDL_GetTicks() - t0 < 10000) SDL_Delay(20);
    close_now(cj->db);
    __atomic_store_n(&cj->done, 1, __ATOMIC_SEQ_CST);
    return 0;
}
/* See db.h. */
void db_close(Db *db) {
    if (!db) return;
    if (__atomic_load_n(&db->abandoned, __ATOMIC_SEQ_CST)) return;   /* leaked on purpose: see Db.abandoned */
    if (db->drv == &db_sqlite_driver) { close_now(db); return; }     /* a file: nothing to wait for */
    const char *name = db->drv->name;
    CloseJob *cj = calloc(1, sizeof *cj);
    if (cj) cj->db = db;
    SDL_Thread *th = cj ? SDL_CreateThread(close_worker, "lume-close", cj) : NULL;
    if (!th) { free(cj); close_now(db); return; }
    Uint64 t0 = SDL_GetTicks();
    while (!__atomic_load_n(&cj->done, __ATOMIC_SEQ_CST) && SDL_GetTicks() - t0 < 5000) SDL_Delay(5);
    if (__atomic_load_n(&cj->done, __ATOMIC_SEQ_CST)) { SDL_WaitThread(th, NULL); free(cj); return; }
    SDL_Log("closing a %s session did not finish in 5 s (the network died under it?): it is left to finish on its own", name);
    SDL_DetachThread(th);   /* cj and db are freed by the worker when the driver returns */
}
static int cancel_worker(void *ud) {
    Db *db = ud;
    db->drv->cancel(db);
    __atomic_sub_fetch(&db->cancels, 1, __ATOMIC_SEQ_CST);
    return 0;
}
/* See db.h. */
void db_cancel(Db *db) {
    if (!db || !db->drv->cancel) return;
    if (db->drv == &db_sqlite_driver) { db->drv->cancel(db); return; }   /* sqlite3_interrupt: a flag, instant */
    __atomic_add_fetch(&db->cancels, 1, __ATOMIC_SEQ_CST);
    SDL_Thread *th = SDL_CreateThread(cancel_worker, "lume-cancel", db);
    if (!th) { cancel_worker(db); return; }
    SDL_DetachThread(th);
}

/* ---- SQLite driver ----------------------------------------------------- */

/* Opens an existing database file read-write (never creates one, except ":memory:") and
 * probes sqlite_master so a non-database file fails here with a clear message. */
static bool sq_open(Db *db, const char *location, char *err, size_t errlen) {
    sqlite3 *h = NULL;
    int flags = SQLITE_OPEN_READWRITE;
    if (strcmp(location, ":memory:") == 0) flags |= SQLITE_OPEN_CREATE | SQLITE_OPEN_MEMORY;
    if (sqlite3_open_v2(location, &h, flags, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "%s", h ? sqlite3_errmsg(h) : "out of memory");
        sqlite3_close(h);
        return false;
    }
    /* Reject files that are not SQLite databases early, with a readable message. */
    char *qerr = NULL;
    if (sqlite3_exec(h, "SELECT count(*) FROM sqlite_master", NULL, NULL, &qerr) != SQLITE_OK) {
        snprintf(err, errlen, "%s", qerr ? qerr : "not a database");
        sqlite3_free(qerr);
        sqlite3_close(h);
        return false;
    }
    db->handle = h;
    return true;
}

/* close: closes the sqlite3 handle. */
static void sq_close(Db *db) { sqlite3_close((sqlite3 *)db->handle); }
/* cancel: sqlite3_interrupt, safe to call from the UI thread during exec. */
static void sq_cancel(Db *db) { sqlite3_interrupt((sqlite3 *)db->handle); }
/* tx_state: 1 while a BEGIN ... COMMIT is open, 0 in autocommit mode. */
static int sq_tx_state(Db *db) { return sqlite3_get_autocommit((sqlite3 *)db->handle) ? 0 : 1; }
/* message plus extended code and, when the library reports it, the byte offset of the error */
static void sq_fail(sqlite3 *h, ResultSet *rs, int base_offset) {
    snprintf(rs->error, sizeof rs->error, "%s", sqlite3_errmsg(h));
    rs->err_code = sqlite3_extended_errcode(h);
#if SQLITE_VERSION_NUMBER >= 3038000
    int off = sqlite3_error_offset(h);
    if (off >= 0) rs->err_offset = base_offset + off;
#else
    (void)base_offset;
#endif
    rs->cancelled = sqlite3_errcode(h) == SQLITE_INTERRUPT;
    rs->ok = false;
}

/* Tables and views from sqlite_master (internal sqlite_* tables excluded), tables first,
 * then views, each sorted case-insensitively. */
static bool sq_list_tables(Db *db, DbTable **out, int *count, char *err, size_t errlen) {
    sqlite3 *h = db->handle;
    sqlite3_stmt *st = NULL;
    const char *sql = "SELECT name, type FROM sqlite_master WHERE type IN ('table','view') "
                      "AND name NOT LIKE 'sqlite_%' ORDER BY type, lower(name)";
    if (sqlite3_prepare_v2(h, sql, -1, &st, NULL) != SQLITE_OK) {
        snprintf(err, errlen, "%s", sqlite3_errmsg(h));
        return false;
    }
    DbTable *tables = NULL;
    int n = 0, cap = 0;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap = cap ? cap * 2 : 16; tables = realloc(tables, sizeof *tables * cap); }
        DbTable *t = &tables[n++];
        memset(t, 0, sizeof *t);
        t->name = dupstr((const char *)sqlite3_column_text(st, 0));
        t->is_view = strcmp((const char *)sqlite3_column_text(st, 1), "view") == 0;
    }
    sqlite3_finalize(st);
    *out = tables;
    *count = n;
    return true;
}

/* Foreign keys via PRAGMA foreign_key_list: one DbFk per column, named "fk<id>" so the
 * columns of a composite key share a name. to_col is "" when the key references the
 * parent's primary key implicitly. */
static bool sq_list_fks(Db *db, const char *table, DbFk **out, int *count, char *err, size_t errlen) {
    sqlite3 *h = db->handle;
    char *sql = sqlite3_mprintf("PRAGMA foreign_key_list(%Q)", table);
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(h, sql, -1, &st, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { snprintf(err, errlen, "%s", sqlite3_errmsg(h)); return false; }
    int n = 0, cap = 0;
    DbFk *fks = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap = cap ? cap * 2 : 8; fks = realloc(fks, sizeof *fks * cap); }
        /* columns: id, seq, table, from, to, on_update, on_delete, match */
        char nm[24]; snprintf(nm, sizeof nm, "fk%d", sqlite3_column_int(st, 0));
        fks[n].name = dupstr(nm);
        fks[n].to_table = dupstr((const char *)sqlite3_column_text(st, 2));
        fks[n].from_col = dupstr((const char *)sqlite3_column_text(st, 3));
        const char *to = (const char *)sqlite3_column_text(st, 4);
        fks[n].to_col = dupstr(to ? to : "");
        n++;
    }
    sqlite3_finalize(st);
    *out = fks; *count = n;
    return true;
}

/* Columns via PRAGMA table_xinfo, which also reports generated columns. Columns without a
 * declared type get "any". Hidden virtual-table columns are skipped. */
static bool sq_list_columns(Db *db, DbTable *t, char *err, size_t errlen) {
    sqlite3 *h = db->handle;
    char *sql = sqlite3_mprintf("PRAGMA table_xinfo(%Q)", t->name);   /* xinfo: column 6 = hidden (2 / 3 = generated) */
    sqlite3_stmt *st = NULL;
    int rc = sqlite3_prepare_v2(h, sql, -1, &st, NULL);
    sqlite3_free(sql);
    if (rc != SQLITE_OK) { snprintf(err, errlen, "%s", sqlite3_errmsg(h)); return false; }
    int n = 0, cap = 0;
    DbColumn *cols = NULL;
    while (sqlite3_step(st) == SQLITE_ROW) {
        if (n == cap) { cap = cap ? cap * 2 : 8; cols = realloc(cols, sizeof *cols * cap); }
        DbColumn *c = &cols[n++];
        c->name = dupstr((const char *)sqlite3_column_text(st, 1));
        const char *type = (const char *)sqlite3_column_text(st, 2);
        c->type = dupstr(type && *type ? type : "any");
        c->notnull = sqlite3_column_int(st, 3) != 0;
        c->pk = sqlite3_column_int(st, 5) != 0;
        int hidden = sqlite3_column_int(st, 6);
        c->generated = hidden == 2 || hidden == 3;
        c->identity_always = false;
        if (hidden == 1) { free(c->name); free(c->type); n--; }   /* hidden virtual-table column: not addressable */
    }
    sqlite3_finalize(st);
    t->cols = cols;
    t->ncols = n;
    t->cols_loaded = true;
    return true;
}

/* Text of one cell for the ResultSet. Returns NULL for SQL NULL, sqlite's own text buffer
 * for ordinary values (valid until the next step), or for blobs a hex string allocated with
 * sqlite3_malloc and returned through *blob_out (caller frees with sqlite3_free). Blobs over
 * 1 MB become a "<blob N bytes>" placeholder in `tmp`. *len receives the byte length. */
static const char *cell_text(sqlite3_stmt *st, int col, char *tmp, size_t tmpn, int *len, char **blob_out) {
    switch (sqlite3_column_type(st, col)) {
    case SQLITE_NULL: *len = 0; return NULL;
    case SQLITE_BLOB: {
        /* hex text like PostgreSQL bytea ("\\x0102"): the viewer decodes it (US-021); huge
         * blobs are described, not copied */
        int n = sqlite3_column_bytes(st, col);
        if (n > (1 << 20)) { *len = snprintf(tmp, tmpn, "<blob %d bytes>", n); return tmp; }
        const unsigned char *b = sqlite3_column_blob(st, col);
        char *hex = sqlite3_malloc(2 * n + 3);
        if (!hex) { *len = snprintf(tmp, tmpn, "<blob %d bytes>", n); return tmp; }
        static const char digits[] = "0123456789abcdef";
        hex[0] = '\\'; hex[1] = 'x';
        for (int i = 0; i < n; i++) { hex[2 + 2 * i] = digits[b[i] >> 4]; hex[3 + 2 * i] = digits[b[i] & 15]; }
        hex[2 + 2 * n] = 0;
        *len = 2 + 2 * n;
        snprintf(tmp, tmpn, "%s", "");
        *blob_out = hex;
        return hex;
    }
    default:
        *len = sqlite3_column_bytes(st, col);
        return (const char *)sqlite3_column_text(st, col);
    }
}

/* Appends up to n rows from a stepped statement. Returns the last step result. */
static int sq_pull(sqlite3_stmt *st, ResultSet *rs, int n) {
    int rc = SQLITE_DONE;
    char tmp[64];
    for (int i = 0; n <= 0 || i < n; i++) {
        rc = sqlite3_step(st);
        if (rc != SQLITE_ROW) break;
        int row = rs_add_row(rs);
        if (row < 0) { rc = SQLITE_ROW; rs->truncated = true; break; }
        for (int c = 0; c < rs->ncols; c++) {
            int len;
            char *blob = NULL;
            const char *v = cell_text(st, c, tmp, sizeof tmp, &len, &blob);
            rs_set(rs, row, c, v, len);
            if (blob) sqlite3_free(blob);
        }
    }
    return rc;
}

/* Finalizes the open cursor statement, if any. */
static void sq_release(Db *db, ResultSet *rs) {
    (void)db;
    if (rs->cursor) sqlite3_finalize((sqlite3_stmt *)rs->cursor);
    rs->cursor = NULL;
}

/* Continues a truncated result by up to n rows (n <= 0: all), adding the time to fetch_ms.
 * At the end of the rows (or on error) the cursor is released. Returns true when at least
 * one row was added. Worker thread. */
static bool sq_fetch(Db *db, ResultSet *rs, int n) {
    if (!rs->cursor) return false;
    Uint64 t0 = SDL_GetPerformanceCounter();
    int before = rs->nrows;
    int rc = sq_pull((sqlite3_stmt *)rs->cursor, rs, n);
    rs->fetch_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (rc == SQLITE_DONE) { sq_release(db, rs); rs->complete = true; rs->truncated = false; }
    else if (rc != SQLITE_ROW) { snprintf(rs->error, sizeof rs->error, "%s", sqlite3_errmsg(db->handle)); sq_release(db, rs); }
    return rs->nrows > before;
}

/* Runs every statement of `sql` (len bytes, or NUL-terminated when len < 0) in order. The
 * ResultSet ends up with the rows of the last row-returning statement (up to `limit`),
 * the sum of affected rows of the others, and the first error (which stops the script; its
 * offset is relative to the start of `sql`). Timing is split into exec_ms and fetch_ms.
 * Worker thread. */
static void sq_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs) {
    sqlite3 *h = db->handle;
    rs_init(rs);
    rs->db = db;
    if (len < 0) len = (int)strlen(sql);
    char *text = malloc(len + 1);
    memcpy(text, sql, len);
    text[len] = 0;

    Uint64 t0 = SDL_GetPerformanceCounter();
    const char *p = text;
    rs->ok = true;
    rs->complete = true;
    /* one iteration per statement: skip separators, prepare, run, advance to the tail */
    while (*p) {
        while (*p && (*p == ' ' || *p == '\n' || *p == '\r' || *p == '\t' || *p == ';')) p++;
        if (!*p) break;
        sqlite3_stmt *st = NULL;
        const char *tail = NULL;
        if (sqlite3_prepare_v2(h, p, -1, &st, &tail) != SQLITE_OK) { sq_fail(h, rs, (int)(p - text)); break; }
        if (!st) { p = tail; continue; }
        int ncols = sqlite3_column_count(st);
        int rc;
        if (ncols > 0) {
            /* a later row-returning statement replaces the previous rows (and its cursor) */
            Db *keep = rs->db;
            rs_free(rs);
            rs_init(rs);
            rs->db = keep; rs->ok = true;
            for (int c = 0; c < ncols; c++) {
                const char *type = sqlite3_column_decltype(st, c);
                rs_add_column(rs, sqlite3_column_name(st, c), type ? type : "", rs_kind_from_type(type));
            }
            rs->has_rows = true;
            Uint64 t1 = SDL_GetPerformanceCounter();
            rc = sq_pull(st, rs, limit);
            rs->fetch_ms = (SDL_GetPerformanceCounter() - t1) * 1000.0 / SDL_GetPerformanceFrequency();
            if (rc == SQLITE_ROW) {
                /* stopped by the limit: keep the statement open as the cursor */
                rs->truncated = true;
                rs->complete = false;
                rs->cursor = st;
                st = NULL;
            } else rs->complete = true;
        } else {
            rc = sqlite3_step(st);
            if (rc == SQLITE_DONE) rs->affected += sqlite3_changes(h);
        }
        if (rc != SQLITE_DONE && rc != SQLITE_ROW) sq_fail(h, rs, (int)(p - text));
        if (st) sqlite3_finalize(st);
        if (!rs->ok) break;
        p = tail;
    }
    double total = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    rs->exec_ms = total - rs->fetch_ms;
    free(text);
}

/* Quotes a single identifier with double quotes, doubling embedded quotes. Unlike
 * db_quote_ident_double it does not split on dots: SQLite has no schemas to qualify. */
static void sq_quote_ident(const char *ident, char *buf, size_t buflen) {
    size_t n = 0;
    if (n < buflen - 1) buf[n++] = '"';
    for (const char *p = ident; *p && n < buflen - 2; p++) {
        if (*p == '"' && n < buflen - 3) buf[n++] = '"';
        buf[n++] = *p;
    }
    if (n < buflen - 1) buf[n++] = '"';
    buf[n] = 0;
}

/* select_sql for SQLite: SELECT * ... [WHERE ...] [ORDER BY ...] LIMIT n. */
static void sq_select(const char *q, const char *where, const char *order, int limit, char *buf, size_t n) {
    snprintf(buf, n, "SELECT * FROM %s%s%s%s%s LIMIT %d;", q, where && *where ? " WHERE " : "", where ? where : "",
             order && *order ? " ORDER BY " : "", order ? order : "", limit);
}

/* Field order follows struct DbDriver: no rehome (the cursor does not point back at the
 * ResultSet) and no set_autocommit (BEGIN handles transactions). */
/* SQLite is linked: always available; the version is the library's own. */
static bool sq_probe(char *v, size_t n) { snprintf(v, n, "%s", sqlite3_libversion()); return true; }

const DbDriver db_sqlite_driver = {
    "SQLite", sq_open, sq_close, sq_list_tables, sq_list_columns, sq_exec, sq_fetch, sq_release, NULL, sq_quote_ident, sq_select,
    DB_CAP_FETCH_MORE | DB_CAP_CANCEL | DB_CAP_SERVER_SORT | DB_CAP_SERVER_FILTER, "BEGIN", sq_list_fks, NULL, sq_cancel, sq_tx_state, sq_probe,
};
