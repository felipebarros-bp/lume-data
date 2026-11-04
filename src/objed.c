/* SPDX-License-Identifier: AGPL-3.0-only */
/* Object source editor UI: see objed.h. Main thread only (jobs run in objsrc.c's workers). */
#include "objed.h"
#include "commands.h"
#include "complete.h"
#include "conn.h"
#include "draw.h"
#include "editor.h"
#include "explorer.h"
#include "font.h"
#include "log.h"
#include "objdiag.h"
#include "objlang.h"
#include "objplan_int.h"
#include "objsrc.h"
#include "outlog.h"
#include "output.h"
#include "panels.h"
#include "sqlfmt.h"
#include "sha256.h"
#include "sqllex.h"
#include "textdiff.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <errno.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_DOCS 32
#define MAX_LISTS 32
#define MAX_SOURCE (16 << 20)   /* OBJ-075: larger definitions are not opened in the editor */

/* editor states (spec 2.4 / 7) */
enum { ST_LOADING, ST_SYNCED, ST_DIRTY, ST_VALIDATING, ST_CONFLICT, ST_APPLYING, ST_APPLIED, ST_FAILED, ST_UNKNOWN, ST_LOAD_FAILED };
/* what a running job is for */
enum { JK_NONE, JK_LOAD, JK_REMOTE, JK_DESCRIBE, JK_APPLY, JK_OTHER };
/* what to do once the definition is (re)read */
enum { RF_NONE, RF_REVIEW, RF_COMPARE, RF_RECONCILE, RF_REFRESH, RF_VALIDATE, RF_COMPARE_OTHER };
/* what to do once a freshly opened document is loaded (menu actions on a closed object) */
enum { PEND_NONE, PEND_DEPS, PEND_PROPS, PEND_CONSOLE, PEND_DEPLOY_SQL, PEND_EXPORT, PEND_COPY_ORIG, PEND_VALIDATE, PEND_COMPARE, PEND_COMPILE, PEND_COMPILE_SPEC, PEND_COMPILE_BODY, PEND_ENABLE, PEND_DISABLE, PEND_ERRORS, PEND_MV_REFRESH };
/* modal dialogs */
enum { M_NONE, M_REVIEW, M_DIFF, M_CONFLICT, M_INFO, M_FIX, M_RENAME, M_BATCH, M_REFACTOR };

struct ObjDoc {
    ObjKey key;
    char conn_name[64];
    int dialect; const ObjEngine *eng;
    int tab_id;
    ObjSnapshot base; bool loaded;
    int state; char msg[400];
    bool read_only; char ro_reason[200];
    ObjJob *job; int job_kind, remote_for, pending;
    double retry_at;
    bool remote_pending; int remote_retries;   /* a pre-review / compare read refused while the session was busy: retried shortly (a few times) */
    /* dirty / draft */
    uint32_t seen_rev; bool dirty; double changed_at; uint32_t draft_rev; bool draft_saved;
    char author[64];   /* who edited the text (from the draft, else this OS user) */
    /* diagnostics */
    ObjDiag diags[64]; int ndiags; uint32_t diag_rev; int prob_sel;
    int validated;   /* 0 none, 1 syntax ok (local), 2 valid on the server, -1 problems */
    ObjDiag server_diags[32]; int nserver;   /* from the last Apply / validation (DSRC_SERVER) */
    /* review */
    ObjSnapshot remote; bool have_remote;
    ObjColumn *new_cols; int n_new_cols;
    ObjPlan plan; bool have_plan, allow_destructive, custom_plan;
    char plan_label[120];
    uint64_t plan_fp;
    /* deployment */
    int out_exec; double t0; char deploy_id[24];
    /* bottom pane */
    int bottom_tab; bool bottom_open;
    float scroll_bottom; ScrollBar sb_bottom;
};

typedef struct ObjList {
    bool used; char conn_id[16]; char schema[128];
    ObjJob *job; ObjListItem *items; int n; bool loaded; char err[300];
} ObjList;

/* background diff for the Diff / conflict dialogs (AT-08: never on the frame) */
typedef struct DiffTask {
    pthread_t th; bool started; volatile int done; volatile int cancel;
    char *a, *b; int al, bl; bool ignore_ws;
    TextLines A, B; Diff d;
} DiffTask;

static struct {
    App *a;
    ObjDoc *docs[MAX_DOCS];
    ObjList lists[MAX_LISTS];
    /* context menu */
    PopupMenu menu, sub; char menu_conn[16]; ObjKey menu_key; char menu_schema[128]; bool menu_group;
    /* toolbar dropdowns of the active tab */
    PopupMenu drop; ObjDoc *drop_doc;
    /* modal */
    int modal; ObjDoc *mdoc; int rtab; float mscroll; ScrollBar msb;
    bool consent; TextField confirm, reason; int focus_field;
    char info_title[120]; char *info_text;
    DiffTask diff; bool diff_ws;
    /* stashed input while a modal owns it */
    Input stash; bool stashed;
    Uint32 ev_export; char export_text_path[8];
    char *export_pending;   /* text to write when the save dialog answers */
    /* style and policy */
    SqlStyle style[DIALECT_MAX]; bool style_loaded;
    unsigned policy[CONN_ENV_COUNT];
    int deploy_seq;
    /* quick fix preview (OBJ-028) */
    ObjDiag fix;
    /* rename symbol (OBJ-021) */
    TextField rename; char rename_old[128];
    /* compare with another connection (OBJ-066) */
    char compare_with[64]; bool compare_other;
    /* multi-object deployment (OBJ-043) */
    ObjDoc *batch[MAX_DOCS]; int nbatch, batch_i; bool batch_running;
    char last_hint[900];   /* last signature hint shown (harness) */
    /* refactor preview (OBJ-021): 1 extract CTE, 2 named parameters */
    int ref_kind; int ref_s0, ref_s1; TextField ref_name;
} G;

/* policy bits (<prefs>/object-policy.txt, OBJ-073) */
enum { POL_DENY_APPLY = 1, POL_DENY_DESTRUCTIVE = 2, POL_REQUIRE_REASON = 4, POL_REQUIRE_VALIDATE = 8 };

/* ------------------------------------------------------------------------ */
/* helpers                                                                   */
/* ------------------------------------------------------------------------ */

static double now_s(void) { return SDL_GetTicks() / 1000.0; }

static Connection *conn_of(App *a, const char *id) {
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i];
    return NULL;
}
static int dialect_of(Connection *c) { return dialect_for_driver(c->driver); }

/* the preferences folder: the log lives in <prefs>/logs/lume.log */
static void pref_dir(char *out, int n) {
    snprintf(out, (size_t)n, "%s", log_path());
    for (int k = 0; k < 2; k++) { char *s = strrchr(out, '/'); if (s) *s = 0; }
}
static void mkdir_p(const char *dir) { mkdir(dir, 0700); }

/* who edits / applies on this machine (OBJ-045): the OS account, never a database secret */
static const char *os_user(void) { const char *u = getenv("USER"); if (!u || !*u) u = getenv("USERNAME"); return u && *u ? u : "unknown"; }

static const char *state_text(const ObjDoc *d) {
    switch (d->state) {
    case ST_LOADING: return "Loading source";
    case ST_SYNCED: return "Synced";
    case ST_DIRTY: return d->draft_saved ? "Modified \xC2\xB7 draft saved" : "Modified \xC2\xB7 unsaved";
    case ST_VALIDATING: return "Validating";
    case ST_CONFLICT: return "Remote changed";
    case ST_APPLYING: return "Applying";
    case ST_APPLIED: return "Applied";
    case ST_FAILED: return "Failed";
    case ST_UNKNOWN: return "Outcome unknown";
    default: return "Load failed";
    }
}

static Editor *doc_editor(App *a, ObjDoc *d) { return a->panels ? panels_tab_editor(a->panels, d->tab_id) : NULL; }
static char *doc_text(App *a, ObjDoc *d, int *len) {
    Editor *e = doc_editor(a, d);
    if (!e) { *len = 0; return strdup(""); }
    return editor_text(e, len);
}
static void key_id(const ObjKey *k, char *out, int n) {
    char b[1200]; snprintf(b, sizeof b, "%s|%s|%s|%d|%s|%s", k->conn_id, k->schema, k->name, k->kind, k->signature, k->table);
    snprintf(out, (size_t)n, "%016llx", (unsigned long long)obj_hash(b, -1));
}
static void doc_title(const ObjKey *k, char *out, int n) {
    if (k->kind == OBJK_FUNCTION || k->kind == OBJK_PROCEDURE) snprintf(out, (size_t)n, "%s(%s)", k->name, k->signature);
    else if (k->kind == OBJK_PKG_SPEC) snprintf(out, (size_t)n, "%s / SPEC", k->name);
    else if (k->kind == OBJK_PKG_BODY) snprintf(out, (size_t)n, "%s / BODY", k->name);
    else if (k->kind == OBJK_TYPE_BODY) snprintf(out, (size_t)n, "%s / TYPE BODY", k->name);
    else snprintf(out, (size_t)n, "%s.sql", k->name);
}

/* ------------------------------------------------------------------------ */
/* style, policy, audit                                                      */
/* ------------------------------------------------------------------------ */

/* <prefs>/sqlstyle.txt (OBJ-016): "postgres.keywords=lower", "*.indent=2", "oracle.tabs=yes" */
static void load_style(void) {
    if (G.style_loaded) return;
    G.style_loaded = true;
    for (int d = 0; d < DIALECT_MAX; d++) sql_style_default(&G.style[d]);
    char p[1024]; pref_dir(p, sizeof p); strncat(p, "/sqlstyle.txt", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char who[64], key[64], val[64];
        if (sscanf(line, "%63[^.].%63[^=]=%63s", who, key, val) != 3 || who[0] == '#') continue;
        for (int d = 0; d < DIALECT_MAX; d++) {
            const char *drv = d < dialect_count() ? dialect_name(d) : NULL;
            bool match = !strcmp(who, "*");
            if (!match && drv) { int dd = dialect_for_driver(who); match = dd == d && dd != DIALECT_GENERIC; }
            if (!match) continue;
            if (!strcmp(key, "keywords")) G.style[d].kw_case = !strcasecmp(val, "lower") ? 1 : !strcasecmp(val, "keep") ? 2 : 0;
            else if (!strcmp(key, "indent")) G.style[d].indent = atoi(val) > 0 && atoi(val) <= 8 ? atoi(val) : 4;
            else if (!strcmp(key, "tabs")) G.style[d].tabs = !strcasecmp(val, "yes");
            else if (!strcmp(key, "clauses")) G.style[d].clause_newlines = strcasecmp(val, "inline") != 0;
        }
    }
    fclose(f);
}

/* <prefs>/object-policy.txt (OBJ-073): "deny-apply PROD", "deny-destructive PROD",
 * "require-reason PROD", "require-validate STG" ("HML" is still accepted) */
static void load_policy(void) {
    memset(G.policy, 0, sizeof G.policy);
    char p[1024]; pref_dir(p, sizeof p); strncat(p, "/object-policy.txt", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "r"); if (!f) return;
    char line[256];
    while (fgets(line, sizeof line, f)) {
        char rule[64], env[32];
        if (line[0] == '#' || sscanf(line, "%63s %31s", rule, env) != 2) continue;
        int e = conn_env_by_name(env);
        unsigned bit = !strcmp(rule, "deny-apply") ? POL_DENY_APPLY : !strcmp(rule, "deny-destructive") ? POL_DENY_DESTRUCTIVE :
                       !strcmp(rule, "require-reason") ? POL_REQUIRE_REASON : !strcmp(rule, "require-validate") ? POL_REQUIRE_VALIDATE : 0;
        if (!strcmp(env, "*")) for (int k = 0; k < CONN_ENV_COUNT; k++) G.policy[k] |= bit;
        else if (e >= 0 && e < CONN_ENV_COUNT) G.policy[e] |= bit;
    }
    fclose(f);
    LOG_I("object policy loaded");
}

/* One line per publication, no SQL text and no values (OBJ-071): who / where / what / how / outcome. */
static void audit(App *a, ObjDoc *d, const char *result, double ms) {
    char p[1024]; pref_dir(p, sizeof p); strncat(p, "/logs/object-audit.log", sizeof p - strlen(p) - 1);
    FILE *f = fopen(p, "a"); if (!f) return;
    Connection *c = conn_of(a, d->key.conn_id);
    char disp[700]; objkey_display(&d->key, disp, sizeof disp);
    uint64_t h = 0; for (int i = 0; i < d->plan.nsql; i++) h ^= obj_hash(d->plan.sql[i], -1) + (uint64_t)i;
    time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    /* OBJ-045: who edited, who applied (OS account) and with which database account */
    const char *dbacct = c ? conn_value(c, "user") : "";
    fprintf(f, "%s deployment=%s connection=\"%s\" env=%s engine=%s object=\"%s\" strategy=\"%s\" risk=%s statements=%d result=%s ms=%.0f sql_hash=%016llx author=%s applied_by=%s db_account=%s reason=\"%s\"\n",
            ts, d->deploy_id, c ? c->name : d->conn_name, c ? conn_env_name(c->env) : "", dialect_name(d->dialect), disp, d->plan.strategy,
            risk_name(d->plan.risk), d->plan.nsql, result, ms, (unsigned long long)h, d->author[0] ? d->author : os_user(), os_user(), dbacct && *dbacct ? dbacct : "-", G.reason.buf);
    fclose(f);
}

/* ------------------------------------------------------------------------ */
/* drafts (OBJ-009 / 033)                                                    */
/* ------------------------------------------------------------------------ */

static void draft_paths(const ObjKey *k, char *text, char *base, int n) {
    char dir[1024]; pref_dir(dir, sizeof dir); strncat(dir, "/drafts", sizeof dir - strlen(dir) - 1); mkdir_p(dir);
    char id[32]; key_id(k, id, sizeof id);
    snprintf(text, (size_t)n, "%s/%s.sql", dir, id);
    if (base) snprintf(base, (size_t)n, "%s/%s.base.sql", dir, id);
}
static bool write_file(const char *path, const char *s, int len) {
    FILE *f = fopen(path, "wb"); if (!f) return false;
    bool ok = fwrite(s, 1, (size_t)len, f) == (size_t)len;
    fclose(f); chmod(path, 0600);
    return ok;
}
static char *read_file(const char *path, int *len) {
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > MAX_SOURCE) { fclose(f); return NULL; }
    char *b = malloc((size_t)n + 1); size_t r = fread(b, 1, (size_t)n, f); b[r] = 0; fclose(f);
    if (len) *len = (int)r;
    return b;
}
/* draft = header line with the base fingerprint, then the text; the base text beside it */
static bool draft_save(App *a, ObjDoc *d) {
    int len; char *t = doc_text(a, d, &len);
    char p[1100], pb[1100]; draft_paths(&d->key, p, pb, sizeof p);
    size_t hl = 64; char *buf = malloc((size_t)len + hl);
    if (!d->author[0]) snprintf(d->author, sizeof d->author, "%s", os_user());
    hl += strlen(d->author);
    int h = snprintf(buf, hl, "-- lume-draft 1 base=%016llx author=%s\n", (unsigned long long)d->base.fingerprint, d->author);
    memcpy(buf + h, t, (size_t)len);
    bool ok = write_file(p, buf, h + len);
    const char *bt = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : "";
    ok = ok && write_file(pb, bt, (int)strlen(bt));
    free(buf); free(t);
    if (ok) { d->draft_saved = true; d->draft_rev = d->seen_rev; }
    return ok;
}
static void draft_delete(ObjDoc *d) {
    char p[1100], pb[1100]; draft_paths(&d->key, p, pb, sizeof p);
    remove(p); remove(pb);
    d->draft_saved = false;
}
/* returns the draft text (malloc'd) and its base fingerprint / text, or NULL */
static char *draft_load(const ObjKey *k, uint64_t *fp, char **base_text, char *author, int an) {
    char p[1100], pb[1100]; draft_paths(k, p, pb, sizeof p);
    int len; char *s = read_file(p, &len);
    if (!s) return NULL;
    unsigned long long f = 0;
    char *nl = strchr(s, '\n');
    if (strncmp(s, "-- lume-draft 1 base=", 21) || !nl || sscanf(s + 21, "%llx", &f) != 1) { free(s); return NULL; }
    if (author) { char *au = strstr(s, " author="); if (au && au < nl) snprintf(author, (size_t)an, "%.*s", (int)(nl - au - 8), au + 8); }
    char *text = strdup(nl + 1); free(s);
    *fp = f;
    if (base_text) *base_text = read_file(pb, NULL);
    return text;
}
bool objed_has_draft(Connection *c, const ObjKey *k) {
    ObjKey kk = *k; snprintf(kk.conn_id, sizeof kk.conn_id, "%s", c->id);
    char p[1100]; draft_paths(&kk, p, NULL, sizeof p);
    struct stat st; return stat(p, &st) == 0;
}

/* ------------------------------------------------------------------------ */
/* documents                                                                 */
/* ------------------------------------------------------------------------ */

static ObjDoc *doc_find(const ObjKey *k) {
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] && objkey_equal(&G.docs[i]->key, k)) return G.docs[i];
    return NULL;
}
static ObjDoc *active_doc(App *a) { return a->panels ? panels_active_obj(a->panels, a) : NULL; }
bool objed_doc_dirty(const ObjDoc *d) { return d && d->dirty; }

static void doc_free(ObjDoc *d) {
    if (d->job) objsrc_free(d->job);
    objsnap_free(&d->base); objsnap_free(&d->remote); objplan_free(&d->plan); free(d->new_cols);
    free(d);
}

/* starts a job for d, or retries later when the connection is busy */
static bool doc_start(App *a, ObjDoc *d, int kind, ObjJob *j) {
    if (d->job) objsrc_free(d->job);
    d->job = j; d->job_kind = kind;
    if (j && j->state == OJS_FAILED && strstr(j->err, "busy")) {
        d->retry_at = now_s() + 0.5;   /* a query runs on this session: try again shortly */
        return false;
    }
    return true;
}
static Db *doc_db(App *a, ObjDoc *d) { Connection *c = conn_of(a, d->key.conn_id); return c ? c->db : NULL; }

static void start_load(App *a, ObjDoc *d) {
    Db *db = doc_db(a, d);
    Connection *cc = conn_of(a, d->key.conn_id);
    if (!db && cc && (cc->state == CONN_CONNECTING || cc->state == CONN_RECONNECTING)) {
        /* the connection is on its way (a client library may take seconds to start): load when it is up */
        d->state = ST_LOADING; snprintf(d->msg, sizeof d->msg, "Waiting for the connection to %s...", d->conn_name);
        d->retry_at = now_s() + 0.3;
        return;
    }
    if (!db) { d->state = ST_LOAD_FAILED; snprintf(d->msg, sizeof d->msg, "Not connected: connect to %s to load the source.", d->conn_name); return; }
    doc_start(a, d, JK_LOAD, objsrc_load(db, d->eng, &d->key));
    d->state = ST_LOADING;
}
static void start_remote(App *a, ObjDoc *d, int purpose) {
    Db *db = doc_db(a, d);
    if (!db) { snprintf(d->msg, sizeof d->msg, "Not connected: %s.", d->conn_name); app_status(a, "%s", d->msg); return; }
    d->remote_for = purpose;
    doc_start(a, d, JK_REMOTE, objsrc_load(db, d->eng, &d->key));
}

bool objed_open(App *a, Connection *c, const ObjKey *k0) {
    if (!c) return false;
    ObjKey k = *k0; snprintf(k.conn_id, sizeof k.conn_id, "%s", c->id);
    ObjDoc *d = doc_find(&k);
    if (d && panels_tab_exists(a->panels, d->tab_id)) { panels_activate_tab(a->panels, a, d->tab_id); return true; }
    const ObjEngine *e = obj_engine_for_driver(c->driver);
    ObjCaps caps = e ? e->caps(k.kind, 0) : (ObjCaps){ 0 };
    if (!e || !caps.source_read) { app_status(a, "%s: reading a %s is not supported on %s", c->name, objkind_word(k.kind), e ? dialect_name(e->dialect) : c->driver); return false; }
    int slot = -1; for (int i = 0; i < MAX_DOCS; i++) if (!G.docs[i]) { slot = i; break; }
    if (slot < 0) { app_status(a, "Too many object tabs open (%d)", MAX_DOCS); return false; }
    d = calloc(1, sizeof *d);
    d->key = k; d->eng = e; d->dialect = e->dialect; snprintf(d->conn_name, sizeof d->conn_name, "%s", c->name);
    d->prob_sel = -1; d->bottom_open = true;
    char title[48]; doc_title(&k, title, sizeof title);
    char disp[700]; objkey_display(&k, disp, sizeof disp);
    char placeholder[800]; snprintf(placeholder, sizeof placeholder, "-- loading the definition of %s ...\n", disp);
    d->tab_id = panels_open_object_tab(a->panels, a, title, c->id, placeholder, d);
    if (d->tab_id < 0) { free(d); app_status(a, "No free tab for the object"); return false; }
    Editor *ed = doc_editor(a, d); if (ed) { editor_set_read_only(ed, true); editor_set_dialect(ed, d->dialect); }   /* read-only until the source is loaded */
    G.docs[slot] = d;
    if (c->read_only) { d->read_only = false; snprintf(d->ro_reason, sizeof d->ro_reason, "the connection is read-only in Lume"); }
    LOG_I("object editor: open %s on %s", disp, c->name);
    start_load(a, d);
    return true;
}

