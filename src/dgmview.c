/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram tabs: catalog per connection + scope, documents, camera, history, commands, harness.
 * See dgmview.h and docs/diagrams.md. */
#include "dgmview_int.h"
#include "commands.h"
#include "conn.h"
#include "db.h"
#include "log.h"
#include "nsx.h"
#include "panels.h"
#include "sqlguard.h"
#include "sqllex.h"
#include "tdes.h"
#include "toolbar.h"
#include <ctype.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

DgGlobals DG;

static double now_ms(void) { return (double)SDL_GetTicksNS() / 1e6; }
static uint64_t fnv(const char *s, uint64_t h) { if (!h) h = 1469598103934665603ULL; for (; s && *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }

Connection *dg_conn(App *a, const char *id) {
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i];
    return NULL;
}
void dg_msg(DgDoc *d, const char *fmt, ...) {
    va_list ap; va_start(ap, fmt); vsnprintf(d->msg, sizeof d->msg, fmt, ap); va_end(ap);
    d->msg_t = SDL_GetTicks();
    if (DG.a) app_wake_in(DG.a, 16);
}
void dg_touch(DgDoc *d) { d->d.dirty = true; d->dirty_at = SDL_GetTicks(); }
DgCat *dg_cat(DgDoc *d) { return d && d->cat >= 0 && d->cat < DG_MAX_CATS && DG.cats[d->cat].used ? &DG.cats[d->cat] : NULL; }
bool dg_conn_read_only(App *a, DgDoc *d) { Connection *c = dg_conn(a, d->conn_id); return !c || c->read_only; }
const char *dg_exf_ext(int f) { static const char *const e[] = { "png", "svg", "pdf", "mmd", "graphml", "drawio", "html", "lume-erd" }; return f >= 0 && f < EXF_COUNT ? e[f] : "txt"; }

/* ---------------------------------------------------------------- fonts per zoom */
Font *dg_font(App *a, float pt) {
    int px_ = (int)lroundf(pt * a->scale);
    if (px_ < 5) px_ = 5;
    if (px_ > 96) px_ = 96;
    int free_i = -1, lru = 0;
    for (int i = 0; i < 12; i++) {
        if (DG.fonts[i].f && DG.fonts[i].px == px_) { DG.fonts[i].used = SDL_GetTicks(); return DG.fonts[i].f; }
        if (!DG.fonts[i].f && free_i < 0) free_i = i;
        if (DG.fonts[i].used < DG.fonts[lru].used) lru = i;
    }
    if (free_i < 0) { font_destroy(DG.fonts[lru].f); DG.fonts[lru].f = NULL; free_i = lru; }
    if (!DG.font_path[0]) snprintf(DG.font_path, sizeof DG.font_path, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
    Font *f = font_load(a->renderer, DG.font_path, (float)px_);
    if (!f) return a->font_small;
    DG.fonts[free_i].f = f; DG.fonts[free_i].px = px_; DG.fonts[free_i].used = SDL_GetTicks();
    return f;
}
static float measure_cb(void *ud, const char *s) { App *a = ud; Font *f = dg_font(a, 12); return font_width(f, s, -1) / a->scale; }
void dg_measure(App *a, DgDoc *d) { dgm_measure_all(&d->d, measure_cb, a); }

/* ---------------------------------------------------------------- camera */
float dg_k(const DgDoc *d, App *a) { return d->d.zoom * a->scale; }
void dg_world_to_screen(const DgDoc *d, App *a, float wx, float wy, float *sx, float *sy) {
    float k = dg_k(d, a); *sx = d->canvas.x + (wx - d->d.panx) * k; *sy = d->canvas.y + (wy - d->d.pany) * k;
}
void dg_screen_to_world(const DgDoc *d, App *a, float sx, float sy, float *wx, float *wy) {
    float k = dg_k(d, a); *wx = d->d.panx + (sx - d->canvas.x) / k; *wy = d->d.pany + (sy - d->canvas.y) / k;
}
void dg_zoom_at(App *a, DgDoc *d, float f, float sx, float sy) {
    float wx, wy; dg_screen_to_world(d, a, sx, sy, &wx, &wy);
    float z = fmaxf(0.1f, fminf(4.0f, d->d.zoom * f));
    d->d.zoom = z;
    float k = dg_k(d, a);
    d->d.panx = wx - (sx - d->canvas.x) / k; d->d.pany = wy - (sy - d->canvas.y) / k;
    d->anim = false;
    app_wake_in(a, 16);
}
static void cam_to(App *a, DgDoc *d, float z, float px_, float py_, bool animate) {
    if (!animate || DG.reduce_motion || getenv("LUME_SCRIPT")) { d->d.zoom = z; d->d.panx = px_; d->d.pany = py_; d->anim = false; app_wake_in(a, 16); return; }
    d->anim = true; d->az0 = d->d.zoom; d->ax0 = d->d.panx; d->ay0 = d->d.pany; d->az1 = z; d->ax1 = px_; d->ay1 = py_; d->at0 = SDL_GetTicks();
    app_wake_in(a, 16);
}
void dg_fit(App *a, DgDoc *d, bool only_sel) {
    float x0, y0, x1, y1;
    if (!dgm_bounds(&d->d, only_sel, &x0, &y0, &x1, &y1)) { if (only_sel) dg_msg(d, "Fit Selection: nothing is selected"); return; }
    float cw = d->canvas.w / a->scale, ch = d->canvas.h / a->scale;
    if (cw < 50 || ch < 50) { d->need_fit = true; return; }
    float mmw = d->minimap ? 200 : 0;   /* the minimap covers the bottom-right corner: frame beside it */
    float m = 40, z = fminf((cw - 2 * m - mmw) / fmaxf(1, x1 - x0), (ch - 2 * m) / fmaxf(1, y1 - y0));
    bool too_big = z < 0.1f;
    z = fmaxf(0.1f, fminf(only_sel ? 1.0f : 1.2f, z));   /* a small selection is not blown up (DGM-021) */
    float px_ = (x0 + x1) / 2 - (cw - mmw) / (2 * z), py_ = (y0 + y1) / 2 - ch / (2 * z);
    if (too_big) { dg_msg(d, "Too large to fit at a readable size: showing the centre (the minimap shows the whole)"); }
    cam_to(a, d, z, px_, py_, true);
    d->fitted = true; d->need_fit = false;
}
void dg_center_on(App *a, DgDoc *d, int node, bool animate) {
    if (node < 0 || node >= d->d.nn) return;
    DgNode *n = &d->d.n[node];
    float z = d->d.zoom < 0.5f ? 0.8f : d->d.zoom;   /* never stay at an unreadable zoom when focusing */
    float cw = d->canvas.w / a->scale, ch = d->canvas.h / a->scale;
    cam_to(a, d, z, n->x + n->w / 2 - cw / (2 * z), n->y + fminf(n->h, ch / z * 0.8f) / 2 - ch / (2 * z), animate);
}
void dg_push_history(DgDoc *d) {
    DgView v = { d->d.zoom, d->d.panx, d->d.pany, "" };
    for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel) { snprintf(v.sel, sizeof v.sel, "%s", d->d.n[i].key); break; }
    if (d->ihist < d->nhist) d->nhist = d->ihist;   /* a new branch drops the forward entries */
    if (d->nhist == DG_HIST) { memmove(&d->hist[0], &d->hist[1], sizeof d->hist[0] * (DG_HIST - 1)); d->nhist--; }
    d->hist[d->nhist++] = v; d->ihist = d->nhist;
}
static void restore_view(App *a, DgDoc *d, const DgView *v) {
    for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = v->sel[0] && !strcmp(d->d.n[i].key, v->sel);
    cam_to(a, d, v->zoom, v->panx, v->pany, true);
}
bool dg_back(App *a, DgDoc *d) {
    if (d->ihist <= 0) return false;
    if (d->ihist == d->nhist) { dg_push_history(d); d->ihist = d->nhist - 1; }
    d->ihist--;
    restore_view(a, d, &d->hist[d->ihist]);
    return true;
}
bool dg_forward(App *a, DgDoc *d) {
    if (d->ihist >= d->nhist - 1) return false;
    d->ihist++;
    restore_view(a, d, &d->hist[d->ihist]);
    return true;
}

/* ---------------------------------------------------------------- selection helpers */
void dg_select_only(DgDoc *d, int node) { for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = i == node; d->focus_node = node; d->focus_row = -1; }
int dg_nsel(const DgDoc *d) { int n = 0; for (int i = 0; i < d->d.nn; i++) n += d->d.n[i].sel; return n; }
void dg_iso_update(DgDoc *d) {
    free(d->iso); d->iso = NULL;
    if (!d->isolate) return;
    int N = d->d.nn; d->iso = calloc((size_t)(N ? N : 1), 1);
    int *dist = malloc(sizeof(int) * (size_t)(N ? N : 1)), *q = malloc(sizeof(int) * (size_t)(N ? N : 1)), qh = 0, qt = 0;
    for (int i = 0; i < N; i++) { dist[i] = -1; if (d->d.n[i].sel) { dist[i] = 0; q[qt++] = i; } }
    while (qh < qt) {
        int c = q[qh++]; d->iso[c] = 1;
        if (dist[c] >= d->isolate_hops) continue;
        for (int e = 0; e < d->d.ne; e++) {
            int o = d->d.e[e].from == c ? d->d.e[e].to : d->d.e[e].to == c ? d->d.e[e].from : -1;
            if (o >= 0 && dist[o] < 0) { dist[o] = dist[c] + 1; q[qt++] = o; }
        }
    }
    free(dist); free(q);
}

