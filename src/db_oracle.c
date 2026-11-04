/* SPDX-License-Identifier: AGPL-3.0-only */
/* Oracle driver over OCI (libclntsh), loaded at runtime with dlopen. Validated against Oracle
 * Free 26ai by tests/oracle_check.sh. See docs/connections.md.
 *
 * How it works:
 *   - oci_load finds the Oracle client (LUME_ORACLE_LIB, ORACLE_HOME, usual paths) and
 *     resolves the OCI functions used into the `oci` table, once per process;
 *   - db->handle is an OraConn (environment, two error handles, service context, manual
 *     transaction flag, session id for the cancel fallback). The location string is
 *     "user\npassword\n//host:port/service" (built by conn.c);
 *   - exec() runs exactly one statement (exec.c splits scripts): the trailing ';' is removed
 *     except for PL/SQL blocks. SELECTs define one CELL_MAX text buffer per column and are
 *     fetched row by row; an OraCursor left in rs->cursor lets fetch() continue. LOB columns
 *     are shown as "<lob>" and not read;
 *   - DML commits on success (OCI_COMMIT_ON_SUCCESS) unless set_autocommit(false) was called
 *     by the table editor's batch (edit.c);
 *   - the catalog reads user_tables / user_views and the all_* dictionary views; names can
 *     be "OWNER.TABLE" for objects of other schemas.
 *
 * Threading: the environment is created OCI_THREADED. exec / fetch / release run on the
 * executor's worker thread; cancel() runs on the UI thread and uses its own error handle
 * (errb) so it never shares one with the running statement.
 */
#include "db.h"
#include <ctype.h>
#include <strings.h>
#ifdef _WIN32
#include <windows.h>
#define DLOPEN(p) ((void *)LoadLibraryA(p))
#define DLSYM(h, s) ((void *)GetProcAddress((HMODULE)(h), s))
#else
#include <dlfcn.h>
#define DLOPEN(p) dlopen(p, RTLD_NOW | RTLD_LOCAL)
#define DLSYM(h, s) dlsym(h, s)
#endif

/* Minimal OCI declarations (scalar typedefs, opaque handles, constants) copied from the OCI
 * headers, so the build needs no Oracle SDK. */
typedef int sword; typedef unsigned int ub4; typedef unsigned short ub2; typedef int sb4; typedef short sb2;
typedef unsigned char OraText;
typedef struct OCIEnv OCIEnv; typedef struct OCIError OCIError; typedef struct OCISvcCtx OCISvcCtx;
typedef struct OCIStmt OCIStmt; typedef struct OCIDefine OCIDefine; typedef struct OCISnapshot OCISnapshot;

enum { OCI_SUCCESS = 0, OCI_SUCCESS_WITH_INFO = 1, OCI_NO_DATA = 100, OCI_ERROR = -1 };
enum { OCI_HTYPE_ENV = 1, OCI_HTYPE_ERROR = 2, OCI_HTYPE_SVCCTX = 3, OCI_HTYPE_STMT = 4 };
enum { OCI_DTYPE_PARAM = 53 };
enum { OCI_ATTR_DATA_TYPE = 2, OCI_ATTR_NAME = 4, OCI_ATTR_ROW_COUNT = 9, OCI_ATTR_PREFETCH_ROWS = 11, OCI_ATTR_PARAM_COUNT = 18, OCI_ATTR_STMT_TYPE = 24 };
enum { OCI_STMT_SELECT = 1, OCI_NTV_SYNTAX = 1, OCI_DEFAULT = 0, OCI_THREADED = 1, OCI_COMMIT_ON_SUCCESS = 0x20, OCI_FETCH_NEXT = 2 };
enum { SQLT_STR = 5, SQLT_CLOB = 112, SQLT_BLOB = 113, SQLT_BFILE = 114 };
#define OCI_UTF8_CHARSET 873
#define OCI_ATTR_CALL_TIMEOUT 531   /* per round trip, milliseconds (client 18c+; older clients ignore it) */
/* CELL_MAX: bytes fetched per non-LOB cell; longer values are truncated by OCI. */
#define CELL_MAX 4000