void objed_tab_closed(App *a, ObjDoc *d) {
    if (!d) return;
    if (d->dirty && !d->draft_saved) draft_save(a, d);   /* closing never loses the edit (OBJ-009) */
    if (d->dirty) app_status(a, "%s closed: the draft is kept and comes back when the object is reopened", d->key.name);
    if (G.mdoc == d) { G.modal = M_NONE; G.mdoc = NULL; }
    if (G.drop_doc == d) { popup_close(&G.drop); G.drop_doc = NULL; }
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] == d) G.docs[i] = NULL;
    doc_free(d);
}

/* the definition arrived: fill the editor (or the draft) */
static void on_loaded(App *a, ObjDoc *d, ObjJob *j) {
    objsnap_free(&d->base);
    d->base = j->snap; memset(&j->snap, 0, sizeof j->snap);
    Editor *ed = doc_editor(a, d);
    if (!d->base.exists) { d->state = ST_LOAD_FAILED; snprintf(d->msg, sizeof d->msg, "The object does not exist on the server (dropped, renamed, or not visible to this user)."); if (ed) editor_set_text(ed, "-- the object does not exist on the server\n"); return; }
    const char *text = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : "";
    if ((int)strlen(text) > MAX_SOURCE) { d->state = ST_LOAD_FAILED; snprintf(d->msg, sizeof d->msg, "The definition is larger than %d MB: not opened in the editor (export it instead).", MAX_SOURCE >> 20); return; }
    d->loaded = true;
    d->read_only = d->base.system || d->base.encrypted;
    if (d->read_only) snprintf(d->ro_reason, sizeof d->ro_reason, "%s", d->base.ro_reason[0] ? d->base.ro_reason : d->base.encrypted ? "the source is encrypted / wrapped" : "a system object");
    if (d->read_only) snprintf(d->msg, sizeof d->msg, "Read-only: %s.", d->ro_reason);
    if (!d->base.ddl && d->base.missing[0]) snprintf(d->msg, sizeof d->msg, "The server keeps only part of the definition: %s", d->base.missing);
    uint64_t dfp = 0; char *dbase = NULL;
    char *draft = d->read_only ? NULL : draft_load(&d->key, &dfp, &dbase, d->author, sizeof d->author);
    if (ed) {
        editor_set_read_only(ed, false);
        editor_set_text(ed, draft ? draft : text);
        editor_set_read_only(ed, d->read_only);
        d->seen_rev = 0;
    }
    if (draft) {
        d->draft_saved = true; d->dirty = true;
        if (dfp != d->base.fingerprint) {
            /* the server changed since the draft was written: the draft's base becomes the base of
             * a three-way comparison with what the server has now */
            d->state = ST_CONFLICT;
            snprintf(d->msg, sizeof d->msg, "Draft restored, but the definition changed on the server since it was written. Compare before publishing.");
            objsnap_free(&d->remote); d->remote = d->base; d->have_remote = true;
            memset(&d->base, 0, sizeof d->base);
            d->base.exists = true; d->base.ddl = dbase ? dbase : strdup(""); d->base.fingerprint = dfp;
            d->base.original = strdup(d->base.ddl);
            dbase = NULL;
        } else { d->state = ST_DIRTY; snprintf(d->msg, sizeof d->msg, "Draft restored (saved earlier; not published)."); }
        free(draft);
    } else d->state = ST_SYNCED;
    free(dbase);
    if (d->state == ST_SYNCED && d->base.status[0] && strcasecmp(d->base.status, "VALID")) {
        snprintf(d->msg, sizeof d->msg, "Status on the server: %s (%d compilation error(s)).", d->base.status, d->base.nerrors);
    }
    /* server errors already on the object (Oracle INVALID) into Problems */
    d->nserver = 0;
    for (int i = 0; i < d->base.nerrors && d->nserver < 32; i++) {
        ObjDiag *g = &d->server_diags[d->nserver++]; memset(g, 0, sizeof *g);
        g->source = DSRC_SERVER; g->severity = DIAG_ERROR; snprintf(g->code, sizeof g->code, "%s", d->base.errors[i].code);
        snprintf(g->message, sizeof g->message, "%s", d->base.errors[i].text); g->line = d->base.errors[i].line; g->col = d->base.errors[i].col; g->start = -1; g->end = -1;
        snprintf(g->unit, sizeof g->unit, "%s", d->base.errors[i].unit); g->confidence = 1;
    }
}

/* ------------------------------------------------------------------------ */
/* diagnostics                                                               */
/* ------------------------------------------------------------------------ */

static void run_diagnostics(App *a, ObjDoc *d, Editor *ed) {
    int len; char *t = editor_text(ed, &len);
    Connection *c = conn_of(a, d->key.conn_id);
    /* catalog names for reference checks (OBJ-027): the tables Lume already knows */
    const char **known = NULL; int nk = 0;
    const char **cols = NULL; int nc = 0;
    if (c && c->db && c->tables) {
        known = malloc(sizeof *known * (size_t)(c->ntables + 1));
        for (int i = 0; i < c->ntables; i++) known[nk++] = c->tables[i].name;
        int total = 0; for (int i = 0; i < c->ntables; i++) if (c->tables[i].cols_loaded) total += c->tables[i].ncols;
        if (total) {
            cols = malloc(sizeof *cols * (size_t)total);
            for (int i = 0; i < c->ntables; i++) if (c->tables[i].cols_loaded)
                for (int k = 0; k < c->tables[i].ncols; k++) {
                    char *s = malloc(strlen(c->tables[i].name) + strlen(c->tables[i].cols[k].name) + 2);
                    sprintf(s, "%s.%s", c->tables[i].name, c->tables[i].cols[k].name);
                    cols[nc++] = s;
                }
        }
    }
    ObjDiagCtx ctx = { .dialect = d->dialect, .key = &d->key, .base = &d->base, .known = known, .nknown = nk, .columns = cols, .ncolumns = nc };
    const LangService *ls = lang_for(d->dialect, NULL);
    d->ndiags = ls && ls->diagnose ? ls->diagnose(t, len, &ctx, d->diags, 64, NULL) : 0;
    for (int i = 0; i < nc; i++) free((char *)cols[i]);
    free(cols); free(known); free(t);
    EdMark m[EDITOR_MARKS_MAX]; int nm = 0;
    for (int i = 0; i < d->ndiags && nm < EDITOR_MARKS_MAX; i++) if (d->diags[i].start >= 0) {
        m[nm].start = d->diags[i].start; m[nm].end = d->diags[i].end; m[nm].severity = d->diags[i].severity == DIAG_ERROR ? 0 : d->diags[i].severity == DIAG_WARNING ? 1 : 2; nm++;
    }
    editor_set_marks(ed, m, nm);
    d->diag_rev = editor_revision(ed);
    bool errors = false; for (int i = 0; i < d->ndiags; i++) if (d->diags[i].severity == DIAG_ERROR) errors = true;
    if (d->validated != 2) d->validated = errors ? -1 : 1;
}
static int count_sev(const ObjDoc *d, int sev) {
    int n = 0; for (int i = 0; i < d->ndiags; i++) if (d->diags[i].severity == sev) n++;
    if (sev == DIAG_ERROR) n += d->nserver;
    return n;
}

/* ------------------------------------------------------------------------ */
/* planning and the review                                                   */
/* ------------------------------------------------------------------------ */

static void make_plan(App *a, ObjDoc *d) {
    objplan_free(&d->plan);
    int len; char *t = doc_text(a, d, &len);
    Connection *c = conn_of(a, d->key.conn_id);
    ObjPlanInput in; memset(&in, 0, sizeof in);
    in.dialect = d->dialect; in.key = &d->key; in.base = &d->base; in.remote = d->have_remote ? &d->remote : NULL;
    in.local = t; in.local_len = len; in.new_cols = d->new_cols; in.n_new_cols = d->n_new_cols;
    in.allow_destructive = d->allow_destructive; in.has_driver = true;
    in.read_only = c && c->read_only; in.ro_reason = "marked read-only in Lume";
    in.tx_open = c && c->db && c->db->drv->tx_state && c->db->drv->tx_state(c->db) == 1;
    in.server_version = d->remote.server_version ? d->remote.server_version : d->base.server_version;
    in.mariadb = d->dialect == DIALECT_MARIADB;
    objplan_make(&in, &d->plan);
    d->have_plan = true; d->custom_plan = false;
    d->plan_fp = d->have_remote ? d->remote.fingerprint : d->base.fingerprint;
    snprintf(d->plan_label, sizeof d->plan_label, "Apply");
    free(t);
}

static void open_review(App *a, ObjDoc *d) {
    G.modal = M_REVIEW; G.mdoc = d; G.rtab = 0; G.mscroll = 0; G.consent = false;
    textfield_set(&G.confirm, ""); textfield_set(&G.reason, ""); G.focus_field = 0;
    load_policy();
    (void)a;
}

/* a plan made by hand (compile, enable / disable): one statement, reviewed like the others */
static void custom_plan(App *a, ObjDoc *d, const char *label, const char *sql, int risk, const char *impact) {
    objplan_free(&d->plan);
    d->plan.create_index = -1; d->plan.atomicity = ATOM_AUTOCOMMIT; d->plan.risk = risk;
    snprintf(d->plan.strategy, sizeof d->plan.strategy, "%s", label);
    plan_sql(&d->plan, "%s", sql);
    plan_impact(&d->plan, "%s", impact);
    Connection *c = conn_of(a, d->key.conn_id);
    if (c && c->read_only) plan_block(&d->plan, "The connection is read-only in Lume: nothing will be written.");
    d->have_plan = true; d->custom_plan = true; d->plan_fp = d->base.fingerprint;
    snprintf(d->plan_label, sizeof d->plan_label, "%s", label);
    open_review(a, d);
}

/* Apply… : re-read the definition, then (PG views) describe the new query, then plan + review */
static void begin_apply(App *a, ObjDoc *d) {
    if (!d->loaded) { app_status(a, "The source is not loaded yet"); return; }
    d->allow_destructive = false;
    start_remote(a, d, RF_REVIEW);
    snprintf(d->msg, sizeof d->msg, "Reading the server definition before the review...");
}

/* the view's query from the edited text (for the server describe) */
static bool view_query(App *a, ObjDoc *d, char **q, int *ql) {
    int len; char *t = doc_text(a, d, &len);
    ObjHeader h;
    if (!obj_parse_header(t, len, d->dialect, &h) || h.select_start < 0 || h.body_end <= h.select_start) { free(t); return false; }
    int e = h.body_end;
    /* "WITH CHECK OPTION" ends the query */
    char *chk = NULL; for (char *p = t + h.select_start; p < t + e - 4; p++) if (!strncasecmp(p, "WITH CHECK OPTION", 17) || !strncasecmp(p, "WITH CASCADED CHECK", 19) || !strncasecmp(p, "WITH LOCAL CHECK", 16)) { chk = p; break; }
    if (chk) e = (int)(chk - t);
    *ql = e - h.select_start; *q = malloc((size_t)*ql + 1); memcpy(*q, t + h.select_start, (size_t)*ql); (*q)[*ql] = 0;
    free(t);
    return true;
}

static void start_describe(App *a, ObjDoc *d, int purpose) {
    char *q; int ql;
    Db *db = doc_db(a, d);
    if (!db || !view_query(a, d, &q, &ql)) { if (purpose == RF_REVIEW) { make_plan(a, d); open_review(a, d); } else app_status(a, "No view query to validate"); return; }
    d->remote_for = purpose;
    doc_start(a, d, JK_DESCRIBE, objsrc_describe(db, d->eng, q, ql));
    free(q);
}

/* ------------------------------------------------------------------------ */
/* job completion                                                            */
/* ------------------------------------------------------------------------ */

static void out_plan_events(App *a, ObjDoc *d, ObjJob *j, Connection *c) {
    for (int i = 0; i < d->plan.nsql; i++) {
        if (i < j->ran) output_obj_event(a->output, a, c, d->out_exec, i + 1, OL_SUCCESS, i == d->plan.create_index ? "Definition published" : "Statement completed", d->plan.sql[i]);
        else if (i == j->failed_at) output_obj_event(a->output, a, c, d->out_exec, i + 1, OL_ERROR, j->failed_error, d->plan.sql[i]);
    }
}

/* maps a server error offset inside the CREATE statement to the buffer */
static void server_problem(App *a, ObjDoc *d, const char *msg, int stmt_offset) {
    d->nserver = 0;
    ObjDiag *g = &d->server_diags[d->nserver++]; memset(g, 0, sizeof *g);
    g->source = DSRC_SERVER; g->severity = DIAG_ERROR; snprintf(g->code, sizeof g->code, "SERVER");
    snprintf(g->message, sizeof g->message, "%s", msg); g->start = g->end = -1; snprintf(g->unit, sizeof g->unit, "MAIN"); g->confidence = 2;
    if (stmt_offset >= 0 && d->plan.create_index >= 0) {
        int len; char *t = doc_text(a, d, &len);
        const char *cs = d->plan.sql[d->plan.create_index];
        char *at = strstr(t, cs);
        if (at) {
            int off = (int)(at - t) + stmt_offset;
            g->start = off; g->end = off + 1; g->confidence = 1;
            int l = 1, col = 1; for (int i = 0; i < off && i < len; i++) { if (t[i] == '\n') { l++; col = 1; } else col++; }
            g->line = l; g->col = col;
        }
        free(t);
    }
}

static void finish_apply(App *a, ObjDoc *d, ObjJob *j) {
    Connection *c = conn_of(a, d->key.conn_id);
    double ms = (now_s() - d->t0) * 1000;
    out_plan_events(a, d, j, c);
    Editor *ed = doc_editor(a, d);
    if (j->state == OJS_DONE) {
        objsnap_free(&d->base); d->base = j->snap; memset(&j->snap, 0, sizeof j->snap);
        objsnap_free(&d->remote); d->have_remote = false;
        d->nserver = 0;
        if (j->compiled_invalid || (d->base.status[0] && strcasecmp(d->base.status, "VALID"))) {
            /* AT-05: the DDL ran but the object does not compile: never reported as success */
            d->state = ST_FAILED;
            snprintf(d->msg, sizeof d->msg, "Published, but the server reports %s with %d compilation error(s). See Problems.", d->base.status[0] ? d->base.status : "INVALID", d->base.nerrors);
            for (int i = 0; i < d->base.nerrors && d->nserver < 32; i++) {
                ObjDiag *g = &d->server_diags[d->nserver++]; memset(g, 0, sizeof *g);
                g->source = DSRC_SERVER; g->severity = DIAG_ERROR; snprintf(g->code, sizeof g->code, "%s", d->base.errors[i].code);
                snprintf(g->message, sizeof g->message, "%s", d->base.errors[i].text); g->line = d->base.errors[i].line; g->col = d->base.errors[i].col;
                g->start = g->end = -1; snprintf(g->unit, sizeof g->unit, "%s", d->base.errors[i].unit); g->confidence = 1;
                output_obj_event(a->output, a, c, d->out_exec, 0, OL_ERROR, g->message, NULL);
            }
            output_obj_event(a->output, a, c, d->out_exec, 0, OL_WARNING, d->msg, NULL);
            output_obj_finish(a->output, a, c, d->out_exec, ES_FAILED, ms);
            audit(a, d, "invalid", ms);
            d->bottom_open = true; d->bottom_tab = 0;
            draft_delete(d); draft_save(a, d);   /* the text is on the server; keep it as the base of further edits */
            draft_delete(d);
            d->dirty = false; d->seen_rev = ed ? editor_revision(ed) : 0;
        } else {
            d->state = ST_APPLIED;
            snprintf(d->msg, sizeof d->msg, "Applied to %s in %.0f ms (%s). The definition was read back from the server.", d->conn_name, ms, d->plan.strategy);
            output_obj_event(a->output, a, c, d->out_exec, 0, OL_SUCCESS, d->msg, NULL);
            output_obj_finish(a->output, a, c, d->out_exec, ES_COMPLETED, ms);
            audit(a, d, "applied", ms);
            draft_delete(d);
            /* the buffer shows what the server now stores (one undo step back to the text as typed) */
            const char *now = d->base.ddl ? d->base.ddl : d->base.original;
            if (ed && now && !d->custom_plan) {
                int len; char *cur = editor_text(ed, &len);
                if (strcmp(cur, now)) editor_replace_all(ed, now, (int)strlen(now));
                free(cur);
            }
            d->dirty = false; d->seen_rev = ed ? editor_revision(ed) : 0;
        }
        /* the Explorer entry of this object only (AT-09: no global re-introspection) */
        for (int i = 0; i < MAX_LISTS; i++) if (G.lists[i].used && !strcmp(G.lists[i].conn_id, d->key.conn_id))
            for (int k = 0; k < G.lists[i].n; k++) if (objkey_equal(&G.lists[i].items[k].key, &d->key) || (!strcmp(G.lists[i].items[k].key.name, d->key.name) && G.lists[i].items[k].key.kind == d->key.kind))
                snprintf(G.lists[i].items[k].status, sizeof G.lists[i].items[k].status, "%s", d->base.status);
        explorer_rebuild(a->explorer, a);
    } else if (j->state == OJS_UNKNOWN) {
        d->state = ST_UNKNOWN;
        snprintf(d->msg, sizeof d->msg, "%s", j->err);
        output_obj_event(a->output, a, c, d->out_exec, 0, OL_WARNING, j->err, NULL);
        output_obj_finish(a->output, a, c, d->out_exec, ES_FAILED, ms);
        audit(a, d, "unknown", ms);
        draft_save(a, d);   /* the buffer is never lost (AT-07) */
    } else {
        d->state = j->conflict ? ST_CONFLICT : ST_FAILED;
        snprintf(d->msg, sizeof d->msg, "%s", j->err);
        if (j->failed_at >= 0) server_problem(a, d, j->failed_error, j->failed_at == d->plan.create_index ? j->failed_offset : -1);
        output_obj_event(a->output, a, c, d->out_exec, 0, OL_ERROR, j->err, NULL);
        output_obj_finish(a->output, a, c, d->out_exec, j->state == OJS_CANCELLED ? ES_CANCELLED : ES_FAILED, ms);
        audit(a, d, j->conflict ? "conflict" : j->state == OJS_CANCELLED ? "cancelled" : "failed", ms);
        draft_save(a, d);   /* OBJ-038: the edit survives the failure */
        d->bottom_open = true; d->bottom_tab = 0;
    }
    app_status(a, "%s", d->msg);
}

bool objed_command(App *a, int cmd);
static void start_batch_step(App *a);
static void do_pending(App *a, ObjDoc *d, int p) {
    switch (p) {
    case PEND_DEPS: objed_command(a, OBJCMD_DEPENDENCIES); break;
    case PEND_PROPS: objed_command(a, OBJCMD_PROPERTIES); break;
    case PEND_CONSOLE: objed_command(a, OBJCMD_CONSOLE); break;
    case PEND_DEPLOY_SQL: objed_command(a, OBJCMD_DEPLOY_SQL); break;
    case PEND_EXPORT: objed_command(a, OBJCMD_EXPORT); break;
    case PEND_VALIDATE: objed_command(a, OBJCMD_VALIDATE_SERVER); break;
    case PEND_COMPARE: objed_command(a, OBJCMD_COMPARE); break;
    case PEND_COMPILE: case PEND_COMPILE_SPEC: case PEND_COMPILE_BODY: objed_command(a, OBJCMD_COMPILE); break;
    case PEND_ERRORS: d->bottom_open = true; d->bottom_tab = 0; break;
    case PEND_COPY_ORIG: if (d->base.original) { SDL_SetClipboardText(d->base.original); app_status(a, "Original source copied (%d bytes)", (int)strlen(d->base.original)); } break;
    case PEND_MV_REFRESH: objed_command(a, OBJCMD_MV_REFRESH); break;
    case PEND_ENABLE: case PEND_DISABLE: {
        char qn[600]; obj_qname(&d->key, d->dialect, qn, sizeof qn); char sql[1200];
        if (d->dialect == DIALECT_PG) { char tb[300]; char tt[300]; snprintf(tt, sizeof tt, "%s.%s", d->key.schema, d->key.table); obj_qtable(tt, d->dialect, tb, sizeof tb);
            ObjKey k3; memset(&k3, 0, sizeof k3); snprintf(k3.name, sizeof k3.name, "%s", d->key.name); char qt[300]; obj_qname(&k3, d->dialect, qt, sizeof qt);
            snprintf(sql, sizeof sql, "ALTER TABLE %s %s TRIGGER %s", tb, p == PEND_ENABLE ? "ENABLE" : "DISABLE", qt); }
        else snprintf(sql, sizeof sql, "ALTER TRIGGER %s %s", qn, p == PEND_ENABLE ? "ENABLE" : "DISABLE");
        custom_plan(a, d, p == PEND_ENABLE ? "Enable trigger" : "Disable trigger", sql, RISK_POTENTIAL, p == PEND_ENABLE ? "The trigger fires again on the next write to its table." : "While disabled, writes to the table do not fire the trigger.");
        break; }
    }
}