/* ---------------------------------------------------------------- catalog per connection + scope */
static void pref_dir(App *a, char *o, size_t n) { snprintf(o, n, "%s", a->conns && a->conns->dir[0] ? a->conns->dir : "."); size_t L = strlen(o); if (L > 1 && o[L - 1] == '/') o[L - 1] = 0; }
static const char *dialect_of(const Connection *c) {
    if (!c) return NULL;
    if (!strcmp(c->driver, "postgres")) return "postgres";
    if (!strcmp(c->driver, "oracle")) return "oracle";
    if (!strcmp(c->driver, "mysql") || !strcmp(c->driver, "mariadb")) return "mysql";
    if (!strcmp(c->driver, "sqlserver")) return "mssql";
    if (!strcmp(c->driver, "sqlite")) return "sqlite";
    return NULL;
}
static void cache_path(App *a, const Connection *c, const char *scope, char *o, size_t n) {
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    char cd[1200]; snprintf(cd, sizeof cd, "%s/cache", dir); mkdir(cd, 0700);
    /* the key: connection, scope, login and server (DGM-018); the file holds names and types, never rows */
    const char *u = c ? conn_value((Connection *)c, "user") : NULL;
    uint64_t h = fnv(scope, 0); h = fnv(u ? u : "", h); h = fnv(c ? c->driver : "", h);   /* the server version is not known to the Db: a Refresh re-reads after an upgrade */
    snprintf(o, n, "%s/dgm-%s-%016llx.idx", cd, c ? c->id : "none", (unsigned long long)h);
}
static void cat_free(DgCat *k) {
    if (k->job) { exec_free(k->job); k->job = NULL; }
    for (int i = 0; i < k->nst; i++) { free(k->stmts[i]); k->stmts[i] = NULL; }
    k->nst = 0;
    schidx_free(&k->x);
}
static int cat_find(const char *conn_id, const char *scope) {
    for (int i = 0; i < DG_MAX_CATS; i++) if (DG.cats[i].used && !strcmp(DG.cats[i].conn_id, conn_id) && !strcmp(DG.cats[i].scope, scope)) return i;
    return -1;
}
static bool cat_in_use(int k) { for (int i = 0; i < DG_MAX_DOCS; i++) if (DG.docs[i] && DG.docs[i]->cat == k) return true; return false; }
static int cat_get(App *a, Connection *c, const char *scope, bool refresh) {
    int k = cat_find(c->id, scope);
    if (k < 0) {
        for (int i = 0; i < DG_MAX_CATS && k < 0; i++) if (!DG.cats[i].used) k = i;
        for (int i = 0; i < DG_MAX_CATS && k < 0; i++) if (!cat_in_use(i)) { cat_free(&DG.cats[i]); DG.cats[i].used = false; k = i; }
        if (k < 0) return -1;
        DgCat *nk = &DG.cats[k]; unsigned ver = nk->version;
        memset(nk, 0, sizeof *nk); nk->used = true; nk->version = ver;
        snprintf(nk->conn_id, sizeof nk->conn_id, "%s", c->id); snprintf(nk->scope, sizeof nk->scope, "%s", scope);
        schidx_init(&nk->x);
        refresh = refresh || false;
    }
    DgCat *ck = &DG.cats[k];
    if (refresh || ck->state == CS_NONE || ck->state == CS_ERROR) {
        char cp[1300]; cache_path(a, c, scope, cp, sizeof cp);
        struct stat st;
        if (!refresh && ck->state == CS_NONE && stat(cp, &st) == 0 && time(NULL) - st.st_mtime < 12 * 3600) {
            SchIdx y; if (schidx_load(&y, cp)) {
                schidx_free(&ck->x); ck->x = y; ck->state = CS_READY; ck->version++; ck->stale = false;
                LOG_I("diagram: catalog of %s (scope %s) from the cache: %d tables", c->name, scope[0] ? scope : "default", y.n);
                return k;
            }
        }
        if (ck->job) { exec_free(ck->job); ck->job = NULL; }
        ck->state = CS_WAIT; ck->err[0] = 0; ck->t0 = now_ms();
    }
    return k;
}
/* offline: the last cached catalog, whatever its age, marked stale (DGM-018) */
static bool cat_offline(App *a, DgCat *k, Connection *c) {
    char cp[1300]; cache_path(a, c, k->scope, cp, sizeof cp);
    SchIdx y; if (!schidx_load(&y, cp)) return false;
    schidx_free(&k->x); k->x = y; k->state = CS_READY; k->stale = true; k->version++;
    return true;
}
static void cat_pump(App *a, DgCat *k) {
    if (k->state != CS_WAIT && k->state != CS_RUN) return;
    Connection *c = dg_conn(a, k->conn_id);
    if (!c) { k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "the connection is gone"); return; }
    const char *dl = dialect_of(c);
    if (!dl) { k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "ER diagrams are not available for this driver yet"); return; }
    if (k->state == CS_WAIT) {
        if (!c->db) {
            if (c->state == CONN_FAILED) { if (!cat_offline(a, k, c)) { k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "%s: %s", c->name, c->error); } return; }
            if (c->state != CONN_CONNECTING && c->state != CONN_RECONNECTING) conn_connect_async(c, false);
            app_wake_in(a, 200); return;
        }
        if (c->db->busy) { app_wake_in(a, 120); return; }   /* one statement at a time per session */
        char sch[24][200]; int ns = 0;
        for (const char *p = k->scope; *p && ns < 24; ) {
            const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
            while (l && *p == ' ') { p++; l--; }
            if (l) { snprintf(sch[ns], 200, "%.*s", (int)(l < 199 ? l : 199), p); ns++; }
            if (!e) break;
            p = e + 1;
        }
        const char *sp[24]; for (int i = 0; i < ns; i++) sp[i] = sch[i];
        char sql[16384], why[300]; int kind;
        k->nst = 0;
        for (int q = 0; q < 4 && schidx_sql(dl, q, sp, ns, &kind, sql, sizeof sql); q++) {
            if (!sqlguard_check(sql, dialect_for_driver(c->driver), why, sizeof why)) { LOG_W("diagram: catalog read refused by the read-only policy: %s", why); continue; }
            k->stmts[k->nst] = strdup(sql); k->kinds[k->nst] = kind; k->nst++;
        }
        if (!k->nst) { k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "no catalog read for this database"); return; }
        k->job = exec_start_list(c->db, (const char *const *)k->stmts, k->nst, 5000000, false, false);
        if (!k->job) { app_wake_in(a, 120); return; }
        k->state = CS_RUN; k->statements += k->nst;
        LOG_I("diagram: reading the structure of %s (scope %s) in the background: %d catalog statements, no rows", c->name, k->scope[0] ? k->scope : "default", k->nst);
        app_wake_in(a, 100);
        return;
    }
    if (exec_running(k->job)) { app_wake_in(a, 100); return; }
    SchIdx y; schidx_init(&y); snprintf(y.dialect, sizeof y.dialect, "%s", dl);
    bool cols_ok = false; char err[300] = "";
    for (int i = 0; i < k->nst; i++) {
        ResultSet rs; rs_init(&rs);
        if (!exec_take_result(k->job, i, &rs)) continue;
        if (rs.error[0]) { if (k->kinds[i] == SXQ_COLS) snprintf(err, sizeof err, "%s", rs.error); LOG_W("diagram: catalog read %d failed: %s", i, rs.error); rs_free(&rs); continue; }
        for (int r = 0; r < rs.nrows; r++) {
            const char *f[8]; for (int q = 0; q < 8; q++) f[q] = q < rs.ncols ? rs_cell(&rs, r, q) : NULL;
            if (k->kinds[i] == SXQ_COLS && rs.ncols >= 8 && f[1] && f[3]) {
                int ti = schidx_table(&y, f[0] ? f[0] : "", f[1], f[2] && atoi(f[2]) != 0);
                schidx_add_col(&y, ti, f[3], f[4] ? f[4] : "?", f[5] && atoi(f[5]) != 0, f[6] && atoi(f[6]) != 0, f[7]);
            } else if (k->kinds[i] == SXQ_TCOMMENT && rs.ncols >= 3 && f[1] && f[2]) {
                schidx_table_comment(&y, schidx_find(&y, f[0] ? f[0] : "", f[1]), f[2]);
            } else if (k->kinds[i] == SXQ_FKS && rs.ncols >= 5 && f[1] && f[4]) {
                schidx_add_fk2(&y, schidx_find(&y, f[0] ? f[0] : "", f[1]), rs.ncols > 5 ? f[5] : NULL, f[2] ? f[2] : "", f[3], f[4], rs.ncols > 6 ? f[6] : NULL);
            } else if (k->kinds[i] == SXQ_UNIQUES && rs.ncols >= 3 && f[1] && f[2]) {
                schidx_add_unique(&y, schidx_find(&y, f[0] ? f[0] : "", f[1]), f[2]);
            }
        }
        if (k->kinds[i] == SXQ_COLS) cols_ok = true;
        rs_free(&rs);
    }
    bool cancelled = exec_cancelling(k->job);
    exec_free(k->job); k->job = NULL;
    for (int i = 0; i < k->nst; i++) { free(k->stmts[i]); k->stmts[i] = NULL; }
    k->nst = 0;
    if (cancelled) { schidx_free(&y); k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "Cancelled"); return; }
    if (!cols_ok) { schidx_free(&y); if (!cat_offline(a, k, c)) { k->state = CS_ERROR; snprintf(k->err, sizeof k->err, "%s", err[0] ? err : "the catalog could not be read"); } return; }
    schidx_finish(&y);
    schidx_free(&k->x); k->x = y; k->state = CS_READY; k->stale = false; k->version++;
    k->ms = now_ms() - k->t0;
    char cp[1300]; cache_path(a, c, k->scope, cp, sizeof cp);
    if (!schidx_save(&k->x, cp)) LOG_W("diagram: the catalog could not be cached");
    LOG_I("diagram: structure of %s (scope %s) read: %d tables in %.1f s", c->name, k->scope[0] ? k->scope : "default", k->x.n, k->ms / 1000);
    app_wake_in(a, 16);
}

