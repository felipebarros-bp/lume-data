/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * MySQL / MariaDB driver (BL-013)
 * ===============================
 *
 * The client library (libmysqlclient, or MariaDB Connector/C: same C API) is loaded at run time
 * with dlopen, like libpq: Lume builds and runs without it, and the driver reports "client library
 * not found" until it is installed (LUME_LIBMYSQL points at a specific file).
 *
 * Location (conn.c): "host\nport\nuser\npassword\ndatabase". The password is never logged.
 * Results stream with mysql_use_result: the first `limit` rows are read, the rest stays on the
 * connection as the cursor (fetch-more continues, release drains it). Cancel asks the server
 * with KILL QUERY from a second short session (MySQL has no out-of-band cancel).
 * Only the stable prefix of MYSQL_FIELD is used (name ... type), identical in MySQL 5.7-9 and
 * MariaDB Connector/C; tests/mysql_check.c compares it with the real header.
 */
#include "db.h"
#include "rs.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#ifdef _WIN32
#include <windows.h>
#define DLOPEN(p) ((void *)LoadLibraryA(p))
#define DLSYM(h, s) ((void *)GetProcAddress((HMODULE)(h), s))
#else
#include <dlfcn.h>
#define DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define DLSYM(h, s) dlsym(h, s)
#endif

typedef struct MYSQL MYSQL;
typedef struct MYSQL_RES MYSQL_RES;
typedef char **MYSQL_ROW;
#include "db_mysql_field.h"

static struct {
    void *lib;
    MYSQL *(*init)(MYSQL *);
    MYSQL *(*real_connect)(MYSQL *, const char *, const char *, const char *, const char *, unsigned int, const char *, unsigned long);
    void (*close)(MYSQL *);
    const char *(*error)(MYSQL *);
    unsigned int (*errno_)(MYSQL *);
    const char *(*sqlstate)(MYSQL *);
    int (*real_query)(MYSQL *, const char *, unsigned long);
    MYSQL_RES *(*use_result)(MYSQL *);
    unsigned int (*field_count)(MYSQL *);
    unsigned int (*num_fields)(MYSQL_RES *);
    LumeMyField *(*fetch_field_direct)(MYSQL_RES *, unsigned int);
    MYSQL_ROW (*fetch_row)(MYSQL_RES *);
    unsigned long *(*fetch_lengths)(MYSQL_RES *);
    void (*free_result)(MYSQL_RES *);
    unsigned long long (*affected_rows)(MYSQL *);
    int (*next_result)(MYSQL *);
    unsigned long (*thread_id)(MYSQL *);
    const char *(*get_client_info)(void);
    int (*set_character_set)(MYSQL *, const char *);
    int (*options)(MYSQL *, int, const void *);
} my;

static bool my_load(char *err, size_t n) {
    if (my.lib) return true;
    const char *env = getenv("LUME_LIBMYSQL");
    const char *cand[] = {
        env,
#ifdef __APPLE__
        "/opt/homebrew/opt/mysql-client/lib/libmysqlclient.dylib", "/opt/homebrew/lib/libmysqlclient.dylib", "/usr/local/opt/mysql-client/lib/libmysqlclient.dylib",
        "/opt/homebrew/opt/mariadb-connector-c/lib/mariadb/libmariadb.3.dylib", "/opt/homebrew/lib/mariadb/libmariadb.3.dylib", "libmysqlclient.dylib",
#elif defined(_WIN32)
        "libmysql.dll", "libmariadb.dll",
#else
        "libmysqlclient.so.24", "libmysqlclient.so.21", "libmysqlclient.so", "libmariadb.so.3", "libmariadb.so",
#endif
    };
    for (size_t i = 0; i < sizeof cand / sizeof *cand && !my.lib; i++) if (cand[i]) my.lib = DLOPEN(cand[i]);
    if (!my.lib) { snprintf(err, n, "MySQL client library not found. Install it (brew install mysql-client) or set LUME_LIBMYSQL to the library path."); return false; }
#define L(f, s) do { *(void **)&my.f = DLSYM(my.lib, s); if (!my.f) { snprintf(err, n, "MySQL client: missing %s", s); my.lib = NULL; return false; } } while (0)
    L(init, "mysql_init"); L(real_connect, "mysql_real_connect"); L(close, "mysql_close"); L(error, "mysql_error"); L(errno_, "mysql_errno");
    L(sqlstate, "mysql_sqlstate"); L(real_query, "mysql_real_query"); L(use_result, "mysql_use_result"); L(field_count, "mysql_field_count");
    L(num_fields, "mysql_num_fields"); L(fetch_field_direct, "mysql_fetch_field_direct"); L(fetch_row, "mysql_fetch_row");
    L(fetch_lengths, "mysql_fetch_lengths"); L(free_result, "mysql_free_result"); L(affected_rows, "mysql_affected_rows");
    L(next_result, "mysql_next_result"); L(thread_id, "mysql_thread_id"); L(get_client_info, "mysql_get_client_info");
    L(set_character_set, "mysql_set_character_set"); L(options, "mysql_options");
#undef L
    return true;
}

