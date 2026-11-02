/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Execution Plan panel: see planview.h.
 *
 *   capture   plan_capture_sql -> one exec job on the tab's session (never the SDL thread);
 *             Output records start / success / failure / cancel under one execution (EPL-003 / 005)
 *   views     Graph (canvas, zoom / pan / fit, culling, properties), Operators (table, sortable,
 *             selection focuses the node), Raw, Findings (evidence, index candidates), AI
 *   history   the last 20 plans of the session, in memory only (EPL-007); pinned ones stay
 *   AI        context preview with share toggles, explicit Send, model + date in the answer,
 *             candidate SQL opened as a copy, audit line without content (EPL-043..054 / 073..077)
 */
#include "planview.h"
#include "telemetry.h"
#include "rptview.h"
#include "ai.h"
#include "commands.h"
#include "conn.h"
#include "draw.h"
#include "exec.h"
#include "font.h"
#include "log.h"
#include "output.h"
#include "outlog.h"
#include "panels.h"
#include "plan.h"
#include "results.h"
#include "tdes.h"
#include "theme.h"
#include "ui.h"
#include <ctype.h>
#include <errno.h>
#include <math.h>
#include <stdarg.h>
#include <sys/stat.h>
#include <time.h>

#define MAX_PLANS 20
enum { V_GRAPH, V_OPS, V_RAW, V_FINDINGS, V_AI, V_N };

typedef struct PView {
    Plan p;
    PlanFinding f[64]; int nf;
    bool used, pinned;
    int seq;                     /* #n shown to the user */
    float zoom, panx, pany; bool fitted;
    bool user_view; float fit_w, fit_h;   /* the user zoomed / panned: no automatic re-fit when the panel resizes */
    int sel;                     /* node id, -1 none */
    char conn_id[32];
    /* AI session: bound to this snapshot (EPL-054) */
    bool share_sql, share_lit, share_names, share_raw, share_subtree;
    TextField question;
    AiJob *ai; char *answer; char answer_hdr[200]; char ai_err[600];
    char sqlc[4][4096]; int nsql;
} PView;

static struct {
    App *a;
    PView v[MAX_PLANS]; int cur, cmp, seq;
    int view; bool rtl;
    TextField search; int search_hit;
    float ops_scroll, raw_scroll, f_scroll, ai_scroll; int ops_sort; bool ops_desc;
    ScrollBar ops_sb, f_sb, pane_sb[4]; float *pane_key[4]; float f_total;
    /* capture job */
    ExecJob *job; PlanSql ps; int mode; char conn_id[32], conn_name[64]; char *stmt; int out_exec; double t0; char note[300];
    int cmp_after;               /* seq of the plan a candidate's estimated plan is compared with when it lands (0 none) */
    char hypo_ddl[700]; bool hypo;   /* roadmap item 16: the next capture plans with this hypothetical index (PostgreSQL + hypopg) */
    char status[600];
    bool dragging; float dx, dy;
    AiConfig ai;
    PopupMenu menu;
    bool thick;                  /* edge thickness by rows (EPL-029), on by default */
    int style;                   /* 0 SQL Server Management Studio look (default), 1 compact boxes */
} G;

static PView *curv(void) { return G.cur >= 0 && G.cur < MAX_PLANS && G.v[G.cur].used ? &G.v[G.cur] : NULL; }
static void relayout(Plan *p) { if (G.style == 0) plan_layout_rows(p); else plan_layout(p, G.rtl); }
static Connection *conn_of(App *a, const char *id) { for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i]; return NULL; }
static void pref_dir(App *a, char *o, int n) { snprintf(o, (size_t)n, "%s", a->conns && a->conns->dir[0] ? a->conns->dir : "."); size_t L = strlen(o); if (L > 1 && o[L - 1] == '/') o[L - 1] = 0; }
static uint64_t fnv(const char *s) { uint64_t h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }

bool planview_supported(const char *driver, bool actual) { int d = plan_dialect_for_driver(driver); return d >= 0 && (!actual || plan_caps(d).actual); }

/* ---- capture ---- */

static PView *new_slot(void) {
    /* LRU: the oldest unpinned plan goes (EPL-007) */
    int slot = -1, oldest = -1, oseq = 1 << 30;
    for (int i = 0; i < MAX_PLANS; i++) { if (!G.v[i].used) { slot = i; break; } if (!G.v[i].pinned && G.v[i].seq < oseq) { oseq = G.v[i].seq; oldest = i; } }
    if (slot < 0) slot = oldest;
    if (slot < 0) return NULL;
    PView *v = &G.v[slot];
    if (v->used) { plan_free(&v->p); if (v->ai) ai_free(v->ai); free(v->answer); }
    memset(v, 0, sizeof *v);
    v->used = true; v->seq = ++G.seq; v->zoom = 1; v->sel = -1;
    G.cur = slot;
    return v;
}
void planview_explain(App *a, Connection *c, const char *stmt, int len, int mode, const char *note) {
    telemetry_count(TM_PLAN);
    if (!c || !c->db) { app_status(a, "Explain: not connected"); return; }
    if (G.job) { app_status(a, "A plan is still being captured: wait or cancel it"); return; }
    int d = plan_dialect_for_driver(c->driver);
    if (d < 0) { app_status(a, "Explain: no execution plans for %s", c->driver); return; }
    char *st = malloc((size_t)len + 1); memcpy(st, stmt, (size_t)len); st[len] = 0;
    char why[300] = "";
    /* protected environments: an actual plan of a statement that writes is not offered at all (EPL-072, scenario 02) */
    if (mode == PM_ACTUAL && plan_stmt_writes(st) && (c->env == CONN_ENV_PROD || c->read_only)) {
        app_status(a, "Explain Actual refused on %s (%s): it would execute a statement that writes. Use Explain Plan (estimated, not run).", c->name, c->read_only ? "read-only" : "PROD");
        snprintf(G.status, sizeof G.status, "Explain Actual refused on %s: the statement writes and the connection is %s; ANALYZE would execute it", c->name, c->read_only ? "read-only" : "PROD");
        LOG_I("plan: actual plan of a writing statement refused on a protected connection");
        free(st); return;
    }
    plan_sql_free(&G.ps);
    if (!plan_capture_sql(d, mode, st, 0, &G.ps, why, sizeof why)) { app_status(a, "Explain: %s", why); free(st); return; }
    G.hypo = false;
    if (G.hypo_ddl[0] && d == PL_PG && mode == PM_ESTIMATED && G.ps.n == 1) {   /* hypothetical index: created in this session only, then reset */
        char esc[1500]; size_t w = 0; for (const char *q = G.hypo_ddl; *q && w + 3 < sizeof esc; q++) { if (*q == '\'') esc[w++] = '\''; esc[w++] = *q; } esc[w] = 0;
        size_t L = strlen(esc) + 64; char *c1 = malloc(L); snprintf(c1, L, "SELECT * FROM hypopg_create_index('%s')", esc);
        G.ps.sql[2] = strdup("SELECT hypopg_reset()"); G.ps.sql[1] = G.ps.sql[0]; G.ps.sql[0] = c1; G.ps.n = 3; G.ps.result = 1; G.ps.finally_last = true;
        snprintf(G.ps.note, sizeof G.ps.note, "Plans the statement with a hypothetical index (hypopg): nothing is created, the index exists only for this EXPLAIN, then hypopg_reset()");
        G.hypo = true;
    }
    G.hypo_ddl[0] = 0;
    c->db->want_last_result = G.ps.last_result;   /* read by the SQL Server driver only; cleared when the capture ends */
    G.job = exec_start_list(c->db, (const char *const *)G.ps.sql, G.ps.n, 100000, true, G.ps.finally_last);
    if (!G.job) c->db->want_last_result = false;
    if (!G.job) { app_status(a, "Explain: the session is busy (a statement is running on %s)", c->name); free(st); plan_sql_free(&G.ps); return; }
    free(G.stmt); G.stmt = st;
    G.mode = mode; snprintf(G.conn_id, sizeof G.conn_id, "%s", c->id); snprintf(G.conn_name, sizeof G.conn_name, "%s", c->name);
    snprintf(G.note, sizeof G.note, "%s", note && *note ? note : mode == PM_ACTUAL ? "Actual execution plan" : "Execution plan");
    G.t0 = SDL_GetTicks() / 1000.0;
    G.out_exec = output_obj_begin(a->output, a, c, G.note, 1);
    output_obj_event(a->output, a, c, G.out_exec, 0, OL_INFO, G.ps.note, NULL);   /* the diagnostic commands said up front (EPL-071.3) */
    for (int i = 0; i < G.ps.n; i++) output_obj_event(a->output, a, c, G.out_exec, 0, OL_INFO, "Plan statement", G.ps.sql[i]);
    snprintf(G.status, sizeof G.status, "Capturing the %s plan on %s...", mode == PM_ACTUAL ? "actual" : "estimated", c->name);
    LOG_I("plan: %s capture on %s", mode == PM_ACTUAL ? "actual" : "estimated", c->name);
    panels_show(a->panels, a, "lume.data.plan");
}
static void finish_capture(App *a) {
    Connection *c = conn_of(a, G.conn_id);
    if (c && c->db) c->db->want_last_result = false;
    double ms = (SDL_GetTicks() / 1000.0 - G.t0) * 1000;
    ResultSet rs[PLAN_MAXSQL]; ResultSet *rp[PLAN_MAXSQL]; int n = G.job->n;
    char err[600] = "";
    bool cancelled = exec_overall(G.job) == 5;
    for (int i = 0; i < n && i < PLAN_MAXSQL; i++) { rs_init(&rs[i]); exec_take_result(G.job, i, &rs[i]); rp[i] = &rs[i]; if (!err[0] && rs[i].error[0] && i != n - 1) snprintf(err, sizeof err, "%s", rs[i].error); }
    if (!err[0] && G.ps.result < n && rs[G.ps.result].error[0]) snprintf(err, sizeof err, "%s", rs[G.ps.result].error);
    PView *v = NULL;
    if (!err[0] && !cancelled) {
        v = new_slot();
        plan_init(&v->p);
        if (!plan_parse(plan_dialect_for_driver(c ? c->driver : ""), G.mode, rp, n, G.ps.result, &v->p, err, sizeof err)) { plan_free(&v->p); v->used = false; v = NULL; }
    }
    if (v) {
        v->p.sql = G.stmt; G.stmt = NULL; v->p.sql_fp = fnv(v->p.sql);
        snprintf(v->p.conn_name, sizeof v->p.conn_name, "%s", G.conn_name); snprintf(v->conn_id, sizeof v->conn_id, "%s", G.conn_id);
        v->p.exec_id = G.out_exec; v->p.stmt_id = 1; v->p.captured_at = (double)time(NULL);
        if (c && c->db && !strcmp(c->driver, "postgres")) { /* version for the summary comes from the plan rows when present */ }
        relayout(&v->p);
        v->nf = plan_diagnose(&v->p, NULL, v->f, 64);
        snprintf(G.status, sizeof G.status, "Plan #%d: %d operator(s), %d finding(s)", v->seq, v->p.n, v->nf);
        if (G.cmp_after && G.hypo) {   /* the same statement with a hypothetical index, beside the plan without it */
            for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used && G.v[i].seq == G.cmp_after) G.cmp = i;
            G.view = V_GRAPH;
            char ca[32], cb[32]; const PView *o = G.cmp >= 0 ? &G.v[G.cmp] : NULL;
            plan_num_fmt(v->p.total_cost, ca, sizeof ca, 2); if (o) plan_num_fmt(o->p.total_cost, cb, sizeof cb, 2); else snprintf(cb, sizeof cb, "?");
            snprintf(G.status, sizeof G.status, "Plan #%d with the hypothetical index vs #%d without it: cost %s vs %s (estimated; no index was created).", v->seq, G.cmp_after, ca, cb);
            G.cmp_after = 0;
        }
        if (G.cmp_after) {   /* a candidate from Lume Intelligence: side by side with the plan it rewrites */
            for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used && G.v[i].seq == G.cmp_after) G.cmp = i;
            G.view = V_GRAPH;
            char ca[32], cb[32]; const PView *o = G.cmp >= 0 ? &G.v[G.cmp] : NULL;
            plan_num_fmt(v->p.total_cost, ca, sizeof ca, 2); if (o) plan_num_fmt(o->p.total_cost, cb, sizeof cb, 2); else snprintf(cb, sizeof cb, "?");
            snprintf(G.status, sizeof G.status, "Candidate plan #%d vs #%d (estimated, nothing ran): cost %s vs %s. Results equivalence is not verified.", v->seq, G.cmp_after, ca, cb);
            G.cmp_after = 0;
        }
        if (c) { output_obj_event(a->output, a, c, G.out_exec, 1, OL_SUCCESS, G.status, NULL); output_obj_finish(a->output, a, c, G.out_exec, ES_COMPLETED, ms); }
    } else {
        char hint[700];
        /* specific guidance (EPL-005.2) */
        if (cancelled) snprintf(hint, sizeof hint, "Plan capture cancelled; the editor and the results are unaffected");
        else if (strcasestr(err, "permission") || strstr(err, "ORA-01031") || strstr(err, "SHOWPLAN")) snprintf(hint, sizeof hint, "No permission to read the plan: %s", err);
        else if (G.hypo && strstr(err, "hypopg")) snprintf(hint, sizeof hint, "Estimating with a hypothetical index needs the hypopg extension in this database (CREATE EXTENSION hypopg, by an administrator). Nothing was created.");
        else if (strstr(err, "ORA-00942") || strstr(err, "V$")) snprintf(hint, sizeof hint, "%s (actual plans need SELECT on V$SQL_PLAN_STATISTICS_ALL, V$SQL, V$SESSION)", err);
        else snprintf(hint, sizeof hint, "The plan could not be captured: %s", err);
        snprintf(G.status, sizeof G.status, "%s", hint);
        if (c) { output_obj_event(a->output, a, c, G.out_exec, 1, cancelled ? OL_WARNING : OL_ERROR, hint, NULL); output_obj_finish(a->output, a, c, G.out_exec, cancelled ? ES_CANCELLED : ES_FAILED, ms); }
        free(G.stmt); G.stmt = NULL;
        G.cmp_after = 0; G.hypo = false;
    }
    app_status(a, "%s", G.status);
    for (int i = 0; i < n && i < PLAN_MAXSQL; i++) rs_free(&rs[i]);
    exec_free(G.job); G.job = NULL;
    plan_sql_free(&G.ps);
}

/* ---- AI (EPL-043..054) ---- */