/* ---------------------------------------------------------------- documents */
DgDoc *dg_active(App *a) {
    struct DgDoc *p = panels_active_diagram(a->panels, a);
    return p;
}
static void default_scope(App *a, Connection *c, char *o, size_t n) {
    o[0] = 0;
    if (!c) return;
    NsState *ns = nsx_find(c->id);
    if (!strcmp(c->driver, "sqlite")) return;   /* one file */
    if (ns && ns->current[0]) { snprintf(o, n, "%s", ns->current); return; }
    if (!strcmp(c->driver, "postgres")) snprintf(o, n, "%s", c->schema[0] ? c->schema : "public");
    else if (!strcmp(c->driver, "oracle")) { const char *u = conn_value(c, "user"); snprintf(o, n, "%s", u ? u : ""); for (char *x = o; *x; x++) *x = (char)toupper((unsigned char)*x); }
    else if (!strcmp(c->driver, "sqlserver")) snprintf(o, n, "dbo");
    else { const char *db = conn_value(c, "database"); snprintf(o, n, "%s", db ? db : ""); }
}
static void diagrams_dir(App *a, char *o, size_t n) {
    const char *ws = a->toolbar ? toolbar_workspace_dir(a->toolbar) : "";
    if (ws && *ws) snprintf(o, n, "%s/diagrams", ws);
    else { char p[1100]; pref_dir(a, p, sizeof p); snprintf(o, n, "%s/diagrams", p); }
    mkdir(o, 0700);
}
static void file_name(const char *name, char *o, size_t n) {
    size_t k = 0;
    for (const char *p = name; *p && k + 1 < n; p++) o[k++] = isalnum((unsigned char)*p) || *p == '-' || *p == '_' || *p == '.' ? *p : '_';
    o[k] = 0;
    if (!k) snprintf(o, n, "diagram");
}
static void unique_path(App *a, const char *name, char *o, size_t n) {
    char dir[1150]; diagrams_dir(a, dir, sizeof dir);
    char base[200]; file_name(name, base, sizeof base);
    struct stat st;
    snprintf(o, n, "%s/%s.lume-erd", dir, base);
    for (int i = 2; stat(o, &st) == 0 && i < 1000; i++) snprintf(o, n, "%s/%s-%d.lume-erd", dir, base, i);
}
static DgDoc *doc_new(App *a, Connection *c, const char *name, const char *scope) {
    int slot = -1; for (int i = 0; i < DG_MAX_DOCS; i++) if (!DG.docs[i]) { slot = i; break; }
    if (slot < 0) { app_status(a, "Too many diagrams are open: close one first"); return NULL; }
    DgDoc *d = calloc(1, sizeof *d); if (!d) return NULL;
    d->used = true; d->id = ++DG.seq; d->cat = -1; d->hover = d->hover_edge = d->hover_note = d->focus_node = d->cm_node = d->cm_edge = d->cm_note = -1;
    d->pending_focus_ti = -1; d->link_from = -1; d->isolate_hops = 1; d->lod = 2; d->flash_note = -1;
    dgm_init(&d->d);
    snprintf(d->d.name, sizeof d->d.name, "%s", name);
    d->find_flags = DF_TABLES; d->legend = true; d->ex_legend = true; d->ex_title = true; d->ex_scale = 2; d->ex_scope = EXS_DIAGRAM;
    d->hits = malloc(sizeof *d->hits * DG_HITS);
    SDL_SetAtomicInt(&d->lay_done, 0);
    if (c) {
        snprintf(d->conn_id, sizeof d->conn_id, "%s", c->id);
        snprintf(d->d.conn_id, sizeof d->d.conn_id, "%s", c->id); snprintf(d->d.conn_name, sizeof d->d.conn_name, "%s", c->name);
        char sc[1200]; if (scope) snprintf(sc, sizeof sc, "%s", scope); else default_scope(a, c, sc, sizeof sc);
        snprintf(d->d.schemas, sizeof d->d.schemas, "%s", sc);
        d->cat = cat_get(a, c, sc, false);
    }
    unique_path(a, name, d->path, sizeof d->path);
    DG.docs[slot] = d;
    return d;
}
static void doc_free(DgDoc *d) {
    if (d->lay_th) { d->lay_cancel = 1; SDL_WaitThread(d->lay_th, NULL); d->lay_th = NULL; }
    if (d->lay_copy) { free(d->lay_copy->n); free(d->lay_copy->e); free(d->lay_copy); d->lay_copy = NULL; }
    if (d->ai) { ai_free(d->ai); d->ai = NULL; }
    free(d->ai_payload); free(d->ai_text); free(d->hits); free(d->pending_list); free(d->drag_xy); free(d->iso); free(d->pick); free(d->pick_on);
    dgm_free(&d->d);
    for (int i = 0; i < DG_MAX_DOCS; i++) if (DG.docs[i] == d) DG.docs[i] = NULL;
    free(d);
}
static int open_tab(App *a, DgDoc *d) {
    char t[48]; snprintf(t, sizeof t, "%.30s.erd", d->d.name);
    return panels_open_diagram_tab(a->panels, a, t, d);
}
void dg_rebind(DgDoc *d) {
    DgCat *k = dg_cat(d);
    dgm_bind(&d->d, k && k->state == CS_READY ? &k->x : NULL);
    if (k) d->cat_version = k->version;
    if (k && k->state == CS_READY && k->x.built_at > 0) {   /* the snapshot's time: when this structure was read */
        time_t t = (time_t)k->x.built_at; struct tm tm; localtime_r(&t, &tm);
        strftime(d->d.captured, sizeof d->d.captured, "%Y-%m-%d %H:%M", &tm);
    }
    if (DG.a) dg_measure(DG.a, d);
    dgm_compare(&d->d);
    dg_iso_update(d);
    d->last_cat = 0;   /* find re-runs */
}
bool dg_save(App *a, DgDoc *d) {
    char why[200];
    if (!d->d.captured[0]) { time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); strftime(d->d.captured, sizeof d->d.captured, "%Y-%m-%dT%H:%M:%SZ", &tm); }
    /* the structure reference of each table is taken when it is first saved (DGM-016); "Accept Current Structure" renews it */
    for (int i = 0; i < d->d.nn; i++) { DgNode *n = &d->d.n[i]; if (n->kind == DN_TABLE && n->ti >= 0 && !n->sig[0] && d->d.x) dgm_table_sig(d->d.x, n->ti, n->sig, sizeof n->sig); }
    if (!dgm_save(&d->d, d->path, why, sizeof why)) {
        if (!d->save_failed) { dg_msg(d, "Could not save the diagram: %s", why); LOG_W("diagram: save failed: %s", why); }
        d->save_failed = true;
        return false;
    }
    d->save_failed = false; d->d.dirty = false;
    (void)a;
    return true;
}
void dg_set_scope(App *a, DgDoc *d, const char *schemas) {
    Connection *c = dg_conn(a, d->conn_id); if (!c) return;
    snprintf(d->d.schemas, sizeof d->d.schemas, "%s", schemas);
    d->cat = cat_get(a, c, schemas, false);
    d->cat_version = 0;
    dg_touch(d);
}
void dg_refresh(App *a, DgDoc *d, bool full) {
    Connection *c = dg_conn(a, d->conn_id); if (!c) { dg_msg(d, "The connection of this diagram no longer exists"); return; }
    d->cat = cat_get(a, c, d->d.schemas, true);
    d->cat_version = 0;
    dg_msg(d, full ? "Refreshing the diagram's structure..." : "Refreshing...");
}

