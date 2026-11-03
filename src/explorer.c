/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Explorer panel: see explorer.h.
 *
 * Data model: a tree of Node, rebuilt from the ConnStore by explorer_rebuild after any
 * connection change. Nodes borrow their Connection and DbTable pointers from the store
 * (they never free them), so the tree must be rebuilt whenever a connection's table list is
 * replaced. `vis` is the flattened list of visible rows, recomputed every frame and after
 * any action that may have rebuilt the tree (a node pointer must never outlive a rebuild).
 *
 * Schema Explorer v2 (DBX-077..126): connections whose driver has a namespace adapter (nsx.h)
 * get one "Schemas" (MySQL: "Databases") node holding the highlighted schemas and, last,
 * "Others". The model lives in nsx.c; this file draws it, runs its catalog reads one at a time
 * per connection on the executor's worker (never on the SDL thread, never in draw), and keeps
 * selection / expansion / scroll by stable keys across rebuilds. SQLite keeps its native tree.
 *
 * Folders (NODE_FOLDER): a connection's `folder` ("a/b") puts it under nested folder rows; folders
 * come first at each level (by name), then connections in the store's order. A folder's path is in
 * its `sname`; which folders are open is saved in <prefs>/explorer-folders.txt. Folders exist only
 * through their connections: renaming or removing one rewrites those connections' `folder`.
 *
 * Columns are loaded lazily the first time a table is expanded (conn_load_columns), which
 * calls the driver synchronously on the UI thread (refused while a worker owns the session).
 *
 * Actions go back to the app through app_* calls (app_connect, app_set_active,
 * app_run_sql_mode for previews, app_edit_connection for the dialog).
 */
#include "explorer.h"
#include <ctype.h>
#include <stdarg.h>
#include <strings.h>
#include "caps.h"
#include "complete.h"
#include "conn.h"
#include "db.h"
#include "draw.h"
#include "exec.h"
#include "font.h"
#include "log.h"
#include "nsx.h"
#include "objed.h"
#include "panels.h"
#include "theme.h"
#include "tdes.h"
#include "dgmview.h"
#include "dialog.h"
#include "ui.h"

/* Row kinds: a connection root, a group (Tables / Views / a schema, also used for message
 * rows), a table or view, a column, an object, a namespace (schema / database), an action row. */
typedef enum { NODE_CONN, NODE_GROUP, NODE_TABLE, NODE_COLUMN, NODE_OBJECT, NODE_SCHEMA, NODE_ACTION, NODE_FOLDER } NodeKind;
/* Schema Explorer roles of a group */
enum { NR_NONE, NR_ROOT, NR_OTHERS };
/* action rows */
enum { ACT_NONE, ACT_SEARCH_OTHERS, ACT_LOAD_MORE, ACT_RETRY_PAGE, ACT_RETRY_TABLES, ACT_RETRY_SCOPE, ACT_CANCEL_TABLES };

/* One row of the tree. Owns its label, detail and children. */
typedef struct Node {
    NodeKind kind;
    char *label, *detail;   /* text on the left; dimmed text on the right (driver, count, column type) */
    bool expanded, loaded, leaf, pk;   /* open; columns fetched; no chevron; column is part of the primary key */
    struct Node **kids; int nkids, cap;   /* children, owned */
    struct Node *parent;
    int depth;   /* indent level, 0 for roots */
    DbTable *table;   /* borrowed from Connection.tables or the Explorer's schema cache (NODE_TABLE only) */
    Connection *conn;   /* owning connection, inherited from the parent */
    bool objgroup;      /* the "Objects" group of a schema: filled from the object editor's list on expand */
    char *schema;       /* objgroup: the schema it lists ("" = the connection's current one) */
    ObjKey *okey;       /* NODE_OBJECT: the object's identity (owned) */
    int role, action;   /* NR_* of a group; ACT_* of an action row */
    int order;          /* NODE_CONN under folders: the connection's position in the store (sorting) */
    char *sname;        /* NODE_SCHEMA / action rows: the namespace */
    bool in_others;     /* NODE_SCHEMA listed under Others */
} Node;

/* Context-menu commands. */
enum {
    CM_CONNECT, CM_DISCONNECT, CM_EDIT, CM_REFRESH, CM_NEW, CM_SEP, CM_INFO,
    CM_NS_SEARCH, CM_NS_REFRESH, CM_NS_OPTIONS, CM_NS_SHOWALL, CM_NS_COMPACT, CM_NS_COPY, CM_NS_PROPS,
    CM_OT_SEARCH, CM_OT_MORE, CM_OT_REFRESH,
    CM_SC_CONSOLE, CM_SC_EXPAND, CM_SC_FIND, CM_SC_REFRESH, CM_SC_PROPS, CM_SC_COPY, CM_SC_VERIFY, CM_SC_PIN, CM_SC_UNPIN,
    CM_OPT_COMPACT, CM_OPT_ALL, CM_OPT_PINNED, CM_OPT_SYSTEM, CM_OPT_RECENT, CM_OPT_KNOWN, CM_OPT_CAP, CM_OPT_PAGE, CM_OPT_KINDS, CM_OPT_DISCOVER, CM_OPT_RESET,
    CM_HIT_OPEN, CM_HIT_REVEAL, CM_HIT_PIN, CM_HIT_COPY, CM_HIT_PROPS,
    CM_FD_NEW, CM_FD_RENAME, CM_FD_REMOVE, CM_FD_COLLAPSE, CM_FD_EXPAND,
    CM_NEW_TABLE, CM_NEW_QUERY, CM_DUP_CONN, CM_TB_ERD, CM_TB_ERD_ADD, CM_SC_ERD, CM_TB_MODIFY, CM_TB_DATA, CM_TB_DDL, CM_TB_DUP, CM_TB_DEPS, CM_TB_DROP,
};
#define CM_MAX 28

/* Panel state. */
struct Explorer {
    Node **roots; int nroots;   /* one per connection, same order as a->conns */
    Node *selected;
    Node *msel[512]; int nmsel;   /* tables picked with Cmd / Shift + click (ER diagram of several tables) */
    Node *anchor;                 /* the last plainly clicked row: Shift + click selects from it */
    Node **vis; int nvis, capvis;   /* flattened visible rows (pointers into the tree) */
    float scroll, scroll_t;   /* current and target scroll offset, pixels */
    ScrollBar sb;             /* the overlay scrollbar */
    Rect rows_rect, plus_rect;   /* last frame's rows area and "+" button */
    /* context menu (also the Properties panel: info lines only) */
    bool cm_open;
    Connection *cm_conn;
    float cm_x, cm_y;
    int cm_cmd[CM_MAX], cm_n, cm_sel;
    char cm_text[CM_MAX][160];
    signed char cm_check[CM_MAX];   /* -1 none, 0 / 1 */
    bool cm_dis[CM_MAX];
    char cm_schema[128];            /* target namespace (schema menus, search hits, tables) */
    char cm_table[256];             /* target table (table menus) */
    int cm_hit;                     /* search hit index for CM_HIT_* */
    /* search bar (DBX-085 / 087) */
    bool search_open, search_sub, search_objects;
    Connection *search_conn;
    char search_ns[128], search_last[256];
    TextField sf;
    int search_sel;
    float search_scroll;
    Rect search_rows;
    uint64_t ns_sig;                /* sum of the models' revisions the tree was built from */
    char sel_key[400];              /* selection by stable key across rebuilds */
    char cm_folder[128];            /* target folder path (folder menus) */
    /* inline rename of a folder row */
    bool rn_open; char rn_path[128]; TextField rn;
};

/* Allocates a node (copying label and detail) and appends it to parent's children, from
 * which it inherits the connection and depth. */
static Node *node_new(NodeKind kind, const char *label, const char *detail, Node *parent) {
    Node *n = calloc(1, sizeof *n);
    n->kind = kind;
    n->label = SDL_strdup(label);
    n->detail = detail ? SDL_strdup(detail) : NULL;
    n->parent = parent;
    n->depth = parent ? parent->depth + 1 : 0;
    if (parent) {
        if (parent->nkids == parent->cap) {
            parent->cap = parent->cap ? parent->cap * 2 : 8;
            parent->kids = realloc(parent->kids, sizeof *parent->kids * parent->cap);
        }
        parent->kids[parent->nkids++] = n;
        n->conn = parent->conn;
    }
    return n;
}

/* Frees a node and its whole subtree. */
static void node_free(Node *n) {
    if (!n) return;
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    free(n->kids);
    free(n->label);
    free(n->detail);
    free(n->schema);
    free(n->okey);
    free(n->sname);
    free(n);
}

/* Frees the children of n, keeping n and its kids array. */
static void node_clear_kids(Node *n) {
    for (int i = 0; i < n->nkids; i++) node_free(n->kids[i]);
    n->nkids = 0;
}

/* ---- stable keys: expansion and selection survive rebuilds (DBX-077.5 / 088.3 / 113) ---- */

/* the label part of a key: Others keeps one key whatever its count says */
static const char *key_label(const Node *n) {
    if (n->role == NR_OTHERS) return "\x01others";
    if (n->kind == NODE_SCHEMA && n->sname) return n->sname;
    return n->label ? n->label : "";
}
/* "conn id|kind:label|kind:label..." from the root down (a schema keeps its key in or out of Others) */
static void node_key(const Node *n, char *out, int len) {
    const Node *path[32]; int d = 0;
    for (const Node *p = n; p && d < 32; p = p->parent) path[d++] = p;
    int o = snprintf(out, (size_t)len, "%s", n->conn ? n->conn->id : "");
    for (int i = d - 1; i >= 0 && o < len; i--) {
        const Node *p = path[i];
        if (p->kind == NODE_GROUP && p->role == NR_OTHERS && i > 0 && path[i - 1]->kind == NODE_SCHEMA) continue;   /* Others is not part of a schema's identity */
        o += snprintf(out + o, (size_t)(len - o), "|%d:%s", (int)p->kind, key_label(p));
    }
}
#define XS_MAX 512
static struct { char key[400]; bool open; } xs[XS_MAX]; static int nxs;
static int xs_find(const char *k) { for (int i = 0; i < nxs; i++) if (!strcmp(xs[i].key, k)) return i; return -1; }
/* the user's open / closed choice for a node, else `def` */
static bool xs_get(const Node *n, bool def) {
    char k[400]; node_key(n, k, sizeof k);
    int i = xs_find(k);
    return i >= 0 ? xs[i].open : def;
}
static void xs_set(const Node *n, bool open) {
    char k[400]; node_key(n, k, sizeof k);
    int i = xs_find(k);
    if (i < 0) { if (nxs == XS_MAX) { memmove(xs, xs + 1, sizeof xs[0] * (XS_MAX - 1)); nxs--; } i = nxs++; snprintf(xs[i].key, sizeof xs[i].key, "%s", k); }
    xs[i].open = open;
}

/* ---- folders: which are open, saved in <prefs>/explorer-folders.txt (one path per line) ---- */
#define FO_MAX 512
static char fo[FO_MAX][128]; static int nfo; static bool fo_loaded; static char fo_file[1100];
static void fo_load(App *a) {
    if (fo_loaded || !a->conns) return;
    fo_loaded = true;
    if (!a->conns->dir[0]) return;
    snprintf(fo_file, sizeof fo_file, "%sexplorer-folders.txt", a->conns->dir);
    FILE *f = fopen(fo_file, "r"); if (!f) return;
    char line[160];
    while (nfo < FO_MAX && fgets(line, sizeof line, f)) { line[strcspn(line, "\r\n")] = 0; if (line[0]) snprintf(fo[nfo++], sizeof fo[0], "%s", line); }
    fclose(f);
}
static void fo_save(void) {
    if (!fo_file[0]) return;
    FILE *f = fopen(fo_file, "w"); if (!f) return;
    for (int i = 0; i < nfo; i++) fprintf(f, "%s\n", fo[i]);
    fclose(f);
}
static bool fo_is_open(const char *path) { for (int i = 0; i < nfo; i++) if (!strcmp(fo[i], path)) return true; return false; }
static void fo_set(const char *path, bool open) {
    for (int i = 0; i < nfo; i++) if (!strcmp(fo[i], path)) { if (!open) { memmove(fo[i], fo[i + 1], sizeof fo[0] * (size_t)(nfo - i - 1)); nfo--; fo_save(); } return; }
    if (open && nfo < FO_MAX) { snprintf(fo[nfo++], sizeof fo[0], "%s", path); fo_save(); }
}
/* path is `folder` itself or inside it */
static bool in_folder(const char *path, const char *folder) {
    size_t n = strlen(folder);
    return !strncmp(path, folder, n) && (path[n] == 0 || path[n] == '/');
}
/* Replaces the `from` prefix of every saved folder path (connections and open state) with `to`
 * ("" = the top level, i.e. the folder is removed and its content moves up). */
static void fo_move(App *a, const char *from, const char *to) {
    size_t n = strlen(from);
    for (int i = 0; a->conns && i < a->conns->n; i++) {
        Connection *c = a->conns->items[i];
        if (!in_folder(c->folder, from)) continue;
        char nf[256]; snprintf(nf, sizeof nf, "%s%s", to, c->folder + n);   /* the rest keeps its "/" (normalize drops a leading one) */
        conn_folder_normalize(nf, sizeof nf);
        snprintf(c->folder, sizeof c->folder, "%s", nf);
    }
    for (int i = 0; i < nfo; i++) {
        if (!in_folder(fo[i], from)) continue;
        char nf[256]; snprintf(nf, sizeof nf, "%s%s", to, fo[i] + n);
        conn_folder_normalize(nf, sizeof nf);
        if (nf[0]) snprintf(fo[i], sizeof fo[0], "%s", nf); else { memmove(fo[i], fo[i + 1], sizeof fo[0] * (size_t)(nfo - i - 1)); nfo--; i--; }
    }
    fo_save();
}

/* See explorer.h. */
Explorer *explorer_create(void) { return calloc(1, sizeof(Explorer)); }

/* Frees every root (and so the whole tree) and clears the selection. */
static void free_roots(Explorer *e) {
    for (int i = 0; i < e->nroots; i++) node_free(e->roots[i]);
    free(e->roots);
    e->roots = NULL; e->nroots = 0;
    e->selected = NULL; e->nmsel = 0; e->anchor = NULL;
}

/* The App, for the object lists (the tree functions below have no App parameter). */
static App *E_APP;

/* ---- catalog reads of the Schema Explorer: one statement at a time per connection ---- */

typedef struct NsRun { char id[32]; unsigned gen_seen; ExecJob *job; bool cancel, timed_out; int kind; } NsRun;
static NsRun runs[64]; static int nruns;
static NsRun *run_slot(const char *id) {
    for (int i = 0; i < nruns; i++) if (!strcmp(runs[i].id, id)) return &runs[i];
    if (nruns == 64) return NULL;
    NsRun *r = &runs[nruns++]; memset(r, 0, sizeof *r); snprintf(r->id, sizeof r->id, "%s", id);
    return r;
}
/* true while the Explorer's own catalog read holds this session (a long table listing) */
bool explorer_reading(App *a, Db *db) {
    for (int i = 0; db && a->conns && i < a->conns->n; i++) {
        Connection *c = a->conns->items[i];
        if (c->db != db) continue;
        for (int k = 0; k < nruns; k++) if (!strcmp(runs[k].id, c->id) && runs[k].job && runs[k].kind == NSR_TABLES) return true;
    }
    return false;
}
static void run_stop(NsRun *r) {
    if (!r || !r->job) return;
    exec_cancel(r->job);
    exec_free(r->job);   /* joins the worker: the Db is free again */
    r->job = NULL; r->cancel = r->timed_out = false;
}

/* tables of schemas outside the connection's own list (DBX-090), by connection and schema */
typedef struct ExtTables { char id[32], schema[128]; uint32_t serial; DbTable *t; int n; } ExtTables;
static ExtTables *ext; static int next_n;
static void ext_free_conn(const char *id) {
    for (int i = 0; i < next_n; ) {
        if (!id || !strcmp(ext[i].id, id)) { db_tables_free(ext[i].t, ext[i].n); ext[i] = ext[--next_n]; } else i++;
    }
}
static ExtTables *ext_get(const char *id, const NsInfo *in) {
    for (int i = 0; i < next_n; i++) if (!strcmp(ext[i].id, id) && !strcmp(ext[i].schema, in->name)) {
        if (ext[i].serial == in->tables_serial) return &ext[i];
        db_tables_free(ext[i].t, ext[i].n); ext[i] = ext[--next_n]; break;
    }
    ext = realloc(ext, sizeof *ext * (size_t)(next_n + 1));
    ExtTables *x = &ext[next_n++];
    memset(x, 0, sizeof *x);
    snprintf(x->id, sizeof x->id, "%s", id); snprintf(x->schema, sizeof x->schema, "%s", in->name); x->serial = in->tables_serial;
    x->n = in->ntables; x->t = calloc((size_t)(x->n ? x->n : 1), sizeof *x->t);
    for (int i = 0; i < x->n; i++) { x->t[i].name = db_strdup(in->tables[i].name); x->t[i].is_view = in->tables[i].is_view; }
    return x;
}

