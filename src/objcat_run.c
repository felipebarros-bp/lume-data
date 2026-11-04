/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object catalog, runtime half: the catalog session (a second session on a worker), the disk
 * cache, column requests and the hand-over to the completer and the Explorer. See objcat.h. */
#include "objcat.h"
#include "complete.h"
#include "conn.h"
#include "db.h"
#include "editor.h"
#include "explorer.h"
#include "log.h"
#include "rs.h"
#include "sshtun.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

/* A request to the catalog session: RQ_COLUMNS (`key` names the table as asked, lower-case
 * "schema.name"; `query` the object whose columns are read, a synonym's target) or RQ_MEMBERS
 * (`key` the package, `query` the SQL listing its subprograms). */
enum { RQ_COLUMNS, RQ_MEMBERS, RQ_FKS };
typedef struct Req { int kind; char key[300]; char query[2400]; } Req;
typedef struct Res { int kind; char key[300]; DbTable t; OcMember *m; int nm; DbFk *fks; int nfks; char err[200]; } Res;
static void res_free(Res *r);

/* The catalog session. Shared by the main thread and the worker; the last one to let go frees it
 * (refs: 2 at start, each side drops one). The worker reads the object list once (unless
 * skip_catalog), then serves column requests until cancelled or the session fails. */
typedef struct Job {
    Connection *copy;          /* a plain description of the connection (runtime pointers cleared); wiped after the open */
    char driver[16];
    bool skip_catalog;         /* reopened for columns only (the object list is already here) */
    ObjCat cat;                /* filled by the worker; the main thread takes it once cat_ready */
    char err[300];
    volatile int cancel, done, refs, cat_ready, open_ok, inflight;   /* inflight: the session is inside a statement (a cancel reaches it); idle otherwise */
    volatile int pending;      /* requests enqueued and not yet answered: the app polls only while this is not 0 (an idle session costs no wake-ups) */
    Db *volatile db;           /* the session while it is open (cancel reaches its statement) */
    SDL_Mutex *mu;             /* guards db, the queues */
    SDL_Condition *cv;         /* a request arrived / cancel */
    Req *req; int nreq, capreq;
    Res *res; int nres, capres;
} Job;

typedef struct ColT { char key[300]; DbTable *t; int state; } ColT;   /* state OCS_* */
typedef struct MemT { char key[300]; OcMembers *ms; } MemT;             /* a package's members (state inside) */
typedef struct FkT { char key[300]; DbFk *fks; int n; int state; } FkT;   /* a table's foreign keys, state OCS_* */
typedef struct Entry {
    char conn_id[16]; unsigned generation;
    ObjCat cat; bool have;     /* the catalog in use (cache or last read) */
    Job *job;                  /* the catalog session, while it runs */
    bool tried;                /* a read was started for this generation (once per connect) */
    bool cat_taken;            /* this generation's object list arrived */
    char state[160];
    double t0;
    ColT *ct; int nct, capct;  /* tables whose columns were asked for (stable DbTable pointers) */
    MemT *mt; int nmt, capmt;  /* packages whose members were asked for (stable pointers) */
    FkT *ft; int nft, capft;   /* tables whose foreign keys were asked for */
} Entry;
static Entry *E; static int nE, capE;
static App *A;

/* "36,323": a count with thousands separators, like the grid's row counts */
static void thousands(char *o, size_t n, int v) {
    char raw[24]; int l = snprintf(raw, sizeof raw, "%d", v), k = 0;
    for (int i = 0; i < l && k + 1 < (int)n; i++) { if (i && (l - i) % 3 == 0 && raw[i - 1] != '-') o[k++] = ','; o[k++] = raw[i]; }
    o[k] = 0;
}
static void lower_key(char *o, size_t n, const char *schema, const char *name) {
    size_t k = 0;
    for (const char *p = schema ? schema : ""; *p && k + 2 < n; p++) o[k++] = (char)tolower((unsigned char)*p);
    if (schema && schema[0] && k + 2 < n) o[k++] = '.';
    for (const char *p = name; *p && k + 1 < n; p++) o[k++] = (char)tolower((unsigned char)*p);
    o[k] = 0;
}
static void table_free(DbTable *t) {
    if (!t) return;
    for (int i = 0; i < t->ncols; i++) { free(t->cols[i].name); free(t->cols[i].type); }
    free(t->cols); free(t->name);
}

