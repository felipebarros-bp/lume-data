/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
#include "db.h"

/*
 * Saved connections and the driver registry
 * =========================================
 *
 * A Connection is what the user sees in the explorer: a name, a driver id and the driver's
 * field values (host, port, ...), plus runtime state once it is open (the Db handle and the
 * table list). The ConnStore owns every Connection and persists them in the preferences
 * folder: connections.txt (everything but passwords) and secrets.txt (passwords only, file
 * mode 0600).
 *
 * The driver registry (DriverInfo) describes, per database engine, which fields the
 * connection dialog shows and how they are turned into the location string that the
 * DbDriver's open() expects.
 *
 * Threading: all of this runs on the UI thread. The Db handle inside a Connection is lent to
 * the executor (exec.h) for statement runs.
 */
/* Saved connections and the driver registry. See docs/connections.md. */

/* How the connection dialog edits a field. FIELD_PASSWORD values are masked on screen and
 * stored in secrets.txt; FIELD_FILE gets a file picker. */
typedef enum { FIELD_TEXT, FIELD_INT, FIELD_PASSWORD, FIELD_FILE } FieldType;
/* One field of a driver: storage key, dialog label, editor type and default value. */
typedef struct FieldSpec { const char *key, *label; FieldType type; const char *def; } FieldSpec;

/* Limits of the fixed-size value table inside Connection (fields per driver, bytes per value). */
#define CONN_MAX_FIELDS 8
#define CONN_VALUE_LEN 256

typedef struct Connection Connection;

/* Static description of a database engine (see the table in conn.c). */
typedef struct DriverInfo {
    const char *id, *name;   /* id stored in connections.txt ("postgres"), display name ("PostgreSQL") */
    uint32_t color;   /* 0xRRGGBB accent used for the connection icon */
    const FieldSpec *fields; int nfields;   /* fields shown by the dialog, in order */
    const DbDriver *drv;   /* the driver implementation (db.h) */
    /* Builds the location string the driver's open() takes. */
    void (*location)(const Connection *c, char *buf, size_t n);
    /* how the New Connection picker shows it: group, a 2-letter badge, words it is also found by */
    const char *category, *abbr, *aliases;
} DriverInfo;

/* A saved connection. Owned by the ConnStore; heap allocated so pointers stay stable while
 * the store's array grows. */
struct Connection {
    char id[12];   /* random stable id, links secrets.txt entries */
    char name[64];   /* user-visible name */
    char driver[16];   /* DriverInfo id */
    char values[CONN_MAX_FIELDS][CONN_VALUE_LEN];   /* indexed like the driver's fields[] */
    bool pw_in_file[CONN_MAX_FIELDS];   /* this password is in secrets.txt (the secret store did not take it) */
    int demo;   /* a bundled demo connection: 1 the classic demo, 2 the e-commerce shop (demo_shop.c) */
    /* metadata set by the user, never deduced from the host name (TBR-013 / 014) */
    int env;                    /* CONN_ENV_*: a label shown next to the connection and in write confirmations */
    uint32_t env_color;         /* 0xRRGGBB, 0 = the environment's default colour */
    bool read_only;             /* "read-only in Lume": enforced as far as the driver allows, see db_set_read_only */
    char ai_conn[12];           /* the connection the assistant's SQL runs on (a SELECT-only login), "" = this one (BL-051) */
    char folder[128];           /* Explorer folder, "/" between levels ("bepass/Palmeiras"); "" = top level */
    /* runtime */
    Db *db;   /* NULL while disconnected */
    DbTable *tables; int ntables;   /* catalog, owned; freed with db_tables_free */
    bool columns_lazy;      /* big schema: columns are loaded on demand */
    char error[256];   /* last connect / refresh error, "" when fine */
    /* connection life cycle (TBR-011 / 012 / 045): the connect runs on a worker thread */
    int state;                  /* CONN_* */
    unsigned generation;        /* +1 on every connect / disconnect: stale events compare it */
    struct ConnAttempt *attempt;   /* connect in progress, owned; NULL otherwise */
    Uint64 state_since;         /* SDL_GetTicks when the state last changed */
    bool tx_lost;               /* the last disconnect / reconnect dropped an open transaction */
    /* session settings, reset on every connect (they belong to the server session) */
    int ro_level;               /* RO_* obtained for read_only (caps.h) */
    char schema[64];            /* default schema chosen in Lume for this session; "" = the server's own (unknown) */
    bool manual_tx;             /* Tx: Manual (TBR-016): statements run inside BEGIN ... until Commit / Rollback */
    char isolation[24];         /* isolation level set for the next transactions, "" = server default */
};
/* Environment label of a connection (TBR-014). */
enum { CONN_ENV_NONE, CONN_ENV_DEV, CONN_ENV_HML, CONN_ENV_PROD, CONN_ENV_COUNT };
const char *conn_env_name(int env);                 /* "", "DEV", "STG", "PROD" (STG = staging; "HML" is still read) */
int conn_env_by_name(const char *name);             /* CONN_ENV_NONE when unknown */
uint32_t conn_env_color(const Connection *c);       /* configured colour, else the environment's default */
/* Life cycle of a saved connection, as shown by the toolbar (with text, never only a colour). */
enum { CONN_DISCONNECTED, CONN_CONNECTING, CONN_CONNECTED, CONN_FAILED, CONN_RECONNECTING };
const char *conn_state_name(int state);   /* "disconnected", "connecting", ... */