static void ns_on_close(Connection *c) {
    NsRun *r = NULL;
    for (int i = 0; i < nruns; i++) if (!strcmp(runs[i].id, c->id)) r = &runs[i];
    if (r) { run_stop(r); r->gen_seen = 0; }
    nsx_disconnected(c->id);
    ext_free_conn(c->id);
}

/* See explorer.h. */
void explorer_destroy(Explorer *e) {
    for (int i = 0; i < nruns; i++) run_stop(&runs[i]);
    conn_remove_close_hook(ns_on_close);
    free_roots(e);
    free(e->vis);
    ext_free_conn(NULL); free(ext); ext = NULL;
    nsx_shutdown();
    free(e);
}

/* The schema whose tables are in the connection's own list when that list covers one schema
 * only (Oracle: the login user's tables; MySQL: DATABASE()). */
static const char *home_schema(Connection *c, NsState *s) {
    if (s->ad->caps.conn_tables_all) return NULL;
    if (!strcmp(c->driver, "oracle")) return s->user[0] ? s->user : NULL;
    return s->current[0] ? s->current : NULL;
}
/* schema of an entry of the connection's table list (PostgreSQL: bare = public) */
static void table_schema(Connection *c, const char *name, char *out, int n) {
    const char *dot = strchr(name, '.');
    if (dot) snprintf(out, (size_t)n, "%.*s", (int)(dot - name), name);
    else snprintf(out, (size_t)n, "%s", !strcmp(c->driver, "postgres") ? "public" : "");
}

/* A connection opened or closed: start or drop its model; schemas known from the table list. */
static void ns_sync(App *a, Connection *c) {
    if (!nsx_adapter(c->driver)) return;
    NsState *s = nsx_get(c->id, c->driver);
    NsRun *r = run_slot(c->id);
    if (!s || !r) return;
    if (!c->db) {
        if (r->gen_seen) { run_stop(r); nsx_disconnected(c->id); ext_free_conn(c->id); r->gen_seen = 0; }
        return;
    }
    if (r->gen_seen != c->generation) {
        run_stop(r); ext_free_conn(c->id);
        nsx_connected(s, c->schema, (double)SDL_GetTicks());
        r->gen_seen = c->generation;
    }
    /* owners already known to have visible tables: no extra query (DBX-086) */
    const char *names[256]; int k = 0; char buf[256][128];
    if (s->ad->caps.conn_tables_all) {
        for (int i = 0; i < c->ntables && k < 256; i++) {
            char sc[128]; table_schema(c, c->tables[i].name, sc, sizeof sc);
            bool seen = false; for (int j = 0; j < k && !seen; j++) seen = !strcmp(buf[j], sc);
            if (!seen && sc[0]) { snprintf(buf[k], sizeof buf[k], "%s", sc); names[k] = buf[k]; k++; }
        }
    } else if (c->ntables) {
        const char *h = home_schema(c, s);
        if (h) { snprintf(buf[0], sizeof buf[0], "%s", h); names[k++] = buf[0]; }
    }
    nsx_note_known(s, names, k);
    (void)a;
}

/* Runs the models' catalog reads: at most one statement per connection, on the executor's
 * worker; cancelled at its time budget or when a newer request made it obsolete (DBX-115 / 116
 * / 119). Nothing runs in draw; while idle nothing ticks (DBX-125.4). */
static void ns_pump(App *a) {
    uint64_t now = SDL_GetTicks();
    for (int i = 0; a->conns && i < a->conns->n; i++) {
        Connection *c = a->conns->items[i];
        NsState *s = c->db ? nsx_find(c->id) : NULL;
        NsRun *r = s ? run_slot(c->id) : NULL;
        if (!s || !r || r->gen_seen != c->generation) continue;
        if (r->job) {
            if (exec_running(r->job)) {
                double el = exec_elapsed_ms(r->job);
                if (!r->cancel && (el > s->running.timeout_ms || nsx_running_stale(s))) { r->timed_out = el > s->running.timeout_ms; exec_cancel(r->job); r->cancel = true; }
                if (s->running.kind == NSR_TABLES) {   /* the schema's "Loading... N s" follows the clock */
                    NsInfo *in = nsx_info(s, s->running.key, false);
                    if (in && in->tables_secs != (int)(el / 1000)) { in->tables_secs = (int)(el / 1000); s->rev++; }
                }
                app_wake_in(a, 50);
                continue;
            }
            ResultSet rs; rs_init(&rs);
            bool got = exec_take_result(r->job, 0, &rs);
            bool ok = got && rs.ok && !rs.error[0] && !rs.cancelled;
            int pages = s->m.pages;
            double ms = exec_elapsed_ms(r->job);
            int budget = s->running.timeout_ms;
            nsx_complete(s, ok ? &rs : NULL, ok ? NULL : got && rs.error[0] ? rs.error : "the statement did not run", r->timed_out, r->cancel && !r->timed_out, ms, now);
            if (s->m.pages != pages) LOG_I("explorer: %s page %d: %d names loaded in total, %.0f ms", s->ad->label, s->pager.pages, s->pager.n, ms);   /* counts, no names (DBX-114) */
            if (r->timed_out) LOG_I("explorer: catalog read stopped at its budget (%d ms)", budget);
            rs_free(&rs); exec_free(r->job); r->job = NULL; r->cancel = r->timed_out = false;
        }
        if (c->db->busy) { if (nsx_busy(s)) app_wake_in(a, 200); continue; }   /* the editor owns the session: wait */
        const NsReq *q = nsx_next_request(s, now);
        if (!q) { uint64_t due = nsx_next_due(s); if (due) app_wake_in(a, due > now ? (Uint32)(due - now) : 0); continue; }
        if (caps_sql_may_write(q->sql, -1)) { nsx_complete(s, NULL, "refused: a catalog statement must be a plain read", false, false, 0, now); continue; }   /* OBJ-080 style guard */
        const char *sql = q->sql;
        /* tests only (under the harness): LUME_TEST_SLOW_TABLES_S makes an Oracle table listing a
         * really slow statement (DBMS_SESSION.SLEEP), so a suite can watch "Loading... N s" and Cancel it */
        char slow[17000]; const char *ss = getenv("LUME_TEST_SLOW_TABLES_S");
        if (q->kind == NSR_TABLES && ss && atoi(ss) > 0 && getenv("LUME_SCRIPT") && !strcmp(c->driver, "oracle")) {
            snprintf(slow, sizeof slow, "WITH FUNCTION lume_slow RETURN NUMBER IS BEGIN DBMS_SESSION.SLEEP(%d); RETURN 1; END; "
                     "SELECT q.* FROM (%s) q WHERE lume_slow() = 1", atoi(ss), q->sql); sql = slow;
        }
        int fetch = q->limit + 2 > 10 ? q->limit + 2 : 10;
        r->job = exec_start_list(c->db, &sql, 1, fetch, true, false);
        r->kind = q->kind;
        if (!r->job) nsx_complete(s, NULL, "the connection is busy", false, false, 0, now);
        app_wake_in(a, 30);
    }
}

/* "Objects" groups the user expanded, by "connection id|schema": the tree is rebuilt often and
 * this state must survive it */
static char obj_open[32][160]; static int nobj_open;
static bool objgroup_is_open(Connection *c, const char *schema) {
    char k[160]; snprintf(k, sizeof k, "%s|%s", c->id, schema);
    for (int i = 0; i < nobj_open; i++) if (!strcmp(obj_open[i], k)) return true;
    return false;
}
static void objgroup_set_open(Connection *c, const char *schema, bool open) {
    char k[160]; snprintf(k, sizeof k, "%s|%s", c->id, schema);
    for (int i = 0; i < nobj_open; i++) if (!strcmp(obj_open[i], k)) { if (!open) { obj_open[i][0] = 0; snprintf(obj_open[i], sizeof obj_open[i], "%s", obj_open[--nobj_open]); } return; }
    if (open && nobj_open < 32) snprintf(obj_open[nobj_open++], sizeof obj_open[0], "%s", k);
}
/* Fills an "Objects" group from the object editor's cached list (one subgroup per kind). Views
 * are not repeated: they are already in the tree, and their menu opens the definition. */
static void build_objects(Node *grp) {
    Connection *c = grp->conn;
    node_clear_kids(grp);
    ObjListView v = E_APP ? objed_list(E_APP, c, grp->schema, grp->expanded) : (ObjListView){ 0 };
    if (v.loading || (!v.loaded && grp->expanded)) { node_new(NODE_GROUP, "Loading objects\xE2\x80\xA6", NULL, grp)->leaf = true; return; }
    if (v.error) { node_new(NODE_GROUP, v.error, NULL, grp)->leaf = true; return; }
    if (!v.loaded) return;
    static const char *names[OBJK_COUNT] = { "Views", "Materialized views", "Functions", "Procedures", "Packages", "Package bodies", "Triggers", "Types", "Type bodies",
                                                "Operators", "Event triggers", "Virtual tables" };
    int count[OBJK_COUNT] = { 0 };
    for (int i = 0; i < v.n; i++) if (v.items[i].key.kind > OBJK_VIEW && v.items[i].key.kind < OBJK_COUNT) count[v.items[i].key.kind]++;
    int shown = 0;
    for (int k = OBJK_MATVIEW; k < OBJK_COUNT; k++) {
        if (!count[k]) continue;
        char buf[16]; snprintf(buf, sizeof buf, "%d", count[k]);
        Node *sub = node_new(NODE_GROUP, names[k], buf, grp);
        sub->expanded = count[k] <= 200;
        for (int i = 0; i < v.n; i++) {
            const ObjListItem *it = &v.items[i];
            if (it->key.kind != k) continue;
            char label[400];
            if ((k == OBJK_FUNCTION || k == OBJK_PROCEDURE) && it->key.signature[0]) snprintf(label, sizeof label, "%s(%s)", it->key.name, it->key.signature);
            else if (k == OBJK_FUNCTION || k == OBJK_PROCEDURE) snprintf(label, sizeof label, "%s()", it->key.name);
            else snprintf(label, sizeof label, "%s", it->key.name);
            char detail[64] = "";
            bool draft = objed_has_draft(c, &it->key);
            if (it->extension[0]) snprintf(detail, sizeof detail, "extension %s", it->extension);   /* OBJ-058: read-only, owned by it */
            else if (!strcmp(it->status, "AGGREGATE")) snprintf(detail, sizeof detail, "aggregate");
            else if (it->status[0] && strcasecmp(it->status, "VALID")) snprintf(detail, sizeof detail, "%s%s", it->status, draft ? " \xC2\xB7 draft" : "");
            else if (draft) snprintf(detail, sizeof detail, "draft");
            else if (k == OBJK_TRIGGER && it->key.table[0]) snprintf(detail, sizeof detail, "on %s", it->key.table);
            Node *n = node_new(NODE_OBJECT, label, detail[0] ? detail : NULL, sub);
            n->leaf = true;
            n->okey = malloc(sizeof *n->okey); *n->okey = it->key;
            shown++;
        }
    }
    if (!shown) node_new(NODE_GROUP, "No routines, packages or triggers", NULL, grp)->leaf = true;
}
static Node *add_objgroup(Node *parent, Connection *c, const char *schema) {
    const ObjEngine *eng = obj_engine_for_driver(c->driver);
    if (!eng || !eng->list_sql) return NULL;
    Node *g = node_new(NODE_GROUP, "Objects", NULL, parent);
    g->objgroup = true; g->schema = SDL_strdup(schema);
    g->expanded = objgroup_is_open(c, schema);
    build_objects(g);
    return g;
}

/* Fills a table node with its column nodes, loading them from the database the first time
 * (blocking). A failure leaves a single message row; a busy session is retried on next expand. */
static void load_columns(Node *n) {
    if (n->loaded || !n->table) return;
    if (n->conn && n->conn->db && n->conn->db->busy && !n->table->cols_loaded) {
        node_clear_kids(n);
        node_new(NODE_COLUMN, "The session is busy: expand again to load the columns", NULL, n)->leaf = true;
        return;
    }
    n->loaded = true;
    if (!conn_load_columns(n->conn, n->table)) {
        node_clear_kids(n);
        node_new(NODE_COLUMN, "Could not load columns", NULL, n)->leaf = true;
        return;
    }
    node_clear_kids(n);
    for (int i = 0; i < n->table->ncols; i++) {
        DbColumn *c = &n->table->cols[i];
        Node *k = node_new(NODE_COLUMN, c->name, c->type, n);
        k->leaf = true;
        k->pk = c->pk;
    }
    if (!n->table->ncols) node_new(NODE_COLUMN, "(no columns)", NULL, n)->leaf = true;
}

/* a table / view row; reopens its columns when the user had it open */
static Node *add_table(Node *parent, DbTable *t, const char *label) {
    Node *n = node_new(NODE_TABLE, label, NULL, parent);
    n->table = t;
    if (xs_get(n, false)) { n->expanded = true; load_columns(n); }
    return n;
}
static Node *add_action(Node *parent, const char *label, int act, const char *schema) {
    Node *n = node_new(NODE_ACTION, label, NULL, parent);
    n->leaf = true; n->action = act; if (schema) n->sname = SDL_strdup(schema);
    return n;
}
static Node *add_msg(Node *parent, const char *label) { Node *n = node_new(NODE_GROUP, label, NULL, parent); n->leaf = true; return n; }

/* Children of an open schema: its tables / views (the connection's list, else read on demand),
 * then its Objects group. Never DDL, never the routines' text (DBX-090). */
static void fill_schema(Node *n) {
    Connection *c = n->conn;
    NsState *s = nsx_find(c->id);
    node_clear_kids(n);
    if (!s || !c->db) return;
    const char *home = home_schema(c, s);
    int shown = 0;
    if (s->ad->caps.conn_tables_all || (home && !strcmp(home, n->sname))) {
        for (int i = 0; i < c->ntables; i++) {
            char sc[128]; const char *name = c->tables[i].name;
            if (s->ad->caps.conn_tables_all) { table_schema(c, name, sc, sizeof sc); if (strcmp(sc, n->sname)) continue; }
            const char *dot = s->ad->caps.conn_tables_all ? strchr(name, '.') : NULL;
            add_table(n, &c->tables[i], dot ? dot + 1 : name);
            shown++;
        }
        if (!shown) add_msg(n, c->error[0] ? c->error : "No tables or views visible to this session");
    } else {
        NsInfo *in = nsx_info(s, n->sname, false);
        if (!in || (!in->tables_loaded && !in->tables_loading && !in->tables_err[0])) { nsx_load_tables(s, n->sname); in = nsx_info(s, n->sname, false); }
        if (in && in->tables_loading) {
            /* large catalogs can take minutes: the elapsed time shows it is still working, Cancel stops it */
            char m[64];
            if (in->tables_cancel) snprintf(m, sizeof m, "Cancelling\xE2\x80\xA6");
            else if (in->tables_secs >= 3) snprintf(m, sizeof m, "Loading\xE2\x80\xA6 %d s", in->tables_secs);
            else snprintf(m, sizeof m, "Loading\xE2\x80\xA6");
            add_msg(n, m);
            if (!in->tables_cancel) add_action(n, "Cancel", ACT_CANCEL_TABLES, n->sname);
        }
        else if (in && !strcmp(in->tables_err, "cancelled")) { add_msg(n, "Listing cancelled"); add_action(n, "Retry", ACT_RETRY_TABLES, n->sname); }
        else if (in && in->tables_err[0]) { char m[260]; snprintf(m, sizeof m, "Could not list tables: %s", in->tables_err); add_msg(n, m); add_action(n, "Retry", ACT_RETRY_TABLES, n->sname); }
        else if (in && in->tables_loaded) {
            ExtTables *x = ext_get(c->id, in);
            for (int i = 0; i < x->n; i++) { const char *dot = strchr(x->t[i].name, '.'); add_table(n, &x->t[i], dot ? dot + 1 : x->t[i].name); }
            if (!x->n) add_msg(n, "No tables or views visible to this session");   /* not "empty" (DBX-078) */
            if (in->tables_more) add_msg(n, "More than 5,000: use Find Objects");
        }
    }
    add_objgroup(n, c, n->sname);
}