static void poll_doc(App *a, ObjDoc *d) {
    if (!d->job) {
        if (d->retry_at > 0 && now_s() >= d->retry_at) {
            d->retry_at = 0;
            if (d->state == ST_LOADING) start_load(a, d);
            else if (d->remote_pending) { d->remote_pending = false; start_remote(a, d, d->remote_for); }
        }
        return;
    }
    if (!objsrc_poll(d->job)) { a->animating = true; return; }
    ObjJob *j = d->job; d->job = NULL; int kind = d->job_kind; d->job_kind = JK_NONE;
    if (j->state == OJS_FAILED && strstr(j->err, "busy") && (kind == JK_LOAD || kind == JK_REMOTE)) {
        if (kind == JK_LOAD) d->retry_at = now_s() + 0.5;
        else if (d->remote_retries++ < 20) { d->remote_pending = true; d->retry_at = now_s() + 0.5; snprintf(d->msg, sizeof d->msg, "The session is busy: the definition is re-read when it frees up\xE2\x80\xA6"); }   /* up to ~10 s */
        else { d->remote_retries = 0; snprintf(d->msg, sizeof d->msg, "The session stayed busy: try again when the running statement ends"); app_status(a, "%s", d->msg); }
        objsrc_free(j); return;
    }
    switch (kind) {
    case JK_LOAD:
        if (j->state == OJS_DONE) on_loaded(a, d, j);
        else { d->state = ST_LOAD_FAILED; snprintf(d->msg, sizeof d->msg, "Could not read the definition: %s", j->err); Editor *ed = doc_editor(a, d); if (ed) { editor_set_read_only(ed, false); editor_set_text(ed, "-- the definition could not be read; see the message above\n"); editor_set_read_only(ed, true); } }
        /* actions requested from the Explorer menu before the source was there */
        if (d->loaded && d->pending) { int p = d->pending; d->pending = PEND_NONE; do_pending(a, d, p); }
        break;
    case JK_REMOTE:
        if (j->state != OJS_DONE) { snprintf(d->msg, sizeof d->msg, "Could not re-read the definition: %s", j->err); app_status(a, "%s", d->msg); break; }
        objsnap_free(&d->remote); d->remote = j->snap; memset(&j->snap, 0, sizeof j->snap); d->have_remote = true; d->remote_retries = 0;
        if (d->remote_for == RF_REVIEW) {
            bool view = (d->key.kind == OBJK_VIEW || d->key.kind == OBJK_MATVIEW) && d->eng->describe_sql && d->eng->caps(d->key.kind, 0).server_validate;
            if (view && d->dialect == DIALECT_PG && d->remote.fingerprint == d->base.fingerprint) start_describe(a, d, RF_REVIEW);
            else {
                make_plan(a, d);
                if (d->plan.conflict && d->remote.exists) { G.modal = M_CONFLICT; G.mdoc = d; G.mscroll = 0; d->state = ST_CONFLICT; }
                else open_review(a, d);
            }
        } else if (d->remote_for == RF_COMPARE || d->remote_for == RF_COMPARE_OTHER) {
            G.compare_other = d->remote_for == RF_COMPARE_OTHER;
            if (G.compare_other && !d->remote.exists) { app_status(a, "%s does not exist on %s", d->key.name, G.compare_with); objsnap_free(&d->remote); d->have_remote = false; G.compare_other = false; break; }
            G.modal = M_DIFF; G.mdoc = d; G.mscroll = 0;
            memset(&G.diff, 0, sizeof G.diff);
        } else if (d->remote_for == RF_RECONCILE) {
            /* AT-07: compare what the server has with what was there before and with the text */
            int len; char *t = doc_text(a, d, &len);
            bool same_as_base = d->remote.fingerprint == d->base.fingerprint;
            if (!d->remote.exists) snprintf(d->msg, sizeof d->msg, "Reconciled: the object does not exist on the server now.");
            else if (same_as_base) snprintf(d->msg, sizeof d->msg, "Reconciled: the server still has the definition from before the deployment: the change was NOT applied. Apply again after review.");
            else snprintf(d->msg, sizeof d->msg, "Reconciled: the server definition changed: the deployment (or another session) was applied. Compare before doing anything else.");
            d->state = same_as_base ? ST_DIRTY : ST_CONFLICT;
            free(t);
            app_status(a, "%s", d->msg);
            if (!same_as_base && d->remote.exists) { G.modal = M_DIFF; G.mdoc = d; G.mscroll = 0; memset(&G.diff, 0, sizeof G.diff); }
        } else if (d->remote_for == RF_REFRESH) {
            /* Refresh Definition: replace when unchanged locally, otherwise compare (never overwrite an edit) */
            if (!d->dirty) {
                objsnap_free(&d->base); d->base = d->remote; memset(&d->remote, 0, sizeof d->remote); d->have_remote = false;
                Editor *ed = doc_editor(a, d);
                const char *now = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : "";
                if (ed) { editor_set_read_only(ed, false); editor_set_text(ed, now); editor_set_read_only(ed, d->read_only); d->seen_rev = editor_revision(ed); }
                d->state = ST_SYNCED; snprintf(d->msg, sizeof d->msg, "Definition refreshed from the server.");
            } else if (d->remote.fingerprint != d->base.fingerprint) {
                d->state = ST_CONFLICT; snprintf(d->msg, sizeof d->msg, "The server definition changed and you have local edits: three-way comparison.");
                G.modal = M_CONFLICT; G.mdoc = d; G.mscroll = 0;
            } else snprintf(d->msg, sizeof d->msg, "The server definition is unchanged; your edits are kept.");
        }
        break;
    case JK_DESCRIBE: {
        Connection *c = conn_of(a, d->key.conn_id);
        if (d->remote_for == RF_REVIEW) {
            free(d->new_cols); d->new_cols = NULL; d->n_new_cols = 0;
            if (j->state == OJS_DONE) { d->new_cols = j->cols; d->n_new_cols = j->ncols; j->cols = NULL; }
            make_plan(a, d);
            if (j->state != OJS_DONE) plan_pre(&d->plan, "Server validation failed: %s", j->err);
            if (j->state != OJS_DONE) plan_block(&d->plan, "The server rejects the query: %s", j->err);
            open_review(a, d);
        } else {
            int x = output_obj_begin(a->output, a, c, "Validate on the server (compiled as a temporary view, rolled back)", 1);
            if (j->state == OJS_DONE) {
                d->validated = 2; d->nserver = 0;
                snprintf(d->msg, sizeof d->msg, "Valid on the server: %d column(s); nothing was changed (rolled back).", j->ncols);
                output_obj_event(a->output, a, c, x, 1, OL_SUCCESS, d->msg, NULL);
                output_obj_finish(a->output, a, c, x, ES_COMPLETED, (now_s() - d->t0) * 1000);
            } else {
                d->validated = -1;
                snprintf(d->msg, sizeof d->msg, "The server rejects the query: %s", j->err);
                server_problem(a, d, j->err, -1);
                output_obj_event(a->output, a, c, x, 1, OL_ERROR, j->err, NULL);
                output_obj_finish(a->output, a, c, x, ES_FAILED, (now_s() - d->t0) * 1000);
                d->bottom_open = true; d->bottom_tab = 0;
            }
            d->state = d->dirty ? ST_DIRTY : ST_SYNCED;
            app_status(a, "%s", d->msg);
        }
        break; }
    case JK_APPLY:
        finish_apply(a, d, j);
        if (G.batch_running && G.batch_i < G.nbatch && G.batch[G.batch_i] == d) {
            if (d->state == ST_APPLIED) { G.batch_i++; start_batch_step(a); }
            else { G.batch_running = false; app_status(a, "Deployment stopped at %s (%d of %d applied): %s", d->key.name, G.batch_i, G.nbatch, d->msg); }
        }
        break;
    }
    objsrc_free(j);
}

/* ------------------------------------------------------------------------ */
/* lists for the Explorer                                                    */
/* ------------------------------------------------------------------------ */

ObjListView objed_list(App *a, Connection *c, const char *schema, bool request) {
    ObjListView v = { 0 };
    if (!c) return v;
    ObjList *l = NULL, *free_slot = NULL;
    for (int i = 0; i < MAX_LISTS; i++) {
        if (G.lists[i].used && !strcmp(G.lists[i].conn_id, c->id) && !strcmp(G.lists[i].schema, schema ? schema : "")) l = &G.lists[i];
        else if (!G.lists[i].used && !free_slot) free_slot = &G.lists[i];
    }
    if (!l && request && free_slot && c->db) {
        const ObjEngine *e = obj_engine_for_driver(c->driver);
        if (!e || !e->list_sql) { v.loaded = true; v.error = "Objects are not listed for this engine"; return v; }
        l = free_slot; memset(l, 0, sizeof *l); l->used = true;
        snprintf(l->conn_id, sizeof l->conn_id, "%s", c->id); snprintf(l->schema, sizeof l->schema, "%s", schema ? schema : "");
        l->job = objsrc_list(c->db, e, schema, c->id);
    }
    if (!l) return v;
    v.items = l->items; v.n = l->n; v.loaded = l->loaded; v.loading = l->job != NULL; v.error = l->err[0] ? l->err : NULL;
    return v;
}
void objed_list_invalidate(Connection *c) {
    for (int i = 0; i < MAX_LISTS; i++) if (G.lists[i].used && !strcmp(G.lists[i].conn_id, c->id)) {
        if (G.lists[i].job) objsrc_free(G.lists[i].job);
        free(G.lists[i].items); memset(&G.lists[i], 0, sizeof G.lists[i]);
    }
}
static void poll_lists(App *a) {
    bool changed = false;
    for (int i = 0; i < MAX_LISTS; i++) {
        ObjList *l = &G.lists[i];
        if (!l->used || !l->job) continue;
        if (!objsrc_poll(l->job)) { a->animating = true; continue; }
        ObjJob *j = l->job; l->job = NULL;
        if (j->state == OJS_FAILED && strstr(j->err, "busy")) {
            /* retry on a later frame: forget the slot */
            objsrc_free(j); memset(l, 0, sizeof *l); changed = true; continue;
        }
        if (j->state == OJS_DONE) { l->items = j->items; l->n = j->nitems; j->items = NULL; }
        else snprintf(l->err, sizeof l->err, "%s", j->err);
        l->loaded = true; changed = true;
        objsrc_free(j);
    }
    if (changed) explorer_rebuild(a->explorer, a);
}

/* ------------------------------------------------------------------------ */
/* commands                                                                  */
/* ------------------------------------------------------------------------ */

static void open_console(App *a, ObjDoc *d, const char *text, const char *suffix) {
    char title[48]; snprintf(title, sizeof title, "%s%s", d->key.name, suffix);
    int id = panels_open_tab(a->panels, a, title, d->key.conn_id, text);
    if (id >= 0) panels_activate_tab(a->panels, a, id);
}

static void SDLCALL on_export(void *ud, const char *const *files, int filter) {
    (void)filter;
    if (!files || !files[0]) return;
    SDL_Event ev; SDL_zero(ev); ev.type = G.ev_export; ev.user.data1 = SDL_strdup(files[0]);
    SDL_PushEvent(&ev);
    (void)ud;
}
void objed_handle_event(App *a, const SDL_Event *e) {
    if (!G.ev_export || e->type != G.ev_export) return;
    char *path = e->user.data1;
    if (G.export_pending && path) {
        if (write_file(path, G.export_pending, (int)strlen(G.export_pending))) app_status(a, "Definition exported to %s", path);
        else app_status(a, "Could not write %s: %s", path, strerror(errno));
    }
    free(G.export_pending); G.export_pending = NULL;
    SDL_free(path);
}
bool objed_export_to(App *a, const char *path) {
    ObjDoc *d = active_doc(a); if (!d) return false;
    int len; char *t = doc_text(a, d, &len);
    bool ok = write_file(path, t, len);
    free(t);
    app_status(a, ok ? "Definition exported to %s" : "Could not write %s", path);
    return ok;
}

static void validate_local(App *a, ObjDoc *d) {
    Editor *ed = doc_editor(a, d); if (!ed) return;
    run_diagnostics(a, d, ed);
    int e = count_sev(d, DIAG_ERROR), w = count_sev(d, DIAG_WARNING);
    d->bottom_open = true; d->bottom_tab = 0;
    snprintf(d->msg, sizeof d->msg, e ? "Local check: %d error(s), %d warning(s). This is a syntax / catalog check, not a compilation." :
                                         "Syntax OK (local): %d warning(s). Not compiled: use Validate on server or Apply to compile.", e, w);
    Connection *c = conn_of(a, d->key.conn_id);
    int x = output_obj_begin(a->output, a, c, "Validate (local: parser and catalog, nothing sent)", 1);
    output_obj_event(a->output, a, c, x, 1, e ? OL_WARNING : OL_SUCCESS, d->msg, NULL);
    output_obj_finish(a->output, a, c, x, ES_COMPLETED, 0);
    app_status(a, "%s", d->msg);
}

static void deploy_sql_console(App *a, ObjDoc *d) {
    make_plan(a, d);
    char *s = objplan_script(&d->plan, d->dialect);
    char head[700]; char disp[600]; objkey_display(&d->key, disp, sizeof disp);
    snprintf(head, sizeof head, "-- Deployment SQL for %s (%s)\n-- Strategy: %s; risk: %s; %s\n%s%s%s\n", disp, dialect_name(d->dialect), d->plan.strategy[0] ? d->plan.strategy : "-",
             risk_name(d->plan.risk), atomicity_name(d->plan.atomicity), d->plan.blocked ? "-- Not runnable as is: " : "", d->plan.blocked ? d->plan.block_reason : "", d->plan.blocked ? "\n" : "");
    char *all = malloc(strlen(head) + strlen(s) + 1); strcpy(all, head); strcat(all, s);
    open_console(a, d, all, " (deploy).sql");
    free(all); free(s);
}