typedef struct MyConn {
    MYSQL *m;
    MYSQL_RES *res;          /* open streaming result (the cursor) */
    ResultSet *active;
    char host[256], user[128], pass[256], dbname[128]; unsigned port;   /* for KILL QUERY; never logged */
} MyConn;

static void split_location(const char *loc, MyConn *c) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", loc);
    char *f[5] = { 0 }; int i = 0; char *s = tmp;
    while (i < 5) { f[i++] = s; char *nl = strchr(s, '\n'); if (!nl) break; *nl = 0; s = nl + 1; }
    snprintf(c->host, sizeof c->host, "%s", f[0] ? f[0] : "");
    c->port = f[1] && *f[1] ? (unsigned)atoi(f[1]) : 3306;
    snprintf(c->user, sizeof c->user, "%s", f[2] ? f[2] : "");
    snprintf(c->pass, sizeof c->pass, "%s", f[3] ? f[3] : "");
    snprintf(c->dbname, sizeof c->dbname, "%s", f[4] ? f[4] : "");
    memset(tmp, 0, sizeof tmp);
}

static MYSQL *connect_with(MyConn *c, char *err, size_t n) {
    MYSQL *m = my.init(NULL);
    if (!m) { snprintf(err, n, "out of memory"); return NULL; }
    unsigned timeout = 10;
    my.options(m, 0 /* MYSQL_OPT_CONNECT_TIMEOUT */, &timeout);
    if (!my.real_connect(m, c->host[0] ? c->host : "127.0.0.1", c->user, c->pass, c->dbname[0] ? c->dbname : NULL, c->port, NULL, 0)) {
        snprintf(err, n, "%s", my.error(m)); my.close(m); return NULL;
    }
    my.set_character_set(m, "utf8mb4");
    return m;
}

static bool my_open(Db *db, const char *location, char *err, size_t n) {
    if (!my_load(err, n)) return false;
    MyConn *c = calloc(1, sizeof *c);
    split_location(location, c);
    c->m = connect_with(c, err, n);
    if (!c->m) { memset(c, 0, sizeof *c); free(c); return false; }
    db->handle = c;
    return true;
}

static void my_release(Db *db, ResultSet *rs);
static void my_close(Db *db) {
    MyConn *c = db->handle;
    if (!c) return;
    if (c->active) my_release(db, c->active);
    my.close(c->m);
    memset(c, 0, sizeof *c); free(c);
}

/* KILL QUERY from a second session: the only way to stop a running MySQL statement */
static void my_cancel(Db *db) {
    MyConn *c = db->handle;
    char err[256];
    MYSQL *k = connect_with(c, err, sizeof err);
    if (!k) return;
    char q[64]; snprintf(q, sizeof q, "KILL QUERY %lu", my.thread_id(c->m));
    my.real_query(k, q, strlen(q));
    my.close(k);
}

static int kind_of(const LumeMyField *f, const char **tname) {
    switch (f->type) {
    case 0: case 246: *tname = "decimal"; return CK_NUMBER;
    case 1: *tname = "tinyint"; return CK_NUMBER;
    case 2: *tname = "smallint"; return CK_NUMBER;
    case 3: *tname = "int"; return CK_NUMBER;
    case 9: *tname = "mediumint"; return CK_NUMBER;
    case 8: *tname = "bigint"; return CK_NUMBER;
    case 4: *tname = "float"; return CK_NUMBER;
    case 5: *tname = "double"; return CK_NUMBER;
    case 13: *tname = "year"; return CK_NUMBER;
    case 7: *tname = "timestamp"; return CK_DATE;
    case 10: case 14: *tname = "date"; return CK_DATE;
    case 11: *tname = "time"; return CK_DATE;
    case 12: *tname = "datetime"; return CK_DATE;
    case 16: *tname = "bit"; return CK_OTHER;
    case 245: *tname = "json"; return CK_JSON;
    case 247: *tname = "enum"; return CK_TEXT;
    case 248: *tname = "set"; return CK_TEXT;
    case 255: *tname = "geometry"; return CK_BINARY;
    case 249: case 250: case 251: case 252: *tname = f->charsetnr == 63 ? "blob" : "text"; return f->charsetnr == 63 ? CK_BINARY : CK_TEXT;
    case 15: case 253: *tname = f->charsetnr == 63 ? "varbinary" : "varchar"; return f->charsetnr == 63 ? CK_BINARY : CK_TEXT;
    case 254: *tname = f->charsetnr == 63 ? "binary" : "char"; return f->charsetnr == 63 ? CK_BINARY : CK_TEXT;
    default: *tname = ""; return CK_OTHER;
    }
}