static void ai_audit(App *a, PView *v, const char *decision, size_t bytes) {
    char dir[1100]; pref_dir(a, dir, sizeof dir); strncat(dir, "/audit", sizeof dir - strlen(dir) - 1); mkdir(dir, 0700);
    char p[1300]; snprintf(p, sizeof p, "%s/ai.log", dir);
    FILE *f = fopen(p, "a"); if (!f) return;
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    const char *u = getenv("USER");
    fprintf(f, "%s user=%s provider=%s model=%s engine=%s plan=#%d tool=plan_analysis decision=%s bytes_sent=%zu shared=sql:%d,literals:%d,names:%d,raw:%d,subtree:%d\n", ts, u ? u : "?", G.ai.provider, G.ai.model,
            plan_dialect_name(v->p.dialect), v->seq, decision, bytes, v->share_sql, v->share_lit, v->share_names, v->share_raw, v->share_subtree);
    fclose(f);
}
static char *ai_user_message(PView *v) {
    PlanShare s = { v->share_sql, v->share_lit, v->share_names, v->share_raw, v->share_subtree && v->sel >= 0 ? v->sel : -1 };
    char *ctx = plan_context(&v->p, &s);
    const char *q = v->question.buf[0] ? v->question.buf : "Explain this execution plan and its likely bottlenecks.";
    size_t cap = strlen(ctx) + strlen(q) + 256;
    char *m = malloc(cap);
    snprintf(m, cap, "<plan_context>\n%s</plan_context>\n\nQuestion: %s", ctx, q);
    free(ctx);
    return m;
}
static void ai_send(App *a, PView *v) {
    char why[400];
    ai_config_load(a->conns ? a->conns->dir : NULL, &G.ai);
    if (!ai_ready(&G.ai, why, sizeof why)) { snprintf(v->ai_err, sizeof v->ai_err, "%s", why); ai_audit(a, v, "not-sent-not-configured", 0); return; }
    if (v->ai) { ai_free(v->ai); v->ai = NULL; }
    char *m = ai_user_message(v);
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    v->ai = ai_start(&G.ai, ai_plan_system_prompt(), m, dir);
    ai_audit(a, v, "sent", strlen(m));
    free(m);
    v->ai_err[0] = 0; free(v->answer); v->answer = NULL; v->nsql = 0;
    LOG_I("plan: AI analysis of plan #%d sent to %s (%s)", v->seq, G.ai.provider, G.ai.model);
}
static void poll_ai(PView *v) {
    if (!v->ai || !ai_done(v->ai)) return;
    const AiResult *r = ai_result(v->ai);
    if (r->text) {
        free(v->answer); v->answer = strdup(r->text);
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tm);
        snprintf(v->answer_hdr, sizeof v->answer_hdr, "%s \xC2\xB7 %s \xC2\xB7 %s \xC2\xB7 %d in / %d out tokens \xC2\xB7 %.1f s \xC2\xB7 hypotheses, not guarantees", G.ai.provider, r->model[0] ? r->model : G.ai.model, ts, r->in_tokens, r->out_tokens, ai_elapsed(v->ai));
        v->nsql = ai_sql_blocks(v->answer, v->sqlc, 4);
    } else snprintf(v->ai_err, sizeof v->ai_err, "%s", r->err[0] ? r->err : "No answer");
    ai_free(v->ai); v->ai = NULL;
}

/* ---- drawing helpers ---- */