bool objed_command(App *a, int cmd) {
    ObjDoc *d = active_doc(a);
    if (!d) { app_status(a, "No object tab is active"); return false; }
    Editor *ed = doc_editor(a, d);
    Connection *c = conn_of(a, d->key.conn_id);
    if (d->job && cmd != OBJCMD_NEXT_PROBLEM && cmd != OBJCMD_PREV_PROBLEM && cmd != OBJCMD_SAVE_DRAFT && cmd != OBJCMD_EXPORT) {
        app_status(a, "%s is busy (%s); wait or cancel", d->key.name, state_text(d)); return false;
    }
    switch (cmd) {
    case OBJCMD_SAVE_DRAFT:
        if (!d->loaded) return false;
        if (draft_save(a, d)) { snprintf(d->msg, sizeof d->msg, "Draft saved locally. The database is unchanged."); app_status(a, "%s", d->msg); }
        else app_status(a, "Could not save the draft");
        return true;
    case OBJCMD_FORMAT: case OBJCMD_FORMAT_SEL: {
        if (!ed || d->read_only) return false;
        const LangService *ls = lang_for(d->dialect, NULL);
        if (!ls || !ls->format) { app_status(a, "No formatter for %s", dialect_name(d->dialect)); return false; }
        load_style();
        int len; char *t = editor_text(ed, &len);
        int s0 = -1, s1 = -1;
        if (cmd == OBJCMD_FORMAT_SEL) { int sl; char *sel = editor_selection(ed, &sl); s0 = editor_selection_start(ed); s1 = sel ? s0 + sl : -1; free(sel); if (s1 < 0) s0 = -1; }
        FmtResult r; ls->format(t, len, d->dialect, &G.style[d->dialect], s0, s1, &r, NULL);
        if (r.changed && !r.refused) editor_replace_all(ed, r.text, r.len);   /* one undo step (OBJ-014) */
        snprintf(d->msg, sizeof d->msg, "%s", r.note);
        app_status(a, "%s", r.note);
        fmt_free(&r); free(t);
        return true; }
    case OBJCMD_VALIDATE: validate_local(a, d); return true;
    case OBJCMD_VALIDATE_SERVER: {
        ObjCaps caps = d->eng->caps(d->key.kind, d->base.server_version);
        validate_local(a, d);
        if (!caps.server_validate) {
            snprintf(d->msg + strlen(d->msg), sizeof d->msg - strlen(d->msg), " Server validation without writing is not available for a %s on %s%s.", objkind_word(d->key.kind), dialect_name(d->dialect),
                     caps.server_compile ? " (Compile is the server check, and it publishes)" : "");
            return true;
        }
        if (!c || !c->db) { app_status(a, "Not connected"); return false; }
        if (c->db->drv->tx_state && c->db->drv->tx_state(c->db) == 1) { app_status(a, "The session has an open transaction: validation on the server would roll it back. Commit or roll back first."); return false; }
        d->t0 = now_s(); d->state = ST_VALIDATING;
        start_describe(a, d, RF_VALIDATE);
        return true; }
    case OBJCMD_DIFF:
        if (!d->loaded) return false;
        G.modal = M_DIFF; G.mdoc = d; G.mscroll = 0; memset(&G.diff, 0, sizeof G.diff);
        objsnap_free(&d->remote); d->have_remote = false;   /* Diff = original vs local */
        return true;
    case OBJCMD_COMPARE: start_remote(a, d, RF_COMPARE); return true;
    case OBJCMD_APPLY:
        if (d->read_only) { app_status(a, "Read-only: %s", d->ro_reason); return false; }
        begin_apply(a, d); return true;
    case OBJCMD_REFRESH: start_remote(a, d, RF_REFRESH); return true;
    case OBJCMD_RECONCILE: start_remote(a, d, RF_RECONCILE); return true;
    case OBJCMD_NEXT_PROBLEM: case OBJCMD_PREV_PROBLEM: {
        int n = d->ndiags; if (!n || !ed) { app_status(a, "No problems"); return false; }
        d->prob_sel = cmd == OBJCMD_NEXT_PROBLEM ? (d->prob_sel + 1) % n : (d->prob_sel - 1 + n) % n;
        const ObjDiag *g = &d->diags[d->prob_sel];
        if (g->start >= 0) { editor_goto_offset(ed, g->start); a->focus = FOCUS_EDITOR; }
        app_status(a, "%s %d/%d: line %d: %s", diag_severity_name(g->severity), d->prob_sel + 1, n, g->line, g->message);
        d->bottom_open = true; d->bottom_tab = 0;
        return true; }
    case OBJCMD_DISCARD:
        draft_delete(d);
        if (ed) { const char *t = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : ""; editor_replace_all(ed, t, (int)strlen(t)); }
        snprintf(d->msg, sizeof d->msg, "Draft discarded: the text is the stored definition again (Undo brings the edit back).");
        return true;
    case OBJCMD_EXPORT: {
        int len; char *t = doc_text(a, d, &len);
        free(G.export_pending); G.export_pending = t;
        static const SDL_DialogFileFilter f[] = { { "SQL", "sql" } };
        SDL_ShowSaveFileDialog(on_export, NULL, a->window, f, 1, NULL);
        return true; }
    case OBJCMD_DEPLOY_SQL: deploy_sql_console(a, d); return true;
    case OBJCMD_CONSOLE: {
        int len; char *t = doc_text(a, d, &len);
        open_console(a, d, t, " (console).sql");   /* a copy: running it there never publishes this tab (OBJ-062) */
        free(t); return true; }
    case OBJCMD_SHOW_ORIGINAL: d->bottom_open = true; d->bottom_tab = 2; return true;
    case OBJCMD_DEPENDENCIES: case OBJCMD_PROPERTIES: {
        char *b = malloc(16384); int o = 0;
        char disp[700]; objkey_display(&d->key, disp, sizeof disp);
        if (cmd == OBJCMD_DEPENDENCIES) {
            snprintf(G.info_title, sizeof G.info_title, "Dependencies of %s", d->key.name);
            o += snprintf(b + o, 16384 - (size_t)o, "%s\n\n", disp);
            if (!d->base.deps_known) o += snprintf(b + o, 16384 - (size_t)o, "This engine does not report dependents.\n");
            else if (!d->base.ndeps) o += snprintf(b + o, 16384 - (size_t)o, "No object depends on it in the catalog.\n");
            for (int i = 0; i < d->base.ndeps; i++) o += snprintf(b + o, 16384 - (size_t)o, "  %-20s %s   (%s)\n", d->base.deps[i].kind, d->base.deps[i].name, d->base.deps[i].how);
            if (d->base.nrecreate) { o += snprintf(b + o, 16384 - (size_t)o, "\nRestored after a drop + create:\n"); for (int i = 0; i < d->base.nrecreate; i++) o += snprintf(b + o, 16384 - (size_t)o, "  %s\n", d->base.recreate[i]); }
        } else {
            snprintf(G.info_title, sizeof G.info_title, "Properties of %s", d->key.name);
            ObjCaps cp = d->eng->caps(d->key.kind, d->base.server_version);
            o += snprintf(b + o, 16384 - (size_t)o, "%s\nConnection: %s (%s)\nSchema: %s\nOwner: %s\nStatus: %s\nOptions: %s\nGrants: %s\nIdentity: %s%s\nServer version: %d\nFingerprint: %016llx\nRead-only: %s\n\nCapabilities: read %s, edit %s, format %s, server validation %s, compile %s, replace in place %s, transactional DDL %s, dependents %s\n",
                          disp, d->conn_name, dialect_name(d->dialect), d->key.schema, d->base.owner[0] ? d->base.owner : "-", d->base.status[0] ? d->base.status : "-",
                          d->base.options[0] ? d->base.options : "-", d->base.acl[0] ? d->base.acl : "-", d->key.native_id[0] ? "oid " : "", d->key.native_id[0] ? d->key.native_id : "by name and kind",
                          d->base.server_version, (unsigned long long)d->base.fingerprint, d->read_only ? d->ro_reason : "no",
                          cp.source_read ? "yes" : "no", cp.source_edit ? "yes" : "no", cp.format ? "yes" : "no", cp.server_validate ? "yes" : "no", cp.server_compile ? "yes" : "no",
                          cp.replace_in_place ? "yes" : "no", cp.transactional_ddl ? "yes" : "no", cp.dependency_graph ? "yes" : "no");
            if (d->base.result_type[0]) o += snprintf(b + o, 16384 - (size_t)o, "Returns: %s\n", d->base.result_type);
            if (d->base.ncols) { o += snprintf(b + o, 16384 - (size_t)o, "\nColumns:\n"); for (int i = 0; i < d->base.ncols; i++) o += snprintf(b + o, 16384 - (size_t)o, "  %-24s %s\n", d->base.cols[i].name, d->base.cols[i].type); }
        }
        free(G.info_text); G.info_text = b;
        G.modal = M_INFO; G.mdoc = d; G.mscroll = 0;
        return true; }
    case OBJCMD_COMPILE: {
        ObjCaps cp = d->eng->caps(d->key.kind, 0);
        if (!cp.server_compile) { app_status(a, "Compile is not an operation of %s for a %s", dialect_name(d->dialect), objkind_word(d->key.kind)); return false; }
        char qn[600]; ObjKey k = d->key; obj_qname(&k, d->dialect, qn, sizeof qn);
        const char *what = d->key.kind == OBJK_PKG_SPEC || d->key.kind == OBJK_PKG_BODY ? "PACKAGE" : d->key.kind == OBJK_TYPE_SPEC || d->key.kind == OBJK_TYPE_BODY ? "TYPE" : objkind_name(d->key.kind);
        char sql[800]; snprintf(sql, sizeof sql, "ALTER %s %s COMPILE%s", what, qn, d->key.kind == OBJK_PKG_BODY || d->key.kind == OBJK_TYPE_BODY ? " BODY" : d->key.kind == OBJK_PKG_SPEC || d->key.kind == OBJK_TYPE_SPEC ? " SPECIFICATION" : "");
        custom_plan(a, d, "Compile", sql, RISK_NON_DESTRUCTIVE, "Recompiles the stored source (not your edits); the status and errors are read back.");
        return true; }
    case OBJCMD_MV_REFRESH: {
        if (!d->eng->caps(d->key.kind, 0).refresh) { app_status(a, "Not a materialized view"); return false; }
        char qn[600]; obj_qname(&d->key, d->dialect, qn, sizeof qn); char sql[900];
        if (d->dialect == DIALECT_ORACLE) { char lit[400], full[300]; snprintf(full, sizeof full, "%s.%s", d->key.schema, d->key.name); obj_qlit(full, lit, sizeof lit); snprintf(sql, sizeof sql, "BEGIN DBMS_MVIEW.REFRESH(%s); END;", lit); }
        else snprintf(sql, sizeof sql, "REFRESH MATERIALIZED VIEW %s", qn);
        /* OBJ-056: refreshing data is not changing the definition: its own reviewed operation */
        custom_plan(a, d, "Refresh data", sql, RISK_POTENTIAL, "Recomputes the stored rows with the current definition (the definition is not changed); readers may wait for it.");
        return true; }
    case OBJCMD_RENAME: {
        if (!ed || d->read_only) return false;
        int line, col; editor_cursor_pos(ed, &line, &col);
        int len; char *t = editor_text(ed, &len);
        int off = 0, l = 1; while (off < len && l < line) { if (t[off] == '\n') l++; off++; }
        off += col - 1; if (off > len) off = len;
        int s0 = off, e0 = off;
        while (s0 > 0 && (isalnum((unsigned char)t[s0 - 1]) || t[s0 - 1] == '_')) s0--;
        while (e0 < len && (isalnum((unsigned char)t[e0]) || t[e0] == '_')) e0++;
        snprintf(G.rename_old, sizeof G.rename_old, "%.*s", e0 - s0, t + s0); free(t);
        if (!G.rename_old[0] || !(isalpha((unsigned char)G.rename_old[0]) || G.rename_old[0] == '_') || sql_is_keyword(G.rename_old, (int)strlen(G.rename_old))) { app_status(a, "Put the caret on a parameter, variable or alias to rename it"); return false; }
        if (!strcasecmp(G.rename_old, d->key.name)) { app_status(a, "Renaming the object itself is a separate DDL operation, not a text edit"); return false; }
        textfield_set(&G.rename, G.rename_old);
        G.modal = M_RENAME; G.mdoc = d;
        return true; }
    case OBJCMD_APPLY_ALL: {
        /* OBJ-043: every modified object of this connection, planned and reviewed together */
        G.nbatch = 0; G.batch_i = 0; G.batch_running = false;
        for (int i = 0; i < MAX_DOCS; i++) {
            ObjDoc *o = G.docs[i];
            if (!o || !o->loaded || !o->dirty || o->read_only || strcmp(o->key.conn_id, d->key.conn_id)) continue;
            objsnap_free(&o->remote); o->have_remote = false; o->allow_destructive = false;
            make_plan(a, o);
            G.batch[G.nbatch++] = o;
        }
        if (!G.nbatch) { app_status(a, "No modified objects on %s", d->conn_name); return false; }
        G.modal = M_BATCH; G.mdoc = d; G.consent = false; G.mscroll = 0;
        return true; }
    case OBJCMD_EXTRACT_CTE: case OBJCMD_NAMED_PARAMS: case OBJCMD_QUALIFY: {
        if (!ed || d->read_only) return false;
        G.ref_kind = cmd == OBJCMD_EXTRACT_CTE ? 1 : cmd == OBJCMD_NAMED_PARAMS ? 2 : 3;
        if (G.ref_kind == 1) {
            int sl = 0; char *sel = editor_selection(ed, &sl); G.ref_s0 = editor_selection_start(ed); G.ref_s1 = G.ref_s0 + sl; free(sel);
            if (!sl) { app_status(a, "Select a subquery (SELECT ...) used in FROM / JOIN first"); return false; }
            textfield_set(&G.ref_name, "sub");
        }
        G.modal = M_REFACTOR; G.mdoc = d; G.mscroll = 0;
        return true; }
    case OBJCMD_SUBMIT_REVIEW: {
        /* OBJ-045: a review artifact (diff + deployment SQL + SHA-256) for someone else to approve
         * outside Lume: there is no approval engine here, and none is pretended */
        if (!d->loaded) return false;
        if (!d->author[0]) snprintf(d->author, sizeof d->author, "%s", os_user());
        make_plan(a, d);
        int len; char *t = doc_text(a, d, &len);
        const char *old = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : "";
        TextLines A, B; lines_split(&A, old, (int)strlen(old)); lines_split(&B, t, len);
        Diff df; char *ud = text_diff(&A, &B, false, 20000, &df) ? diff_unified(&A, &B, &df, 3) : strdup("(too different for a line diff: see the deployment SQL)\n");
        char *sql = objplan_script(&d->plan, d->dialect);
        size_t bl = strlen(ud) + strlen(sql) + 256; char *body = malloc(bl);
        snprintf(body, bl, "---- diff (stored definition -> proposed)\n%s---- deployment SQL (%s)\n%s", ud, dialect_name(d->dialect), sql);
        char hex[65]; sha256_hex(body, strlen(body), hex);
        char disp[700]; objkey_display(&d->key, disp, sizeof disp);
        time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
        char dir[1100]; pref_dir(dir, sizeof dir); strncat(dir, "/reviews", sizeof dir - strlen(dir) - 1); mkdir_p(dir);
        char path[1500]; snprintf(path, sizeof path, "%s/%s-%s-%ld.review.sql", dir, d->conn_name, d->key.name, (long)now);
        for (char *q = path + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '/') *q = '_';
        FILE *f = fopen(path, "wb");
        if (f) {
            fprintf(f, "-- Lume Data change review (OBJ-045)\n-- Object: %s\n-- Connection: %s (%s, %s)\n-- Author: %s\n-- Created: %s\n-- Base fingerprint: %016llx\n"
                       "-- Strategy: %s; risk: %s; %s\n%s%s%s-- Approval: outside Lume (this version has no approval engine)\n-- SHA-256 of everything after the next line: %s\n-- ----\n%s",
                    disp, d->conn_name, conn_of(a, d->key.conn_id) ? conn_env_name(conn_of(a, d->key.conn_id)->env) : "", dialect_name(d->dialect), d->author, ts,
                    (unsigned long long)d->base.fingerprint, d->plan.strategy[0] ? d->plan.strategy : "-", risk_name(d->plan.risk), atomicity_name(d->plan.atomicity),
                    d->plan.blocked ? "-- Not runnable as is: " : "", d->plan.blocked ? d->plan.block_reason : "", d->plan.blocked ? "\n" : "", hex, body);
            fclose(f); chmod(path, 0600);
            snprintf(d->msg, sizeof d->msg, "Review artifact written (SHA-256 %.12s...): %s", hex, path);
        } else snprintf(d->msg, sizeof d->msg, "Could not write the review artifact: %s", strerror(errno));
        app_status(a, "%s", d->msg);
        free(body); free(sql); free(ud); free(t); diff_free(&df); lines_free(&A); lines_free(&B);
        return f != NULL; }
    case OBJCMD_GOTO_DEF: {
        if (!ed) return false;
        int line, col; editor_cursor_pos(ed, &line, &col);
        int len; char *t = editor_text(ed, &len);
        int off = 0, l = 1; while (off < len && l < line) { if (t[off] == '\n') l++; off++; }
        off += col - 1; if (off > len) off = len;
        int s = off, e = off;
        while (s > 0 && (isalnum((unsigned char)t[s - 1]) || t[s - 1] == '_' || t[s - 1] == '$')) s--;
        while (e < len && (isalnum((unsigned char)t[e]) || t[e] == '_' || t[e] == '$')) e++;
        char word[128]; snprintf(word, sizeof word, "%.*s", e - s, t + s); free(t);
        if (!word[0]) return false;
        for (int i = 0; i < MAX_LISTS; i++) if (G.lists[i].used && !strcmp(G.lists[i].conn_id, d->key.conn_id))
            for (int k = 0; k < G.lists[i].n; k++) if (!strcasecmp(G.lists[i].items[k].key.name, word) && !objkey_equal(&G.lists[i].items[k].key, &d->key))
                return objed_open(a, c, &G.lists[i].items[k].key);
        for (int i = 0; c && i < c->ntables; i++) {
            const char *n = c->tables[i].name, *dot = strrchr(n, '.');
            if (!strcasecmp(dot ? dot + 1 : n, word) && c->tables[i].is_view) {
                ObjKey k; memset(&k, 0, sizeof k); k.kind = OBJK_VIEW; snprintf(k.name, sizeof k.name, "%s", dot ? dot + 1 : n);
                if (dot) snprintf(k.schema, sizeof k.schema, "%.*s", (int)(dot - n), n); else snprintf(k.schema, sizeof k.schema, "%s", d->dialect == DIALECT_SQLITE ? "main" : d->key.schema);
                return objed_open(a, c, &k);
            }
            if (!strcasecmp(dot ? dot + 1 : n, word)) { app_status(a, "%s is a table: its structure is in the Explorer (tables are not edited here)", n); return true; }
        }
        app_status(a, "No definition found for %s (objects listed in the Explorer are searched)", word);
        return false; }
    }
    return false;
}

/* ------------------------------------------------------------------------ */
/* the tab body: header, toolbar, editor, bottom pane                        */
/* ------------------------------------------------------------------------ */

enum { DROP_FORMAT = 1, DROP_VALIDATE, DROP_MORE };
enum { DI_FORMAT_DOC = 100, DI_FORMAT_SEL, DI_VALIDATE_LOCAL, DI_VALIDATE_SERVER, DI_COMPILE, DI_COMPARE, DI_REFRESH, DI_ORIGINAL, DI_EXPORT, DI_DEPLOY_SQL,
       DI_COPY_ORIG, DI_DEPS, DI_PROPS, DI_CONSOLE, DI_DISCARD, DI_RECONCILE, DI_SPEC, DI_BODY, DI_GOTO, DI_RENAME, DI_APPLY_ALL, DI_MV_REFRESH, DI_EXTRACT_CTE, DI_NAMED_PARAMS, DI_QUALIFY, DI_SUBMIT_REVIEW,
       DI_OTHER_BASE = 200 };

static void open_drop(App *a, ObjDoc *d, int which, float x, float y) {
    PopupMenu *m = &G.drop; popup_open(m, x, y); G.drop_doc = d;
    ObjCaps cp = d->eng->caps(d->key.kind, d->base.server_version);
    if (which == DROP_FORMAT) {
        popup_add(m, DI_FORMAT_DOC, "Format Document", false, cp.format && !d->read_only);
        popup_add(m, DI_FORMAT_SEL, "Format Selection", false, cp.format && !d->read_only);
    } else if (which == DROP_VALIDATE) {
        popup_add(m, DI_VALIDATE_LOCAL, "Validate (local)", false, true);
        popup_add(m, DI_VALIDATE_SERVER, cp.server_validate ? "Validate on Server (rolled back)" : "Validate on Server (not available)", false, cp.server_validate);
        if (cp.server_compile) popup_add(m, DI_COMPILE, "Compile\xE2\x80\xA6", true, true);
    } else {
        popup_add(m, DI_COMPARE, "Compare With Database\xE2\x80\xA6", false, true);
        /* OBJ-066: the same object on another open connection of the same engine */
        static char other_lbl[8][96]; int no = 0;
        for (int i = 0; a->conns && i < a->conns->n && no < 8; i++) {
            Connection *oc = a->conns->items[i];
            if (!oc->db || !strcmp(oc->id, d->key.conn_id) || dialect_of(oc) != d->dialect) continue;
            snprintf(other_lbl[no], sizeof other_lbl[0], "Compare With %s\xE2\x80\xA6", oc->name);
            popup_add(m, DI_OTHER_BASE + i, other_lbl[no], false, true); no++;
        }
        popup_add(m, DI_REFRESH, "Refresh Definition", false, true);
        if (cp.refresh) popup_add(m, DI_MV_REFRESH, "Refresh Materialized View Data\xE2\x80\xA6", false, true);
        if (d->state == ST_UNKNOWN) popup_add(m, DI_RECONCILE, "Reconcile With Server\xE2\x80\xA6", false, true);
        popup_add(m, DI_ORIGINAL, "Show Original Source", true, true);
        popup_add(m, DI_COPY_ORIG, "Copy Original Source", false, d->base.original != NULL);
        popup_add(m, DI_DEPLOY_SQL, "Generate Deployment SQL\xE2\x80\xA6", false, true);
        popup_add(m, DI_SUBMIT_REVIEW, "Submit for Review\xE2\x80\xA6", false, d->loaded);
        popup_add(m, DI_EXPORT, "Export Definition\xE2\x80\xA6", false, true);
        popup_add(m, DI_CONSOLE, "Open in Query Console", false, true);
        popup_add(m, DI_GOTO, "Go to Definition (F12)", true, true);
        popup_add(m, DI_RENAME, "Rename Symbol\xE2\x80\xA6", false, !d->read_only);
        popup_add(m, DI_EXTRACT_CTE, "Extract Subquery to CTE\xE2\x80\xA6", false, !d->read_only);
        popup_add(m, DI_QUALIFY, "Qualify Columns\xE2\x80\xA6", false, !d->read_only);
        if (d->dialect == DIALECT_PG) popup_add(m, DI_NAMED_PARAMS, "Use Parameter Names ($n)\xE2\x80\xA6", false, !d->read_only);
        popup_add(m, DI_APPLY_ALL, "Review All Changes on This Connection\xE2\x80\xA6", true, !d->read_only);
        popup_add(m, DI_DEPS, "Dependencies\xE2\x80\xA6", false, true);
        popup_add(m, DI_PROPS, "Properties\xE2\x80\xA6", false, true);
        if (d->key.kind == OBJK_PKG_SPEC) popup_add(m, DI_BODY, "Open Body", true, true);
        if (d->key.kind == OBJK_PKG_BODY) popup_add(m, DI_SPEC, "Open Specification", true, true);
        popup_add(m, DI_DISCARD, "Discard Draft", true, d->dirty && !d->read_only);
    }
    (void)a;
}
static void run_drop(App *a, ObjDoc *d, int id) {
    static const int map[][2] = { { DI_FORMAT_DOC, OBJCMD_FORMAT }, { DI_FORMAT_SEL, OBJCMD_FORMAT_SEL }, { DI_VALIDATE_LOCAL, OBJCMD_VALIDATE }, { DI_VALIDATE_SERVER, OBJCMD_VALIDATE_SERVER },
        { DI_COMPILE, OBJCMD_COMPILE }, { DI_COMPARE, OBJCMD_COMPARE }, { DI_REFRESH, OBJCMD_REFRESH }, { DI_ORIGINAL, OBJCMD_SHOW_ORIGINAL }, { DI_EXPORT, OBJCMD_EXPORT },
        { DI_DEPLOY_SQL, OBJCMD_DEPLOY_SQL }, { DI_DEPS, OBJCMD_DEPENDENCIES }, { DI_PROPS, OBJCMD_PROPERTIES }, { DI_CONSOLE, OBJCMD_CONSOLE }, { DI_DISCARD, OBJCMD_DISCARD },
        { DI_RECONCILE, OBJCMD_RECONCILE }, { DI_GOTO, OBJCMD_GOTO_DEF }, { DI_RENAME, OBJCMD_RENAME }, { DI_APPLY_ALL, OBJCMD_APPLY_ALL },
        { DI_MV_REFRESH, OBJCMD_MV_REFRESH }, { DI_EXTRACT_CTE, OBJCMD_EXTRACT_CTE }, { DI_NAMED_PARAMS, OBJCMD_NAMED_PARAMS }, { DI_QUALIFY, OBJCMD_QUALIFY }, { DI_SUBMIT_REVIEW, OBJCMD_SUBMIT_REVIEW }, { 0, 0 } };
    if (id >= DI_OTHER_BASE && a->conns && id - DI_OTHER_BASE < a->conns->n) {
        Connection *oc = a->conns->items[id - DI_OTHER_BASE];
        if (!oc->db) return;
        snprintf(G.compare_with, sizeof G.compare_with, "%s", oc->name);
        d->remote_for = RF_COMPARE_OTHER;
        doc_start(a, d, JK_REMOTE, objsrc_load(oc->db, d->eng, &d->key));
        app_status(a, "Reading %s from %s...", d->key.name, oc->name);
        return;
    }
    if (id == DI_COPY_ORIG) { if (d->base.original) { SDL_SetClipboardText(d->base.original); app_status(a, "Original source copied"); } return; }
    if (id == DI_SPEC || id == DI_BODY) {
        ObjKey k = d->key; k.kind = id == DI_SPEC ? OBJK_PKG_SPEC : OBJK_PKG_BODY;
        objed_open(a, conn_of(a, d->key.conn_id), &k); return;
    }
    for (int i = 0; map[i][0]; i++) if (map[i][0] == id) { objed_command(a, map[i][1]); return; }
}

static void draw_badge(App *a, const char *text, uint32_t color, float x, float y, float h, float *w_out) {
    float w = font_width(a->font_small, text, -1) + px(a, 12);
    Rect r = rect_make(x, y, w, h);
    draw_rrect(a->renderer, r, px(a, 4), color_alpha(color_hex(color), 60));
    draw_rect_outline(a->renderer, r, px(a, 1), color_hex(color));
    draw_text_fit(a->font_small, a->renderer, text, -1, r, color_hex(T_TEXT), ALIGN_CENTER);
    *w_out = w;
}