/* a schema row (highlighted or in Others); its detail never carries a visibility badge (DBX-078.5) */
static Node *add_schema(Node *parent, NsState *s, const char *name, int flags, bool in_others, bool anchored) {
    Connection *c = parent->conn;
    char detail[64] = "";
    if (flags & NSF_CURRENT) snprintf(detail, sizeof detail, "current");
    else if (flags & NSF_PINNED) snprintf(detail, sizeof detail, "pinned");
    else if (flags & NSF_RECENT) snprintf(detail, sizeof detail, "recent");
    else if (anchored) snprintf(detail, sizeof detail, "found");
    NsInfo *in = nsx_info(s, name, false);
    if (in && in->system) snprintf(detail + strlen(detail), sizeof detail - strlen(detail), "%ssystem", detail[0] ? " \xC2\xB7 " : "");
    if (in && in->exists_known && !in->exists && !in_others) snprintf(detail + strlen(detail), sizeof detail - strlen(detail), "%snot found", detail[0] ? " \xC2\xB7 " : "");
    Node *n = node_new(NODE_SCHEMA, name, detail[0] ? detail : NULL, parent);
    n->sname = SDL_strdup(name); n->in_others = in_others;
    /* default: the schema the connection's tables belong to opens, as the old tree did when small */
    bool def = false;
    if (!in_others) {
        const char *home = home_schema(c, s);
        if (s->ad->caps.conn_tables_all) {
            int cnt = 0; for (int i = 0; i < c->ntables; i++) { char sc[128]; table_schema(c, c->tables[i].name, sc, sizeof sc); cnt += !strcmp(sc, name); }
            def = cnt > 0 && cnt <= 200;
        } else def = home && !strcmp(home, name) && c->ntables <= 200;
    }
    n->expanded = xs_get(n, def);
    if (n->expanded) fill_schema(n);
    return n;
}

/* The "Schemas" / "Databases" node: highlighted schemas, then Others (DBX-077). */
static void build_ns(Node *root, Connection *c, NsState *s) {
    nsx_featured(s);
    char det[160] = "";
    if (s->scope_loading) snprintf(det, sizeof det, "loading\xE2\x80\xA6");
    else if (s->scope_failed) snprintf(det, sizeof det, "limited");
    else if (s->container[0] && strcmp(c->driver, "mysql") && strcmp(c->driver, "mariadb")) snprintf(det, sizeof det, "%s", s->container);
    Node *g = node_new(NODE_GROUP, s->ad->label, det[0] ? det : NULL, root);
    g->role = NR_ROOT;
    g->expanded = xs_get(g, true);
    if (!g->expanded) return;
    if (s->scope_failed) {   /* degraded mode: what is known, Others and search keep working (DBX-118) */
        char m[260]; snprintf(m, sizeof m, "Schema list limited: %s", s->scope_error);
        add_msg(g, m);
        add_action(g, "Retry", ACT_RETRY_SCOPE, NULL);
    }
    for (int i = 0; i < s->nfeatured; i++) add_schema(g, s, s->featured[i].name, s->featured[i].flags, false, false);
    Node *list = g;
    if (nsx_others_shown(s)) {
        char lbl[64]; nsx_others_label(s, lbl, sizeof lbl);
        Node *o = node_new(NODE_GROUP, lbl, NULL, g);
        o->role = NR_OTHERS;
        o->expanded = xs_get(o, false);
        if (!o->expanded) return;
        nsx_expand_others(s);   /* first page of names only, when opened (DBX-079) */
        list = o;
        add_action(o, "Search in Others\xE2\x80\xA6", ACT_SEARCH_OTHERS, NULL);
    } else if (s->prefs.mode != NSM_ALL) return;
    else nsx_expand_others(s);
    /* anchors, then the loaded pages minus the highlighted ones; one row per loaded name only */
    for (int i = 0; i < s->nanchors; i++) if (!nsx_is_featured(s, s->anchors[i])) add_schema(list, s, s->anchors[i], 0, list != g, true);
    for (int i = 0; i < s->pager.n; i++) {
        const char *nm = s->pager.names[i];
        if (nsx_is_featured(s, nm) || (!s->prefs.show_system && s->pager.sys[i])) continue;
        add_schema(list, s, nm, 0, list != g, false);
    }
    if (s->pager.loading) add_msg(list, "Loading\xE2\x80\xA6");
    else if (s->pager.failed) { char m[260]; snprintf(m, sizeof m, "Could not list names: %s", s->pager.error); add_msg(list, m); add_action(list, "Retry", ACT_RETRY_PAGE, NULL); }
    else if (s->pager.has_more) { char m[64]; snprintf(m, sizeof m, "Load more (%d per page)", s->prefs.page_size); add_action(list, m, ACT_LOAD_MORE, NULL); }
    else if (s->pager.started && list != g && nsx_others_names(s, NULL, 0) == 0) add_msg(list, "No other schemas");
}

/* Builds the subtree of one connection. Closed connections are a single leaf. Open ones get
 * the Schemas node when the driver has a namespace adapter; otherwise one group per schema
 * when any table name is "schema.table", or "Tables" and "Views" groups. Column nodes are not
 * created here (see load_columns). */
static Node *build_conn(Connection *c, bool expanded) {
    const DriverInfo *di = conn_driver(c);
    Node *root = node_new(NODE_CONN, c->name, di ? di->name : c->driver, NULL);
    root->conn = c;
    root->leaf = c->db == NULL;
    root->expanded = c->db && expanded;
    if (!c->db) return root;
    NsState *ns = nsx_find(c->id);
    if (ns && ns->ad) { build_ns(root, c, ns); return root; }
    bool schemas = false;
    for (int i = 0; i < c->ntables && !schemas; i++) schemas = strchr(c->tables[i].name, '.') != NULL;
    char buf[64];
    if (schemas) {
        /* one group per schema (names are "schema.table"; bare names belong to the default schema) */
        for (int i = 0; i < c->ntables; i++) {
            const char *name = c->tables[i].name, *dot = strchr(name, '.');
            char schema[256];
            snprintf(schema, sizeof schema, "%.*s", dot ? (int)(dot - name) : 6, dot ? name : "public");
            Node *grp = NULL;
            for (int k = 0; k < root->nkids; k++) if (strcmp(root->kids[k]->label, schema) == 0) grp = root->kids[k];
            if (!grp) grp = node_new(NODE_GROUP, schema, "", root);
            Node *n = node_new(NODE_TABLE, dot ? dot + 1 : name, NULL, grp);
            n->table = &c->tables[i];
        }
        for (int k = 0; k < root->nkids; k++) {
            Node *grp = root->kids[k];
            snprintf(buf, sizeof buf, "%d", grp->nkids);
            free(grp->detail);
            grp->detail = SDL_strdup(buf);
            grp->expanded = grp->nkids <= 200;   /* huge schemas start collapsed */
        }
        int ngroups = root->nkids;
        for (int k = 0; k < ngroups; k++) add_objgroup(root->kids[k], c, root->kids[k]->label);
        return root;
    }
    int nt = 0, nv = 0;
    for (int i = 0; i < c->ntables; i++) c->tables[i].is_view ? nv++ : nt++;
    snprintf(buf, sizeof buf, "%d", nt);
    Node *tables = node_new(NODE_GROUP, "Tables", buf, root);
    tables->expanded = true;
    Node *views = NULL;
    if (nv) {
        snprintf(buf, sizeof buf, "%d", nv);
        views = node_new(NODE_GROUP, "Views", buf, root);
        views->expanded = true;
    }
    for (int i = 0; i < c->ntables; i++) {
        Node *n = node_new(NODE_TABLE, c->tables[i].name, NULL, c->tables[i].is_view ? views : tables);
        n->table = &c->tables[i];
    }
    if (!nt) node_new(NODE_GROUP, c->error[0] ? c->error : "No tables yet", NULL, tables)->leaf = true;
    add_objgroup(root, c, strcmp(c->driver, "sqlite") ? "" : "main");
    return root;
}

/* forward declaration: explorer_rebuild flattens the new tree */
static void flatten(Explorer *e, Node *n);

/* Moves a built subtree (a root) under parent: depths follow. */
static void shift_depth(Node *n, int by) { n->depth += by; for (int i = 0; i < n->nkids; i++) shift_depth(n->kids[i], by); }
static void adopt(Node *parent, Node *child) {
    if (parent->nkids == parent->cap) { parent->cap = parent->cap ? parent->cap * 2 : 8; parent->kids = realloc(parent->kids, sizeof *parent->kids * (size_t)parent->cap); }
    parent->kids[parent->nkids++] = child;
    child->parent = parent;
    shift_depth(child, parent->depth + 1 - child->depth);
}
/* Folders first (by name, case-insensitive), then connections in the store's order; recursively. */
static int level_cmp(const void *x, const void *y) {
    const Node *a = *(Node *const *)x, *b = *(Node *const *)y;
    bool fa = a->kind == NODE_FOLDER, fb = b->kind == NODE_FOLDER;
    if (fa != fb) return fa ? -1 : 1;
    if (fa) return strcasecmp(a->label, b->label);
    return a->order - b->order;
}
static int sort_level(Node *n) {   /* returns how many connections are inside */
    int count = 0;
    for (int i = 0; i < n->nkids; i++) count += n->kids[i]->kind == NODE_FOLDER ? sort_level(n->kids[i]) : 1;
    if (n->nkids > 1) qsort(n->kids, (size_t)n->nkids, sizeof *n->kids, level_cmp);
    if (n->kind == NODE_FOLDER && n->sname) { char d[16]; snprintf(d, sizeof d, "%d", count); free(n->detail); n->detail = SDL_strdup(d); n->leaf = n->nkids == 0; }
    return count;
}
/* The node of connection c, wherever its folder put it (NULL when not in the tree). */
static Node *conn_node_in(Node *n, Connection *c) {
    if (n->kind == NODE_CONN) return n->conn == c ? n : NULL;
    if (n->kind != NODE_FOLDER) return NULL;
    for (int i = 0; i < n->nkids; i++) { Node *f = conn_node_in(n->kids[i], c); if (f) return f; }
    return NULL;
}
static Node *conn_node(Explorer *e, Connection *c) {
    for (int i = 0; i < e->nroots; i++) { Node *f = conn_node_in(e->roots[i], c); if (f) return f; }
    return NULL;
}
/* Opens the folders above n (so a revealed row is visible). */
static void open_ancestors(Node *n) {
    for (Node *p = n ? n->parent : NULL; p; p = p->parent) if (p->kind == NODE_FOLDER && !p->expanded) { p->expanded = true; if (p->sname) fo_set(p->sname, true); }
}

/* depth-first search for the node that matches a remembered selection */
static Node *find_node(Node *n, int kind, const char *label, const char *parent_label) {
    if ((int)n->kind == kind && n->label && strcmp(n->label, label) == 0 &&
        (!parent_label || (n->parent && n->parent->label && strcmp(n->parent->label, parent_label) == 0))) return n;
    for (int i = 0; i < n->nkids; i++) { Node *f = find_node(n->kids[i], kind, label, parent_label); if (f) return f; }
    return NULL;
}
static Node *find_key(Node *n, const char *key) {
    char k[400]; node_key(n, k, sizeof k);
    if (!strcmp(k, key)) return n;
    for (int i = 0; i < n->nkids; i++) { Node *f = find_key(n->kids[i], key); if (f) return f; }
    return NULL;
}
static uint64_t ns_signature(App *a) {
    uint64_t sig = 0;
    for (int i = 0; a->conns && i < a->conns->n; i++) { NsState *s = nsx_find(a->conns->items[i]->id); if (s) sig += (uint64_t)s->rev * (uint64_t)(i + 1) * 2654435761u; }
    return sig;
}

/* See explorer.h. Keeps the expanded / collapsed state of each root when the connection
 * list did not change order, and re-finds the selected row in the new tree. */
void explorer_rebuild(Explorer *e, App *a) {
    E_APP = a;
    static bool loaded;
    if (!loaded && a->conns) { loaded = true; nsx_set_dir(a->conns->dir); nsx_load(); conn_add_close_hook(ns_on_close); }
    for (int i = 0; a->conns && i < a->conns->n; i++) ns_sync(a, a->conns->items[i]);
    /* remember which connections were collapsed and which row was selected: every Node is
     * freed below, so the selection is re-found by its stable key (never kept as a pointer:
     * that was a use-after-free when switching connections) */
    Connection *sel = e->selected ? e->selected->conn : NULL;
    int sel_kind = e->selected ? (int)e->selected->kind : -1;
    char sel_label[256] = "", sel_parent[256] = "";
    if (e->selected) node_key(e->selected, e->sel_key, sizeof e->sel_key);
    if (e->selected && e->selected->label) snprintf(sel_label, sizeof sel_label, "%s", e->selected->label);
    if (e->selected && e->selected->parent && e->selected->parent->label) snprintf(sel_parent, sizeof sel_parent, "%s", e->selected->parent->label);
    /* scroll anchor: the first visible row keeps its place on screen (DBX-113) */
    char anchor[400] = ""; float anchor_off = 0;
    float row_h = a ? px(a, M_ROW) : 24;
    if (e->nvis && row_h > 0) {
        int first = (int)(e->scroll_t / row_h);
        if (first >= 0 && first < e->nvis) { node_key(e->vis[first], anchor, sizeof anchor); anchor_off = e->scroll_t - first * row_h; }
    }
    e->selected = NULL; e->nmsel = 0; e->anchor = NULL;
    e->nvis = 0;                                   /* e->vis points into the tree being freed */
    /* which open connections the user collapsed: by connection (folders can move them around) */
    int ncon = a->conns ? a->conns->n : 0;
    Connection **was_c = calloc(ncon ? (size_t)ncon : 1, sizeof *was_c); bool *was_closed = calloc(ncon ? (size_t)ncon : 1, sizeof *was_closed); int nwas = 0;
    {   /* folders and connections only: at most one stack entry per connection and folder level */
      int cap = 16 + e->nroots, sp = 0; Node **stack = malloc(sizeof *stack * (size_t)cap);
      for (int i = 0; stack && i < e->nroots; i++) stack[sp++] = e->roots[i];
      while (stack && sp) { Node *n = stack[--sp];
          if (n->kind == NODE_CONN) { if (nwas < ncon) { was_c[nwas] = n->conn; was_closed[nwas++] = !n->leaf && !n->expanded; } }
          else if (n->kind == NODE_FOLDER) for (int k = 0; k < n->nkids; k++) {
              if (sp == cap) { cap *= 2; Node **g = realloc(stack, sizeof *stack * (size_t)cap); if (!g) break; stack = g; }
              stack[sp++] = n->kids[k];
          } }
      free(stack); }
    free_roots(e);
    fo_load(a);
    /* the top level is a hidden holder node: its children become the roots */
    Node *top = node_new(NODE_FOLDER, "", NULL, NULL); top->depth = -1;
    for (int i = 0; i < ncon; i++) {
        Connection *c = a->conns->items[i];
        Node *parent = top;
        char path[256] = "";
        for (const char *p = c->folder; *p;) {   /* one folder node per level, created on first use */
            const char *q = strchr(p, '/'); size_t L = q ? (size_t)(q - p) : strlen(p);
            char seg[128]; snprintf(seg, sizeof seg, "%.*s", (int)L, p);
            size_t pl = strlen(path); snprintf(path + pl, sizeof path - pl, "%s%s", pl ? "/" : "", seg);
            Node *f = NULL;
            for (int k = 0; k < parent->nkids && !f; k++) if (parent->kids[k]->kind == NODE_FOLDER && !strcmp(parent->kids[k]->label, seg)) f = parent->kids[k];
            if (!f) { f = node_new(NODE_FOLDER, seg, NULL, parent); f->sname = SDL_strdup(path); f->expanded = fo_is_open(path); f->conn = NULL; }
            parent = f;
            p += L; if (*p == '/') p++;
        }
        bool closed = false;
        for (int k = 0; k < nwas; k++) if (was_c[k] == c) closed = was_closed[k];
        Node *cn = build_conn(c, !closed);
        adopt(parent, cn);
        cn->order = i;   /* store order, for sorting */
    }
    free(was_c); free(was_closed);
    sort_level(top);
    e->nroots = top->nkids;
    e->roots = calloc(e->nroots ? (size_t)e->nroots : 1, sizeof *e->roots);
    for (int i = 0; i < e->nroots; i++) { e->roots[i] = top->kids[i]; e->roots[i]->parent = NULL; }
    top->nkids = 0; node_free(top);
    /* the selection, by its stable key */
    if (sel_kind >= 0) {
        for (int i = 0; i < e->nroots && !e->selected; i++) e->selected = find_key(e->roots[i], e->sel_key);
        if (!e->selected && sel) {
            Node *cn = conn_node(e, sel);
            if (cn && sel_kind == NODE_CONN) e->selected = cn;
            else if (cn) e->selected = find_node(cn, sel_kind, sel_label, sel_parent[0] ? sel_parent : NULL);
        }
    }
    for (int i = 0; i < e->nroots; i++) flatten(e, e->roots[i]);
    if (anchor[0])
        for (int i = 0; i < e->nvis; i++) {
            char k[400]; node_key(e->vis[i], k, sizeof k);
            if (!strcmp(k, anchor)) { e->scroll_t = i * row_h + anchor_off; e->scroll = e->scroll_t; break; }
        }
    e->ns_sig = ns_signature(a);
}