static void res_free(Res *r) {
    table_free(&r->t);
    if (r->fks) db_fks_free(r->fks, r->nfks); r->fks = NULL; r->nfks = 0;
    for (int i = 0; i < r->nm; i++) { free(r->m[i].name); free(r->m[i].args); }
    free(r->m); r->m = NULL; r->nm = 0;
}
static void job_release(Job *j) {
    if (__atomic_sub_fetch(&j->refs, 1, __ATOMIC_SEQ_CST) > 0) return;
    objcat_free(&j->cat);
    if (j->copy) { memset(j->copy, 0, sizeof *j->copy); free(j->copy); }
    for (int i = 0; i < j->nres; i++) res_free(&j->res[i]);
    free(j->req); free(j->res);
    SDL_DestroyCondition(j->cv);
    SDL_DestroyMutex(j->mu);
    free(j);
}

static void read_catalog(Job *j, Db *db) {
    int nread = 0;
    for (int i = 0; !__atomic_load_n(&j->cancel, __ATOMIC_SEQ_CST); i++) {
        const char *sql = objcat_sql(j->driver, i, false);
        if (!sql) break;
        ResultSet rs; rs_init(&rs);
        __atomic_store_n(&j->inflight, 1, __ATOMIC_SEQ_CST);
        db->drv->exec(db, sql, (int)strlen(sql), 0, &rs);
        __atomic_store_n(&j->inflight, 0, __ATOMIC_SEQ_CST);
        if (!rs.ok && !__atomic_load_n(&j->cancel, __ATOMIC_SEQ_CST) && objcat_sql(j->driver, i, true)) {   /* an older server: the fallback */
            const char *alt = objcat_sql(j->driver, i, true);
            rs_free(&rs); rs_init(&rs);
            __atomic_store_n(&j->inflight, 1, __ATOMIC_SEQ_CST);
            db->drv->exec(db, alt, (int)strlen(alt), 0, &rs);
            __atomic_store_n(&j->inflight, 0, __ATOMIC_SEQ_CST);
        }
        if (!rs.ok) {
            if (i > 0 && !j->err[0]) snprintf(j->err, sizeof j->err, "%.280s", rs.error[0] ? rs.error : "the catalog query failed");
            rs_free(&rs); continue;   /* no current schema: everything qualified; a failed list: the others still come */
        }
        if (i == 0) { const char *h = rs.nrows ? rs_cell(&rs, 0, 0) : NULL; snprintf(j->cat.home, sizeof j->cat.home, "%s", h ? h : ""); }
        else for (int r = 0; r < rs.nrows && rs.ncols >= 3; r++) {
            objcat_add(&j->cat, rs_cell(&rs, r, 0), rs_cell(&rs, r, 1), objcat_kind(rs_cell(&rs, r, 2)), rs.ncols > 3 ? rs_cell(&rs, r, 3) : NULL);
            nread++;
        }
        rs_free(&rs);
    }
    objcat_finish(&j->cat);
    if (nread) j->err[0] = 0;   /* a partial list is still a catalog */
}