/* OCI entry points resolved by oci_load; `lib` is the dlopen handle (never closed). */
static struct {
    void *lib;
    int major;   /* client version (OCIClientVersion), 0 when it does not report one */
    sword (*EnvNlsCreate)(OCIEnv **, ub4, void *, void *, void *, void *, size_t, void **, ub2, ub2);
    sword (*HandleAlloc)(const void *, void **, ub4, size_t, void **);
    sword (*HandleFree)(void *, ub4);
    sword (*Logon2)(OCIEnv *, OCIError *, OCISvcCtx **, const OraText *, ub4, const OraText *, ub4, const OraText *, ub4, ub4);
    sword (*Logoff)(OCISvcCtx *, OCIError *);
    sword (*StmtPrepare2)(OCISvcCtx *, OCIStmt **, OCIError *, const OraText *, ub4, const OraText *, ub4, ub4, ub4);
    sword (*StmtRelease)(OCIStmt *, OCIError *, const OraText *, ub4, ub4);
    sword (*StmtExecute)(OCISvcCtx *, OCIStmt *, OCIError *, ub4, ub4, const OCISnapshot *, OCISnapshot *, ub4);
    sword (*AttrGet)(const void *, ub4, void *, ub4 *, ub4, OCIError *);
    sword (*AttrSet)(void *, ub4, void *, ub4, ub4, OCIError *);
    sword (*ParamGet)(const void *, ub4, OCIError *, void **, ub4);
    sword (*DefineByPos)(OCIStmt *, OCIDefine **, OCIError *, ub4, void *, sb4, ub2, void *, ub2 *, ub2 *, ub4);
    sword (*StmtFetch2)(OCIStmt *, OCIError *, ub4, ub2, sb4, ub4);
    sword (*ErrorGet)(void *, ub4, OraText *, sb4 *, OraText *, ub4, ub4);
    sword (*Break)(void *, OCIError *);
    sword (*DescriptorFree)(void *, ub4);
} oci;

typedef struct { OCIEnv *env; OCIError *err, *errb; OCISvcCtx *svc; bool manual_tx, explicit_tx; char sid[40]; } OraConn;   /* explicit_tx: a SET TRANSACTION opened a transaction; no autocommit until COMMIT / ROLLBACK */   /* sid: "sid,serial#" for ALTER SYSTEM CANCEL SQL */   /* errb: error handle used by OCIBreak from the UI thread */

/* Loads the Oracle client library and resolves every symbol; true once done. The first
 * candidate that opens wins. On failure err tells the user what to install or set. */
static bool oci_load(char *err, size_t errlen) {
    if (oci.lib) return true;
    const char *env = SDL_getenv("LUME_ORACLE_LIB");
    const char *home = SDL_getenv("ORACLE_HOME");
    char homelib[1200] = "";
#ifdef __APPLE__
    if (home) snprintf(homelib, sizeof homelib, "%s/lib/libclntsh.dylib", home);
    /* ~/oracle/instantclient: a per-user Instant Client (no sudo needed) */
    char userlib[1024] = ""; const char *uh = SDL_getenv("HOME");
    if (uh) snprintf(userlib, sizeof userlib, "%s/oracle/instantclient/libclntsh.dylib", uh);
    const char *candidates[] = { env, homelib[0] ? homelib : NULL, userlib[0] ? userlib : NULL, "libclntsh.dylib", "/opt/oracle/instantclient/libclntsh.dylib", "/usr/local/lib/libclntsh.dylib", "/opt/homebrew/lib/libclntsh.dylib" };
#elif defined(_WIN32)
    if (home) snprintf(homelib, sizeof homelib, "%s\\bin\\oci.dll", home);
    const char *candidates[] = { env, homelib[0] ? homelib : NULL, "oci.dll" };
#else
    if (home) snprintf(homelib, sizeof homelib, "%s/lib/libclntsh.so", home);
    const char *candidates[] = { env, homelib[0] ? homelib : NULL, "libclntsh.so", "libclntsh.so.23.1", "libclntsh.so.21.1", "libclntsh.so.19.1", "/opt/oracle/instantclient/libclntsh.so" };
#endif
    for (size_t i = 0; i < sizeof candidates / sizeof *candidates; i++) {
        if (!candidates[i]) continue;
        oci.lib = DLOPEN(candidates[i]);
        if (oci.lib) break;
    }
    if (!oci.lib) {
        snprintf(err, errlen, "Oracle client not found. Install Oracle Instant Client and set LUME_ORACLE_LIB (or ORACLE_HOME) to it.");
        return false;
    }
#define LOAD(field, name) do { oci.field = DLSYM(oci.lib, name); if (!oci.field) { snprintf(err, errlen, "Oracle client: missing %s", name); return false; } } while (0)
    LOAD(EnvNlsCreate, "OCIEnvNlsCreate"); LOAD(HandleAlloc, "OCIHandleAlloc"); LOAD(HandleFree, "OCIHandleFree");
    LOAD(Logon2, "OCILogon2"); LOAD(Logoff, "OCILogoff"); LOAD(StmtPrepare2, "OCIStmtPrepare2"); LOAD(StmtRelease, "OCIStmtRelease");
    LOAD(StmtExecute, "OCIStmtExecute"); LOAD(AttrGet, "OCIAttrGet"); LOAD(AttrSet, "OCIAttrSet"); LOAD(ParamGet, "OCIParamGet");
    LOAD(DefineByPos, "OCIDefineByPos"); LOAD(StmtFetch2, "OCIStmtFetch2"); LOAD(ErrorGet, "OCIErrorGet"); LOAD(Break, "OCIBreak"); LOAD(DescriptorFree, "OCIDescriptorFree");
#undef LOAD
    void (*cv)(int *, int *, int *, int *, int *) = (void (*)(int *, int *, int *, int *, int *))DLSYM(oci.lib, "OCIClientVersion");
    int a = 0, b = 0, c = 0, d = 0, e = 0;
    if (cv) { cv(&a, &b, &c, &d, &e); oci.major = a; }
    return true;
}