/* See explorer.h. Blocking catalog query on the UI thread. */
void explorer_refresh(Explorer *e, App *a) {
    if (a->active) { conn_refresh(a->active); app_refresh_schema(a); }
    explorer_rebuild(e, a);
}

/* See explorer.h. */
Connection *explorer_selected_conn(Explorer *e) { return e->selected ? e->selected->conn : NULL; }

/* Appends n and, when expanded, its visible descendants to e->vis (pre-order). */
static void flatten(Explorer *e, Node *n) {
    if (e->nvis == e->capvis) {
        e->capvis = e->capvis ? e->capvis * 2 : 64;
        e->vis = realloc(e->vis, sizeof *e->vis * e->capvis);
    }
    e->vis[e->nvis++] = n;
    if (n->expanded) for (int i = 0; i < n->nkids; i++) flatten(e, n->kids[i]);
}

/* Expands or collapses a node; expanding a table loads its columns, a schema its tables. */
static void toggle(Node *n) {
    if (n->leaf) return;
    n->expanded = !n->expanded;
    if (n->kind == NODE_FOLDER) { if (n->sname) fo_set(n->sname, n->expanded); return; }
    if (n->kind == NODE_TABLE || n->kind == NODE_SCHEMA || n->role) xs_set(n, n->expanded);
    if (n->expanded && n->kind == NODE_TABLE) { if (!n->loaded) node_clear_kids(n); load_columns(n); }
    if (n->kind == NODE_SCHEMA && n->conn) {
        NsState *s = nsx_find(n->conn->id);
        if (n->expanded) { fill_schema(n); if (s) nsx_note_recent(s, n->sname); }   /* lazy (DBX-090) */
    }
    if (n->role == NR_OTHERS && n->expanded && n->conn) { NsState *s = nsx_find(n->conn->id); if (s) nsx_expand_others(s); }
    if (n->objgroup) { objgroup_set_open(n->conn, n->schema, n->expanded); if (n->expanded) build_objects(n); }   /* lazy (OBJ-004) */
}

/* Identity of the object a node stands for: an object node, or a view of the table list. */
static bool node_objkey(Node *n, ObjKey *k) {
    if (n->kind == NODE_OBJECT && n->okey) { *k = *n->okey; return true; }
    if (n->kind != NODE_TABLE || !n->table || !n->table->is_view) return false;
    memset(k, 0, sizeof *k);
    k->kind = OBJK_VIEW;
    const char *name = n->table->name, *dot = strchr(name, '.');
    if (dot) { snprintf(k->schema, sizeof k->schema, "%.*s", (int)(dot - name), name); snprintf(k->name, sizeof k->name, "%s", dot + 1); }
    else { snprintf(k->name, sizeof k->name, "%s", name); snprintf(k->schema, sizeof k->schema, "%s", !strcmp(n->conn->driver, "sqlite") ? "main" : n->conn->schema); }
    return true;
}

/* Opens a preview of a table (first 200 rows) by name, making its connection active first. */
static void preview_name(App *a, Connection *c, const char *name, const char *label) {
    if (!c || !c->db) return;
    char nm[512], lb[256];
    snprintf(nm, sizeof nm, "%s", name); snprintf(lb, sizeof lb, "%s", label);
    if (a->active != c) app_set_active(a, c);
    if (!c->db) return;
    char q[512], sql[640];
    c->db->drv->quote_ident(nm, q, sizeof q);
    c->db->drv->select_sql(q, NULL, NULL, 200, sql, sizeof sql);
    app_run_sql_mode(a, c->db, sql, -1, lb, RUN_PREVIEW, q);
}
/* `n` may be freed by the connection switch, so it is not used afterwards. */
static void preview_table(App *a, Node *n) {
    Connection *c = n->conn;
    char name[512], label[256];
    snprintf(name, sizeof name, "%s", n->table ? n->table->name : n->label);
    snprintf(label, sizeof label, "%s", n->label);
    preview_name(a, c, name, label);
}

/* Row index of a node in the visible list, -1 when not visible. */
static int vis_index(Explorer *e, Node *n) {
    for (int i = 0; i < e->nvis; i++) if (e->vis[i] == n) return i;
    return -1;
}

/* Adjusts the target scroll so visible row idx is fully inside the rows area. */
static void ensure_visible(Explorer *e, App *a, int idx) {
    float row_h = px(a, M_ROW);
    float top = idx * row_h, bottom = top + row_h;
    if (top < e->scroll_t) e->scroll_t = top;
    else if (bottom > e->scroll_t + e->rows_rect.h) e->scroll_t = bottom - e->rows_rect.h;
}

/* Makes an open connection active, or connects a closed one. */
static void activate_conn(App *a, Connection *c) {
    if (!c) return;
    if (c->db) app_set_active(a, c);
    else app_connect(a, c);
}

/* ---- Schema Explorer actions ---- */

static void search_open(Explorer *e, Connection *c, bool objects, const char *ns);
static void cm_open_node(Explorer *e, App *a, Node *n, float x, float y);
static const char *dbs_label(NsState *s);

/* the Schemas / Others nodes of a connection's subtree */
static Node *ns_group(Explorer *e, Connection *c, int role) {
    Node *cn = conn_node(e, c);
    if (cn) {
        for (int k = 0; k < cn->nkids; k++) {
            Node *g = cn->kids[k];
            if (g->role != NR_ROOT) continue;
            if (role == NR_ROOT) return g;
            for (int j = 0; j < g->nkids; j++) if (g->kids[j]->role == NR_OTHERS) return g->kids[j];
        }
    }
    return NULL;
}
static Node *find_schema(Node *n, const char *name) {
    if (n->kind == NODE_SCHEMA && n->sname && !strcmp(n->sname, name)) return n;
    for (int i = 0; i < n->nkids; i++) { Node *f = find_schema(n->kids[i], name); if (f) return f; }
    return NULL;
}
/* Reveal in Explorer: highlighted, or anchored in Others; never the pages before it (DBX-106 / 109) */
static void reveal_schema(Explorer *e, App *a, Connection *c, const char *name, bool open) {
    NsState *s = nsx_find(c->id);
    if (!s) return;
    char nm[128]; snprintf(nm, sizeof nm, "%s", name);
    nsx_reveal(s, nm);
    explorer_rebuild(e, a);
    Node *root = ns_group(e, c, NR_ROOT);
    if (root && !root->expanded) { toggle(root); explorer_rebuild(e, a); }
    if (!nsx_is_featured(s, nm) && s->prefs.mode != NSM_ALL) {
        Node *o = ns_group(e, c, NR_OTHERS);
        if (o && !o->expanded) { xs_set(o, true); explorer_rebuild(e, a); }
    }
    Node *cn = conn_node(e, c);
    if (cn) {
        open_ancestors(cn);
        if (!cn->expanded) { cn->expanded = true; }
        Node *n = find_schema(cn, nm);
        if (n && open && !n->expanded) toggle(n);
        if (n) { e->selected = n; node_key(n, e->sel_key, sizeof e->sel_key); }
    }
    e->nvis = 0; for (int i = 0; i < e->nroots; i++) flatten(e, e->roots[i]);
    int idx = e->selected ? vis_index(e, e->selected) : -1;
    if (idx >= 0) ensure_visible(e, a, idx);
    a->focus = FOCUS_EXPLORER;
}

/* Open SQL Console: a new query tab on this connection; the session's schema and the other
 * tabs are left alone (DBX-109.3 / 123.1) */
static void open_console(App *a, Connection *c, const char *schema) {
    NsState *s = nsx_find(c->id);
    if (s) nsx_note_recent(s, schema);
    char text[400], title[80];
    snprintf(text, sizeof text, "-- %s on %s. Names are not qualified for you and the session's default schema is unchanged.\n\n", schema, c->name);
    snprintf(title, sizeof title, "%s", schema);
    panels_open_tab(a->panels, a, title, c->id, text);
}
static void copy_text(App *a, const char *t, const char *what) {
    SDL_SetClipboardText(t);
    app_status(a, "%s copied", what);
}

/* runs an action row (Enter / double click / single click) */
static void run_action(Explorer *e, App *a, Node *n) {
    Connection *c = n->conn;
    NsState *s = c ? nsx_find(c->id) : NULL;
    if (!s) return;
    switch (n->action) {
    case ACT_SEARCH_OTHERS: search_open(e, c, false, ""); break;
    case ACT_LOAD_MORE: nsx_load_more(s); break;   /* only the next page of names (DBX-105.2) */
    case ACT_RETRY_PAGE: s->pager.failed = false; s->pager.started = s->pager.n > 0; nsx_load_more(s); break;
    case ACT_RETRY_TABLES: if (n->sname) { nsx_refresh_schema(s, n->sname); nsx_load_tables(s, n->sname); } break;
    case ACT_RETRY_SCOPE: nsx_refresh(s); break;
    case ACT_CANCEL_TABLES: if (n->sname) { nsx_cancel_tables(s, n->sname); s->rev++; app_wake_in(a, 0); } break;
    }
}

/* Keyboard navigation when the explorer has focus: Up / Down move, Right expands or enters
 * the first child, Left collapses or goes to the parent, Enter previews a table, activates
 * a connection or toggles a group; Shift+F10 / Menu opens the row's menu; Cmd+F searches. */
static void handle_keys(Explorer *e, App *a) {
    unsigned mods;
    int idx = e->selected ? vis_index(e, e->selected) : -1;
    if (input_key_any(&a->in, SDLK_F, &mods) && (mods & (SDL_KMOD_GUI | SDL_KMOD_CTRL))) {
        Connection *c = e->selected ? e->selected->conn : a->active;
        if (c && nsx_find(c->id)) { search_open(e, c, false, ""); return; }
    }
    if (input_key_any(&a->in, SDLK_DOWN, &mods) && e->nvis) {
        idx = idx < e->nvis - 1 ? idx + 1 : e->nvis - 1;
        e->selected = e->vis[idx];
        ensure_visible(e, a, idx);
    }
    if (input_key_any(&a->in, SDLK_UP, &mods) && e->nvis) {
        idx = idx > 0 ? idx - 1 : 0;
        e->selected = e->vis[idx];
        ensure_visible(e, a, idx);
    }
    if (!e->selected) return;
    if (input_key_any(&a->in, SDLK_APPLICATION, &mods) || (input_key_any(&a->in, SDLK_F10, &mods) && (mods & SDL_KMOD_SHIFT))) {
        int i = vis_index(e, e->selected);
        float y = e->rows_rect.y + (i + 1) * px(a, M_ROW) - e->scroll;
        cm_open_node(e, a, e->selected, e->rows_rect.x + px(a, 40), y);
        return;
    }
    if (input_key_any(&a->in, SDLK_RIGHT, &mods)) {
        if (!e->selected->leaf && !e->selected->expanded) toggle(e->selected);
        else if (e->selected->nkids) e->selected = e->selected->kids[0];
    }
    if (input_key_any(&a->in, SDLK_LEFT, &mods)) {
        if (e->selected->expanded) toggle(e->selected);
        else if (e->selected->parent) e->selected = e->selected->parent;
    }
    if (input_key_any(&a->in, SDLK_RETURN, &mods) || input_key_any(&a->in, SDLK_KP_ENTER, &mods)) {
        if (e->selected->kind == NODE_TABLE) preview_table(a, e->selected);
        else if (e->selected->kind == NODE_CONN) activate_conn(a, e->selected->conn);
        else if (e->selected->kind == NODE_OBJECT && e->selected->okey) { ObjKey k = *e->selected->okey; objed_open(a, e->selected->conn, &k); }
        else if (e->selected->kind == NODE_ACTION) run_action(e, a, e->selected);
        else toggle(e->selected);
    }
}

/* ---- context menu ------------------------------------------------------ */

static void cm_reset(Explorer *e, Connection *c, float x, float y) {
    e->cm_open = true; e->cm_conn = c; e->cm_x = x; e->cm_y = y; e->cm_n = 0; e->cm_sel = -1; e->cm_schema[0] = 0; e->cm_hit = -1;
}
static void cm_add(Explorer *e, int cmd, const char *text, int check, bool disabled) {
    if (e->cm_n >= CM_MAX) return;
    e->cm_cmd[e->cm_n] = cmd; snprintf(e->cm_text[e->cm_n], sizeof e->cm_text[0], "%s", text ? text : "");
    e->cm_check[e->cm_n] = (signed char)check; e->cm_dis[e->cm_n] = disabled || cmd == CM_INFO || cmd == CM_SEP;
    e->cm_n++;
}
static void cm_info(Explorer *e, const char *fmt, ...) {
    char b[160]; va_list ap; va_start(ap, fmt); vsnprintf(b, sizeof b, fmt, ap); va_end(ap);
    cm_add(e, CM_INFO, b, -1, true);
}

/* Opens the context menu at (x, y) for connection c (NULL: only "New Connection"). */
static void cm_open(Explorer *e, Connection *c, float x, float y) {
    cm_reset(e, c, x, y);
    if (c) {
        /* grouped: work on it / create in it / the connection itself */
        cm_add(e, CM_NEW_QUERY, "New Query", -1, false);   /* a query tab on this connection (connects first when needed) */
        if (c->db && tdes_supported(c)) cm_add(e, CM_NEW_TABLE, "New Table\xE2\x80\xA6", -1, c->read_only);
        cm_add(e, CM_SC_ERD, "New ER Diagram\xE2\x80\xA6", -1, false);   /* DGM-002: the connection's default scope */
        if (c->db) cm_add(e, CM_REFRESH, "Refresh", -1, false);
        cm_add(e, CM_SEP, "", -1, true);
        cm_add(e, CM_EDIT, "Edit Connection\xE2\x80\xA6", -1, false);
        cm_add(e, CM_DUP_CONN, "Duplicate Connection\xE2\x80\xA6", -1, false);   /* prefilled, without the password */
        cm_add(e, c->db ? CM_DISCONNECT : CM_CONNECT, c->db ? "Disconnect" : "Connect", -1, false);
        cm_add(e, CM_SEP, "", -1, true);
    }
    cm_add(e, CM_NEW, "New Connection\xE2\x80\xA6", -1, false);
}

/* View Options (DBX-108 / 080 / 082 / 089 / 097): a menu of toggles that stays open */
static void cm_options(Explorer *e, Connection *c, float x, float y) {
    NsState *s = nsx_find(c->id);
    if (!s) return;
    cm_reset(e, c, x, y);
    NsPrefs *p = &s->prefs;
    cm_info(e, "Others: other %s that are not highlighted (says nothing about their objects)", !strcmp(s->ad->label, "Databases") ? "databases" : "schemas");
    cm_add(e, CM_OPT_COMPACT, "Compact: highlights + Others", p->mode == NSM_COMPACT, false);
    cm_add(e, CM_OPT_ALL, "All names (paged)", p->mode == NSM_ALL, false);
    cm_add(e, CM_OPT_PINNED, "Pinned only", p->mode == NSM_PINNED, false);
    cm_add(e, CM_SEP, "", -1, true);
    cm_add(e, CM_OPT_SYSTEM, "Show system schemas", p->show_system, false);
    cm_add(e, CM_OPT_RECENT, "Highlight recent schemas", p->auto_recent, false);
    cm_add(e, CM_OPT_KNOWN, "Highlight owners with known objects", p->auto_known, false);
    char b[96];
    snprintf(b, sizeof b, "Automatic highlights: %d (pins not counted)", p->auto_cap); cm_add(e, CM_OPT_CAP, b, -1, false);
    snprintf(b, sizeof b, "Page size: %d names", p->page_size); cm_add(e, CM_OPT_PAGE, b, -1, false);
    snprintf(b, sizeof b, "Relevant objects: %s", p->kinds == NSK_ALL ? "any" : p->kinds == (NSK_TABLES | NSK_VIEWS) ? "tables and views" : p->kinds == NSK_ROUTINES ? "routines and packages" : "tables");
    cm_add(e, CM_OPT_KINDS, b, -1, false);
    cm_add(e, CM_OPT_DISCOVER, s->ad->caps.discovery ? "Discover owners with objects (one bounded catalog query)" : "Discovery not available", p->discover, !s->ad->caps.discovery);
    if (p->discover || s->discover_note[0]) cm_info(e, "%s", s->discover_note[0] ? s->discover_note : "Advanced: reads the object catalog once, with a time budget");
    cm_add(e, CM_SEP, "", -1, true);
    cm_add(e, CM_OPT_RESET, "Reset for this connection", -1, false);
}

