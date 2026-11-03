/* SPDX-License-Identifier: AGPL-3.0-only */
/* PostgreSQL driver over libpq, loaded at runtime with dlopen so the build has no hard
 * dependency and the binary stays small. See docs/connections.md.
 *
 * How it works:
 *   - pq_load finds libpq (LUME_LIBPQ, then the usual install paths) and resolves the few
 *     PQ* functions used into the `pq` table, once per process;
 *   - db->handle is a PgConn: the PGconn, a PGcancel object for cancel(), the ResultSet that
 *     currently owns the streaming cursor and a buffer of server notices;
 *   - exec() sends the whole script with PQsendQuery in single-row mode and pulls rows until
 *     the limit; when rows remain, the connection itself is the cursor (rs->cursor = PgConn)
 *     and fetch() keeps pulling. Any other query on the connection first drains that cursor
 *     (pg_drain), marking the owning ResultSet cursor_lost;
 *   - the catalog uses information_schema / pg_catalog. Tables outside "public" are named
 *     "schema.table"; tables in "public" keep their bare name.
 *
 * Threading: exec / fetch / release run on the executor's worker thread; cancel() may be
 * called from the UI thread meanwhile (PQcancel is thread-safe). The notice receiver runs
 * inside libpq calls, i.e. on whichever thread is executing.
 */
#include "db.h"
#ifdef _WIN32
#include <windows.h>
#define DLOPEN(p) ((void *)LoadLibraryA(p))
#define DLSYM(h, s) ((void *)GetProcAddress((HMODULE)(h), s))
#else
#include <dlfcn.h>

/* db.h: defined here, not in db.c, because every build of the network drivers links db_pg.c and
 * some (NO_SQLITE) leave db.c out */
int db_connect_timeout_s = 10;
#define DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define DLSYM(h, s) dlsym(h, s)
#endif

/* Minimal libpq declarations (opaque types and the enum values used), so no libpq headers
 * are needed at build time. Values match libpq-fe.h. */
typedef struct pg_conn PGconn;
typedef struct pg_result PGresult;
enum { CONNECTION_OK = 0 };
enum { PGRES_EMPTY_QUERY = 0, PGRES_COMMAND_OK = 1, PGRES_TUPLES_OK = 2, PGRES_SINGLE_TUPLE = 9 };
typedef struct pg_cancel PGcancel;

/* Per-connection state: libpq allows one command at a time, so an open streaming cursor must be
 * drained before any other query runs on the same connection. */
/* Fields: c = the libpq connection; active = ResultSet whose cursor is streaming (NULL when
 * idle); cancel = PQgetCancel object kept for cancel(); notices / nlen = notices collected
 * during the current exec. */
typedef struct PgConn { PGconn *c; ResultSet *active; PGcancel *cancel; char notices[4096]; size_t nlen; } PgConn;
typedef void (*PQnoticeReceiver)(void *arg, const PGresult *res);

/* libpq entry points resolved by pq_load; `lib` is the dlopen handle (never closed). */
static struct {
    void *lib;
    PGconn *(*connectdb)(const char *);
    int (*status)(const PGconn *);
    char *(*errorMessage)(const PGconn *);
    void (*finish)(PGconn *);
    PGresult *(*exec)(PGconn *, const char *);
    int (*sendQuery)(PGconn *, const char *);
    int (*setSingleRowMode)(PGconn *);
    PGresult *(*getResult)(PGconn *);
    int (*resultStatus)(const PGresult *);
    char *(*resultErrorMessage)(const PGresult *);
    int (*nfields)(const PGresult *);
    int (*ntuples)(const PGresult *);
    char *(*fname)(const PGresult *, int);
    unsigned (*ftype)(const PGresult *, int);
    char *(*getvalue)(const PGresult *, int, int);
    int (*getlength)(const PGresult *, int, int);
    int (*getisnull)(const PGresult *, int, int);
    char *(*cmdTuples)(PGresult *);
    void (*clear)(PGresult *);
    PGcancel *(*getCancel)(PGconn *);
    char *(*resultErrorField)(const PGresult *, int);
    int (*transactionStatus)(const PGconn *);
    PQnoticeReceiver (*setNoticeReceiver)(PGconn *, PQnoticeReceiver, void *);
    int (*cancel)(PGcancel *, char *, int);
    void (*freeCancel)(PGcancel *);
} pq;