/* Copies the first error record of an error handle (e.g. "ORA-00942: table or view does not
 * exist") into out, without trailing newlines. */
static void ora_error(OCIError *errh, char *out, size_t n) {
    sb4 code = 0;
    OraText buf[1024] = "";
    if (errh) oci.ErrorGet(errh, 1, NULL, &code, buf, sizeof buf, OCI_HTYPE_ERROR);
    snprintf(out, n, "%s", buf[0] ? (char *)buf : "Oracle error");
    size_t l = strlen(out);
    while (l && (out[l - 1] == '\n' || out[l - 1] == ' ')) out[--l] = 0;
}

/* forward declarations: ora_open uses ora_query, ora_cancel uses ora_exec / ora_close */
static bool ora_query(Db *db, const char *sql, ResultSet *rs, char *err, size_t errlen);
static void ora_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs);
static void ora_close(Db *db);

/* open: parses the three-line location, creates a threaded UTF-8 environment, logs on and
 * looks up the session's "sid,serial#" for the cancel fallback. Blocks until connected. */
/* open with a limit on establishing the connection (connect_s seconds), see ora_open */
static bool ora_open_within(Db *db, const char *location, char *err, size_t errlen, int connect_s) {
    if (!oci_load(err, errlen)) return false;
    /* location: "user\npassword\n//host:port/service" */
    char user[256] = "", pass[256] = "", dsn[512] = "";
    const char *p = location, *nl = strchr(p, '\n');
    if (!nl) { snprintf(err, errlen, "Bad Oracle location"); return false; }
    snprintf(user, sizeof user, "%.*s", (int)(nl - p), p);
    p = nl + 1; nl = strchr(p, '\n');
    if (!nl) { snprintf(err, errlen, "Bad Oracle location"); return false; }
    snprintf(pass, sizeof pass, "%.*s", (int)(nl - p), p);
    snprintf(dsn, sizeof dsn, "%s", nl + 1);
    /* EZConnect Plus (client 19c+; an older client would refuse the '?'): a limit on the TCP
     * connect and on the whole handshake, so a host that accepts and never answers (a dropped VPN,
     * a frozen server) fails after connect_s instead of blocking for minutes. A TNS alias or a
     * descriptor is left alone. */
    if (oci.major >= 19 && !strncmp(dsn, "//", 2) && !strchr(dsn, '?') && connect_s > 0) {
        size_t l = strlen(dsn);
        snprintf(dsn + l, sizeof dsn - l, "?connect_timeout=%d&transport_connect_timeout=%d&retry_count=0", connect_s, connect_s);
    }

    OraConn *c = calloc(1, sizeof *c);
    /* OCI_THREADED: the executor thread runs statements while the UI thread may call OCIBreak */
    if (oci.EnvNlsCreate(&c->env, OCI_THREADED, NULL, NULL, NULL, NULL, 0, NULL, OCI_UTF8_CHARSET, OCI_UTF8_CHARSET) != OCI_SUCCESS) {
        snprintf(err, errlen, "OCIEnvNlsCreate failed"); free(c); return false;
    }
    oci.HandleAlloc(c->env, (void **)&c->err, OCI_HTYPE_ERROR, 0, NULL);
    oci.HandleAlloc(c->env, (void **)&c->errb, OCI_HTYPE_ERROR, 0, NULL);
    sword rc = oci.Logon2(c->env, c->err, &c->svc, (OraText *)user, (ub4)strlen(user), (OraText *)pass, (ub4)strlen(pass),
                          (OraText *)dsn, (ub4)strlen(dsn), OCI_DEFAULT);
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) {
        ora_error(c->err, err, errlen);
        oci.HandleFree(c->err, OCI_HTYPE_ERROR);
        oci.HandleFree(c->env, OCI_HTYPE_ENV);
        free(c);
        return false;
    }
    db->handle = c;
    /* session id for the cancel fallback (needs v$session; silently absent otherwise) */
    { ResultSet r; char e[200];
      if (ora_query(db, "SELECT sid || ',' || serial# FROM v$session WHERE sid = SYS_CONTEXT('USERENV', 'SID')", &r, e, sizeof e) && r.nrows && rs_cell(&r, 0, 0))
          snprintf(c->sid, sizeof c->sid, "%s", rs_cell(&r, 0, 0));
      rs_free(&r); }
    return true;
}
/* open: "user\npassword\n//host:port/service"; the connect limit is the Settings one. */
static bool ora_open(Db *db, const char *location, char *err, size_t errlen) {
    return ora_open_within(db, location, err, errlen, db_connect_timeout_s > 0 ? db_connect_timeout_s : 10);
}