/* Properties (DBX-078 / 107 / 117): what is known, where it came from, never "empty" */
static void cm_props_schema(Explorer *e, Connection *c, const char *name, float x, float y) {
    NsState *s = nsx_find(c->id);
    if (!s) return;
    cm_reset(e, c, x, y);
    snprintf(e->cm_schema, sizeof e->cm_schema, "%s", name);
    nsx_featured(s);
    int f = nsx_featured_flags(s, name);
    NsInfo *in = nsx_info(s, name, false);
    cm_info(e, "%s  %s", !strcmp(s->ad->label, "Databases") ? "Database" : "Schema", name);
    if (s->container[0]) cm_info(e, "Container / database: %s", s->container);
    if (f) cm_info(e, "Shown under %s:%s%s%s%s%s", s->ad->label, f & NSF_CURRENT ? " current" : "", f & NSF_PINNED ? " pinned" : "", f & NSF_RECENT ? " recent" : "", f & NSF_AUTO ? " known objects" : "", f & NSF_SMALL ? " small catalog" : "");
    else cm_info(e, "In Others: not highlighted (this says nothing about its objects)");
    cm_info(e, "Visible objects: %s", nsx_vis_text(in ? in->vis : NSV_UNKNOWN));
    if (in && in->note[0]) cm_info(e, "%s", in->note);
    if (in && in->checked_ms) cm_info(e, "Checked %.0f s ago for user %s", (SDL_GetTicks() - in->checked_ms) / 1000.0, s->user[0] ? s->user : "?");
    if (in && in->exists_known && !in->exists) cm_info(e, "Not found in this scope (a pin kept from earlier)");
    cm_info(e, "System schema: %s", in && in->exists_known ? (in->system ? "yes" : "no") : s->sysflag_unavailable ? "unknown (no ORACLE_MAINTAINED)" : "not checked");
    cm_add(e, CM_SEP, "", -1, true);
    cm_add(e, CM_SC_VERIFY, "Verify Visible Objects", -1, !s->ad->caps.visibility_probe);
}
static void cm_props_root(Explorer *e, Connection *c, float x, float y) {
    NsState *s = nsx_find(c->id);
    if (!s) return;
    cm_reset(e, c, x, y);
    nsx_featured(s);
    char lbl[64]; nsx_others_label(s, lbl, sizeof lbl);
    cm_info(e, "%s of %s", s->ad->label, c->name);
    cm_info(e, "User %s%s%s", s->user[0] ? s->user : "?", s->container[0] ? " in " : "", s->container);
    if (s->scope_failed) cm_info(e, "Limited mode: %s", s->scope_error);
    cm_info(e, "Highlighted: %d \xC2\xB7 mode %s", s->nfeatured, s->prefs.mode == NSM_ALL ? "all names" : s->prefs.mode == NSM_PINNED ? "pinned only" : "compact");
    cm_info(e, "%s \xC2\xB7 %d names loaded in %d page%s%s", s->total_known ? lbl : "Total not known yet (never counted at a cost)", s->pager.n, s->pager.pages, s->pager.pages == 1 ? "" : "s", s->pager.has_more ? ", more available" : "");
    cm_info(e, "Catalog reads: %d \xC2\xB7 ready in %.0f ms \xC2\xB7 last page %.0f ms \xC2\xB7 last search %.0f ms", s->m.queries, s->m.useful_ms, s->m.last_page_ms, s->m.last_search_ms);
    cm_info(e, "Cancelled %d \xC2\xB7 timed out %d \xC2\xB7 stale answers dropped %d \xC2\xB7 failures %d", s->m.cancels, s->m.timeouts, s->m.skipped_stale, s->m.failures);
    if (s->discover_note[0]) cm_info(e, "%s", s->discover_note);
}


/* schema and name of a table row as the table designer addresses it */
static void table_target(Node *n, char *schema, int sn, char *name, int nn) {
    Connection *c = n->conn;
    const char *full = n->table ? n->table->name : n->label, *dot = strchr(full, '.');
    snprintf(name, (size_t)nn, "%s", dot ? dot + 1 : full);
    schema[0] = 0;
    if (dot) { snprintf(schema, (size_t)sn, "%.*s", (int)(dot - full), full); return; }
    for (Node *p = n->parent; p; p = p->parent) if (p->kind == NODE_SCHEMA && p->sname) { snprintf(schema, (size_t)sn, "%s", p->sname); return; }
    if (!strcmp(c->driver, "sqlite")) snprintf(schema, (size_t)sn, "main");
    else if (!strcmp(c->driver, "postgres")) snprintf(schema, (size_t)sn, "public");
    else if (!strcmp(c->driver, "oracle")) { const char *u = conn_value(c, "user"); snprintf(schema, (size_t)sn, "%s", u ? u : ""); for (char *x = schema; *x; x++) *x = (char)toupper((unsigned char)*x); }
    else { const char *db = conn_value(c, "database"); snprintf(schema, (size_t)sn, "%s", db ? db : ""); }
}
/* the menu of a tree row, by what it is (DBX-104 / 105 / 107) */
static void cm_open_node(Explorer *e, App *a, Node *n, float x, float y) {
    if (n && n->kind == NODE_FOLDER && n->sname) {
        cm_reset(e, NULL, x, y);
        snprintf(e->cm_folder, sizeof e->cm_folder, "%s", n->sname);
        cm_add(e, CM_FD_NEW, "New Connection Here\xE2\x80\xA6", -1, false);
        cm_add(e, CM_FD_RENAME, "Rename Folder\xE2\x80\xA6", -1, false);
        cm_add(e, CM_FD_REMOVE, "Remove Folder (keeps its connections)", -1, false);
        cm_add(e, CM_SEP, "", -1, true);
        cm_add(e, CM_FD_EXPAND, "Expand All Folders", -1, false);
        cm_add(e, CM_FD_COLLAPSE, "Collapse All Folders", -1, false);
        return;
    }
    Connection *c = n ? n->conn : NULL;
    NsState *s = c && c->db ? nsx_find(c->id) : NULL;
    ObjKey ok;
    if (n && c && c->db && node_objkey(n, &ok)) { objed_object_menu(a, c, &ok, x, y); return; }   /* OBJ-059 */
    if (n && n->objgroup && c->db) { objed_group_menu(a, c, n->schema, x, y); return; }
    if (n && c && c->db && n->kind == NODE_TABLE && !(n->table && n->table->is_view)) {   /* TBL-083 */
        cm_reset(e, c, x, y);
        table_target(n, e->cm_schema, sizeof e->cm_schema, e->cm_table, sizeof e->cm_table);
        bool td = tdes_supported(c), ro = c->read_only;
        cm_add(e, CM_TB_MODIFY, ro ? "Modify Table... (read-only connection: view only)" : "Modify Table...", -1, !td);
        cm_add(e, CM_TB_DATA, "Edit Data", -1, false);
        cm_add(e, CM_TB_DDL, "Generate SQL > CREATE", -1, !td);
        cm_add(e, CM_TB_DUP, "Duplicate Structure...", -1, !td || ro);
        cm_add(e, CM_TB_DEPS, "Dependencies...", -1, !td);
        cm_add(e, CM_SEP, "", -1, true);
        if (e->nmsel > 1) { char lb[64]; snprintf(lb, sizeof lb, "Show ER Diagram (%d tables)", e->nmsel); cm_add(e, CM_TB_ERD, lb, -1, false); }
        else cm_add(e, CM_TB_ERD, "Show ER Diagram", -1, false);
        if (dgmview_has_diagram_for(a, c->id)) cm_add(e, CM_TB_ERD_ADD, "Add to Current Diagram", -1, false);
        cm_add(e, CM_SEP, "", -1, true);
        cm_add(e, CM_TB_DROP, "Drop Table...", -1, !td || ro);
        return;
    }
    if (s && n->role == NR_ROOT) {
        cm_reset(e, c, x, y);
        bool dbs = !strcmp(s->ad->label, "Databases");
        cm_add(e, CM_NS_SEARCH, dbs ? "Search Databases\xE2\x80\xA6" : "Search Schemas\xE2\x80\xA6", -1, !s->ad->caps.server_search);
        cm_add(e, CM_NS_REFRESH, "Refresh", -1, false);
        cm_add(e, CM_NS_OPTIONS, "View Options\xE2\x80\xA6", -1, false);
        if (s->prefs.mode == NSM_ALL) cm_add(e, CM_NS_COMPACT, "Compact View", -1, false);
        else cm_add(e, CM_NS_SHOWALL, dbs ? "Show All Databases" : "Show All Schemas", -1, false);
        char b[64]; snprintf(b, sizeof b, "Copy Loaded Names (%d)", s->nfeatured + nsx_others_names(s, NULL, 0));
        cm_add(e, CM_NS_COPY, b, -1, false);
        cm_add(e, CM_NS_PROPS, "Properties", -1, false);
        return;
    }
    if (s && n->role == NR_OTHERS) {
        cm_reset(e, c, x, y);
        cm_add(e, CM_OT_SEARCH, "Search in Others\xE2\x80\xA6", -1, !s->ad->caps.server_search);
        cm_add(e, CM_OT_MORE, s->pager.started ? "Load More" : "Load First Page", -1, s->pager.loading || (s->pager.started && !s->pager.has_more));
        cm_add(e, CM_OT_REFRESH, "Refresh Others", -1, !s->pager.started);
        cm_add(e, CM_NS_SHOWALL, "Show All Schemas", -1, false);
        cm_add(e, CM_NS_OPTIONS, "View Options\xE2\x80\xA6", -1, false);
        return;   /* never a drop / DDL on Others (DBX-105.5) */
    }
    if (s && n->kind == NODE_SCHEMA) {
        cm_reset(e, c, x, y);
        snprintf(e->cm_schema, sizeof e->cm_schema, "%s", n->sname);
        cm_add(e, CM_SC_CONSOLE, "Open SQL Console", -1, false);
        cm_add(e, CM_NEW_TABLE, "New > Table...", -1, !tdes_supported(c) || c->read_only);   /* TBL-001 / 082 */
        cm_add(e, CM_SC_ERD, "New ER Diagram\xE2\x80\xA6", -1, false);   /* DGM-002 */
        cm_add(e, CM_SC_EXPAND, n->expanded ? "Collapse" : "Expand", -1, false);
        cm_add(e, CM_SC_FIND, "Find Objects\xE2\x80\xA6", -1, !s->ad->caps.object_search);
        cm_add(e, CM_SC_REFRESH, "Refresh", -1, false);
        cm_add(e, CM_SC_VERIFY, "Verify Visible Objects", -1, !s->ad->caps.visibility_probe);
        cm_add(e, CM_SEP, "", -1, true);
        nsx_featured(s);
        int f = nsx_featured_flags(s, n->sname);
        if (!f) cm_add(e, CM_SC_PIN, dbs_label(s), -1, false);
        else if (!nsx_can_unpin(s, n->sname)) cm_add(e, CM_SC_UNPIN, "Move to Others (not for the current schema)", -1, true);
        else cm_add(e, CM_SC_UNPIN, (f & NSF_PINNED) && f == NSF_PINNED ? "Unpin (Move to Others)" : "Move to Others", -1, false);
        cm_add(e, CM_SC_COPY, "Copy Qualified Name", -1, false);
        cm_add(e, CM_SC_PROPS, "Properties", -1, false);
        return;
    }
    cm_open(e, c, x, y);
}

/* Menu rectangle, shifted to stay inside the window. */
static Rect cm_rect(Explorer *e, App *a) {
    float w = px(a, 190), ih = px(a, 26), pad = px(a, 6);
    for (int i = 0; i < e->cm_n; i++) { float tw = font_width(a->font_ui, e->cm_text[i], -1) + px(a, 52); if (tw > w) w = tw; }
    if (w > px(a, 520)) w = px(a, 520);
    if (w > a->width - px(a, 16)) w = a->width - px(a, 16);
    float h = pad * 2;
    for (int i = 0; i < e->cm_n; i++) h += e->cm_cmd[i] == CM_SEP ? px(a, 9) : ih;
    float x = e->cm_x, y = e->cm_y;
    if (x + w > a->width - px(a, 8)) x = a->width - px(a, 8) - w;
    if (y + h > a->height - px(a, 8)) y = a->height - px(a, 8) - h;
    if (y < 0) y = 0;
    return rect_make(x, y, w, h);
}
static Rect cm_item_rect(Explorer *e, App *a, Rect rc, int i) {
    float ih = px(a, 26), y = rc.y + px(a, 6);
    for (int k = 0; k < i; k++) y += e->cm_cmd[k] == CM_SEP ? px(a, 9) : ih;
    return rect_make(rc.x + px(a, 6), y, rc.w - px(a, 12), e->cm_cmd[i] == CM_SEP ? px(a, 9) : ih);
}

/* Index (into cm_cmd) of the enabled item under the mouse, -1 when none. */
static int cm_item_at(Explorer *e, App *a, float mx, float my) {
    Rect rc = cm_rect(e, a);
    for (int i = 0; i < e->cm_n; i++)
        if (!e->cm_dis[i] && rect_contains(cm_item_rect(e, a, rc, i), mx, my)) return i;
    return -1;
}

static const char *dbs_label(NsState *s) { return !strcmp(s->ad->label, "Databases") ? "Pin to Databases" : "Pin to Schemas"; }

static void hit_open(Explorer *e, App *a, int i, bool reveal_only);