/* Loads libpq and resolves every symbol; true once done. The first library that opens wins.
 * On failure err explains how to install libpq or point LUME_LIBPQ at it. Not thread-safe:
 * the first call happens from a connect on the UI thread. */
static bool pq_load(char *err, size_t errlen) {
    if (pq.lib) return true;
    const char *env = SDL_getenv("LUME_LIBPQ");
    const char *candidates[] = {
        env,
#ifdef __APPLE__
        "libpq.5.dylib", "/opt/homebrew/opt/libpq/lib/libpq.5.dylib", "/usr/local/opt/libpq/lib/libpq.5.dylib",
        "/opt/homebrew/lib/libpq.5.dylib", "/usr/local/lib/libpq.5.dylib", "/Library/PostgreSQL/16/lib/libpq.5.dylib",
#elif defined(_WIN32)
        "libpq.dll",
#else
        "libpq.so.5", "libpq.so",
#endif
    };
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        if (!candidates[i]) continue;
        pq.lib = DLOPEN(candidates[i]);
        if (pq.lib) break;
    }
    if (!pq.lib) {
        snprintf(err, errlen, "libpq not found. Install PostgreSQL client libraries (brew install libpq) or set LUME_LIBPQ to the library path.");
        return false;
    }
#define LOAD(field, name) do { pq.field = DLSYM(pq.lib, name); if (!pq.field) { snprintf(err, errlen, "libpq: missing %s", name); return false; } } while (0)
    LOAD(connectdb, "PQconnectdb"); LOAD(status, "PQstatus"); LOAD(errorMessage, "PQerrorMessage"); LOAD(finish, "PQfinish");
    LOAD(exec, "PQexec"); LOAD(resultStatus, "PQresultStatus"); LOAD(resultErrorMessage, "PQresultErrorMessage");
    LOAD(nfields, "PQnfields"); LOAD(ntuples, "PQntuples"); LOAD(fname, "PQfname"); LOAD(getvalue, "PQgetvalue");
    LOAD(getisnull, "PQgetisnull"); LOAD(cmdTuples, "PQcmdTuples"); LOAD(clear, "PQclear");
    LOAD(sendQuery, "PQsendQuery"); LOAD(setSingleRowMode, "PQsetSingleRowMode"); LOAD(getResult, "PQgetResult");
    LOAD(ftype, "PQftype"); LOAD(getlength, "PQgetlength");
    LOAD(getCancel, "PQgetCancel"); LOAD(cancel, "PQcancel"); LOAD(freeCancel, "PQfreeCancel");
    LOAD(resultErrorField, "PQresultErrorField"); LOAD(setNoticeReceiver, "PQsetNoticeReceiver"); LOAD(transactionStatus, "PQtransactionStatus");
#undef LOAD
    return true;
}

/* Strips the trailing newlines / spaces libpq puts at the end of its messages. */
static void trim_msg(char *s) { size_t n = strlen(s); while (n && (s[n - 1] == '\n' || s[n - 1] == ' ')) s[--n] = 0; }

/* server NOTICE / WARNING messages arrive here during exec; the executor turns them into
 * database_message events (OUT-013). Capped: the rest is counted, not kept. */
static void pg_notice(void *arg, const PGresult *res) {
    struct PgConn *pc = arg;
    const char *m = pq.resultErrorField(res, 'M'), *sev = pq.resultErrorField(res, 'S');
    if (!m) return;
    size_t room = sizeof pc->notices - pc->nlen;
    if (room < 8) return;
    int n = snprintf(pc->notices + pc->nlen, room, "%s%s: %s\n", pc->nlen ? "" : "", sev ? sev : "NOTICE", m);
    pc->nlen += (size_t)n < room ? (size_t)n : room - 1;
}