/* ---------------------------------------------------------------- layout (inline or on a worker) */
static int lay_th(void *p) {
    DgDoc *d = p;
    d->lay_ok = dgm_layout(d->lay_copy, false, &d->lay_cancel);
    SDL_SetAtomicInt(&d->lay_done, 1);
    if (DG.a) app_wake_in(DG.a, 16);
    return 0;
}
void dg_layout(App *a, DgDoc *d, bool only_sel) {
    if (d->lay_th) { dg_msg(d, "A layout is already running"); return; }
    dgm_snapshot(&d->d);
    dg_measure(a, d);
    if (d->d.nn <= 200 || only_sel) {
        double t0 = now_ms();
        dgm_layout(&d->d, only_sel, NULL);
        d->d.grid_ok = false;
        dg_touch(d);
        LOG_I("diagram: layout of %d cards in %.1f ms", d->d.nn, now_ms() - t0);
        return;
    }
    /* a copy of what the layout reads; positions come back by key */
    Dgm *cp = calloc(1, sizeof *cp); *cp = d->d;
    cp->n = malloc(sizeof *cp->n * (size_t)d->d.nn); memcpy(cp->n, d->d.n, sizeof *cp->n * (size_t)d->d.nn); cp->capn = d->d.nn;
    cp->e = malloc(sizeof *cp->e * (size_t)(d->d.ne ? d->d.ne : 1)); memcpy(cp->e, d->d.e, sizeof *cp->e * (size_t)d->d.ne); cp->cape = d->d.ne;
    cp->rels = NULL; cp->notes = NULL; cp->hidden = NULL; cp->rev = cp->rev_start = cp->cell_start = cp->cell_items = NULL; cp->nundo = cp->iundo = 0;
    d->lay_copy = cp; d->lay_cancel = 0; SDL_SetAtomicInt(&d->lay_done, 0);
    d->lay_th = SDL_CreateThread(lay_th, "dgm-layout", d);
    if (!d->lay_th) { free(cp->n); free(cp->e); free(cp); d->lay_copy = NULL; dgm_layout(&d->d, false, NULL); dg_touch(d); return; }
    dg_msg(d, "Laying out %d tables... (Esc cancels)", d->d.nn);
}
static void lay_poll(App *a, DgDoc *d) {
    if (!d->lay_th || !SDL_GetAtomicInt(&d->lay_done)) return;
    SDL_WaitThread(d->lay_th, NULL); d->lay_th = NULL;
    Dgm *cp = d->lay_copy; d->lay_copy = NULL;
    if (d->lay_ok) {
        for (int i = 0; i < cp->nn; i++) { int k = dgm_find_node(&d->d, cp->n[i].key); if (k >= 0 && !d->d.n[k].pinned) { d->d.n[k].x = cp->n[i].x; d->d.n[k].y = cp->n[i].y; } }
        d->d.grid_ok = false; dg_touch(d);
        dg_msg(d, "Layout done");
        if (d->need_fit || !d->fitted) dg_fit(a, d, false);
    } else dg_msg(d, "Layout cancelled: the cards kept their positions");
    free(cp->n); free(cp->e); free(cp);
}

/* ---------------------------------------------------------------- adding tables */
int dg_add_hit(App *a, DgDoc *d, int ti) {
    DgCat *k = dg_cat(d); if (!k || k->state != CS_READY) return -1;
    char key[DG_KEY]; dgm_key_of(&k->x, ti, key, sizeof key);
    int have = dgm_find_node(&d->d, key);
    if (have >= 0) return have;
    /* next to its nearest neighbour already on the diagram, else at the viewport's centre */
    int nb[512]; int nn = dgm_catalog_neighbours(&d->d, ti, nb, 512);
    float x, y; dg_screen_to_world(d, a, d->canvas.x + d->canvas.w / 2, d->canvas.y + d->canvas.h / 2, &x, &y);
    for (int i = 0; i < nn; i++) { char nk[DG_KEY]; dgm_key_of(&k->x, nb[i], nk, sizeof nk); int on = dgm_find_node(&d->d, nk); if (on >= 0) { x = d->d.n[on].x + d->d.n[on].w + 90; y = d->d.n[on].y; break; } }
    /* do not cover another card */
    for (int tries = 0; tries < 40; tries++) {
        bool clash = false;
        for (int i = 0; i < d->d.nn; i++) { DgNode *o = &d->d.n[i]; if (x < o->x + o->w + 20 && x + 200 > o->x - 20 && y < o->y + o->h + 20 && y + 60 > o->y - 20) { clash = true; break; } }
        if (!clash) break;
        y += 60;
    }
    int n = dgm_add_table(&d->d, ti, roundf(x), roundf(y));
    dgm_edges(&d->d);
    dg_measure(a, d);
    dg_iso_update(d);
    dg_touch(d);
    return n;
}
void dg_expand(App *a, DgDoc *d, int ti, int hops) {
    int would = dgm_expand(&d->d, ti, hops, 0, true);
    if (!would) { dg_msg(d, "Every related table is already on the diagram"); return; }
    if (would > 25) {   /* say how many first (DGM-075) */
        d->dlg = DLG_CONFIRM_ADD; d->confirm_ti = ti; d->confirm_hops = hops; d->confirm_count = would;
        snprintf(d->dlg_title, sizeof d->dlg_title, "Add %d table%s?", would > 150 ? 150 : would, would == 1 ? "" : "s");
        snprintf(d->dlg_body, sizeof d->dlg_body, "%d table%s within %d hop%s are not on the diagram yet.%s", would, would == 1 ? "" : "s", hops, hops == 1 ? "" : "s",
                 would > 150 ? " One step adds at most 150 (the closest first); repeat to add more." : "");
        return;
    }
    dgm_snapshot(&d->d);
    int added = dgm_expand(&d->d, ti, hops, 150, false);
    dg_measure(a, d); dg_layout(a, d, false); dg_iso_update(d); dg_touch(d); d->need_fit = true;
    dg_msg(d, "Added %d related table%s", added, added == 1 ? "" : "s");
}