/* Executes a context-menu command. */
static void cm_run(Explorer *e, App *a, int item) {
    Connection *c = e->cm_conn;
    NsState *s = c && c->db ? nsx_find(c->id) : NULL;
    char sc[128]; snprintf(sc, sizeof sc, "%s", e->cm_schema);
    float x = e->cm_x, y = e->cm_y;
    switch (item) {
    case CM_CONNECT: app_connect(a, c); break;
    case CM_DISCONNECT: app_disconnect(a, c); break;
    case CM_EDIT: e->cm_open = false; app_edit_connection(a, c); break;
    case CM_REFRESH: if (c->db) { objed_list_invalidate(c); conn_refresh(c); if (a->active == c) app_refresh_schema(a); if (s) nsx_refresh(s); explorer_rebuild(e, a); } break;
    case CM_NEW: e->cm_open = false; app_edit_connection(a, NULL); break;
    case CM_NEW_QUERY: if (c) {
            e->cm_open = false;
            if (c->db) { app_set_active(a, c); panels_new_editor_tab(a->panels, a, ""); }
            else { app_connect_async(a, c, false); int id = panels_open_tab(a->panels, a, "query.sql", c->id, ""); if (id > 0) panels_activate_tab(a->panels, a, id); }
            a->focus = FOCUS_EDITOR;
            return;
        }
        break;
    case CM_NEW_TABLE: if (c && c->db) {
            const char *sc = e->cm_schema[0] ? e->cm_schema : !strcmp(c->driver, "sqlite") ? "main" : !strcmp(c->driver, "postgres") ? (c->schema[0] ? c->schema : "public") : "";
            char sch[128]; snprintf(sch, sizeof sch, "%s", sc);
            if (!sch[0]) { NsState *ns = nsx_find(c->id); if (ns && ns->current[0]) snprintf(sch, sizeof sch, "%s", ns->current); }
            e->cm_open = false; tdes_new_table(a, c, sch); return;
        } break;
    case CM_TB_MODIFY: case CM_TB_DDL: case CM_TB_DUP: case CM_TB_DEPS: case CM_TB_DROP:
        if (c && c->db) { char sc[128], tb[256]; snprintf(sc, sizeof sc, "%s", e->cm_schema); snprintf(tb, sizeof tb, "%s", e->cm_table); e->cm_open = false;
            tdes_open_table(a, c, sc, tb, item == CM_TB_DDL ? 1 : item == CM_TB_DUP ? 2 : item == CM_TB_DROP ? 3 : 0); return; }
        break;
    case CM_TB_ERD: case CM_TB_ERD_ADD:
        if (c) { char sc[128], tb[256]; snprintf(sc, sizeof sc, "%s", e->cm_schema); snprintf(tb, sizeof tb, "%s", e->cm_table); e->cm_open = false;
            if (item == CM_TB_ERD && e->nmsel > 1) {   /* the tables picked with Cmd / Shift */
                char names[512][256]; int nn = 0;
                for (int q = 0; q < e->nmsel && nn < 512; q++) { char s2[128]; table_target(e->msel[q], s2, sizeof s2, names[nn], sizeof names[nn]); nn++; }
                const char *ptr[512]; for (int q = 0; q < nn; q++) ptr[q] = names[q];
                dgmview_open_tables(a, c, sc, ptr, nn);
            }
            else if (item == CM_TB_ERD) dgmview_open_table(a, c, sc, tb); else dgmview_add_to_current(a, c, sc, tb);
            return; }
        break;
    case CM_DUP_CONN: if (c) { e->cm_open = false; dialog_open_copy(a->dialog, a, c); return; } break;
    case CM_SC_ERD: if (c) { char sc[200]; snprintf(sc, sizeof sc, "%s", e->cm_schema); e->cm_open = false; dgmview_new_for_schema(a, c, sc); return; } break;
    case CM_TB_DATA: if (c && c->db) {   /* the data grid, not the structure (TBL-083.2) */
            char full[400]; snprintf(full, sizeof full, "%s", e->cm_table);
            for (int i = 0; i < c->ntables; i++) { const char *nm = c->tables[i].name, *dt = strchr(nm, '.'); if (!strcmp(dt ? dt + 1 : nm, e->cm_table)) { snprintf(full, sizeof full, "%s", nm); break; } }
            e->cm_open = false; preview_name(a, c, full, e->cm_table); return;
        } break;
    case CM_FD_NEW: e->cm_open = false; app_new_connection_in(a, e->cm_folder); return;
    case CM_FD_RENAME: {
        e->cm_open = false;
        const char *last = strrchr(e->cm_folder, '/');
        e->rn_open = true; snprintf(e->rn_path, sizeof e->rn_path, "%s", e->cm_folder);
        textfield_set(&e->rn, last ? last + 1 : e->cm_folder);
        a->focus = FOCUS_EXPLORER;
        return;
    }
    case CM_FD_REMOVE: {
        char up[128]; snprintf(up, sizeof up, "%s", e->cm_folder);
        char *sl = strrchr(up, '/'); if (sl) *sl = 0; else up[0] = 0;
        int moved = 0; for (int i = 0; a->conns && i < a->conns->n; i++) moved += in_folder(a->conns->items[i]->folder, e->cm_folder);
        char was[128]; snprintf(was, sizeof was, "%s", e->cm_folder);
        fo_move(a, was, up);
        app_connections_changed(a);
        app_status(a, "Folder %s removed: %d connection(s) moved to %s", was, moved, up[0] ? up : "the top level");
        e->cm_open = false; return;   /* folder menus have no connection: never reach the Schema Explorer cases below */
    }
    case CM_FD_EXPAND: case CM_FD_COLLAPSE: {
        bool open = item == CM_FD_EXPAND;
        int cap = 16 + e->nroots, sp = 0; Node **stack = malloc(sizeof *stack * (size_t)cap);
        for (int i = 0; stack && i < e->nroots; i++) stack[sp++] = e->roots[i];
        while (stack && sp) { Node *n = stack[--sp]; if (n->kind != NODE_FOLDER) continue; n->expanded = open; if (n->sname) fo_set(n->sname, open);
            for (int k = 0; k < n->nkids; k++) { if (sp == cap) { cap *= 2; Node **g = realloc(stack, sizeof *stack * (size_t)cap); if (!g) break; stack = g; } stack[sp++] = n->kids[k]; } }
        free(stack);
        e->cm_open = false; return;   /* folder menus have no connection: never reach the Schema Explorer cases below */
    }
    default: break;
    }
    if (!s) return;
    switch (item) {
    case CM_NS_SEARCH: case CM_OT_SEARCH: search_open(e, c, false, ""); break;
    case CM_NS_REFRESH: nsx_refresh(s); break;   /* highlights and the start of discovery only (DBX-104.2) */
    case CM_NS_OPTIONS: cm_options(e, c, x, y); return;
    case CM_NS_SHOWALL: nsx_set_mode(s, NSM_ALL); break;
    case CM_NS_COMPACT: nsx_set_mode(s, NSM_COMPACT); break;
    case CM_NS_COPY: {
        static char buf[1 << 20];
        int n = nsx_copy_loaded(s, buf, sizeof buf);
        SDL_SetClipboardText(buf);
        app_status(a, "%d loaded name%s copied (names not loaded yet are not included)", n, n == 1 ? "" : "s");
        break;
    }
    case CM_NS_PROPS: cm_props_root(e, c, x, y); return;
    case CM_OT_MORE: nsx_load_more(s); break;
    case CM_OT_REFRESH: nsx_refresh_others(s); break;
    case CM_SC_CONSOLE: open_console(a, c, sc); break;
    case CM_SC_EXPAND: { Node *cn = conn_node(e, c), *n = cn ? find_schema(cn, sc) : NULL; if (n) toggle(n); break; }
    case CM_SC_FIND: search_open(e, c, true, sc); break;
    case CM_SC_REFRESH: nsx_refresh_schema(s, sc); break;
    case CM_SC_PROPS: cm_props_schema(e, c, sc, x, y); return;
    case CM_SC_COPY: { char q[300]; c->db->drv->quote_ident(sc, q, sizeof q); copy_text(a, q, "Qualified name"); break; }
    case CM_SC_VERIFY: nsx_probe(s, sc); app_status(a, "Checking visible objects in %s\xE2\x80\xA6", sc); break;
    case CM_SC_PIN: nsx_pin(s, sc); explorer_rebuild(e, a); reveal_schema(e, a, c, sc, false); break;
    case CM_SC_UNPIN: nsx_unpin(s, sc); explorer_rebuild(e, a); reveal_schema(e, a, c, sc, false); break;   /* presentation only: no SQL, editors stay open (DBX-107.4) */
    case CM_OPT_COMPACT: nsx_set_mode(s, NSM_COMPACT); cm_options(e, c, x, y); return;
    case CM_OPT_ALL: nsx_set_mode(s, NSM_ALL); cm_options(e, c, x, y); return;
    case CM_OPT_PINNED: nsx_set_mode(s, NSM_PINNED); cm_options(e, c, x, y); return;
    case CM_OPT_SYSTEM: nsx_set_system(s, !s->prefs.show_system); cm_options(e, c, x, y); return;
    case CM_OPT_RECENT: nsx_set_option(s, "auto_recent", !s->prefs.auto_recent); cm_options(e, c, x, y); return;
    case CM_OPT_KNOWN: nsx_set_option(s, "auto_known", !s->prefs.auto_known); cm_options(e, c, x, y); return;
    case CM_OPT_CAP: { static const int caps_[] = { 0, 10, 20, 50 }; int k = 0; while (k < 4 && caps_[k] != s->prefs.auto_cap) k++; nsx_set_option(s, "auto_cap", caps_[(k + 1) % 4]); cm_options(e, c, x, y); return; }
    case CM_OPT_PAGE: { static const int ps[] = { 50, 100, 200, 500 }; int k = 0; while (k < 4 && ps[k] != s->prefs.page_size) k++; nsx_set_option(s, "page_size", ps[(k + 1) % 4]); cm_options(e, c, x, y); return; }
    case CM_OPT_KINDS: { static const int ks[] = { NSK_ALL, NSK_TABLES | NSK_VIEWS, NSK_ROUTINES, NSK_TABLES }; int k = 0; while (k < 4 && ks[k] != s->prefs.kinds) k++; nsx_set_kinds(s, ks[(k + 1) % 4]); cm_options(e, c, x, y); return; }
    case CM_OPT_DISCOVER: nsx_set_option(s, "discover", !s->prefs.discover); cm_options(e, c, x, y); return;
    case CM_OPT_RESET: nsx_reset_prefs(s); app_status(a, "Schema tree preferences reset for %s (connection settings kept)", c->name); cm_options(e, c, x, y); return;
    case CM_HIT_OPEN: hit_open(e, a, e->cm_hit, false); break;
    case CM_HIT_REVEAL: hit_open(e, a, e->cm_hit, true); break;
    case CM_HIT_PIN: if (e->cm_hit >= 0 && e->cm_hit < s->search.nhits) { char nm[128]; snprintf(nm, sizeof nm, "%s", s->search.hits[e->cm_hit].schema); nsx_pin(s, nm); app_status(a, "%s pinned to %s", nm, s->ad->label); } break;
    case CM_HIT_COPY: if (e->cm_hit >= 0 && e->cm_hit < s->search.nhits) { NsHit *h = &s->search.hits[e->cm_hit]; char t[300]; snprintf(t, sizeof t, "%s%s%s", h->schema, h->name[0] ? "." : "", h->name); copy_text(a, t, "Name"); } break;
    case CM_HIT_PROPS: if (e->cm_hit >= 0 && e->cm_hit < s->search.nhits) { char nm[128]; snprintf(nm, sizeof nm, "%s", s->search.hits[e->cm_hit].schema); cm_props_schema(e, c, nm, x, y); return; } break;
    }
    e->cm_open = false;
}

/* Input for the open context menu: Escape or any click outside closes it, a left click on an
 * item runs it; Up / Down / Enter work too. While open it swallows the frame's key events. */
static void cm_update(Explorer *e, App *a) {
    Input *in = &a->in;
    if (!e->cm_open) return;
    if (input_key(in, SDLK_ESCAPE, 0)) e->cm_open = false;
    unsigned mods;
    if (input_key_any(in, SDLK_DOWN, &mods)) { for (int k = 1; k <= e->cm_n; k++) { int i = (e->cm_sel + k + e->cm_n) % e->cm_n; if (!e->cm_dis[i]) { e->cm_sel = i; break; } } }
    if (input_key_any(in, SDLK_UP, &mods)) { for (int k = 1; k <= e->cm_n; k++) { int i = ((e->cm_sel < 0 ? 0 : e->cm_sel) - k + 2 * e->cm_n) % e->cm_n; if (!e->cm_dis[i]) { e->cm_sel = i; break; } } }
    if ((input_key_any(in, SDLK_RETURN, &mods) || input_key_any(in, SDLK_KP_ENTER, &mods)) && e->cm_sel >= 0 && e->cm_open) { in->nkeys = 0; cm_run(e, a, e->cm_cmd[e->cm_sel]); return; }
    int hover = cm_item_at(e, a, in->mouse_x, in->mouse_y);
    if (hover >= 0) { a->want_cursor = CURSOR_HAND; e->cm_sel = hover; }
    Rect rc = cm_rect(e, a);
    for (int i = 0; i < e->cm_n; i++) if (!e->cm_dis[i]) ui_record(e->cm_text[i], cm_item_rect(e, a, rc, i));   /* harness: "button Pin to Schemas" */
    if (in->mouse_pressed || in->mouse_rpressed) {
        in->mouse_consumed = true;
        bool inside = rect_contains(rc, in->mouse_x, in->mouse_y);
        if (hover >= 0 && in->mouse_pressed) { cm_run(e, a, e->cm_cmd[hover]); }
        else if (!inside) e->cm_open = false;
    }
    in->nkeys = 0;
}

/* See explorer.h. Soft shadow, border, background, then the items with hover highlight. */
void explorer_draw_overlay(Explorer *e, App *a) {
    if (!e->cm_open) return;
    SDL_Renderer *r = a->renderer;
    Rect rc = cm_rect(e, a);
    for (int s = 3; s >= 1; s--)
        draw_rrect(r, rect_make(rc.x - s * px(a, 2), rc.y + px(a, 2), rc.w + s * px(a, 4), rc.h + s * px(a, 3)), px(a, 8) + s * px(a, 2), color_alpha(color_hex(0x000000), 28));
    draw_rrect(r, rc, px(a, 8), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, 7), color_hex(T_PANEL_ALT));
    for (int i = 0; i < e->cm_n; i++) {
        Rect ir = cm_item_rect(e, a, rc, i);
        if (e->cm_cmd[i] == CM_SEP) { draw_rect(r, rect_make(ir.x + px(a, 4), ir.y + ir.h / 2, ir.w - px(a, 8), px(a, 1)), color_hex(T_BORDER)); continue; }
        bool hot = i == e->cm_sel && !e->cm_dis[i];
        if (hot) draw_rrect(r, ir, px(a, M_RADIUS), color_hex(T_ACCENT));
        float tx = ir.x + px(a, 10);
        if (e->cm_check[i] >= 0) {   /* a check box drawn as shapes (the UI font has no check glyph) */
            float bs = px(a, 10); Rect bx = rect_make(tx, ir.y + (ir.h - bs) / 2, bs, bs);
            draw_rrect(r, bx, px(a, 2), color_hex(hot ? 0xFFFFFF : T_TEXT_MUTED));
            draw_rrect(r, rect_inset(bx, px(a, 1.5f), px(a, 1.5f)), px(a, 1.5f), color_hex(hot ? T_ACCENT : T_PANEL_ALT));
            if (e->cm_check[i]) draw_rrect(r, rect_inset(bx, px(a, 3), px(a, 3)), px(a, 1), color_hex(hot ? 0xFFFFFF : T_ACCENT));
            tx += bs + px(a, 8);
        }
        uint32_t col = e->cm_cmd[i] == CM_INFO ? T_TEXT_MUTED : e->cm_dis[i] ? T_TEXT_DIM : hot ? 0xFFFFFF : T_TEXT;
        draw_text_fit(e->cm_cmd[i] == CM_INFO ? a->font_small : a->font_ui, r, e->cm_text[i], -1, rect_make(tx, ir.y, ir.x + ir.w - tx - px(a, 6), ir.h), color_hex(col), ALIGN_LEFT);
    }
}

/* ---- search bar: schemas or objects, server side (DBX-085 / 087 / 106 / 109) ---- */

static void search_open(Explorer *e, Connection *c, bool objects, const char *ns) {
    e->search_open = true; e->search_conn = c; e->search_objects = objects; e->search_sub = false;
    snprintf(e->search_ns, sizeof e->search_ns, "%s", ns ? ns : "");
    textfield_set(&e->sf, "");
    e->search_last[0] = 0; e->search_sel = 0; e->search_scroll = 0;
    NsState *s = c ? nsx_find(c->id) : NULL;
    if (s) nsx_search_clear(s);
    if (E_APP) E_APP->focus = FOCUS_EXPLORER;
}
static void search_close(Explorer *e) {
    NsState *s = e->search_conn ? nsx_find(e->search_conn->id) : NULL;
    if (s) nsx_search_clear(s);
    e->search_open = false; e->search_conn = NULL;
}
static int kind_of_type(const char *t) {
    if (!strcmp(t, "VIEW")) return OBJK_VIEW;
    if (!strcmp(t, "MATERIALIZED VIEW")) return OBJK_MATVIEW;
    if (!strcmp(t, "FUNCTION") || strstr(t, "FUNCTION")) return OBJK_FUNCTION;
    if (!strcmp(t, "PROCEDURE") || strstr(t, "PROCEDURE")) return OBJK_PROCEDURE;
    if (!strcmp(t, "PACKAGE")) return OBJK_PKG_SPEC;
    if (!strcmp(t, "TRIGGER")) return OBJK_TRIGGER;
    return -1;
}
/* opens a hit: a schema is revealed (and opened); a table previews; a view / routine opens its
 * definition; anything else reveals its schema. Loads only the path it needs (DBX-087.3). */
static void hit_open(Explorer *e, App *a, int i, bool reveal_only) {
    Connection *c = e->search_conn ? e->search_conn : e->cm_conn;
    NsState *s = c ? nsx_find(c->id) : NULL;
    if (!s || i < 0 || i >= s->search.nhits) return;
    NsHit h = s->search.hits[i];
    if (!h.name[0] || reveal_only) { search_close(e); reveal_schema(e, a, c, h.schema, !reveal_only); if (!reveal_only) nsx_note_recent(s, h.schema); return; }
    if (!strcmp(h.type, "TABLE") || strstr(h.type, "TABLE")) {
        char q[300]; snprintf(q, sizeof q, "%s.%s", h.schema, h.name);
        if (!strcmp(c->driver, "postgres") && !strcmp(h.schema, "public")) snprintf(q, sizeof q, "%s", h.name);
        preview_name(a, c, q, h.name);
        return;
    }
    int k = kind_of_type(h.type);
    if (k == OBJK_VIEW || k == OBJK_MATVIEW || k == OBJK_PKG_SPEC || k == OBJK_TRIGGER || ((k == OBJK_FUNCTION || k == OBJK_PROCEDURE) && strcmp(c->driver, "postgres"))) {
        ObjKey key; memset(&key, 0, sizeof key);
        key.kind = k; snprintf(key.schema, sizeof key.schema, "%s", h.schema); snprintf(key.name, sizeof key.name, "%s", h.name); snprintf(key.conn_id, sizeof key.conn_id, "%s", c->id);
        if (objed_open(a, c, &key)) return;
    }
    search_close(e); reveal_schema(e, a, c, h.schema, true);   /* PostgreSQL overloads: pick it in the schema's Objects */
}