/* error fields beyond the message (OUT-012): SQLSTATE, detail, hint, statement position */
static void pg_error_fields(const PGresult *r, ResultSet *rs) {
    const char *f;
    if ((f = pq.resultErrorField(r, 'C'))) snprintf(rs->err_state, sizeof rs->err_state, "%s", f);
    if ((f = pq.resultErrorField(r, 'D'))) snprintf(rs->err_detail, sizeof rs->err_detail, "%s", f);
    if ((f = pq.resultErrorField(r, 'H'))) snprintf(rs->err_hint, sizeof rs->err_hint, "%s", f);
    if ((f = pq.resultErrorField(r, 'P')) && atoi(f) > 0) rs->err_offset = atoi(f) - 1;   /* 1-based character position */
    if ((f = pq.resultErrorField(r, 'M'))) { snprintf(rs->error, sizeof rs->error, "%s", f); }
    if (rs->err_state[0] && strcmp(rs->err_state, "57014") == 0) rs->cancelled = true;      /* query_canceled */
}

/* tx_state: maps PQtransactionStatus to 0 (idle) / 1 (inside a transaction, failed or not)
 * / -1 (busy or unknown). */
static int pg_tx_state(Db *db) {
    int st = pq.transactionStatus(((PgConn *)db->handle)->c);   /* 0 idle, 1 active, 2 in transaction, 3 in failed transaction, 4 unknown */
    return st == 0 ? 0 : st == 2 || st == 3 ? 1 : -1;
}

/* cancel: asks the server to cancel the running statement. Called from the UI thread while
 * the worker is inside exec / fetch. */
static void pg_cancel(Db *db) {
    PgConn *pc = db->handle;
    char buf[256];
    if (pc->cancel) pq.cancel(pc->cancel, buf, sizeof buf);
}

/* open: `location` is a libpq conninfo string (built by conn.c). Blocks until connected or
 * failed (connect_timeout in the conninfo bounds it). Installs the notice receiver. */
static bool pg_open(Db *db, const char *location, char *err, size_t errlen) {
    if (!pq_load(err, errlen)) return false;
    PGconn *c = pq.connectdb(location);
    if (!c || pq.status(c) != CONNECTION_OK) {
        snprintf(err, errlen, "%s", c ? pq.errorMessage(c) : "out of memory");
        trim_msg(err);
        if (c) pq.finish(c);
        return false;
    }
    PgConn *pc = calloc(1, sizeof *pc);
    pq.setNoticeReceiver(c, pg_notice, pc);
    pc->cancel = pq.getCancel(c);
    pc->c = c;
    db->handle = pc;
    return true;
}

/* forward declaration: pg_close and pg_drain release an open cursor */
static void pg_release(Db *db, ResultSet *rs);

/* close: frees the cancel object, stops any open cursor and closes the connection. */
static void pg_close(Db *db) {
    PgConn *pc = db->handle;
    if (!pc) return;
    if (pc->cancel) { pq.freeCancel(pc->cancel); pc->cancel = NULL; }
    if (pc->active) pg_release(db, pc->active);
    pq.finish(pc->c);
    free(pc);
}

/* The PGconn of a Db. */
static PGconn *conn_of(Db *db) { return ((PgConn *)db->handle)->c; }

/* Drops the streaming cursor so the connection can take another command. */
static void pg_drain(Db *db) {
    PgConn *pc = db->handle;
    if (!pc->active) return;
    ResultSet *owner = pc->active;
    pg_release(db, owner);
    owner->cursor_lost = true;
}

/* Runs one query expecting rows; returns the result or NULL with err filled. */
static PGresult *pg_query(Db *db, const char *sql, char *err, size_t errlen) {
    pg_drain(db);
    PGresult *r = pq.exec(conn_of(db), sql);
    if (!r || pq.resultStatus(r) != PGRES_TUPLES_OK) {
        snprintf(err, errlen, "%s", r ? pq.resultErrorMessage(r) : pq.errorMessage(conn_of(db)));
        trim_msg(err);
        if (r) pq.clear(r);
        return NULL;
    }
    return r;
}

/* list_tables: every user table and view, "public" first, then the current schema, then
 * the others alphabetically. Non-public names are "schema.table". */