/* close: logs off and frees every handle. Freeing the environment last also releases any
 * handle allocated from it. The logoff is a round trip: when the network died under the session
 * (a VPN dropped) nothing answers and nothing resets the socket, so without a limit it blocks
 * until TCP gives up, minutes later, with the window frozen. A call timeout makes it give up
 * after a few seconds (ORA-03156); the handles are freed either way. */
static void ora_close(Db *db) {
    OraConn *c = db->handle;
    if (!c) return;
    if (c->svc) {
        ub4 ms = 3000;
        oci.AttrSet(c->svc, OCI_HTYPE_SVCCTX, &ms, 0, OCI_ATTR_CALL_TIMEOUT, c->err);
        sword rc = oci.Logoff(c->svc, c->err);
        if (rc != OCI_SUCCESS) { char e[256]; ora_error(c->err, e, sizeof e); SDL_Log("Oracle logoff: rc %d: %s", rc, e); }
    }
    if (c->err) oci.HandleFree(c->err, OCI_HTYPE_ERROR);
    if (c->errb) oci.HandleFree(c->errb, OCI_HTYPE_ERROR);
    if (c->env) oci.HandleFree(c->env, OCI_HTYPE_ENV);
    free(c);
}

/* An open SELECT: the statement plus one define buffer per column. Stored in rs->cursor. */
typedef struct OraCursor {
    OCIStmt *st;
    ub4 ncols;   /* number of select-list columns */
    char **bufs; sb2 *inds; ub2 *lens; bool *lob;   /* per column: text buffer (NULL for LOBs), null indicator (-1 = NULL), fetched length, is a LOB */
} OraCursor;

/* Frees the define buffers and releases the statement. NULL is accepted. */
static void ora_cursor_free(OraConn *c, OraCursor *cur) {
    if (!cur) return;
    for (ub4 i = 0; i < cur->ncols; i++) free(cur->bufs[i]);
    free(cur->bufs); free(cur->inds); free(cur->lens); free(cur->lob);
    if (cur->st) oci.StmtRelease(cur->st, c->err, NULL, 0, OCI_DEFAULT);
    free(cur);
}

/* Type name and display kind (CK_*) for the OCI external data type codes we recognise; ""
 * and CK_OTHER otherwise. */
static const char *ora_type_name(ub2 dtype, int *kind) {
    switch (dtype) {
    case 1: *kind = CK_TEXT; return "VARCHAR2";
    case 96: *kind = CK_TEXT; return "CHAR";
    case 2: *kind = CK_NUMBER; return "NUMBER";
    case 100: *kind = CK_NUMBER; return "BINARY_FLOAT";
    case 101: *kind = CK_NUMBER; return "BINARY_DOUBLE";
    case 12: *kind = CK_DATE; return "DATE";
    case 187: *kind = CK_DATE; return "TIMESTAMP";
    case 188: *kind = CK_DATE; return "TIMESTAMP WITH TIME ZONE";
    case 232: *kind = CK_DATE; return "TIMESTAMP WITH LOCAL TIME ZONE";
    case 8: *kind = CK_TEXT; return "LONG";
    case 23: *kind = CK_BINARY; return "RAW";
    case 112: *kind = CK_TEXT; return "CLOB";
    case 113: *kind = CK_BINARY; return "BLOB";
    case 114: *kind = CK_BINARY; return "BFILE";
    case 69: *kind = CK_TEXT; return "ROWID";
    default: *kind = CK_OTHER; return "";
    }
}

/* Appends up to n rows (n <= 0: all). Returns OCI_NO_DATA at the end, OCI_SUCCESS when stopped
 * by n, anything else on error (err filled). */
static sword ora_pull(OraConn *c, OraCursor *cur, ResultSet *rs, int n, char *err, size_t errlen) {
    for (int i = 0; n <= 0 || i < n; i++) {
        sword rc = oci.StmtFetch2(cur->st, c->err, 1, OCI_FETCH_NEXT, 0, OCI_DEFAULT);
        if (rc == OCI_NO_DATA) return OCI_NO_DATA;
        if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO) { ora_error(c->err, err, errlen); return rc; }
        int row = rs_add_row(rs);
        if (row < 0) return OCI_SUCCESS;
        for (ub4 k = 0; k < cur->ncols; k++) {
            if (cur->lob[k]) rs_set(rs, row, k, "<lob>", -1);
            else rs_set(rs, row, k, cur->inds[k] == -1 ? NULL : cur->bufs[k], cur->inds[k] == -1 ? 0 : cur->lens[k]);
        }
    }
    return OCI_SUCCESS;
}