static int worker(void *ud) {
    Job *j = ud;
    struct SshTunnel *tun = NULL;
    Db *db = conn_open(j->copy, &tun, j->err, sizeof j->err);
    memset(j->copy, 0, sizeof *j->copy); free(j->copy); j->copy = NULL;   /* the password does not outlive the open */
    if (db) {
        __atomic_store_n(&j->open_ok, 1, __ATOMIC_SEQ_CST);
        SDL_LockMutex(j->mu); j->db = db; SDL_UnlockMutex(j->mu);
        if (!j->skip_catalog) read_catalog(j, db);
        __atomic_store_n(&j->cat_ready, 1, __ATOMIC_SEQ_CST);
        /* then columns, one table at a time, until cancelled */
        for (;;) {
            SDL_LockMutex(j->mu);
            while (!j->nreq && !__atomic_load_n(&j->cancel, __ATOMIC_SEQ_CST)) SDL_WaitConditionTimeout(j->cv, j->mu, 1000);
            if (__atomic_load_n(&j->cancel, __ATOMIC_SEQ_CST)) { SDL_UnlockMutex(j->mu); break; }
            Req r = j->req[0];
            memmove(j->req, j->req + 1, sizeof *j->req * (size_t)(j->nreq - 1)); j->nreq--;
            SDL_UnlockMutex(j->mu);
            Res out; memset(&out, 0, sizeof out);
            out.kind = r.kind;
            snprintf(out.key, sizeof out.key, "%s", r.key);
            __atomic_store_n(&j->inflight, 1, __ATOMIC_SEQ_CST);
            if (r.kind == RQ_COLUMNS) {
                out.t.name = strdup(r.query);
                if (!db->drv->list_columns(db, &out.t, out.err, sizeof out.err) && !out.err[0]) snprintf(out.err, sizeof out.err, "the columns could not be read");
                if (!out.err[0]) out.t.cols_loaded = true;
            } else if (r.kind == RQ_FKS) {
                if (!db->drv->list_fks) snprintf(out.err, sizeof out.err, "this driver lists no foreign keys");
                else if (!db->drv->list_fks(db, r.query, &out.fks, &out.nfks, out.err, sizeof out.err) && !out.err[0]) snprintf(out.err, sizeof out.err, "the foreign keys could not be read");
            } else {
                ResultSet rs; rs_init(&rs);
                db->drv->exec(db, r.query, (int)strlen(r.query), 0, &rs);
                if (!rs.ok) snprintf(out.err, sizeof out.err, "%.190s", rs.error[0] ? rs.error : "the members could not be read");
                else if (rs.ncols >= 3 && rs.nrows) {
                    out.m = calloc((size_t)rs.nrows, sizeof *out.m);
                    for (int i = 0; out.m && i < rs.nrows; i++) {
                        const char *nm = rs_cell(&rs, i, 0), *kw = rs_cell(&rs, i, 1), *ar = rs_cell(&rs, i, 2);
                        out.m[out.nm].name = strdup(nm ? nm : "");
                        out.m[out.nm].args = strdup(ar ? ar : "");
                        out.m[out.nm].function = kw && !strcmp(kw, "FUNCTION");
                        out.nm++;
                    }
                }
                rs_free(&rs);
            }
            __atomic_store_n(&j->inflight, 0, __ATOMIC_SEQ_CST);
            SDL_LockMutex(j->mu);
            if (j->nres == j->capres) { int nc = j->capres ? j->capres * 2 : 16; Res *nr = realloc(j->res, sizeof *nr * (size_t)nc); if (nr) { j->res = nr; j->capres = nc; } }
            if (j->nres < j->capres) j->res[j->nres++] = out; else res_free(&out);
            SDL_UnlockMutex(j->mu);
            __atomic_sub_fetch(&j->pending, 1, __ATOMIC_SEQ_CST);
        }
        SDL_LockMutex(j->mu); j->db = NULL; SDL_UnlockMutex(j->mu);
        db_close(db);
        sshtun_close(tun);
    } else __atomic_store_n(&j->cat_ready, 1, __ATOMIC_SEQ_CST);
    __atomic_store_n(&j->done, 1, __ATOMIC_SEQ_CST);
    job_release(j);
    return 0;
}

static Entry *entry_of(const Connection *c, bool make) {
    for (int i = 0; i < nE; i++) if (!strcmp(E[i].conn_id, c->id)) return &E[i];
    if (!make) return NULL;
    if (nE == capE) { int nc = capE ? capE * 2 : 8; Entry *ne = realloc(E, sizeof *ne * (size_t)nc); if (!ne) return NULL; E = ne; capE = nc; }
    Entry *e = &E[nE++]; memset(e, 0, sizeof *e);
    snprintf(e->conn_id, sizeof e->conn_id, "%s", c->id);
    return e;
}
static Connection *conn_of(App *a, const char *id) {
    for (int k = 0; a->conns && k < a->conns->n; k++) if (!strcmp(a->conns->items[k]->id, id)) return a->conns->items[k];
    return NULL;
}
static void cache_path(const Connection *c, char *o, size_t n) { snprintf(o, n, "%sobjcat-%s.tsv", A && A->conns ? A->conns->dir : "", c->id); }