void objed_tab_body(App *a, ObjDoc *d, Editor *ed, Rect body) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Connection *c = conn_of(a, d->key.conn_id);
    /* dirty tracking and debounced diagnostics */
    uint32_t rev = editor_revision(ed);
    if (d->loaded && rev != d->seen_rev) {
        d->seen_rev = rev; d->changed_at = now_s();
        int len; char *t = editor_text(ed, &len);
        const char *base = d->base.ddl ? d->base.ddl : d->base.original ? d->base.original : "";
        bool was = d->dirty;
        d->dirty = strcmp(t, base) != 0;
        free(t);
        if (d->state == ST_APPLIED && d->dirty) d->msg[0] = 0;   /* "Applied ..." no longer describes the text */
        if (d->state == ST_SYNCED || d->state == ST_DIRTY || d->state == ST_APPLIED) d->state = d->dirty ? ST_DIRTY : ST_SYNCED;
        if (!d->dirty && was && d->draft_saved) draft_delete(d);
        if (d->validated == 2) d->validated = 0;
        d->nserver = d->state == ST_FAILED ? d->nserver : 0;
    }
    if (d->loaded && d->diag_rev != rev && now_s() - d->changed_at > 0.25) run_diagnostics(a, d, ed);
    else if (d->loaded && d->diag_rev != rev) app_wake_in(a, 260);
    /* autosave of the draft after a pause (crash safety; Save Draft is the explicit one) */
    if (d->loaded && d->dirty && !d->read_only && d->draft_rev != rev && now_s() - d->changed_at > 2.0) draft_save(a, d);
    else if (d->loaded && d->dirty && d->draft_rev != rev) app_wake_in(a, 2100);

    float h1 = px(a, 28), h2 = px(a, 34);
    Rect hdr = rect_make(body.x, body.y, body.w, h1 + h2);
    draw_rect(r, hdr, color_hex(T_PANEL_ALT));
    draw_rect(r, rect_make(hdr.x, hdr.y + hdr.h - px(a, 1), hdr.w, px(a, 1)), color_hex(T_BORDER));
    /* row 1: Object: KIND schema.name(sig)  [ENV] [state] ... status text ... Problems n */
    char disp[700]; objkey_display(&d->key, disp, sizeof disp);
    char line[800]; snprintf(line, sizeof line, "Object: %s", disp);
    float x = hdr.x + px(a, 12);
    float tw = font_width(a->font_ui, line, -1);
    draw_text_fit(a->font_ui, r, line, -1, rect_make(x, hdr.y, fminf(tw + px(a, 4), hdr.w * 0.5f), h1), color_hex(T_TEXT), ALIGN_LEFT);
    x += fminf(tw + px(a, 4), hdr.w * 0.5f) + px(a, 10);
    float bw;
    if (c && c->env != CONN_ENV_NONE) { draw_badge(a, conn_env_name(c->env), conn_env_color(c), x, hdr.y + px(a, 5), h1 - px(a, 10), &bw); x += bw + px(a, 6); }
    uint32_t sc = d->state == ST_FAILED || d->state == ST_LOAD_FAILED ? T_RED : d->state == ST_CONFLICT || d->state == ST_UNKNOWN ? T_YELLOW : d->state == ST_DIRTY ? T_ACCENT : d->state == ST_APPLIED ? T_GREEN : T_TEXT_DIM;
    draw_badge(a, state_text(d), sc, x, hdr.y + px(a, 5), h1 - px(a, 10), &bw); x += bw + px(a, 6);
    if (d->read_only) { draw_badge(a, "Read-only", T_TEXT_DIM, x, hdr.y + px(a, 5), h1 - px(a, 10), &bw); ui_record("obj:read-only", rect_make(x, hdr.y, bw, h1)); x += bw + px(a, 6); }
    if (d->base.status[0]) { draw_badge(a, d->base.status, strcasecmp(d->base.status, "VALID") ? T_RED : T_GREEN, x, hdr.y + px(a, 5), h1 - px(a, 10), &bw); x += bw + px(a, 6); }
    int ne = count_sev(d, DIAG_ERROR), nw = count_sev(d, DIAG_WARNING);
    char pb[64]; snprintf(pb, sizeof pb, "Problems: %d", ne + nw);
    float pw = font_width(a->font_small, pb, -1) + px(a, 16);
    Rect prb = rect_make(hdr.x + hdr.w - pw - px(a, 10), hdr.y + px(a, 4), pw, h1 - px(a, 8));
    if (ui_button(a, prb, pb, BTN_QUIET, true)) { d->bottom_open = !(d->bottom_open && d->bottom_tab == 0); d->bottom_tab = 0; }
    ui_record("obj:problems", prb);
    /* row 2: toolbar */
    float by = hdr.y + h1, bh = h2 - px(a, 8);
    float bxx = hdr.x + px(a, 10);
    ObjCaps cp = d->eng->caps(d->key.kind, d->base.server_version);
    bool busy = d->job != NULL || !d->loaded;
    struct { const char *label, *tag; int cmd; bool enabled; int style; } btn[] = {
        { "Save Draft", "obj:save-draft", OBJCMD_SAVE_DRAFT, d->loaded && !d->read_only, BTN_NORMAL },
        { "Format   ", "obj:format", -DROP_FORMAT, d->loaded && cp.format && !d->read_only, BTN_NORMAL },
        { "Validate   ", "obj:validate", -DROP_VALIDATE, d->loaded && !busy, BTN_NORMAL },
        { "Diff", "obj:diff", OBJCMD_DIFF, d->loaded, BTN_NORMAL },
        { "Apply\xE2\x80\xA6", "obj:apply", OBJCMD_APPLY, d->loaded && !busy && !d->read_only && cp.source_edit, BTN_PRIMARY },
        { "More   ", "obj:more", -DROP_MORE, d->loaded, BTN_QUIET },
    };
    for (int i = 0; i < 6; i++) {
        float w = font_width(a->font_ui, btn[i].label, -1) + px(a, 22);
        Rect br = rect_make(bxx, by + px(a, 4), w, bh);
        if (ui_button(a, br, btn[i].label, btn[i].style, btn[i].enabled)) {
            if (btn[i].cmd > 0) objed_command(a, btn[i].cmd);
            else open_drop(a, d, -btn[i].cmd, br.x, br.y + br.h + px(a, 2));
        }
        ui_record(btn[i].tag, br);
        if (btn[i].cmd < 0)   /* dropdown: a drawn triangle (the UI font has no arrow glyph) */
            draw_triangle(r, br.x + br.w - px(a, 13), br.y + br.h / 2, px(a, 7), px(a, 4), true, color_hex(btn[i].enabled ? T_TEXT_MUTED : T_TEXT_DIM));
        bxx += w + px(a, 6);
    }
    if (d->job && (d->job_kind == JK_APPLY || d->job_kind == JK_DESCRIBE || d->job_kind == JK_REMOTE)) {
        Rect cr = rect_make(bxx + px(a, 4), by + px(a, 4), px(a, 70), bh);
        if (ui_button(a, cr, "Cancel", BTN_QUIET, true)) objsrc_cancel(d->job);
        ui_record("obj:cancel", cr);
        bxx += px(a, 80);
    }
    /* status text: engine and version, then the last message, in words (not colour only) */
    char st[600];
    const char *vchk = d->validated == 2 ? "Valid on server" : d->validated == 1 ? "Syntax OK (local)" : d->validated == -1 ? "Problems found (local)" : "Not validated";
    snprintf(st, sizeof st, "%s%s \xC2\xB7 %s \xC2\xB7 %s%s%s", dialect_name(d->dialect), d->base.server_version ? "" : "", state_text(d), vchk,
             d->msg[0] ? " \xC2\xB7 " : "", d->msg);
    draw_text_fit(a->font_small, r, st, -1, rect_make(bxx + px(a, 8), by, hdr.x + hdr.w - bxx - px(a, 16), h2), color_hex(d->state == ST_FAILED || d->state == ST_LOAD_FAILED ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("obj:status", rect_make(bxx + px(a, 8), by, hdr.x + hdr.w - bxx - px(a, 16), h2));
    /* keyboard: F8 / Shift+F8 problems, F12 definition, Cmd+S draft (object tabs only) */
    if (a->focus == FOCUS_EDITOR && a->editor == ed) {
        unsigned mods;
        if (input_key_any(in, SDLK_F8, &mods)) objed_command(a, (mods & SDL_KMOD_SHIFT) ? OBJCMD_PREV_PROBLEM : OBJCMD_NEXT_PROBLEM);
        if (input_key(in, SDLK_F12, 0)) objed_command(a, OBJCMD_GOTO_DEF);
    }
    /* bottom pane */
    float bot_h = d->bottom_open ? fminf(px(a, 150), body.h * 0.35f) : px(a, 26);
    Rect ebody = rect_make(body.x, hdr.y + hdr.h, body.w, body.h - hdr.h - bot_h);
    editor_update(ed, a, ebody);
    /* the routines of this schema (signature help, go to definition): asked once, in the background */
    if (d->loaded && c && c->db) objed_list(a, c, d->key.schema, true);
    /* OBJ-018: inside "name(" of a listed routine, its signature(s) under the editor */
    if (a->focus == FOCUS_EDITOR && a->editor == ed) {
        int line, col; editor_cursor_pos(ed, &line, &col);
        int len; char *t = editor_text(ed, &len);
        int off = 0, l = 1; while (off < len && l < line) { if (t[off] == '\n') l++; off++; }
        off += col - 1; if (off > len) off = len;
        int depth = 0, p = off - 1, lstart = off - 400 > 0 ? off - 400 : 0;
        for (; p >= lstart; p--) { if (t[p] == ')') depth++; else if (t[p] == '(') { if (!depth) break; depth--; } else if (t[p] == ';') { p = -1; break; } }
        if (p >= lstart && p > 0) {
            int e = p; while (e > 0 && t[e - 1] == ' ') e--;
            int s0 = e; while (s0 > 0 && (isalnum((unsigned char)t[s0 - 1]) || t[s0 - 1] == '_')) s0--;
            char nm[128]; snprintf(nm, sizeof nm, "%.*s", e - s0, t + s0);
            /* which argument the caret is in: top-level commas between "(" and the caret */
            int argi = 0, dp = 0;
            for (int q = p + 1; q < off; q++) { if (t[q] == '(') dp++; else if (t[q] == ')') dp--; else if (t[q] == ',' && !dp) argi++; }
            char hint[900] = ""; int ho = 0;
            for (int i = 0; nm[0] && i < MAX_LISTS; i++) if (G.lists[i].used && !strcmp(G.lists[i].conn_id, d->key.conn_id))
                for (int k = 0; k < G.lists[i].n; k++) {
                    const ObjListItem *it = &G.lists[i].items[k];
                    const ObjKey *ok = &it->key;
                    if (!((ok->kind == OBJK_FUNCTION || ok->kind == OBJK_PROCEDURE) && !strcasecmp(ok->name, nm)) || ho > 700) continue;
                    /* names, types and the current argument in [ ] (OBJ-018); types only when the engine gives no names */
                    const char *args = it->args[0] ? it->args : ok->signature;
                    ho += snprintf(hint + ho, sizeof hint - (size_t)ho, "%s%s(", ho ? "   |   " : "", ok->name);
                    int ai = 0, depth2 = 0; const char *a0 = args;
                    for (const char *q = args;; q++) {
                        if (*q == '(') depth2++; else if (*q == ')') depth2--;
                        if (*q == 0 || (*q == ',' && !depth2)) {
                            const char *s0 = a0; while (*s0 == ' ') s0++;
                            ho += snprintf(hint + ho, sizeof hint - (size_t)ho, "%s%s%.*s%s", ai ? ", " : "", ai == argi ? "[" : "", (int)(q - s0), s0, ai == argi ? "]" : "");
                            ai++; a0 = q + 1;
                            if (!*q) break;
                        }
                    }
                    ho += snprintf(hint + ho, sizeof hint - (size_t)ho, ")%s%s%s%s", it->returns[0] ? " returns " : "", it->returns, it->comment[0] ? "  -  " : "", it->comment);
                }
            if (hint[0]) {
                float hw = fminf(font_width(a->font_mono, hint, -1) + px(a, 20), ebody.w - px(a, 20));
                Rect hr = rect_make(ebody.x + ebody.w - hw - px(a, 12), ebody.y + ebody.h - px(a, 30), hw, px(a, 24));
                draw_rrect(r, hr, px(a, 5), color_hex(T_PANEL_ALT)); draw_rect_outline(r, hr, px(a, 1), color_hex(T_BORDER));
                draw_text_fit(a->font_mono, r, hint, -1, rect_inset(hr, px(a, 10), 0), color_hex(T_TEXT), ALIGN_LEFT);
                ui_record("obj:signature", hr);
                snprintf(G.last_hint, sizeof G.last_hint, "%s", hint);
            }
        }
        free(t);
    }
    Rect bot = rect_make(body.x, ebody.y + ebody.h, body.w, bot_h);
    draw_rect(r, bot, color_hex(T_PANEL));
    draw_rect(r, rect_make(bot.x, bot.y, bot.w, px(a, 1)), color_hex(T_BORDER));
    const char *tabs[] = { "Problems", "Dependencies", "Original source" };
    float tx = bot.x + px(a, 8);
    for (int i = 0; i < 3; i++) {
        char lbl[64]; if (i == 0) snprintf(lbl, sizeof lbl, "Problems (%d)", ne + nw + count_sev(d, DIAG_INFO)); else if (i == 1) snprintf(lbl, sizeof lbl, "Dependencies (%d)", d->base.ndeps); else snprintf(lbl, sizeof lbl, "%s", tabs[i]);
        float w = font_width(a->font_small, lbl, -1) + px(a, 18);
        Rect tr = rect_make(tx, bot.y + px(a, 2), w, px(a, 22));
        bool act = d->bottom_open && d->bottom_tab == i;
        if (act) draw_rect(r, rect_make(tr.x, tr.y + tr.h - px(a, 2), tr.w, px(a, 2)), color_hex(T_ACCENT));
        draw_text_fit(a->font_small, r, lbl, -1, tr, color_hex(act ? T_TEXT : T_TEXT_MUTED), ALIGN_CENTER);
        char tg[48]; snprintf(tg, sizeof tg, "obj:pane:%s", tabs[i]); ui_record(tg, tr);
        if (rect_contains(tr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; if (act) d->bottom_open = false; else { d->bottom_open = true; d->bottom_tab = i; } }
        tx += w + px(a, 4);
    }
    if (!d->bottom_open) return;
    Rect list = rect_make(bot.x, bot.y + px(a, 26), bot.w, bot.h - px(a, 26));
    clip_push(r, list);
    float rh = px(a, 20), y = list.y + px(a, 2);
    if (d->bottom_tab == 0) {
        int total = d->nserver + d->ndiags;
        if (!total) draw_text_fit(a->font_small, r, d->validated ? "No problems found. Local checks do not compile the code: Validate on server / Apply do." : "Problems appear as you type (parser, catalog) and after validation or Apply (server).", -1, rect_make(list.x + px(a, 12), y, list.w - px(a, 24), rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        for (int i = 0; i < total && y < list.y + list.h; i++, y += rh) {
            const ObjDiag *g = i < d->nserver ? &d->server_diags[i] : &d->diags[i - d->nserver];
            Rect row = rect_make(list.x, y, list.w, rh);
            bool hov = rect_contains(row, in->mouse_x, in->mouse_y);
            if (hov || (i >= d->nserver && i - d->nserver == d->prob_sel)) draw_rect(r, row, color_hex(T_HOVER));
            const char *sev = diag_severity_name(g->severity);
            char text[600];
            snprintf(text, sizeof text, "%-7s %s%s line %d%s  %s  %s  [%s%s]", sev, g->unit[0] && strcmp(g->unit, "MAIN") ? g->unit : "", g->unit[0] && strcmp(g->unit, "MAIN") ? " " : "",
                     g->line, g->confidence == 2 ? " (approx.)" : "", g->code, g->message, diag_source_name(g->source), g->has_fix ? ", quick fix" : "");
            draw_text_fit(a->font_small, r, text, -1, rect_make(row.x + px(a, 12), row.y, row.w - px(a, 140), rh),
                          color_hex(g->severity == DIAG_ERROR ? T_RED : g->severity == DIAG_WARNING ? T_YELLOW : T_TEXT_MUTED), ALIGN_LEFT);
            char tg[48]; snprintf(tg, sizeof tg, "obj:problem:%d", i); ui_record(tg, row);
            if (g->has_fix && !d->read_only) {
                Rect fr = rect_make(row.x + row.w - px(a, 124), row.y + px(a, 1), px(a, 116), rh - px(a, 2));
                if (ui_button(a, fr, g->fix_label, BTN_QUIET, true)) { G.fix = *g; G.modal = M_FIX; G.mdoc = d; }   /* OBJ-028: preview first */
                char tf[48]; snprintf(tf, sizeof tf, "obj:fix:%d", i); ui_record(tf, fr);
            }
            if (hov && in->mouse_pressed && !in->mouse_consumed) {
                in->mouse_consumed = true;
                if (g->start >= 0) { editor_goto_offset(ed, g->start); a->focus = FOCUS_EDITOR; }
                else if (g->line > 0) {
                    int len; char *t = editor_text(ed, &len); int off = 0, l = 1;
                    while (off < len && l < g->line) { if (t[off] == '\n') l++; off++; }
                    free(t); editor_goto_offset(ed, off + (g->col > 0 ? g->col - 1 : 0)); a->focus = FOCUS_EDITOR;
                }
                if (i >= d->nserver) d->prob_sel = i - d->nserver;
            }
        }
    } else if (d->bottom_tab == 1) {
        if (!d->base.deps_known) draw_text_fit(a->font_small, r, "This engine does not report dependents: check them before publishing.", -1, rect_make(list.x + px(a, 12), y, list.w, rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        else if (!d->base.ndeps) draw_text_fit(a->font_small, r, "No dependent objects in the catalog.", -1, rect_make(list.x + px(a, 12), y, list.w, rh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        for (int i = 0; i < d->base.ndeps && y < list.y + list.h; i++, y += rh) {
            char text[400]; snprintf(text, sizeof text, "%-18s %s   (%s)", d->base.deps[i].kind, d->base.deps[i].name, d->base.deps[i].how);
            draw_text_fit(a->font_small, r, text, -1, rect_make(list.x + px(a, 12), y, list.w - px(a, 24), rh), color_hex(T_TEXT), ALIGN_LEFT);
        }
    } else {
        /* OBJ-007: the source as the server keeps it, next to the executable DDL being edited */
        const char *o = d->base.original ? d->base.original : "";
        if (d->base.missing[0]) { draw_text_fit(a->font_small, r, d->base.missing, -1, rect_make(list.x + px(a, 12), y, list.w, rh), color_hex(T_YELLOW), ALIGN_LEFT); y += rh; }
        int ln = 0, total_lines = 1; for (const char *q = o; *q; q++) total_lines += *q == '\n';
        if (d->scroll_bottom > total_lines - 1) d->scroll_bottom = (float)(total_lines - 1);
        ui_scrollbar_input_rows(a, &d->sb_bottom, list, (float)total_lines + 1, rh, &d->scroll_bottom);
        for (const char *p = o; *p && y < list.y + list.h; ln++) {
            const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p);
            if (ln >= (int)(d->scroll_bottom)) { draw_text_fit(a->font_mono, r, p, l, rect_make(list.x + px(a, 12), y, list.w - px(a, 24), rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT); y += rh; }
            p = e ? e + 1 : p + l;
        }
        if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { d->scroll_bottom -= in->wheel_y / px(a, 20); if (d->scroll_bottom < 0) d->scroll_bottom = 0; }
        ui_scrollbar_draw_rows(a, &d->sb_bottom, list, (float)total_lines + 1, rh, d->scroll_bottom);
    }
    clip_pop(r);
}

/* ------------------------------------------------------------------------ */
/* context menus (Explorer)                                                  */
/* ------------------------------------------------------------------------ */

enum { MI_OPEN = 300, MI_OPEN_SPEC, MI_OPEN_BODY, MI_OPEN_BOTH, MI_PREVIEW, MI_CONSOLE, MI_VALIDATE, MI_COMPARE, MI_DEPLOY_SQL, MI_EXPORT, MI_COPY_ORIG,
       MI_DEPS, MI_REFRESH, MI_PROPS, MI_SIGNATURE, MI_COMPILE, MI_ERRORS, MI_TABLE, MI_EVENTS, MI_ENABLE, MI_DISABLE,
       MI_TPL_VIEW, MI_TPL_FUNCTION, MI_TPL_PROCEDURE, MI_TPL_TRIGGER, MI_TPL_PACKAGE, MI_RELOAD_LIST, MI_MV_REFRESH };

void objed_object_menu(App *a, Connection *c, const ObjKey *k, float x, float y) {
    PopupMenu *m = &G.menu; popup_open(m, x, y);
    G.menu_group = false; snprintf(G.menu_conn, sizeof G.menu_conn, "%s", c->id); G.menu_key = *k;
    const ObjEngine *e = obj_engine_for_driver(c->driver);
    ObjCaps cp = e ? e->caps(k->kind, 0) : (ObjCaps){ 0 };
    bool pkg = k->kind == OBJK_PKG_SPEC || k->kind == OBJK_PKG_BODY;
    if (pkg) {
        popup_add(m, MI_OPEN_SPEC, "Open Specification", false, cp.source_read);
        popup_add(m, MI_OPEN_BODY, "Open Body", false, cp.source_read);
        popup_add(m, MI_OPEN_BOTH, "Open Both", false, cp.source_read);
        popup_add(m, MI_COMPILE, k->kind == OBJK_PKG_SPEC ? "Compile Specification\xE2\x80\xA6" : "Compile Body\xE2\x80\xA6", true, cp.server_compile);
        popup_add(m, MI_ERRORS, "Show Compilation Errors", false, true);
    } else {
        popup_add(m, MI_OPEN, "Open Definition", false, cp.source_read);
        if (k->kind == OBJK_VIEW || k->kind == OBJK_MATVIEW) popup_add(m, MI_PREVIEW, "Open Preview / View Data", false, c->db != NULL);
        if (k->kind == OBJK_FUNCTION || k->kind == OBJK_PROCEDURE) popup_add(m, MI_SIGNATURE, "Show Signature / Parameters", false, true);
        if (k->kind == OBJK_TRIGGER) { popup_add(m, MI_TABLE, "Show Associated Table", false, true); popup_add(m, MI_EVENTS, "Show Timing and Events", false, cp.source_read); }
    }
    popup_add(m, MI_CONSOLE, "Open in Query Console", false, cp.source_read);
    popup_add(m, MI_VALIDATE, "Validate Source\xE2\x80\xA6", true, cp.source_read);
    popup_add(m, MI_COMPARE, "Compare With Database\xE2\x80\xA6", false, cp.source_read);
    if (!pkg && cp.server_compile) popup_add(m, MI_COMPILE, "Compile\xE2\x80\xA6", false, true);
    if (k->kind == OBJK_TRIGGER && cp.enable_disable) { popup_add(m, MI_ENABLE, "Enable Trigger\xE2\x80\xA6", false, !c->read_only); popup_add(m, MI_DISABLE, "Disable Trigger\xE2\x80\xA6", false, !c->read_only); }
    popup_add(m, MI_COPY_ORIG, "Copy Original Source", true, cp.source_read);
    popup_add(m, MI_DEPLOY_SQL, "Generate Deployment SQL\xE2\x80\xA6", false, cp.source_read);
    popup_add(m, MI_EXPORT, "Export Definition\xE2\x80\xA6", false, cp.source_read);
    popup_add(m, MI_DEPS, "Dependencies\xE2\x80\xA6", false, cp.source_read);
    popup_add(m, MI_REFRESH, "Refresh Definition", true, true);
    if (cp.refresh) popup_add(m, MI_MV_REFRESH, "Refresh Materialized View Data\xE2\x80\xA6", false, !c->read_only);
    popup_add(m, MI_PROPS, "Properties\xE2\x80\xA6", false, cp.source_read);
    (void)a;
}
void objed_group_menu(App *a, Connection *c, const char *schema, float x, float y) {
    PopupMenu *m = &G.menu; popup_open(m, x, y);
    G.menu_group = true; snprintf(G.menu_conn, sizeof G.menu_conn, "%s", c->id); snprintf(G.menu_schema, sizeof G.menu_schema, "%s", schema ? schema : "");
    int d = dialect_of(c);
    popup_add_caption(m, "New from template (opens a query console)", 0, false);
    popup_add(m, MI_TPL_VIEW, "View", false, true);
    popup_add(m, MI_TPL_FUNCTION, "Function", false, d != DIALECT_SQLITE);
    popup_add(m, MI_TPL_PROCEDURE, "Procedure", false, d != DIALECT_SQLITE);
    popup_add(m, MI_TPL_TRIGGER, "Trigger", false, true);
    if (d == DIALECT_ORACLE) popup_add(m, MI_TPL_PACKAGE, "Package", false, true);
    popup_add(m, MI_RELOAD_LIST, "Refresh Objects", true, c->db != NULL);
    (void)a;
}

/* OBJ-020: one template per dialect and kind; placeholders in angle brackets */
static const char *template_for(int d, int id) {
    switch (id) {
    case MI_TPL_VIEW: return d == DIALECT_SQLITE ? "CREATE VIEW <name> AS\nSELECT <columns>\nFROM <table>;\n" : d == DIALECT_MSSQL ? "CREATE OR ALTER VIEW <schema>.<name> AS\nSELECT <columns>\nFROM <table>;\nGO\n" :
                                 "CREATE OR REPLACE VIEW <schema>.<name> AS\nSELECT <columns>\nFROM <table>;\n";
    case MI_TPL_FUNCTION:
        if (d == DIALECT_PG) return "CREATE OR REPLACE FUNCTION <schema>.<name>(p_id integer)\nRETURNS integer\nLANGUAGE plpgsql\nAS $$\nBEGIN\n    RETURN p_id;\nEND;\n$$;\n";
        if (d == DIALECT_ORACLE) return "CREATE OR REPLACE FUNCTION <name>(p_id IN NUMBER) RETURN NUMBER IS\nBEGIN\n    RETURN p_id;\nEND <name>;\n/\n";
        if (d == DIALECT_MSSQL) return "CREATE OR ALTER FUNCTION dbo.<name>(@id int)\nRETURNS int\nAS\nBEGIN\n    RETURN @id;\nEND\nGO\n";
        return "CREATE FUNCTION <name>(p_id INT) RETURNS INT DETERMINISTIC\nBEGIN\n    RETURN p_id;\nEND\n";
    case MI_TPL_PROCEDURE:
        if (d == DIALECT_PG) return "CREATE OR REPLACE PROCEDURE <schema>.<name>(p_id integer)\nLANGUAGE plpgsql\nAS $$\nBEGIN\n    NULL;\nEND;\n$$;\n";
        if (d == DIALECT_ORACLE) return "CREATE OR REPLACE PROCEDURE <name>(p_id IN NUMBER) IS\nBEGIN\n    NULL;\nEND <name>;\n/\n";
        if (d == DIALECT_MSSQL) return "CREATE OR ALTER PROCEDURE dbo.<name> @id int\nAS\nBEGIN\n    SET NOCOUNT ON;\nEND\nGO\n";
        return "CREATE PROCEDURE <name>(IN p_id INT)\nBEGIN\n    SELECT p_id;\nEND\n";
    case MI_TPL_TRIGGER:
        if (d == DIALECT_SQLITE) return "CREATE TRIGGER <name> AFTER INSERT ON <table>\nBEGIN\n    SELECT 1;\nEND;\n";
        if (d == DIALECT_PG) return "CREATE OR REPLACE TRIGGER <name>\nBEFORE INSERT OR UPDATE ON <schema>.<table>\nFOR EACH ROW EXECUTE FUNCTION <schema>.<function>();\n";
        if (d == DIALECT_ORACLE) return "CREATE OR REPLACE TRIGGER <name>\nBEFORE INSERT ON <table>\nFOR EACH ROW\nBEGIN\n    NULL;\nEND;\n/\n";
        return "CREATE TRIGGER <name> BEFORE INSERT ON <table>\nFOR EACH ROW SET NEW.<column> = NEW.<column>;\n";
    case MI_TPL_PACKAGE: return "CREATE OR REPLACE PACKAGE <name> AS\n    FUNCTION f(p_id NUMBER) RETURN NUMBER;\nEND <name>;\n/\nCREATE OR REPLACE PACKAGE BODY <name> AS\n    FUNCTION f(p_id NUMBER) RETURN NUMBER IS\n    BEGIN\n        RETURN p_id;\n    END f;\nEND <name>;\n/\n";
    }
    return "";
}

static void run_menu(App *a, int id) {
    Connection *c = conn_of(a, G.menu_conn);
    if (!c) return;
    if (G.menu_group) {
        if (id == MI_RELOAD_LIST) { objed_list_invalidate(c); objed_list(a, c, G.menu_schema, true); explorer_rebuild(a->explorer, a); return; }
        const char *t = template_for(dialect_of(c), id);
        int tid = panels_open_tab(a->panels, a, "new object.sql", c->id, t);
        if (tid >= 0) panels_activate_tab(a->panels, a, tid);
        return;
    }
    ObjKey k = G.menu_key;
    if (id == MI_OPEN_SPEC || id == MI_OPEN_BODY || id == MI_OPEN_BOTH) {
        ObjKey s = k; s.kind = OBJK_PKG_SPEC; ObjKey b = k; b.kind = OBJK_PKG_BODY;
        if (id != MI_OPEN_BODY) objed_open(a, c, &s);
        if (id != MI_OPEN_SPEC) objed_open(a, c, &b);
        return;
    }
    if (id == MI_PREVIEW) {
        if (!c->db) return;
        char full[300], q[400], sql[700];
        if (k.schema[0] && dialect_of(c) != DIALECT_SQLITE) snprintf(full, sizeof full, "%s.%s", k.schema, k.name); else snprintf(full, sizeof full, "%s", k.name);
        c->db->drv->quote_ident(full, q, sizeof q); c->db->drv->select_sql(q, NULL, NULL, 200, sql, sizeof sql);
        if (a->active != c) app_set_active(a, c);
        app_run_sql_mode(a, c->db, sql, -1, k.name, RUN_PREVIEW, q);
        return;
    }
    if (id == MI_SIGNATURE) { app_status(a, "%s %s.%s(%s)%s", objkind_name(k.kind), k.schema, k.name, k.signature, k.native_id[0] ? "" : ""); return; }
    if (id == MI_TABLE) { app_status(a, "Trigger %s fires on table %s", k.name, k.table[0] ? k.table : "(see its definition)"); return; }
    /* everything else needs the definition: open it, then act */
    if (!objed_open(a, c, &k)) return;
    ObjKey kk = k; snprintf(kk.conn_id, sizeof kk.conn_id, "%s", c->id);
    ObjDoc *d = doc_find(&kk); if (!d) return;
    int pend = id == MI_CONSOLE ? PEND_CONSOLE : id == MI_VALIDATE ? PEND_VALIDATE : id == MI_COMPARE ? PEND_COMPARE : id == MI_DEPLOY_SQL ? PEND_DEPLOY_SQL :
               id == MI_EXPORT ? PEND_EXPORT : id == MI_COPY_ORIG ? PEND_COPY_ORIG : id == MI_DEPS ? PEND_DEPS : id == MI_PROPS ? PEND_PROPS : id == MI_COMPILE ? PEND_COMPILE :
               id == MI_ERRORS ? PEND_ERRORS : id == MI_ENABLE ? PEND_ENABLE : id == MI_DISABLE ? PEND_DISABLE : id == MI_EVENTS ? PEND_PROPS :
               id == MI_MV_REFRESH ? PEND_MV_REFRESH : PEND_NONE;
    if (id == MI_REFRESH) { if (d->loaded) objed_command(a, OBJCMD_REFRESH); return; }
    if (pend == PEND_NONE) return;
    if (d->loaded && !d->job) do_pending(a, d, pend);
    else d->pending = pend;
}

/* ------------------------------------------------------------------------ */
/* modal dialogs                                                             */
/* ------------------------------------------------------------------------ */

static Rect modal_rect(App *a, float wf, float hf) {
    float w = fminf(a->width * wf, px(a, 1100)), h = fminf(a->height * hf, px(a, 760));
    return rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
}
static void modal_frame(App *a, Rect rc, const char *title) {
    SDL_Renderer *r = a->renderer;
    draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 110));
    draw_rrect(r, rc, px(a, 10), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, 9), color_hex(T_PANEL_ALT));
    draw_text_fit(a->font_ui, r, title, -1, rect_make(rc.x + px(a, 18), rc.y + px(a, 8), rc.w - px(a, 36), px(a, 28)), color_hex(T_TEXT), ALIGN_LEFT);
}
/* lines of text with per-line colours, scrolled by G.mscroll (lines) */
typedef struct { const char *s; int len; uint32_t fg, bg; } MLine;
static void draw_mlines(App *a, Rect rc, const MLine *l, int n) {
    SDL_Renderer *r = a->renderer; Input *in = &a->in;
    draw_rect(r, rc, color_hex(T_EDITOR_BG));
    float rh = font_height(a->font_mono) + px(a, 3);
    int vis = (int)(rc.h / rh);
    if (rect_contains(rc, in->mouse_x, in->mouse_y) && in->wheel_y != 0) G.mscroll -= in->wheel_y / rh;
    if (G.mscroll > n - vis) G.mscroll = (float)(n - vis);
    if (G.mscroll < 0) G.mscroll = 0;
    ui_scrollbar_input_rows(a, &G.msb, rc, (float)n, rh, &G.mscroll);
    clip_push(r, rc);
    int first = (int)G.mscroll;
    for (int i = first; i < n && i - first <= vis; i++) {
        float y = rc.y + (i - first) * rh;
        if (l[i].bg) draw_rect(r, rect_make(rc.x, y, rc.w, rh), color_alpha(color_hex(l[i].bg), 50));
        draw_text_fit(a->font_mono, r, l[i].s, l[i].len, rect_make(rc.x + px(a, 8), y, rc.w - px(a, 16), rh), color_hex(l[i].fg ? l[i].fg : T_TEXT), ALIGN_LEFT);
    }
    clip_pop(r);
    ui_scrollbar_draw_rows(a, &G.msb, rc, (float)n, rh, G.mscroll);
}
/* splits text into MLines (pointers into it) */
static int split_lines(const char *t, MLine *out, int max, uint32_t fg, uint32_t bg) {
    int n = 0; const char *p = t;
    while (*p && n < max) { const char *e = strchr(p, '\n'); int l = e ? (int)(e - p) : (int)strlen(p); out[n++] = (MLine){ p, l, fg, bg }; p = e ? e + 1 : p + l; }
    return n;
}

static void *diff_worker(void *arg) {
    DiffTask *t = arg;
    lines_split(&t->A, t->a, t->al); lines_split(&t->B, t->b, t->bl);
    text_diff_ex(&t->A, &t->B, t->ignore_ws, 20000, &t->d, &t->cancel);
    __atomic_store_n(&t->done, 1, __ATOMIC_SEQ_CST);
    return NULL;
}
static void diff_reset(void) {
    if (G.diff.started) { G.diff.cancel = 1; pthread_join(G.diff.th, NULL); diff_free(&G.diff.d); lines_free(&G.diff.A); lines_free(&G.diff.B); }
    free(G.diff.a); free(G.diff.b);
    memset(&G.diff, 0, sizeof G.diff);
}
static void diff_start(const char *a, const char *b, bool ws) {
    diff_reset();
    G.diff.a = strdup(a); G.diff.b = strdup(b); G.diff.al = (int)strlen(a); G.diff.bl = (int)strlen(b); G.diff.ignore_ws = ws;
    G.diff.started = pthread_create(&G.diff.th, NULL, diff_worker, &G.diff) == 0;
}

/* runs d's reviewed plan (the review's Apply, and each step of a multi-object deployment) */
static void run_plan(App *a, ObjDoc *d) {
    Connection *c = conn_of(a, d->key.conn_id);
    if (!c || !c->db) { app_status(a, "Not connected"); return; }
    char disp[700]; objkey_display(&d->key, disp, sizeof disp);
    snprintf(d->deploy_id, sizeof d->deploy_id, "%lx-%d", (long)time(NULL), ++G.deploy_seq);
    /* OBJ-041: the definition before the change, as a recovery script */
    if (!d->custom_plan && d->base.ddl) {
        char dir[1100]; pref_dir(dir, sizeof dir); strncat(dir, "/backups", sizeof dir - strlen(dir) - 1); mkdir_p(dir);
        char p[1400]; snprintf(p, sizeof p, "%s/%s-%s-%s.sql", dir, d->conn_name, d->key.name, d->deploy_id);
        for (char *q = p + strlen(dir) + 1; *q; q++) if (*q == ' ' || *q == '/') *q = '_';
        const char *bk = d->have_remote && d->remote.ddl ? d->remote.ddl : d->base.ddl;
        if (write_file(p, bk, (int)strlen(bk))) plan_pre(&d->plan, "Recovery script of the previous definition: %s", p);
    }
    char ot[700]; snprintf(ot, sizeof ot, "Deploy %s \xC2\xB7 %s \xC2\xB7 deployment %s", disp, d->plan.strategy, d->deploy_id);
    d->out_exec = output_obj_begin(a->output, a, c, ot, d->plan.nsql);
    for (int i = 0; i < d->plan.npre; i++) output_obj_event(a->output, a, c, d->out_exec, 0, OL_INFO, d->plan.pre[i], NULL);
    output_obj_event(a->output, a, c, d->out_exec, 0, d->plan.risk == RISK_DESTRUCTIVE ? OL_WARNING : OL_INFO, d->plan.risk == RISK_DESTRUCTIVE ? "Risk: destructive (explicit consent given)" : "Risk reviewed", NULL);
    d->t0 = now_s(); d->state = ST_APPLYING;
    snprintf(d->msg, sizeof d->msg, "Applying (%s)...", d->plan.strategy);
    doc_start(a, d, JK_APPLY, objsrc_apply(c->db, d->eng, &d->key, &d->plan, d->plan_fp));
    LOG_I("object editor: deployment %s of %s on %s started (%d statement(s), %s)", d->deploy_id, disp, d->conn_name, d->plan.nsql, d->plan.strategy);
}
static void start_batch_step(App *a) {
    if (G.batch_i >= G.nbatch) { G.batch_running = false; app_status(a, "All %d object(s) applied", G.nbatch); return; }
    ObjDoc *d = G.batch[G.batch_i];
    bool alive = false; for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] == d) alive = true;
    if (!alive) { G.batch_running = false; return; }
    run_plan(a, d);
}

/* the review dialog (spec 2.2, OBJ-034 / 035 / 036 / 040 / 065 / 068) */
static void draw_review(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.86f, 0.86f);
    Connection *c = conn_of(a, d->key.conn_id);
    char title[200]; snprintf(title, sizeof title, d->custom_plan ? "%s: review" : "Apply Object Change", d->plan_label);
    modal_frame(a, rc, title);
    SDL_Renderer *r = a->renderer;
    char disp[700]; objkey_display(&d->key, disp, sizeof disp);
    char tgt[900]; snprintf(tgt, sizeof tgt, "Target: %s%s%s%s / %s / %s", c && c->env ? "[" : "", c && c->env ? conn_env_name(c->env) : "", c && c->env ? "] " : "", d->conn_name, dialect_name(d->dialect), disp);
    draw_text_fit(a->font_small, r, tgt, -1, rect_make(rc.x + px(a, 18), rc.y + px(a, 36), rc.w - px(a, 36), px(a, 20)), color_hex(c && c->env == CONN_ENV_PROD ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    static const char *tabs[] = { "Original vs Changes", "Generated SQL", "Dependencies", "Warnings" };
    int sel = ui_segmented(a, rect_make(rc.x + px(a, 18), rc.y + px(a, 60), px(a, 520), px(a, 28)), tabs, 4, G.rtab);
    if (sel >= 0 && sel != G.rtab) { G.rtab = sel; G.mscroll = 0; }
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 96), rc.w - px(a, 36), rc.h - px(a, 96) - px(a, 150));
    MLine *ls = malloc(sizeof *ls * 4000); int n = 0;
    char *tmp = NULL;
    if (G.rtab == 0) {
        if (d->custom_plan) n = split_lines("This operation does not change the source text: see Generated SQL.", ls, 4000, T_TEXT_MUTED, 0);
        else {
            const char *old = d->have_remote && d->remote.ddl ? d->remote.ddl : d->base.ddl ? d->base.ddl : "";
            int len; char *loc = doc_text(a, d, &len);
            if (!G.diff.started || strcmp(G.diff.a, old) || strcmp(G.diff.b, loc)) diff_start(old, loc, false);
            if (!__atomic_load_n(&G.diff.done, __ATOMIC_SEQ_CST)) { n = split_lines("Comparing...", ls, 4000, T_TEXT_MUTED, 0); a->animating = true; }
            else if (G.diff.d.too_large) n = split_lines("Too different to show line by line (budget reached): the Generated SQL tab has the complete text.", ls, 4000, T_YELLOW, 0);
            else {
                for (int i = 0; i < G.diff.d.nops && n < 4000; i++) {
                    const DiffOp *op = &G.diff.d.ops[i];
                    for (int k = 0; k < op->n && n < 4000; k++) {
                        if (op->kind == DIFF_EQ) ls[n++] = (MLine){ G.diff.A.text + G.diff.A.start[op->a + k], line_len(&G.diff.A, op->a + k), T_TEXT_MUTED, 0 };
                        else if (op->kind == DIFF_DEL) ls[n++] = (MLine){ G.diff.A.text + G.diff.A.start[op->a + k], line_len(&G.diff.A, op->a + k), T_RED, T_RED };
                        else ls[n++] = (MLine){ G.diff.B.text + G.diff.B.start[op->b + k], line_len(&G.diff.B, op->b + k), T_GREEN, T_GREEN };
                    }
                }
            }
            free(loc);
        }
    } else if (G.rtab == 1) {
        tmp = objplan_script(&d->plan, d->dialect);
        n = split_lines(tmp, ls, 4000, T_TEXT, 0);
        ui_record("review:sql", body);
    } else if (G.rtab == 2) {
        size_t cap = 4096 + (size_t)d->base.ndeps * 240; tmp = malloc(cap); int o = 0;
        const ObjSnapshot *s = d->have_remote ? &d->remote : &d->base;
        if (!s->deps_known) o += snprintf(tmp + o, cap - (size_t)o, "This engine does not report dependents (spec 4): check them yourself before publishing.\n");
        else if (!s->ndeps) o += snprintf(tmp + o, cap - (size_t)o, "No dependent objects in the catalog.\n");
        for (int i = 0; i < s->ndeps; i++) o += snprintf(tmp + o, cap - (size_t)o, "%-18s %s   (%s)\n", s->deps[i].kind, s->deps[i].name, s->deps[i].how);
        n = split_lines(tmp, ls, 4000, T_TEXT, 0);
    } else {
        size_t cap = 16384; tmp = malloc(cap); int o = 0;
        if (d->plan.blocked) o += snprintf(tmp + o, cap - (size_t)o, "BLOCKED: %s\n\n", d->plan.block_reason);
        for (int i = 0; i < d->plan.nimpact; i++) o += snprintf(tmp + o, cap - (size_t)o, "Impact: %s\n", d->plan.impact[i]);
        for (int i = 0; i < d->plan.npre; i++) o += snprintf(tmp + o, cap - (size_t)o, "Check: %s\n", d->plan.pre[i]);
        for (int i = 0; i < d->ndiags; i++) if (d->diags[i].severity <= DIAG_WARNING) o += snprintf(tmp + o, cap - (size_t)o, "%s line %d: %s\n", diag_severity_name(d->diags[i].severity), d->diags[i].line, d->diags[i].message);
        n = split_lines(tmp, ls, 4000, T_TEXT, 0);
        for (int i = 0; i < n; i++) if (!strncmp(ls[i].s, "BLOCKED", 7)) ls[i].fg = T_RED; else if (!strncmp(ls[i].s, "error", 5)) ls[i].fg = T_RED; else if (!strncmp(ls[i].s, "warning", 7)) ls[i].fg = T_YELLOW;
    }
    draw_mlines(a, body, ls, n);
    free(ls); free(tmp);
    /* summary line (words, not colour only) */
    int ne = 0, nw = 0; for (int i = 0; i < d->ndiags; i++) { if (d->diags[i].severity == DIAG_ERROR && d->diags[i].confidence == 0) ne++; else if (d->diags[i].severity == DIAG_WARNING) nw++; }
    char sum[600];
    snprintf(sum, sizeof sum, "Strategy: %s  |  Risk: %s  |  Transaction: %s  |  Checks: %d blocking error(s) / %d warning(s)  |  Remote fingerprint: %s",
             d->plan.strategy[0] ? d->plan.strategy : "-", risk_name(d->plan.risk), atomicity_name(d->plan.atomicity), ne, nw,
             d->custom_plan ? "not needed" : d->have_remote ? (d->remote.fingerprint == d->base.fingerprint ? "match" : "CHANGED") : "re-read before running");
    float fy = body.y + body.h + px(a, 8);
    draw_text_fit(a->font_small, r, sum, -1, rect_make(rc.x + px(a, 18), fy, rc.w - px(a, 36), px(a, 20)), color_hex(d->plan.risk == RISK_DESTRUCTIVE ? T_RED : T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("review:summary", rect_make(rc.x + px(a, 18), fy, rc.w - px(a, 36), px(a, 20)));
    if (d->plan.blocked) { draw_text_fit(a->font_small, r, d->plan.block_reason, -1, rect_make(rc.x + px(a, 18), fy + px(a, 20), rc.w - px(a, 36), px(a, 20)), color_hex(T_RED), ALIGN_LEFT); ui_record("review:blocked", rect_make(rc.x + px(a, 18), fy + px(a, 20), rc.w - px(a, 36), px(a, 20))); }
    /* consents */
    unsigned pol = c ? G.policy[c->env < CONN_ENV_COUNT ? c->env : 0] : 0;
    bool need_consent = d->plan.destructive, need_typed = c && c->env == CONN_ENV_PROD, need_reason = pol & POL_REQUIRE_REASON;
    float cy = fy + px(a, 44);
    if (need_consent) {
        char lbl[300]; snprintf(lbl, sizeof lbl, "I reviewed the DROP: %s is dropped and created again (%s)", d->key.name, d->plan.strategy);
        Rect cb = rect_make(rc.x + px(a, 18), cy, rc.w * 0.6f, px(a, 22));
        if (ui_checkbox(a, cb, lbl, G.consent, true)) G.consent = !G.consent;
        ui_record("review:consent", cb);
        cy += px(a, 26);
    }
    float fx = rc.x + px(a, 18);
    if (need_typed) {
        char ph[200]; snprintf(ph, sizeof ph, "type %s to publish to PROD", d->key.name);
        Rect tf = rect_make(fx, cy, px(a, 260), px(a, 26));
        if (ui_textfield(a, &G.confirm, tf, ph, G.focus_field == 1)) G.focus_field = 1;
        ui_record("review:confirm", tf);
        fx += px(a, 270);
    }
    if (need_reason) {
        Rect tf = rect_make(fx, cy, px(a, 300), px(a, 26));
        if (ui_textfield(a, &G.reason, tf, "reason (policy: recorded in the audit log)", G.focus_field == 2)) G.focus_field = 2;
        ui_record("review:reason", tf);
    }
    /* buttons */
    float bh = px(a, 30), by = rc.y + rc.h - bh - px(a, 14);
    Rect cancel = rect_make(rc.x + px(a, 18), by, px(a, 100), bh);
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; diff_reset(); return; }
    ui_record("review:cancel", cancel);
    Rect exp = rect_make(cancel.x + cancel.w + px(a, 10), by, px(a, 120), bh);
    if (ui_button(a, exp, "Export SQL", BTN_NORMAL, d->plan.nsql > 0)) {
        char *s = objplan_script(&d->plan, d->dialect);
        free(G.export_pending); G.export_pending = s;
        static const SDL_DialogFileFilter f[] = { { "SQL", "sql" } };
        SDL_ShowSaveFileDialog(on_export, NULL, a->window, f, 1, NULL);
    }
    ui_record("review:export", exp);
    if (d->plan.destructive_available) {
        Rect dc = rect_make(exp.x + exp.w + px(a, 10), by, px(a, 220), bh);
        if (ui_button(a, dc, "Review drop + create plan", BTN_DANGER, true)) { d->allow_destructive = true; make_plan(a, d); G.consent = false; G.rtab = 1; G.mscroll = 0; }
        ui_record("review:destructive", dc);
    }
    bool typed_ok = !need_typed || !strcmp(G.confirm.buf, d->key.name);
    bool reason_ok = !need_reason || strlen(G.reason.buf) >= 3;
    bool pol_ok = !(pol & POL_DENY_APPLY) && !(d->plan.destructive && (pol & POL_DENY_DESTRUCTIVE)) && !((pol & POL_REQUIRE_VALIDATE) && d->validated != 2 && !d->custom_plan);
    bool can = !d->plan.blocked && ne == 0 && (!need_consent || G.consent) && typed_ok && reason_ok && pol_ok && d->plan.nsql > 0 && c && c->db;
    char lbl[120]; snprintf(lbl, sizeof lbl, "%s to %s", d->custom_plan ? d->plan_label : "Apply", c && c->env ? conn_env_name(c->env) : d->conn_name);
    float aw = font_width(a->font_ui, lbl, -1) + px(a, 30);
    Rect ap = rect_make(rc.x + rc.w - aw - px(a, 18), by, aw, bh);
    if (!pol_ok) draw_text_fit(a->font_small, r, (pol & POL_DENY_APPLY) ? "Policy: publishing is not allowed on this environment." : (pol & POL_REQUIRE_VALIDATE) && d->validated != 2 ? "Policy: validate on the server first." : "Policy: destructive plans are not allowed here.", -1,
                               rect_make(ap.x - px(a, 330), by, px(a, 320), bh), color_hex(T_YELLOW), ALIGN_RIGHT);
    if (ui_button(a, ap, lbl, d->plan.destructive || (c && c->env == CONN_ENV_PROD) ? BTN_DANGER : BTN_PRIMARY, can)) {
        /* confirmed: the reviewed plan, against the fingerprint it was reviewed with */
        G.modal = M_NONE; G.mdoc = NULL; diff_reset();
        run_plan(a, d);
    }
    ui_record("review:apply", ap);
}

/* Diff dialog: original (or the server now) vs the local text, side by side (OBJ-034) */
static void draw_diff(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.9f, 0.86f);
    bool vs_server = d->have_remote;
    char dtitle[160];
    if (G.compare_other) snprintf(dtitle, sizeof dtitle, "Compare: %s on %s vs local (%s)", d->key.name, G.compare_with, d->conn_name);
    else snprintf(dtitle, sizeof dtitle, "%s", vs_server ? "Compare With Database: server now vs local" : "Diff: original vs local");
    modal_frame(a, rc, dtitle);
    SDL_Renderer *r = a->renderer;
    const char *left = vs_server ? (d->remote.ddl ? d->remote.ddl : "") : (d->base.ddl ? d->base.ddl : "");
    int len; char *loc = doc_text(a, d, &len);
    if (!G.diff.started || strcmp(G.diff.a, left) || strcmp(G.diff.b, loc) || G.diff.ignore_ws != G.diff_ws) diff_start(left, loc, G.diff_ws);
    free(loc);
    Rect cb = rect_make(rc.x + px(a, 18), rc.y + px(a, 40), px(a, 300), px(a, 22));
    if (ui_checkbox(a, cb, "Ignore whitespace (view only)", G.diff_ws, true)) G.diff_ws = !G.diff_ws;
    ui_record("diff:ignore-ws", cb);
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 70), rc.w - px(a, 36), rc.h - px(a, 70) - px(a, 60));
    float half = body.w / 2 - px(a, 4);
    Rect L = rect_make(body.x, body.y, half, body.h), R = rect_make(body.x + half + px(a, 8), body.y, half, body.h);
    char sum[200] = "";
    if (!__atomic_load_n(&G.diff.done, __ATOMIC_SEQ_CST)) { draw_text_fit(a->font_ui, r, "Comparing...  (Esc cancels)", -1, body, color_hex(T_TEXT_MUTED), ALIGN_CENTER); a->animating = true; }
    else if (G.diff.d.too_large || G.diff.d.cancelled) draw_text_fit(a->font_ui, r, G.diff.d.cancelled ? "Cancelled." : "Too different to show line by line.", -1, body, color_hex(T_YELLOW), ALIGN_CENTER);
    else {
        /* rows: equal lines side by side; a deletion run next to the insertion run that follows it */
        int cap = G.diff.A.n + G.diff.B.n + 8;
        MLine *l = malloc(sizeof *l * (size_t)cap), *rr = malloc(sizeof *rr * (size_t)cap); int n = 0;
        for (int i = 0; i < G.diff.d.nops; i++) {
            const DiffOp *op = &G.diff.d.ops[i];
            if (op->kind == DIFF_EQ) for (int k = 0; k < op->n; k++, n++) {
                l[n] = (MLine){ G.diff.A.text + G.diff.A.start[op->a + k], line_len(&G.diff.A, op->a + k), T_TEXT_MUTED, 0 };
                rr[n] = (MLine){ G.diff.B.text + G.diff.B.start[op->b + k], line_len(&G.diff.B, op->b + k), T_TEXT_MUTED, 0 };
            } else if (op->kind == DIFF_DEL) {
                const DiffOp *ins = i + 1 < G.diff.d.nops && G.diff.d.ops[i + 1].kind == DIFF_INS ? &G.diff.d.ops[i + 1] : NULL;
                int rows = ins && ins->n > op->n ? ins->n : op->n;
                for (int k = 0; k < rows; k++, n++) {
                    l[n] = k < op->n ? (MLine){ G.diff.A.text + G.diff.A.start[op->a + k], line_len(&G.diff.A, op->a + k), T_RED, T_RED } : (MLine){ "", 0, 0, 0 };
                    rr[n] = ins && k < ins->n ? (MLine){ G.diff.B.text + G.diff.B.start[ins->b + k], line_len(&G.diff.B, ins->b + k), T_GREEN, T_GREEN } : (MLine){ "", 0, 0, 0 };
                }
                if (ins) i++;
            } else for (int k = 0; k < op->n; k++, n++) {
                l[n] = (MLine){ "", 0, 0, 0 };
                rr[n] = (MLine){ G.diff.B.text + G.diff.B.start[op->b + k], line_len(&G.diff.B, op->b + k), T_GREEN, T_GREEN };
            }
        }
        float keep = G.mscroll;
        draw_mlines(a, L, l, n); G.mscroll = keep == G.mscroll ? G.mscroll : G.mscroll;
        draw_mlines(a, R, rr, n);
        free(l); free(rr);
        snprintf(sum, sizeof sum, "%d line(s) added, %d removed%s", G.diff.d.added, G.diff.d.removed, G.diff.d.added + G.diff.d.removed ? "" : ": identical");
    }
    ui_record("diff:body", body);
    draw_text_fit(a->font_small, r, sum, -1, rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 46), rc.w * 0.6f, px(a, 30)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("diff:summary", rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 46), rc.w * 0.6f, px(a, 30)));
    Rect close = rect_make(rc.x + rc.w - px(a, 118), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    if (ui_button(a, close, "Close", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) {
        G.modal = M_NONE; G.mdoc = NULL; diff_reset();
        /* another connection's definition is never the remote of this tab's review */
        if (G.compare_other) { objsnap_free(&d->remote); d->have_remote = false; G.compare_other = false; }
    }
    ui_record("diff:close", close);
}

/* three-way: what the editor opened, the local text, what the server has now (AT-04) */
static void draw_conflict(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.94f, 0.86f);
    modal_frame(a, rc, "The definition changed on the server");
    SDL_Renderer *r = a->renderer;
    draw_text_fit(a->font_small, r, "Nothing was published. Lines changed on both sides are marked CONFLICT. Choose how to continue; publishing still goes through a new review.", -1,
                  rect_make(rc.x + px(a, 18), rc.y + px(a, 36), rc.w - px(a, 36), px(a, 20)), color_hex(T_YELLOW), ALIGN_LEFT);
    int len; char *loc = doc_text(a, d, &len);
    const char *base = d->base.ddl ? d->base.ddl : "", *rem = d->remote.ddl ? d->remote.ddl : "";
    TextLines B, L, R; lines_split(&B, base, (int)strlen(base)); lines_split(&L, loc, len); lines_split(&R, rem, (int)strlen(rem));
    bool *cf = calloc((size_t)(B.n + 1), 1);
    int nconf = B.n <= 20000 ? diff3_conflicts(&B, &L, &R, cf, 20000) : -1;
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 84), rc.w - px(a, 36), rc.h - px(a, 84) - px(a, 64));
    float w3 = body.w / 3 - px(a, 6);
    const char *heads[] = { "When you opened it", "Your version (local)", "Server now" };
    const TextLines *src[] = { &B, &L, &R };
    for (int col = 0; col < 3; col++) {
        Rect cr = rect_make(body.x + col * (w3 + px(a, 9)), body.y, w3, body.h);
        draw_text_fit(a->font_small, r, heads[col], -1, rect_make(cr.x, cr.y - px(a, 22), cr.w, px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        int n = src[col]->n; MLine *ml = malloc(sizeof *ml * (size_t)(n + 1));
        for (int i = 0; i < n; i++) {
            bool conf = col == 0 && i < B.n && cf[i];
            ml[i] = (MLine){ src[col]->text + src[col]->start[i], line_len(src[col], i), conf ? T_RED : T_TEXT, conf ? T_RED : 0 };
        }
        float keep = G.mscroll; draw_mlines(a, cr, ml, n); if (col < 2) G.mscroll = keep;
        free(ml);
    }
    char sum[200]; snprintf(sum, sizeof sum, nconf < 0 ? "Too large for a line-level comparison." : "%d conflicting line(s) in the original", nconf);
    draw_text_fit(a->font_small, r, sum, -1, rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 50), px(a, 300), px(a, 30)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("conflict:summary", rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 50), px(a, 300), px(a, 30)));
    free(cf); lines_free(&B); lines_free(&L); lines_free(&R);
    float by = rc.y + rc.h - px(a, 46), bh = px(a, 30);
    Rect take = rect_make(rc.x + rc.w - px(a, 18) - px(a, 200), by, px(a, 200), bh);
    Rect keep = rect_make(take.x - px(a, 290), by, px(a, 280), bh);
    Rect cancel = rect_make(keep.x - px(a, 110), by, px(a, 100), bh);
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; free(loc); return; }
    ui_record("conflict:cancel", cancel);
    if (ui_button(a, keep, "Keep mine (review against the server)", BTN_PRIMARY, d->remote.exists)) {
        /* explicit decision: the server version becomes the base; the plan is made again and reviewed */
        objsnap_free(&d->base); d->base = d->remote; memset(&d->remote, 0, sizeof d->remote); d->have_remote = false;
        d->state = ST_DIRTY; draft_save(a, d);
        snprintf(d->msg, sizeof d->msg, "Your version is now compared with the server definition; review before publishing.");
        G.modal = M_NONE; G.mdoc = NULL; free(loc);
        begin_apply(a, d);
        return;
    }
    ui_record("conflict:keep", keep);
    if (ui_button(a, take, "Use the server version", BTN_NORMAL, d->remote.exists)) {
        Editor *ed = doc_editor(a, d);
        if (ed) editor_replace_all(ed, rem, (int)strlen(rem));   /* Undo brings your text back */
        objsnap_free(&d->base); d->base = d->remote; memset(&d->remote, 0, sizeof d->remote); d->have_remote = false;
        draft_delete(d); d->state = ST_SYNCED;
        snprintf(d->msg, sizeof d->msg, "The server version replaced your text (Undo brings your text back).");
        G.modal = M_NONE; G.mdoc = NULL;
    }
    ui_record("conflict:take", take);
    free(loc);
}