static bool pg_list_tables(Db *db, DbTable **out, int *count, char *err, size_t errlen) {
    const char *sql =
        "SELECT table_schema, table_name, table_type FROM information_schema.tables "
        "WHERE table_schema NOT IN ('pg_catalog','information_schema') AND table_schema NOT LIKE 'pg_toast%' "
        "ORDER BY (table_schema <> 'public'), (table_schema <> current_schema()), table_schema, lower(table_name)";
    PGresult *r = pg_query(db, sql, err, errlen);
    if (!r) return false;
    int n = pq.ntuples(r);
    DbTable *tables = n ? calloc(n, sizeof *tables) : NULL;
    for (int i = 0; i < n; i++) {
        const char *schema = pq.getvalue(r, i, 0), *name = pq.getvalue(r, i, 1), *type = pq.getvalue(r, i, 2);
        char full[512];
        if (strcmp(schema, "public") == 0) snprintf(full, sizeof full, "%s", name);
        else snprintf(full, sizeof full, "%s.%s", schema, name);
        tables[i].name = db_strdup(full);
        tables[i].is_view = strcmp(type, "VIEW") == 0;
    }
    pq.clear(r);
    *out = tables; *count = n;
    return true;
}

/* Splits "schema.table" at the first dot; a bare name is in "public". */
static void split_name(const char *full, char *schema, size_t sn, char *name, size_t nn) {
    const char *dot = strchr(full, '.');
    if (dot) { snprintf(schema, sn, "%.*s", (int)(dot - full), full); snprintf(name, nn, "%s", dot + 1); }
    else { snprintf(schema, sn, "public"); snprintf(name, nn, "%s", full); }
}

/* Writes s as a single-quoted SQL literal with quotes doubled, truncating to fit n. Used to
 * embed catalog names in the queries below (no bind parameters in this minimal API). */
static void sql_literal(const char *s, char *out, size_t n) {
    size_t p = 0;
    out[p++] = '\'';
    for (; *s && p < n - 3; s++) { if (*s == '\'') out[p++] = '\''; out[p++] = *s; }
    out[p++] = '\''; out[p] = 0;
}

/* list_columns: from pg_attribute, in column order, with not-null, primary-key membership,
 * generated (attgenerated) and GENERATED ALWAYS AS IDENTITY flags. */
static bool pg_list_columns(Db *db, DbTable *t, char *err, size_t errlen) {
    char schema[256], name[256], ls[520], ln[520], sql[2048];
    split_name(t->name, schema, sizeof schema, name, sizeof name);
    sql_literal(schema, ls, sizeof ls);
    sql_literal(name, ln, sizeof ln);
    snprintf(sql, sizeof sql,
        "SELECT a.attname, format_type(a.atttypid, a.atttypmod), a.attnotnull, "
        "  EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = a.attrelid AND i.indisprimary AND a.attnum = ANY(i.indkey)), "
        "  a.attgenerated <> '', a.attidentity = 'a' "
        "FROM pg_attribute a JOIN pg_class c ON c.oid = a.attrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
        "WHERE n.nspname = %s AND c.relname = %s AND a.attnum > 0 AND NOT a.attisdropped ORDER BY a.attnum", ls, ln);
    PGresult *r = pg_query(db, sql, err, errlen);
    if (!r) return false;
    int n = pq.ntuples(r);
    t->cols = n ? calloc(n, sizeof *t->cols) : NULL;
    for (int i = 0; i < n; i++) {
        t->cols[i].name = db_strdup(pq.getvalue(r, i, 0));
        t->cols[i].type = db_strdup(pq.getvalue(r, i, 1));
        t->cols[i].notnull = pq.getvalue(r, i, 2)[0] == 't';
        t->cols[i].pk = pq.getvalue(r, i, 3)[0] == 't';
        t->cols[i].generated = pq.getvalue(r, i, 4)[0] == 't';
        t->cols[i].identity_always = pq.getvalue(r, i, 5)[0] == 't';
    }
    t->ncols = n;
    t->cols_loaded = true;
    pq.clear(r);
    return true;
}

/* list_fks: one DbFk per column pair of every foreign key of `table`, named after the
 * constraint. The caller frees the array with db_fks_free. */
