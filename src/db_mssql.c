/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * SQL Server driver through FreeTDS db-lib (BL-014)
 * =================================================
 *
 * libsybdb is loaded at run time (LUME_LIBSYBDB, or the usual Homebrew / Linux paths), like libpq.
 * Location (conn.c): "host\nport\nuser\npassword\ndatabase"; never logged.
 * One statement per exec (GO is a client separator and never reaches the server). Rows stream
 * with dbnextrow: the first `limit` rows are read and the rest stays as the cursor. Values are
 * converted to text by dbconvert (binary as 0x hex). Errors come from the message / error
 * handlers (number, state, severity). Cancel sends a TDS attention with dbcancel.
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

/* sybdb.h values (FreeTDS 1.x) */
enum { SUCCEED = 1, FAIL = 0, NO_MORE_RESULTS = 2, REG_ROW = -1, NO_MORE_ROWS = -2, INT_CANCEL = 2, DBSETUSER = 2, DBSETPWD = 3, DBSETAPP = 5, DBSETCHARSET = 10, DBVERSION_74 = 8 };
enum { SYBCHAR = 47, SYBVARCHAR = 39, SYBINT1 = 48, SYBINT2 = 52, SYBINT4 = 56, SYBINT8 = 127, SYBFLT8 = 62, SYBREAL = 59, SYBNUMERIC = 108, SYBDECIMAL = 106,
       SYBMONEY = 60, SYBMONEY4 = 122, SYBDATETIME = 61, SYBDATETIME4 = 58, SYBBIT = 50, SYBTEXT = 35, SYBNTEXT = 99, SYBNVARCHAR = 103, SYBBINARY = 45,
       SYBVARBINARY = 37, SYBIMAGE = 34, SYBMSDATE = 40, SYBMSTIME = 41, SYBMSDATETIME2 = 42, SYBMSDATETIMEOFFSET = 43, SYBINTN = 38, SYBBITN = 104 };
/* DBDATEREC2: 12 ints, the same positions in the Microsoft and Sybase layouts (opened with msdblib = 1: month 1-12) */
typedef struct { int year, quarter, month, day, dayofyear, week, weekday, hour, minute, second, nanosecond, tzone; } LumeDateRec;

typedef struct tds_dblib_dbprocess DBPROCESS;
typedef struct tds_dblib_loginrec LOGINREC;
typedef int (*ErrHandler)(DBPROCESS *, int, int, int, char *, char *);
typedef int (*MsgHandler)(DBPROCESS *, int, int, int, char *, char *, char *, int);

static struct {
    void *lib;
    int (*init)(void);
    LOGINREC *(*login)(void);
    int (*setlname)(LOGINREC *, const char *, int);
    int (*setlversion)(LOGINREC *, unsigned char);
    void (*loginfree)(LOGINREC *);
    DBPROCESS *(*open)(LOGINREC *, const char *);
    int (*use)(DBPROCESS *, const char *);
    void (*close)(DBPROCESS *);
    int (*cmd)(DBPROCESS *, const char *);
    int (*sqlexec)(DBPROCESS *);
    int (*results)(DBPROCESS *);
    int (*numcols)(DBPROCESS *);
    char *(*colname)(DBPROCESS *, int);
    int (*coltype)(DBPROCESS *, int);
    int (*nextrow)(DBPROCESS *);
    unsigned char *(*data)(DBPROCESS *, int);
    int (*datlen)(DBPROCESS *, int);
    int (*convert)(DBPROCESS *, int, const unsigned char *, int, int, unsigned char *, int);
    int (*count)(DBPROCESS *);
    int (*cancel)(DBPROCESS *);
    int (*canquery)(DBPROCESS *);
    void (*setuserdata)(DBPROCESS *, unsigned char *);
    unsigned char *(*getuserdata)(DBPROCESS *);
    ErrHandler (*errhandle)(ErrHandler);
    MsgHandler (*msghandle)(MsgHandler);
    int (*setlogintime)(int);
    const char *(*version)(void);
    int (*anydatecrack)(DBPROCESS *, LumeDateRec *, int, const void *);
} tds;

typedef struct MsConn {
    DBPROCESS *p;
    ResultSet *active;
    bool in_rows;            /* a result's rows are being read (the cursor) */
    char err[512]; long err_no; int err_state; bool has_err;   /* last server / library error */
    char notices[2048]; int nlen;
    volatile int cancel_req;
} MsConn;