/* Runs one statement. Row-returning statements fetch up to `limit` rows and may leave an open
 * cursor in rs->cursor. Returns false with err filled on error. */
/* Steps: prepare, read the statement type, execute (DML commits unless manual_tx), then
 * either record the affected rows or, for a SELECT, describe the columns, define the text
 * buffers and pull the first rows. Timing accumulates into rs->exec_ms / fetch_ms. */
static bool ora_run(Db *db, const char *sql, int limit, ResultSet *rs, char *err, size_t errlen) {
    OraConn *c = db->handle;
    OCIStmt *st = NULL;
    sword rc = oci.StmtPrepare2(c->svc, &st, c->err, (const OraText *)sql, (ub4)strlen(sql), NULL, 0, OCI_NTV_SYNTAX, OCI_DEFAULT);
    if (rc != OCI_SUCCESS) { ora_error(c->err, err, errlen); return false; }
    ub2 type = 0;
    oci.AttrGet(st, OCI_HTYPE_STMT, &type, NULL, OCI_ATTR_STMT_TYPE, c->err);
    bool select = type == OCI_STMT_SELECT;
    ub4 prefetch = 200;
    oci.AttrSet(st, OCI_HTYPE_STMT, &prefetch, 0, OCI_ATTR_PREFETCH_ROWS, c->err);
    /* SET TRANSACTION (READ ONLY, ISOLATION LEVEL...) starts a transaction that commit-on-success would
     * end at once: from it until COMMIT / ROLLBACK nothing is auto-committed (a read-only transaction
     * then really refuses writes, ORA-01456) */
    const char *k = sql; while (*k == ' ' || *k == '\t' || *k == '\n' || *k == '\r') k++;
    bool set_tx = !strncasecmp(k, "SET TRANSACTION", 15);
    bool ends_tx = (!strncasecmp(k, "COMMIT", 6) || (!strncasecmp(k, "ROLLBACK", 8) && !strstr(k, "SAVEPOINT") && !strcasestr(k, " TO ")));
    if (set_tx) c->explicit_tx = true;
    Uint64 t0 = SDL_GetPerformanceCounter();
    rc = oci.StmtExecute(c->svc, st, c->err, select ? 0 : 1, 0, NULL, NULL, select || c->manual_tx || c->explicit_tx ? OCI_DEFAULT : OCI_COMMIT_ON_SUCCESS);
    if (ends_tx || (set_tx && rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO)) c->explicit_tx = false;
    rs->exec_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (rc != OCI_SUCCESS && rc != OCI_SUCCESS_WITH_INFO && rc != OCI_NO_DATA) {
        ora_error(c->err, err, errlen);
        oci.StmtRelease(st, c->err, NULL, 0, OCI_DEFAULT);
        return false;
    }
    if (!select) {
        ub4 rows = 0;
        oci.AttrGet(st, OCI_HTYPE_STMT, &rows, NULL, OCI_ATTR_ROW_COUNT, c->err);
        rs->affected += (int)rows;
        oci.StmtRelease(st, c->err, NULL, 0, OCI_DEFAULT);
        return true;
    }
    /* SELECT: describe each column and bind a text buffer for it */
    OraCursor *cur = calloc(1, sizeof *cur);
    cur->st = st;
    oci.AttrGet(st, OCI_HTYPE_STMT, &cur->ncols, NULL, OCI_ATTR_PARAM_COUNT, c->err);
    cur->bufs = calloc(cur->ncols, sizeof *cur->bufs);
    cur->inds = calloc(cur->ncols, sizeof *cur->inds);
    cur->lens = calloc(cur->ncols, sizeof *cur->lens);
    cur->lob = calloc(cur->ncols, sizeof *cur->lob);
    for (ub4 i = 0; i < cur->ncols; i++) {
        void *parm = NULL;
        oci.ParamGet(st, OCI_HTYPE_STMT, c->err, &parm, i + 1);
        OraText *name = NULL; ub4 nlen = 0; ub2 dtype = 0;
        oci.AttrGet(parm, OCI_DTYPE_PARAM, &name, &nlen, OCI_ATTR_NAME, c->err);
        oci.AttrGet(parm, OCI_DTYPE_PARAM, &dtype, NULL, OCI_ATTR_DATA_TYPE, c->err);
        char cname[256];
        snprintf(cname, sizeof cname, "%.*s", (int)nlen, name ? (char *)name : "");
        int kind;
        const char *tname = ora_type_name(dtype, &kind);
        rs_add_column(rs, cname, tname, kind);
        oci.DescriptorFree(parm, OCI_DTYPE_PARAM);
        cur->lob[i] = dtype == SQLT_CLOB || dtype == SQLT_BLOB || dtype == SQLT_BFILE;
        if (cur->lob[i]) continue;
        cur->bufs[i] = malloc(CELL_MAX + 1);
        OCIDefine *def = NULL;
        if (oci.DefineByPos(st, &def, c->err, i + 1, cur->bufs[i], CELL_MAX + 1, SQLT_STR, &cur->inds[i], &cur->lens[i], NULL, OCI_DEFAULT) != OCI_SUCCESS) {
            ora_error(c->err, err, errlen);
            ora_cursor_free(c, cur);
            return false;
        }
    }
    /* first batch of rows; leftover rows stay behind an open cursor */
    rs->has_rows = true;
    Uint64 t1 = SDL_GetPerformanceCounter();
    rc = ora_pull(c, cur, rs, limit, err, errlen);
    rs->fetch_ms += (SDL_GetPerformanceCounter() - t1) * 1000.0 / SDL_GetPerformanceFrequency();
    if (rc == OCI_NO_DATA) { rs->complete = true; ora_cursor_free(c, cur); return true; }
    if (rc != OCI_SUCCESS) { ora_cursor_free(c, cur); return false; }
    rs->truncated = true;
    rs->complete = false;
    rs->cursor = cur;
    return true;
}