/* Every saved connection, in display order. `dir` is the preferences folder, with a trailing
 * slash ("" when none could be resolved: nothing is saved then). */
typedef struct ConnStore {
    Connection **items; int n, cap;
    char dir[1024];
} ConnStore;

/* The registry: returns the static array and stores its length in *count. */
const DriverInfo *driver_infos(int *count);
/* Driver by id ("sqlite", "postgres", "oracle"), NULL when unknown. */
const DriverInfo *driver_info(const char *id);
/* Index of a field key in d->fields, -1 when the driver has no such field. */
int driver_field_index(const DriverInfo *d, const char *key);
/* Value of a field by key. Points into the Connection; do not free. */
const char *conn_value(const Connection *c, const char *key);   /* "" when absent */
/* Sets a field by key (truncated to CONN_VALUE_LEN - 1; NULL means ""). Unknown keys are
 * ignored. Does not save. */
void conn_set_value(Connection *c, const char *key, const char *value);
/* The connection's driver, NULL when its driver id is unknown. */
const DriverInfo *conn_driver(const Connection *c);
/* Builds the driver-specific location string (conninfo, path, ...) into buf; "" when the
 * driver is unknown. The result can contain the password: never log it. */
void conn_location(const Connection *c, char *buf, size_t n);
/* Tidies a folder path in place: segments trimmed, empty ones dropped, joined by "/" ("" = top level). */
void conn_folder_normalize(char *path, size_t n);
/* One-line summary for the explorer detail, e.g. "localhost:15432/syn". */
void conn_summary(const Connection *c, char *buf, size_t n);

/* Reads connections.txt and secrets.txt from the preferences folder (LUME_PREF_DIR when set).
 * Always returns a store (possibly empty); free it with conns_free. */
ConnStore *conns_load(void);
/* Rewrites both files from the current store. Passwords only go to secrets.txt. */
void conns_save(ConnStore *s);
/* Disconnects and frees every connection, then the store. */
void conns_free(ConnStore *s);
Connection *conns_add(ConnStore *s, const char *driver_id);   /* new, defaults filled, not saved */
void conns_remove(ConnStore *s, Connection *c);                /* disconnects, frees, saves */
/* Connection by id, NULL when not found. */
Connection *conns_find(ConnStore *s, const char *id);

/* Opens the connection and loads table names (and columns when the schema is small).
 * On failure returns false and fills c->error. */
bool conn_connect(Connection *c);
/* Starts connecting on a worker thread (state CONNECTING, or RECONNECTING when `reconnect`).
 * The location (which may hold the password) is built here, on the calling thread, and never
 * logged. Returns false when it could not start (already connected / connecting, unknown
 * driver). conn_poll finishes it on the UI thread. */
bool conn_connect_async(Connection *c, bool reconnect);
/* UI thread, every frame: applies a finished connect (CONNECTED with its catalog, or FAILED
 * with c->error). Returns true on the frame the attempt ended. */
bool conn_poll(Connection *c);
/* Abandons a connect in progress: its result is closed when the worker returns; the
 * connection goes back to DISCONNECTED. */
void conn_cancel_connect(Connection *c);
/* Frees the table list and closes the Db. Safe on a closed connection. */
void conn_disconnect(Connection *c);
/* Called by conn_disconnect before the Db is closed (modules with jobs on it stop them). */
void conn_add_close_hook(void (*hook)(Connection *c));      /* called before a Db is closed (workers stop) */
void conn_remove_close_hook(void (*hook)(Connection *c));
bool conn_refresh(Connection *c);     /* reloads tables of an open connection */
/* Loads the columns of one table when they are not loaded yet (lazy schemas). False when
 * the connection is closed or the driver fails. Blocking call on the UI thread. */
bool conn_load_columns(Connection *c, DbTable *t);
/* Opens and closes immediately; for the dialog's Test button. */
bool conn_test(const Connection *c, char *err, size_t errlen);