/* handlers: the dbproc's user data points at its MsConn */
static int on_msg(DBPROCESS *p, int no, int state, int severity, char *text, char *srv, char *proc, int line) {
    (void)srv; (void)proc; (void)line;
    MsConn *c = p && tds.getuserdata ? (MsConn *)tds.getuserdata(p) : NULL;
    if (!c || !text) return 0;
    if (severity > 10) { snprintf(c->err, sizeof c->err, "%s", text); c->err_no = no; c->err_state = state; c->has_err = true; }
    else if (no != 5701 && no != 5703 && c->nlen < (int)sizeof c->notices - 2) c->nlen += snprintf(c->notices + c->nlen, sizeof c->notices - (size_t)c->nlen, "%s%s", c->nlen ? "\n" : "", text);   /* not "changed database context" */
    return 0;
}
static int on_err(DBPROCESS *p, int severity, int dberr, int oserr, char *dberrstr, char *oserrstr) {
    (void)severity; (void)oserr; (void)oserrstr;
    MsConn *c = p && tds.getuserdata ? (MsConn *)tds.getuserdata(p) : NULL;
    if (c && !c->has_err && dberrstr) { snprintf(c->err, sizeof c->err, "%s", dberrstr); c->err_no = dberr; c->has_err = true; }
    return INT_CANCEL;
}

static bool ms_load(char *err, size_t n) {
    if (tds.lib) return true;
    const char *env = getenv("LUME_LIBSYBDB");
    const char *cand[] = { env,
#ifdef __APPLE__
        "/opt/homebrew/opt/freetds/lib/libsybdb.dylib", "/usr/local/opt/freetds/lib/libsybdb.dylib", "libsybdb.dylib",
#elif defined(_WIN32)
        "sybdb.dll",
#else
        "libsybdb.so.5", "libsybdb.so",
#endif
    };
    for (size_t i = 0; i < sizeof cand / sizeof *cand && !tds.lib; i++) if (cand[i]) tds.lib = DLOPEN(cand[i]);
    if (!tds.lib) { snprintf(err, n, "FreeTDS (libsybdb) not found. Install it (brew install freetds) or set LUME_LIBSYBDB to the library path."); return false; }
#define L(f, s) do { *(void **)&tds.f = DLSYM(tds.lib, s); if (!tds.f) { snprintf(err, n, "FreeTDS: missing %s", s); tds.lib = NULL; return false; } } while (0)
    L(init, "dbinit"); L(login, "dblogin"); L(setlname, "dbsetlname"); L(setlversion, "dbsetlversion"); L(loginfree, "dbloginfree");
    L(open, "tdsdbopen"); L(use, "dbuse"); L(close, "dbclose"); L(cmd, "dbcmd"); L(sqlexec, "dbsqlexec"); L(results, "dbresults");
    L(numcols, "dbnumcols"); L(colname, "dbcolname"); L(coltype, "dbcoltype"); L(nextrow, "dbnextrow"); L(data, "dbdata"); L(datlen, "dbdatlen");
    L(convert, "dbconvert"); L(count, "dbcount"); L(cancel, "dbcancel"); L(canquery, "dbcanquery"); L(setuserdata, "dbsetuserdata");
    L(getuserdata, "dbgetuserdata"); L(errhandle, "dberrhandle"); L(msghandle, "dbmsghandle"); L(setlogintime, "dbsetlogintime"); L(version, "dbversion"); L(anydatecrack, "dbanydatecrack");
#undef L
    if (tds.init() != SUCCEED) { snprintf(err, n, "FreeTDS: dbinit failed"); tds.lib = NULL; return false; }
    tds.errhandle(on_err); tds.msghandle(on_msg);
    return true;
}