static uint32_t kind_color(const PlanNode *n) {
    if (n->flags & (PF_SPILL | PF_WARN)) return T_RED;
    if (n->flags & (PF_CARD | PF_HOT)) return T_YELLOW;
    if (!strcmp(n->kind, "scan")) return 0xC98A3F;
    if (!strcmp(n->kind, "seek")) return T_GREEN;
    if (!strcmp(n->kind, "join")) return T_ACCENT;
    if (!strcmp(n->kind, "sort") || !strcmp(n->kind, "temp") || !strcmp(n->kind, "aggregate")) return 0x9E7CC1;
    return T_TEXT_MUTED;
}
static void fit(App *a, PView *v, Rect r) {
    float minx = 1e9f, miny = 1e9f, maxx = -1e9f, maxy = -1e9f;
    for (int i = 0; i < v->p.n; i++) { PlanNode *n = &v->p.nodes[i]; minx = fminf(minx, n->x); miny = fminf(miny, n->y); maxx = fmaxf(maxx, n->x + n->w); maxy = fmaxf(maxy, n->y + n->h); }
    if (minx > maxx) return;
    float sx = (r.w - px(a, 40)) / ((maxx - minx) * px(a, 1)), sy = (r.h - px(a, 40)) / ((maxy - miny) * px(a, 1));
    /* readable first: never below 50 % (text stays legible); a larger plan is panned, starting at the root */
    v->zoom = fmaxf(0.5f, fminf(1.2f, fminf(sx, sy)));
    v->panx = -minx * v->zoom * px(a, 1) + px(a, 20);
    float h = (maxy - miny) * v->zoom * px(a, 1);
    v->pany = h < r.h - px(a, 40) ? (r.h - h) / 2 - miny * v->zoom * px(a, 1) : -miny * v->zoom * px(a, 1) + px(a, 20);
    if (G.style == 0) v->pany = -miny * v->zoom * px(a, 1) + px(a, 20);   /* SQL Server: anchored top-left */
    else if (v->p.n && G.rtl) { float rooty = v->p.nodes[0].y * v->zoom * px(a, 1); if (h >= r.h - px(a, 40)) v->pany = r.h / 2 - rooty - v->p.nodes[0].h * v->zoom * px(a, 1) / 2; }
    v->fitted = true; v->fit_w = r.w; v->fit_h = r.h;
}
static Rect node_rect(App *a, PView *v, Rect r, const PlanNode *n) {
    float s = v->zoom * px(a, 1);
    return rect_make(r.x + v->panx + n->x * s, r.y + v->pany + n->y * s, n->w * s, n->h * s);
}
static void props_lines(PView *v, const PlanNode *n, void (*line)(const char *k, const char *val, void *ud), void *ud) {
    char b[64];
    line("Operator (as the engine names it)", n->op, ud);
    line("Node id", n->pid, ud);
    if (n->object[0]) line("Object", n->object, ud);
    if (n->index[0]) line("Index", n->index, ud);
    if (n->predicate[0]) line("Predicate", n->predicate, ud);
    plan_num_fmt(n->est_rows, b, sizeof b, 2); line("Estimated rows", b, ud);
    plan_num_fmt(n->act_rows, b, sizeof b, 2); line(v->p.dialect == PL_MSSQL || v->p.dialect == PL_ORACLE ? "Actual rows (all executions)" : "Actual rows (per loop)", b, ud);
    plan_num_fmt(n->loops, b, sizeof b, 0); line("Loops / executions", b, ud);
    plan_num_fmt(n->est_cost, b, sizeof b, 2); line("Cost (optimizer units, absolute)", b, ud);
    if (v->p.total_cost.st == PV_PRESENT && n->est_cost.st == PV_PRESENT && v->p.total_cost.v > 0) { snprintf(b, sizeof b, "%.1f%% of the plan's total cost", 100.0 * n->est_cost.v / v->p.total_cost.v); line("Cost (relative)", b, ud); }
    plan_num_fmt(n->act_ms, b, sizeof b, 3); line("Actual time, ms (real)", b, ud);
    if (n->warning[0]) line("Warning (reported by the engine)", n->warning, ud);
    for (int i = 0; i < n->nprops; i++) line(n->props[i].key, n->props[i].val, ud);
}
/* properties as text, for the clipboard */
static char clip_buf[16384]; static size_t clip_w;
static void clip_line(const char *k, const char *val, void *ud) { (void)ud; if (clip_w < sizeof clip_buf - 1) clip_w += (size_t)snprintf(clip_buf + clip_w, sizeof clip_buf - clip_w, "%s: %s\n", k, val); if (clip_w >= sizeof clip_buf) clip_w = sizeof clip_buf - 1; }
static void copy_props(App *a, PView *v) { clip_buf[0] = 0; clip_w = 0; props_lines(v, &v->p.nodes[v->sel], clip_line, NULL); SDL_SetClipboardText(clip_buf); app_status(a, "Properties copied (they may contain object names)"); }
typedef struct PL { App *a; Rect r; float y; int k; const char *filter; } PL;
static void draw_prop(const char *k, const char *val, void *ud) {
    PL *l = ud; App *a = l->a;
    if (l->filter && l->filter[0] && !strcasestr(k, l->filter) && !strcasestr(val, l->filter)) return;
    float rh = px(a, 20);
    if (l->y + rh > l->r.y + l->r.h) return;
    Rect kr = rect_make(l->r.x + px(a, 8), l->y, l->r.w * 0.42f, rh), vr = rect_make(l->r.x + l->r.w * 0.44f, l->y, l->r.w * 0.55f, rh);
    draw_text_fit(a->font_small, a->renderer, k, -1, kr, color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    draw_text_fit(a->font_small, a->renderer, val, -1, vr, color_hex(T_TEXT), ALIGN_LEFT);
    l->y += rh; l->k++;
}


/* ---- SQL Server Management Studio look (default style) ---- */

/* the SQL Server layout in Lume's dark theme */
#define SS_BG T_EDITOR_BG
#define SS_TEXT T_TEXT
#define SS_MUTED T_TEXT_MUTED
#define SS_EDGE 0x5E6675
#define SS_BLUE 0x5B8DEF
#define SS_BLUE_D 0x4A78D0
#define SS_YELLOW 0xE8B21F
#define SS_GREEN T_GREEN
#define SS_SEL T_ACCENT
#define SS_FILL 0x262B35       /* inside the glyphs (tables, sheets, keys) */
#define SS_GRID 0x3E5577       /* table glyph lines */
#define SS_LIGHT 0x7FA7D9
#define SS_GRAY 0x8A93A3
#define SS_TIP_BG T_PANEL_ALT
#define SS_RULE T_BORDER
static bool has(const char *s, const char *w) { return strcasestr(s, w) != NULL; }
/* the operator's family, from the engine's own name (SQL Server, PostgreSQL, MySQL, Oracle, SQLite) */
enum { IC_RESULT, IC_TSCAN, IC_CSCAN, IC_ISCAN, IC_CSEEK, IC_ISEEK, IC_LOOKUP, IC_LOOPS, IC_HASH, IC_MERGE, IC_SORT, IC_AGG, IC_SCALAR, IC_FILTER, IC_TOP, IC_PAR, IC_SPOOL, IC_CONCAT, IC_MODIFY, IC_OTHER };
static int icon_of(const PlanNode *n) {
    const char *o = n->op;
    if (!strcmp(n->kind, "result") || (has(o, "SELECT") && n->parent < 0)) return IC_RESULT;
    if (has(o, "Key Lookup") || has(o, "RID Lookup") || has(o, "Bookmark") || has(o, "TABLE ACCESS BY")) return IC_LOOKUP;
    if (has(o, "Clustered Index Seek") || has(o, "INDEX UNIQUE SCAN")) return IC_CSEEK;
    if (has(o, "Index Seek") || (has(o, "Index Scan") && has(o, "Seek")) || has(o, "INDEX RANGE SCAN") || has(o, "Index Only Scan") || has(o, "Index Scan using") || (has(o, "ref") && has(o, "index")) || has(o, "USING INDEX") || has(o, "SEARCH")) return IC_ISEEK;
    if (has(o, "Clustered Index Scan") || has(o, "INDEX FULL SCAN") || has(o, "INDEX FAST FULL")) return IC_CSCAN;
    if (has(o, "Index Scan")) return IC_ISCAN;
    if (has(o, "Table Scan") || has(o, "Seq Scan") || has(o, "TABLE ACCESS FULL") || has(o, "Table scan") || has(o, "SCAN") || has(o, "ALL")) return IC_TSCAN;
    if (has(o, "Nested Loop") || has(o, "NESTED LOOPS")) return IC_LOOPS;
    if (has(o, "Hash")) return IC_HASH;
    if (has(o, "Merge")) return IC_MERGE;
    if (has(o, "Sort") || has(o, "ORDER BY")) return IC_SORT;
    if (has(o, "Aggregate") || has(o, "GROUP") || has(o, "Group")) return IC_AGG;
    if (has(o, "Compute Scalar") || has(o, "Result") || has(o, "Constant")) return IC_SCALAR;
    if (has(o, "Filter") || has(o, "FILTER")) return IC_FILTER;
    if (has(o, "Top") || has(o, "Limit") || has(o, "STOPKEY") || has(o, "COUNT STOPKEY")) return IC_TOP;
    if (has(o, "Parallelism") || has(o, "Gather") || has(o, "PX ")) return IC_PAR;
    if (has(o, "Spool") || has(o, "Materialize") || has(o, "TEMP") || has(o, "temporary")) return IC_SPOOL;
    if (has(o, "Concatenation") || has(o, "Append") || has(o, "UNION")) return IC_CONCAT;
    if (has(o, "Insert") || has(o, "Update") || has(o, "Delete") || has(o, "Modify")) return IC_MODIFY;
    if (!strcmp(n->kind, "scan")) return IC_TSCAN;
    if (!strcmp(n->kind, "seek")) return IC_ISEEK;
    if (!strcmp(n->kind, "join")) return IC_LOOPS;
    if (!strcmp(n->kind, "sort")) return IC_SORT;
    if (!strcmp(n->kind, "aggregate")) return IC_AGG;
    return IC_OTHER;
}
static void ss_table(SDL_Renderer *R, Rect b, uint32_t head) {   /* a small grid: the "table" glyph */
    draw_rect(R, b, color_hex(SS_FILL)); draw_rect_outline(R, b, fmaxf(1, b.w / 22), color_hex(SS_BLUE_D));
    draw_rect(R, rect_make(b.x, b.y, b.w, b.h * 0.26f), color_hex(head));
    for (int k = 1; k < 3; k++) draw_rect(R, rect_make(b.x + b.w * k / 3, b.y + b.h * 0.26f, fmaxf(1, b.w / 30), b.h * 0.74f), color_hex(SS_GRID));
    for (int k = 1; k < 3; k++) draw_rect(R, rect_make(b.x, b.y + b.h * 0.26f + b.h * 0.74f * k / 3, b.w, fmaxf(1, b.w / 30)), color_hex(SS_GRID));
}
static void ss_arrow_r(SDL_Renderer *R, float x1, float y, float x2, float t, uint32_t c) { draw_line(R, x1, y, x2, y, t, color_hex(c)); draw_line(R, x2, y, x2 - t * 2.5f, y - t * 2.5f, t, color_hex(c)); draw_line(R, x2, y, x2 - t * 2.5f, y + t * 2.5f, t, color_hex(c)); }
static void ss_icon(SDL_Renderer *R, int ic, Rect b) {
    float s = b.w, t = fmaxf(1.2f, s / 18);
    Rect in = rect_inset(b, s * 0.12f, s * 0.12f);
    switch (ic) {
    case IC_RESULT:   /* the statement: a result sheet */
        draw_rect(R, in, color_hex(SS_FILL)); draw_rect_outline(R, in, t, color_hex(SS_BLUE_D));
        draw_rect(R, rect_make(in.x, in.y, in.w, in.h * 0.22f), color_hex(SS_BLUE));
        for (int k = 0; k < 3; k++) draw_rect(R, rect_make(in.x + in.w * 0.15f, in.y + in.h * (0.38f + k * 0.18f), in.w * (0.7f - k * 0.15f), t), color_hex(SS_MUTED));
        break;
    case IC_TSCAN: case IC_CSCAN: case IC_ISCAN:
        ss_table(R, rect_make(in.x, in.y + in.h * 0.1f, in.w * 0.78f, in.h * 0.8f), ic == IC_CSCAN ? SS_BLUE : ic == IC_ISCAN ? SS_LIGHT : SS_GRAY);
        ss_arrow_r(R, in.x + in.w * 0.1f, in.y + in.h * 0.62f, in.x + in.w, t * 1.3f, SS_YELLOW);   /* the scan: across every row */
        break;
    case IC_CSEEK: case IC_ISEEK:
        ss_table(R, rect_make(in.x, in.y, in.w * 0.72f, in.h * 0.72f), ic == IC_CSEEK ? SS_BLUE : SS_LIGHT);
        draw_rrect(R, rect_make(in.x + in.w * 0.42f, in.y + in.h * 0.42f, in.w * 0.42f, in.h * 0.42f), in.w * 0.21f, color_hex(SS_YELLOW));   /* the lens: straight to the range */
        draw_rrect(R, rect_make(in.x + in.w * 0.5f, in.y + in.h * 0.5f, in.w * 0.26f, in.h * 0.26f), in.w * 0.13f, color_hex(SS_FILL));
        draw_line(R, in.x + in.w * 0.78f, in.y + in.h * 0.78f, in.x + in.w, in.y + in.h, t * 1.6f, color_hex(SS_YELLOW));
        break;
    case IC_LOOKUP:
        ss_table(R, rect_make(in.x, in.y, in.w * 0.75f, in.h * 0.75f), SS_BLUE);
        draw_icon_key(R, in.x + in.w * 0.72f, in.y + in.h * 0.72f, in.w * 0.5f, color_hex(SS_YELLOW));
        break;
    case IC_LOOPS:   /* two inputs turning into each other */
        draw_rect_outline(R, rect_make(in.x + in.w * 0.12f, in.y + in.h * 0.15f, in.w * 0.6f, in.h * 0.45f), t * 1.4f, color_hex(SS_BLUE));
        draw_rect_outline(R, rect_make(in.x + in.w * 0.3f, in.y + in.h * 0.42f, in.w * 0.6f, in.h * 0.45f), t * 1.4f, color_hex(SS_YELLOW));
        ss_arrow_r(R, in.x + in.w * 0.3f, in.y + in.h * 0.87f, in.x + in.w * 0.9f, t, SS_BLUE_D);
        break;
    case IC_HASH:
        ss_table(R, rect_make(in.x, in.y + in.h * 0.12f, in.w * 0.62f, in.h * 0.76f), SS_LIGHT);
        for (int k = 0; k < 2; k++) { draw_line(R, in.x + in.w * (0.7f + k * 0.13f), in.y + in.h * 0.25f, in.x + in.w * (0.66f + k * 0.13f), in.y + in.h * 0.8f, t * 1.3f, color_hex(SS_YELLOW)); draw_line(R, in.x + in.w * 0.6f, in.y + in.h * (0.42f + k * 0.2f), in.x + in.w, in.y + in.h * (0.42f + k * 0.2f), t * 1.3f, color_hex(SS_YELLOW)); }
        break;
    case IC_MERGE:   /* two sorted inputs into one */
        draw_line(R, in.x, in.y + in.h * 0.2f, in.x + in.w * 0.5f, in.y + in.h * 0.5f, t * 1.5f, color_hex(SS_BLUE));
        draw_line(R, in.x, in.y + in.h * 0.8f, in.x + in.w * 0.5f, in.y + in.h * 0.5f, t * 1.5f, color_hex(SS_YELLOW));
        ss_arrow_r(R, in.x + in.w * 0.5f, in.y + in.h * 0.5f, in.x + in.w, t * 1.5f, SS_BLUE_D);
        break;
    case IC_SORT:
        for (int k = 0; k < 4; k++) draw_rect(R, rect_make(in.x, in.y + in.h * (0.08f + k * 0.22f), in.w * (0.3f + k * 0.15f), in.h * 0.13f), color_hex(k % 2 ? SS_BLUE : SS_LIGHT));
        draw_line(R, in.x + in.w * 0.85f, in.y + in.h * 0.1f, in.x + in.w * 0.85f, in.y + in.h * 0.9f, t * 1.4f, color_hex(SS_YELLOW));
        draw_triangle(R, in.x + in.w * 0.85f, in.y + in.h * 0.86f, in.w * 0.3f, in.h * 0.2f, true, color_hex(SS_YELLOW));
        break;
    case IC_AGG:   /* a sigma */
        draw_line(R, in.x + in.w * 0.2f, in.y + in.h * 0.12f, in.x + in.w * 0.8f, in.y + in.h * 0.12f, t * 1.8f, color_hex(SS_BLUE));
        draw_line(R, in.x + in.w * 0.2f, in.y + in.h * 0.12f, in.x + in.w * 0.55f, in.y + in.h * 0.5f, t * 1.8f, color_hex(SS_BLUE));
        draw_line(R, in.x + in.w * 0.55f, in.y + in.h * 0.5f, in.x + in.w * 0.2f, in.y + in.h * 0.88f, t * 1.8f, color_hex(SS_BLUE));
        draw_line(R, in.x + in.w * 0.2f, in.y + in.h * 0.88f, in.x + in.w * 0.8f, in.y + in.h * 0.88f, t * 1.8f, color_hex(SS_BLUE));
        break;
    case IC_SCALAR:   /* a calculator */
        draw_rrect(R, in, s * 0.06f, color_hex(SS_BLUE_D));
        draw_rect(R, rect_make(in.x + in.w * 0.14f, in.y + in.h * 0.12f, in.w * 0.72f, in.h * 0.2f), color_hex(0x2E3A52));
        for (int r2 = 0; r2 < 3; r2++) for (int c2 = 0; c2 < 3; c2++) draw_rect(R, rect_make(in.x + in.w * (0.14f + c2 * 0.26f), in.y + in.h * (0.42f + r2 * 0.18f), in.w * 0.18f, in.h * 0.12f), color_hex(c2 == 2 && r2 == 2 ? SS_YELLOW : SS_FILL));
        break;
    case IC_FILTER:   /* a funnel */
        draw_triangle(R, in.x + in.w / 2, in.y + in.h * 0.35f, in.w * 0.95f, in.h * 0.5f, true, color_hex(SS_BLUE));
        draw_rect(R, rect_make(in.x + in.w * 0.43f, in.y + in.h * 0.55f, in.w * 0.14f, in.h * 0.38f), color_hex(SS_BLUE_D));
        break;
    case IC_TOP:
        for (int k = 0; k < 3; k++) draw_rect(R, rect_make(in.x, in.y + in.h * (0.5f + k * 0.17f), in.w * 0.6f, in.h * 0.1f), color_hex(SS_GRAY));
        draw_rect(R, rect_make(in.x, in.y + in.h * 0.15f, in.w * 0.6f, in.h * 0.22f), color_hex(SS_BLUE));
        draw_triangle(R, in.x + in.w * 0.82f, in.y + in.h * 0.3f, in.w * 0.32f, in.h * 0.3f, false, color_hex(SS_YELLOW));
        break;
    case IC_PAR:
        ss_arrow_r(R, in.x, in.y + in.h * 0.3f, in.x + in.w, t * 1.4f, SS_BLUE);
        ss_arrow_r(R, in.x, in.y + in.h * 0.7f, in.x + in.w, t * 1.4f, SS_YELLOW);
        break;
    case IC_SPOOL:   /* stacked storage */
        for (int k = 2; k >= 0; k--) { draw_rrect(R, rect_make(in.x + in.w * 0.1f, in.y + in.h * (0.1f + k * 0.24f), in.w * 0.8f, in.h * 0.32f), in.h * 0.14f, color_hex(k ? SS_LIGHT : SS_BLUE)); }
        break;
    case IC_CONCAT:
        draw_rect(R, rect_make(in.x, in.y + in.h * 0.1f, in.w * 0.45f, in.h * 0.3f), color_hex(SS_BLUE));
        draw_rect(R, rect_make(in.x, in.y + in.h * 0.6f, in.w * 0.45f, in.h * 0.3f), color_hex(SS_LIGHT));
        ss_arrow_r(R, in.x + in.w * 0.5f, in.y + in.h * 0.5f, in.x + in.w, t * 1.4f, SS_YELLOW);
        break;
    case IC_MODIFY:
        ss_table(R, rect_make(in.x, in.y, in.w * 0.78f, in.h * 0.78f), SS_BLUE);
        draw_line(R, in.x + in.w * 0.45f, in.y + in.h, in.x + in.w, in.y + in.h * 0.45f, t * 2.2f, color_hex(SS_YELLOW));
        break;
    default:
        draw_rrect(R, in, s * 0.08f, color_hex(SS_LIGHT));
        draw_rrect(R, rect_inset(in, in.w * 0.28f, in.h * 0.28f), s * 0.08f, color_hex(SS_FILL));
    }
}
/* what an operator does, in Lume's own words (tooltip second line) */
static const char *ss_desc(int ic) {
    switch (ic) {
    case IC_RESULT: return "The statement: rows reach the client from here.";
    case IC_TSCAN: return "Reads every row of the table (no index used).";
    case IC_CSCAN: return "Reads the clustered index (the table itself), entirely or a range.";
    case IC_ISCAN: return "Reads a nonclustered index, entirely or a range.";
    case IC_CSEEK: return "Goes straight to a range of rows of the clustered index.";
    case IC_ISEEK: return "Goes straight to a range of rows of an index.";
    case IC_LOOKUP: return "For each row, fetches the missing columns from the table by its key.";
    case IC_LOOPS: return "For each row of the outer input, searches the inner input for matches.";
    case IC_HASH: return "Builds a hash table from one input and probes it with the other.";
    case IC_MERGE: return "Joins two inputs already sorted on the join keys.";
    case IC_SORT: return "Sorts its input.";
    case IC_AGG: return "Computes summary values for groups of rows.";
    case IC_SCALAR: return "Computes new values from existing columns.";
    case IC_FILTER: return "Keeps only the rows that satisfy a predicate.";
    case IC_TOP: return "Returns only the first rows of its input.";
    case IC_PAR: return "Moves rows between parallel threads.";
    case IC_SPOOL: return "Stores rows to read them again.";
    case IC_CONCAT: return "Appends the rows of several inputs.";
    case IC_MODIFY: return "Changes rows of the table.";
    }
    return "";
}
static void ss_physical(const PlanNode *n, char *phys, int pn, char *logi, int ln) {
    snprintf(phys, (size_t)pn, "%s", n->op); logi[0] = 0;
    char *p = strstr(phys, " (");
    if (p && phys[strlen(phys) - 1] == ')') { snprintf(logi, (size_t)ln, "%s", p + 1); *p = 0; }
}
static void ss_fmt_num(double v, char *o, int n) {   /* 1234567 -> 1234567, 0.0123 -> 0,0123 style kept neutral: dot */
    if (fabs(v) >= 1e6 || v == floor(v)) snprintf(o, (size_t)n, "%.0f", v); else if (fabs(v) >= 1) snprintf(o, (size_t)n, "%.2f", v); else snprintf(o, (size_t)n, "%.6g", v);
}
typedef struct SsRow { char k[48], v[96]; } SsRow;
static int ss_rows(PView *v, const PlanNode *n, SsRow *r, int max) {
    int k = 0; char b[96];
    #define ROW(key, ...) do { if (k < max) { snprintf(r[k].k, sizeof r[k].k, "%s", key); snprintf(r[k].v, sizeof r[k].v, __VA_ARGS__); k++; } } while (0)
    bool ms = v->p.dialect == PL_MSSQL, act = v->p.mode == PM_ACTUAL;
    const char *po = plan_prop_get(n, "PhysicalOp"), *lo = plan_prop_get(n, "LogicalOp");
    char phys[128], logi[128]; ss_physical(n, phys, sizeof phys, logi, sizeof logi);
    ROW("Physical Operation", "%s", po ? po : phys);
    if (lo || logi[0]) ROW("Logical Operation", "%s", lo ? lo : logi);
    if (act && plan_prop_get(n, "ActualExecutionMode")) ROW("Actual Execution Mode", "%s", plan_prop_get(n, "ActualExecutionMode"));
    if (plan_prop_get(n, "EstimatedExecutionMode")) ROW("Estimated Execution Mode", "%s", plan_prop_get(n, "EstimatedExecutionMode"));
    if (act && n->act_rows.st == PV_PRESENT) { ss_fmt_num(n->act_rows.v, b, sizeof b); ROW(ms ? "Actual Number of Rows for All Executions" : "Actual Number of Rows", "%s", b); }
    if (act && plan_prop_get(n, "ActualRowsRead")) ROW("Number of Rows Read", "%s", plan_prop_get(n, "ActualRowsRead"));
    if (act && plan_prop_get(n, "ActualLogicalReads")) ROW("Actual Logical Reads", "%s", plan_prop_get(n, "ActualLogicalReads"));
    if (act && n->act_ms.st == PV_PRESENT) ROW("Actual Elapsed Time (ms)", "%.3f", n->act_ms.v);
    if (act && plan_prop_get(n, "ActualCPUms")) ROW("Actual CPU Time (ms)", "%s", plan_prop_get(n, "ActualCPUms"));
    if (act && n->loops.st == PV_PRESENT) ROW("Number of Executions", "%.0f", n->loops.v);
    if (plan_prop_get(n, "EstimateIO")) ROW("Estimated I/O Cost", "%s", plan_prop_get(n, "EstimateIO"));
    bool ok; double sc = plan_self_cost(&v->p, n->id, &ok);
    if (ok) { double pc = v->p.total_cost.st == PV_PRESENT && v->p.total_cost.v > 0 ? 100.0 * sc / v->p.total_cost.v : -1; ss_fmt_num(sc, b, sizeof b); if (pc >= 0) ROW("Estimated Operator Cost", "%s (%.0f%%)", b, pc); else ROW("Estimated Operator Cost", "%s", b); }
    if (plan_prop_get(n, "EstimateCPU")) ROW("Estimated CPU Cost", "%s", plan_prop_get(n, "EstimateCPU"));
    if (n->est_cost.st == PV_PRESENT) { ss_fmt_num(n->est_cost.v, b, sizeof b); ROW("Estimated Subtree Cost", "%s", b); }
    if (plan_prop_get(n, "EstimateRebinds")) ROW("Estimated Number of Executions", "%.0f", 1 + atof(plan_prop_get(n, "EstimateRebinds")) + atof(plan_prop_get(n, "EstimateRewinds") ? plan_prop_get(n, "EstimateRewinds") : "0"));
    if (n->est_rows.st == PV_PRESENT) { ss_fmt_num(n->est_rows.v, b, sizeof b); ROW(ms ? "Estimated Number of Rows Per Execution" : "Estimated Number of Rows", "%s", b); }
    if (plan_prop_get(n, "EstimatedRowsRead")) ROW("Estimated Number of Rows to be Read", "%s", plan_prop_get(n, "EstimatedRowsRead"));
    if (plan_prop_get(n, "AvgRowSize")) ROW("Estimated Row Size", "%s B", plan_prop_get(n, "AvgRowSize"));
    if (plan_prop_get(n, "Parallel")) ROW("Parallel", "%s", !strcmp(plan_prop_get(n, "Parallel"), "1") || !strcasecmp(plan_prop_get(n, "Parallel"), "true") ? "True" : "False");
    if (!ms && v->p.dialect != PL_SQLITE && n->est_cost.st != PV_PRESENT && n->est_rows.st != PV_PRESENT) ROW("Costs", "not reported by %s", plan_dialect_name(v->p.dialect));
    ROW("Node ID", "%s", n->pid);
    #undef ROW
    return k;
}
static Rect ss_icon_rect(App *a, PView *v, Rect r, const PlanNode *n) {
    Rect nr = node_rect(a, v, r, n);
    float is = 40 * px(a, 1) * v->zoom;
    return rect_make(nr.x + (nr.w - is) / 2, nr.y, is, is);
}
static bool ss_parallel(const PlanNode *n) { const char *p = plan_prop_get(n, "Parallel"); return p && (!strcmp(p, "1") || !strcasecmp(p, "true")); }
/* "CREATE INDEX hint on [dbo].[t]: [EQUALITY] [a] [INEQUALITY] [b] [INCLUDE] [c]" -> the DDL SQL Server shows */
static bool ss_hint_ddl(const char *hint, char *out, int n) {
    const char *on = strstr(hint, " on "), *colon = on ? strchr(on, ':') : NULL;
    if (!on || !colon) return false;
    char table[300]; snprintf(table, sizeof table, "%.*s", (int)(colon - on - 4), on + 4);
    char keys[600] = "", inc[600] = ""; int mode = 0;
    const char *q = colon + 1;
    while (*q && *q != '\n') {
        while (*q == ' ') q++;
        if (*q != '[') break;
        const char *e = strchr(q, ']'); if (!e) break;
        char tok[200]; snprintf(tok, sizeof tok, "%.*s", (int)(e - q + 1), q);
        if (!strcmp(tok, "[EQUALITY]") || !strcmp(tok, "[INEQUALITY]")) mode = 0;
        else if (!strcmp(tok, "[INCLUDE]")) mode = 1;
        else { char *dst = mode ? inc : keys; size_t L = strlen(dst); snprintf(dst + L, 600 - L, "%s%s", L ? "," : "", tok); }
        q = e + 1;
    }
    if (!keys[0]) return false;
    if (inc[0]) snprintf(out, (size_t)n, "CREATE NONCLUSTERED INDEX [<Name of Missing Index, sysname,>] ON %s (%s) INCLUDE (%s)", table, keys, inc);
    else snprintf(out, (size_t)n, "CREATE NONCLUSTERED INDEX [<Name of Missing Index, sysname,>] ON %s (%s)", table, keys);
    return true;
}
/* the SQL Server header: query cost relative to the batch, the statement, the missing index in green */
static float ss_header(App *a, PView *v, Rect r) {
    SDL_Renderer *R = a->renderer;
    float lh = px(a, 17), y = r.y + px(a, 6);
    char b[700];
    snprintf(b, sizeof b, "Query 1: Query cost (relative to the batch): 100%%");
    draw_text_fit(a->font_small, R, b, -1, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), lh), color_hex(SS_TEXT), ALIGN_LEFT); y += lh;
    if (v->p.sql) {
        char one[600]; int w = 0; for (const char *s = v->p.sql; *s && w < (int)sizeof one - 1; s++) one[w++] = (*s == '\n' || *s == '\r' || *s == '\t') ? ' ' : *s; one[w] = 0;
        draw_text_fit(a->font_small, R, one, -1, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), lh), color_hex(SS_TEXT), ALIGN_LEFT); y += lh;
    }
    /* Missing Index: the engine's hint (SQL Server) or Lume's evidence-based candidate */
    char ddl[600] = "", why[300]; const char *impact = v->p.n ? plan_prop_get(&v->p.nodes[0], "MissingIndexImpact") : NULL;
    const char *hint = v->p.n ? plan_prop_get(&v->p.nodes[0], "MissingIndexes (SQL Server hint)") : NULL;
    if (!(hint && ss_hint_ddl(hint, ddl, sizeof ddl))) { impact = NULL; for (int i = 0; i < v->nf && !ddl[0]; i++) if (v->f[i].kind == FD_INDEX) plan_index_ddl(&v->p, &v->f[i], ddl, sizeof ddl, why, sizeof why); }
    if (ddl[0]) {
        if (impact) snprintf(b, sizeof b, "Missing Index (Impact %s): %s", impact, ddl); else snprintf(b, sizeof b, "Missing Index (candidate, verify first): %s", ddl);
        draw_text_fit(a->font_small, R, b, -1, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), lh), color_hex(SS_GREEN), ALIGN_LEFT);
        ui_record("plan:missing-index", rect_make(r.x, y, r.w, lh));
        y += lh;
    }
    y += px(a, 4);
    draw_rect(R, rect_make(r.x, y, r.w, px(a, 1)), color_hex(SS_RULE));
    return y + px(a, 1) - r.y;
}
static float seg_dist(float px_, float py_, float x1, float y1, float x2, float y2) {
    float dx = x2 - x1, dy = y2 - y1, L = dx * dx + dy * dy, t = L > 0 ? ((px_ - x1) * dx + (py_ - y1) * dy) / L : 0;
    if (t < 0) t = 0; if (t > 1) t = 1;
    float qx = x1 + t * dx - px_, qy = y1 + t * dy - py_; return sqrtf(qx * qx + qy * qy);
}
static void ss_tooltip(App *a, Rect r, float mx, float my, const char *title, const char *desc, SsRow *rows, int nr, const char *sec1k, const char *sec1, const char *sec2k, const char *sec2) {
    SDL_Renderer *R = a->renderer;
    float w = px(a, 400), lh = px(a, 16), pad = px(a, 8);
    float dh = desc && desc[0] ? results_draw_wrapped(a->font_small, R, desc, rect_make(0, 0, w - pad * 2, px(a, 60)), color_hex(SS_TEXT), 0, false) : 0;
    float s1 = sec1 && sec1[0] ? results_draw_wrapped(a->font_small, R, sec1, rect_make(0, 0, w - pad * 2, px(a, 70)), color_hex(SS_TEXT), 0, false) : 0;
    float s2 = sec2 && sec2[0] ? results_draw_wrapped(a->font_small, R, sec2, rect_make(0, 0, w - pad * 2, px(a, 40)), color_hex(SS_TEXT), 0, false) : 0;
    s1 = fminf(s1, px(a, 70)); s2 = fminf(s2, px(a, 40));
    float h = pad + px(a, 20) + dh + px(a, 6) + nr * lh + (s1 ? px(a, 8) + lh + s1 : 0) + (s2 ? px(a, 8) + lh + s2 : 0) + pad;
    Rect b = rect_make(mx + px(a, 16), my + px(a, 16), w, h);
    if (b.x + w > r.x + r.w) b.x = mx - w - px(a, 10);
    if (b.y + h > r.y + r.h) b.y = fmaxf(r.y, r.y + r.h - h);
    if (b.x < r.x) b.x = r.x;
    draw_rect(R, rect_make(b.x + px(a, 3), b.y + px(a, 3), b.w, b.h), color_alpha(color_hex(0x000000), 40));
    draw_rect(R, b, color_hex(SS_TIP_BG)); draw_rect_outline(R, b, px(a, 1), color_hex(SS_RULE));
    float y = b.y + pad;
    draw_text_fit(a->font_ui, R, title, -1, rect_make(b.x + pad, y, w - pad * 2, px(a, 18)), color_hex(SS_TEXT), ALIGN_LEFT); y += px(a, 20);
    if (dh) { results_draw_wrapped(a->font_small, R, desc, rect_make(b.x + pad, y, w - pad * 2, dh), color_hex(SS_TEXT), 0, true); y += dh; }
    y += px(a, 3); draw_rect(R, rect_make(b.x + pad, y, w - pad * 2, px(a, 1)), color_hex(SS_RULE)); y += px(a, 3);
    for (int i = 0; i < nr; i++) {
        draw_text_fit(a->font_small, R, rows[i].k, -1, rect_make(b.x + pad, y, w * 0.62f, lh), color_hex(SS_TEXT), ALIGN_LEFT);
        draw_text_fit(a->font_small, R, rows[i].v, -1, rect_make(b.x + w * 0.6f, y, w * 0.4f - pad, lh), color_hex(SS_TEXT), ALIGN_RIGHT);
        y += lh;
    }
    if (s1) { y += px(a, 4); draw_rect(R, rect_make(b.x + pad, y, w - pad * 2, px(a, 1)), color_hex(SS_RULE)); y += px(a, 4); draw_text(a->font_small, R, sec1k, b.x + pad, y, color_hex(SS_TEXT)); y += lh; clip_push(R, rect_make(b.x, y, w, s1)); results_draw_wrapped(a->font_small, R, sec1, rect_make(b.x + pad, y, w - pad * 2, s1), color_hex(SS_MUTED), 0, true); clip_pop(R); y += s1; }
    if (s2) { y += px(a, 4); draw_rect(R, rect_make(b.x + pad, y, w - pad * 2, px(a, 1)), color_hex(SS_RULE)); y += px(a, 4); draw_text(a->font_small, R, sec2k, b.x + pad, y, color_hex(SS_TEXT)); y += lh; clip_push(R, rect_make(b.x, y, w, s2)); results_draw_wrapped(a->font_small, R, sec2, rect_make(b.x + pad, y, w - pad * 2, s2), color_hex(SS_MUTED), 0, true); clip_pop(R); }
}
static void draw_graph_rows(App *a, PView *v, Rect r0, bool interactive) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r0, color_hex(SS_BG));
    float hh = ss_header(a, v, r0);
    Rect r = rect_make(r0.x, r0.y + hh, r0.w, r0.h - hh);
    if (v->fitted && !v->user_view && (fabsf(v->fit_w - r.w) > 2 || fabsf(v->fit_h - r.h) > 2)) v->fitted = false;   /* follows the panel size until the user takes over */
    if (!v->fitted) fit(a, v, r);
    bool hov = rect_contains(r, in->mouse_x, in->mouse_y);
    if (interactive && hov && in->wheel_y != 0) {
        v->user_view = true;
        float old = v->zoom; v->zoom = fmaxf(0.15f, fminf(3.0f, v->zoom * (in->wheel_y > 0 ? 1.12f : 1 / 1.12f)));
        float k = v->zoom / old; v->panx = in->mouse_x - r.x - (in->mouse_x - r.x - v->panx) * k; v->pany = in->mouse_y - r.y - (in->mouse_y - r.y - v->pany) * k;
    }
    clip_push(R, r);
    double maxr = 1; for (int i = 0; i < v->p.n; i++) { const PlanNode *n = &v->p.nodes[i]; double rr = n->act_rows.st == PV_PRESENT ? n->act_rows.v : n->est_rows.st == PV_PRESENT ? n->est_rows.v : 0; if (rr > maxr) maxr = rr; }
    int hover_edge = -1;
    /* arrows: from each input to its consumer, pointing at the consumer; width by rows */
    for (int i = 0; i < v->p.n; i++) {
        const PlanNode *n = &v->p.nodes[i];
        if (n->parent < 0) continue;
        bool hidden = false; for (int q = n->parent; q >= 0; q = v->p.nodes[q].parent) if (v->p.nodes[q].collapsed) hidden = true;
        if (hidden) continue;
        Rect ci = ss_icon_rect(a, v, r, n), pi = ss_icon_rect(a, v, r, &v->p.nodes[n->parent]);
        double rr = n->act_rows.st == PV_PRESENT ? n->act_rows.v : n->est_rows.st == PV_PRESENT ? n->est_rows.v : -1;
        float th = G.thick && rr >= 0 ? px(a, 1) + px(a, 9) * (float)(log10(rr + 1) / log10(maxr + 1)) * fminf(1.0f, v->zoom) : px(a, 1.2f);
        float x1 = ci.x - px(a, 2) * v->zoom, y1 = ci.y + ci.h / 2, x2 = pi.x + pi.w + px(a, 2) * v->zoom, y2 = pi.y + pi.h / 2;
        float mx = x2 + (x1 - x2) * 0.5f;
        bool straight = fabsf(y1 - y2) < 1;   /* same row: one straight arrow */
        uint32_t ec = SS_EDGE;
        if (interactive && hov && (seg_dist(in->mouse_x, in->mouse_y, x1, y1, mx, y1) < th / 2 + px(a, 3) || seg_dist(in->mouse_x, in->mouse_y, mx, y1, mx, y2) < th / 2 + px(a, 3) || seg_dist(in->mouse_x, in->mouse_y, mx, y2, x2 + px(a, 8), y2) < th / 2 + px(a, 3))) { hover_edge = i; ec = SS_SEL; }
        if (straight) draw_line(R, x1, y1, x2 + px(a, 8) * v->zoom, y2, th, color_hex(ec));
        else { draw_line(R, x1, y1, mx, y1, th, color_hex(ec)); draw_line(R, mx, y1, mx, y2, th, color_hex(ec)); draw_line(R, mx, y2, x2 + px(a, 8) * v->zoom, y2, th, color_hex(ec)); }
        float ah = fmaxf(px(a, 5), th * 1.2f) * fminf(1.0f, v->zoom + 0.3f);
        draw_line(R, x2, y2, x2 + ah * 1.4f, y2 - ah, fmaxf(px(a, 1.5f), th * 0.6f), color_hex(ec));
        draw_line(R, x2, y2, x2 + ah * 1.4f, y2 + ah, fmaxf(px(a, 1.5f), th * 0.6f), color_hex(ec));
    }
    int hover = -1;
    bool act = v->p.mode == PM_ACTUAL;
    for (int i = 0; i < v->p.n; i++) {
        PlanNode *n = &v->p.nodes[i];
        bool hidden = false; for (int q = n->parent; q >= 0; q = v->p.nodes[q].parent) if (v->p.nodes[q].collapsed) hidden = true;
        if (hidden) continue;
        Rect nr = node_rect(a, v, r, n);
        if (nr.x > r.x + r.w || nr.y > r.y + r.h || nr.x + nr.w < r.x || nr.y + nr.h < r.y) continue;
        Rect ic = ss_icon_rect(a, v, r, n);
        bool sel = v->sel == n->id, hit = G.search.buf[0] && (strcasestr(n->op, G.search.buf) || strcasestr(n->object, G.search.buf) || strcasestr(n->index, G.search.buf));
        if (interactive && rect_contains(nr, in->mouse_x, in->mouse_y)) hover = i;
        if (sel || hit) draw_rect_outline(R, rect_inset(nr, -px(a, 3), -px(a, 3)), px(a, 1.5f), color_hex(sel ? SS_SEL : SS_YELLOW));
        int kind = icon_of(n);
        ss_icon(R, kind, ic);
        if (n->warning[0] || (n->flags & (PF_SPILL | PF_WARN))) {   /* the yellow warning triangle */
            float ws = ic.w * 0.42f; float cx = ic.x + ic.w - ws * 0.4f, cy = ic.y + ic.h - ws * 0.3f;
            draw_triangle(R, cx, cy, ws, ws * 0.85f, false, color_hex(0xF2C811));
            draw_text_fit(a->font_small, R, "!", -1, rect_make(cx - ws / 2, cy - ws * 0.35f, ws, ws * 0.8f), color_hex(0x000000), ALIGN_CENTER);
        }
        if (ss_parallel(n)) { float ps = ic.w * 0.36f; draw_rrect(R, rect_make(ic.x - ps * 0.3f, ic.y + ic.h - ps * 0.8f, ps, ps), ps / 2, color_hex(0xF2C811)); draw_line(R, ic.x - ps * 0.1f, ic.y + ic.h - ps * 0.3f, ic.x + ps * 0.5f, ic.y + ic.h - ps * 0.3f, px(a, 1.5f), color_hex(0x000000)); }
        char tag[160]; snprintf(tag, sizeof tag, "plannode:%s", n->pid); ui_record(tag, nr);
        if (v->zoom < 0.4f) continue;
        Font *f = a->font_small;
        float ly = ic.y + ic.h + px(a, 3) * v->zoom, lh = font_height(f) * 0.98f;
        char phys[128], logi[128], l[300];
        ss_physical(n, phys, sizeof phys, logi, sizeof logi);
        Rect tr = rect_make(nr.x - px(a, 18) * v->zoom, ly, nr.w + px(a, 36) * v->zoom, lh);
        #define LINE(txt, col) do { tr.y = ly; draw_text_fit(f, R, txt, -1, tr, color_hex(col), ALIGN_CENTER); ly += lh; } while (0)
        LINE(phys, SS_TEXT);
        if (logi[0]) LINE(logi, SS_TEXT);
        if (n->object[0]) { snprintf(l, sizeof l, "%s%s%s", n->object, n->index[0] ? "." : "", n->index); LINE(l, SS_TEXT); }
        bool ok; double sc = plan_self_cost(&v->p, i, &ok);
        if (ok && v->p.total_cost.st == PV_PRESENT && v->p.total_cost.v > 0) { snprintf(l, sizeof l, "Cost: %.0f %%", 100.0 * sc / v->p.total_cost.v); LINE(l, SS_TEXT); }
        else if (v->p.dialect == PL_SQLITE && kind != IC_RESULT) LINE("Cost: n/a", SS_MUTED);
        if (act && n->act_ms.st == PV_PRESENT) { snprintf(l, sizeof l, "%.3fs", n->act_ms.v / 1000.0); LINE(l, SS_TEXT); }
        if (act && n->act_rows.st == PV_PRESENT && n->est_rows.st == PV_PRESENT && kind != IC_RESULT) {
            double est = n->est_rows.v * (v->p.dialect == PL_MSSQL && n->loops.st == PV_PRESENT && n->loops.v > 0 ? n->loops.v : 1);
            double pc = est > 0 ? 100.0 * n->act_rows.v / est : 0;
            snprintf(l, sizeof l, "%.0f of %.0f (%.0f%%)", n->act_rows.v, est, pc);
            LINE(l, (pc > 1000 || pc < 10) && (n->act_rows.v > 100 || est > 100) ? T_RED : SS_TEXT);   /* a far-off estimate stands out */
        }
        if (n->nchildren && n->collapsed) { snprintf(l, sizeof l, "+%d hidden", n->nchildren); LINE(l, SS_SEL); }
        #undef LINE
    }
    clip_pop(R);
    if (!interactive) return;
    if (hover >= 0 && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; v->sel = hover; if (in->clicks >= 2 && v->p.nodes[hover].nchildren) v->p.nodes[hover].collapsed = !v->p.nodes[hover].collapsed, relayout(&v->p); }
    else if (hov && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; G.dragging = true; G.dx = in->mouse_x - v->panx; G.dy = in->mouse_y - v->pany; }
    if (G.dragging) { if (in->mouse_down) { if (fabsf(in->mouse_x - G.dx - v->panx) > 1 || fabsf(in->mouse_y - G.dy - v->pany) > 1) v->user_view = true; v->panx = in->mouse_x - G.dx; v->pany = in->mouse_y - G.dy; } else G.dragging = false; }
    if (hover >= 0 && in->mouse_rpressed && !in->mouse_consumed) {
        in->mouse_consumed = true; v->sel = hover;
        popup_open(&G.menu, in->mouse_x, in->mouse_y);
        popup_add(&G.menu, 1, "Copy Properties", false, true);
        popup_add(&G.menu, 2, "Focus Node", false, true);
        popup_add(&G.menu, 3, v->p.nodes[hover].collapsed ? "Expand Subtree" : "Collapse Subtree", false, v->p.nodes[hover].nchildren > 0);
        popup_add(&G.menu, 4, "Explain this Operator (AI)...", true, true);
        popup_add(&G.menu, 5, "Analyze Subtree with AI...", false, true);
        bool idx = false; for (int i = 0; i < v->nf; i++) if (v->f[i].node == hover && v->f[i].kind == FD_INDEX) idx = true;
        popup_add(&G.menu, 6, "Suggest Index... (review, never applied from here)", true, idx);
    }
    if (G.menu.open) return;
    if (hover >= 0) {   /* the operator tooltip, as SQL Server lays it out */
        const PlanNode *n = &v->p.nodes[hover];
        SsRow rows[24]; int nr2 = ss_rows(v, n, rows, 24);
        char phys[128], logi[128]; ss_physical(n, phys, sizeof phys, logi, sizeof logi);
        char obj[400]; snprintf(obj, sizeof obj, "%s%s%s", n->object, n->index[0] ? "." : "", n->index);
        char pred[1100]; snprintf(pred, sizeof pred, "%s%s%s", n->predicate, n->warning[0] ? "\nWarning: " : "", n->warning);
        ss_tooltip(a, r0, in->mouse_x, in->mouse_y, phys, ss_desc(icon_of(n)), rows, nr2, n->warning[0] && !n->predicate[0] ? "Warnings" : "Predicate", pred, "Object", obj);
    } else if (hover_edge >= 0) {   /* the arrow tooltip: what flows to the consumer */
        const PlanNode *n = &v->p.nodes[hover_edge];
        SsRow rows[6]; int k = 0; char b[64];
        if (v->p.mode == PM_ACTUAL && n->act_rows.st == PV_PRESENT) { snprintf(rows[k].k, sizeof rows[k].k, "Actual Number of Rows"); ss_fmt_num(n->act_rows.v, b, sizeof b); snprintf(rows[k].v, sizeof rows[k].v, "%s", b); k++; }
        if (n->est_rows.st == PV_PRESENT) { snprintf(rows[k].k, sizeof rows[k].k, "Estimated Number of Rows"); ss_fmt_num(n->est_rows.v, b, sizeof b); snprintf(rows[k].v, sizeof rows[k].v, "%s", b); k++; }
        const char *rs = plan_prop_get(n, "AvgRowSize");
        if (rs) { snprintf(rows[k].k, sizeof rows[k].k, "Estimated Row Size"); snprintf(rows[k].v, sizeof rows[k].v, "%s B", rs); k++; }
        if (rs && n->est_rows.st == PV_PRESENT) { double bytes = atof(rs) * n->est_rows.v; snprintf(rows[k].k, sizeof rows[k].k, "Estimated Data Size"); if (bytes >= 1048576) snprintf(rows[k].v, sizeof rows[k].v, "%.1f MB", bytes / 1048576); else if (bytes >= 1024) snprintf(rows[k].v, sizeof rows[k].v, "%.0f KB", bytes / 1024); else snprintf(rows[k].v, sizeof rows[k].v, "%.0f B", bytes); k++; }
        if (!k) { snprintf(rows[k].k, sizeof rows[k].k, "Rows"); snprintf(rows[k].v, sizeof rows[k].v, "not reported by %s", plan_dialect_name(v->p.dialect)); k++; }
        char t[200]; snprintf(t, sizeof t, "%s  \xE2\x86\x92  %s", n->op, v->p.nodes[n->parent].op);
        ss_tooltip(a, r0, in->mouse_x, in->mouse_y, t, "", rows, k, NULL, NULL, NULL, NULL);
    }
}