/* ---------------------------------------------------------------- entry points */
static int find_table(DgCat *k, const char *schema, const char *table) {
    if (!k || k->state != CS_READY) return -1;
    int ti = schidx_find(&k->x, schema && *schema ? schema : NULL, table);
    if (ti < 0) ti = schidx_find(&k->x, NULL, table);
    return ti;
}
static void run_pending(App *a, DgDoc *d) {
    DgCat *k = dg_cat(d); if (!k || k->state != CS_READY || (!d->pending_table[0] && d->pending_kind < 2)) return;
    int kind = d->pending_kind; char tb[DG_KEY]; snprintf(tb, sizeof tb, "%s", d->pending_table); d->pending_table[0] = 0; d->pending_kind = -1;
    if (kind == 0 || kind == 1) {
        char *dot = strrchr(tb, '.'); char sc[200] = ""; const char *nm = tb;
        if (dot) { snprintf(sc, sizeof sc, "%.*s", (int)(dot - tb), tb); nm = dot + 1; }
        int ti = find_table(k, sc, nm);
        if (ti < 0) { dg_msg(d, "%s is not in this diagram's scope (%s)", tb, d->d.schemas[0] ? d->d.schemas : "default"); return; }
        if (kind == 0) {
            dgm_open_focus(&d->d, ti, 40);
            int more = dgm_expand(&d->d, ti, 1, 0, true);
            dg_measure(a, d); dg_layout(a, d, false); d->need_fit = true;
            int fn = dgm_find_node(&d->d, d->d.focus); if (fn >= 0) { dg_select_only(d, fn); snprintf(d->flash, sizeof d->flash, "%s", d->d.focus); }   /* the table it was opened on stands out */
            if (more) dg_msg(d, "+%d more related table%s: Show Neighbors adds them", more, more == 1 ? "" : "s");
        } else { int n = dg_add_hit(a, d, ti); if (n >= 0) { dg_select_only(d, n); dg_center_on(a, d, n, true); } }
        dg_touch(d);
    } else if (kind == 2) dg_open_picker(a, d, d->pending_schema);
    else if (kind == 4 && d->pending_list) {   /* the tables picked in the Explorer, laid out together */
        int added = 0, missing = 0; char *list = d->pending_list; d->pending_list = NULL;
        for (char *t = strtok(list, "\n"); t; t = strtok(NULL, "\n")) {
            int ti = find_table(k, d->pending_schema, t);
            if (ti < 0) { missing++; continue; }
            if (!added) dgm_key_of(&k->x, ti, d->d.focus, sizeof d->d.focus);
            dgm_add_table(&d->d, ti, 0, (float)added * 10); added++;
        }
        free(list);
        dgm_edges(&d->d); dg_measure(a, d); dg_layout(a, d, false); d->need_fit = true; dg_touch(d);
        int more = 0; for (int i = 0; i < d->d.nn; i++) more += d->d.n[i].more;
        dg_msg(d, "%d table%s%s%s", added, added == 1 ? "" : "s", missing ? " (some were not found in the scope)" : "", more ? ": +N on a card adds its related tables" : "");
    }
}
bool dgmview_open_table(App *a, Connection *c, const char *schema, const char *table) {
    if (!c) return false;
    if (!dialect_of(c)) { app_status(a, "ER diagrams are not available for %s connections yet", c->driver); return false; }
    char sc[1200]; default_scope(a, c, sc, sizeof sc);
    if (schema && *schema && strcmp(c->driver, "sqlite")) snprintf(sc, sizeof sc, "%s", schema);
    DgDoc *d = doc_new(a, c, table, sc); if (!d) return false;
    snprintf(d->pending_table, sizeof d->pending_table, "%s%s%s", schema && *schema && strcmp(c->driver, "sqlite") ? schema : "", schema && *schema && strcmp(c->driver, "sqlite") ? "." : "", table);
    d->pending_kind = 0;
    if (open_tab(a, d) < 0) { doc_free(d); return false; }
    LOG_I("diagram: opened on %s (connection %s)", table, c->name);
    return true;
}
bool dgmview_open_tables(App *a, Connection *c, const char *schema, const char *const *tables, int n) {
    if (!c || n < 1) return false;
    if (n == 1) return dgmview_open_table(a, c, schema, tables[0]);
    if (!dialect_of(c)) { app_status(a, "ER diagrams are not available for %s connections yet", c->driver); return false; }
    char sc[1200]; default_scope(a, c, sc, sizeof sc);
    if (schema && *schema && strcmp(c->driver, "sqlite")) snprintf(sc, sizeof sc, "%s", schema);
    char nm[120]; snprintf(nm, sizeof nm, "%s +%d", tables[0], n - 1);
    DgDoc *d = doc_new(a, c, nm, sc); if (!d) return false;
    size_t cap = 1; for (int i = 0; i < n; i++) cap += strlen(tables[i]) + 1;
    d->pending_list = malloc(cap); d->pending_list[0] = 0;
    for (int i = 0; i < n; i++) { strcat(d->pending_list, tables[i]); strcat(d->pending_list, "\n"); }
    snprintf(d->pending_schema, sizeof d->pending_schema, "%s", schema && strcmp(c->driver, "sqlite") ? schema : "");
    if (open_tab(a, d) < 0) { doc_free(d); return false; }
    LOG_I("diagram: opened on %d tables picked in the Explorer (connection %s)", n, c->name);
    return true;
}
bool dgmview_new_for_schema(App *a, Connection *c, const char *schema) {
    if (!c || !dialect_of(c)) { if (c) app_status(a, "ER diagrams are not available for %s connections yet", c->driver); return false; }
    char nm[160]; snprintf(nm, sizeof nm, "%s", schema && *schema ? schema : c->name);
    DgDoc *d = doc_new(a, c, nm, schema && *schema && strcmp(c->driver, "sqlite") ? schema : NULL); if (!d) return false;
    d->pending_kind = 2; snprintf(d->pending_schema, sizeof d->pending_schema, "%s", schema ? schema : "");
    if (open_tab(a, d) < 0) { doc_free(d); return false; }
    return true;
}
bool dgmview_new_empty(App *a, Connection *c) {
    if (!c) { app_status(a, "New Diagram: choose a connection first"); return false; }
    if (!dialect_of(c)) { app_status(a, "ER diagrams are not available for %s connections yet", c->driver); return false; }
    DgDoc *d = doc_new(a, c, "New diagram", NULL); if (!d) return false;
    if (open_tab(a, d) < 0) { doc_free(d); return false; }
    d->find_focus = true;
    return true;
}
bool dgmview_add_to_current(App *a, Connection *c, const char *schema, const char *table) {
    DgDoc *d = dg_active(a);
    if (!d || !c || strcmp(d->conn_id, c->id)) { for (int i = 0; i < DG_MAX_DOCS && c; i++) if (DG.docs[i] && !strcmp(DG.docs[i]->conn_id, c->id)) d = DG.docs[i]; }
    if (!d || !c || strcmp(d->conn_id, c->id)) { app_status(a, "Add to Current Diagram: no diagram is open for this connection"); return false; }
    snprintf(d->pending_table, sizeof d->pending_table, "%s%s%s", schema && *schema && strcmp(c->driver, "sqlite") ? schema : "", schema && *schema && strcmp(c->driver, "sqlite") ? "." : "", table);
    d->pending_kind = 1;
    panels_activate_diagram(a->panels, a, d);
    return true;
}
bool dgmview_has_diagram_for(App *a, const char *conn_id) { (void)a; for (int i = 0; i < DG_MAX_DOCS; i++) if (DG.docs[i] && !strcmp(DG.docs[i]->conn_id, conn_id)) return true; return false; }
bool dgmview_open_file(App *a, const char *path) {
    Dgm t; dgm_init(&t); char why[300];
    if (!dgm_load(&t, path, why, sizeof why)) { app_status(a, "Could not open the diagram: %s", why); dgm_free(&t); return false; }
    Connection *c = t.conn_id[0] ? dg_conn(a, t.conn_id) : NULL;
    DgDoc *d = doc_new(a, c, t.name[0] ? t.name : "diagram", t.schemas);
    if (!d) { dgm_free(&t); return false; }
    dgm_free(&d->d); d->d = t;
    snprintf(d->path, sizeof d->path, "%s", path);
    d->fitted = true;   /* the saved viewport */
    if (!c) dg_msg(d, "The connection %s is not configured here: the diagram opens without its catalog", t.conn_name[0] ? t.conn_name : t.conn_id);
    if (open_tab(a, d) < 0) { doc_free(d); return false; }
    return true;
}
bool dgmview_doc_dirty(const DgDoc *d) { return d && d->d.dirty; }
const char *dgmview_doc_conn(const DgDoc *d) { return d ? d->conn_id : ""; }
void dgmview_tab_closed(App *a, DgDoc *d) {
    if (d->d.dirty && d->d.nn) dg_save(a, d);   /* the layout is kept (DGM-053) */
    doc_free(d);
}