/* release: closes the cursor of rs, if any. */
static void ora_release(Db *db, ResultSet *rs) {
    if (rs->cursor) ora_cursor_free((OraConn *)db->handle, (OraCursor *)rs->cursor);
    rs->cursor = NULL;
}

/* fetch: pulls up to n more rows into rs; closes the cursor at the end or on error. Returns
 * true when rows were added. Worker thread. */
static bool ora_fetch(Db *db, ResultSet *rs, int n) {
    if (!rs->cursor) return false;
    OraConn *c = db->handle;
    int before = rs->nrows;
    Uint64 t0 = SDL_GetPerformanceCounter();
    sword rc = ora_pull(c, (OraCursor *)rs->cursor, rs, n, rs->error, sizeof rs->error);
    rs->fetch_ms += (SDL_GetPerformanceCounter() - t0) * 1000.0 / SDL_GetPerformanceFrequency();
    if (rc == OCI_NO_DATA) { ora_release(db, rs); rs->complete = true; rs->truncated = false; }
    else if (rc != OCI_SUCCESS) { ora_release(db, rs); rs->ok = false; }
    return rs->nrows > before;
}

/* Metadata helper: runs a query and fetches everything. */
static bool ora_query(Db *db, const char *sql, ResultSet *rs, char *err, size_t errlen) {
    rs_init(rs);
    rs->db = db;
    return ora_run(db, sql, 0, rs, err, errlen);
}

/* list_tables: the user's own tables, then views, each alphabetical. */
static bool ora_list_tables(Db *db, DbTable **out, int *count, char *err, size_t errlen) {
    ResultSet rs;
    if (!ora_query(db, "SELECT table_name, 'TABLE' FROM user_tables UNION ALL SELECT view_name, 'VIEW' FROM user_views ORDER BY 2, 1", &rs, err, errlen)) { rs_free(&rs); return false; }
    DbTable *tables = rs.nrows ? calloc(rs.nrows, sizeof *tables) : NULL;
    for (int i = 0; i < rs.nrows; i++) {
        const char *n = rs_cell(&rs, i, 0), *t = rs_cell(&rs, i, 1);
        tables[i].name = db_strdup(n ? n : "");
        tables[i].is_view = t && strcmp(t, "VIEW") == 0;
    }
    *out = tables; *count = rs.nrows;
    rs_free(&rs);
    return true;
}

/* Writes s as a single-quoted SQL literal with quotes doubled, truncating to fit n. */
static void sql_literal(const char *s, char *out, size_t n) {
    size_t p = 0;
    out[p++] = '\'';
    for (; *s && p < n - 3; s++) { if (*s == '\'') out[p++] = '\''; out[p++] = *s; }
    out[p++] = '\''; out[p] = 0;
}

/* "OWNER.TABLE" -> owner and table literals; a bare name means the current schema. Lets the
 * catalog queries below work on tables of other schemas the user was granted access to
 * (the user_* views only ever show the user's own objects). */
static void owner_and_name(const char *table, char *owner_lit, size_t on, char *name_lit, size_t nn) {
    const char *dot = strchr(table, '.');
    if (dot && dot != table && dot[1]) {
        char owner[256];
        snprintf(owner, sizeof owner, "%.*s", (int)(dot - table), table);
        sql_literal(owner, owner_lit, on);
        sql_literal(dot + 1, name_lit, nn);
    } else {
        snprintf(owner_lit, on, "SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA')");
        sql_literal(table, name_lit, nn);
    }
}