static void draw_graph(App *a, PView *v, Rect r, bool interactive) {
    if (G.style == 0) { draw_graph_rows(a, v, r, interactive); return; }
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r, color_hex(T_EDITOR_BG));
    if (v->fitted && !v->user_view && (fabsf(v->fit_w - r.w) > 2 || fabsf(v->fit_h - r.h) > 2)) v->fitted = false;
    if (!v->fitted) fit(a, v, r);
    bool hov = rect_contains(r, in->mouse_x, in->mouse_y);
    if (interactive && hov && in->wheel_y != 0) {   /* zoom around the pointer (EPL-025) */
        v->user_view = true;
        float old = v->zoom; v->zoom = fmaxf(0.15f, fminf(3.0f, v->zoom * (in->wheel_y > 0 ? 1.12f : 1 / 1.12f)));
        float k = v->zoom / old; v->panx = in->mouse_x - r.x - (in->mouse_x - r.x - v->panx) * k; v->pany = in->mouse_y - r.y - (in->mouse_y - r.y - v->pany) * k;
    }
    clip_push(R, r);
    /* edges first: real topology only, child -> parent; thickness normalised by rows (EPL-029) */
    double maxr = 1; for (int i = 0; i < v->p.n; i++) { const PlanNode *n = &v->p.nodes[i]; double rr = n->act_rows.st == PV_PRESENT ? n->act_rows.v : n->est_rows.st == PV_PRESENT ? n->est_rows.v : 0; if (rr > maxr) maxr = rr; }
    for (int i = 0; i < v->p.n; i++) {
        const PlanNode *n = &v->p.nodes[i];
        if (n->parent < 0) continue;
        const PlanNode *pa = &v->p.nodes[n->parent];
        bool hidden = false; for (int q = n->parent; q >= 0; q = v->p.nodes[q].parent) if (v->p.nodes[q].collapsed) hidden = true;
        if (hidden) continue;
        Rect a1 = node_rect(a, v, r, n), b1 = node_rect(a, v, r, pa);
        double rr = n->act_rows.st == PV_PRESENT ? n->act_rows.v : n->est_rows.st == PV_PRESENT ? n->est_rows.v : -1;
        float th = G.thick && rr >= 0 ? px(a, 1) + px(a, 7) * (float)(log10(rr + 1) / log10(maxr + 1)) : px(a, 1.5f);
        float x1 = G.rtl ? a1.x : a1.x + a1.w / 2, y1 = G.rtl ? a1.y + a1.h / 2 : a1.y, x2 = G.rtl ? b1.x + b1.w : b1.x + b1.w / 2, y2 = G.rtl ? b1.y + b1.h / 2 : b1.y + b1.h;
        float mx = (x1 + x2) / 2;
        if (G.rtl) { draw_line(R, x1, y1, mx, y1, th, color_hex(T_BORDER)); draw_line(R, mx, y1, mx, y2, th, color_hex(T_BORDER)); draw_line(R, mx, y2, x2, y2, th, color_hex(T_BORDER)); }
        else draw_line(R, x1, y1, x2, y2, th, color_hex(T_BORDER));
        /* arrow towards the consumer (data flows to the parent) */
        draw_line(R, x2, y2, x2 + px(a, 6), y2 - px(a, 4), px(a, 1.5f), color_hex(T_TEXT_MUTED)); draw_line(R, x2, y2, x2 + px(a, 6), y2 + px(a, 4), px(a, 1.5f), color_hex(T_TEXT_MUTED));
    }
    int hover = -1;
    for (int i = 0; i < v->p.n; i++) {
        PlanNode *n = &v->p.nodes[i];
        bool hidden = false; for (int q = n->parent; q >= 0; q = v->p.nodes[q].parent) if (v->p.nodes[q].collapsed) hidden = true;
        if (hidden) continue;
        Rect nr = node_rect(a, v, r, n);
        if (nr.x > r.x + r.w || nr.y > r.y + r.h || nr.x + nr.w < r.x || nr.y + nr.h < r.y) continue;   /* only what is visible is drawn (EPL-078) */
        bool sel = v->sel == n->id, hit = G.search.buf[0] && (strcasestr(n->op, G.search.buf) || strcasestr(n->object, G.search.buf) || strcasestr(n->index, G.search.buf));
        if (interactive && rect_contains(nr, in->mouse_x, in->mouse_y)) hover = i;
        draw_rrect(R, nr, px(a, 6) * v->zoom, color_hex(sel ? T_ACCENT : hit ? T_YELLOW : T_BORDER));
        draw_rrect(R, rect_inset(nr, px(a, 1.5f), px(a, 1.5f)), px(a, 5) * v->zoom, color_hex(T_PANEL_ALT));
        draw_rect(R, rect_make(nr.x + px(a, 2), nr.y + px(a, 2), px(a, 4) * v->zoom, nr.h - px(a, 4)), color_hex(kind_color(n)));
        char tag[160]; snprintf(tag, sizeof tag, "plannode:%s", n->pid); ui_record(tag, nr);
        if (v->zoom >= 0.35f) {
            float lh = nr.h / 3.2f, x = nr.x + px(a, 10) * v->zoom;
            char l1[200], l2[300], l3[200], er[32], ar[32], cs[32];
            snprintf(l1, sizeof l1, "%s%s", (n->flags & (PF_SPILL | PF_WARN | PF_CARD | PF_HOT)) ? "! " : "", n->op);   /* a mark in text, not colour only (EPL-080) */
            snprintf(l2, sizeof l2, "%s%s%s", n->object, n->index[0] ? " \xC2\xB7 " : "", n->index);
            plan_num_fmt(n->est_rows, er, sizeof er, 0); plan_num_fmt(n->act_rows, ar, sizeof ar, 0); plan_num_fmt(n->est_cost, cs, sizeof cs, 1);
            if (v->p.dialect == PL_SQLITE) snprintf(l3, sizeof l3, "#%s", n->pid);
            else snprintf(l3, sizeof l3, "#%s  est %s%s%s  cost %s", n->pid, n->est_rows.st == PV_PRESENT ? er : "-", n->act_rows.st == PV_PRESENT ? "  act " : "", n->act_rows.st == PV_PRESENT ? ar : "", n->est_cost.st == PV_PRESENT ? cs : "-");
            Font *f = v->zoom >= 0.8f ? a->font_ui : a->font_small;
            draw_text_fit(f, R, l1, -1, rect_make(x, nr.y + px(a, 3), nr.w - px(a, 14) * v->zoom, lh), color_hex(T_TEXT), ALIGN_LEFT);
            draw_text_fit(a->font_small, R, l2, -1, rect_make(x, nr.y + lh, nr.w - px(a, 14) * v->zoom, lh), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            draw_text_fit(a->font_small, R, l3, -1, rect_make(x, nr.y + lh * 2, nr.w - px(a, 14) * v->zoom, lh), color_hex((n->flags & PF_CARD) ? T_YELLOW : T_TEXT_DIM), ALIGN_LEFT);
            if (n->nchildren && n->collapsed) { char c[32]; snprintf(c, sizeof c, "+%d", n->nchildren); draw_text_fit(a->font_small, R, c, -1, rect_make(nr.x + nr.w - px(a, 30), nr.y, px(a, 28), lh), color_hex(T_ACCENT), ALIGN_RIGHT); }
        }
    }
    clip_pop(R);
    if (!interactive) return;
    if (hover >= 0 && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; v->sel = hover; if (in->clicks >= 2 && v->p.nodes[hover].nchildren) v->p.nodes[hover].collapsed = !v->p.nodes[hover].collapsed, relayout(&v->p); }
    else if (hov && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; G.dragging = true; G.dx = in->mouse_x - v->panx; G.dy = in->mouse_y - v->pany; }
    if (G.dragging) { if (in->mouse_down) { if (fabsf(in->mouse_x - G.dx - v->panx) > 1 || fabsf(in->mouse_y - G.dy - v->pany) > 1) v->user_view = true; v->panx = in->mouse_x - G.dx; v->pany = in->mouse_y - G.dy; } else G.dragging = false; }
    if (hover >= 0 && in->mouse_rpressed && !in->mouse_consumed) {   /* the node's menu (EPL-032): no DDL from here */
        in->mouse_consumed = true; v->sel = hover;
        popup_open(&G.menu, in->mouse_x, in->mouse_y);
        popup_add(&G.menu, 1, "Copy Properties", false, true);
        popup_add(&G.menu, 2, "Focus Node", false, true);
        popup_add(&G.menu, 3, v->p.nodes[hover].collapsed ? "Expand Subtree" : "Collapse Subtree", false, v->p.nodes[hover].nchildren > 0);
        popup_add(&G.menu, 4, "Explain this Operator (AI)...", true, true);
        popup_add(&G.menu, 5, "Analyze Subtree with AI...", false, true);
        bool idx = false; for (int i = 0; i < v->nf; i++) if (v->f[i].node == hover && v->f[i].kind == FD_INDEX) idx = true;
        popup_add(&G.menu, 6, "Suggest Index... (review, never applied from here)", true, idx);
    }
    /* tooltip (EPL-026): everything known, the unknown said so */
    if (hover >= 0 && !G.menu.open) {
        const PlanNode *n = &v->p.nodes[hover];
        char er[32], ar[32], cs[32], lp[32], ms[32], tip[1600];
        plan_num_fmt(n->est_rows, er, sizeof er, 2); plan_num_fmt(n->act_rows, ar, sizeof ar, 2); plan_num_fmt(n->est_cost, cs, sizeof cs, 2); plan_num_fmt(n->loops, lp, sizeof lp, 0); plan_num_fmt(n->act_ms, ms, sizeof ms, 3);
        snprintf(tip, sizeof tip, "%s  (#%s, %s)\nObject: %s %s\nEstimated rows: %s   Actual rows: %s   Loops: %s\nCost: %s (optimizer units)   Actual time: %s ms\nFlow: rows go from this operator to %s\n%.400s%s%s",
                 n->op, n->pid, n->kind, n->object[0] ? n->object : "-", n->index, er, ar, lp, cs, ms, n->parent >= 0 ? v->p.nodes[n->parent].op : "the result", n->predicate, n->warning[0] ? "\nWarning: " : "", n->warning);
        float tw = px(a, 460), th = px(a, 120);
        Rect tr = rect_make(fminf(in->mouse_x + px(a, 14), r.x + r.w - tw), fminf(in->mouse_y + px(a, 14), r.y + r.h - th), tw, th);
        draw_rrect(R, tr, px(a, 6), color_hex(T_BORDER)); draw_rrect(R, rect_inset(tr, px(a, 1), px(a, 1)), px(a, 5), color_hex(T_PANEL));
        const char *s = tip; float y = tr.y + px(a, 6);
        while (*s && y < tr.y + th - px(a, 16)) { const char *e = strchr(s, '\n'); int l = e ? (int)(e - s) : (int)strlen(s); draw_text_fit(a->font_small, R, s, l, rect_make(tr.x + px(a, 8), y, tw - px(a, 16), px(a, 18)), color_hex(T_TEXT), ALIGN_LEFT); y += px(a, 18); s += l + (e ? 1 : 0); }
    }
}