/* tdsdbopen(login, server, msdblib): the same as dbopen, exported under this name by FreeTDS */
static DBPROCESS *open_with(const char *loc, MsConn *c, char *err, size_t n) {
    char tmp[1024]; snprintf(tmp, sizeof tmp, "%s", loc);
    char *f[5] = { 0 }; int i = 0; char *s = tmp;
    while (i < 5) { f[i++] = s; char *nl = strchr(s, '\n'); if (!nl) break; *nl = 0; s = nl + 1; }
    tds.setlogintime(db_connect_timeout_s > 0 ? db_connect_timeout_s : 10);   /* Settings may change it between connects */
    LOGINREC *lg = tds.login();
    tds.setlname(lg, f[2] ? f[2] : "", DBSETUSER);
    tds.setlname(lg, f[3] ? f[3] : "", DBSETPWD);
    tds.setlname(lg, "Lume", DBSETAPP);
    tds.setlname(lg, "UTF-8", DBSETCHARSET);
    tds.setlversion(lg, DBVERSION_74);
    char server[300]; snprintf(server, sizeof server, "%s:%s", f[0] && *f[0] ? f[0] : "127.0.0.1", f[1] && *f[1] ? f[1] : "1433");
    DBPROCESS *(*open3)(LOGINREC *, const char *, int) = (DBPROCESS *(*)(LOGINREC *, const char *, int))(void *)tds.open;
    DBPROCESS *p = open3(lg, server, 1);
    tds.loginfree(lg);
    memset(tmp, 0, sizeof tmp);
    if (!p) { snprintf(err, n, "Could not connect to SQL Server at %s (check host, port, login and password)", server); return NULL; }
    tds.setuserdata(p, (unsigned char *)c);
    if (f[4] && *f[4] && tds.use(p, f[4]) != SUCCEED) { snprintf(err, n, "%s", c->err[0] ? c->err : "cannot use the database"); tds.close(p); return NULL; }
    c->err[0] = 0; c->has_err = false;
    return p;
}

static bool ms_open(Db *db, const char *location, char *err, size_t n) {
    if (!ms_load(err, n)) return false;
    MsConn *c = calloc(1, sizeof *c);
    c->p = open_with(location, c, err, n);
    if (!c->p) { free(c); return false; }
    db->handle = c;
    return true;
}
static void ms_release(Db *db, ResultSet *rs);
static void ms_close(Db *db) {
    MsConn *c = db->handle;
    if (!c) return;
    if (c->active) ms_release(db, c->active);
    tds.close(c->p);
    free(c);
}

static int kind_of(int t, const char **tn) {
    switch (t) {
    case SYBINT1: *tn = "tinyint"; return CK_NUMBER;
    case SYBINT2: *tn = "smallint"; return CK_NUMBER;
    case SYBINT4: *tn = "int"; return CK_NUMBER;
    case SYBINT8: *tn = "bigint"; return CK_NUMBER;
    case SYBFLT8: *tn = "float"; return CK_NUMBER;
    case SYBREAL: *tn = "real"; return CK_NUMBER;
    case SYBNUMERIC: *tn = "numeric"; return CK_NUMBER;
    case SYBDECIMAL: *tn = "decimal"; return CK_NUMBER;
    case SYBMONEY: case SYBMONEY4: *tn = "money"; return CK_NUMBER;
    case SYBDATETIME: case SYBDATETIME4: *tn = "datetime"; return CK_DATE;
    case SYBMSDATE: *tn = "date"; return CK_DATE;
    case SYBMSTIME: *tn = "time"; return CK_DATE;
    case SYBMSDATETIME2: *tn = "datetime2"; return CK_DATE;
    case SYBMSDATETIMEOFFSET: *tn = "datetimeoffset"; return CK_DATE;
    case SYBBIT: case SYBBITN: *tn = "bit"; return CK_BOOL;
    case SYBINTN: *tn = "int"; return CK_NUMBER;
    case SYBBINARY: case SYBVARBINARY: case SYBIMAGE: *tn = "varbinary"; return CK_BINARY;
    case SYBTEXT: *tn = "text"; return CK_TEXT;
    case SYBNTEXT: *tn = "ntext"; return CK_TEXT;
    case SYBNVARCHAR: *tn = "nvarchar"; return CK_TEXT;
    case SYBCHAR: *tn = "char"; return CK_TEXT;
    case SYBVARCHAR: *tn = "varchar"; return CK_TEXT;
    default: *tn = ""; return CK_OTHER;
    }
}

static void take_error(MsConn *c, ResultSet *rs) {
    snprintf(rs->error, sizeof rs->error, "%s", c->err[0] ? c->err : "SQL Server: the statement failed");
    rs->err_code = c->err_no; rs->ok = false;
    snprintf(rs->err_state, sizeof rs->err_state, "%d", c->err_state);
    if (c->cancel_req) rs->cancelled = true;
}