/* the worker stops at its next step; a statement in flight is cancelled now (an idle session
 * is not: a cancel may open a second session, pointless here) */
static void job_abandon(Entry *e) {
    Job *j = e->job; if (!j) return;
    e->job = NULL;
    SDL_LockMutex(j->mu);
    __atomic_store_n(&j->cancel, 1, __ATOMIC_SEQ_CST);
    if (j->db && __atomic_load_n(&j->inflight, __ATOMIC_SEQ_CST)) db_cancel(j->db);
    SDL_SignalCondition(j->cv);
    SDL_UnlockMutex(j->mu);
    job_release(j);
}
/* columns and members asked for this connection are dropped (a new session may see other grants / DDL) */
static void columns_clear(Entry *e) {
    for (int i = 0; i < e->nct; i++) { table_free(e->ct[i].t); free(e->ct[i].t); }
    free(e->ct); e->ct = NULL; e->nct = e->capct = 0;
    for (int i = 0; i < e->nmt; i++) { objcat_members_free(e->mt[i].ms); free(e->mt[i].ms); }
    free(e->mt); e->mt = NULL; e->nmt = e->capmt = 0;
    for (int i = 0; i < e->nft; i++) if (e->ft[i].fks) db_fks_free(e->ft[i].fks, e->ft[i].n);
    free(e->ft); e->ft = NULL; e->nft = e->capft = 0;
}

static bool start_session(Entry *e, Connection *c, bool skip_catalog) {
    if (!objcat_sql(c->driver, 1, false)) return false;
    Job *j = calloc(1, sizeof *j); if (!j) return false;
    j->copy = malloc(sizeof *j->copy);
    j->mu = SDL_CreateMutex(); j->cv = SDL_CreateCondition();
    if (!j->copy || !j->mu || !j->cv) { free(j->copy); if (j->mu) SDL_DestroyMutex(j->mu); if (j->cv) SDL_DestroyCondition(j->cv); free(j); return false; }
    *j->copy = *c;
    j->copy->db = NULL; j->copy->tables = NULL; j->copy->ntables = 0; j->copy->attempt = NULL; j->copy->tunnel = NULL;   /* a plain description, no live state */
    snprintf(j->driver, sizeof j->driver, "%s", c->driver);
    j->skip_catalog = skip_catalog;
    j->refs = 2;
    SDL_Thread *th = SDL_CreateThread(worker, "lume-objcat", j);
    if (!th) { memset(j->copy, 0, sizeof *j->copy); free(j->copy); SDL_DestroyCondition(j->cv); SDL_DestroyMutex(j->mu); free(j); return false; }
    SDL_DetachThread(th);   /* never joined: quitting does not wait for a slow catalog */
    e->job = j; e->t0 = (double)SDL_GetTicks() / 1000.0;
    if (!skip_catalog) snprintf(e->state, sizeof e->state, "reading");
    LOG_I("object catalog: %s %s in a second session", skip_catalog ? "reopening the catalog session of" : "reading", c->name);
    return true;
}

static void on_close(Connection *c) {
    Entry *e = entry_of(c, false); if (!e) return;
    job_abandon(e);
    e->tried = false; e->cat_taken = false; e->state[0] = 0;
    columns_clear(e);
}

void objcat_init(App *a) { A = a; conn_add_close_hook(on_close); }

/* the entry for c in its current generation (a reconnect starts over) */
static Entry *entry_now(Connection *c) {
    Entry *e = entry_of(c, true); if (!e) return NULL;
    if (e->generation != c->generation) { job_abandon(e); e->generation = c->generation; e->tried = false; e->cat_taken = false; e->state[0] = 0; columns_clear(e); }
    return e;
}