/* the search bar and its result rows inside `b`; returns the rows area left for the tree */
static void search_update(Explorer *e, App *a, Rect b, bool focused) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    Connection *c = e->search_conn;
    NsState *s = c && c->db ? nsx_find(c->id) : NULL;
    if (!s) { e->search_open = false; return; }
    float row_h = px(a, M_ROW);
    Rect bar = rect_make(b.x + px(a, 8), b.y + px(a, 4), b.w - px(a, 16), px(a, 26));
    float bw = px(a, 96), xw = px(a, 22);
    char ph[200];
    if (e->search_objects) snprintf(ph, sizeof ph, "Find objects%s%s (prefix)", e->search_ns[0] ? " in " : "", e->search_ns);
    else snprintf(ph, sizeof ph, "Search %s by name (prefix)", !strcmp(s->ad->label, "Databases") ? "databases" : "schemas");
    ui_textfield(a, &e->sf, rect_make(bar.x, bar.y, bar.w - bw - xw - px(a, 8), bar.h), ph, focused);
    ui_record("explorer search", bar);
    Rect tb = rect_make(bar.x + bar.w - bw - xw - px(a, 4), bar.y, bw, bar.h);
    if (ui_checkbox(a, tb, "Contains", e->search_sub, true)) { e->search_sub = !e->search_sub; e->search_last[0] = 1; }   /* a substring is an explicit, costlier request */
    Rect xb = rect_make(bar.x + bar.w - xw, bar.y + px(a, 2), xw, bar.h - px(a, 4));
    if (ui_button(a, xb, "x", BTN_QUIET, true)) { search_close(e); return; }
    /* text changed: a new request, debounced in the model */
    if (strcmp(e->sf.buf, e->search_last)) {
        snprintf(e->search_last, sizeof e->search_last, "%s", e->sf.buf);
        nsx_search(s, e->sf.buf, e->search_sub, e->search_objects, e->search_ns, SDL_GetTicks());
        e->search_sel = 0; e->search_scroll = 0;
    }
    NsSearch *q = &s->search;
    Rect rows = rect_make(b.x, bar.y + bar.h + px(a, 4), b.w, b.y + b.h - (bar.y + bar.h + px(a, 4)));
    e->search_rows = rows;
    char st[260];
    if (!e->sf.buf[0]) snprintf(st, sizeof st, "Type a name: the server is searched, including names never loaded");
    else if (q->pending || q->loading) snprintf(st, sizeof st, "Searching\xE2\x80\xA6");
    else if (q->failed) snprintf(st, sizeof st, "Search failed: %s", q->error);
    else if (q->partial) snprintf(st, sizeof st, "First %d matches: type more to narrow", q->nhits);
    else if (q->done) snprintf(st, sizeof st, "%d match%s%s", q->nhits, q->nhits == 1 ? "" : "es", e->search_objects ? "" : " (system schemas marked)");
    else st[0] = 0;
    draw_text_fit(a->font_small, r, st, -1, rect_make(rows.x + px(a, 14), rows.y, rows.w - px(a, 28), row_h), color_hex(q->failed ? T_RED : T_TEXT_DIM), ALIGN_LEFT);
    Rect list = rect_make(rows.x, rows.y + row_h, rows.w, rows.h - row_h);
    int n = q->done ? q->nhits : 0;
    /* keys */
    if (focused) {
        unsigned mods;
        if (input_key_any(in, SDLK_ESCAPE, &mods)) { search_close(e); return; }
        if (input_key_any(in, SDLK_DOWN, &mods) && n) e->search_sel = e->search_sel < n - 1 ? e->search_sel + 1 : n - 1;
        if (input_key_any(in, SDLK_UP, &mods) && n) e->search_sel = e->search_sel > 0 ? e->search_sel - 1 : 0;
        if ((input_key_any(in, SDLK_RETURN, &mods) || input_key_any(in, SDLK_KP_ENTER, &mods)) && n) { hit_open(e, a, e->search_sel, false); return; }
        float top = e->search_sel * row_h;
        if (top < e->search_scroll) e->search_scroll = top;
        if (top + row_h > e->search_scroll + list.h) e->search_scroll = top + row_h - list.h;
    }
    bool hov = rect_contains(list, in->mouse_x, in->mouse_y);
    if (hov && in->wheel_y != 0) e->search_scroll -= in->wheel_y;
    float maxs = fmaxf(0, n * row_h - list.h);
    e->search_scroll = clampf(e->search_scroll, 0, maxs);
    int hover = -1;
    if (hov) { int i = (int)floorf((in->mouse_y - list.y + e->search_scroll) / row_h); if (i >= 0 && i < n) hover = i; }
    if (hover >= 0 && (in->mouse_pressed || in->mouse_rpressed) && !in->mouse_consumed) {
        in->mouse_consumed = true; a->focus = FOCUS_EXPLORER;
        e->search_sel = hover;
        if (in->mouse_rpressed) {
            cm_reset(e, c, in->mouse_x, in->mouse_y);
            e->cm_hit = hover;
            NsHit *h = &q->hits[hover];
            snprintf(e->cm_schema, sizeof e->cm_schema, "%s", h->schema);
            if (h->name[0]) { cm_add(e, CM_HIT_OPEN, "Open", -1, false); cm_add(e, CM_HIT_REVEAL, "Reveal Schema in Explorer", -1, false); }
            else { cm_add(e, CM_HIT_OPEN, "Open Schema", -1, false); cm_add(e, CM_HIT_REVEAL, "Reveal in Explorer", -1, false); }
            nsx_featured(s);
            cm_add(e, CM_HIT_PIN, dbs_label(s), -1, nsx_is_featured(s, h->schema));
            cm_add(e, CM_HIT_COPY, h->name[0] ? "Copy Name" : "Copy Schema Name", -1, false);
            cm_add(e, CM_HIT_PROPS, "Properties", -1, false);
        } else if (in->clicks >= 2) { hit_open(e, a, hover, false); return; }
    }
    clip_push(r, list);
    nsx_featured(s);
    int first = (int)floorf(e->search_scroll / row_h), last = (int)ceilf((e->search_scroll + list.h) / row_h);
    for (int i = first < 0 ? 0 : first; i < last && i < n; i++) {   /* visible rows only */
        NsHit *h = &q->hits[i];
        Rect rr = rect_make(list.x, list.y + i * row_h - e->search_scroll, list.w, row_h);
        bool sel = i == e->search_sel;
        if (sel) draw_rect(r, rr, color_hex(focused ? T_SELECTED : T_HOVER)); else if (i == hover) draw_rect(r, rr, color_hex(T_HOVER));
        char lbl[300], det[96] = "";
        if (h->name[0]) { snprintf(lbl, sizeof lbl, "%s", h->name); snprintf(det, sizeof det, "%s \xC2\xB7 %s", h->type, h->schema); }
        else {
            snprintf(lbl, sizeof lbl, "%s", h->schema);
            int f = nsx_featured_flags(s, h->schema);
            snprintf(det, sizeof det, "%s%s%s", f ? "highlighted" : "", f && h->system ? " \xC2\xB7 " : "", h->system ? "system" : "");
        }
        ui_record(lbl, rr);
        float dw = det[0] ? font_width(a->font_small, det, -1) + px(a, 12) : 0;
        draw_text_fit(a->font_small, r, det, -1, rect_make(rr.x + rr.w - dw - px(a, 6), rr.y, dw, rr.h), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
        draw_text_fit(a->font_ui, r, lbl, -1, rect_make(rr.x + px(a, 22), rr.y, rr.w - dw - px(a, 34), rr.h), color_hex(T_TEXT), ALIGN_LEFT);
    }
    clip_pop(r);
}

/* ---- panel ------------------------------------------------------------- */

/* See explorer.h. Order: catalog reads, context menu input, header and "+" button, keyboard,
 * scrolling, mouse clicks (which may rebuild the tree), then drawing of the visible rows and
 * the scrollbar. */