static bool pg_list_fks(Db *db, const char *table, DbFk **out, int *count, char *err, size_t errlen) {
    char schema[256], name[256], ls[520], ln[520], sql[2048];
    split_name(table, schema, sizeof schema, name, sizeof name);
    sql_literal(schema, ls, sizeof ls);
    sql_literal(name, ln, sizeof ln);
    /* pg_catalog, not information_schema: constraint_column_usage only shows tables owned by
     * the current role, so for an ordinary application user no foreign key was ever listed.
     * unnest(conkey, confkey) WITH ORDINALITY pairs the columns of a composite key in order
     * (joining the two information_schema views by constraint name crossed them). The
     * referenced table is listed even when this user cannot read it: following the link then
     * fails with the server's permission error, which says more than a missing menu item. */
    snprintf(sql, sizeof sql,
        "SELECT a.attname, nf.nspname, cf.relname, af.attname, c.conname "
        "FROM pg_constraint c "
        "JOIN pg_class cl ON cl.oid = c.conrelid JOIN pg_namespace n ON n.oid = cl.relnamespace "
        "JOIN pg_class cf ON cf.oid = c.confrelid JOIN pg_namespace nf ON nf.oid = cf.relnamespace "
        "CROSS JOIN LATERAL unnest(c.conkey, c.confkey) WITH ORDINALITY AS k(col, fcol, ord) "
        "JOIN pg_attribute a ON a.attrelid = c.conrelid AND a.attnum = k.col "
        "JOIN pg_attribute af ON af.attrelid = c.confrelid AND af.attnum = k.fcol "
        "WHERE c.contype = 'f' AND n.nspname = %s AND cl.relname = %s ORDER BY c.conname, k.ord", ls, ln);
    PGresult *r = pg_query(db, sql, err, errlen);
    if (!r) return false;
    int n = pq.ntuples(r);
    DbFk *fks = n ? calloc(n, sizeof *fks) : NULL;
    for (int i = 0; i < n; i++) {
        char full[520];
        const char *ts = pq.getvalue(r, i, 1), *tn = pq.getvalue(r, i, 2);
        if (strcmp(ts, "public") == 0) snprintf(full, sizeof full, "%s", tn); else snprintf(full, sizeof full, "%s.%s", ts, tn);
        fks[i].from_col = db_strdup(pq.getvalue(r, i, 0));
        fks[i].to_table = db_strdup(full);
        fks[i].to_col = db_strdup(pq.getvalue(r, i, 3));
        fks[i].name = db_strdup(pq.getvalue(r, i, 4));
    }
    pq.clear(r);
    *out = fks; *count = n;
    return true;
}

/* Type name and display kind (CK_*) for the built-in type OIDs we recognise; "" and
 * CK_OTHER for anything else (the caller then shows "oid N"). */
static const char *oid_name(unsigned oid, int *kind) {
    switch (oid) {
    case 16: *kind = CK_BOOL; return "bool";
    case 17: *kind = CK_BINARY; return "bytea";
    case 18: *kind = CK_TEXT; return "char";
    case 19: *kind = CK_TEXT; return "name";
    case 20: *kind = CK_NUMBER; return "int8";
    case 21: *kind = CK_NUMBER; return "int2";
    case 23: *kind = CK_NUMBER; return "int4";
    case 25: *kind = CK_TEXT; return "text";
    case 26: *kind = CK_NUMBER; return "oid";
    case 114: *kind = CK_JSON; return "json";
    case 700: *kind = CK_NUMBER; return "float4";
    case 701: *kind = CK_NUMBER; return "float8";
    case 790: *kind = CK_NUMBER; return "money";
    case 1042: *kind = CK_TEXT; return "bpchar";
    case 1043: *kind = CK_TEXT; return "varchar";
    case 1082: *kind = CK_DATE; return "date";
    case 1083: *kind = CK_DATE; return "time";
    case 1114: *kind = CK_DATE; return "timestamp";
    case 1184: *kind = CK_DATE; return "timestamptz";
    case 1186: *kind = CK_DATE; return "interval";
    case 1700: *kind = CK_NUMBER; return "numeric";
    case 2950: *kind = CK_TEXT; return "uuid";
    case 3802: *kind = CK_JSON; return "jsonb";
    default: *kind = CK_OTHER; return "";
    }
}