const ObjCat *objcat_for(App *a, Connection *c) {
    A = a;
    if (!c || !a->catalog_session || !objcat_sql(c->driver, 1, false)) return NULL;   /* off in Settings: this schema's tables only */
    Entry *e = entry_now(c); if (!e) return NULL;
    if (!e->have) {   /* the cache of the last read, at once */
        char p[1200]; cache_path(c, p, sizeof p);
        if (objcat_load(&e->cat, p)) { e->have = true; LOG_I("object catalog: %d objects of %s from the cache", e->cat.n, c->name); }
    }
    if (c->db && !e->tried && !e->job) { e->tried = true; start_session(e, c, false); }
    return e->have ? &e->cat : NULL;
}

const char *objcat_state(Connection *c) { Entry *e = c ? entry_of(c, false) : NULL; return e ? e->state : ""; }

/* ---- columns ---- */

static const OcObj *find_obj(const ObjCat *k, const char *schema, const char *name) {
    const OcSchema *s = objcat_schema(k, schema, (int)strlen(schema));
    if (!s) return NULL;
    const OcObj *ci = NULL;
    for (int i = s->first; i < s->first + s->n; i++) {
        if (!strcmp(k->o[i].name, name)) return &k->o[i];
        if (!ci && !strcasecmp(k->o[i].name, name)) ci = &k->o[i];
    }
    return ci;
}
/* the object whose columns describe schema.name: a synonym's target, followed up to 5 steps */
static void column_source(const ObjCat *k, const char *schema, const char *name, char *out, size_t n) {
    char s[160], nm[160];
    snprintf(s, sizeof s, "%s", schema ? schema : ""); snprintf(nm, sizeof nm, "%s", name);
    for (int step = 0; k && step < 5; step++) {
        const OcObj *o = s[0] ? find_obj(k, s, nm) : NULL;
        if (!o || o->kind != OC_SYNONYM || !o->target) break;
        const char *dot = strchr(o->target, '.');
        if (!dot) break;
        snprintf(s, sizeof s, "%.*s", (int)(dot - o->target), o->target); snprintf(nm, sizeof nm, "%s", dot + 1);
    }
    if (s[0] && strcmp(s, "PUBLIC")) snprintf(out, n, "%s.%s", s, nm); else snprintf(out, n, "%s", nm);
}

static bool enqueue(Entry *e, Connection *c, int kind, const char *key, const char *query) {
    if (!e->job) {   /* the catalog session ended (dropped by the server, failed): reopen it for columns */
        if (!A->catalog_session || !c->db || !start_session(e, c, true)) return false;
        if (!e->cat_taken) e->cat_taken = e->have;
    }
    Job *j = e->job;
    SDL_LockMutex(j->mu);
    bool ok = true;
    if (j->nreq == j->capreq) { int nc = j->capreq ? j->capreq * 2 : 16; Req *nr = realloc(j->req, sizeof *nr * (size_t)nc); if (nr) { j->req = nr; j->capreq = nc; } else ok = false; }
    if (ok) { Req *r = &j->req[j->nreq++]; r->kind = kind; snprintf(r->key, sizeof r->key, "%s", key); snprintf(r->query, sizeof r->query, "%s", query); __atomic_add_fetch(&j->pending, 1, __ATOMIC_SEQ_CST); SDL_SignalCondition(j->cv); }
    SDL_UnlockMutex(j->mu);
    if (ok && A) app_wake_in(A, 150);   /* the pump may already have run this frame and seen nothing pending: without this wake an idle app never collects the answer */
    return ok;
}