static void set_error(MyConn *c, ResultSet *rs) {
    snprintf(rs->error, sizeof rs->error, "%s", my.error(c->m));
    rs->err_code = (long)my.errno_(c->m);
    snprintf(rs->err_state, sizeof rs->err_state, "%s", my.sqlstate(c->m));
    rs->ok = false;
    if (rs->err_code == 1317 /* ER_QUERY_INTERRUPTED */) rs->cancelled = true;
}

/* reads up to limit rows (limit <= 0: all) of the open result; true while rows remain */
static bool pull(Db *db, ResultSet *rs, int limit) {
    MyConn *c = db->handle;
    int got = 0;
    for (;;) {
        MYSQL_ROW row = my.fetch_row(c->res);
        if (!row) {
            if (my.errno_(c->m)) set_error(c, rs);
            my.free_result(c->res); c->res = NULL;
            while (my.next_result(c->m) == 0) { MYSQL_RES *x = my.use_result(c->m); if (x) my.free_result(x); }   /* CALL's extra results */
            return false;
        }
        unsigned long *len = my.fetch_lengths(c->res);
        int r = rs_add_row(rs);
        if (r < 0) continue;
        for (int i = 0; i < rs->ncols; i++) rs_set(rs, r, i, row[i], row[i] ? (int)len[i] : 0);
        if (limit > 0 && ++got >= limit) return true;
    }
}

static void my_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs) {
    MyConn *c = db->handle;
    if (c->active) { ResultSet *o = c->active; my_release(db, o); o->cursor_lost = true; }
    rs_init(rs); rs->db = db; rs->ok = true;
    if (len < 0) len = (int)strlen(sql);
    Uint64 t0 = SDL_GetPerformanceCounter();
    if (my.real_query(c->m, sql, (unsigned long)len)) { set_error(c, rs); return; }
    if (my.field_count(c->m) == 0) {
        rs->affected = (int)my.affected_rows(c->m);
        while (my.next_result(c->m) == 0) { MYSQL_RES *x = my.use_result(c->m); if (x) my.free_result(x); }
        rs->complete = true;
        rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        return;
    }
    c->res = my.use_result(c->m);
    if (!c->res) { set_error(c, rs); return; }
    unsigned nf = my.num_fields(c->res);
    for (unsigned i = 0; i < nf; i++) { LumeMyField *f = my.fetch_field_direct(c->res, i); const char *tn; int k = kind_of(f, &tn); rs_add_column(rs, f->name, tn, k); }
    rs->has_rows = true;
    bool more = pull(db, rs, limit);
    rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (more) { rs->cursor = c; c->active = rs; rs->truncated = true; rs->complete = false; }
    else rs->complete = rs->ok;
}

static bool my_fetch(Db *db, ResultSet *rs, int n) {
    MyConn *c = db->handle;
    if (!rs->cursor || !c->res) return false;
    int before = rs->nrows;
    bool more = pull(db, rs, n);
    if (!more) { rs->cursor = NULL; c->active = NULL; rs->complete = rs->ok; rs->truncated = false; }
    return rs->nrows > before;
}

/* release: mysql_free_result reads and discards the rest of the stream */
static void my_release(Db *db, ResultSet *rs) {
    MyConn *c = db->handle;
    if (!rs->cursor) return;
    rs->cursor = NULL;
    if (c->active == rs) c->active = NULL;
    if (c->res) { my.free_result(c->res); c->res = NULL; while (my.next_result(c->m) == 0) { MYSQL_RES *x = my.use_result(c->m); if (x) my.free_result(x); } }
}
static void my_rehome(Db *db, ResultSet *rs) { ((MyConn *)db->handle)->active = rs; }

/* one-shot query into a ResultSet (catalog helpers) */
static bool q_all(Db *db, const char *sql, ResultSet *rs, char *err, size_t n) {
    my_exec(db, sql, -1, 0, rs);
    if (!rs->ok) { snprintf(err, n, "%s", rs->error); rs_free(rs); return false; }
    return true;
}
static void lit(const char *s, char *out, size_t n) {
    size_t o = 0; out[o++] = '\'';
    for (; *s && o < n - 3; s++) { if (*s == '\'' || *s == '\\') out[o++] = *s == '\\' ? '\\' : '\''; out[o++] = *s; }
    out[o++] = '\''; out[o] = 0;
}

