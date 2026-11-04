/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Database driver abstraction
 * ===========================
 *
 * Every database engine is a DbDriver: a table of function pointers (open, catalog queries,
 * exec / fetch of statements, quoting, cancel, ...). A Db is one open connection: the driver
 * plus the driver's private handle. Results come back in a ResultSet (rs.h).
 *
 * Drivers: db_sqlite_driver (db.c, linked in), db_pg_driver (db_pg.c, libpq loaded with
 * dlopen) and db_oracle_driver (db_oracle.c, OCI loaded with dlopen). The connection store
 * (conn.h) picks the driver and builds the location string; the executor (exec.h) calls
 * exec / fetch / cancel; the explorer and autocomplete use the catalog calls.
 *
 * Threading: a Db is not thread-safe. While a statement runs on the executor's worker thread
 * `busy` is set and only that thread may call the driver; the one exception is cancel(),
 * which is designed to be called from the UI thread during exec().
 *
 * Ownership: strings in DbTable / DbColumn / DbFk are heap allocated by the driver and freed
 * with db_tables_free / db_fks_free.
 */
#pragma once
#include "lume.h"

#include "rs.h"

/* generated: computed by the database (never editable); identity_always: GENERATED ALWAYS
 * AS IDENTITY (US-050) */
typedef struct DbColumn { char *name; char *type; bool pk; bool notnull; bool generated, identity_always; } DbColumn;
/* A table or view of the catalog. Columns may be loaded lazily (cols_loaded false). */
typedef struct DbTable {
    char *name;   /* schema-qualified when the driver lists several schemas */
    bool is_view;
    DbColumn *cols; int ncols;   /* NULL / 0 until list_columns ran */
    bool cols_loaded;   /* list_columns has filled cols */
} DbTable;

/* one column of a foreign key; `name` groups the columns of a composite key (US-058) */
typedef struct DbFk { char *name; char *from_col; char *to_table; char *to_col; } DbFk;

typedef struct DbDriver DbDriver;

/* One open connection. Created by db_open, destroyed by db_close. */
struct Db {
    const DbDriver *drv;
    void *handle;   /* driver-private state (sqlite3 *, PgConn *, ...) */
    char *label;        /* short display name */
    /* location: the string given to db_open. For server drivers it is the full connection
     * string and includes the password, so it must never be logged or displayed. */
    char *location;     /* file path or ":memory:" */
    volatile int busy;  /* an executor thread owns the connection: the UI must not call the driver */
    volatile int abandoned;  /* a worker stuck in the driver (the network died under it) keeps the connection: db_close leaves it to the worker instead of calling the driver under it */
    volatile int cancels;    /* db_cancel calls in flight on their own threads (db_close waits for them) */
    bool want_last_result;   /* SQL Server: keep the LAST result set of a statement (STATISTICS XML puts the plan after the rows); set by the plan capture only */
};