DbTable *objcat_table(App *a, Connection *c, const char *schema, const char *name, int *state) {
    A = a;
    *state = OCS_NONE;
    if (!c || !c->db || !a->catalog_session || !objcat_sql(c->driver, 1, false)) return NULL;
    Entry *e = entry_now(c); if (!e) return NULL;
    char key[300]; lower_key(key, sizeof key, schema, name);
    for (int i = 0; i < e->nct; i++) if (!strcmp(e->ct[i].key, key)) { *state = e->ct[i].state; return e->ct[i].t; }
    if (e->nct == e->capct) { int nc = e->capct ? e->capct * 2 : 64; ColT *n2 = realloc(e->ct, sizeof *n2 * (size_t)nc); if (!n2) return NULL; e->ct = n2; e->capct = nc; }
    DbTable *t = calloc(1, sizeof *t); if (!t) return NULL;
    char disp[300]; snprintf(disp, sizeof disp, "%s%s%s", schema && schema[0] && strcmp(schema, "PUBLIC") ? schema : "", schema && schema[0] && strcmp(schema, "PUBLIC") ? "." : "", name);
    t->name = strdup(disp);
    ColT *ct = &e->ct[e->nct++]; snprintf(ct->key, sizeof ct->key, "%s", key); ct->t = t;
    char query[300]; column_source(e->have ? &e->cat : NULL, schema, name, query, sizeof query);
    ct->state = enqueue(e, c, RQ_COLUMNS, key, query) ? OCS_LOADING : OCS_UNAVAILABLE;
    *state = ct->state;
    return t;
}

int objcat_fks(App *a, Connection *c, const char *schema, const char *name, DbFk **out, int *n) {
    A = a; *out = NULL; *n = 0;
    if (!c || !c->db || !a->catalog_session || !objcat_sql(c->driver, 1, false)) return OCS_UNAVAILABLE;
    Entry *e = entry_now(c); if (!e) return OCS_UNAVAILABLE;
    char key[300]; lower_key(key, sizeof key, schema, name);
    for (int i = 0; i < e->nft; i++) if (!strcmp(e->ft[i].key, key)) { *out = e->ft[i].fks; *n = e->ft[i].n; return e->ft[i].state; }
    if (e->nft == e->capft) { int nc = e->capft ? e->capft * 2 : 32; FkT *n2 = realloc(e->ft, sizeof *n2 * (size_t)nc); if (!n2) return OCS_UNAVAILABLE; e->ft = n2; e->capft = nc; }
    FkT *ft = &e->ft[e->nft++]; memset(ft, 0, sizeof *ft); snprintf(ft->key, sizeof ft->key, "%s", key);
    char query[300]; column_source(e->have ? &e->cat : NULL, schema, name, query, sizeof query);   /* a synonym: its target's keys */
    ft->state = enqueue(e, c, RQ_FKS, key, query) ? OCS_LOADING : OCS_UNAVAILABLE;
    return ft->state;
}

const OcMembers *objcat_members(App *a, Connection *c, const char *schema, const char *name) {
    A = a;
    if (!c || !c->db || !a->catalog_session || !objcat_sql(c->driver, 1, false)) return NULL;
    char sql[2400];
    if (!objcat_members_sql(c->driver, schema, name, sql, sizeof sql)) return NULL;
    Entry *e = entry_now(c); if (!e) return NULL;
    char key[300]; lower_key(key, sizeof key, schema, name);
    for (int i = 0; i < e->nmt; i++) if (!strcmp(e->mt[i].key, key)) return e->mt[i].ms;
    if (e->nmt == e->capmt) { int nc = e->capmt ? e->capmt * 2 : 16; MemT *n2 = realloc(e->mt, sizeof *n2 * (size_t)nc); if (!n2) return NULL; e->mt = n2; e->capmt = nc; }
    OcMembers *ms = calloc(1, sizeof *ms); if (!ms) return NULL;
    MemT *mt = &e->mt[e->nmt++]; snprintf(mt->key, sizeof mt->key, "%s", key); mt->ms = ms;
    ms->state = enqueue(e, c, RQ_MEMBERS, key, sql) ? OCS_LOADING : OCS_UNAVAILABLE;
    return ms;
}