static int ops_cmp_col;
static PView *ops_v;
static int ops_cmp(const void *x, const void *y) {
    const PlanNode *a = &ops_v->p.nodes[*(const int *)x], *b = &ops_v->p.nodes[*(const int *)y];
    double va = 0, vb = 0; int r = 0;
    switch (ops_cmp_col) {
    case 1: r = strcmp(a->op, b->op); break;
    case 2: r = strcmp(a->object, b->object); break;
    case 3: va = a->est_rows.st == PV_PRESENT ? a->est_rows.v : -1; vb = b->est_rows.st == PV_PRESENT ? b->est_rows.v : -1; r = va < vb ? -1 : va > vb; break;
    case 4: va = a->act_rows.st == PV_PRESENT ? a->act_rows.v : -1; vb = b->act_rows.st == PV_PRESENT ? b->act_rows.v : -1; r = va < vb ? -1 : va > vb; break;
    case 5: va = a->est_cost.st == PV_PRESENT ? a->est_cost.v : -1; vb = b->est_cost.st == PV_PRESENT ? b->est_cost.v : -1; r = va < vb ? -1 : va > vb; break;
    default: r = a->id - b->id;
    }
    return G.ops_desc ? -r : r;
}
static void draw_ops(App *a, PView *v, Rect r) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r, color_hex(T_EDITOR_BG));
    static const char *hdr[] = { "#", "Operator", "Object / index", "Est. rows", "Act. rows", "Cost", "Flags" };
    float cw[] = { 0.05f, 0.28f, 0.25f, 0.1f, 0.1f, 0.1f, 0.12f };
    float rh = px(a, M_ROW), x = r.x;
    for (int c = 0; c < 7; c++) {
        Rect hr = rect_make(x, r.y, r.w * cw[c], rh);
        draw_rect(R, hr, color_hex(T_PANEL));
        char t[48]; snprintf(t, sizeof t, "%s%s", hdr[c], G.ops_sort == c ? (G.ops_desc ? " v" : " ^") : "");
        draw_text_fit(a->font_small, R, t, -1, rect_inset(hr, px(a, 6), 0), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        char tg[48]; snprintf(tg, sizeof tg, "planops:%s", hdr[c]); ui_record(tg, hr);
        if (rect_contains(hr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed && c < 6) { in->mouse_consumed = true; if (G.ops_sort == c) G.ops_desc = !G.ops_desc; else { G.ops_sort = c; G.ops_desc = c >= 3; } }
        x += hr.w;
    }
    int idx[4096]; int n = v->p.n < 4096 ? v->p.n : 4096; for (int i = 0; i < n; i++) idx[i] = i;
    ops_v = v; ops_cmp_col = G.ops_sort; qsort(idx, (size_t)n, sizeof idx[0], ops_cmp);   /* sorting the table never touches the graph (EPL-028.2) */
    Rect body = rect_make(r.x, r.y + rh, r.w, r.h - rh);
    if (rect_contains(body, in->mouse_x, in->mouse_y) && in->wheel_y != 0) G.ops_scroll -= in->wheel_y;
    G.ops_scroll = fmaxf(0, fminf(G.ops_scroll, n * rh - body.h));
    ui_scrollbar_input(a, &G.ops_sb, body, n * rh, &G.ops_scroll);
    clip_push(R, body);
    for (int k = (int)(G.ops_scroll / rh); k < n && (k * rh - G.ops_scroll) < body.h; k++) {
        const PlanNode *nd = &v->p.nodes[idx[k]];
        Rect rr = rect_make(body.x, body.y + k * rh - G.ops_scroll, body.w, rh);
        if (v->sel == nd->id) draw_rect(R, rr, color_hex(T_SELECTED)); else if (rect_contains(rr, in->mouse_x, in->mouse_y)) draw_rect(R, rr, color_hex(T_HOVER));
        char cells[7][300]; char er[32], ar[32], cs[32];
        plan_num_fmt(nd->est_rows, er, sizeof er, 1); plan_num_fmt(nd->act_rows, ar, sizeof ar, 1); plan_num_fmt(nd->est_cost, cs, sizeof cs, 2);
        snprintf(cells[0], 300, "%s", nd->pid); snprintf(cells[1], 300, "%*s%s", nd->depth * 2, "", nd->op);
        snprintf(cells[2], 300, "%s%s%s", nd->object, nd->index[0] ? " / " : "", nd->index);
        snprintf(cells[3], 300, "%s", er); snprintf(cells[4], 300, "%s", ar); snprintf(cells[5], 300, "%s", cs);
        snprintf(cells[6], 300, "%s%s%s%s", nd->flags & PF_CARD ? "estimate " : "", nd->flags & (PF_SCAN | PF_HOT) ? "scan " : "", nd->flags & PF_SPILL ? "spill " : "", nd->flags & PF_WARN ? "warning" : "");
        x = rr.x;
        for (int c = 0; c < 7; c++) { draw_text_fit(c == 1 ? a->font_mono : a->font_small, R, cells[c], -1, rect_make(x + px(a, 6), rr.y, r.w * cw[c] - px(a, 8), rr.h), color_hex(c == 6 ? T_YELLOW : T_TEXT), ALIGN_LEFT); x += r.w * cw[c]; }
        char tg[64]; snprintf(tg, sizeof tg, "planrow:%s", nd->pid); ui_record(tg, rr);
        if (rect_contains(rr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; v->sel = nd->id; if (in->clicks >= 2) { G.view = V_GRAPH; v->fitted = true; } }
    }
    clip_pop(R);
    ui_scrollbar_draw(a, &G.ops_sb, body, n * rh, G.ops_scroll);
}
static void draw_text_block(App *a, Rect r, const char *t, float *scroll, Font *f) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r, color_hex(T_EDITOR_BG));
    float rh = font_height(f) + px(a, 3);
    int lines = 0; for (const char *s = t; *s; s++) lines += *s == '\n'; lines++;
    if (rect_contains(r, in->mouse_x, in->mouse_y) && in->wheel_y != 0) *scroll -= in->wheel_y;
    *scroll = fmaxf(0, fminf(*scroll, lines * rh - r.h + rh));
    ScrollBar *psb = NULL;   /* one bar per text pane, keyed by its scroll variable */
    for (int k = 0; k < 4 && !psb; k++) if (G.pane_key[k] == scroll) psb = &G.pane_sb[k];
    for (int k = 0; k < 4 && !psb; k++) if (!G.pane_key[k]) { G.pane_key[k] = scroll; psb = &G.pane_sb[k]; }
    if (psb) ui_scrollbar_input(a, psb, r, lines * rh + rh, scroll);
    clip_push(R, r);
    int i = 0; const char *s = t;
    while (*s) {
        const char *e = strchr(s, '\n'); int l = e ? (int)(e - s) : (int)strlen(s);
        float y = r.y + px(a, 6) + i * rh - *scroll;
        if (y > r.y + r.h) break;
        if (y + rh >= r.y) draw_text_fit(f, R, s, l, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), rh), color_hex(T_TEXT), ALIGN_LEFT);
        s += l + (e ? 1 : 0); i++;
    }
    clip_pop(R);
    if (psb) ui_scrollbar_draw(a, psb, r, lines * rh + rh, *scroll);
}
static void open_copy(App *a, PView *v, const char *title, const char *text) {
    panels_open_tab(a->panels, a, title, v->conn_id, text);
    app_status(a, "Opened as a copy in a new tab: nothing was run");
}
static void suggest_index(App *a, PView *v, int fi) {
    char ddl[700], why[400];
    if (!plan_index_ddl(&v->p, &v->f[fi], ddl, sizeof ddl, why, sizeof why)) { app_status(a, "%s", why); return; }
    Connection *c = conn_of(a, v->conn_id);
    const PlanNode *n = &v->p.nodes[v->f[fi].node];
    /* the table designer applies it through its own review (EPL-060 / 062); without a session, a copy */
    char cols[600] = ""; const char *o = strchr(ddl, '('), *e = o ? strrchr(ddl, ')') : NULL;
    if (o && e) { int w = 0; for (const char *s = o + 1; s < e && w < 590; s++) if (*s != '"' && *s != '`' && *s != '[' && *s != ']') cols[w++] = *s; cols[w] = 0; }
    char sch[160] = "", tb[160]; const char *dot = strrchr(n->object, '.');
    if (dot) { snprintf(sch, sizeof sch, "%.*s", (int)(dot - n->object), n->object); snprintf(tb, sizeof tb, "%s", dot + 1); } else snprintf(tb, sizeof tb, "%s", n->object);
    for (char *x = sch; *x; x++) if (*x == '[' || *x == ']') memmove(x, x + 1, strlen(x));
    for (char *x = tb; *x; x++) if (*x == '[' || *x == ']') memmove(x, x + 1, strlen(x));
    if (!sch[0] && c) { const char *u = !strcmp(c->driver, "postgres") ? "public" : !strcmp(c->driver, "sqlite") ? "main" : !strcmp(c->driver, "oracle") ? conn_value(c, "user") : !strcmp(c->driver, "sqlserver") ? "dbo" : conn_value(c, "database"); snprintf(sch, sizeof sch, "%s", u ? u : ""); if (!strcmp(c->driver, "oracle")) for (char *x = sch; *x; x++) *x = (char)toupper((unsigned char)*x); }
    char iname[128]; const char *q = strstr(ddl, "INDEX "); snprintf(iname, sizeof iname, "%s", q ? q + 7 : "ix"); char *qe = strpbrk(iname, "\"`] "); if (qe) *qe = 0;
    if (c && c->db && tdes_supported(c)) tdes_open_with_index(a, c, sch, tb, cols, iname, why);
    else { char text[1400]; snprintf(text, sizeof text, "-- Index candidate from execution plan #%d (%s). A hypothesis: review existing indexes, write cost and storage first.\n-- %s\n%s;\n", v->seq, plan_dialect_name(v->p.dialect), why, ddl); open_copy(a, v, "index-candidate.sql", text); }
}