/* OBJ-028: what the quick fix changes, before it changes it */
static void draw_fix(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.6f, 0.45f);
    char t[200]; snprintf(t, sizeof t, "Quick fix: %s", G.fix.fix_label);
    modal_frame(a, rc, t);
    Editor *ed = doc_editor(a, d);
    int len; char *txt = ed ? editor_text(ed, &len) : strdup(""); if (!ed) len = 0;
    int nl = 0; char *nt = obj_apply_fix(txt, len, &G.fix, &nl);
    /* the line(s) around the fix, before and after */
    int ls = G.fix.fix_start; while (ls > 0 && txt[ls - 1] != '\n') ls--;
    int le = G.fix.fix_end; while (le < len && txt[le] != '\n') le++;
    int ne = le + (nt ? nl - len : 0);
    MLine ml[4]; int n = 0;
    ml[n++] = (MLine){ "Before:", 7, T_TEXT_MUTED, 0 };
    ml[n++] = (MLine){ txt + ls, le - ls, T_RED, T_RED };
    ml[n++] = (MLine){ "After:", 6, T_TEXT_MUTED, 0 };
    if (nt) ml[n++] = (MLine){ nt + ls, ne - ls, T_GREEN, T_GREEN };
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 44), rc.w - px(a, 36), rc.h - px(a, 100));
    draw_mlines(a, body, ml, n);
    ui_record("fix:preview", body);
    Rect cancel = rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    Rect ok = rect_make(rc.x + rc.w - px(a, 138), rc.y + rc.h - px(a, 44), px(a, 120), px(a, 30));
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; }
    else if (ui_button(a, ok, "Apply Fix", BTN_PRIMARY, nt != NULL)) {
        editor_replace_all(ed, nt, nl);   /* one undo step */
        app_status(a, "Quick fix applied: %s (Undo reverts it)", G.fix.fix_label);
        G.modal = M_NONE; G.mdoc = NULL;
    }
    ui_record("fix:cancel", cancel); ui_record("fix:apply", ok);
    free(nt); free(txt);
}