/* Columns of a table: name, type, NOT NULL, part of the primary key, virtual, identity ALWAYS. */
static bool ora_list_columns(Db *db, DbTable *t, char *err, size_t errlen) {
    char own[520], lit[520], sql[1600];
    owner_and_name(t->name, own, sizeof own, lit, sizeof lit);
    snprintf(sql, sizeof sql,
        "SELECT c.column_name, c.data_type, c.nullable, "
        "(SELECT COUNT(*) FROM all_constraints k JOIN all_cons_columns kc ON kc.owner = k.owner AND kc.constraint_name = k.constraint_name "
        " WHERE k.constraint_type = 'P' AND k.owner = c.owner AND k.table_name = c.table_name AND kc.column_name = c.column_name), "
        "c.virtual_column, "
        "(SELECT COUNT(*) FROM all_tab_identity_cols ic WHERE ic.owner = c.owner AND ic.table_name = c.table_name AND ic.column_name = c.column_name AND ic.generation_type = 'ALWAYS') "
        "FROM all_tab_cols c WHERE c.owner = %s AND c.table_name = %s AND c.hidden_column = 'NO' ORDER BY c.column_id", own, lit);
    ResultSet rs;
    if (!ora_query(db, sql, &rs, err, errlen)) { rs_free(&rs); return false; }
    t->cols = rs.nrows ? calloc(rs.nrows, sizeof *t->cols) : NULL;
    for (int i = 0; i < rs.nrows; i++) {
        const char *n = rs_cell(&rs, i, 0), *ty = rs_cell(&rs, i, 1), *nn = rs_cell(&rs, i, 2), *pk = rs_cell(&rs, i, 3);
        t->cols[i].name = db_strdup(n ? n : "");
        t->cols[i].type = db_strdup(ty ? ty : "any");
        t->cols[i].notnull = nn && nn[0] == 'N';
        t->cols[i].pk = pk && atoi(pk) > 0;
        const char *virt = rs_cell(&rs, i, 4), *ident = rs_cell(&rs, i, 5);
        t->cols[i].generated = virt && virt[0] == 'Y';
        t->cols[i].identity_always = ident && atoi(ident) > 0;
    }
    t->ncols = rs.nrows;
    t->cols_loaded = true;
    rs_free(&rs);
    return true;
}

/* ad-hoc statements commit on success (what the editor expects); the edit batch turns that
 * off so its UPDATE / INSERT / DELETE stay in one transaction until COMMIT (US-049) */
static void ora_set_autocommit(Db *db, bool on) { ((OraConn *)db->handle)->manual_tx = !on; }

/* outgoing foreign keys, simple and composite, through the data dictionary (US-058); a
 * referenced table in another schema is returned as OWNER.TABLE so quote_ident splits it */
/* Foreign keys of a table, one row per column pair in key order. The referenced side comes
 * from all_constraints, which hides tables this user cannot read: such a key is still listed,
 * with an empty to_table, so the UI can say why it cannot be followed instead of hiding it. */
static bool ora_list_fks(Db *db, const char *table, DbFk **out, int *count, char *err, size_t errlen) {
    char own[520], lit[520], sql[2000];
    owner_and_name(table, own, sizeof own, lit, sizeof lit);
    snprintf(sql, sizeof sql,
        "SELECT c.constraint_name, cc.column_name, r.owner, r.table_name, rc.column_name, "
        "CASE WHEN r.owner = SYS_CONTEXT('USERENV', 'CURRENT_SCHEMA') THEN 'Y' ELSE 'N' END "
        "FROM all_constraints c "
        "JOIN all_cons_columns cc ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name AND cc.table_name = c.table_name "
        "LEFT JOIN all_constraints r ON r.constraint_name = c.r_constraint_name AND r.owner = c.r_owner "
        "LEFT JOIN all_cons_columns rc ON rc.constraint_name = r.constraint_name AND rc.owner = r.owner AND rc.position = cc.position "
        "WHERE c.constraint_type = 'R' AND c.owner = %s AND c.table_name = %s ORDER BY c.constraint_name, cc.position", own, lit);
    ResultSet rs;
    if (!ora_query(db, sql, &rs, err, errlen)) { rs_free(&rs); return false; }
    DbFk *fks = rs.nrows ? calloc(rs.nrows, sizeof *fks) : NULL;
    for (int i = 0; i < rs.nrows; i++) {
        const char *nm = rs_cell(&rs, i, 0), *from = rs_cell(&rs, i, 1), *own = rs_cell(&rs, i, 2), *tn = rs_cell(&rs, i, 3), *to = rs_cell(&rs, i, 4), *same = rs_cell(&rs, i, 5);
        char full[520];
        if (!tn) full[0] = 0;                                   /* referenced table not visible */
        else if (same && same[0] == 'Y') snprintf(full, sizeof full, "%s", tn); else snprintf(full, sizeof full, "%s.%s", own ? own : "", tn);
        fks[i].name = db_strdup(nm ? nm : ""); fks[i].from_col = db_strdup(from ? from : ""); fks[i].to_table = db_strdup(full); fks[i].to_col = db_strdup(to ? to : "");
    }
    *out = fks; *count = rs.nrows;
    rs_free(&rs);
    return true;
}