/* roadmap item 16: the plan of the same statement with the index candidate as a hypothetical index */
static void estimate_index(App *a, PView *v, int fi) {
    char ddl[700], why[400];
    Connection *c = conn_of(a, v->conn_id);
    if (!c || !c->db || strcmp(c->driver, "postgres")) { app_status(a, "Estimating with a hypothetical index is offered on PostgreSQL (hypopg)"); return; }
    if (!v->p.sql || G.job) return;
    if (!plan_index_ddl(&v->p, &v->f[fi], ddl, sizeof ddl, why, sizeof why)) { app_status(a, "%s", why); return; }
    snprintf(G.hypo_ddl, sizeof G.hypo_ddl, "%s", ddl);
    G.cmp_after = v->seq;
    planview_explain(a, c, v->p.sql, (int)strlen(v->p.sql), PM_ESTIMATED, "Estimated plan with a hypothetical index (nothing created)");
    if (!G.job) { G.cmp_after = 0; G.hypo_ddl[0] = 0; }
}
/* ---- the panel ---- */

static void pv_draw(void *view, void *ctx, DockRect drc, bool focused) {
    (void)view; (void)focused;
    App *a = ctx; G.a = a;
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    Rect r = rect_make(drc.x, drc.y, drc.w, drc.h);
    draw_rect(R, r, color_hex(T_PANEL));
    float bh = px(a, 28);
    /* plan chips: #n, mode, engine; pinned plans keep their slot */
    float x = r.x + px(a, 8);
    for (int k = 0; k < MAX_PLANS; k++) {
        int i = -1, best = 1 << 30; for (int j = 0; j < MAX_PLANS; j++) { if (!G.v[j].used) continue; int rank = 0; for (int q = 0; q < MAX_PLANS; q++) if (G.v[q].used && G.v[q].seq > G.v[j].seq) rank++; if (rank == k) i = j; (void)best; }
        if (i < 0) continue;
        PView *v = &G.v[i];
        char t[96]; snprintf(t, sizeof t, "%s#%d %s \xC2\xB7 %s", v->pinned ? "* " : "", v->seq, v->p.mode == PM_ACTUAL ? "Actual" : "Estimated", plan_dialect_name(v->p.dialect));
        float w = font_width(a->font_small, t, -1) + px(a, 20);
        if (x + w > r.x + r.w * 0.6f) break;
        Rect cr = rect_make(x, r.y + px(a, 4), w, bh - px(a, 6));
        /* the plan shown: the selection style (tinted, accent edge), never the primary-button blue */
        draw_rrect(R, cr, px(a, 4), color_hex(i == G.cur ? T_ACCENT_DIM : T_BORDER));
        draw_rrect(R, rect_inset(cr, px(a, 1), px(a, 1)), px(a, 3), color_hex(i == G.cur ? T_SELECTED : T_PANEL_ALT));
        draw_text_fit(a->font_small, R, t, -1, rect_inset(cr, px(a, 8), 0), color_hex(i == G.cur ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        char tg[64]; snprintf(tg, sizeof tg, "planchip:#%d", v->seq); ui_record(tg, cr);
        if (rect_contains(cr, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; if (in->clicks >= 2) v->pinned = !v->pinned; G.cur = i; }
        x += w + px(a, 6);
    }
    PView *v = curv();
    /* view switcher + zoom + search */
    static const char *views[] = { "Graph", "Operators", "Raw", "Findings", "AI" };
    Rect seg = rect_make(r.x + r.w - px(a, 400), r.y + px(a, 3), px(a, 392), bh - px(a, 4)); ui_record("plan:views", seg);
    int s = ui_segmented(a, seg, views, V_N, G.view); if (s >= 0) G.view = s;
    float y = r.y + bh + px(a, 2);
    if (G.job) {   /* collecting: cancellable, the rest of the IDE works (EPL-005 / 079) */
        Rect lr = rect_make(r.x + px(a, 10), y, r.w - px(a, 130), bh);
        draw_text_fit(a->font_small, R, G.status, -1, lr, color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        if (ui_button(a, rect_make(r.x + r.w - px(a, 110), y + px(a, 2), px(a, 100), bh - px(a, 4)), "Cancel", BTN_NORMAL, true)) exec_cancel(G.job);
        app_wake_in(a, 100);
        y += bh;
    }
    if (!v) {
        draw_text_fit(a->font_ui, R, G.status[0] && !G.job ? G.status : "No plan yet: Query > Explain Plan (estimated, the statement is not run) or Explain Analyze (runs it).", -1, rect_make(r.x + px(a, 14), y + px(a, 10), r.w - px(a, 28), px(a, 26)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        return;
    }
    /* summary: engine, mode, version, time; honest units (EPL-002 / 035) */
    char sum[700]; plan_summary(&v->p, sum, sizeof sum);
    char line[900]; time_t t = (time_t)v->p.captured_at; struct tm tm; localtime_r(&t, &tm); char ts[16]; strftime(ts, sizeof ts, "%H:%M:%S", &tm);
    snprintf(line, sizeof line, "%s \xC2\xB7 %s \xC2\xB7 %s%s", sum, v->p.conn_name, ts, v->nf ? "" : "");
    draw_rect(R, rect_make(r.x, y, r.w, bh), color_alpha(color_hex(v->p.mode == PM_ACTUAL ? T_YELLOW : T_ACCENT), 30));
    draw_text_fit(a->font_small, R, line, -1, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), bh), color_hex(T_TEXT), ALIGN_LEFT);
    ui_record("plan:summary", rect_make(r.x, y, r.w, bh));
    y += bh;
    if (v->p.notes[0]) { draw_text_fit(a->font_small, R, v->p.notes, -1, rect_make(r.x + px(a, 10), y, r.w - px(a, 20), px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT); y += px(a, 20); }
    Rect body = rect_make(r.x, y, r.w, r.y + r.h - y);
    if (G.view == V_GRAPH) {
        Rect tb = rect_make(body.x + px(a, 8), body.y + px(a, 2), body.w, bh - px(a, 4));
        if (ui_button(a, rect_make(tb.x, tb.y, px(a, 44), tb.h), "Fit", BTN_QUIET, true)) { v->fitted = false; v->user_view = false; }
        if (ui_button(a, rect_make(tb.x + px(a, 46), tb.y, px(a, 28), tb.h), "-", BTN_QUIET, true)) { v->zoom = fmaxf(0.15f, v->zoom / 1.2f); v->user_view = true; }
        char zt[16]; snprintf(zt, sizeof zt, "%d%%", (int)(v->zoom * 100)); draw_text_fit(a->font_small, R, zt, -1, rect_make(tb.x + px(a, 76), tb.y, px(a, 44), tb.h), color_hex(T_TEXT_MUTED), ALIGN_CENTER);
        if (ui_button(a, rect_make(tb.x + px(a, 120), tb.y, px(a, 28), tb.h), "+", BTN_QUIET, true)) { v->zoom = fminf(3.0f, v->zoom * 1.2f); v->user_view = true; }
        if (ui_button(a, rect_make(tb.x + px(a, 152), tb.y, px(a, 110), tb.h), G.style == 0 ? "Style: SQL Server" : G.rtl ? "Compact: R to L" : "Compact: top down", BTN_QUIET, true)) {
            /* SQL Server look -> compact right-to-left -> compact top-down -> SQL Server look */
            if (G.style == 0) { G.style = 1; G.rtl = true; } else if (G.rtl) G.rtl = false; else G.style = 0;
            for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used) { relayout(&G.v[i].p); G.v[i].fitted = false; }
        }
        if (ui_button(a, rect_make(tb.x + px(a, 266), tb.y, px(a, 120), tb.h), G.thick ? "Edge width: rows" : "Edge width: flat", BTN_QUIET, true)) G.thick = !G.thick;
        static bool sf; Rect sr = rect_make(tb.x + px(a, 392), tb.y, px(a, 220), tb.h); ui_record("plan:search", sr);
        if (ui_textfield(a, &G.search, sr, "Find operator / table / index", sf && a->focus == FOCUS_RESULTS)) { sf = true; a->focus = FOCUS_RESULTS; }
        if (ui_button(a, rect_make(tb.x + px(a, 616), tb.y, px(a, 70), tb.h), "Next", BTN_QUIET, G.search.buf[0] != 0)) {   /* next match, unknown operators included (EPL-031) */
            for (int k = 1; k <= v->p.n; k++) { int i = (v->sel + k + v->p.n) % v->p.n; PlanNode *n = &v->p.nodes[i]; if (strcasestr(n->op, G.search.buf) || strcasestr(n->object, G.search.buf) || strcasestr(n->index, G.search.buf)) { v->sel = i; break; } }
        }
        if (ui_button(a, rect_make(tb.x + px(a, 690), tb.y, px(a, 150), tb.h), "Compare with...", BTN_QUIET, true)) {
            popup_open(&G.menu, tb.x + px(a, 690), tb.y + tb.h);
            static char lab[MAX_PLANS][64];
            for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used && i != G.cur) { snprintf(lab[i], sizeof lab[i], "#%d %s %s", G.v[i].seq, G.v[i].p.mode == PM_ACTUAL ? "Actual" : "Estimated", plan_dialect_name(G.v[i].p.dialect)); popup_add(&G.menu, 100 + i, lab[i], false, true); }
            popup_add(&G.menu, 99, "No comparison", true, G.cmp >= 0);
        }
        Rect cv = rect_make(body.x, body.y + bh, body.w, body.h - bh);
        float pw = fminf(px(a, 360), cv.w * 0.32f);
        PView *b = G.cmp >= 0 && G.cmp < MAX_PLANS && G.v[G.cmp].used && G.cmp != G.cur ? &G.v[G.cmp] : NULL;
        if (b) {   /* side by side, each with its own layout (EPL-065) */
            Rect left = rect_make(cv.x, cv.y, cv.w / 2 - px(a, 1), cv.h - px(a, 46)), right = rect_make(cv.x + cv.w / 2 + px(a, 1), cv.y, cv.w / 2 - px(a, 1), cv.h - px(a, 46));
            draw_graph(a, v, left, true); draw_graph(a, b, right, false);
            PlanDiff dd; plan_compare(&v->p, &b->p, &dd);
            char ct[700]; char ca[32], cb[32]; plan_num_fmt(v->p.total_cost, ca, sizeof ca, 2); plan_num_fmt(b->p.total_cost, cb, sizeof cb, 2);
            snprintf(ct, sizeof ct, "#%d vs #%d: %d operator(s) differ%s \xC2\xB7 cost %s / %s %s \xC2\xB7 runtime: %s \xC2\xB7 results equivalence: not verified",
                     v->seq, b->seq, dd.changed_ops, dd.changed_ops ? "" : " (same shape)", ca, cb, dd.same_engine ? "(same engine)" : "(different engines: not comparable)",
                     v->p.mode == PM_ACTUAL && b->p.mode == PM_ACTUAL ? "see the summaries" : "not measured on both");
            draw_text_fit(a->font_small, R, ct, -1, rect_make(cv.x + px(a, 10), cv.y + cv.h - px(a, 44), cv.w - px(a, 20), px(a, 20)), color_hex(T_TEXT), ALIGN_LEFT);
            ui_record("plan:compare", rect_make(cv.x, cv.y + cv.h - px(a, 44), cv.w, px(a, 40)));
        } else {
            Rect g = rect_make(cv.x, cv.y, cv.w - pw, cv.h);
            draw_graph(a, v, g, true);
            /* properties of the selected node (EPL-027) */
            Rect pr = rect_make(cv.x + cv.w - pw, cv.y, pw, cv.h);
            draw_rect(R, pr, color_hex(T_PANEL)); draw_rect(R, rect_make(pr.x, pr.y, px(a, 1), pr.h), color_hex(T_BORDER));
            if (v->sel >= 0 && v->sel < v->p.n) {
                draw_text_fit(a->font_small, R, "PROPERTIES", -1, rect_make(pr.x + px(a, 8), pr.y + px(a, 4), pw, px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
                PL l = { a, rect_make(pr.x, pr.y + px(a, 28), pw, pr.h - px(a, 60)), pr.y + px(a, 28), 0, NULL };
                props_lines(v, &v->p.nodes[v->sel], draw_prop, &l);
                if (ui_button(a, rect_make(pr.x + px(a, 8), pr.y + pr.h - px(a, 30), px(a, 120), px(a, 26)), "Copy Properties", BTN_QUIET, true)) copy_props(a, v);
            } else draw_text_fit(a->font_small, R, "Select an operator to see its properties", -1, rect_make(pr.x + px(a, 8), pr.y + px(a, 8), pw - px(a, 16), px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        }
    } else if (G.view == V_OPS) draw_ops(a, v, body);
    else if (G.view == V_RAW) {
        Rect tb = rect_make(body.x + px(a, 8), body.y + px(a, 2), body.w, bh - px(a, 4));
        if (ui_button(a, rect_make(tb.x, tb.y, px(a, 110), tb.h), "Copy Raw", BTN_QUIET, v->p.raw != NULL)) { SDL_SetClipboardText(v->p.raw); app_status(a, "Raw plan copied"); }
        draw_text_block(a, rect_make(body.x, body.y + bh, body.w, body.h - bh), v->p.raw ? v->p.raw : "(no raw text kept for this plan)", &G.raw_scroll, a->font_mono);
    } else if (G.view == V_FINDINGS) {
        draw_rect(R, body, color_hex(T_EDITOR_BG));
        Rect tb = rect_make(body.x + px(a, 8), body.y + px(a, 2), body.w, bh - px(a, 4));
        if (ui_button(a, rect_make(tb.x, tb.y, px(a, 190), tb.h), "Export Report (Markdown)", BTN_QUIET, true)) {
            char *md = plan_report_md(&v->p, v->f, v->nf);
            char dir[1100]; pref_dir(a, dir, sizeof dir); strncat(dir, "/reports", sizeof dir - strlen(dir) - 1); mkdir(dir, 0700);
            char p[1300]; snprintf(p, sizeof p, "%s/plan-%d-%ld.md", dir, v->seq, (long)time(NULL));
            FILE *f = fopen(p, "w"); if (f) { fputs(md, f); fclose(f); app_status(a, "Report saved: %s (no SQL text, no connection details)", p); } else app_status(a, "Could not write %s: %s", p, strerror(errno));
            free(md);
        }
        float fy = body.y + bh + px(a, 6) - G.f_scroll;
        if (rect_contains(body, in->mouse_x, in->mouse_y) && in->wheel_y != 0) G.f_scroll = fmaxf(0, G.f_scroll - in->wheel_y);
        Rect fview = rect_make(body.x, body.y + bh, body.w, body.h - bh);
        if (G.f_total > 0 && G.f_scroll > G.f_total - fview.h) G.f_scroll = G.f_total - fview.h > 0 ? G.f_total - fview.h : 0;
        ui_scrollbar_input(a, &G.f_sb, fview, G.f_total, &G.f_scroll);
        clip_push(R, rect_make(body.x, body.y + bh, body.w, body.h - bh));
        if (!v->nf) draw_text_fit(a->font_ui, R, "No finding: the rules found nothing to point at (not a proof that the query is optimal).", -1, rect_make(body.x + px(a, 14), fy, body.w - px(a, 28), px(a, 24)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
        static const char *cf[] = { "low", "medium", "high" };
        for (int i = 0; i < v->nf; i++) {
            PlanFinding *f = &v->f[i];
            char t1[400], t2[700], t3[700], t4[700];
            snprintf(t1, sizeof t1, "%d. %s  (confidence %s)", i + 1, f->title, cf[f->confidence]);
            snprintf(t2, sizeof t2, "Evidence: %s", f->evidence); snprintf(t3, sizeof t3, "To confirm: %s", f->missing[0] ? f->missing : "-"); snprintf(t4, sizeof t4, "Suggestion: %s", f->suggestion);
            draw_text_fit(a->font_ui, R, t1, -1, rect_make(body.x + px(a, 14), fy, body.w - px(a, 330), px(a, 22)), color_hex(f->kind == FD_SPILL || f->kind == FD_WARNING ? T_RED : T_YELLOW), ALIGN_LEFT);
            Rect gb = rect_make(body.x + body.w - px(a, 312), fy, px(a, 100), px(a, 22));
            char tg[48]; snprintf(tg, sizeof tg, "plan:goto %d", i + 1); ui_record(tg, gb);
            if (ui_button(a, gb, "Go to node", BTN_QUIET, true)) { v->sel = f->node; G.view = V_GRAPH; }
            if (f->kind == FD_INDEX) { Rect ib = rect_make(body.x + body.w - px(a, 206), fy, px(a, 196), px(a, 22)); snprintf(tg, sizeof tg, "plan:index %d", i + 1); ui_record(tg, ib); if (ui_button(a, ib, "Review index candidate...", BTN_QUIET, true)) suggest_index(a, v, i);
                Connection *pc = conn_of(a, v->conn_id);
                if (pc && !strcmp(pc->driver, "postgres")) { Rect eb = rect_make(ib.x - px(a, 196), fy, px(a, 190), px(a, 22)); if (ui_button(a, eb, "Estimate with it (hypopg)", BTN_QUIET, !G.job)) estimate_index(a, v, i); } }
            fy += px(a, 24);
            draw_text_fit(a->font_small, R, t2, -1, rect_make(body.x + px(a, 30), fy, body.w - px(a, 44), px(a, 20)), color_hex(T_TEXT), ALIGN_LEFT); fy += px(a, 20);
            draw_text_fit(a->font_small, R, t3, -1, rect_make(body.x + px(a, 30), fy, body.w - px(a, 44), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT); fy += px(a, 20);
            draw_text_fit(a->font_small, R, t4, -1, rect_make(body.x + px(a, 30), fy, body.w - px(a, 44), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT); fy += px(a, 28);
        }
        clip_pop(R);
        G.f_total = fy + G.f_scroll - fview.y + px(a, 30);   /* content height, for the bar and the clamp of the next frame */
        ui_scrollbar_draw(a, &G.f_sb, fview, G.f_total, G.f_scroll);
    } else if (G.view == V_AI) {   /* one assistant: the plan is analysed in Lume Intelligence, with this plan as context */
        draw_rect(R, body, color_hex(T_EDITOR_BG));
        float ay = body.y + px(a, 12), ax = body.x + px(a, 14), aw = body.w - px(a, 28), rh = px(a, 28);
        draw_text_fit(a->font_ui, R, "Ask Lume Intelligence about this plan", -1, rect_make(ax, ay, aw, rh), color_hex(T_TEXT), ALIGN_LEFT); ay += rh;
        ay += results_draw_wrapped(a->font_small, R, "The assistant (in its panel) gets this plan with literals masked, Lume's findings and the query tab's SQL. It can check its own ideas (plans, same result, time) and its rewrites can be compared here, side by side.", rect_make(ax, ay, aw, px(a, 60)), color_hex(T_TEXT_DIM), 0, true) + px(a, 10);
        static const char *const ql[] = { "Why is it slow?", "Suggest indexes", "Rewrite the query" };
        static const char *const qq[] = { "Por que esta query está lenta? Analise o plano de execução e aponte os operadores caros e por quê.",
                                          "Sugira índices para esta query com base no plano de execução (script para revisar, nunca executado pelo Lume).",
                                          "Reescreva esta query para um plano mais barato, sem mudar o resultado." };
        for (int i = 0; i < 3; i++) {
            Rect b = rect_make(ax + px(a, 196) * (float)i, ay, px(a, 188), rh); char tg[32]; snprintf(tg, sizeof tg, "plan:ask %d", i + 1); ui_record(tg, b);
            if (ui_button(a, b, ql[i], i == 0 ? BTN_PRIMARY : BTN_NORMAL, true)) rptview_ask_sql(a, qq[i]);
        }
        ay += rh + px(a, 8);
        if (v->sel >= 0 && v->sel < v->p.n) {
            char lb[120]; snprintf(lb, sizeof lb, "Explain operator #%s (%s)", v->p.nodes[v->sel].pid, v->p.nodes[v->sel].op);
            Rect b = rect_make(ax, ay, px(a, 300), rh); ui_record("plan:ask op", b);
            if (ui_button(a, b, lb, BTN_QUIET, true)) { char q[300]; snprintf(q, sizeof q, "Explique o operador #%s (%s) do plano em palavras simples e o que os números dele significam.", v->p.nodes[v->sel].pid, v->p.nodes[v->sel].op); rptview_ask_sql(a, q); }
        }
    }
}

static void *pv_create(void *ud, void *ctx) { (void)ud; (void)ctx; return &G; }
static void pv_destroy(void *view, void *ctx) { (void)view; (void)ctx; }
static void pv_vis(void *view, void *ctx, bool shown) { (void)view; (void)ctx; (void)shown; }

static void plan_module_init(Panels *P, App *a) {
    DockPanelDesc d = { .id = "lume.data.plan", .title = "Execution Plan", .icon = "plan", .region = DOCK_BOTTOM,
        .min_w = 360, .min_h = 140, .pref_size = 320, .priority = 2, .start_hidden = true,
        .draw = pv_draw, .create = pv_create, .destroy = pv_destroy, .visibility = pv_vis };
    panels_register(P, a, &d);
}

static void inv(void *ud, const LumeCtx *c) {
    (void)c;
    App *a = G.a; if (!a) return;
    int what = (int)(intptr_t)ud;
    if (what == 1) { panels_show(a->panels, a, "lume.data.plan"); G.view = V_AI; }
    else if (what == 2) panels_show(a->panels, a, "lume.data.plan");
}
void planview_init(App *a) {
    /* G is static (zeroed): no memset, which would touch ~2 MB of pages for nothing (RSS) */
    G.a = a; G.cur = -1; G.cmp = -1; G.rtl = true; G.thick = true;
    ai_config_load(a->conns ? a->conns->dir : NULL, &G.ai);
    panels_add_module("lume.plan", plan_module_init);
    Command c; memset(&c, 0, sizeof c);
    c.id = "plan.analyze_ai"; c.label = "Analyze Plan with AI..."; c.group = "Execution"; c.synonyms = "intelligence tuning llm explain"; c.icon = "plan"; c.invoke = inv; c.ud = (void *)(intptr_t)1; c.module = "lume.plan";
    commands_register(&c);
    c.id = "plan.show"; c.label = "Show Execution Plan"; c.synonyms = "graph plan"; c.ud = (void *)(intptr_t)2;
    commands_register(&c);
}
void planview_shutdown(App *a) {
    (void)a;
    if (G.job) { exec_cancel(G.job); exec_free(G.job); G.job = NULL; }
    for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used) { if (G.v[i].ai) ai_free(G.v[i].ai); plan_free(&G.v[i].p); free(G.v[i].answer); }
    plan_sql_free(&G.ps); free(G.stmt);
}
void planview_pre_update(App *a) {
    { PView *pv = curv(); if (pv) poll_ai(pv); }   /* the legacy plan-only request (harness): answers still land */
    G.a = a;
    if (G.job && !exec_running(G.job)) finish_capture(a);
    else if (G.job) app_wake_in(a, 80);
    for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used && G.v[i].ai) { poll_ai(&G.v[i]); if (G.v[i].ai) app_wake_in(a, 150); }
    if (G.menu.open) {
        int id = popup_update(a, &G.menu);
        if (id >= 0) {
            popup_close(&G.menu);
            PView *v = curv();
            if (v) {
                if (id == 1 && v->sel >= 0) copy_props(a, v);
                else if (id == 2 && v->sel >= 0) { v->fitted = true; v->zoom = 1; v->panx = -v->p.nodes[v->sel].x * px(a, 1) + px(a, 200); v->pany = -v->p.nodes[v->sel].y * px(a, 1) + px(a, 100); }
                else if (id == 3 && v->sel >= 0) { v->p.nodes[v->sel].collapsed = !v->p.nodes[v->sel].collapsed; relayout(&v->p); }
                else if (id == 4 || id == 5) { char q[300]; snprintf(q, sizeof q, "Explique o operador #%s (%s) do plano em palavras simples e o que os números dele significam.", v->p.nodes[v->sel].pid, v->p.nodes[v->sel].op); rptview_ask_sql(a, q); }
                else if (id == 6) { for (int i = 0; i < v->nf; i++) if (v->f[i].node == v->sel && v->f[i].kind == FD_INDEX) { suggest_index(a, v, i); break; } }
                else if (id == 99) G.cmp = -1;
                else if (id >= 100 && id < 100 + MAX_PLANS) G.cmp = id - 100;
            }
        }
    }
}
void planview_draw_overlay(App *a) { if (G.menu.open) popup_draw(a, &G.menu); }

/* ---- harness ---- */

void planview_dump(App *a) {
    (void)a;
    PView *v = curv();
    int used = 0; for (int i = 0; i < MAX_PLANS; i++) used += G.v[i].used;
    printf("planstate plans=%d job=%d view=%d status=\"%s\"", used, G.job != NULL, G.view, G.status);
    if (v) {
        char sum[600]; plan_summary(&v->p, sum, sizeof sum);
        printf(" cur=#%d dialect=%s mode=%s nodes=%d findings=%d sel=%d zoom=%.2f cmp=%d ai=%d answer=%d sql_candidates=%d ai_err=\"%s\" summary=\"%s\"",
               v->seq, plan_dialect_name(v->p.dialect), v->p.mode == PM_ACTUAL ? "actual" : "estimated", v->p.n, v->nf, v->sel, v->zoom, G.cmp >= 0 ? G.v[G.cmp].seq : -1, v->ai != NULL, v->answer != NULL, v->nsql, v->ai_err, sum);
    }
    printf("\n");
    if (v) {
        for (int i = 0; i < v->p.n; i++) { char er[32], ar[32]; plan_num_fmt(v->p.nodes[i].est_rows, er, sizeof er, 1); plan_num_fmt(v->p.nodes[i].act_rows, ar, sizeof ar, 1); printf("plannode %s depth=%d op=\"%s\" object=\"%s\" est=%s act=%s flags=%d\n", v->p.nodes[i].pid, v->p.nodes[i].depth, v->p.nodes[i].op, v->p.nodes[i].object, er, ar, v->p.nodes[i].flags); }
        for (int i = 0; i < v->nf; i++) printf("planfinding %d kind=%d node=%d conf=%d %s | %s\n", i + 1, v->f[i].kind, v->f[i].node, v->f[i].confidence, v->f[i].title, v->f[i].evidence);
        if (v->answer) printf("plananswer %.200s\n", v->answer);
    }
    fflush(stdout);
}
/* plan explain | plan analyze | plan view graph|ops|raw|findings|ai | plan select <pid> | plan import <file> |
 * plan ai send [question] | plan ai share sql|literals|names|raw | plan compare <seq> | plan index <n> | plan cancel | plan pin */
void planview_cmd(App *a, const char *arg) {
    char op[32] = "", rest[600] = ""; sscanf(arg, "%31s %599[^\n]", op, rest);
    PView *v = curv();
    if (!strcmp(op, "explain") || !strcmp(op, "analyze")) {
        char why[200]; int len = 0, base = 0;
        char *st = app_editor_sql(a, RUNSRC_AUTO, false, &len, &base, why, sizeof why);
        Connection *c = panels_tab_connection(a->panels, a, NULL, 0);
        if (!st || !c) { printf("script: plan %s: %s\n", op, st ? "no connection" : why); fflush(stdout); free(st); return; }
        planview_explain(a, c, st, len, op[0] == 'a' ? PM_ACTUAL : PM_ESTIMATED, NULL); free(st);
    } else if (!strcmp(op, "view")) { static const char *n[] = { "graph", "ops", "raw", "findings", "ai" }; for (int i = 0; i < V_N; i++) if (!strcmp(rest, n[i])) G.view = i; panels_show(a->panels, a, "lume.data.plan"); }
    else if (!strcmp(op, "select") && v) { for (int i = 0; i < v->p.n; i++) if (!strcmp(v->p.nodes[i].pid, rest)) v->sel = i; }
    else if (!strcmp(op, "import")) {
        FILE *f = fopen(rest, "rb"); if (!f) { printf("script: plan import failed: %s\n", strerror(errno)); fflush(stdout); return; }
        fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET); char *t = malloc((size_t)n + 1); size_t r = fread(t, 1, (size_t)n, f); t[r] = 0; fclose(f);
        PView *nv = new_slot(); char err[300] = "";
        plan_init(&nv->p);
        if (plan_import(t, r, "auto", &nv->p, err, sizeof err)) { snprintf(nv->p.conn_name, sizeof nv->p.conn_name, "imported file"); nv->p.captured_at = (double)time(NULL); relayout(&nv->p); nv->nf = plan_diagnose(&nv->p, NULL, nv->f, 64); snprintf(G.status, sizeof G.status, "Imported plan #%d (snapshot, no connection)", nv->seq); }
        else { plan_free(&nv->p); nv->used = false; snprintf(G.status, sizeof G.status, "Import refused: %s", err); }
        free(t); panels_show(a->panels, a, "lume.data.plan");
    } else if (!strcmp(op, "ai") && v) {
        char sub[16] = "", q[500] = ""; sscanf(rest, "%15s %499[^\n]", sub, q);
        if (!strcmp(sub, "send")) { if (q[0]) textfield_set(&v->question, q); ai_send(a, v); }
        else if (!strcmp(sub, "share")) { if (!strcmp(q, "sql")) v->share_sql = !v->share_sql; else if (!strcmp(q, "literals")) v->share_lit = !v->share_lit; else if (!strcmp(q, "names")) v->share_names = !v->share_names; else if (!strcmp(q, "raw")) v->share_raw = !v->share_raw; else if (!strcmp(q, "subtree")) v->share_subtree = !v->share_subtree; }
        else if (!strcmp(sub, "preview")) { char *m = ai_user_message(v); printf("planpreview %zu bytes\n%s\nplanpreview-end\n", strlen(m), m); fflush(stdout); free(m); }
        else if (!strcmp(sub, "open")) { int k = atoi(q) - 1; if (k >= 0 && k < v->nsql) open_copy(a, v, "candidate.sql", v->sqlc[k]); }
        else if (!strcmp(sub, "compare")) { int k = atoi(q) - 1; Connection *pc = conn_of(a, v->conn_id); if (k >= 0 && k < v->nsql && pc && !G.job) { G.cmp_after = v->seq; planview_explain(a, pc, v->sqlc[k], (int)strlen(v->sqlc[k]), PM_ESTIMATED, "Candidate from Lume Intelligence (estimated plan, not run)"); if (!G.job) G.cmp_after = 0; } }
    }
    else if (!strcmp(op, "compare")) { int s = atoi(rest); G.cmp = -1; for (int i = 0; i < MAX_PLANS; i++) if (G.v[i].used && G.v[i].seq == s) G.cmp = i; G.view = V_GRAPH; }
    else if (!strcmp(op, "index") && v) { int k = atoi(rest) - 1; if (k >= 0 && k < v->nf) suggest_index(a, v, k); }
    else if (!strcmp(op, "current") && !strcmp(rest, "cmp")) { if (G.cmp >= 0 && G.v[G.cmp].used) { G.cur = G.cmp; G.cmp = -1; } }
    else if (!strcmp(op, "hypo") && v) { int k = atoi(rest) - 1; if (k >= 0 && k < v->nf) estimate_index(a, v, k); }
    else if (!strcmp(op, "cancel") && G.job) exec_cancel(G.job);
    else if (!strcmp(op, "pin") && v) v->pinned = !v->pinned;
    else if (!strcmp(op, "fit") && v) v->fitted = false;
    else { printf("script: plan: unknown op %s\n", op); fflush(stdout); }
}

/* The current plan for the assistant in a query tab (roadmap item 8): its sanitized context (literals
 * masked) and Lume's findings, when it was captured on that connection. NULL when there is none. */
char *planview_context_for(const char *conn_name) {
    if (G.cur < 0 || G.cur >= MAX_PLANS || !G.v[G.cur].used) return NULL;
    PView *v = &G.v[G.cur];
    if (conn_name && strcmp(v->p.conn_name, conn_name)) return NULL;
    PlanShare sh = { false, false, true, false, -1 };   /* names yes (the tab's SQL already went), literals masked, no raw plan */
    char *ctx = plan_context(&v->p, &sh);
    size_t cap = strlen(ctx) + 8192, w = 0; char *o = malloc(cap);
    w += (size_t)snprintf(o + w, cap - w, "plan #%d, %s, %s, %d operator(s)\n%s", v->seq, plan_dialect_name(v->p.dialect), v->p.mode == PM_ACTUAL ? "ACTUAL (the statement ran)" : "ESTIMATED (not run)", v->p.n, ctx);
    free(ctx);
    if (v->nf) w += (size_t)snprintf(o + w, cap - w, "\nLume's findings:\n");
    for (int i = 0; i < v->nf && w + 900 < cap; i++) w += (size_t)snprintf(o + w, cap - w, "- %s | %s%s%s\n", v->f[i].title, v->f[i].evidence, v->f[i].suggestion[0] ? " | suggestion: " : "", v->f[i].suggestion);
    return o;
}

/* A rewrite proposed in the assistant: its estimated plan (never run) beside the current one */
bool planview_compare_sql(App *a, Connection *c, const char *sql) {
    if (!c || !c->db || G.job || !sql || !*sql) return false;
    PView *v = curv(); if (!v) return false;
    G.cmp_after = v->seq;
    planview_explain(a, c, sql, (int)strlen(sql), PM_ESTIMATED, "Rewrite from Lume Intelligence (estimated plan, not run)");
    if (!G.job) { G.cmp_after = 0; return false; }
    return true;
}