/* The driver interface. Optional members may be NULL; callers check before calling. */
struct DbDriver {
    const char *name;   /* display name ("SQLite", "PostgreSQL", "Oracle") */
    /* Connects and stores the private handle in db->handle; on failure fills err and returns
     * false (the Db is then discarded by db_open). */
    bool (*open)(Db *db, const char *location, char *err, size_t errlen);
    /* Releases db->handle. */
    void (*close)(Db *db);
    /* Lists tables and views (without columns). *out is heap allocated; free with db_tables_free. */
    bool (*list_tables)(Db *db, DbTable **out, int *count, char *err, size_t errlen);
    /* Fills t->cols / t->ncols and sets t->cols_loaded. */
    bool (*list_columns)(Db *db, DbTable *t, char *err, size_t errlen);
    /* Executes sql (one or more statements). A row-returning statement fetches up to `limit`
     * rows (limit <= 0: all); when more rows may exist and the driver can continue, it leaves
     * rs->cursor open so fetch() can append more. */
    void (*exec)(Db *db, const char *sql, int len, int limit, ResultSet *rs);
    bool (*fetch)(Db *db, ResultSet *rs, int n);    /* appends up to n rows; false at the end */
    void (*release)(Db *db, ResultSet *rs);         /* closes rs->cursor */
    void (*rehome)(Db *db, ResultSet *rs);          /* optional: rs moved to a new address */
    /* Writes a quoted identifier into buf. */
    void (*quote_ident)(const char *ident, char *buf, size_t buflen);
    /* Writes a "rows of this table" statement for the quoted identifier: optional WHERE
     * condition and ORDER BY list (already quoted/escaped by the caller), first `limit` rows.
     * Used for table previews and for server-side sort / filter (US-012). */
    void (*select_sql)(const char *qident, const char *where, const char *order_by, int limit, char *buf, size_t buflen);
    unsigned caps;                                   /* DB_CAP_* */
    /* Statement that opens an explicit transaction (NULL when the driver has none, e.g.
     * Oracle, where DML is implicitly in one until COMMIT / ROLLBACK). */
    const char *tx_begin;
    /* Foreign keys of a table (US-036); optional. Caller frees with db_fks_free. */
    bool (*list_fks)(Db *db, const char *table, DbFk **out, int *count, char *err, size_t errlen);
    /* Optional: drivers that commit every DML on their own (Oracle's OCI_COMMIT_ON_SUCCESS)
     * switch that off while a batch runs inside one transaction (US-049). */
    void (*set_autocommit)(Db *db, bool on);
    /* Optional: asks the server / library to stop the statement running on another thread
     * (PQcancel, sqlite3_interrupt, OCIBreak). Safe to call while exec() is in progress. */
    void (*cancel)(Db *db);
    /* Optional: transaction state as the library reports it (OUT-017): 0 none / autocommit,
     * 1 a transaction is open, -1 unknown. Never inferred from the SQL text. */
    int (*tx_state)(Db *db);
    /* Optional: whether the engine can be used in this process (its client library loads) and
     * the library version, for About / Copy Info (ABOUT-012). May load the library; never
     * connects. NULL = always available, version unknown. */
    bool (*probe)(char *version, size_t n);
};

/* DbDriver.caps bits: FETCH_MORE = fetch() can continue a truncated result; CANCEL = cancel()
 * works; SERVER_SORT / SERVER_FILTER = the result grid may re-query with select_sql instead of
 * sorting / filtering the loaded rows. */
enum { DB_CAP_FETCH_MORE = 1, DB_CAP_CANCEL = 2, DB_CAP_SERVER_SORT = 4, DB_CAP_SERVER_FILTER = 8 };

/* Shared helpers for drivers. */
/* Heap copy of s (NULL stays NULL); free with free(). */
char *db_strdup(const char *s);
/* ANSI double-quote quoting, splitting on dots so "schema.table" quotes each part. Embedded
 * quotes are doubled; the output is truncated to fit buflen. */
void db_quote_ident_double(const char *ident, char *buf, size_t buflen);   /* "a.b" -> "a"."b" */

/* The built-in SQLite driver (db.c). */
extern const DbDriver db_sqlite_driver;

/* Allocates a Db, calls drv->open and derives a default label from the location (file name
 * or "memory"). Returns NULL with err filled on failure. Close with db_close. */
Db *db_open(const DbDriver *drv, const char *location, char *err, size_t errlen);
/* Closes the driver connection and frees the Db. NULL is accepted. */
/* Closes the session and frees db. A server driver's close is a round trip (a cursor to drain, a
 * logoff), and when the network died under the session (a VPN dropped) nothing answers and
 * nothing resets the socket: the call returns only when TCP gives up, minutes later. So the close
 * runs on its own thread and the caller waits at most 5 s; past that the thread is left to finish
 * (and free db) on its own, and the caller goes on. Any driver, present or future, gets this.
 * The caller must make sure no statement is running on the Db (or Db.abandoned is set). */
void db_close(Db *db);
/* Asks the driver to stop the statement in flight, on its own thread: a cancel may itself be a
 * round trip or a second connection (PQcancel, KILL QUERY, ALTER SYSTEM CANCEL SQL, a TDS
 * attention waiting for its acknowledgement), and on a dead network any of them blocks. The
 * caller never waits. No-op for drivers without cancel. */
void db_cancel(Db *db);
/* Seconds a driver waits to reach its server (Settings > Query Execution > Timeouts, default 10).
 * PostgreSQL, MySQL and SQL Server honour it; Oracle uses its client's own setting. */
extern int db_connect_timeout_s;
/* Frees a table array returned by list_tables, including any loaded columns. */
void db_tables_free(DbTable *tables, int count);
/* Frees an array returned by list_fks. */
void db_fks_free(DbFk *fks, int count);