void explorer_update(Explorer *e, App *a, Rect b) {
    E_APP = a;
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    bool focused = a->focus == FOCUS_EXPLORER;

    ns_pump(a);
    if (ns_signature(a) != e->ns_sig) explorer_rebuild(e, a);   /* a page / answer arrived: only the model's rows change */

    cm_update(e, a);
    draw_rect(r, b, color_hex(T_PANEL));

    /* Header with the "+" button */
    Rect hdr = rect_make(b.x, b.y, b.w, px(a, M_HEADER));
    draw_text_fit(a->font_small, r, "CONNECTIONS", -1, rect_make(hdr.x + px(a, 14), hdr.y, hdr.w - px(a, 50), hdr.h),
                  color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    Rect plus = rect_make(hdr.x + hdr.w - px(a, 32), hdr.y + px(a, 5), px(a, 20), px(a, 20));
    e->plus_rect = plus;
    bool plus_hover = rect_contains(plus, in->mouse_x, in->mouse_y);
    if (plus_hover) { a->want_cursor = CURSOR_HAND; draw_rrect(r, plus, px(a, 4), color_hex(T_HOVER)); }
    {
        float cx = plus.x + plus.w / 2, cy = plus.y + plus.h / 2, s = px(a, 4);
        Color c = color_hex(plus_hover ? T_TEXT : T_TEXT_MUTED);
        draw_rect(r, rect_make(cx - s, cy - px(a, 0.75f), s * 2, px(a, 1.5f)), c);
        draw_rect(r, rect_make(cx - px(a, 0.75f), cy - s, px(a, 1.5f), s * 2), c);
    }
    if (plus_hover && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; app_edit_connection(a, NULL); }

    Rect body = rect_make(b.x, hdr.y + hdr.h, b.w, b.h - hdr.h);
    if (e->search_open) {
        search_update(e, a, body, focused);
        if (e->search_open) return;   /* the results replace the tree while searching */
        explorer_rebuild(e, a);
    }
    Rect rows = body;
    e->rows_rect = rows;

    if (e->rn_open) {   /* inline folder rename: Enter applies, Escape cancels (the row draws the field) */
        unsigned m;
        if (input_key_any(in, SDLK_ESCAPE, &m)) e->rn_open = false;
        else if (input_key_any(in, SDLK_RETURN, &m) || input_key_any(in, SDLK_KP_ENTER, &m)) {
            e->rn_open = false;
            char nm[128]; snprintf(nm, sizeof nm, "%s", e->rn.buf); conn_folder_normalize(nm, sizeof nm);
            char to[256]; snprintf(to, sizeof to, "%s", e->rn_path);
            char *sl = strrchr(to, '/'); if (sl) sl[1] = 0; else to[0] = 0;   /* the parent, with its "/" */
            size_t tl = strlen(to); snprintf(to + tl, sizeof to - tl, "%s", nm);
            conn_folder_normalize(to, sizeof to);
            if (nm[0] && strcmp(to, e->rn_path)) {
                bool was_open = fo_is_open(e->rn_path);
                fo_move(a, e->rn_path, to);
                if (was_open) fo_set(to, true);
                app_connections_changed(a);
                app_status(a, "Folder renamed to %s", to);
            }
        }
    }
    else if (focused && in->nkeys) handle_keys(e, a);
    if (e->search_open) return;

    e->nvis = 0;
    for (int i = 0; i < e->nroots; i++) flatten(e, e->roots[i]);

    /* Scrolling: wheel moves the target, the offset eases towards it */
    float row_h = px(a, M_ROW);
    float content_h = e->nvis * row_h + px(a, 8);
    float max_scroll = fmaxf(0, content_h - rows.h);

    bool hover_rows = rect_contains(rows, in->mouse_x, in->mouse_y);
    if (hover_rows && in->wheel_y != 0) e->scroll_t -= in->wheel_y;
    e->scroll_t = clampf(e->scroll_t, 0, max_scroll);
    if (ui_scrollbar_input(a, &e->sb, rows, content_h, &e->scroll_t) && e->sb.drag) e->scroll = e->scroll_t;   /* dragging follows the pointer at once */
    e->scroll = a->reduce_motion ? e->scroll_t : lerpf(e->scroll, e->scroll_t, 0.4f);   /* PNL-039 */
    if (fabsf(e->scroll - e->scroll_t) > 0.5f) a->animating = true; else e->scroll = e->scroll_t;

    int hover_idx = -1;
    if (hover_rows) {
        int i = (int)floorf((in->mouse_y - rows.y + e->scroll) / row_h);
        if (i >= 0 && i < e->nvis) hover_idx = i;
    }

    /* Right click: select the row and open its context menu */
    if (hover_rows && in->mouse_rpressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        a->focus = FOCUS_EXPLORER;
        Node *n = hover_idx >= 0 ? e->vis[hover_idx] : NULL;
        bool in_set = false; for (int q = 0; n && q < e->nmsel; q++) if (e->msel[q] == n) in_set = true;
        if (n && !in_set) e->nmsel = 0;
        if (n) e->selected = n;
        if (n) cm_open_node(e, a, n, in->mouse_x, in->mouse_y);
        else cm_open(e, NULL, in->mouse_x, in->mouse_y);
    }
    /* Left click: double click acts (preview, insert column name, connect / edit with Alt);
     * single click on a chevron or group toggles; on a connection it only selects it: the query
     * tab's execution target changes by an explicit action only (double click, the toolbar's
     * datasource menu), never by the Explorer's selection (toolbar spec, section 9) */
    if (hover_rows && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        a->focus = FOCUS_EXPLORER;
        if (hover_idx >= 0) {
            Node *n = e->vis[hover_idx];
            bool on_rename = e->rn_open && n->kind == NODE_FOLDER && n->sname && !strcmp(n->sname, e->rn_path);
            if (e->rn_open && !on_rename) e->rn_open = false;   /* a click elsewhere cancels the rename */
            if (on_rename) { in->mouse_consumed = false; goto rows_done; }   /* the field takes the click */
            SDL_Keymod km = app_modstate(a);
            bool add = (km & (SDL_KMOD_GUI | SDL_KMOD_CTRL)) != 0, range = (km & SDL_KMOD_SHIFT) != 0;
            if (n->kind == NODE_TABLE && (add || range)) {   /* several tables: Cmd toggles one, Shift selects from the last one clicked */
                if (!e->nmsel && e->selected && e->selected->kind == NODE_TABLE && e->selected->conn == n->conn) e->msel[e->nmsel++] = e->selected;
                if (range && e->anchor && e->anchor->kind == NODE_TABLE && e->anchor->conn == n->conn) {
                    int i0 = vis_index(e, e->anchor), i1 = hover_idx;
                    if (i0 > i1) { int t = i0; i0 = i1; i1 = t; }
                    if (!add) e->nmsel = 0;
                    for (int i = i0; i >= 0 && i <= i1 && e->nmsel < 512; i++) {
                        Node *m = e->vis[i]; if (m->kind != NODE_TABLE || m->conn != n->conn) continue;
                        bool have = false; for (int q = 0; q < e->nmsel; q++) if (e->msel[q] == m) have = true;
                        if (!have) e->msel[e->nmsel++] = m;
                    }
                } else {
                    int at = -1; for (int q = 0; q < e->nmsel; q++) if (e->msel[q] == n) at = q;
                    if (at >= 0) { memmove(&e->msel[at], &e->msel[at + 1], sizeof e->msel[0] * (size_t)(e->nmsel - at - 1)); e->nmsel--; }
                    else if (e->nmsel < 512) { for (int q = 0; q < e->nmsel; q++) if (e->msel[q]->conn != n->conn) { e->nmsel = 0; break; } e->msel[e->nmsel++] = n; }
                    e->anchor = n;
                }
                e->selected = n;
                return;
            }
            e->nmsel = 0; e->anchor = n;
            e->selected = n;
            float chevron_x = rows.x + px(a, 10 + 14 * n->depth);
            bool on_chevron = in->mouse_x < chevron_x + px(a, 16);
            if (n->kind == NODE_ACTION) run_action(e, a, n);
            else if (n->kind == NODE_FOLDER) { if (in->clicks < 2) toggle(n); }   /* one click opens / closes; the double click's second press does nothing */
            else if (in->clicks >= 2) {
                if (n->kind == NODE_TABLE) preview_table(a, n);
                else if (n->kind == NODE_OBJECT && n->okey) { ObjKey k = *n->okey; objed_open(a, n->conn, &k); }
                else if (n->kind == NODE_COLUMN && n->parent && n->parent->kind == NODE_TABLE) app_editor_insert(a, n->label);
                else if (n->kind == NODE_CONN) {
                    if (app_modstate(a) & SDL_KMOD_ALT) app_edit_connection(a, n->conn);
                    else activate_conn(a, n->conn);
                } else toggle(n);
            } else if (n->kind == NODE_CONN) {
                if (on_chevron && n->conn->db) toggle(n);
            } else if (on_chevron || n->kind == NODE_GROUP) {
                toggle(n);
            }
        }
    }

rows_done:
    /* a click above may have rebuilt the tree (switching connection, connect, preview, DDL):
     * flatten again so nothing below touches a freed node */
    e->nvis = 0;
    for (int i = 0; i < e->nroots; i++) flatten(e, e->roots[i]);
    if (hover_idx >= e->nvis) hover_idx = -1;
    content_h = e->nvis * row_h + px(a, 8);
    max_scroll = fmaxf(0, content_h - rows.h);

    clip_push(r, rows);
    if (!e->nroots) {
        Rect m = rect_make(rows.x + px(a, 14), rows.y + px(a, 8), rows.w - px(a, 28), row_h);
        draw_text_fit(a->font_ui, r, "No connections", -1, m, color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        m.y += row_h;
        draw_text_fit(a->font_small, r, "Click + or press \xE2\x87\xA7\xE2\x8C\x98N to add one", -1, m, color_hex(T_TEXT_DIM), ALIGN_LEFT);
    }
    /* Visible rows only (DBX-088): background, chevron, icon, detail on the right, label */
    int first = (int)floorf(e->scroll / row_h);
    int last = (int)ceilf((e->scroll + rows.h) / row_h);
    for (int i = first; i < last && i < e->nvis; i++) {
        if (i < 0) continue;
        Node *n = e->vis[i];
        Rect rr = rect_make(rows.x, rows.y + i * row_h - e->scroll, rows.w, row_h);
        bool selected = n == e->selected;
        for (int q = 0; q < e->nmsel && !selected; q++) if (e->msel[q] == n) selected = true;
        if (selected) draw_rect(r, rr, color_hex(focused ? T_SELECTED : T_HOVER));
        else if (i == hover_idx) draw_rect(r, rr, color_hex(T_HOVER));
        if (selected && focused) draw_rect(r, rect_make(rr.x, rr.y, px(a, 2), rr.h), color_hex(T_ACCENT));

        float x = rr.x + px(a, 10 + 14 * n->depth);
        float cy = rr.y + rr.h / 2;
        if (!n->leaf)
            draw_chevron(r, x + px(a, 6), cy, px(a, 7), px(a, 1.5f), n->expanded, color_hex(T_TEXT_MUTED));
        x += px(a, 18);
        float is = px(a, 14);
        Color icon = color_hex(n->kind == NODE_COLUMN ? T_TEXT_DIM : T_TEXT_MUTED);
        Color bg = color_hex(selected ? (focused ? T_SELECTED : T_HOVER) : (i == hover_idx ? T_HOVER : T_PANEL));
        switch (n->kind) {
        case NODE_CONN: {
            const DriverInfo *di = conn_driver(n->conn);
            bool on = n->conn->db != NULL;
            draw_icon_db(r, x + is / 2, cy, is, color_hex(on ? (di ? di->color : T_ACCENT) : T_TEXT_DIM), bg);
            break;
        }
        case NODE_TABLE:
            if (n->table && n->table->is_view) draw_icon_view(r, x + is / 2, cy, is, color_hex(T_GREEN), bg);
            else draw_icon_table(r, x + is / 2, cy, is, icon, color_hex(T_ACCENT_DIM), bg);
            break;
        case NODE_OBJECT:
            draw_icon_view(r, x + is / 2, cy, is, color_hex(n->detail && !strncmp(n->detail, "INVALID", 7) ? T_RED : T_ACCENT), bg);
            break;
        case NODE_COLUMN:
            if (n->pk) draw_icon_key(r, x + is / 2 - px(a, 2), cy - px(a, 2), is * 0.9f, color_hex(T_YELLOW));
            else draw_icon_column(r, x + is / 2, cy, is, icon);
            break;
        case NODE_SCHEMA: {   /* a namespace: a small folder drawn with shapes */
            Rect f = rect_make(x + px(a, 1), cy - is * 0.32f, is - px(a, 2), is * 0.64f);
            Color fc = color_hex(n->detail && strstr(n->detail, "not found") ? T_TEXT_DIM : T_ACCENT_DIM);
            draw_rrect(r, rect_make(f.x, f.y - px(a, 2), f.w * 0.45f, px(a, 4)), px(a, 1), fc);
            draw_rrect(r, f, px(a, 2), fc);
            if (n->detail && (strstr(n->detail, "current") || strstr(n->detail, "pinned"))) draw_rrect(r, rect_make(f.x + f.w - px(a, 4), f.y + px(a, 1), px(a, 3), px(a, 3)), px(a, 1.5f), color_hex(T_YELLOW));
            break;
        }
        case NODE_FOLDER: {   /* a folder of connections: outlined when closed, filled when open */
            Rect f = rect_make(x + px(a, 1), cy - is * 0.34f, is - px(a, 2), is * 0.68f);
            Color fc = color_hex(T_YELLOW);
            draw_rrect(r, rect_make(f.x, f.y - px(a, 2), f.w * 0.45f, px(a, 4)), px(a, 1), fc);
            draw_rrect(r, f, px(a, 2), fc);
            if (!n->expanded) draw_rrect(r, rect_inset(f, px(a, 1.5f), px(a, 1.5f)), px(a, 1.5f), bg);
            break;
        }
        default: break;
        }
        if (n->kind != NODE_GROUP && n->kind != NODE_ACTION) x += is + px(a, 8);
        else x += px(a, 2);

        float detail_w = 0;
        if (n->kind == NODE_CONN) {
            /* state dot + driver name on the right */
            bool on = n->conn->db != NULL;
            const char *d = n->detail;
            detail_w = font_width(a->font_small, d, -1) + px(a, 12);
            Rect dr = rect_make(rr.x + rr.w - detail_w - px(a, 6), rr.y, detail_w, rr.h);
            draw_text_fit(a->font_small, r, d, -1, dr, color_hex(T_TEXT_DIM), ALIGN_RIGHT);
            float dot = px(a, 6);
            /* one meaning per colour: connected green, connecting yellow, failed red, not connected hollow */
            int st = n->conn->state;
            Rect dotr = rect_make(dr.x - dot - px(a, 4), cy - dot / 2, dot, dot);
            if (st == CONN_FAILED) draw_rrect(r, dotr, dot / 2, color_hex(T_RED));
            else if (st == CONN_CONNECTING || st == CONN_RECONNECTING) draw_rrect(r, dotr, dot / 2, color_hex(T_YELLOW));
            else if (on) draw_rrect(r, dotr, dot / 2, color_hex(T_GREEN));
            else { draw_rrect(r, dotr, dot / 2, color_hex(T_TEXT_DIM)); draw_rrect(r, rect_inset(dotr, px(a, 1.5f), px(a, 1.5f)), dot / 2, bg); }
            detail_w += dot + px(a, 8);
        } else if (n->detail) {
            detail_w = font_width(a->font_small, n->detail, -1) + px(a, 12);
            Rect dr = rect_make(rr.x + rr.w - detail_w - px(a, 6), rr.y, detail_w, rr.h);
            /* INVALID in words and colour (not colour only); a draft marker */
            bool bad = n->kind == NODE_OBJECT && !strncmp(n->detail, "INVALID", 7);
            draw_text_fit(a->font_small, r, n->detail, -1, dr, color_hex(bad ? T_RED : T_TEXT_DIM), ALIGN_RIGHT);
        }
        Rect lr = rect_make(x, rr.y, rr.x + rr.w - x - detail_w - px(a, 10), rr.h);
        if (e->rn_open && n->kind == NODE_FOLDER && n->sname && !strcmp(n->sname, e->rn_path)) {   /* inline rename */
            Rect fr = rect_make(lr.x - px(a, 4), rr.y + px(a, 2), lr.w + px(a, 4), rr.h - px(a, 4)); ui_record("explorer:rename", fr);
            ui_textfield(a, &e->rn, fr, "Folder name", true);
            continue;
        }
        Color tc = color_hex(n->kind == NODE_ACTION ? T_ACCENT : n->kind == NODE_GROUP ? T_TEXT_MUTED : n->kind == NODE_CONN && !n->conn->db ? T_TEXT_MUTED : T_TEXT);
        draw_text_fit(n->kind == NODE_GROUP || n->kind == NODE_ACTION ? a->font_small : a->font_ui, r, n->label, -1, lr, tc, ALIGN_LEFT);
    }
    ui_scrollbar_draw(a, &e->sb, rows, content_h, e->scroll);
    clip_pop(r);

    if (hover_idx >= 0 && e->vis[hover_idx]->kind != NODE_COLUMN && !e->cm_open) a->want_cursor = CURSOR_HAND;
}

/* See explorer.h. One line: folders "+Name(n)" closed / "-Name(n)" open, connections by name,
 * nesting by dots ("..acme-prod"); rows below an open connection are left out. */
void explorer_dump(Explorer *e) {
    printf("explorer");
    for (int i = 0; i < e->nvis; i++) {
        Node *n = e->vis[i];
        if (n->kind != NODE_FOLDER && n->kind != NODE_CONN) continue;
        printf(" |");
        for (int d = 0; d < n->depth; d++) putchar('.');
        if (n->kind == NODE_FOLDER) printf("%c%s(%s)", n->expanded ? '-' : '+', n->label, n->detail ? n->detail : "0");
        else printf("%s", n->label);
    }
    printf("\n"); fflush(stdout);
}

/* Test harness: every visible row ("explorerdump rows"): depth dots, then the label */
void explorer_dump_rows(Explorer *e) {
    printf("explorer rows");
    for (int i = 0; i < e->nvis; i++) { Node *n = e->vis[i]; printf(" |"); for (int d = 0; d < n->depth; d++) putchar('.'); printf("%s", n->label); }
    printf("\n"); fflush(stdout);
}

/* Test harness: centre (window units) of the first visible node whose label is `label`. */
bool explorer_node_pos(Explorer *e, App *a, const char *label, float *x, float *y) {
    float row_h = px(a, M_ROW);
    for (int i = 0; i < e->nvis; i++) {
        if (!e->vis[i]->label || strcmp(e->vis[i]->label, label)) continue;
        float cy = e->rows_rect.y + i * row_h - e->scroll + row_h / 2;
        if (cy < e->rows_rect.y || cy > e->rows_rect.y + e->rows_rect.h) {
            /* off screen: scroll it into view like a user would, then report where it is now */
            float maxs = e->nvis * row_h - e->rows_rect.h;   /* the same limit the list applies when it draws */
            e->scroll = i * row_h - e->rows_rect.h / 2; if (e->scroll > maxs) e->scroll = maxs; if (e->scroll < 0) e->scroll = 0;
            e->scroll_t = e->scroll;   /* no smooth-scroll animation back to the old place */
            cy = e->rows_rect.y + i * row_h - e->scroll + row_h / 2;
        }
        *x = (e->rows_rect.x + px(a, 10 + 14 * e->vis[i]->depth) + px(a, 60)) / a->density;
        *y = cy / a->density;
        return true;
    }
    return false;
}

/* Test harness (DBX-126): nsx "<connection>" <op> [arg]. Prints the model's state line for
 * "state"; other ops act as the menus would. */
void explorer_nsx_cmd(Explorer *e, App *a, const char *arg) {
    char cn[64] = "", op[32] = "", rest[256] = "";
    sscanf(arg, "\"%63[^\"]\" %31s %255[^\n]", cn, op, rest);
    Connection *c = NULL;
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) c = a->conns->items[i];
    NsState *s = c ? nsx_find(c->id) : NULL;
    if (!s) { printf("nsx: no model for %s\n", cn); fflush(stdout); return; }
    uint64_t now = SDL_GetTicks();
    if (!strcmp(op, "state")) {
        static char buf[8192]; nsx_dump(s, buf, sizeof buf, true);
        int nodes = 0; for (int i = 0; i < e->nvis; i++) nodes += e->vis[i]->kind == NODE_SCHEMA;
        printf("%s rows=%d schema_rows=%d busy=%d\n", buf, e->nvis, nodes, (int)nsx_busy(s)); fflush(stdout); return;
    }
    if (!strcmp(op, "expand")) { Node *o = ns_group(e, c, NR_OTHERS); if (o && !o->expanded) toggle(o); }
    else if (!strcmp(op, "collapse")) { Node *o = ns_group(e, c, NR_OTHERS); if (o && o->expanded) toggle(o); }
    else if (!strcmp(op, "more")) nsx_load_more(s);
    else if (!strcmp(op, "refresh")) nsx_refresh(s);
    else if (!strcmp(op, "refresh-others")) nsx_refresh_others(s);
    else if (!strcmp(op, "pin")) nsx_pin(s, rest);
    else if (!strcmp(op, "unpin")) nsx_unpin(s, rest);
    else if (!strcmp(op, "probe")) nsx_probe(s, rest);
    else if (!strcmp(op, "reveal")) reveal_schema(e, a, c, rest, false);
    else if (!strcmp(op, "open")) reveal_schema(e, a, c, rest, true);
    else if (!strcmp(op, "mode")) nsx_set_mode(s, !strcmp(rest, "all") ? NSM_ALL : !strcmp(rest, "pinned") ? NSM_PINNED : NSM_COMPACT);
    else if (!strcmp(op, "system")) nsx_set_system(s, atoi(rest) != 0);
    else if (!strcmp(op, "option")) { char k[32]; int v = 0; if (sscanf(rest, "%31s %d", k, &v) == 2) nsx_set_option(s, k, v); }
    else if (!strcmp(op, "reset")) nsx_reset_prefs(s);
    else if (!strcmp(op, "search") || !strcmp(op, "contains") || !strcmp(op, "objects")) {
        search_open(e, c, op[0] == 'o', "");
        e->search_sub = op[0] == 'c';
        textfield_set(&e->sf, rest);
        snprintf(e->search_last, sizeof e->search_last, "%s", rest);
        nsx_search(s, rest, e->search_sub, op[0] == 'o', "", now);
    }
    else if (!strcmp(op, "hit")) hit_open(e, a, atoi(rest), false);
    else if (!strcmp(op, "close-search")) search_close(e);
    else if (!strcmp(op, "console")) open_console(a, c, rest);
    else if (!strcmp(op, "props")) {   /* Properties of a schema ("props NAME") or of the Schemas node ("props") */
        if (rest[0]) cm_props_schema(e, c, rest, 100, 100); else cm_props_root(e, c, 100, 100);
        printf("nsx props:"); for (int i = 0; i < e->cm_n; i++) if (e->cm_cmd[i] == CM_INFO) printf(" | %s", e->cm_text[i]);
        printf("\n"); fflush(stdout); e->cm_open = false; return;
    }
    else if (!strcmp(op, "menu")) {   /* the context menu of a row by label: "menu Others" */
        for (int i = 0; i < e->nvis; i++) if (e->vis[i]->label && !strncmp(e->vis[i]->label, rest, strlen(rest))) { e->selected = e->vis[i]; cm_open_node(e, a, e->vis[i], e->rows_rect.x + px(a, 60), e->rows_rect.y + (i + 1) * px(a, M_ROW) - e->scroll); break; }
        printf("nsx menu:"); for (int i = 0; i < e->cm_n; i++) if (e->cm_cmd[i] != CM_SEP) printf(" [%s%s%s]", e->cm_dis[i] && e->cm_cmd[i] != CM_INFO ? "-" : "", e->cm_check[i] == 1 ? "x " : "", e->cm_text[i]);
        printf("\n"); fflush(stdout);
    }
    else { printf("nsx: unknown op %s\n", op); fflush(stdout); return; }
    explorer_rebuild(e, a);
}

/* Test harness: true when the connection's model has read its scope and has nothing queued or
 * running (nsx "<connection>" await). */
bool explorer_nsx_ready(App *a, const char *arg) {
    char cn[64] = ""; sscanf(arg, "\"%63[^\"]\"", cn);
    for (int i = 0; a->conns && i < a->conns->n; i++) {
        Connection *c = a->conns->items[i];
        if (strcmp(c->name, cn)) continue;
        if (c->state == CONN_FAILED) return true;   /* nothing will come: let the script go on */
        NsState *s = c->db ? nsx_find(c->id) : NULL;
        NsRun *r = s ? run_slot(c->id) : NULL;
        return s && r && !r->job && (s->scope_known || s->scope_failed) && !nsx_busy(s);
    }
    return false;
}