/* text of column i (1-based) of the current row; NULL for SQL NULL */
static void put_cell(MsConn *c, ResultSet *rs, int row, int i) {
    unsigned char *d = tds.data(c->p, i);
    int len = tds.datlen(c->p, i);
    if (!d && len == 0) { rs_set(rs, row, i - 1, NULL, 0); return; }
    int t = tds.coltype(c->p, i);
    if (t == SYBCHAR || t == SYBVARCHAR || t == SYBTEXT || t == SYBNVARCHAR || t == SYBNTEXT) { rs_set(rs, row, i - 1, (const char *)d, len); return; }
    if (t == SYBBINARY || t == SYBVARBINARY || t == SYBIMAGE) {
        char *h = malloc((size_t)len * 2 + 3); h[0] = '0'; h[1] = 'x';
        for (int k = 0; k < len; k++) sprintf(h + 2 + k * 2, "%02X", d[k]);
        rs_set(rs, row, i - 1, h, len * 2 + 2); free(h); return;
    }
    if (t == SYBDATETIME || t == SYBDATETIME4 || t == SYBMSDATE || t == SYBMSTIME || t == SYBMSDATETIME2 || t == SYBMSDATETIMEOFFSET) {
        /* ISO 8601, never the library's locale format */
        LumeDateRec r; memset(&r, 0, sizeof r);
        if (tds.anydatecrack(c->p, &r, t, d) == SUCCEED) {
            char b[64]; int o = 0;
            if (t != SYBMSTIME) o += snprintf(b + o, sizeof b - (size_t)o, "%04d-%02d-%02d", r.year, r.month, r.day);
            if (t != SYBMSDATE) {
                o += snprintf(b + o, sizeof b - (size_t)o, "%s%02d:%02d:%02d", o ? " " : "", r.hour, r.minute, r.second);
                if (r.nanosecond) { char f[16]; snprintf(f, sizeof f, "%09d", r.nanosecond); int fl = 9; while (fl > 0 && f[fl - 1] == '0') fl--; o += snprintf(b + o, sizeof b - (size_t)o, ".%.*s", fl, f); }
                if (t == SYBMSDATETIMEOFFSET) o += snprintf(b + o, sizeof b - (size_t)o, "%c%02d:%02d", r.tzone < 0 ? '-' : '+', abs(r.tzone) / 60, abs(r.tzone) % 60);
            }
            rs_set(rs, row, i - 1, b, o); return;
        }
    }
    /* everything else (numbers, xml, uniqueidentifier, sql_variant ...): converted to text in a buffer
     * sized by the value, never cut at a fixed length (a Showplan XML is tens of KB) */
    size_t cap = (size_t)len * 4 + 128; if (cap < 256) cap = 256;
    char *buf = malloc(cap); int n = tds.convert(c->p, t, d, len, SYBCHAR, (unsigned char *)buf, (int)cap - 1);
    if (n < 0) n = 0;
    while (n > 0 && buf[n - 1] == ' ') n--;
    rs_set(rs, row, i - 1, buf, n);
    free(buf);
}

/* reads up to limit rows of the current result; true while rows remain */
static bool pull_ex(MsConn *c, ResultSet *rs, int limit, bool drain);
static bool pull(MsConn *c, ResultSet *rs, int limit) { return pull_ex(c, rs, limit, true); }
static bool pull_ex(MsConn *c, ResultSet *rs, int limit, bool drain) {
    int got = 0, st;
    while ((st = tds.nextrow(c->p)) == REG_ROW) {
        int r = rs_add_row(rs);
        if (r >= 0) for (int i = 1; i <= rs->ncols; i++) put_cell(c, rs, r, i);
        if (limit > 0 && ++got >= limit) return true;
    }
    if (st != NO_MORE_ROWS && c->has_err) take_error(c, rs);
    /* the rest of the batch: further results are drained */
    if (drain) while (tds.results(c->p) == SUCCEED) tds.canquery(c->p);
    c->in_rows = false;
    return false;
}