/* OBJ-021: rename a parameter / variable / alias: whole identifiers only, never inside strings,
 * comments or $$-quoted dynamic text, case-insensitive for unquoted names */
static char *rename_text(const char *t, int len, int dialect, const char *from, const char *to, int *count, int *out_len) {
    SqlTok *tk = malloc(sizeof *tk * (size_t)(len + 1));
    int n = sql_lex(t, len, dialect, tk, len + 1);
    size_t cap = (size_t)len + 64; char *o = malloc(cap); size_t ol = 0; *count = 0;
    int fl = (int)strlen(from), tl = (int)strlen(to);
    for (int i = 0; i < n; i++) {
        const char *p = t + tk[i].off; int l = tk[i].len;
        bool hit = tk[i].kind == TK_WORD && l == fl && !strncasecmp(p, from, (size_t)fl) && !(i > 0 && tk[i - 1].kind == TK_DOT);
        /* inside a PL/pgSQL $$ body: rename there too (it is code), recursively */
        char *inner = NULL; int il = 0, ic = 0;
        if (tk[i].kind == TK_BODY && !tk[i].unterminated) {
            int tag = 1; while (tag < l && p[tag] != '$') tag++; tag++;
            if (l >= 2 * tag) { char *r = rename_text(p + tag, l - 2 * tag, dialect, from, to, &ic, &il); inner = r; }
        }
        size_t need = ol + (size_t)(hit ? tl : inner ? il + 64 : l) + 1;
        if (need > cap) { cap = need * 2; o = realloc(o, cap); }
        if (hit) { memcpy(o + ol, to, (size_t)tl); ol += (size_t)tl; (*count)++; }
        else if (inner) {
            int tag = 1; while (tag < l && p[tag] != '$') tag++; tag++;
            memcpy(o + ol, p, (size_t)tag); ol += (size_t)tag; memcpy(o + ol, inner, (size_t)il); ol += (size_t)il; memcpy(o + ol, p + l - tag, (size_t)tag); ol += (size_t)tag;
            *count += ic;
        } else { memcpy(o + ol, p, (size_t)l); ol += (size_t)l; }
        free(inner);
    }
    o[ol] = 0; *out_len = (int)ol; free(tk);
    return o;
}
static void draw_rename(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.45f, 0.32f);
    char t[220]; snprintf(t, sizeof t, "Rename \"%s\"", G.rename_old);
    modal_frame(a, rc, t);
    Rect tf = rect_make(rc.x + px(a, 18), rc.y + px(a, 50), rc.w - px(a, 36), px(a, 28));
    ui_textfield(a, &G.rename, tf, "new name", true);
    ui_record("rename:field", tf);
    Editor *ed = doc_editor(a, d);
    int len; char *txt = ed ? editor_text(ed, &len) : strdup(""); if (!ed) len = 0;
    bool valid = G.rename.len > 0 && (isalpha((unsigned char)G.rename.buf[0]) || G.rename.buf[0] == '_') && !sql_is_keyword(G.rename.buf, G.rename.len);
    for (int i = 0; i < G.rename.len && valid; i++) if (!(isalnum((unsigned char)G.rename.buf[i]) || G.rename.buf[i] == '_')) valid = false;
    int count = 0, nl = 0; char *nt = rename_text(txt, len, d->dialect, G.rename_old, valid ? G.rename.buf : G.rename_old, &count, &nl);
    char info[200]; snprintf(info, sizeof info, valid ? "%d occurrence(s) will change (strings and comments are left alone)" : "Not a valid identifier", count);
    draw_text_fit(a->font_small, a->renderer, info, -1, rect_make(tf.x, tf.y + tf.h + px(a, 8), tf.w, px(a, 20)), color_hex(valid ? T_TEXT_MUTED : T_YELLOW), ALIGN_LEFT);
    ui_record("rename:info", rect_make(tf.x, tf.y + tf.h + px(a, 8), tf.w, px(a, 20)));
    Rect cancel = rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    Rect ok = rect_make(rc.x + rc.w - px(a, 138), rc.y + rc.h - px(a, 44), px(a, 120), px(a, 30));
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; }
    else if (ui_button(a, ok, "Rename", BTN_PRIMARY, valid && count > 0 && strcmp(G.rename.buf, G.rename_old)) || (valid && count > 0 && input_key(&a->in, SDLK_RETURN, 0))) {
        editor_replace_all(ed, nt, nl);
        app_status(a, "Renamed %s to %s in %d place(s) (Undo reverts it)", G.rename_old, G.rename.buf, count);
        G.modal = M_NONE; G.mdoc = NULL;
    }
    ui_record("rename:cancel", cancel); ui_record("rename:apply", ok);
    free(nt); free(txt);
}