/* Adds the result's columns (name, type name, kind) to the ResultSet. */
static void pg_define_columns(ResultSet *rs, PGresult *r) {
    int nc = pq.nfields(r);
    for (int c = 0; c < nc; c++) {
        int kind;
        char tname[32];
        const char *name = oid_name(pq.ftype(r, c), &kind);
        if (!*name) { snprintf(tname, sizeof tname, "oid %u", pq.ftype(r, c)); name = tname; }
        rs_add_column(rs, pq.fname(r, c), name, kind);
    }
}

/* Appends the single row of a PGRES_SINGLE_TUPLE result. Silently drops it when the
 * ResultSet refuses more rows (row cap reached). */
static void pg_add_tuple(ResultSet *rs, PGresult *r) {
    int row = rs_add_row(rs);
    if (row < 0) return;
    for (int c = 0; c < rs->ncols; c++)
        rs_set(rs, row, c, pq.getisnull(r, 0, c) ? NULL : pq.getvalue(r, 0, c), pq.getlength(r, 0, c));
}

/* Pulls results from a running query. Stops after `limit` rows of the current row-returning
 * statement when limit > 0 (leaving the connection busy = the cursor), or when the query is done.
 * Returns true while the cursor is still open. */
static bool pg_pull(Db *db, ResultSet *rs, int limit) {
    PGconn *c = conn_of(db);
    int got = 0;
    PGresult *r;
    /* one PGresult per row (single-row mode) or per statement end; NULL when the query is done */
    while ((r = pq.getResult(c)) != NULL) {
        int st = pq.resultStatus(r);
        if (st == PGRES_SINGLE_TUPLE) {
            if (!rs->has_rows) {
                /* first row of a row-returning statement: it replaces earlier rows */
                Db *keep = rs->db; bool ok = rs->ok; int affected = rs->affected;
                rs_free(rs); rs_init(rs);
                rs->db = keep; rs->ok = ok; rs->affected = affected;
                pg_define_columns(rs, r);
                rs->has_rows = true;
            }
            pg_add_tuple(rs, r);
            pq.clear(r);
            if (limit > 0 && ++got >= limit) return true;
        } else if (st == PGRES_TUPLES_OK) {
            if (!rs->has_rows) {          /* zero-row result: still define the columns */
                Db *keep = rs->db; bool ok = rs->ok; int affected = rs->affected;
                rs_free(rs); rs_init(rs);
                rs->db = keep; rs->ok = ok; rs->affected = affected;
                pg_define_columns(rs, r);
                rs->has_rows = true;
            }
            rs->complete = true;          /* this statement is exhausted; a later one may replace it */
            pq.clear(r);
            got = 0;
            /* a following row-returning statement starts a fresh set */
            rs->has_rows = rs->has_rows;  /* kept until a new SINGLE_TUPLE arrives */
        } else if (st == PGRES_COMMAND_OK || st == PGRES_EMPTY_QUERY) {
            const char *n = pq.cmdTuples(r);
            rs->affected += n && *n ? atoi(n) : 0;
            pq.clear(r);
        } else {
            snprintf(rs->error, sizeof rs->error, "%s", pq.resultErrorMessage(r));
            trim_msg(rs->error);
            pg_error_fields(r, rs);
            rs->ok = false;
            pq.clear(r);
        }
    }
    return false;
}

/* release: abandons the rest of a streaming result. libpq has no "close cursor", so this
 * cancels the query and discards whatever is still in flight. */
static void pg_release(Db *db, ResultSet *rs) {
    PgConn *pc = db->handle;
    if (!rs->cursor) return;
    rs->cursor = NULL;
    if (pc->active == rs) pc->active = NULL;
    /* ask the server to stop, then drain whatever is already in flight */
    PGcancel *cn = pq.getCancel(pc->c);
    char buf[256];
    if (cn) { pq.cancel(cn, buf, sizeof buf); pq.freeCancel(cn); }
    PGresult *r;
    while ((r = pq.getResult(pc->c)) != NULL) pq.clear(r);
}