static void ms_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs) {
    MsConn *c = db->handle;
    if (c->active) { ResultSet *o = c->active; ms_release(db, o); o->cursor_lost = true; }
    rs_init(rs); rs->db = db; rs->ok = true;
    c->err[0] = 0; c->has_err = false; c->nlen = 0; c->notices[0] = 0; c->cancel_req = 0;
    if (len < 0) len = (int)strlen(sql);
    char *text = malloc((size_t)len + 1); memcpy(text, sql, (size_t)len); text[len] = 0;
    Uint64 t0 = SDL_GetPerformanceCounter();
    tds.cmd(c->p, text); free(text);
    if (tds.sqlexec(c->p) != SUCCEED) { take_error(c, rs); while (tds.results(c->p) == SUCCEED) tds.canquery(c->p); rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency(); return; }
    int r;
    if (db->want_last_result) {   /* every result read whole; the last one with columns is kept (plan capture only) */
        bool any = false;
        while ((r = tds.results(c->p)) == SUCCEED) {
            int nc = tds.numcols(c->p);
            if (nc <= 0) { int k = tds.count(c->p); if (k > 0) rs->affected += k; continue; }
            ResultSet t; rs_init(&t); t.ok = true;
            for (int i = 1; i <= nc; i++) { const char *tn; int kd = kind_of(tds.coltype(c->p, i), &tn); rs_add_column(&t, tds.colname(c->p, i), tn, kd); }
            c->in_rows = true; pull_ex(c, &t, 0, false);
            int aff = rs->affected; rs_free(rs); rs_move(rs, &t); rs->affected = aff; rs->db = db; rs->has_rows = true; any = true;
        }
        if (r == FAIL || c->has_err) take_error(c, rs);
        rs->ok = rs->ok && !c->has_err; rs->complete = rs->ok; (void)any;
        rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        if (c->nlen) rs->notices = db_strdup(c->notices);
        return;
    }
    while ((r = tds.results(c->p)) == SUCCEED) {
        int nc = tds.numcols(c->p);
        if (nc <= 0) { int k = tds.count(c->p); if (k > 0) rs->affected += k; continue; }
        for (int i = 1; i <= nc; i++) { const char *tn; int kd = kind_of(tds.coltype(c->p, i), &tn); rs_add_column(rs, tds.colname(c->p, i), tn, kd); }
        rs->has_rows = true; c->in_rows = true;
        bool more = pull(c, rs, limit);
        rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
        if (more) { rs->cursor = c; c->active = rs; rs->truncated = true; rs->complete = false; }
        else rs->complete = rs->ok;
        if (c->nlen) rs->notices = db_strdup(c->notices);
        return;
    }
    if (r == FAIL || c->has_err) take_error(c, rs);
    rs->complete = rs->ok;
    rs->exec_ms = (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (c->nlen) rs->notices = db_strdup(c->notices);
}

static bool ms_fetch(Db *db, ResultSet *rs, int n) {
    MsConn *c = db->handle;
    if (!rs->cursor || !c->in_rows) return false;
    int before = rs->nrows;
    bool more = pull(c, rs, n);
    if (!more) { rs->cursor = NULL; c->active = NULL; rs->complete = rs->ok; rs->truncated = false; }
    return rs->nrows > before;
}
static void ms_release(Db *db, ResultSet *rs) {
    MsConn *c = db->handle;
    if (!rs->cursor) return;
    rs->cursor = NULL;
    if (c->active == rs) c->active = NULL;
    if (c->in_rows) { tds.cancel(c->p); c->in_rows = false; }
}
static void ms_rehome(Db *db, ResultSet *rs) { ((MsConn *)db->handle)->active = rs; }
/* TDS attention: stops the statement in flight (safe from another thread in FreeTDS) */
static void ms_cancel(Db *db) { MsConn *c = db->handle; c->cancel_req = 1; tds.cancel(c->p); }

static bool q_all(Db *db, const char *sql, ResultSet *rs, char *err, size_t n) {
    ms_exec(db, sql, -1, 0, rs);
    if (!rs->ok) { snprintf(err, n, "%s", rs->error); rs_free(rs); return false; }
    return true;
}
static void nlit(const char *s, char *out, size_t n) {
    size_t o = 0; out[o++] = 'N'; out[o++] = '\'';
    for (; *s && o < n - 3; s++) { if (*s == '\'') out[o++] = '\''; out[o++] = *s; }
    out[o++] = '\''; out[o] = 0;
}

static bool ms_list_tables(Db *db, DbTable **out, int *count, char *err, size_t n) {
    ResultSet rs;
    if (!q_all(db, "SELECT s.name + '.' + o.name, o.type FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id "
                   "WHERE o.type IN ('U', 'V') AND o.is_ms_shipped = 0 ORDER BY CASE WHEN s.name = 'dbo' THEN 0 ELSE 1 END, 1", &rs, err, n)) return false;
    *out = calloc((size_t)(rs.nrows ? rs.nrows : 1), sizeof **out); *count = rs.nrows;
    for (int i = 0; i < rs.nrows; i++) { (*out)[i].name = db_strdup(rs_cell(&rs, i, 0)); const char *t = rs_cell(&rs, i, 1); (*out)[i].is_view = t && t[0] == 'V'; }
    rs_free(&rs);
    return true;
}
static bool ms_list_columns(Db *db, DbTable *t, char *err, size_t n) {
    char q[1400], l[400]; nlit(t->name, l, sizeof l);
    snprintf(q, sizeof q, "SELECT c.name, TYPE_NAME(c.user_type_id), CASE WHEN EXISTS (SELECT 1 FROM sys.indexes i JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id "
                          "WHERE i.is_primary_key = 1 AND i.object_id = c.object_id AND ic.column_id = c.column_id) THEN 1 ELSE 0 END, CASE WHEN c.is_nullable = 0 THEN 1 ELSE 0 END, "
                          "CASE WHEN c.is_computed = 1 THEN 1 ELSE 0 END, c.is_identity FROM sys.columns c WHERE c.object_id = OBJECT_ID(%s) ORDER BY c.column_id", l);
    ResultSet rs;
    if (!q_all(db, q, &rs, err, n)) return false;
    t->cols = calloc((size_t)(rs.nrows ? rs.nrows : 1), sizeof *t->cols); t->ncols = rs.nrows;
    for (int i = 0; i < rs.nrows; i++) {
        DbColumn *c = &t->cols[i];
        c->name = db_strdup(rs_cell(&rs, i, 0)); c->type = db_strdup(rs_cell(&rs, i, 1));
        c->pk = rs_cell(&rs, i, 2) && !strcmp(rs_cell(&rs, i, 2), "1"); c->notnull = rs_cell(&rs, i, 3) && !strcmp(rs_cell(&rs, i, 3), "1");
        c->generated = rs_cell(&rs, i, 4) && !strcmp(rs_cell(&rs, i, 4), "1");
        c->identity_always = rs_cell(&rs, i, 5) && !strcmp(rs_cell(&rs, i, 5), "1");
    }
    t->cols_loaded = true;
    rs_free(&rs);
    return true;
}
static bool ms_list_fks(Db *db, const char *table, DbFk **out, int *count, char *err, size_t n) {
    char q[1200], l[400]; nlit(table, l, sizeof l);
    snprintf(q, sizeof q, "SELECT fk.name, pc.name, rs.name + '.' + ro.name, rc.name FROM sys.foreign_keys fk JOIN sys.foreign_key_columns k ON k.constraint_object_id = fk.object_id "
                          "JOIN sys.columns pc ON pc.object_id = k.parent_object_id AND pc.column_id = k.parent_column_id JOIN sys.objects ro ON ro.object_id = k.referenced_object_id "
                          "JOIN sys.schemas rs ON rs.schema_id = ro.schema_id JOIN sys.columns rc ON rc.object_id = k.referenced_object_id AND rc.column_id = k.referenced_column_id "
                          "WHERE fk.parent_object_id = OBJECT_ID(%s) ORDER BY fk.name, k.constraint_column_id", l);
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
/* [a].[b], ] doubled inside */
static void ms_quote(const char *ident, char *buf, size_t n) {
    size_t o = 0; buf[o++] = '[';
    for (const char *p = ident; *p && o < n - 4; p++) {
        if (*p == '.') { buf[o++] = ']'; buf[o++] = '.'; buf[o++] = '['; continue; }
        if (*p == ']') buf[o++] = ']';
        buf[o++] = *p;
    }
    buf[o++] = ']'; buf[o] = 0;
}
static void ms_select(const char *q, const char *where, const char *order, int limit, char *buf, size_t n) {
    snprintf(buf, n, "SELECT TOP (%d) * FROM %s%s%s%s%s", limit, q, where && *where ? " WHERE " : "", where ? where : "", order && *order ? " ORDER BY " : "", order ? order : "");
}
static bool ms_probe(char *v, size_t n) {
    char err[256];
    if (!ms_load(err, sizeof err)) { snprintf(v, n, "client library not found"); return false; }
    snprintf(v, n, "%s", tds.version());
    return true;
}

const DbDriver db_mssql_driver = {
    "SQL Server", ms_open, ms_close, ms_list_tables, ms_list_columns, ms_exec, ms_fetch, ms_release, ms_rehome, ms_quote, ms_select,
    DB_CAP_FETCH_MORE | DB_CAP_CANCEL | DB_CAP_SERVER_SORT | DB_CAP_SERVER_FILTER, "BEGIN TRANSACTION", ms_list_fks, NULL, ms_cancel, NULL, ms_probe,
};