/* OBJ-021: a refactoring's result previewed as changed lines, then applied as one undo step */
static void draw_refactor(App *a, ObjDoc *d) {
    Rect rc = modal_rect(a, 0.7f, 0.6f);
    modal_frame(a, rc, G.ref_kind == 1 ? "Extract Subquery to CTE" : G.ref_kind == 2 ? "Use Parameter Names Instead of $n" : "Qualify Columns (from the catalog)");
    Editor *ed = doc_editor(a, d);
    int len; char *t = ed ? editor_text(ed, &len) : strdup(""); if (!ed) len = 0;
    float y = rc.y + px(a, 44);
    if (G.ref_kind == 1) {
        Rect tf = rect_make(rc.x + px(a, 18), y, px(a, 240), px(a, 28));
        ui_textfield(a, &G.ref_name, tf, "CTE name", true);
        ui_record("refactor:name", tf);
        y += px(a, 36);
    }
    char why[200] = ""; int count = 0;
    char *nt = NULL;
    if (G.ref_kind == 1) nt = obj_extract_cte(t, len, d->dialect, G.ref_s0, G.ref_s1, G.ref_name.buf, why, sizeof why);
    else if (G.ref_kind == 2) nt = obj_named_params(t, len, d->dialect, &count, why, sizeof why);
    else {
        /* the columns Lume has in its catalog for this connection */
        Connection *c = conn_of(a, d->key.conn_id);
        int total = 0; for (int i = 0; c && i < c->ntables; i++) if (c->tables[i].cols_loaded) total += c->tables[i].ncols;
        char **cols = malloc(sizeof *cols * (size_t)(total + 1)); int nc = 0;
        for (int i = 0; c && i < c->ntables; i++) if (c->tables[i].cols_loaded)
            for (int k = 0; k < c->tables[i].ncols; k++) { size_t l = strlen(c->tables[i].name) + strlen(c->tables[i].cols[k].name) + 2; cols[nc] = malloc(l); snprintf(cols[nc], l, "%s.%s", c->tables[i].name, c->tables[i].cols[k].name); nc++; }
        nt = obj_qualify_columns(t, len, d->dialect, (const char *const *)cols, nc, &count, why, sizeof why);
        for (int i = 0; i < nc; i++) free(cols[i]);
        free(cols);
    }
    Rect body = rect_make(rc.x + px(a, 18), y, rc.w - px(a, 36), rc.y + rc.h - px(a, 60) - y);
    if (!nt) { MLine ml = { why, (int)strlen(why), T_YELLOW, 0 }; draw_mlines(a, body, &ml, 1); }
    else {
        TextLines A, B; lines_split(&A, t, len); lines_split(&B, nt, (int)strlen(nt));
        Diff df; MLine *ml = malloc(sizeof *ml * (size_t)(A.n + B.n + 2)); int n = 0;
        if (text_diff(&A, &B, false, 4000, &df)) {
            for (int i = 0; i < df.nops; i++) for (int k = 0; k < df.ops[i].n; k++) {
                const DiffOp *op = &df.ops[i];
                if (op->kind == DIFF_DEL) ml[n++] = (MLine){ A.text + A.start[op->a + k], line_len(&A, op->a + k), T_RED, T_RED };
                else if (op->kind == DIFF_INS) ml[n++] = (MLine){ B.text + B.start[op->b + k], line_len(&B, op->b + k), T_GREEN, T_GREEN };
            }
            diff_free(&df);
        }
        draw_mlines(a, body, ml, n); free(ml); lines_free(&A); lines_free(&B);
    }
    ui_record("refactor:preview", body);
    Rect cancel = rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    Rect ok = rect_make(rc.x + rc.w - px(a, 138), rc.y + rc.h - px(a, 44), px(a, 120), px(a, 30));
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; }
    else if (ui_button(a, ok, "Apply", BTN_PRIMARY, nt != NULL)) {
        editor_replace_all(ed, nt, (int)strlen(nt));
        if (G.ref_kind == 1) app_status(a, "Subquery extracted to CTE %s (Undo reverts it)", G.ref_name.buf);
        else if (G.ref_kind == 2) app_status(a, "Replaced %d positional parameter reference(s) by names (Undo reverts it)", count);
        else app_status(a, "Qualified %d column reference(s) (Undo reverts it)", count);
        G.modal = M_NONE; G.mdoc = NULL;
    }
    ui_record("refactor:cancel", cancel); ui_record("refactor:apply", ok);
    free(nt); free(t);
}

/* OBJ-043: the modified objects of one connection, each with its plan; applied in order, stopping
 * at the first failure (no partial claim of success) */
static void draw_batch(App *a, ObjDoc *d0) {
    Rect rc = modal_rect(a, 0.8f, 0.7f);
    Connection *c = conn_of(a, d0->key.conn_id);
    char t[200]; snprintf(t, sizeof t, "Apply %d Object Change(s) on %s", G.nbatch, d0->conn_name);
    modal_frame(a, rc, t);
    char *buf = malloc(65536); int o = 0; bool blocked = false, destructive = false;
    for (int i = 0; i < G.nbatch; i++) {
        ObjDoc *d = G.batch[i];
        char disp[700]; objkey_display(&d->key, disp, sizeof disp);
        o += snprintf(buf + o, 65536 - (size_t)o, "%d. %s\n   %s  |  risk %s  |  %s\n", i + 1, disp, d->plan.strategy, risk_name(d->plan.risk), atomicity_name(d->plan.atomicity));
        if (d->plan.blocked) { blocked = true; o += snprintf(buf + o, 65536 - (size_t)o, "   BLOCKED: %s\n", d->plan.block_reason); }
        if (d->plan.destructive) destructive = true;
        for (int k = 0; k < d->plan.nsql; k++) o += snprintf(buf + o, 65536 - (size_t)o, "   > %.*s\n", (int)strcspn(d->plan.sql[k], "\n"), d->plan.sql[k]);
    }
    o += snprintf(buf + o, 65536 - (size_t)o, "\nOrder: as listed. Each object is published in its own deployment (re-read and compared first); the first failure stops the rest.\n");
    MLine *ls = malloc(sizeof *ls * 2000); int n = split_lines(buf, ls, 2000, T_TEXT, 0);
    for (int i = 0; i < n; i++) if (strstr(ls[i].s, "BLOCKED") && strstr(ls[i].s, "BLOCKED") < ls[i].s + ls[i].len) ls[i].fg = T_RED;
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 44), rc.w - px(a, 36), rc.h - px(a, 140));
    draw_mlines(a, body, ls, n); free(ls); free(buf);
    ui_record("batch:body", body);
    float cy = body.y + body.h + px(a, 10);
    if (destructive) { Rect cb = rect_make(rc.x + px(a, 18), cy, rc.w * 0.6f, px(a, 22)); if (ui_checkbox(a, cb, "I reviewed the DROP + CREATE steps listed above", G.consent, true)) G.consent = !G.consent; ui_record("batch:consent", cb); }
    Rect cancel = rect_make(rc.x + px(a, 18), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    Rect ok = rect_make(rc.x + rc.w - px(a, 178), rc.y + rc.h - px(a, 44), px(a, 160), px(a, 30));
    bool prod = c && c->env == CONN_ENV_PROD;
    bool can = !blocked && (!destructive || G.consent) && !prod && c && c->db;
    if (prod) draw_text_fit(a->font_small, a->renderer, "PROD: publish objects one by one (each needs its typed confirmation)", -1, rect_make(ok.x - px(a, 470), ok.y, px(a, 460), ok.h), color_hex(T_YELLOW), ALIGN_RIGHT);
    if (ui_button(a, cancel, "Cancel", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; }
    else if (ui_button(a, ok, "Apply All in Order", destructive ? BTN_DANGER : BTN_PRIMARY, can)) {
        G.modal = M_NONE; G.mdoc = NULL; G.batch_i = 0; G.batch_running = true;
        start_batch_step(a);
    }
    ui_record("batch:cancel", cancel); ui_record("batch:apply", ok);
}

static void draw_info(App *a) {
    Rect rc = modal_rect(a, 0.6f, 0.7f);
    modal_frame(a, rc, G.info_title);
    MLine *ls = malloc(sizeof *ls * 2000); int n = split_lines(G.info_text ? G.info_text : "", ls, 2000, T_TEXT, 0);
    Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 44), rc.w - px(a, 36), rc.h - px(a, 100));
    draw_mlines(a, body, ls, n); free(ls);
    ui_record("info:body", body);
    Rect close = rect_make(rc.x + rc.w - px(a, 118), rc.y + rc.h - px(a, 44), px(a, 100), px(a, 30));
    if (ui_button(a, close, "Close", BTN_NORMAL, true) || input_key(&a->in, SDLK_ESCAPE, 0)) { G.modal = M_NONE; G.mdoc = NULL; }
    ui_record("info:close", close);
}

/* ------------------------------------------------------------------------ */
/* frame hooks                                                               */
/* ------------------------------------------------------------------------ */

static bool cmd_enabled(const LumeCtx *c, char *why, size_t n) {
    (void)c;
    if (!G.a || !active_doc(G.a)) { snprintf(why, n, "Open a database object first (Explorer > object > Open Definition)"); return false; }
    return true;
}
static void cmd_invoke(void *ud, const LumeCtx *c) { (void)c; if (G.a) objed_command(G.a, (int)(intptr_t)ud); }

/* the connection is closing: every job on its Db stops now (exec_free joins the worker) */
static void on_conn_close(Connection *c) {
    for (int i = 0; i < MAX_DOCS; i++) {
        ObjDoc *d = G.docs[i];
        if (!d || strcmp(d->key.conn_id, c->id) || !d->job) continue;
        bool applying = d->job_kind == JK_APPLY;
        objsrc_cancel(d->job); objsrc_free(d->job); d->job = NULL; d->job_kind = JK_NONE;
        if (applying) { d->state = ST_UNKNOWN; snprintf(d->msg, sizeof d->msg, "Disconnected during the deployment: the outcome is unknown. Reconnect and use Reconcile With Server."); }
        else if (d->state == ST_LOADING) { d->state = ST_LOAD_FAILED; snprintf(d->msg, sizeof d->msg, "Disconnected before the source was loaded."); }
    }
    objed_list_invalidate(c);
}

/* OBJ-017: the active object tab's parameters and variables, and the routines of its schema */
static int complete_extra(void *ud, CompleteExtra *out, int max) {
    (void)ud;
    App *a = G.a; ObjDoc *d = a ? active_doc(a) : NULL;
    if (!d || !d->loaded) return 0;
    Editor *ed = doc_editor(a, d); if (!ed) return 0;
    int len; char *t = editor_text(ed, &len);
    ObjSymbol sy[32]; int ns = obj_symbols(t, len, d->dialect, sy, 32);
    free(t);
    int n = 0;
    for (int i = 0; i < ns && n < max; i++, n++) {
        snprintf(out[n].text, sizeof out[n].text, "%s", sy[i].name);
        snprintf(out[n].detail, sizeof out[n].detail, "%s %s", sy[i].parameter ? "parameter" : "variable", sy[i].type);
    }
    for (int i = 0; i < MAX_LISTS && n < max; i++) if (G.lists[i].used && !strcmp(G.lists[i].conn_id, d->key.conn_id))
        for (int k = 0; k < G.lists[i].n && n < max; k++) {
            const ObjListItem *it = &G.lists[i].items[k];
            if (it->key.kind != OBJK_FUNCTION && it->key.kind != OBJK_PROCEDURE) continue;
            bool dup = false; for (int q = 0; q < n; q++) if (!strcmp(out[q].text, it->key.name)) dup = true;
            if (dup) continue;
            snprintf(out[n].text, sizeof out[n].text, "%s", it->key.name);
            snprintf(out[n].detail, sizeof out[n].detail, "%s(%s)", objkind_word(it->key.kind), it->key.signature);
            n++;
        }
    return n;
}

void objed_init(App *a) {
    G.a = a;
    complete_set_extra(complete_extra, NULL);
    conn_add_close_hook(on_conn_close);
    G.ev_export = SDL_RegisterEvents(1);
    static const struct { const char *id, *label, *syn; int cmd; } cmds[] = {
        { "obj.save_draft", "Object: Save Draft", "draft keep local", OBJCMD_SAVE_DRAFT },
        { "obj.format", "Object: Format Source", "beautify indent", OBJCMD_FORMAT },
        { "obj.validate", "Object: Validate (local)", "check lint syntax", OBJCMD_VALIDATE },
        { "obj.validate_server", "Object: Validate on Server", "check compile describe", OBJCMD_VALIDATE_SERVER },
        { "obj.diff", "Object: Diff Original vs Local", "compare changes", OBJCMD_DIFF },
        { "obj.compare", "Object: Compare With Database", "remote server", OBJCMD_COMPARE },
        { "obj.apply", "Object: Apply Changes\xE2\x80\xA6", "publish deploy migrate", OBJCMD_APPLY },
        { "obj.refresh", "Object: Refresh Definition", "reload", OBJCMD_REFRESH },
        { "obj.next_problem", "Object: Next Problem", "error warning f8", OBJCMD_NEXT_PROBLEM },
        { "obj.prev_problem", "Object: Previous Problem", "error warning", OBJCMD_PREV_PROBLEM },
        { "obj.deploy_sql", "Object: Generate Deployment SQL", "script migration export", OBJCMD_DEPLOY_SQL },
        { "obj.export", "Object: Export Definition", "save file backup", OBJCMD_EXPORT },
        { "obj.goto", "Object: Go to Definition", "f12 navigate", OBJCMD_GOTO_DEF },
        { "obj.dependencies", "Object: Dependencies", "dependents references", OBJCMD_DEPENDENCIES },
        { "obj.properties", "Object: Properties", "owner grants status", OBJCMD_PROPERTIES },
        { "obj.discard", "Object: Discard Draft", "revert", OBJCMD_DISCARD },
        { "obj.compile", "Object: Compile", "oracle recompile", OBJCMD_COMPILE },
        { "obj.reconcile", "Object: Reconcile With Server", "unknown lost connection", OBJCMD_RECONCILE },
        { "obj.rename", "Object: Rename Symbol", "refactor parameter variable", OBJCMD_RENAME },
        { "obj.apply_all", "Object: Review All Changes on This Connection", "batch deploy plan multiple", OBJCMD_APPLY_ALL },
        { "obj.mv_refresh", "Object: Refresh Materialized View Data", "matview refresh", OBJCMD_MV_REFRESH },
        { "obj.extract_cte", "Object: Extract Subquery to CTE", "refactor with common table expression", OBJCMD_EXTRACT_CTE },
        { "obj.named_params", "Object: Use Parameter Names ($n)", "refactor positional dollar", OBJCMD_NAMED_PARAMS },
        { "obj.qualify", "Object: Qualify Columns", "refactor alias prefix", OBJCMD_QUALIFY },
        { "obj.submit_review", "Object: Submit for Review", "approval governance checksum diff artifact", OBJCMD_SUBMIT_REVIEW },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        Command c; memset(&c, 0, sizeof c);
        c.id = cmds[i].id; c.label = cmds[i].label; c.group = "Object"; c.synonyms = cmds[i].syn; c.icon = "code";
        c.enabled = cmd_enabled; c.invoke = cmd_invoke; c.ud = (void *)(intptr_t)cmds[i].cmd; c.module = "lume.objects";
        commands_register(&c);
    }
    load_policy();
}
void objed_shutdown(App *a) {
    conn_remove_close_hook(on_conn_close);   /* the connections close after this: nothing of ours is left to stop */
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i]) { if (G.docs[i]->dirty && !G.docs[i]->draft_saved) draft_save(a, G.docs[i]); doc_free(G.docs[i]); G.docs[i] = NULL; }
    for (int i = 0; i < MAX_LISTS; i++) if (G.lists[i].used) { if (G.lists[i].job) objsrc_free(G.lists[i].job); free(G.lists[i].items); memset(&G.lists[i], 0, sizeof G.lists[i]); }
    diff_reset(); free(G.info_text); free(G.export_pending);
    G.info_text = NULL; G.export_pending = NULL;
}

bool objed_modal_open(void) { return G.modal != M_NONE; }

void objed_pre_update(App *a) {
    G.a = a;
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i]) poll_doc(a, G.docs[i]);
    poll_lists(a);
    if (G.menu.open) { int id = popup_update(a, &G.menu); if (id >= 0) { popup_close(&G.menu); run_menu(a, id); } }
    if (G.drop.open) { int id = popup_update(a, &G.drop); if (id >= 0) { popup_close(&G.drop); ObjDoc *d = G.drop_doc; G.drop_doc = NULL; for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] == d && d) run_drop(a, d, id); } }
    /* a modal owns this frame's input: keep it for the dialog, hide it from everything below */
    if (G.modal != M_NONE) {
        G.stash = a->in; G.stashed = true;
        a->in.mouse_pressed = a->in.mouse_released = a->in.mouse_rpressed = false; a->in.mouse_consumed = true;
        a->in.nkeys = 0; a->in.ntext = 0; a->in.wheel_x = a->in.wheel_y = 0;
    }
}

void objed_draw_overlay(App *a) {
    if (G.stashed) {
        bool down = a->in.mouse_down; float mx = a->in.mouse_x, my = a->in.mouse_y;
        a->in = G.stash; a->in.mouse_down = down; a->in.mouse_x = mx; a->in.mouse_y = my;
        G.stashed = false;
    }
    ObjDoc *d = G.mdoc;
    bool alive = false; for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] == d && d) alive = true;
    if (G.modal != M_NONE && !alive) G.modal = M_NONE;
    switch (G.modal) {
    case M_REVIEW: draw_review(a, d); break;
    case M_DIFF: draw_diff(a, d); break;
    case M_CONFLICT: draw_conflict(a, d); break;
    case M_INFO: draw_info(a); break;
    case M_FIX: draw_fix(a, d); break;
    case M_RENAME: draw_rename(a, d); break;
    case M_BATCH: draw_batch(a, d); break;
    case M_REFACTOR: draw_refactor(a, d); break;
    }
    if (G.modal != M_NONE) { a->in.mouse_consumed = true; a->in.nkeys = 0; a->in.ntext = 0; }
    if (G.menu.open) popup_draw(a, &G.menu);
    if (G.drop.open) popup_draw(a, &G.drop);
}

bool objed_menu_pos(App *a, const char *label, float *x, float *y) {
    Rect rc;
    if ((G.menu.open && popup_item_rect(a, &G.menu, label, &rc)) || (G.drop.open && popup_item_rect(a, &G.drop, label, &rc))) {
        *x = (rc.x + rc.w / 2) / a->density; *y = (rc.y + rc.h / 2) / a->density;
        return true;
    }
    return false;
}

/* harness (objawait): the active object tab finished loading, or said why it could not */
bool objed_idle(App *a) {
    ObjDoc *d = active_doc(a);
    return !d || (!d->job && !d->remote_pending && (d->loaded || strstr(state_text(d), "failed") || strstr(state_text(d), "Failed")));
}
void objed_dump(App *a) {
    ObjDoc *d = active_doc(a);
    if (!d) { printf("objstate none\n"); fflush(stdout); return; }
    char disp[700]; objkey_display(&d->key, disp, sizeof disp);
    printf("objstate key=\"%s\" state=%s dirty=%d draft=%d loaded=%d read_only=%d validated=%d problems=%d server_problems=%d status=%s modal=%d msg=\"%s\"\n",
           disp, state_text(d), d->dirty, d->draft_saved, d->loaded, d->read_only, d->validated, d->ndiags, d->nserver, d->base.status[0] ? d->base.status : "-", G.modal, d->msg);
    for (int i = 0; i < d->ndiags; i++) printf("objproblem %s %s line %d: %s%s\n", diag_severity_name(d->diags[i].severity), d->diags[i].code, d->diags[i].line, d->diags[i].message, d->diags[i].has_fix ? " [fix]" : "");
    for (int i = 0; i < d->nserver; i++) printf("objproblem server %s line %d: %s\n", d->server_diags[i].code, d->server_diags[i].line, d->server_diags[i].message);
    if (G.last_hint[0]) printf("objhint %s\n", G.last_hint);
    { CompleteExtra ex[32]; int n = complete_extra(NULL, ex, 32); printf("objcomplete"); for (int i = 0; i < n; i++) printf(" %s=%s;", ex[i].text, ex[i].detail); printf("\n"); }
    if (d->have_plan) {
        printf("objplan strategy=\"%s\" risk=%s atomicity=\"%s\" blocked=%d destructive=%d destructive_available=%d conflict=%d reason=\"%s\"\n", d->plan.strategy, risk_name(d->plan.risk),
               atomicity_name(d->plan.atomicity), d->plan.blocked, d->plan.destructive, d->plan.destructive_available, d->plan.conflict, d->plan.block_reason);
        for (int i = 0; i < d->plan.nsql; i++) { printf("objsql %d: ", i + 1); for (const char *p = d->plan.sql[i]; *p; p++) putchar(*p == '\n' ? ' ' : *p); putchar('\n'); }
    }
    fflush(stdout);
}