int objcat_columns_into(App *a, Connection *c, DbTable *t) {
    if (!t || t->cols_loaded) return OCS_LOADED;
    const char *dot = strrchr(t->name, '.');
    char schema[160] = "";
    if (dot) snprintf(schema, sizeof schema, "%.*s", (int)(dot - t->name), t->name);
    else { Entry *e = c ? entry_of(c, false) : NULL; if (e && e->have) snprintf(schema, sizeof schema, "%s", c->schema[0] ? c->schema : e->cat.home); }
    int st; DbTable *src = objcat_table(a, c, schema, dot ? dot + 1 : t->name, &st);
    if (!src) return OCS_UNAVAILABLE;
    if (st != OCS_LOADED) return st;
    /* copy into the caller's table (it owns its own cache) */
    t->cols = src->ncols ? calloc((size_t)src->ncols, sizeof *t->cols) : NULL;
    for (int i = 0; i < src->ncols && t->cols; i++) { t->cols[i] = src->cols[i]; t->cols[i].name = strdup(src->cols[i].name); t->cols[i].type = strdup(src->cols[i].type ? src->cols[i].type : ""); }
    t->ncols = t->cols ? src->ncols : 0; t->cols_loaded = true;
    return OCS_LOADED;
}

/* ---- per frame ---- */