/* rehome: the ResultSet that owns the cursor was moved (rs.c); update the back pointer. */
static void pg_rehome(Db *db, ResultSet *rs) { ((PgConn *)db->handle)->active = rs; }

/* fetch: pulls up to n more rows from the streaming query; at the end the connection is
 * free again. Returns true when rows were added. Worker thread. */
static bool pg_fetch(Db *db, ResultSet *rs, int n) {
    if (!rs->cursor) return false;
    Uint64 t0 = SDL_GetPerformanceCounter();
    int before = rs->nrows;
    bool more = pg_pull(db, rs, n);
    rs->fetch_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (!more) { rs->cursor = NULL; ((PgConn *)db->handle)->active = NULL; rs->complete = rs->ok; rs->truncated = false; }
    else rs->truncated = true;
    return rs->nrows > before;
}

/* exec: sends the script (len bytes, or NUL-terminated when len < 0) as one simple-protocol
 * query in single-row mode and pulls up to `limit` rows. The script runs as libpq's simple
 * query: all statements in one implicit transaction unless the script manages its own.
 * Notices received are attached to rs->notices (heap copy). Worker thread. */
static void pg_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs) {
    PgConn *pc = db->handle;
    pg_drain(db);
    rs_init(rs);
    rs->db = db;
    rs->ok = true;
    pc->notices[0] = 0; pc->nlen = 0;
    if (len < 0) len = (int)strlen(sql);
    char *text = malloc(len + 1);
    memcpy(text, sql, len);
    text[len] = 0;
    Uint64 t0 = SDL_GetPerformanceCounter();
    if (!pq.sendQuery(pc->c, text)) {
        snprintf(rs->error, sizeof rs->error, "%s", pq.errorMessage(pc->c));
        trim_msg(rs->error);
        rs->ok = false;
        free(text);
        return;
    }
    free(text);
    pq.setSingleRowMode(pc->c);
    bool more = pg_pull(db, rs, limit);
    double total = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (more) {
        rs->cursor = pc;
        pc->active = rs;
        rs->truncated = true;
        rs->complete = false;
    } else rs->complete = rs->ok;
    /* the server executes and streams in one go; count time to the first batch as execution */
    rs->exec_ms = total;
    rs->fetch_ms = 0;
    if (pc->nlen) { rs->notices = db_strdup(pc->notices); pc->notices[0] = 0; pc->nlen = 0; }
}

/* select_sql: SELECT * ... [WHERE ...] [ORDER BY ...] LIMIT n. */
static void pg_select(const char *q, const char *where, const char *order, int limit, char *buf, size_t n) {
    snprintf(buf, n, "SELECT * FROM %s%s%s%s%s LIMIT %d;", q, where && *where ? " WHERE " : "", where ? where : "",
             order && *order ? " ORDER BY " : "", order ? order : "", limit);
}

/* Driver table, in struct DbDriver order. Identifiers use ANSI double quoting split on dots
 * ("schema"."table"); no set_autocommit (BEGIN handles transactions). */
/* libpq loads (and its version, from PQlibVersion: 160002 -> "16.2"). Never connects. */
static bool pg_probe(char *v, size_t n) {
    char err[256];
    if (!pq_load(err, sizeof err)) { snprintf(v, n, "client library not found"); return false; }
    int (*libver)(void) = (int (*)(void))DLSYM(pq.lib, "PQlibVersion");
    int x = libver ? libver() : 0;
    if (x >= 100000) snprintf(v, n, "libpq %d.%d", x / 10000, x % 10000);
    else if (x) snprintf(v, n, "libpq %d.%d.%d", x / 10000, x / 100 % 100, x % 100);
    else snprintf(v, n, "libpq (version not reported)");
    return true;
}

const DbDriver db_pg_driver = {
    "PostgreSQL", pg_open, pg_close, pg_list_tables, pg_list_columns, pg_exec, pg_fetch, pg_release, pg_rehome, db_quote_ident_double, pg_select,
    DB_CAP_FETCH_MORE | DB_CAP_CANCEL | DB_CAP_SERVER_SORT | DB_CAP_SERVER_FILTER, "BEGIN", pg_list_fks, NULL, pg_cancel, pg_tx_state, pg_probe,
};