/* ---------------------------------------------------------------- frame hooks */
void dgmview_pre_update(App *a) {
    DG.a = a;
    for (int i = 0; i < DG_MAX_CATS; i++) if (DG.cats[i].used) cat_pump(a, &DG.cats[i]);
    for (int i = 0; i < DG_MAX_DOCS; i++) {
        DgDoc *d = DG.docs[i]; if (!d) continue;
        DgCat *k = dg_cat(d);
        if (k && k->state == CS_READY && d->cat_version != k->version) { dg_rebind(d); if (d->d.nn && !d->laid_out && !d->fitted) d->need_fit = true; d->laid_out = true; }
        if (k && k->state == CS_READY && (d->pending_table[0] || d->pending_kind == 2 || d->pending_list)) { if (d->pending_list) d->pending_kind = 4; run_pending(a, d); }
        lay_poll(a, d);
        if (d->ai) dg_ai_poll(a, d);
        if (d->d.dirty && d->d.nn && SDL_GetTicks() - d->dirty_at > 1000 && !d->lay_th) dg_save(a, d);
        else if (d->d.dirty) app_wake_in(a, 1100);
        if (d->anim) {
            float t = (float)(SDL_GetTicks() - d->at0) / 220.0f;
            if (t >= 1) { t = 1; d->anim = false; }
            float e = 1 - (1 - t) * (1 - t) * (1 - t);
            d->d.zoom = d->az0 + (d->az1 - d->az0) * e; d->d.panx = d->ax0 + (d->ax1 - d->ax0) * e; d->d.pany = d->ay0 + (d->ay1 - d->ay0) * e;
            if (d->anim) app_wake_in(a, 16);
        }
    }
    dg_poll_paths(a);
    DgDoc *d = dg_active(a);
    if (!d || d->dlg || ui_text_editing()) return;
    Input *in = &a->in;
    /* shortcuts of the active diagram (DGM-020 / 026 / 028 / 032) */
    if (input_key(in, SDLK_F, KM_PRIMARY)) { d->find_focus = true; d->find_list = true; app_wake_in(a, 16); }
    else if (input_key(in, SDLK_F3, 0)) dg_find_step(a, d, 1);
    else if (input_key(in, SDLK_F3, KM_SHIFT)) dg_find_step(a, d, -1);
    else if (input_key(in, SDLK_Z, KM_PRIMARY)) dg_action(a, d, DA_UNDO);
    else if (input_key(in, SDLK_Z, KM_PRIMARY | KM_SHIFT)) dg_action(a, d, DA_REDO);
    else if (input_key(in, SDLK_A, KM_PRIMARY)) dg_action(a, d, DA_SELECT_ALL);
    else if (input_key(in, SDLK_EQUALS, KM_PRIMARY) || input_key(in, SDLK_EQUALS, KM_PRIMARY | KM_SHIFT) || input_key(in, SDLK_PLUS, KM_PRIMARY | KM_SHIFT)) dg_action(a, d, DA_ZOOM_IN);
    else if (input_key(in, SDLK_MINUS, KM_PRIMARY)) dg_action(a, d, DA_ZOOM_OUT);
    else if (input_key(in, SDLK_0, KM_PRIMARY)) dg_action(a, d, DA_ZOOM_100);
    else if (input_key(in, SDLK_1, KM_PRIMARY)) dg_action(a, d, DA_FIT);
    else if (input_key(in, SDLK_2, KM_PRIMARY)) dg_action(a, d, DA_FIT_SEL);
    else if (input_key(in, SDLK_LEFTBRACKET, KM_PRIMARY)) dg_action(a, d, DA_BACK);
    else if (input_key(in, SDLK_RIGHTBRACKET, KM_PRIMARY)) dg_action(a, d, DA_FORWARD);
    else if (input_key(in, SDLK_C, KM_PRIMARY | KM_SHIFT)) dg_action(a, d, DA_COPY_IMAGE);
    else if (input_key(in, SDLK_L, KM_PRIMARY | KM_SHIFT)) dg_action(a, d, DA_LAYOUT);
    else if (input_key(in, SDLK_ESCAPE, 0)) {
        if (d->lay_th) { d->lay_cancel = 1; }
        else if (d->linking) { d->linking = false; dg_msg(d, "Relation cancelled"); }
        else if (d->isolate) dg_action(a, d, DA_RESET_FOCUS);
        else if (d->flash[0]) d->flash[0] = 0;
        else { for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = false; d->focus_node = -1; }
        app_wake_in(a, 16);
    }
    else if (input_key(in, SDLK_DELETE, 0) || input_key(in, SDLK_BACKSPACE, 0)) dg_action(a, d, DA_REMOVE);
    else if (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) { if (d->focus_node >= 0) { d->props = true; dg_select_only(d, d->focus_node); } }
    else if (input_key(in, SDLK_TAB, 0) || input_key(in, SDLK_TAB, KM_SHIFT)) {   /* cards in reading order */
        int n = d->d.nn; if (n) {
            bool back = false; for (int k = 0; k < in->nkeys; k++) (void)k;
            int cur = d->focus_node, best = -1; float bv = 0;
            float cy = cur >= 0 ? d->d.n[cur].y : -1e9f, cx = cur >= 0 ? d->d.n[cur].x : -1e9f;
            for (int i = 0; i < n; i++) {
                float v = floorf(d->d.n[i].y / 200) * 1e6f + d->d.n[i].x, c0 = floorf(cy / 200) * 1e6f + cx;
                if ((back ? v < c0 : v > c0) && (best < 0 || (back ? v > bv : v < bv))) { best = i; bv = v; }
            }
            if (best < 0) best = 0;
            d->focus_node = best; d->focus_row = -1; dg_select_only(d, best); dg_center_on(a, d, best, true);
        }
    }
    else {   /* arrows: the nearest card in that direction (DGM-026) */
        int dir = input_key(in, SDLK_LEFT, 0) ? 0 : input_key(in, SDLK_RIGHT, 0) ? 1 : input_key(in, SDLK_UP, 0) ? 2 : input_key(in, SDLK_DOWN, 0) ? 3 : -1;
        if (dir >= 0 && d->d.nn) {
            int cur = d->focus_node >= 0 ? d->focus_node : 0;
            if (d->focus_node < 0) { d->focus_node = 0; dg_select_only(d, 0); dg_center_on(a, d, 0, true); return; }
            DgNode *c = &d->d.n[cur]; float cx = c->x + c->w / 2, cy = c->y + c->h / 2;
            int best = -1; float bd = 1e30f;
            for (int i = 0; i < d->d.nn; i++) {
                if (i == cur) continue;
                DgNode *o = &d->d.n[i]; float ox = o->x + o->w / 2 - cx, oy = o->y + o->h / 2 - cy;
                bool ok = dir == 0 ? ox < -1 && fabsf(oy) <= fabsf(ox) * 1.5f : dir == 1 ? ox > 1 && fabsf(oy) <= fabsf(ox) * 1.5f : dir == 2 ? oy < -1 && fabsf(ox) <= fabsf(oy) * 1.5f : oy > 1 && fabsf(ox) <= fabsf(oy) * 1.5f;
                float dd = ox * ox + oy * oy;
                if (ok && dd < bd) { bd = dd; best = i; }
            }
            if (best >= 0) { d->focus_node = best; dg_select_only(d, best); dg_center_on(a, d, best, true); }
        }
    }
}
void dgmview_draw_overlay(App *a) {
    DgDoc *d = dg_active(a);
    if (!d) return;
    dg_menus(a, d);
}

/* ---------------------------------------------------------------- commands */
static bool en_diagram(const LumeCtx *c, char *why, size_t n) { (void)c; if (!DG.a || !dg_active(DG.a)) { snprintf(why, n, "No diagram is active"); return false; } return true; }
static bool en_conn(const LumeCtx *c, char *why, size_t n) { (void)c; if (!DG.a || !DG.a->active) { snprintf(why, n, "Choose a connection first"); return false; } if (!dialect_of(DG.a->active)) { snprintf(why, n, "Not available for this driver yet"); return false; } return true; }
static void inv(void *ud, const LumeCtx *c) {
    (void)c; App *a = DG.a; if (!a) return;
    int act = (int)(intptr_t)ud;
    if (act == -1) { dgmview_new_empty(a, a->active); return; }
    if (act == -2) { dgmview_new_for_schema(a, a->active, NULL); return; }
    if (act == -3) { DgDoc *d = dg_active(a); if (d) dg_ask_path(a, d, 1, EXF_JSON); else dg_ask_path(a, NULL, 1, EXF_JSON); return; }
    DgDoc *d = dg_active(a); if (d) dg_action(a, d, act);
}
void dgmview_init(App *a) {
    DG.a = a;
    DG.reduce_motion = getenv("LUME_REDUCE_MOTION") != NULL;
    static const struct { const char *id, *label, *syn; int act; bool conn; } cmds[] = {
        { "dgm.new", "New ER Diagram", "erd diagram model entity relationship der mer", -1, true },
        { "dgm.new_schema", "New ER Diagram from Schema\xE2\x80\xA6", "erd diagram schema tables", -2, true },
        { "dgm.open", "Open ER Diagram\xE2\x80\xA6", "erd diagram file", -3, false },
        { "dgm.find", "Diagram: Find\xE2\x80\xA6", "search table column", DA_FIND, false },
        { "dgm.layout", "Diagram: Auto Layout", "arrange organize", DA_LAYOUT, false },
        { "dgm.fit", "Diagram: Fit Content", "zoom all", DA_FIT, false },
        { "dgm.fit_selection", "Diagram: Fit Selection", "zoom selected", DA_FIT_SEL, false },
        { "dgm.export", "Diagram: Export\xE2\x80\xA6", "png svg pdf mermaid graphml drawio html image", DA_EXPORT, false },
        { "dgm.copy_image", "Diagram: Copy as Image", "clipboard png", DA_COPY_IMAGE, false },
        { "dgm.scope", "Diagram: Scope\xE2\x80\xA6", "schemas filter", DA_SCOPE, false },
        { "dgm.refresh", "Diagram: Refresh Structure", "reload metadata catalog", DA_REFRESH, false },
        { "dgm.minimap", "Diagram: Toggle Minimap", "overview", DA_MINIMAP, false },
        { "dgm.properties", "Diagram: Toggle Properties", "details inspector", DA_PROPS, false },
        { "dgm.back", "Diagram: Back", "history previous", DA_BACK, false },
        { "dgm.forward", "Diagram: Forward", "history next", DA_FORWARD, false },
        { "dgm.compare", "Diagram: Compare with Saved Snapshot", "changes diff schema", DA_COMPARE, false },
        { "dgm.duplicate", "Diagram: Duplicate", "copy thematic view", DA_DUPLICATE, false },
        { "dgm.note", "Diagram: Add Note", "annotation comment", DA_NOTE, false },
        { "dgm.area", "Diagram: Add Group Area", "domain module color", DA_AREA, false },
        { "dgm.entity", "Diagram: Add Logical Entity", "conceptual mer model", DA_ENTITY, false },
        { "dgm.conceptual", "Diagram: Toggle Business Names", "conceptual view mer", DA_CONCEPTUAL, false },
        { "dgm.explain", "Diagram: Explain with Lume Intelligence", "ai assistant module", DA_AI_EXPLAIN, false },
        { "dgm.organise", "Diagram: Suggest Domain Groups (Lume Intelligence)", "ai organize arrange", DA_AI_ORGANISE, false },
        { "dgm.conceptual_ai", "Diagram: Suggest Conceptual Map (Lume Intelligence)", "ai business mer", DA_AI_CONCEPTUAL, false },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        Command c; memset(&c, 0, sizeof c);
        c.id = cmds[i].id; c.label = cmds[i].label; c.group = "Diagram"; c.synonyms = cmds[i].syn; c.icon = "diagram";
        c.enabled = cmds[i].conn ? en_conn : cmds[i].act == -3 ? NULL : en_diagram; c.invoke = inv; c.ud = (void *)(intptr_t)cmds[i].act; c.module = "lume.diagrams";
        commands_register(&c);
    }
}
void dgmview_shutdown(App *a) {
    (void)a;
    for (int i = 0; i < DG_MAX_DOCS; i++) if (DG.docs[i]) { if (DG.docs[i]->d.dirty && DG.docs[i]->d.nn) dg_save(a, DG.docs[i]); doc_free(DG.docs[i]); }
    for (int i = 0; i < DG_MAX_CATS; i++) if (DG.cats[i].used) { cat_free(&DG.cats[i]); DG.cats[i].used = false; }
    for (int i = 0; i < 12; i++) if (DG.fonts[i].f) { font_destroy(DG.fonts[i].f); DG.fonts[i].f = NULL; }
}