void objcat_pump(App *a) {
    A = a;
    bool arrived = false;
    for (int i = 0; i < nE; i++) {
        Entry *e = &E[i]; Job *j = e->job;
        if (!j) continue;
        Connection *c = conn_of(a, e->conn_id);
        bool finished = __atomic_load_n(&j->done, __ATOMIC_SEQ_CST);   /* read first: results pushed before it are drained below */
        /* the object list, once */
        if (!e->cat_taken && !j->skip_catalog && __atomic_load_n(&j->cat_ready, __ATOMIC_SEQ_CST)) {
            e->cat_taken = true;
            double secs = (double)SDL_GetTicks() / 1000.0 - e->t0;
            if (!__atomic_load_n(&j->open_ok, __ATOMIC_SEQ_CST) || (j->cat.n == 0 && j->err[0])) {
                snprintf(e->state, sizeof e->state, "failed: %.140s", j->err[0] ? j->err : "could not open the catalog session");
                LOG_W("object catalog: could not read %s: %s", c ? c->name : e->conn_id, j->err);   /* the driver's message: no credentials in it */
                if (c && a->active == c) app_status(a, "Completion could not list every object (%.120s): it uses this schema's tables", j->err);
            } else {
                ObjCat old = e->cat;   /* the completer may point at it: hand over the new one first */
                e->cat = j->cat; memset(&j->cat, 0, sizeof j->cat); e->have = true;
                if (c && a->active == c) app_refresh_schema(a);
                objcat_free(&old);
                if (a->explorer) explorer_rebuild(a->explorer, a);   /* its kind groups count from the catalog */
                snprintf(e->state, sizeof e->state, "%d object%s in %d schema%s (%.1f s)", e->cat.n, e->cat.n == 1 ? "" : "s", e->cat.ns, e->cat.ns == 1 ? "" : "s", secs);
                LOG_I("object catalog: %s: %d objects in %d schemas, %.1f s", c ? c->name : e->conn_id, e->cat.n, e->cat.ns, secs);
                if (c) { char p[1200]; cache_path(c, p, sizeof p); if (!objcat_save(&e->cat, p)) LOG_W("object catalog: could not write the cache"); }
                if (c && a->active == c) {
                    char cnt[32]; thousands(cnt, sizeof cnt, e->cat.n);
                    bool dbs = !strcmp(c->driver, "mysql") || !strcmp(c->driver, "mariadb");   /* what the Explorer calls them */
                    app_status(a, "Completion knows %s object%s in %d %s", cnt, e->cat.n == 1 ? "" : "s", e->cat.ns, dbs ? (e->cat.ns == 1 ? "database" : "databases") : e->cat.ns == 1 ? "schema" : "schemas");
                }
            }
        }
        /* columns that arrived */
        SDL_LockMutex(j->mu);
        Res *res = j->res; int nres = j->nres; j->res = NULL; j->nres = j->capres = 0;
        SDL_UnlockMutex(j->mu);
        for (int r = 0; r < nres; r++) {
            if (res[r].kind == RQ_FKS) {   /* a table's foreign keys */
                FkT *ft = NULL;
                for (int k = 0; k < e->nft; k++) if (!strcmp(e->ft[k].key, res[r].key)) ft = &e->ft[k];
                if (!ft) { res_free(&res[r]); continue; }
                if (ft->fks) db_fks_free(ft->fks, ft->n);
                ft->fks = res[r].fks; ft->n = res[r].nfks; res[r].fks = NULL; res[r].nfks = 0;
                ft->state = res[r].err[0] ? OCS_FAILED : OCS_LOADED;
                if (res[r].err[0]) LOG_W("object catalog: foreign keys of %s: %s", res[r].key, res[r].err);
                arrived = true;
                continue;
            }
            if (res[r].kind == RQ_MEMBERS) {   /* a package's members */
                MemT *mt = NULL;
                for (int k = 0; k < e->nmt; k++) if (!strcmp(e->mt[k].key, res[r].key)) mt = &e->mt[k];
                if (!mt) { res_free(&res[r]); continue; }
                objcat_members_free(mt->ms);
                mt->ms->m = res[r].m; mt->ms->n = res[r].nm; res[r].m = NULL; res[r].nm = 0;
                mt->ms->state = res[r].err[0] ? OCS_FAILED : OCS_LOADED;
                if (res[r].err[0]) LOG_W("object catalog: members of %s: %s", res[r].key, res[r].err);
                arrived = true;
                continue;
            }
            ColT *ct = NULL;
            for (int k = 0; k < e->nct; k++) if (!strcmp(e->ct[k].key, res[r].key)) ct = &e->ct[k];
            if (!ct) { table_free(&res[r].t); continue; }
            DbTable *t = ct->t;
            for (int q = 0; q < t->ncols; q++) { free(t->cols[q].name); free(t->cols[q].type); }
            free(t->cols);
            t->cols = res[r].t.cols; t->ncols = res[r].t.ncols; t->cols_loaded = true;
            free(res[r].t.name);
            ct->state = res[r].err[0] ? OCS_FAILED : OCS_LOADED;
            if (res[r].err[0]) LOG_W("object catalog: columns of %s: %s", t->name, res[r].err);
            arrived = true;
        }
        free(res);
        if (finished) {   /* the session ended (failed, dropped): reopened on the next request */
            e->job = NULL;
            for (int k = 0; k < e->nct; k++) if (e->ct[k].state == OCS_LOADING) e->ct[k].state = OCS_UNAVAILABLE;
            for (int k = 0; k < e->nmt; k++) if (e->mt[k].ms->state == OCS_LOADING) e->mt[k].ms->state = OCS_UNAVAILABLE;
            for (int k = 0; k < e->nft; k++) if (e->ft[k].state == OCS_LOADING) e->ft[k].state = OCS_UNAVAILABLE;
            job_release(j);
            arrived = true;
        } else if (!e->cat_taken || __atomic_load_n(&j->pending, __ATOMIC_SEQ_CST) > 0) app_wake_in(a, 150);   /* only while something is on its way: an idle catalog session never wakes the app */
    }
    if (arrived) {
        if (a->editor) editor_metadata_arrived(a->editor, a);
        if (a->explorer) explorer_columns_arrived(a->explorer, a);
    }
}

bool objcat_settled(App *a) {
    Connection *c = a->active; Entry *e = c ? entry_of(c, false) : NULL;
    return e && (e->cat_taken || (!e->job && e->state[0]));
}
void objcat_dump(App *a) {
    Connection *c = a->active; Entry *e = c ? entry_of(c, false) : NULL;
    int loading = 0, loaded = 0, mloaded = 0;
    for (int i = 0; e && i < e->nct; i++) { loading += e->ct[i].state == OCS_LOADING; loaded += e->ct[i].state == OCS_LOADED; }
    for (int i = 0; e && i < e->nmt; i++) mloaded += e->mt[i].ms->state == OCS_LOADED;
    printf("objcatstate conn=\"%s\" have=%d n=%d schemas=%d home=\"%s\" session=%d state=\"%s\" columns_loaded=%d columns_loading=%d members_loaded=%d\n", c ? c->name : "", e && e->have, e && e->have ? e->cat.n : 0, e && e->have ? e->cat.ns : 0,
           e && e->have ? e->cat.home : "", e && e->job ? 1 : 0, e ? e->state : "", loaded, loading, mloaded);
    fflush(stdout);
}