/* exec: runs ONE statement (len bytes, or NUL-terminated when len < 0). The ORA- number of a
 * failure goes to rs->err_code and ORA-01013 marks the result as cancelled. Worker thread. */
static void ora_exec(Db *db, const char *sql, int len, int limit, ResultSet *rs) {
    rs_init(rs);
    rs->db = db;
    if (len < 0) len = (int)strlen(sql);
    char *text = malloc(len + 1);
    memcpy(text, sql, len);
    text[len] = 0;
    /* Oracle takes one statement without the trailing ';' (PL/SQL blocks keep theirs). */
    int n = len;
    while (n > 0 && isspace((unsigned char)text[n - 1])) n--;
    const char *s = text;
    while (*s && isspace((unsigned char)*s)) s++;
    bool plsql = strncasecmp(s, "BEGIN", 5) == 0 || strncasecmp(s, "DECLARE", 7) == 0 || strncasecmp(s, "CREATE OR REPLACE", 17) == 0;
    if (!plsql && n > 0 && text[n - 1] == ';') n--;
    text[n] = 0;
    rs->ok = ora_run(db, text, limit, rs, rs->error, sizeof rs->error);
    if (rs->ok && !rs->has_rows) rs->complete = true;
    if (!rs->ok) {
        long code = -1;
        if (sscanf(rs->error, "ORA-%ld", &code) == 1) rs->err_code = code;
        rs->cancelled = code == 1013;                 /* ORA-01013: user requested cancel */
    }
    free(text);
}

/* Two routes (OUT-018): OCIBreak reaches a statement that polls for breaks; when out-of-band
 * breaks are disabled (sqlnet.ora DISABLE_OOB, common in containers) a sleeping or blocked
 * statement never sees it, so a second short-lived session asks the server to cancel ours
 * (ALTER SYSTEM CANCEL SQL 'sid,serial#'). The second route needs that privilege and the
 * session id; without them only the break is sent. */
static void ora_cancel(Db *db) {
    OraConn *c = db->handle;
    if (!c || !c->svc) return;
    sword rc = oci.Break(c->svc, c->errb ? c->errb : c->err);
    if (rc != OCI_SUCCESS) { char e[256]; ora_error(c->errb ? c->errb : c->err, e, sizeof e); SDL_Log("OCIBreak: rc %d: %s", rc, e); }
    if (c->sid[0] && db->location) {
        Db tmp = { .drv = db->drv, .location = db->location };
        char e[256];
        /* a short limit: a cancel on a network that died (a dropped VPN) must not block the caller */
        if (ora_open_within(&tmp, db->location, e, sizeof e, 5)) {
            char sql[160]; snprintf(sql, sizeof sql, "ALTER SYSTEM CANCEL SQL '%s'", c->sid);
            ResultSet r; ora_exec(&tmp, sql, -1, 0, &r);
            if (!r.ok) SDL_Log("cancel fallback: %s", r.error);
            rs_free(&r);
            ora_close(&tmp);
        }
    }
}

/* select_sql: SELECT * ... [WHERE ...] [ORDER BY ...] FETCH FIRST n ROWS ONLY (12c+). */
static void ora_select(const char *q, const char *where, const char *order, int limit, char *buf, size_t n) {
    snprintf(buf, n, "SELECT * FROM %s%s%s%s%s FETCH FIRST %d ROWS ONLY", q, where && *where ? " WHERE " : "", where ? where : "",
             order && *order ? " ORDER BY " : "", order ? order : "", limit);
}

/* Driver table, in struct DbDriver order: no rehome (OraCursor does not point back at the
 * ResultSet) and no tx_begin (Oracle opens transactions implicitly). */
/* OCI loads (and the client version from OCIClientVersion when exported). Never connects. */
static bool ora_probe(char *v, size_t n) {
    char err[256];
    if (!oci_load(err, sizeof err)) { snprintf(v, n, "client library not found"); return false; }
    if (oci.major) snprintf(v, n, "Oracle client %d", oci.major);
    else snprintf(v, n, "Oracle client (version not reported)");
    return true;
}

const DbDriver db_oracle_driver = {
    "Oracle", ora_open, ora_close, ora_list_tables, ora_list_columns, ora_exec, ora_fetch, ora_release, NULL, db_quote_ident_double, ora_select,
    DB_CAP_FETCH_MORE | DB_CAP_CANCEL | DB_CAP_SERVER_SORT | DB_CAP_SERVER_FILTER, NULL, ora_list_fks, ora_set_autocommit, ora_cancel, NULL,   /* implicit transactions; tx state not exposed */
    ora_probe,
};