static bool my_list_tables(Db *db, DbTable **out, int *count, char *err, size_t n) {
    ResultSet rs;
    if (!q_all(db, "SELECT table_name, table_type FROM information_schema.tables WHERE table_schema = DATABASE() ORDER BY table_name", &rs, err, n)) return false;
    *out = calloc((size_t)(rs.nrows ? rs.nrows : 1), sizeof **out); *count = rs.nrows;
    for (int i = 0; i < rs.nrows; i++) { (*out)[i].name = db_strdup(rs_cell(&rs, i, 0)); const char *t = rs_cell(&rs, i, 1); (*out)[i].is_view = t && strstr(t, "VIEW"); }
    rs_free(&rs);
    return true;
}
static bool my_list_columns(Db *db, DbTable *t, char *err, size_t n) {
    /* "db.table" (a table of another database, listed by the Explorer) or a bare name in DATABASE() */
    char q[900], l[300], dbl[300] = "DATABASE()", nm[256], dbn[128];
    const char *dot = strchr(t->name, '.');
    if (dot) { snprintf(dbn, sizeof dbn, "%.*s", (int)(dot - t->name), t->name); snprintf(nm, sizeof nm, "%s", dot + 1); lit(dbn, dbl, sizeof dbl); }
    else snprintf(nm, sizeof nm, "%s", t->name);
    lit(nm, l, sizeof l);
    snprintf(q, sizeof q, "SELECT column_name, column_type, column_key = 'PRI', is_nullable = 'NO', extra LIKE '%%GENERATED%%' FROM information_schema.columns "
                          "WHERE table_schema = %s AND table_name = %s ORDER BY ordinal_position", dbl, l);
    ResultSet rs;
    if (!q_all(db, q, &rs, err, n)) return false;
    t->cols = calloc((size_t)(rs.nrows ? rs.nrows : 1), sizeof *t->cols); t->ncols = rs.nrows;
    for (int i = 0; i < rs.nrows; i++) {
        DbColumn *c = &t->cols[i];
        c->name = db_strdup(rs_cell(&rs, i, 0)); c->type = db_strdup(rs_cell(&rs, i, 1));
        c->pk = rs_cell(&rs, i, 2) && !strcmp(rs_cell(&rs, i, 2), "1"); c->notnull = rs_cell(&rs, i, 3) && !strcmp(rs_cell(&rs, i, 3), "1");
        c->generated = rs_cell(&rs, i, 4) && !strcmp(rs_cell(&rs, i, 4), "1");
    }
    t->cols_loaded = true;
    rs_free(&rs);
    return true;
}
static bool my_list_fks(Db *db, const char *table, DbFk **out, int *count, char *err, size_t n) {
    char q[700], l[300]; lit(table, l, sizeof l);
    snprintf(q, sizeof q, "SELECT constraint_name, column_name, referenced_table_name, referenced_column_name FROM information_schema.key_column_usage "
                          "WHERE table_schema = DATABASE() AND table_name = %s AND referenced_table_name IS NOT NULL ORDER BY constraint_name, ordinal_position", l);
    ResultSet rs;
    if (!q_all(db, q, &rs, err, n)) return false;
    *out = calloc((size_t)(rs.nrows ? rs.nrows : 1), sizeof **out); *count = rs.nrows;
    for (int i = 0; i < rs.nrows; i++) {
        (*out)[i].name = db_strdup(rs_cell(&rs, i, 0)); (*out)[i].from_col = db_strdup(rs_cell(&rs, i, 1));
        (*out)[i].to_table = db_strdup(rs_cell(&rs, i, 2)); (*out)[i].to_col = db_strdup(rs_cell(&rs, i, 3));
    }
    rs_free(&rs);
    return true;
}

/* `a`.`b`, backticks doubled inside */
static void my_quote(const char *ident, char *buf, size_t n) {
    size_t o = 0; buf[o++] = '`';
    for (const char *p = ident; *p && o < n - 4; p++) {
        if (*p == '.') { buf[o++] = '`'; buf[o++] = '.'; buf[o++] = '`'; continue; }
        if (*p == '`') buf[o++] = '`';
        buf[o++] = *p;
    }
    buf[o++] = '`'; buf[o] = 0;
}
static void my_select(const char *q, const char *where, const char *order, int limit, char *buf, size_t n) {
    snprintf(buf, n, "SELECT * FROM %s%s%s%s%s LIMIT %d", q, where && *where ? " WHERE " : "", where ? where : "", order && *order ? " ORDER BY " : "", order ? order : "", limit);
}

static bool my_probe(char *v, size_t n) {
    char err[256];
    if (!my_load(err, sizeof err)) { snprintf(v, n, "client library not found"); return false; }
    snprintf(v, n, "client %s", my.get_client_info());
    return true;
}

const DbDriver db_mysql_driver = {
    "MySQL", my_open, my_close, my_list_tables, my_list_columns, my_exec, my_fetch, my_release, my_rehome, my_quote, my_select,
    DB_CAP_FETCH_MORE | DB_CAP_CANCEL | DB_CAP_SERVER_SORT | DB_CAP_SERVER_FILTER, "START TRANSACTION", my_list_fks, NULL, my_cancel, NULL, my_probe,
};