/* ---------------------------------------------------------------- harness */
static DgDoc *doc_for_cmd(App *a) { DgDoc *d = dg_active(a); return d; }
static int node_arg(DgDoc *d, const char *s) { return dgm_find_node(&d->d, s); }
bool dgmview_ready(App *a) {
    DgDoc *d = dg_active(a); DgCat *k = dg_cat(d);
    return d && k && k->state == CS_READY && d->cat_version == k->version && !d->pending_table[0] && !d->pending_list && !d->lay_th && !d->ai;
}
void dgmview_cmd(App *a, const char *arg) {
    char op[32] = "", r1[400] = "", r2[400] = "", r3[400] = "";
    const char *p = arg;
    /* op, then up to three arguments, each quoted or a single word */
    char *outs[4] = { op, r1, r2, r3 }; size_t sz[4] = { sizeof op, sizeof r1, sizeof r2, sizeof r3 };
    for (int i = 0; i < 4; i++) {
        while (*p == ' ') p++;
        if (!*p) break;
        size_t k = 0;
        if (*p == '"') { p++; while (*p && *p != '"' && k + 1 < sz[i]) outs[i][k++] = *p++; if (*p == '"') p++; }
        else while (*p && *p != ' ' && k + 1 < sz[i]) outs[i][k++] = *p++;
        outs[i][k] = 0;
    }
    DgDoc *d = doc_for_cmd(a);
    Connection *c = a->active;
    if (!strcmp(op, "open")) { Connection *cc = r2[0] ? NULL : c; for (int i = 0; r2[0] && a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, r2)) cc = a->conns->items[i]; char *dot = strrchr(r1, '.'); if (dot && cc && strcmp(cc->driver, "sqlite")) { *dot = 0; dgmview_open_table(a, cc, r1, dot + 1); } else dgmview_open_table(a, cc, "", r1); return; }
    if (!strcmp(op, "new") || !strcmp(op, "empty")) {
        const char *cn = !strcmp(op, "new") ? r2 : r1;
        for (int i = 0; cn[0] && a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, cn)) c = a->conns->items[i];
        if (!strcmp(op, "new")) dgmview_new_for_schema(a, c, r1); else dgmview_new_empty(a, c);
        return;
    }
    if (!strcmp(op, "openfile")) { dgmview_open_file(a, r1); return; }
    if (!d) { printf("dgm: no active diagram for \"%s\"\n", op); fflush(stdout); return; }
    DgCat *k = dg_cat(d);
    if (!strcmp(op, "find")) { textfield_set(&d->find, r1); d->find_list = true; dg_find_run(a, d); }
    else if (!strcmp(op, "flags")) { d->find_flags = atoi(r1); d->last_cat = 0; dg_find_run(a, d); }
    else if (!strcmp(op, "next")) dg_find_step(a, d, 1);
    else if (!strcmp(op, "prev")) dg_find_step(a, d, -1);
    else if (!strcmp(op, "add")) { int ti = k && k->state == CS_READY ? schidx_find(&k->x, NULL, r1) : -1; if (ti >= 0) { int n = dg_add_hit(a, d, ti); dg_select_only(d, n); } else printf("dgm: no table %s\n", r1); }
    else if (!strcmp(op, "expand")) { int ti = k && k->state == CS_READY ? schidx_find(&k->x, NULL, r1) : -1; if (ti >= 0) dg_expand(a, d, ti, r2[0] ? atoi(r2) : 1); }
    else if (!strcmp(op, "confirm")) { if (d->dlg == DLG_CONFIRM_ADD) dg_dialog_ok(a, d); }
    else if (!strcmp(op, "cancel")) d->dlg = DLG_NONE;
    else if (!strcmp(op, "sel")) { int n = node_arg(d, r1); if (r2[0] == '+') { if (n >= 0) d->d.n[n].sel = true; } else dg_select_only(d, n); }
    else if (!strcmp(op, "move")) { int n = node_arg(d, r1); if (n >= 0) { dgm_snapshot(&d->d); d->d.n[n].x += (float)atof(r2); d->d.n[n].y += (float)atof(r3); d->d.grid_ok = false; dg_touch(d); } }
    else if (!strcmp(op, "zoom")) { d->d.zoom = (float)atof(r1); d->anim = false; }
    else if (!strcmp(op, "pan")) { d->d.panx = (float)atof(r1); d->d.pany = (float)atof(r2); d->anim = false; }
    else if (!strcmp(op, "export")) { char why[300]; int fmt = EXF_PNG; for (int f = 0; f < EXF_COUNT; f++) if (!strcmp(r1, dg_exf_ext(f))) fmt = f; if (!strcmp(r1, "image")) { dg_action(a, d, DA_COPY_IMAGE); return; }
        int sc = !strcmp(r3, "selection") ? EXS_SELECTION : !strcmp(r3, "visible") ? EXS_VISIBLE : EXS_DIAGRAM;
        if (dg_export(a, d, fmt, sc, r2, why, sizeof why)) printf("dgm: exported %s\n", r2); else printf("dgm: export failed: %s\n", why); }
    else if (!strcmp(op, "save")) { if (r1[0]) snprintf(d->path, sizeof d->path, "%s", r1); dg_save(a, d); printf("dgm: saved %s\n", d->path); }
    else if (!strcmp(op, "menu")) { int n = node_arg(d, r2); float sx, sy; if (n >= 0) { dg_world_to_screen(d, a, d->d.n[n].x + 10, d->d.n[n].y + 10, &sx, &sy); } else { sx = d->canvas.x + 40; sy = d->canvas.y + 40; } dg_open_menu(a, d, !strcmp(r1, "card") ? 1 : !strcmp(r1, "edge") ? 2 : !strcmp(r1, "note") ? 3 : 0, n, !strcmp(r1, "edge") ? atoi(r2) : -1, !strcmp(r1, "note") ? atoi(r2) : -1, sx, sy); }
    else if (!strcmp(op, "act")) {
        static const struct { const char *n; int a; } names[] = {
            { "find", DA_FIND }, { "layout", DA_LAYOUT }, { "layout_sel", DA_LAYOUT_SEL }, { "fit", DA_FIT }, { "fit_sel", DA_FIT_SEL }, { "zoom_in", DA_ZOOM_IN }, { "zoom_out", DA_ZOOM_OUT }, { "zoom_100", DA_ZOOM_100 },
            { "mode_name", DA_MODE_NAME }, { "mode_keys", DA_MODE_KEYS }, { "mode_all", DA_MODE_ALL }, { "card_all", DA_CARD_ALL }, { "card_name", DA_CARD_NAME }, { "remove", DA_REMOVE },
            { "neigh1", DA_NEIGH1 }, { "neigh2", DA_NEIGH2 }, { "neigh3", DA_NEIGH3 }, { "focus_here", DA_FOCUS_HERE }, { "pin", DA_PIN }, { "alias", DA_ALIAS }, { "copy_name", DA_COPY_NAME },
            { "open_data", DA_OPEN_DATA }, { "edit_table", DA_EDIT_TABLE }, { "ddl", DA_OPEN_DEFINITION }, { "refresh", DA_REFRESH }, { "scope", DA_SCOPE }, { "export", DA_EXPORT },
            { "select_all", DA_SELECT_ALL }, { "undo", DA_UNDO }, { "redo", DA_REDO }, { "back", DA_BACK }, { "forward", DA_FORWARD }, { "minimap", DA_MINIMAP }, { "props", DA_PROPS },
            { "isolate", DA_ISOLATE }, { "reset_focus", DA_RESET_FOCUS }, { "disconnected", DA_DISCONNECTED }, { "show_hidden", DA_SHOW_HIDDEN }, { "toggle_virtual", DA_TOGGLE_VIRTUAL },
            { "note", DA_NOTE }, { "area", DA_AREA }, { "entity", DA_ENTITY }, { "notation", DA_NOTATION }, { "routing", DA_ROUTING }, { "snap", DA_SNAP }, { "conceptual", DA_CONCEPTUAL },
            { "align_left", DA_ALIGN_L }, { "align_top", DA_ALIGN_T }, { "dist_h", DA_DIST_H }, { "components", DA_GROUP_COMPONENTS }, { "duplicate", DA_DUPLICATE }, { "save_subset", DA_SAVE_SUBSET },
            { "snapshot", DA_SNAPSHOT }, { "compare", DA_COMPARE }, { "copy_image", DA_COPY_IMAGE }, { "rename", DA_RENAME }, { "highlight_all", DA_HIGHLIGHT_ALL }, { "virtual", DA_VIRTUAL },
            { "types", DA_TYPES }, { "comments", DA_COMMENTS }, { "nulls", DA_NULLS },
        };
        int act = atoi(r1);
        for (size_t q = 0; q < sizeof names / sizeof names[0]; q++) if (!strcmp(names[q].n, r1)) act = names[q].a; if (r2[0]) { int n = node_arg(d, r2); if (n >= 0) { dg_select_only(d, n); d->cm_node = n; } } dg_action(a, d, act); }
    else if (!strcmp(op, "prompt")) { if (d->dlg == DLG_PROMPT) { textfield_set(&d->pf, r1); if (r2[0]) textfield_set(&d->pf2, r2); d->pf.len = (int)strlen(d->pf.buf); dg_dialogs(a, d); } }
    else if (!strcmp(op, "ok")) dg_dialog_ok(a, d);   /* the dialog's main button, from the harness */
    else if (!strcmp(op, "rel")) { int r = dgm_add_rel(&d->d, DE_VIRTUAL, r1, r2, r3, NULL, NULL); if (r >= 0) { snprintf(d->d.rels[r].author, sizeof d->d.rels[r].author, "user"); dgm_edges(&d->d); dg_touch(d); } }
    else if (!strcmp(op, "alias")) { int n = node_arg(d, r1); if (n >= 0) { snprintf(d->d.n[n].alias, sizeof d->d.n[n].alias, "%s", r2); dg_touch(d); dg_measure(a, d); } }
    else if (!strcmp(op, "note")) { float x, y; dg_screen_to_world(d, a, d->canvas.x + 60, d->canvas.y + 60, &x, &y); dgm_add_note(&d->d, !strcmp(r2, "area"), x, y, 240, !strcmp(r2, "area") ? 200 : 90, r1, r3); dg_touch(d); }
    else if (!strcmp(op, "entity")) { float x, y; dg_screen_to_world(d, a, d->canvas.x + 80, d->canvas.y + 80, &x, &y); int n = dgm_add_entity(&d->d, r1, x, y); if (n >= 0) { snprintf(d->d.n[n].attrs, sizeof d->d.n[n].attrs, "%s", r2); for (char *q = d->d.n[n].attrs; *q; q++) if (*q == ';') *q = '\n'; dg_measure(a, d); dgm_edges(&d->d); dg_touch(d); } }
    else if (!strcmp(op, "hide")) { dgm_toggle_hidden(&d->d, r1); dg_touch(d); }
    else if (!strcmp(op, "scope")) dg_set_scope(a, d, r1);
    else if (!strcmp(op, "refresh")) dg_refresh(a, d, true);
    else if (!strcmp(op, "ai")) dg_ai_start(a, d, !strcmp(r1, "explain") ? AI_EXPLAIN : !strcmp(r1, "organise") ? AI_ORGANISE : AI_CONCEPTUAL);
    else if (!strcmp(op, "aisend")) { if (d->dlg == DLG_AI_CONSENT) { dg_dialog_ok(a, d); } }
    else if (!strcmp(op, "accept")) dg_ai_accept(a, d, atoi(r1), true);
    else if (!strcmp(op, "reject")) dg_ai_accept(a, d, atoi(r1), false);
    else if (!strcmp(op, "pick")) { /* the schema picker: choose its first N (most connected) */ if (d->dlg == DLG_PICKER) { int nn = atoi(r1); for (int i = 0; i < d->npick; i++) d->pick_on[i] = i < nn; dg_dialog_ok(a, d); } }
    else if (!strcmp(op, "frames")) { d->nframe = 0; }
    else { printf("dgm: unknown op \"%s\"\n", op); }
    fflush(stdout);
    app_wake_in(a, 16);
}
static int cmpd(const void *x, const void *y) { double p = *(const double *)x, q = *(const double *)y; return p < q ? -1 : p > q; }
void dgmview_dump(App *a) {
    DgDoc *d = dg_active(a);
    if (!d) { printf("dgmstate none\n"); fflush(stdout); return; }
    DgCat *k = dg_cat(d);
    int nsel = dg_nsel(d), nvirt = 0, nfk = 0, nhid = 0, nstub = 0, nent = 0;
    for (int e = 0; e < d->d.ne; e++) { if (d->d.e[e].origin == DE_FK) nfk++; else nvirt++; nhid += d->d.e[e].hidden; }
    for (int i = 0; i < d->d.nn; i++) { nstub += d->d.n[i].kind == DN_STUB; nent += d->d.n[i].kind == DN_ENTITY; }
    double v[240]; int nf = d->nframe < 240 ? d->nframe : 240; memcpy(v, d->frame_ms, sizeof(double) * (size_t)nf); qsort(v, (size_t)nf, sizeof(double), cmpd);
    char sel[DG_KEY] = ""; for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel) { snprintf(sel, sizeof sel, "%s", d->d.n[i].key); break; }
    printf("dgmstate id=%d name=\"%s\" cat=%d stale=%d scope=\"%s\" in_scope=%d statements=%d nodes=%d stubs=%d entities=%d edges=%d fk=%d user_rel=%d hidden=%d sel=%d first_sel=\"%s\" focus=%d zoom=%.3f pan=%.0f,%.0f lod=%d drawn_cards=%d drawn_edges=%d drawn_labels=%d "
           "find=\"%s\" hits=%d cur=%d flash=\"%s\" dlg=%d dlg_title=\"%s\" menu=%d dirty=%d path=\"%s\" notes=%d rels=%d minimap=%d props=%d isolate=%d conceptual=%d hist=%d/%d frames=%d frame_p95=%.2f frame_p99=%.2f undo=%d/%d msg=\"%s\" err=\"%s\"\n",
           d->id, d->d.name, k ? k->state : -1, k ? k->stale : 0, d->d.schemas, k && k->state == CS_READY ? k->x.n : 0, k ? k->statements : 0, d->d.nn, nstub, nent, d->d.ne, nfk, nvirt, nhid, nsel, sel, d->focus_node, d->d.zoom, d->d.panx, d->d.pany, d->lod,
           d->drawn_cards, d->drawn_edges, d->drawn_labels, d->find.buf, d->total, d->cur, d->flash, d->dlg, d->dlg_title, d->cm.open, d->d.dirty, d->path, d->d.nnotes, d->d.nrels, d->minimap, d->props, d->isolate, d->d.conceptual,
           d->ihist, d->nhist, nf, nf ? v[(int)(nf * 0.95)] : 0, nf ? v[(int)(nf * 0.99) < nf ? (int)(nf * 0.99) : nf - 1] : 0, d->d.iundo, d->d.nundo, d->msg, k ? k->err : "");
    for (int i = 0; i < d->d.nn && i < 400; i++) {
        DgNode *n = &d->d.n[i];
        printf("dgmnode \"%s\" kind=%d x=%.0f y=%.0f w=%.0f h=%.0f sel=%d more=%d status=%d pinned=%d alias=\"%s\" mode=%d\n", n->key, n->kind, n->x, n->y, n->w, n->h, n->sel, n->more, n->status, n->pinned, n->alias, dgm_card_mode(&d->d, i));
    }
    for (int e = 0; e < d->d.ne && e < 400; e++) {
        DgEdge *g = &d->d.e[e];
        printf("dgmedge \"%s\" \"%s\" -> \"%s\" origin=%d card=%d:%d optional=%d hidden=%d cols=\"%s\" ref=\"%s\"\n", g->name, d->d.n[g->from].key, d->d.n[g->to].key, g->origin, g->card_from, g->card_to, g->optional, g->hidden, g->cols, g->ref_cols);
    }
    for (int i = 0; i < d->nhits && i < 50; i++) printf("dgmhit %d \"%s\" on_diagram=%d what=%d\n", i, d->hits[i].text, d->hits[i].node >= 0, d->hits[i].what);
    if (d->ai_text) { char t[300]; snprintf(t, sizeof t, "%s", d->ai_text); for (char *q = t; *q; q++) if (*q == '\n' || *q == '"') *q = ' '; printf("dgmai \"%s\"\n", t); }
    for (int i = 0; i < d->nprops_ai; i++) printf("dgmproposal %d \"%s\" tables=\"%s\" accepted=%d rejected=%d\n", i, d->props_ai[i].title, d->props_ai[i].tables, d->props_ai[i].accepted, d->props_ai[i].rejected);
    fflush(stdout);
}
