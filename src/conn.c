/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Connection store and driver registry: see conn.h.
 *
 * Three parts:
 *   - driver registry: one static DriverInfo per engine (fields shown in the dialog and the
 *     function that turns those fields into the DbDriver's location string);
 *   - store: load / save of connections.txt (INI-like "[connection]" blocks of key=value)
 *     and secrets.txt (one "id<TAB>key<TAB>password" line per stored password, mode 0600);
 *   - lifecycle: connect, refresh the catalog, lazy column loading, disconnect.
 *
 * Ownership: the ConnStore owns its Connection structs (each one calloc'd); a Connection
 * owns its Db and its DbTable array. Called from the UI thread (main.c, explorer.c,
 * dialog.c, results.c); the catalog calls block it for their duration.
 */
#include "conn.h"
#include "log.h"
#include <strings.h>
#include <sys/stat.h>
#ifndef _WIN32
#include <fcntl.h>
#include <unistd.h>
#endif

/* ---- driver registry --------------------------------------------------- */

/* Per-driver field lists, in dialog order; keys are also the connections.txt keys. */
static const FieldSpec sqlite_fields[] = {
    { "path", "File", FIELD_FILE, "" },
};
static const FieldSpec pg_fields[] = {
    { "host", "Host", FIELD_TEXT, "localhost" },
    { "port", "Port", FIELD_INT, "5432" },
    { "database", "Database", FIELD_TEXT, "postgres" },
    { "user", "User", FIELD_TEXT, "postgres" },
    { "password", "Password", FIELD_PASSWORD, "" },
};
static const FieldSpec oracle_fields[] = {
    { "host", "Host", FIELD_TEXT, "localhost" },
    { "port", "Port", FIELD_INT, "1521" },
    { "service", "Service name", FIELD_TEXT, "FREEPDB1" },
    { "user", "User", FIELD_TEXT, "" },
    { "password", "Password", FIELD_PASSWORD, "" },
};

static const FieldSpec mysql_fields[] = {
    { "host", "Host", FIELD_TEXT, "127.0.0.1" },
    { "port", "Port", FIELD_INT, "3306" },
    { "database", "Database", FIELD_TEXT, "" },
    { "user", "User", FIELD_TEXT, "root" },
    { "password", "Password", FIELD_PASSWORD, "" },
};

/* SQLite location: the database file path as typed (":memory:" for an in-memory db). */
static void sqlite_location(const Connection *c, char *buf, size_t n) { snprintf(buf, n, "%s", conn_value(c, "path")); }

/* PostgreSQL location: a libpq conninfo string ("host='h' port='5432' ..."), with quotes and
 * backslashes escaped, plus fixed options (UTF-8, application name, 10 s connect timeout).
 * Empty fields are left out so libpq applies its own defaults. */
static void pg_location(const Connection *c, char *buf, size_t n) {
    /* libpq conninfo; values with spaces or quotes are quoted */
    size_t p = 0;
    const char *keys[] = { "host", "port", "dbname", "user", "password" };
    const char *src[] = { "host", "port", "database", "user", "password" };
    for (int i = 0; i < 5; i++) {
        const char *v = conn_value(c, src[i]);
        if (!*v) continue;
        /* snprintf returns the untruncated length: clamp p so n - p never wraps around */
        p += snprintf(buf + p, n - p, "%s='", keys[i]); if (p >= n) p = n - 1;
        for (const char *q = v; *q && p < n - 3; q++) { if (*q == '\'' || *q == '\\') buf[p++] = '\\'; buf[p++] = *q; }
        p += snprintf(buf + p, n - p, "' "); if (p >= n) p = n - 1;
    }
    snprintf(buf + p, n - p, "client_encoding='UTF8' application_name='Lume' connect_timeout='10'");
}

/* Oracle location: "user\npassword\n//host:port/service", the three-line format that
 * db_oracle.c splits back apart (EZConnect string on the last line). */
static void oracle_location(const Connection *c, char *buf, size_t n) {
    snprintf(buf, n, "%s\n%s\n//%s:%s/%s", conn_value(c, "user"), conn_value(c, "password"),
             conn_value(c, "host"), conn_value(c, "port"), conn_value(c, "service"));
}

/* MySQL / MariaDB location: "host\nport\nuser\npassword\ndatabase" (db_mysql.c splits it). */
static void mysql_location(const Connection *c, char *buf, size_t n) {
    snprintf(buf, n, "%s\n%s\n%s\n%s\n%s", conn_value(c, "host"), conn_value(c, "port"), conn_value(c, "user"), conn_value(c, "password"), conn_value(c, "database"));
}

extern const DbDriver db_mysql_driver;
extern const DbDriver db_mssql_driver;
static const FieldSpec mssql_fields[] = {
    { "host", "Host", FIELD_TEXT, "127.0.0.1" },
    { "port", "Port", FIELD_INT, "1433" },
    { "database", "Database", FIELD_TEXT, "master" },
    { "user", "User", FIELD_TEXT, "sa" },
    { "password", "Password", FIELD_PASSWORD, "" },
};
extern const DbDriver db_pg_driver;
extern const DbDriver db_oracle_driver;

/* The registry itself; N() is the element count of a static array. */
#define N(a) ((int)(sizeof(a) / sizeof(a[0])))
static const DriverInfo drivers[] = {
    { "sqlite", "SQLite", 0x5B8DEF, sqlite_fields, N(sqlite_fields), &db_sqlite_driver, sqlite_location, "Embedded / file", "SQ", "sqlite3 file local embedded" },
    { "postgres", "PostgreSQL", 0x6FA8DC, pg_fields, N(pg_fields), &db_pg_driver, pg_location, "Relational", "PG", "postgres pg psql postgresql" },
    { "oracle", "Oracle", 0xE8795A, oracle_fields, N(oracle_fields), &db_oracle_driver, oracle_location, "Relational", "OR", "oracle ora plsql" },
    { "mysql", "MySQL", 0x4479A1, mysql_fields, N(mysql_fields), &db_mysql_driver, mysql_location, "Relational", "MY", "mysql my" },
    { "mariadb", "MariaDB", 0xC0765A, mysql_fields, N(mysql_fields), &db_mysql_driver, mysql_location, "Relational", "MA", "mariadb maria" },
    { "sqlserver", "SQL Server", 0xCC2927, mssql_fields, N(mssql_fields), &db_mssql_driver, mysql_location, "Relational", "SS", "mssql sqlserver tsql microsoft azure sql" },   /* same "host\nport\nuser\npassword\ndatabase" layout */
};

/* See conn.h. */
const DriverInfo *driver_infos(int *count) { *count = N(drivers); return drivers; }

/* Linear lookup by id; the registry has three entries. */
const DriverInfo *driver_info(const char *id) {
    for (int i = 0; i < N(drivers); i++) if (strcmp(drivers[i].id, id) == 0) return &drivers[i];
    return NULL;
}

/* Position of `key` in the driver's field list, -1 when absent. */
int driver_field_index(const DriverInfo *d, const char *key) {
    for (int i = 0; i < d->nfields; i++) if (strcmp(d->fields[i].key, key) == 0) return i;
    return -1;
}

/* Registry entry for this connection's driver id. */
const DriverInfo *conn_driver(const Connection *c) { return driver_info(c->driver); }

/* Field value by key; "" (a static literal) when the key or the driver is unknown. */
const char *conn_value(const Connection *c, const char *key) {
    const DriverInfo *d = conn_driver(c);
    int i = d ? driver_field_index(d, key) : -1;
    return i >= 0 ? c->values[i] : "";
}

/* Field setter by key; silently ignores keys the driver does not declare. */
void conn_set_value(Connection *c, const char *key, const char *value) {
    const DriverInfo *d = conn_driver(c);
    int i = d ? driver_field_index(d, key) : -1;
    if (i >= 0) snprintf(c->values[i], CONN_VALUE_LEN, "%s", value ? value : "");
}

/* Dispatches to the driver's location builder. */
void conn_location(const Connection *c, char *buf, size_t n) {
    const DriverInfo *d = conn_driver(c);
    if (d) d->location(c, buf, n); else buf[0] = 0;
}

/* SQLite shows the file name (or "in-memory"); server drivers show host:port/database,
 * using the Oracle service name when there is no database field. */
void conn_summary(const Connection *c, char *buf, size_t n) {
    if (strcmp(c->driver, "sqlite") == 0) {
        const char *p = conn_value(c, "path");
        const char *base = strrchr(p, '/');
        snprintf(buf, n, "%s", strcmp(p, ":memory:") == 0 ? "in-memory" : base ? base + 1 : p);
    } else {
        const char *dbn = *conn_value(c, "database") ? conn_value(c, "database") : conn_value(c, "service");
        snprintf(buf, n, "%s:%s/%s", conn_value(c, "host"), conn_value(c, "port"), dbn);
    }
}

/* ---- store ------------------------------------------------------------- */

/* Fills out[0..n-2] with random lowercase letters and digits and terminates it. Not
 * cryptographic; only needs to be unique among the user's connections. */
static void random_id(char *out, size_t n) {
    static const char *alpha = "abcdefghijklmnopqrstuvwxyz0123456789";
    for (size_t i = 0; i + 1 < n; i++) out[i] = alpha[SDL_rand(36)];
    out[n - 1] = 0;
}

/* Full path of a file in the store's folder. */
static void store_path(ConnStore *s, const char *file, char *buf, size_t n) { snprintf(buf, n, "%s%s", s->dir, file); }

/* Appends a new zeroed Connection to the store (growing the pointer array) and returns it. */
static Connection *store_push(ConnStore *s) {
    if (s->n == s->cap) { s->cap = s->cap ? s->cap * 2 : 8; s->items = realloc(s->items, sizeof *s->items * s->cap); }
    Connection *c = calloc(1, sizeof *c);
    s->items[s->n++] = c;
    return c;
}

/* Reads secrets.txt and puts each password back into its connection. Lines for unknown ids
 * or malformed lines are skipped. A missing file is not an error. */
static void load_secrets(ConnStore *s) {
    char path[1200];
    store_path(s, "secrets.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return;
    char line[1024];
    while (fgets(line, sizeof line, f)) {
        char *t1 = strchr(line, '\t');
        if (!t1) continue;
        *t1++ = 0;
        char *t2 = strchr(t1, '\t');
        if (!t2) continue;
        *t2++ = 0;
        t2[strcspn(t2, "\r\n")] = 0;
        Connection *c = conns_find(s, line);
        if (c) conn_set_value(c, t1, t2);
    }
    fclose(f);
}

/* See conn.h. Parsing is tolerant: unknown keys are routed to conn_set_value (which ignores
 * them), comments and blank lines are skipped, and a "[connection]" header starts a new
 * entry defaulting to the sqlite driver. */
ConnStore *conns_load(void) {
    /* resolve the preferences folder */
    ConnStore *s = calloc(1, sizeof *s);
    const char *envdir = SDL_getenv("LUME_PREF_DIR");
    if (envdir && *envdir) {
        SDL_CreateDirectory(envdir);
        snprintf(s->dir, sizeof s->dir, "%s%s", envdir, envdir[strlen(envdir) - 1] == '/' ? "" : "/");
    } else {
        char *pref = SDL_GetPrefPath("lume", "Lume");
        if (pref) { snprintf(s->dir, sizeof s->dir, "%s", pref); SDL_free(pref); }
    }
    char path[1200];
    /* parse connections.txt */
    store_path(s, "connections.txt", path, sizeof path);
    FILE *f = fopen(path, "r");
    if (!f) return s;
    char line[1024];
    Connection *c = NULL;
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (strcmp(line, "[connection]") == 0) { c = store_push(s); snprintf(c->driver, sizeof c->driver, "sqlite"); continue; }
        if (!c || !line[0] || line[0] == '#') continue;
        char *eq = strchr(line, '=');
        if (!eq) continue;
        *eq++ = 0;
        if (strcmp(line, "id") == 0) snprintf(c->id, sizeof c->id, "%s", eq);
        else if (strcmp(line, "name") == 0) snprintf(c->name, sizeof c->name, "%s", eq);
        else if (strcmp(line, "driver") == 0) snprintf(c->driver, sizeof c->driver, "%s", eq);
        else if (strcmp(line, "demo") == 0) c->demo = atoi(eq);
        else if (strcmp(line, "env") == 0) c->env = conn_env_by_name(eq);
        else if (strcmp(line, "env_color") == 0) c->env_color = (uint32_t)strtoul(eq, NULL, 16) & 0xFFFFFF;
        else if (strcmp(line, "read_only") == 0) c->read_only = atoi(eq) != 0;
        else if (strcmp(line, "ai_conn") == 0) snprintf(c->ai_conn, sizeof c->ai_conn, "%s", eq);
        else conn_set_value(c, line, eq);
    }
    fclose(f);
    /* drop entries whose driver is unknown or id is missing */
    for (int i = 0; i < s->n; i++) {
        if (!conn_driver(s->items[i]) || !s->items[i]->id[0]) { free(s->items[i]); s->items[i] = s->items[--s->n]; i--; }
    }
    load_secrets(s);
    return s;
}

/* See conn.h. Both files are rewritten in place (not via a temporary file). secrets.txt is
 * created with mode 0600 on Unix; on Windows it relies on the profile folder's ACL. */
void conns_save(ConnStore *s) {
    if (!s->dir[0]) return;
    char path[1200], tmp[1200];
    store_path(s, "connections.txt", path, sizeof path);
    FILE *f = fopen(path, "w");
    if (f) {
        fprintf(f, "# Lume connections. Passwords live in secrets.txt (mode 0600), never here.\n");
        for (int i = 0; i < s->n; i++) {
            Connection *c = s->items[i];
            const DriverInfo *d = conn_driver(c);
            fprintf(f, "\n[connection]\nid=%s\nname=%s\ndriver=%s\n", c->id, c->name, c->driver);
            if (c->demo) fprintf(f, "demo=%d\n", c->demo);
            if (c->env) fprintf(f, "env=%s\n", conn_env_name(c->env));
            if (c->env_color) fprintf(f, "env_color=%06X\n", (unsigned)c->env_color);
            if (c->read_only) fprintf(f, "read_only=1\n");
            if (c->ai_conn[0]) fprintf(f, "ai_conn=%s\n", c->ai_conn);
            for (int k = 0; d && k < d->nfields; k++)
                if (d->fields[k].type != FIELD_PASSWORD) fprintf(f, "%s=%s\n", d->fields[k].key, c->values[k]);
        }
        fclose(f);
    }
    /* secrets.txt: only non-empty password fields */
    store_path(s, "secrets.txt", tmp, sizeof tmp);
#ifndef _WIN32
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);
    f = fd >= 0 ? fdopen(fd, "w") : NULL;
#else
    f = fopen(tmp, "w");
#endif
    if (f) {
        for (int i = 0; i < s->n; i++) {
            Connection *c = s->items[i];
            const DriverInfo *d = conn_driver(c);
            for (int k = 0; d && k < d->nfields; k++)
                if (d->fields[k].type == FIELD_PASSWORD && c->values[k][0]) fprintf(f, "%s\t%s\t%s\n", c->id, d->fields[k].key, c->values[k]);
        }
        fclose(f);
    }
}

/* See conn.h. */
void conns_free(ConnStore *s) {
    for (int i = 0; i < s->n; i++) { conn_cancel_connect(s->items[i]); conn_disconnect(s->items[i]); free(s->items[i]); }   /* a connect in progress is abandoned first */
    free(s->items);
    free(s);
}

/* Creates a connection with a fresh random id and the driver's default values. The caller
 * names it and calls conns_save when the dialog is confirmed. */
Connection *conns_add(ConnStore *s, const char *driver_id) {
    Connection *c = store_push(s);
    random_id(c->id, sizeof c->id);
    snprintf(c->driver, sizeof c->driver, "%s", driver_id);
    const DriverInfo *d = conn_driver(c);
    for (int k = 0; d && k < d->nfields; k++) snprintf(c->values[k], CONN_VALUE_LEN, "%s", d->fields[k].def);
    return c;
}

/* Removes `c` from the store keeping the order of the others, frees it and saves. `c` is
 * invalid after the call. */
void conns_remove(ConnStore *s, Connection *c) {
    for (int i = 0; i < s->n; i++) if (s->items[i] == c) {
        conn_cancel_connect(c);   /* its worker must not write into freed memory */
        conn_disconnect(c);
        free(c);
        memmove(&s->items[i], &s->items[i + 1], sizeof *s->items * (s->n - i - 1));
        s->n--;
        break;
    }
    conns_save(s);
}

/* Linear lookup by id. */
Connection *conns_find(ConnStore *s, const char *id) {
    for (int i = 0; i < s->n; i++) if (strcmp(s->items[i]->id, id) == 0) return s->items[i];
    return NULL;
}

/* ---- lifecycle --------------------------------------------------------- */

/* Schemas with more tables than this load columns on demand (explorer expand, autocomplete)
 * instead of all at connect time. */
#define LAZY_COLUMNS_ABOVE 500

/* Drops and reloads the table list of an open connection, then loads every table's columns
 * unless the schema is large. On failure the table list stays empty and c->error is set.
 * Column-loading errors are ignored here; those tables just stay unloaded. */
bool conn_refresh(Connection *c) {
    if (!c->db) return false;
    db_tables_free(c->tables, c->ntables);
    c->tables = NULL; c->ntables = 0;
    char err[256] = "";
    if (!c->db->drv->list_tables(c->db, &c->tables, &c->ntables, err, sizeof err)) {
        snprintf(c->error, sizeof c->error, "%s", err);
        return false;
    }
    LOG_I("catalog reloaded: %s, %d tables", c->name, c->ntables);   /* the global introspection (AT-09 checks it is not repeated per change) */
    c->columns_lazy = c->ntables > LAZY_COLUMNS_ABOVE;
    if (!c->columns_lazy)
        for (int i = 0; i < c->ntables; i++) if (!c->tables[i].cols_loaded) c->db->drv->list_columns(c->db, &c->tables[i], err, sizeof err);
    c->error[0] = 0;
    return true;
}

/* See conn.h. Already-open connections return true immediately. The Db label is set to the
 * connection's name (used in logs and the result header). A failed catalog read does not
 * fail the connect: c->error holds it and the tree is simply empty. */
bool conn_connect(Connection *c) {
    if (c->db) return true;
    const DriverInfo *d = conn_driver(c);
    if (!d) { snprintf(c->error, sizeof c->error, "Unknown driver %s", c->driver); return false; }
    char loc[2048], err[256] = "";
    d->location(c, loc, sizeof loc);
    c->db = db_open(d->drv, loc, err, sizeof err);
    memset(loc, 0, sizeof loc);
    c->generation++; c->state_since = SDL_GetTicks();
    if (!c->db) { snprintf(c->error, sizeof c->error, "%s", err[0] ? err : "Could not connect"); c->state = CONN_FAILED; return false; }
    c->state = CONN_CONNECTED;
    c->schema[0] = 0; c->manual_tx = false; c->isolation[0] = 0; c->ro_level = 0;   /* a new session */
    free(c->db->label);
    c->db->label = SDL_strdup(c->name);
    return conn_refresh(c) || true;   /* a connection with an unreadable catalog is still open */
}

/* modules whose workers may hold a connection's Db (object editor, Explorer catalog reads) */
static void (*close_hooks[4])(Connection *c);
void conn_add_close_hook(void (*hook)(Connection *c)) {
    for (int i = 0; i < 4; i++) if (close_hooks[i] == hook) return;
    for (int i = 0; i < 4; i++) if (!close_hooks[i]) { close_hooks[i] = hook; return; }
}
void conn_remove_close_hook(void (*hook)(Connection *c)) { for (int i = 0; i < 4; i++) if (close_hooks[i] == hook) close_hooks[i] = NULL; }

/* See conn.h. */
void conn_disconnect(Connection *c) {
    for (int i = 0; i < 4 && c->db; i++) if (close_hooks[i]) close_hooks[i](c);   /* workers holding this Db stop before it is closed */
    db_tables_free(c->tables, c->ntables);
    c->tables = NULL; c->ntables = 0;
    db_close(c->db);
    c->db = NULL;
    c->state = CONN_DISCONNECTED; c->generation++; c->state_since = SDL_GetTicks();
}

/* ---- asynchronous connect (TBR-011 / 012 / 045) ---- */

/* One connect running on a worker. The worker writes db / tables / err and then sets done;
 * the UI thread reads them only after done. abandoned: the UI gave up (cancel, delete). */
typedef struct ConnAttempt {
    const DbDriver *drv;
    char *location;              /* freed (and wiped) by the worker when it is done with it */
    Db *db; DbTable *tables; int ntables; bool lazy;
    char err[256];
    SDL_Thread *thread;
    int done, abandoned;         /* atomics */
} ConnAttempt;

const char *conn_env_name(int env) {
    static const char *n[] = { "", "DEV", "HML", "PROD" };
    return env >= 0 && env < CONN_ENV_COUNT ? n[env] : "";
}
int conn_env_by_name(const char *name) {
    for (int e = 1; e < CONN_ENV_COUNT; e++) if (!strcasecmp(name, conn_env_name(e))) return e;
    return CONN_ENV_NONE;
}
uint32_t conn_env_color(const Connection *c) {
    static const uint32_t def[] = { 0, 0x4FB477, 0xD9A441, 0xE5534B };   /* dev green, hml amber, prod red */
    if (c->env_color) return c->env_color;
    return c->env >= 0 && c->env < CONN_ENV_COUNT ? def[c->env] : 0;
}

const char *conn_state_name(int state) {
    static const char *n[] = { "disconnected", "connecting", "connected", "failed", "reconnecting" };
    return state >= 0 && state <= CONN_RECONNECTING ? n[state] : "?";
}

/* Worker: open, read the catalog (columns too unless the schema is large), then signal. */
static int connect_worker(void *ud) {
    ConnAttempt *t = ud;
    t->db = db_open(t->drv, t->location, t->err, sizeof t->err);
    memset(t->location, 0, strlen(t->location)); free(t->location); t->location = NULL;   /* the password does not outlive the open */
    if (t->db) {
        char err[256] = "";
        if (t->db->drv->list_tables(t->db, &t->tables, &t->ntables, err, sizeof err)) {
            t->lazy = t->ntables > LAZY_COLUMNS_ABOVE;
            if (!t->lazy) for (int i = 0; i < t->ntables; i++) t->db->drv->list_columns(t->db, &t->tables[i], err, sizeof err);
        }
    }
    __atomic_store_n(&t->done, 1, __ATOMIC_SEQ_CST);
    return 0;
}

static void attempt_free(ConnAttempt *t, bool keep_result);
/* Helper thread for an abandoned attempt: joins the connect worker, closes its result. */
static int abandon_worker(void *ud) { attempt_free(ud, false); return 0; }

bool conn_connect_async(Connection *c, bool reconnect) {
    if (c->db || c->attempt) return false;
    const DriverInfo *d = conn_driver(c);
    if (!d) { snprintf(c->error, sizeof c->error, "Unknown driver %s", c->driver); c->state = CONN_FAILED; return false; }
    ConnAttempt *t = calloc(1, sizeof *t);
    char loc[2048];
    d->location(c, loc, sizeof loc);
    t->drv = d->drv; t->location = strdup(loc);
    memset(loc, 0, sizeof loc);
    t->thread = SDL_CreateThread(connect_worker, "lume-connect", t);
    if (!t->thread) { free(t->location); free(t); snprintf(c->error, sizeof c->error, "could not start the connect thread"); c->state = CONN_FAILED; return false; }
    c->attempt = t; c->error[0] = 0;
    c->state = reconnect ? CONN_RECONNECTING : CONN_CONNECTING; c->generation++; c->state_since = SDL_GetTicks();
    return true;
}

/* Frees what an abandoned or finished attempt still owns (worker joined first). */
static void attempt_free(ConnAttempt *t, bool keep_result) {
    SDL_WaitThread(t->thread, NULL);
    if (!keep_result) { db_tables_free(t->tables, t->ntables); db_close(t->db); }
    free(t);
}

bool conn_poll(Connection *c) {
    ConnAttempt *t = c->attempt;
    if (!t || !__atomic_load_n(&t->done, __ATOMIC_SEQ_CST)) return false;
    c->attempt = NULL;
    if (!t->db) {
        snprintf(c->error, sizeof c->error, "%s", t->err[0] ? t->err : "Could not connect");
        c->state = CONN_FAILED; c->state_since = SDL_GetTicks();
        attempt_free(t, false);
        return true;
    }
    c->db = t->db; c->tables = t->tables; c->ntables = t->ntables; c->columns_lazy = t->lazy;
    free(c->db->label); c->db->label = SDL_strdup(c->name);
    c->state = CONN_CONNECTED; c->state_since = SDL_GetTicks(); c->error[0] = 0;
    c->schema[0] = 0; c->manual_tx = false; c->isolation[0] = 0; c->ro_level = 0;   /* a new session */
    attempt_free(t, true);
    return true;
}

void conn_cancel_connect(Connection *c) {
    ConnAttempt *t = c->attempt;
    if (!t) return;
    c->attempt = NULL;
    /* the worker cannot be interrupted inside the driver's open: wait for it on a helper
     * thread so the UI never blocks, then close whatever it produced */
    __atomic_store_n(&t->abandoned, 1, __ATOMIC_SEQ_CST);
    SDL_DetachThread(SDL_CreateThread(abandon_worker, "lume-connect-drop", t));
    c->state = CONN_DISCONNECTED; c->generation++; c->state_since = SDL_GetTicks();
    snprintf(c->error, sizeof c->error, "connect cancelled");
}

/* See conn.h. The error text is discarded; the caller only shows that loading failed. */
bool conn_load_columns(Connection *c, DbTable *t) {
    if (t->cols_loaded) return true;
    if (!c->db || c->db->busy) return false;   /* a worker owns the session: never two threads in one driver handle */
    char err[256] = "";
    return c->db->drv->list_columns(c->db, t, err, sizeof err);
}

/* See conn.h. err receives the driver's message on failure. Blocks for up to the driver's
 * connect timeout. */
bool conn_test(const Connection *c, char *err, size_t errlen) {
    const DriverInfo *d = conn_driver(c);
    if (!d) { snprintf(err, errlen, "Unknown driver"); return false; }
    char loc[2048];
    d->location(c, loc, sizeof loc);
    Db *db = db_open(d->drv, loc, err, errlen);
    if (!db) return false;
    db_close(db);
    return true;
}
