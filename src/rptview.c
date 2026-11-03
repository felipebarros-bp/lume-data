/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Report Studio and its assistant (see rptview.h).
 *
 *   Data path: card spec -> sem_compile (catalog ids only) -> sqlguard_check -> sqlguard_wrap
 *   (read-only transaction, rollback) -> exec_start_list on the dataset's connection, one query at a
 *   time, with a timeout that cancels for real. Results are cached by (connection, SQL): switching a
 *   line to bars, undo / redo and theme changes never re-query (LI-044.2 / 059.1 / 067.2).
 *
 *   AI path: question -> local ambiguity check (catalog) -> context preview with consent -> model ->
 *   ag_parse_answer (closed schema) -> dry run of the proposal on a copy -> plan with the tools and
 *   queries it implies -> the user approves -> one undoable change -> queries -> done. Audit lines
 *   carry decisions and sizes, never prompts, SQL or values (LI-082).
 */
#include "rptview.h"
#include "telemetry.h"
#include "rpt.h"
#include "sem.h"
#include "agent.h"
#include "sqlguard.h"
#include "sqlfmt.h"
#include "schidx.h"
#include "plan.h"
#include "planview.h"
#include "settings.h"
#include "tbl.h"
#include "tdes.h"
#include <ctype.h>
#include <fcntl.h>
#include "sqllex.h"
#include "pngw.h"
#include "rtheme.h"
#include "jw.h"
#include "json.h"
#include "image.h"
#include "toolbar.h"
#include "editor.h"
#include "pdfw.h"
#include "xlsxw.h"
#include "ai.h"
#include "secstore.h"
#include "chatgpt.h"
#include <pthread.h>
#include "conn.h"
#include "caps.h"
#include "db.h"
#include "exec.h"
#include "rs.h"
#include "draw.h"
#include "font.h"
#include "ui.h"
#include "panels.h"
#include "commands.h"
#include "output.h"
#include "outlog.h"
#include "results.h"
#include "dock.h"
#include "log.h"
#include "theme.h"
#include <dirent.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include <unistd.h>

#define MAX_DOCS 8
#define MAX_DATA 96
#define ROW_PT 34.0f
#define GAP_PT 10.0f
#define QUERY_TIMEOUT_S 30   /* default; Settings > Reports changes it (App.report_timeout_s) */
#define MAX_ROWS 10000

enum { RD_QUEUED, RD_RUNNING, RD_OK, RD_ERROR, RD_TIMEOUT, RD_CANCELLED };
static const char *const RD_NAME[] = { "queued", "running", "ok", "error", "timeout", "cancelled" };

typedef struct RvData {
    bool used; uint64_t key; char conn[16]; char *sql;
    int status; ResultSet rs; double fetched_at, ms; char err[300]; double last_used;
} RvData;

typedef struct TblState { char id[24]; float scroll; int sort_col; bool desc; } TblState;

struct RvDoc {
    bool used; int tab_id;
    Report r; RptHist h;
    char path[1100];
    bool dirty, read_mode, preview;
    double changed_at, autosaved_at;
    bool user_path;   /* path chosen by the user (Save As / opened file); otherwise the default in the prefs folder */
    float zoom, hscroll;   /* page zoom (0 = 100 %) and horizontal scroll when the page is wider than the view */
    ScrollBar sb; float content_h; Rect viewport;   /* the page's overlay scrollbar, on the visible area */
    /* the resolved theme (rtheme.h): from the report's reference, its snapshot, or a built-in one */
    RTheme rt; char rt_key[1200]; double rt_checked; long long rt_mtime; char rt_dir[1024], rt_warn[240]; float head_h;
    int page; char sel[24];
    RvData data[MAX_DATA];
    int drag; char drag_id[24]; float mx0, my0; int ox, oy, ow, oh, gx, gy, gw, gh;
    float scroll;
    char msg[300];
    char prov[24];
    TblState tbl[16];
    TextField f_title, f_text, f_sub, f_rtitle, f_page, f_fval;
    char bound[24]; int bound_rev;
    int queries;
    Rect canvas; float colw, rowh, gap;
};

enum { AS_IDLE, AS_LOCAL_CLARIFY, AS_THINKING, AS_CLARIFY, AS_PLAN, AS_QUERYING, AS_DONE, AS_ERROR, AS_CANCELLED, AS_CANNOT, AS_NEED_SCHEMA, AS_CHECKING, AS_SCOPE, AS_INDEXING, AS_N };
static const char *const AS_NAME[] = { "idle", "clarify-local", "thinking", "clarify", "awaiting-approval", "querying", "done", "error", "cancelled", "cannot-answer", "needs-schema", "checking-sql", "confirm-scope", "indexing-structure" };
enum { ST_PENDING, ST_DONE, ST_OK, ST_REFUSED, ST_CACHED, ST_RUNNING, ST_SKIPPED };
/* a step as the user reads it: a verb, not the tool's name */
static const char *step_verb(const char *tool) {
    static const struct { const char *t, *v; } m[] = {
        { "add_filter", "Add a filter" }, { "check_ai_sql", "Check the written SQL" }, { "create_report_draft", "Create the report draft" },
        { "design_table", "Design the table" }, { "execute_readonly_query", "Run a read-only query" }, { "propose_report_patch", "Update the cards" },
        { "propose_sql_query", "Write the card's query" }, { "set_logo", "Set the logo" }, { "set_theme", "Change the theme" },
        { "validate", "Check the change" }, { "validate_query_policy", "Check the query is safe to run" },
    };
    for (size_t i = 0; i < sizeof m / sizeof m[0]; i++) if (!strcmp(m[i].t, tool)) return m[i].v;
    return tool;
}
/* a step's status: before approval nothing is applied yet, so prepared steps read "ready" */
static const char *step_status(int st, bool before) {
    if (before) return st == 3 ? "refused" : st == 4 ? "uses data already loaded" : st == 6 ? "skipped" : st == 0 ? "planned" : "ready";
    return st == 0 ? "pending" : st == 1 || st == 2 ? "done" : st == 3 ? "refused" : st == 4 ? "data already loaded" : st == 5 ? "running" : "skipped";
}
/* what the user reads (AS_NAME stays the harness vocabulary) */
static const char *const AS_LABEL[] = { "Idle", "A question for you", "Thinking", "A question for you", "Waiting for your approval", "Running the queries", "Done", "Something went wrong", "Cancelled", "Couldn't answer", "Needs the database structure", "Checking the SQL", "Choose what to share", "Indexing the structure" };
static const char *const ST_NAME[] = { "pending", "done", "ok", "refused", "cached", "running", "skipped" };
typedef struct Step { char tool[32]; char detail[400]; int st; char sql[1200]; } Step;

static struct {
    App *a;
    SemCatalog cat; bool cat_loaded;
    RvDoc *docs[MAX_DOCS];
    /* query runner (one job at a time) */
    ExecJob *job; RvDoc *job_doc; int job_slot; char job_conn[16]; double job_t0; int job_timeout; char *stmts[6]; int nst, res; bool fin; int out_exec;
    int timeout_s;
    PopupMenu menu; int menu_kind; RvDoc *menu_doc; int menu_param;
    char menu_lab[POPUP_MAX][96];
    int nmore, more_id[16]; bool more_en[16]; char more_lab[16][64]; float more_x, more_y;   /* report bar actions that did not fit */
    Font *f_title, *f_kpi; float fonts_scale;
    Font *zf_ui, *zf_small, *zf_title, *zf_kpi; float zscale;   /* fonts at the page zoom */
    /* assistant */
    AiConfig cfg; AiJob *ai; int ast; TextField q; AgContext cx; bool show_payload; float pscroll;
    AgAnswer ans; char amsg[500]; char sent_q[700]; RvDoc *adoc; char acard[24]; bool allow_global;
    Step steps[24]; int nsteps;
    char opts[6][48]; int nopt; char term[48]; char choice[200];
    double at0; char applied[8][24]; int napplied;
    char *payload;
    /* setup (LI-001 / 003 / 004): provider, what it needs, a connection test, then enable */
    bool setup, ctx_open, replacing_key; int sprov; TextField s_model, s_endpoint, s_key, s_fast; AiJob *testj; int test_state; char test_msg[500]; double test_t0;
    int oa_mode;                 /* OpenAI: 0 = the user's ChatGPT plan, 1 = an API key */
    bool signing, welcome;       /* a ChatGPT sign-in in the browser; the plan's first-use note */
    char models[40][96]; int nmodels; bool models_open;
    /* the conversation (chat layout): earlier turns; the current turn is the live state below them */
    struct { char role; char *text; } chat[48]; int nchat, turn_start; RvDoc *chat_doc; float chat_scroll; bool chat_stick, steps_open, steps_closed; unsigned chat_sig; float chat_total, chat_max;
    /* text selection in the conversation: message index (nchat = the live message) and byte offset */
    int sel_am, sel_ao, sel_bm, sel_bo; bool sel_on, sel_drag;
    ScrollBar chat_sb;
    /* the selected card's query in the properties panel: an embedded SQL editor */
    Editor *ced; char ced_doc[40], ced_vid[24], ced_orig[RPT_AI_SQL_MAX + 1]; bool ced_focus, ced_apply, ced_ro; Rect ced_rect;
    /* sent questions (session only, never written): Up / Down recall them, like a shell */
    char *qhist[50]; int nqhist, qpos; char qdraft[700];
    /* the AI's own SQL (from the shared schema): connection, structure text, local checks before the plan */
    char schema_conn[16]; char *schema_text; bool schema_wait, resend;
    /* large databases: a local index of the structure, read in the background (src/schidx.c) */
    SchIdx sx; int sx_state; char sx_conn[16]; ExecJob *sx_job; char *sx_stmts[3]; int sx_kinds[3], sx_nst; double sx_t0; char sx_err[240];
    bool sx_wait, sx_search; int sx_rounds; char *sx_extra;   /* sx_wait: a send waits for the index; sx_extra: search results for the next send */
    char *catv_key[160], *catv_val[160]; int ncatv;
    /* the structure scope of a connection (ai-scope.txt): schemas indexed, tables always / never sent */
    char scope_conn[16], scope_schemas[256], scope_pin[256], scope_block[256], scope_consent[64]; bool scope_loaded, scope_has_consent;
    TextField f_sch, f_pin, f_blk; char est_q[300]; int est_tables, est_bytes;
    /* the assistant in a query tab (roadmap items 8-14): the tab it answers about and the edit it proposes */
    bool qmode, req_qmode; int chat_tab, qtab; char qconn[16], qtitle[48]; uint32_t qrev;
    int qsel_s, qsel_e, qstmt_s, qstmt_e, qkind; char qtarget[16]; char *qedit_sql; bool qwrites; char qwrite_why[200];
    TdTable qtable; bool qtable_set;
    char *qcreate; char qcreate_title[120];
    /* validate (BL-056): the checks Lume runs for the model's hypothesis, one statement list at a time */
    int vstate;                 /* 0 none, 1 running, 2 waiting for the user's approval (results), 3 done */
    char *vorig, *vcand; char vidx[300]; bool vwant_results, vapproved; int vstep, vround;
    ExecJob *vjob; char *vst[8]; int vnst, vres; bool vfin; int vkind;
    char vfb[6000]; size_t vfbw; char vline[400];   /* the <validation> text for the model; one line for the chat */
    long long vrows[2]; unsigned long long vsum[2]; double vms[2]; bool vtrunc[2], vran[2]; char vcost[3][48];   /* create_report from a query tab: built in a new report */
    int tok_in, tok_out, nreq;
    int last_tin, last_tout; bool tok_pending;
    bool design_review; int rev_est_rev, rev_est_tokens; RvDoc *rev_est_doc;   /* the design review: the page with placeholder data goes along */   /* the last answer's tokens, shown under it in the chat */          /* this conversation's use, as the provider reported it (roadmap item 23) */
    bool quick;                         /* the request is a quick one (Explain, Fix, Cmd+K): the fast model when set */
    char glossary[8192]; double gloss_mtime; char gloss_path[1200];   /* the workspace glossary (item 24) */   /* design_table: the draft that opens in the Table Designer */  /* category values already read (table.column), per session */   /* schema_conn: where the AI's SQL runs (BL-051) */
    char schema_main[16];                                                 /* the database the user picked */
    char gr_conn[16]; int gr_state; char gr_note[160];                    /* write grants of the login that runs the AI's SQL */
    char *last_answer; int retries; bool probed;
    char *pending_theme;   /* set_theme of the plan being reviewed */
    char attach[1024], attach_name[96];   /* an image attached in the chat (dropped or chosen): stays local, only its name and main colours go to the model */
    bool pending_logo;     /* the plan makes the attached image the theme's logo */
    Rect chat_rc; SDL_WindowID chat_win;  /* where the conversation was drawn: a dropped image there is attached */
    struct { char vid[24], title[80]; int kind; char conn[16]; char *probe; } probes[12]; int nprobes;
    pthread_t mth; volatile int mstate; char mmsg[300]; AiConfig mcfg; char *mkey;   /* model list (0 idle, 1 loading, 2 done) */
    /* stats */
    double render_ms; int exports;
} G;

static double now_s(void) { return (double)SDL_GetTicks() / 1000.0; }
static void pref_dir(App *a, char *o, int n) { snprintf(o, (size_t)n, "%s", a->conns && a->conns->dir[0] ? a->conns->dir : "."); size_t L = strlen(o); if (L > 1 && o[L - 1] == '/') o[L - 1] = 0; }
static Connection *conn_of(App *a, const char *id) { for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->id, id)) return a->conns->items[i]; return NULL; }
static uint64_t fnv(const char *s, uint64_t h) { if (!h) h = 1469598103934665603ULL; for (; *s; s++) { h ^= (unsigned char)*s; h *= 1099511628211ULL; } return h; }
static int doc_index(RvDoc *d) { for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] == d) return i; return -1; }
static void set_msg(RvDoc *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void set_msg(RvDoc *d, const char *fmt, ...) { va_list ap; va_start(ap, fmt); vsnprintf(d->msg, sizeof d->msg, fmt, ap); va_end(ap); }
static bool pt_locale(const RvDoc *d) { return !strncmp(d->r.locale, "pt", 2); }
#define L(d, pt, en) (pt_locale(d) ? (pt) : (en))

/* ---- audit (LI-082): decisions and sizes only ---- */
static void audit(App *a, const char *tool, const RvDoc *d, const char *decision, size_t bytes, const char *extra) {
    char dir[1100]; pref_dir(a, dir, sizeof dir); strncat(dir, "/audit", sizeof dir - strlen(dir) - 1); mkdir(dir, 0700);
    char p[1300]; snprintf(p, sizeof p, "%s/report.log", dir);
    FILE *f = fopen(p, "a"); if (!f) return;
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    const char *u = getenv("USER");
    fprintf(f, "%s user=%s tool=%s report=%s decision=%s bytes=%zu%s%s\n", ts, u ? u : "?", tool, d ? d->r.id : "-", decision, bytes, extra && *extra ? " " : "", extra ? extra : "");
    fclose(f);
}

/* ---- catalog ---- */
static void load_catalog(App *a) {
    char dir[1100], p[1200]; pref_dir(a, dir, sizeof dir); snprintf(p, sizeof p, "%s/semantic.json", dir);
    if (!sem_load(&G.cat, p)) LOG_W("report: semantic catalog not loaded: %s", G.cat.err);
    else LOG_I("report: semantic catalog: %d dataset(s), %d metric(s)", G.cat.nds, G.cat.nm);
    G.cat_loaded = true;
}

/* ---- documents ---- */
static void reports_dir(App *a, char *o, int n) { char dir[1100]; pref_dir(a, dir, sizeof dir); snprintf(o, (size_t)n, "%s/reports", dir); mkdir(o, 0700); }
static void doc_paths(App *a, RvDoc *d) { if (d->path[0]) return; char rd[1100]; reports_dir(a, rd, sizeof rd); snprintf(d->path, sizeof d->path, "%s/%s.lrpt", rd, d->r.id); }
/* autosave and versions live in the prefs folder, keyed by the report id, wherever the file is saved */
static void aux_base(App *a, const char *id, char *o, int n) { char rd[1100]; reports_dir(a, rd, sizeof rd); snprintf(o, (size_t)n, "%s/%s.lrpt", rd, id); }
static void autosave_path(App *a, const RvDoc *d, char *o, int n) { char b[1200]; aux_base(a, d->r.id, b, sizeof b); snprintf(o, (size_t)n, "%s.autosave", b); }
/* recent reports outside the default folder: <prefs>/reports/recent.txt, newest first, at most 20 */
static void recent_add(App *a, const char *path) {
    char rd[1100], rp[1200]; reports_dir(a, rd, sizeof rd); snprintf(rp, sizeof rp, "%s/recent.txt", rd);
    char keep[20][1024]; int n = 0; FILE *f = fopen(rp, "r");
    if (f) { char line[1100]; while (fgets(line, sizeof line, f) && n < 19) { line[strcspn(line, "\r\n")] = 0; if (line[0] && strcmp(line, path)) snprintf(keep[n++], sizeof keep[0], "%s", line); } fclose(f); }
    f = fopen(rp, "w"); if (!f) return;
    fprintf(f, "%s\n", path); for (int i = 0; i < n; i++) fprintf(f, "%s\n", keep[i]);
    fclose(f);
}
static void data_free(RvData *x) { free(x->sql); rs_free(&x->rs); memset(x, 0, sizeof *x); }
static RvDoc *new_doc(App *a) {
    for (int i = 0; i < MAX_DOCS; i++) if (!G.docs[i]) {
        RvDoc *d = calloc(1, sizeof *d); if (!d) return NULL;
        d->used = true; G.docs[i] = d;
        for (int k = 0; k < MAX_DATA; k++) rs_init(&d->data[k].rs);
        return d;
    }
    app_status(a, "Too many open reports (%d): close one first", MAX_DOCS);
    return NULL;
}
static void free_doc(RvDoc *d) {
    int i = doc_index(d); if (i < 0) return;
    if (G.job && G.job_doc == d) { exec_cancel(G.job); exec_free(G.job); G.job = NULL; sqlguard_free(G.stmts, G.nst); G.nst = 0; }
    if (G.adoc == d) { G.adoc = NULL; if (G.ai) { ai_free(G.ai); G.ai = NULL; } G.ast = AS_IDLE; }
    for (int k = 0; k < MAX_DATA; k++) if (d->data[k].used) data_free(&d->data[k]);
    rpt_hist_free(&d->h); rpt_free(&d->r);
    free(d); G.docs[i] = NULL;
}
static void mark_changed(RvDoc *d) { d->dirty = true; d->changed_at = now_s(); }
static void before_change(RvDoc *d) { rpt_hist_push(&d->h, &d->r); }
static void after_change(RvDoc *d) { d->r.revision++; mark_changed(d); }
static RvDoc *active_doc(App *a) { return a->panels ? panels_active_report(a->panels, a) : NULL; }
static RvDoc *any_doc(App *a) { RvDoc *d = active_doc(a); if (d) return d; for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i]) return G.docs[i]; return NULL; }
static RvDoc *open_doc_tab(App *a, RvDoc *d) {
    doc_paths(a, d);
    d->tab_id = panels_open_report_tab(a->panels, a, d->r.title, d);
    if (d->tab_id < 0) { free_doc(d); return NULL; }
    return d;
}
static bool ws_theme_path(char *out, size_t n);
static bool file_exists_(const char *p);
static RvDoc *create_report(App *a, const char *title) {
    telemetry_count(TM_REPORT);
    if (!G.cat_loaded) load_catalog(a);
    RvDoc *d = new_doc(a); if (!d) return NULL;
    rpt_init(&d->r, title && *title ? title : "Untitled report");
    if (a->report_locale[0]) snprintf(d->r.locale, sizeof d->r.locale, "%s", a->report_locale);   /* Settings > Reports */
    snprintf(d->r.pages[0].name, sizeof d->r.pages[0].name, "%s", "Visão geral");
    { char wp[1200]; if (ws_theme_path(wp, sizeof wp) && file_exists_(wp)) snprintf(d->r.theme_ref, sizeof d->r.theme_ref, "workspace"); }   /* a new report starts with the Brand Kit */
    d->dirty = true;
    LOG_I("report: new report %s", d->r.id);
    return open_doc_tab(a, d);
}
static bool save_doc(App *a, RvDoc *d, const char *path) {
    if (d->read_mode) { set_msg(d, "%s", L(d, "Modo leitura: o relatório não é alterado", "Read mode: the report is not changed")); return false; }
    doc_paths(a, d);
    if (path && *path && path != d->path) snprintf(d->path, sizeof d->path, "%s", path);   /* never snprintf a buffer into itself: glibc empties it */
    char why[300];
    if (!rpt_save_atomic(&d->r, d->path, why, sizeof why)) { set_msg(d, "%s", why); return false; }
    /* version history next to the file (LI-050 / 074 lite): one copy per saved revision */
    char vb[1200]; aux_base(a, d->r.id, vb, sizeof vb);
    char vd[1300]; snprintf(vd, sizeof vd, "%s.versions", vb); mkdir(vd, 0700);
    char vp[1400]; snprintf(vp, sizeof vp, "%s/rev-%04d.lrpt", vd, d->r.revision);
    rpt_save_atomic(&d->r, vp, why, sizeof why);
    char ap[1300]; autosave_path(a, d, ap, sizeof ap); unlink(ap);
    if (d->user_path) recent_add(a, d->path);
    d->dirty = false;
    set_msg(d, L(d, "Salvo (revisão %d) em %s", "Saved (revision %d) to %s"), d->r.revision, d->path);
    LOG_I("report: saved %s revision %d", d->r.id, d->r.revision);
    return true;
}
static void autosave(App *a, RvDoc *d) {
    if (!d->dirty || d->read_mode) return;
    doc_paths(a, d);
    char ap[1300], why[300]; autosave_path(a, d, ap, sizeof ap);
    if (rpt_save_atomic(&d->r, ap, why, sizeof why)) d->autosaved_at = now_s();
}
static RvDoc *open_report(App *a, const char *path, bool read) {
    telemetry_count(TM_REPORT);
    if (!G.cat_loaded) load_catalog(a);
    /* already open in the same mode: show it; read mode beside an editable copy is a separate tab */
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] && !strcmp(G.docs[i]->path, path) && G.docs[i]->read_mode == read) { panels_activate_tab(a->panels, a, G.docs[i]->tab_id); return G.docs[i]; }
    RvDoc *d = new_doc(a); if (!d) return NULL;
    char why[300]; bool mig = false;
    /* crash recovery (LI-050.2): a newer autosave than the file is offered first */
    char ap[1300]; snprintf(ap, sizeof ap, "%s.autosave", path);
    struct stat sf, sa; bool use_auto = !read && stat(ap, &sa) == 0 && (stat(path, &sf) != 0 || sa.st_mtime >= sf.st_mtime);
    if (!rpt_load(&d->r, use_auto ? ap : path, &mig, why, sizeof why)) {
        if (use_auto && rpt_load(&d->r, path, &mig, why, sizeof why)) use_auto = false;
        else { app_status(a, "Open report: %s", why); LOG_W("report: open refused: %s", why); free_doc(d); return NULL; }
    }
    if (!use_auto && !read) {   /* a file saved elsewhere: its autosave is in the prefs folder, by id */
        char b[1200]; aux_base(a, d->r.id, b, sizeof b); snprintf(ap, sizeof ap, "%s.autosave", b);
        if (strcmp(b, path) && stat(ap, &sa) == 0 && stat(path, &sf) == 0 && sa.st_mtime >= sf.st_mtime) {
            Report rr; bool m2 = false; char w2[300];
            if (rpt_load(&rr, ap, &m2, w2, sizeof w2)) { rpt_free(&d->r); d->r = rr; use_auto = true; }
        }
    }
    snprintf(d->path, sizeof d->path, "%s", path);
    { char b[1200]; aux_base(a, d->r.id, b, sizeof b); d->user_path = strcmp(b, path) != 0; if (d->user_path) recent_add(a, path); }
    d->read_mode = read; d->preview = read; d->dirty = use_auto || mig;
    if (use_auto) set_msg(d, "%s", L(d, "Recuperado do autosave (alterações não salvas da última sessão)", "Recovered from the autosave (unsaved changes of the last session)"));
    else if (mig) set_msg(d, "%s", L(d, "Formato antigo migrado em memória: salve para gravar no formato atual", "Older format migrated in memory: save to write the current format"));
    LOG_I("report: opened %s%s%s", d->r.id, read ? " (read mode)" : "", use_auto ? " from autosave" : "");
    return open_doc_tab(a, d);
}

/* ---- data (LI-029 / 081 / 092) ---- */
static const SemDataset *dataset_of(const RptVisual *v) {
    const SemMetric *m = sem_metric_c(&G.cat, v->metric); if (!m) return NULL;
    const SemMetricDef *d = v->metric_version ? sem_version(m, v->metric_version) : sem_cur(m);
    return d ? sem_dataset_c(&G.cat, d->dataset) : NULL;
}
static int dialect_of_conn(App *a, const char *conn) { Connection *c = conn_of(a, conn); return c ? dialect_for_driver(c->driver) : DIALECT_GENERIC; }
static const RptFilter *page_filter(const RvDoc *d) {
    for (int i = 0; i < d->r.nf; i++) if (!d->r.filters[i].page[0] || !strcmp(d->r.filters[i].page, d->r.pages[d->page].id)) return &d->r.filters[i];
    return NULL;
}
/* the query of a card (base: the comparison window of a KPI); false with why (the card is blocked) */
static bool visual_sql(App *a, RvDoc *d, const RptVisual *v, bool base, char *sql, int n, char *conn, char *why, int wn) {
    if (v->sql && (v->kind == RK_TABLE || v->kind == RK_KPI || v->kind == RK_LINE || v->kind == RK_BAR)) {
        if (base) { snprintf(why, (size_t)wn, "no comparison for a card with its own SQL"); return false; }
        /* report filters and the period, as safe literals */
        if (!rpt_bind_params(&d->r, v->sql, sql, (size_t)n, NULL, 0, why, wn)) return false;
        snprintf(conn, 16, "%s", v->conn); return true;
    }
    if (!rpt_visual_check(&d->r, v, &G.cat, why, wn)) return false;
    const SemDataset *ds = dataset_of(v);
    if (!ds) { snprintf(why, (size_t)wn, "dataset not found"); return false; }
    snprintf(conn, 16, "%s", ds->conn);
    SemPeriod cur, b; char label[80];
    if (!rpt_period(&d->r, base ? v->compare : RC_NONE, &cur, &b, label, sizeof label) && base) { snprintf(why, (size_t)wn, "%s", L(d, "sem janela de comparação para este período", "no comparison window for this period")); return false; }
    SemQuery q = { v->metric, v->metric_version, v->dim, v->grain, base ? b : cur, NULL, NULL, 0 };
    const RptFilter *f = page_filter(d);
    if (f && f->field[0] && f->value[0]) {
        if (!sem_is_dimension(ds, f->field)) { snprintf(why, (size_t)wn, L(d, "filtro '%s' incompatível com este card (não é dimensão de %s): não aplicado, card bloqueado", "filter '%s' does not apply to this card (not a dimension of %s): card blocked"), f->field, ds->id); return false; }
        q.filter_col = f->field; q.filter_val = f->value;
    }
    q.limit = v->kind == RK_BAR && v->dim[0] ? (v->top > 0 ? v->top : 30) : v->kind == RK_TABLE ? (v->top > 0 ? v->top : 0) : 0;
    return sem_compile(&G.cat, &q, dialect_of_conn(a, ds->conn), sql, n, why, wn);
}
static RvData *find_data(RvDoc *d, const char *conn, const char *sql) {
    uint64_t k = fnv(sql, fnv(conn, 0));
    for (int i = 0; i < MAX_DATA; i++) if (d->data[i].used && d->data[i].key == k && !strcmp(d->data[i].conn, conn) && !strcmp(d->data[i].sql, sql)) return &d->data[i];
    return NULL;
}
static RvData *request_data(RvDoc *d, const char *conn, const char *sql) {
    RvData *x = find_data(d, conn, sql);
    if (x) { x->last_used = now_s(); return x; }
    int slot = -1; double oldest = 1e18;
    for (int i = 0; i < MAX_DATA; i++) { if (!d->data[i].used) { slot = i; break; } if (d->data[i].status != RD_RUNNING && d->data[i].last_used < oldest) { oldest = d->data[i].last_used; slot = i; } }
    if (slot < 0) return NULL;
    x = &d->data[slot]; if (x->used) data_free(x);
    x->used = true; x->key = fnv(sql, fnv(conn, 0)); snprintf(x->conn, sizeof x->conn, "%s", conn); x->sql = strdup(sql); x->status = RD_QUEUED; x->last_used = now_s(); rs_init(&x->rs);
    return x;
}
static void invalidate_data(RvDoc *d) { for (int i = 0; i < MAX_DATA; i++) if (d->data[i].used && d->data[i].status != RD_RUNNING) data_free(&d->data[i]); }
static RvData *visual_data(App *a, RvDoc *d, const RptVisual *v, bool base, char *why, int wn) {
    char sql[4096], conn[16];
    if (!visual_sql(a, d, v, base, sql, sizeof sql, conn, why, wn)) return NULL;
    RvData *x = request_data(d, conn, sql);
    if (!x) snprintf(why, (size_t)wn, "data cache full");
    return x;
}
static void finish_job(App *a) {
    RvDoc *d = G.job_doc; RvData *x = d && doc_index(d) >= 0 ? &d->data[G.job_slot] : NULL;
    Connection *c = conn_of(a, G.job_conn);
    int n = G.nst; ResultSet rs[6]; char err[300] = "";
    bool cancelled = exec_overall(G.job) == 5;
    for (int i = 0; i < n && i < 6; i++) { rs_init(&rs[i]); exec_take_result(G.job, i, &rs[i]); if (!err[0] && rs[i].error[0] && i != n - 1) snprintf(err, sizeof err, "%s", rs[i].error); }
    double ms = (now_s() - G.job_t0) * 1000.0;
    if (x && x->used) {
        bool timed_out = x->status == RD_TIMEOUT;
        if (timed_out) {}
        else if (cancelled) x->status = RD_CANCELLED;
        else if (err[0]) { x->status = RD_ERROR; snprintf(x->err, sizeof x->err, "%s", err); }
        else { rs_move(&x->rs, &rs[G.res]); rs_release(&x->rs); x->status = RD_OK; x->fetched_at = (double)time(NULL); }
        x->ms = ms;
    }
    for (int i = 0; i < n && i < 6; i++) rs_free(&rs[i]);
    if (c) output_obj_finish(a->output, a, c, G.out_exec, err[0] ? ES_FAILED : cancelled ? ES_CANCELLED : ES_COMPLETED, ms);
    LOG_I("report: query %s in %.0f ms%s", err[0] ? "failed" : cancelled ? "cancelled" : "done", ms, x && x->status == RD_TIMEOUT ? " (timeout)" : "");
    exec_free(G.job); G.job = NULL; sqlguard_free(G.stmts, G.nst); G.nst = 0; G.job_doc = NULL;
}
static void start_next(App *a) {
    for (int di = 0; di < MAX_DOCS; di++) {
        RvDoc *d = G.docs[di]; if (!d) continue;
        for (int i = 0; i < MAX_DATA; i++) {
            RvData *x = &d->data[i];
            if (!x->used || x->status != RD_QUEUED) continue;
            Connection *c = conn_of(a, x->conn);
            if (!c) { x->status = RD_ERROR; snprintf(x->err, sizeof x->err, "connection '%s' of the dataset is not configured here", x->conn); continue; }
            if (!c->db) {
                if (c->state == CONN_CONNECTING || c->state == CONN_RECONNECTING) continue;   /* wait */
                if (c->state == CONN_FAILED) { x->status = RD_ERROR; snprintf(x->err, sizeof x->err, "%s: %s", c->name, c->error[0] ? c->error : "connection failed"); continue; }
                conn_connect_async(c, false); continue;
            }
            int dial = dialect_for_driver(c->driver);
            char why[300];
            /* the compiled query goes through the policy too (defense in depth); a user's SQL table always does */
            if (!sqlguard_check(x->sql, dial, why, sizeof why)) { x->status = RD_ERROR; snprintf(x->err, sizeof x->err, "refused by the read-only policy: %s", why); audit(a, "validate_query_policy", d, "refused", 0, ""); continue; }
            char note[300];
            int lim = G.timeout_s > 0 ? G.timeout_s : a->report_timeout_s > 0 ? a->report_timeout_s : QUERY_TIMEOUT_S;   /* harness override, else Settings */
            G.nst = sqlguard_wrap(dial, x->sql, lim, G.stmts, 6, &G.res, &G.fin, note, sizeof note);
            G.job = exec_start_list(c->db, (const char *const *)G.stmts, G.nst, MAX_ROWS, true, G.fin);
            if (!G.job) { sqlguard_free(G.stmts, G.nst); G.nst = 0; return; }   /* session busy: try again next frame */
            G.job_doc = d; G.job_slot = i; snprintf(G.job_conn, sizeof G.job_conn, "%s", x->conn); G.job_t0 = now_s(); G.job_timeout = lim;   /* the limit in force when it started */
            x->status = RD_RUNNING; d->queries++;
            G.out_exec = output_obj_begin(a->output, a, c, "Report query (read-only)", G.nst);
            output_obj_event(a->output, a, c, G.out_exec, 0, OL_INFO, note, NULL);
            output_obj_event(a->output, a, c, G.out_exec, 0, OL_INFO, "Report query", x->sql);
            audit(a, "execute_readonly_query", d, "run", strlen(x->sql), "");
            return;
        }
    }
}
static void pump_queries(App *a) {
    if (G.job) {
        if (!exec_running(G.job)) finish_job(a);
        else {
            if (now_s() - G.job_t0 > G.job_timeout && G.job_doc && !exec_cancelling(G.job)) {
                RvData *x = &G.job_doc->data[G.job_slot];
                x->status = RD_TIMEOUT; snprintf(x->err, sizeof x->err, "%d s", G.job_timeout);
                exec_cancel(G.job);
                LOG_W("report: query timed out after %d s: cancel requested", G.job_timeout);
            }
            app_wake_in(a, 80);
            return;
        }
    }
    start_next(a);
    if (G.job) { app_wake_in(a, 80); return; }
    /* still queued (waiting for a connection or a busy session): keep the loop awake */
    for (int di = 0; di < MAX_DOCS; di++) if (G.docs[di]) for (int i = 0; i < MAX_DATA; i++) if (G.docs[di]->data[i].used && G.docs[di]->data[i].status == RD_QUEUED) { app_wake_in(a, 100); return; }
}
static void on_close_conn(Connection *c) {
    if (G.job && !strcmp(G.job_conn, c->id)) { exec_cancel(G.job); exec_free(G.job); G.job = NULL; sqlguard_free(G.stmts, G.nst); G.nst = 0; if (G.job_doc) { RvData *x = &G.job_doc->data[G.job_slot]; x->status = RD_ERROR; snprintf(x->err, sizeof x->err, "the connection closed during the query"); } G.job_doc = NULL; }
}
/* a number from a cell; false for NULL / non-numeric */
static bool cell_num(const ResultSet *rs, int row, int col, double *v) {
    const char *s = rs_cell(rs, row, col); if (!s) return false;
    char *e; *v = strtod(s, &e);
    return e != s;
}

/* ---- themes (LI-045): contrast checked by hand against WCAG AA for text on card ---- */
typedef struct Theme { uint32_t bg, card, border, text, muted, grid, accent, band, band_text, pos, neg, sel; uint32_t series[6]; } Theme;
static void theme_resolve(RvDoc *d);
/* the drawing palette of the resolved theme (the page may pick a built-in one) */
static const Theme *theme_of(const RvDoc *dc) {
    RvDoc *d = (RvDoc *)dc;
    const char *pt = d->page >= 0 && d->page < d->r.np ? d->r.pages[d->page].theme : "";
    static Theme out;
    if (pt[0]) {   /* a page-level built-in theme */
        RTheme b; rtheme_builtin(&b, pt); d->rt = b;
    } else theme_resolve(d);
    const RTheme *t = &d->rt;
    out.bg = t->bg; out.card = t->surface; out.border = t->border; out.text = t->text; out.muted = t->muted; out.grid = t->grid;
    out.accent = t->accent; out.band = t->header_bg; out.band_text = t->header_text; out.pos = t->pos; out.neg = t->neg; out.sel = t->sel;
    for (int i = 0; i < 6; i++) out.series[i] = t->series[i % (t->nseries > 0 ? t->nseries : 1)];
    return &out;
}
static Color C(uint32_t h) { return color_hex(h); }
/* cards that query data (text, images and shapes do not) */
static bool has_data(const RptVisual *v) { return v->kind == RK_KPI || v->kind == RK_LINE || v->kind == RK_BAR || v->kind == RK_TABLE; }
/* ---- theme resolution: the report's reference -> its file (reloaded when it changes) -> the snapshot -> built-in ---- */
static void doc_dir(const RvDoc *d, char *o, size_t n) {
    snprintf(o, n, "%s", d->path); char *sl = strrchr(o, '/'); if (sl) *sl = 0; else snprintf(o, n, ".");
}
static bool theme_file_path(const RvDoc *d, const char *ref, char *out, size_t n) {
    if (!strcmp(ref, "workspace")) {
        const char *wd = G.a && G.a->toolbar ? toolbar_workspace_dir(G.a->toolbar) : "";
        if (!wd || !wd[0]) return false;
        snprintf(out, n, "%stheme.lrtheme", wd); return true;
    }
    if (!strncmp(ref, "file:", 5)) {
        const char *p = ref + 5;
        if (p[0] == '/') snprintf(out, n, "%s", p);
        else { char dd[1024]; doc_dir(d, dd, sizeof dd); snprintf(out, n, "%s/%s", dd, p); }
        return true;
    }
    return false;
}
static void theme_resolve(RvDoc *d) {
    const char *ref = d->r.theme_ref[0] ? d->r.theme_ref : "";
    char path[1400] = ""; bool is_file = theme_file_path(d, ref, path, sizeof path);
    double now = now_s();
    char key[1200]; snprintf(key, sizeof key, "%s|%s|%zu", ref, d->r.theme, d->r.theme_overrides ? strlen(d->r.theme_overrides) + (size_t)(unsigned char)d->r.theme_overrides[0] * 131u : 0);
    if (!strcmp(key, d->rt_key) && (!is_file || now - d->rt_checked < 1.0)) return;   /* cached; a file is looked at once a second */
    struct stat st; long long mt = is_file && stat(path, &st) == 0 ? (long long)st.st_mtime : -1;
    if (!strcmp(key, d->rt_key) && mt == d->rt_mtime) { d->rt_checked = now; return; }
    RTheme t; char why[300] = ""; bool ok = false;
    d->rt_warn[0] = 0; d->rt_dir[0] = 0;
    if (is_file) {
        if (mt >= 0 && rtheme_load_file(&t, path, why, sizeof why)) {
            ok = true;
            snprintf(d->rt_dir, sizeof d->rt_dir, "%s", path); char *sl = strrchr(d->rt_dir, '/'); if (sl) *sl = 0;
            char *js = rtheme_to_json(&t);   /* the snapshot follows the file, so the report renders the same elsewhere */
            if (!d->r.theme_snapshot || strcmp(d->r.theme_snapshot, js)) { free(d->r.theme_snapshot); d->r.theme_snapshot = js; } else free(js);
        } else if (d->r.theme_snapshot && rtheme_parse(&t, d->r.theme_snapshot, strlen(d->r.theme_snapshot), why, sizeof why)) {
            ok = true;
            snprintf(d->rt_warn, sizeof d->rt_warn, "%s", L(d, "Arquivo de tema não encontrado: usando a cópia salva no relatório", "Theme file not found: using the copy saved in the report"));
        } else snprintf(d->rt_warn, sizeof d->rt_warn, "%s: %s", L(d, "Tema indisponível", "Theme unavailable"), why[0] ? why : "?");
    } else if (!strcmp(ref, "snapshot") && d->r.theme_snapshot) {
        ok = rtheme_parse(&t, d->r.theme_snapshot, strlen(d->r.theme_snapshot), why, sizeof why);
    } else if (!strncmp(ref, "builtin:", 8)) { rtheme_builtin(&t, ref + 8); ok = true; }
    if (!ok) rtheme_builtin(&t, d->r.theme);
    if (d->r.theme_overrides && !rtheme_apply_overrides(&t, d->r.theme_overrides, strlen(d->r.theme_overrides), why, sizeof why))
        snprintf(d->rt_warn, sizeof d->rt_warn, "%s: %s", L(d, "Ajustes de tema ignorados", "Theme overrides ignored"), why);
    d->rt = t;
    snprintf(d->rt_key, sizeof d->rt_key, "%s", key); d->rt_checked = now; d->rt_mtime = mt;
}
/* ---- images (logo, image cards): decoded once, kept as textures, reloaded when the file changes ---- */
static struct { char path[1100]; long long mt; SDL_Texture *tex; int w, h; SDL_Renderer *ren; } g_img[16];
static SDL_Texture *image_tex(App *a, const char *path, int *w, int *h) {
    struct stat st; if (stat(path, &st) != 0 || st.st_size > 8 * 1024 * 1024) return NULL;
    int slot = -1;
    for (int i = 0; i < 16; i++) if (!strcmp(g_img[i].path, path)) { if (g_img[i].mt == (long long)st.st_mtime && g_img[i].ren == a->renderer && g_img[i].tex) { *w = g_img[i].w; *h = g_img[i].h; return g_img[i].tex; } slot = i; }
    if (slot < 0) { for (int i = 0; i < 16; i++) if (!g_img[i].path[0]) { slot = i; break; } if (slot < 0) slot = (int)(SDL_GetTicks() % 16); }
    if (g_img[slot].tex) SDL_DestroyTexture(g_img[slot].tex);
    memset(&g_img[slot], 0, sizeof g_img[slot]);
    FILE *f = fopen(path, "rb"); if (!f) return NULL;
    unsigned char *b = malloc((size_t)st.st_size); size_t got = fread(b, 1, (size_t)st.st_size, f); fclose(f);
    Image im; char err[200];
    bool ok = image_decode(b, got, &im, err, sizeof err); free(b);
    if (!ok) { LOG_W("report: image not shown (%s)", err); return NULL; }
    SDL_Texture *t = SDL_CreateTexture(a->renderer, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, im.w, im.h);
    if (t) { SDL_UpdateTexture(t, NULL, im.rgba, im.w * 4); SDL_SetTextureBlendMode(t, SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(t, SDL_SCALEMODE_LINEAR); }
    snprintf(g_img[slot].path, sizeof g_img[slot].path, "%s", path); g_img[slot].mt = (long long)st.st_mtime; g_img[slot].tex = t; g_img[slot].w = im.w; g_img[slot].h = im.h; g_img[slot].ren = a->renderer;
    image_free(&im);
    *w = g_img[slot].w; *h = g_img[slot].h;
    return t;
}
/* draws an image inside rc keeping its proportions (contain); align -1 left, 0 centre, 1 right */
static void draw_image_fit(App *a, const char *path, Rect rc, int align) {
    int w = 0, h = 0; SDL_Texture *t = image_tex(a, path, &w, &h);
    if (!t || w <= 0 || h <= 0) return;
    float s = fminf(rc.w / w, rc.h / h), dw = w * s, dh = h * s;
    float x = align < 0 ? rc.x : align > 0 ? rc.x + rc.w - dw : rc.x + (rc.w - dw) / 2;
    SDL_FRect dst = { x, rc.y + (rc.h - dh) / 2, dw, dh };
    SDL_RenderTexture(a->renderer, t, NULL, &dst);
}
static void theme_logo_path(const RvDoc *d, char *out, size_t n) { if (d->rt.logo[0] && d->rt_dir[0]) snprintf(out, n, "%s/%s", d->rt_dir, d->rt.logo); else out[0] = 0; }
/* vertical gradient (top c1 -> bottom c2) */
static void draw_vgrad(SDL_Renderer *r, Rect rc, uint32_t c1, uint32_t c2, float alpha) {
    SDL_FColor a1 = { ((c1 >> 16) & 255) / 255.f, ((c1 >> 8) & 255) / 255.f, (c1 & 255) / 255.f, alpha }, a2 = { ((c2 >> 16) & 255) / 255.f, ((c2 >> 8) & 255) / 255.f, (c2 & 255) / 255.f, alpha };
    SDL_Vertex v[4] = { { { rc.x, rc.y }, a1, { 0, 0 } }, { { rc.x + rc.w, rc.y }, a1, { 0, 0 } }, { { rc.x + rc.w, rc.y + rc.h }, a2, { 0, 0 } }, { { rc.x, rc.y + rc.h }, a2, { 0, 0 } } };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}
/* a card's own style (rpt_style_check validated it) */
typedef struct CardStyle { bool fill_set, fill2_set, text_set, accent_set, border_set; uint32_t fill, fill2, text, accent, border; float radius, size, opacity; int variant; int align; bool bold; int shape; } CardStyle;
static uint32_t hex_of(const char *s) { unsigned v = 0; if (s[0] != '#') return 0; size_t L = strlen(s); for (size_t i = 1; i < L; i++) { char c = s[i]; v = v * 16 + (unsigned)(c <= '9' ? c - '0' : (c | 32) - 'a' + 10); } if (L == 4) v = ((v >> 8) & 15) * 0x110000 + ((v >> 4) & 15) * 0x1100 + (v & 15) * 0x11; return v; }
static void card_style(const RptVisual *v, CardStyle *cs) {
    memset(cs, 0, sizeof *cs); cs->radius = -1; cs->variant = -1; cs->align = -1; cs->opacity = 1; cs->shape = 0;
    if (!v->style || !v->style[0]) return;
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, v->style, strlen(v->style))) { json_free(&j); return; }
    char s[64]; double dv;
    if (jr_str(&j, 0, "fill", s, sizeof s)) { cs->fill = hex_of(s); cs->fill_set = true; }
    if (jr_str(&j, 0, "fill2", s, sizeof s)) { cs->fill2 = hex_of(s); cs->fill2_set = true; }
    if (jr_str(&j, 0, "text", s, sizeof s)) { cs->text = hex_of(s); cs->text_set = true; }
    if (jr_str(&j, 0, "accent", s, sizeof s)) { cs->accent = hex_of(s); cs->accent_set = true; }
    if (jr_str(&j, 0, "border", s, sizeof s)) { cs->border = hex_of(s); cs->border_set = true; }
    if (jr_num(&j, 0, "radius", &dv)) cs->radius = (float)dv;
    if (jr_num(&j, 0, "size", &dv)) cs->size = (float)dv;
    if (jr_num(&j, 0, "opacity", &dv)) cs->opacity = (float)dv;
    if (jr_str(&j, 0, "variant", s, sizeof s)) cs->variant = !strcmp(s, "flat") ? 0 : !strcmp(s, "shadow") ? 1 : !strcmp(s, "border") ? 2 : !strcmp(s, "accent-top") ? 3 : !strcmp(s, "plain") ? 10 : !strcmp(s, "accent-bar") ? 11 : !strcmp(s, "tinted") ? 12 : !strcmp(s, "none") ? 20 : -1;
    if (jr_str(&j, 0, "align", s, sizeof s)) cs->align = !strcmp(s, "left") ? 0 : !strcmp(s, "center") ? 1 : 2;
    if (jr_str(&j, 0, "weight", s, sizeof s)) cs->bold = !strcmp(s, "bold");
    if (jr_str(&j, 0, "shape", s, sizeof s)) cs->shape = !strcmp(s, "line") ? 1 : !strcmp(s, "circle") ? 2 : 0;
    json_free(&j);
}
static uint32_t mix(uint32_t a, uint32_t b, float t) { int ar = (a >> 16) & 255, ag = (a >> 8) & 255, ab = a & 255, br = (b >> 16) & 255, bg = (b >> 8) & 255, bb = b & 255; return ((uint32_t)(ar + (br - ar) * t) << 16) | ((uint32_t)(ag + (bg - ag) * t) << 8) | (uint32_t)(ab + (bb - ab) * t); }
static void ensure_fonts(App *a) {
    if (G.f_title && G.fonts_scale == a->scale) return;
    font_destroy(G.f_title); font_destroy(G.f_kpi);
    char p[1200]; snprintf(p, sizeof p, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
    G.f_title = font_load(a->renderer, p, roundf(16 * a->scale));
    G.f_kpi = font_load(a->renderer, p, roundf(30 * a->scale));
    G.fonts_scale = a->scale;
}
static Font *ft(App *a) { return G.f_title ? G.f_title : a->font_ui; }
static Font *fk(App *a) { return G.f_kpi ? G.f_kpi : a->font_ui; }

/* nice axis ticks */
static double nicenum(double x, bool round_) { double e = floor(log10(x)), f = x / pow(10, e), nf; if (round_) nf = f < 1.5 ? 1 : f < 3 ? 2 : f < 7 ? 5 : 10; else nf = f <= 1 ? 1 : f <= 2 ? 2 : f <= 5 ? 5 : 10; return nf * pow(10, e); }
static int ticks(double lo, double hi, double *t, int max) {
    if (hi <= lo) hi = lo + 1;
    double step = nicenum(nicenum(hi - lo, false) / 4, true);
    double s = floor(lo / step) * step; int n = 0;
    for (double v = s; v <= hi + step * 0.5 && n < max; v += step) t[n++] = v;
    return n;
}
static const SemMetricDef *metric_def(const RptVisual *v) { const SemMetric *m = sem_metric_c(&G.cat, v->metric); return m ? (v->metric_version ? sem_version(m, v->metric_version) : sem_cur(m)) : NULL; }
static void fmt_v(RvDoc *d, const RptVisual *v, double x, bool null_, char *o, int n) { rpt_fmt_value(x, null_, metric_def(v), v->fmt, v->decimals, d->r.locale, o, n); }
static void fmt_axis(RvDoc *d, const RptVisual *v, double x, char *o, int n) { const SemMetricDef *m = metric_def(v); rpt_fmt_value(x, false, m, m && m->unit == SU_PERCENT ? RF_PERCENT : RF_COMPACT, -1, d->r.locale, o, n); if (m && m->unit == SU_CURRENCY && fabs(x) < 1000) rpt_fmt_value(x, false, m, RF_CURRENCY, 0, d->r.locale, o, n); }
static void tooltip_box(App *a, const Theme *th, float x, float y, const char *l1, const char *l2) {
    SDL_Renderer *r = a->renderer;
    float w = fmaxf(font_width(a->font_small, l1, -1), font_width(a->font_ui, l2, -1)) + px(a, 16), h = px(a, 40);
    Rect b = rect_make(x + px(a, 10), y - h - px(a, 6), w, h);
    draw_rrect(r, b, px(a, 4), C(th->text));
    draw_text(a->font_small, r, l1, b.x + px(a, 8), b.y + px(a, 4), C(th->card));
    draw_text(a->font_ui, r, l2, b.x + px(a, 8), b.y + px(a, 19), C(th->card));
}
static void state_text(App *a, const Theme *th, Rect r, const char *s, uint32_t col) {
    clip_push(a->renderer, r);
    results_draw_wrapped(a->font_small, a->renderer, s, rect_make(r.x + px(a, 4), r.y + px(a, 4), r.w - px(a, 8), r.h - px(a, 8)), C(col), 0, true);
    clip_pop(a->renderer);
    (void)th;
}
/* draws the status of a card's data; true when the data is ready */
static bool data_ready(App *a, RvDoc *d, const Theme *th, Rect area, RvData *x, const char *why) {
    char m[420];
    if (!x) { snprintf(m, sizeof m, "%s %s", L(d, "Card bloqueado:", "Card blocked:"), why); state_text(a, th, area, m, th->neg); return false; }
    switch (x->status) {
    case RD_QUEUED: case RD_RUNNING: snprintf(m, sizeof m, "%s", x->status == RD_RUNNING ? L(d, "Consultando dados (somente leitura)...", "Querying data (read-only)...") : L(d, "Na fila de consultas...", "Queued...")); state_text(a, th, area, m, th->muted); return false;
    case RD_ERROR: snprintf(m, sizeof m, "%s %s", L(d, "Erro na consulta:", "Query error:"), x->err); state_text(a, th, area, m, th->neg); return false;
    case RD_TIMEOUT: snprintf(m, sizeof m, L(d, "Tempo esgotado (%s): reduza o período ou o escopo e atualize. Os demais cards continuam válidos.", "Timed out (%s): narrow the period or scope and refresh. Other cards stay valid."), x->err); state_text(a, th, area, m, th->neg); return false;
    case RD_CANCELLED: state_text(a, th, area, L(d, "Consulta cancelada.", "Query cancelled."), th->muted); return false;
    }
    if (!x->rs.ok) { state_text(a, th, area, x->rs.error[0] ? x->rs.error : "no result", th->neg); return false; }
    return true;
}
static void draw_kpi(App *a, RvDoc *d, const Theme *th, const RptVisual *v, Rect area, bool hover_ok) {
    SDL_Renderer *r = a->renderer;
    char why[300] = "";
    RvData *x = visual_data(a, d, v, false, why, sizeof why);
    if (!data_ready(a, d, th, area, x, why)) return;
    double cur = 0; bool cn = x->rs.nrows < 1 || !cell_num(&x->rs, 0, x->rs.ncols - 1, &cur);
    char val[96]; fmt_v(d, v, cur, cn, val, sizeof val);
    /* the value never gets cut and stays the card's biggest text: its own face, then the title's; before
     * going smaller, the compact form ("R$ 293,0 mil"); smaller faces only as the last resort */
    float room = area.w - px(a, 2);
    Font *vf = fk(a);
    if (font_width(vf, val, -1) > room) vf = ft(a);
    if (font_width(vf, val, -1) > room && !cn) {
        char cv[96]; rpt_fmt_value(cur, false, metric_def(v), RF_COMPACT, -1, d->r.locale, cv, sizeof cv);
        if (font_width(fk(a), cv, -1) <= room) { snprintf(val, sizeof val, "%s", cv); vf = fk(a); }
        else { snprintf(val, sizeof val, "%s", cv); vf = ft(a); }   /* shorter either way: the smaller faces below get it */
    }
    if (font_width(vf, val, -1) > room) vf = a->font_ui;
    if (font_width(vf, val, -1) > room) vf = a->font_small;
    draw_text_fit(vf, r, val, -1, rect_make(area.x, area.y + px(a, 2), area.w, font_height(vf)), C(cn ? th->muted : th->text), ALIGN_LEFT);
    const SemMetricDef *md = metric_def(v);
    float y = area.y + font_height(vf) + px(a, 6);
    if (v->compare != RC_NONE) {
        char why2[300] = "";
        RvData *bx = visual_data(a, d, v, true, why2, sizeof why2);
        const char *cmpname = v->compare == RC_YOY ? L(d, "vs mesmo período do ano anterior", "vs same period last year") : L(d, "vs período anterior", "vs previous period");
        if (!bx || bx->status != RD_OK) { draw_text(a->font_small, r, bx && (bx->status == RD_QUEUED || bx->status == RD_RUNNING) ? L(d, "Calculando comparação...", "Computing comparison...") : (bx ? bx->err : why2), area.x + px(a, 4), y, C(th->muted)); return; }
        double base = 0; bool bn = bx->rs.nrows < 1 || !cell_num(&bx->rs, 0, bx->rs.ncols - 1, &base);
        double ab, rel; bool aok, rok; rpt_delta(cur, cn, base, bn, &ab, &rel, &aok, &rok);
        char line[200], bs[96]; fmt_v(d, v, base, bn, bs, sizeof bs);
        uint32_t col = th->muted;
        if (rok && md && md->direction) col = (rel > 0) == (md->direction > 0) ? th->pos : th->neg;   /* green / red only when the metric says what "better" means (LI-063.2) */
        if (rok) {
            char pc[48]; rpt_fmt_value(rel, false, NULL, RF_PERCENT, 1, d->r.locale, pc, sizeof pc);
            snprintf(line, sizeof line, "%s%s", rel > 0 ? "+" : "", pc);
            if (rel != 0) draw_triangle(r, area.x + px(a, 9), y + px(a, 8), px(a, 9), px(a, 7), rel < 0, C(col));
            draw_text(a->font_ui, r, line, area.x + px(a, 18), y, C(col));
            /* the comparator is always said (R03): on its own line so it is never cut */
            draw_text_fit(a->font_small, r, cmpname, -1, rect_make(area.x + px(a, 4), y + px(a, 18), area.w - px(a, 4), px(a, 14)), C(th->muted), ALIGN_LEFT);
            y += px(a, 16);
        } else {
            snprintf(line, sizeof line, "%s %s", L(d, "Sem variação calculável", "No computable change"), aok ? L(d, "(base zero)", "(zero base)") : L(d, "(sem dado na base)", "(no data in the base)"));
            draw_text(a->font_ui, r, line, area.x + px(a, 4), y, C(th->muted));
        }
        char bl[200]; snprintf(bl, sizeof bl, "%s %s", L(d, "Base:", "Base:"), bs);
        draw_text(a->font_small, r, bl, area.x + px(a, 4), y + px(a, 20), C(th->muted));
    }
    (void)hover_ok;
}
typedef struct Series { int n; char lab[400][16]; double v[400]; bool nul[400]; } Series;
static Series SER;
static int series_of(RvDoc *d, const RptVisual *v, const RvData *x, Series *s) {
    s->n = 0;
    int vc = x->rs.ncols - 1; int lc = 0;
    for (int i = 0; i < x->rs.nrows && s->n < 400; i++) {
        const char *l = rs_cell(&x->rs, i, lc);
        if (v->grain >= 0 && l) rpt_fmt_period(l, v->grain, d->r.locale, s->lab[s->n], 16); else snprintf(s->lab[s->n], 16, "%s", l ? l : "(null)");
        s->nul[s->n] = !cell_num(&x->rs, i, vc, &s->v[s->n]);
        s->n++;
    }
    return s->n;
}
static void draw_xy(App *a, RvDoc *d, const Theme *th, const RptVisual *v, Rect area, bool line, bool interactive) {
    SDL_Renderer *r = a->renderer;
    char why[300] = "";
    RvData *x = visual_data(a, d, v, false, why, sizeof why);
    if (!data_ready(a, d, th, area, x, why)) return;
    Series *s = &SER; series_of(d, v, x, s);
    if (!s->n) { state_text(a, th, area, L(d, "Sem dados no período e filtros escolhidos.", "No data for the chosen period and filters."), th->muted); return; }
    double lo = 0, hi = 0; bool any = false;
    for (int i = 0; i < s->n; i++) if (!s->nul[i]) { if (!any || s->v[i] > hi) hi = s->v[i]; if (!any || s->v[i] < lo) lo = s->v[i]; any = true; }
    if (lo > 0) lo = 0;   /* never a truncated axis by default (LI-052.2) */
    double t[12]; int nt = ticks(lo, hi, t, 12); lo = t[0]; hi = t[nt - 1];
    bool horiz = !line && v->dim[0] && s->n > 6;
    float lw = 0; char lb[64];
    for (int i = 0; i < nt; i++) { fmt_axis(d, v, t[i], lb, sizeof lb); lw = fmaxf(lw, font_width(a->font_small, lb, -1)); }
    if (horiz) {
        float labw = 0; for (int i = 0; i < s->n; i++) labw = fmaxf(labw, font_width(a->font_small, s->lab[i], -1));
        labw = fminf(labw + px(a, 6), area.w * 0.35f);
        Rect pl = rect_make(area.x + labw, area.y, area.w - labw - px(a, 70), area.h - px(a, 4));
        float bh = pl.h / (float)s->n;
        for (int i = 0; i < s->n; i++) {
            float y = pl.y + bh * i;
            draw_text_fit(a->font_small, r, s->lab[i], -1, rect_make(area.x, y, labw - px(a, 6), bh), C(th->muted), ALIGN_RIGHT);
            if (s->nul[i]) { draw_text(a->font_small, r, L(d, "sem dado", "no data"), pl.x + px(a, 4), y + bh / 2 - px(a, 7), C(th->muted)); continue; }
            float w = hi > lo ? (float)((s->v[i] - 0) / (hi - lo)) * pl.w : 0; if (w < 1 && s->v[i] > 0) w = 1;
            Rect bar = rect_make(pl.x, y + bh * 0.15f, fmaxf(0, w), bh * 0.7f);
            draw_rect(r, bar, C(th->series[0]));
            char vl[64]; fmt_v(d, v, s->v[i], false, vl, sizeof vl);
            if (bh >= px(a, 12)) draw_text_fit(a->font_small, r, vl, -1, rect_make(bar.x + bar.w + px(a, 4), y, px(a, 70), bh), C(th->text), ALIGN_LEFT);
            if (interactive && rect_contains(rect_make(area.x, y, area.w, bh), a->in.mouse_x, a->in.mouse_y)) tooltip_box(a, th, a->in.mouse_x, a->in.mouse_y, s->lab[i], vl);
        }
        return;
    }
    Rect pl = rect_make(area.x + lw + px(a, 8), area.y + px(a, 4), area.w - lw - px(a, 12), area.h - px(a, 24));
    for (int i = 0; i < nt; i++) {
        float y = pl.y + pl.h - (float)((t[i] - lo) / (hi - lo)) * pl.h;
        draw_rect(r, rect_make(pl.x, y, pl.w, px(a, 1)), C(t[i] == 0 ? th->muted : th->grid));
        fmt_axis(d, v, t[i], lb, sizeof lb);
        draw_text_fit(a->font_small, r, lb, -1, rect_make(area.x, y - px(a, 7), lw + px(a, 2), px(a, 14)), C(th->muted), ALIGN_RIGHT);
    }
    float step = pl.w / (float)s->n;
    float maxlab = 0; for (int i = 0; i < s->n; i++) maxlab = fmaxf(maxlab, font_width(a->font_small, s->lab[i], -1));
    int every = (int)ceilf((maxlab + px(a, 8)) / fmaxf(step, 1)); if (every < 1) every = 1;
    float zero = pl.y + pl.h - (float)((0 - lo) / (hi - lo)) * pl.h;
    int hov = -1;
    if (interactive && rect_contains(rect_make(pl.x, pl.y, pl.w, pl.h + px(a, 20)), a->in.mouse_x, a->in.mouse_y)) { hov = (int)((a->in.mouse_x - pl.x) / step); if (hov >= s->n) hov = s->n - 1; }
    float px_prev = 0, py_prev = 0; bool have_prev = false;
    for (int i = 0; i < s->n; i++) {
        float cx = pl.x + step * (i + 0.5f);
        if (i % every == 0) draw_text_fit(a->font_small, r, s->lab[i], -1, rect_make(cx - step * every / 2, pl.y + pl.h + px(a, 4), step * every, px(a, 14)), C(th->muted), ALIGN_CENTER);
        if (s->nul[i]) { have_prev = false; continue; }   /* a gap, never a zero (R03) */
        float y = pl.y + pl.h - (float)((s->v[i] - lo) / (hi - lo)) * pl.h;
        if (line) {
            if (have_prev) draw_line(r, px_prev, py_prev, cx, y, px(a, 2), C(th->series[0]));
            draw_rrect(r, rect_make(cx - px(a, 3), y - px(a, 3), px(a, 6), px(a, 6)), px(a, 3), C(hov == i ? th->text : th->series[0]));
            px_prev = cx; py_prev = y; have_prev = true;
        } else {
            float bw = fmaxf(px(a, 2), step * 0.7f);
            Rect bar = s->v[i] >= 0 ? rect_make(cx - bw / 2, y, bw, zero - y) : rect_make(cx - bw / 2, zero, bw, y - zero);
            draw_rect(r, bar, C(hov == i ? th->accent : th->series[0]));
        }
    }
    if (hov >= 0) { char vl[64]; fmt_v(d, v, s->v[hov], s->nul[hov], vl, sizeof vl); tooltip_box(a, th, a->in.mouse_x, a->in.mouse_y, s->lab[hov], vl); }
}
static TblState *tbl_state(RvDoc *d, const char *id) {
    for (int i = 0; i < 16; i++) if (!strcmp(d->tbl[i].id, id)) return &d->tbl[i];
    for (int i = 0; i < 16; i++) if (!d->tbl[i].id[0]) { snprintf(d->tbl[i].id, sizeof d->tbl[i].id, "%s", id); d->tbl[i].sort_col = -1; return &d->tbl[i]; }
    return &d->tbl[0];
}
static const ResultSet *SORT_RS; static int SORT_COL; static bool SORT_DESC;
static int sort_cmp(const void *pa, const void *pb) {
    int i = *(const int *)pa, k = *(const int *)pb;
    const char *x = rs_cell(SORT_RS, i, SORT_COL), *y = rs_cell(SORT_RS, k, SORT_COL);
    int c;
    if (!x || !y) c = !x - !y;
    else { char *e1, *e2; double a1 = strtod(x, &e1), a2 = strtod(y, &e2); c = (*e1 == 0 && *e2 == 0 && e1 != x && e2 != y) ? (a1 > a2) - (a1 < a2) : strcmp(x, y); }
    return SORT_DESC ? -c : c;
}
static void draw_table(App *a, RvDoc *d, const Theme *th, const RptVisual *v, Rect area, bool interactive) {
    SDL_Renderer *r = a->renderer;
    char why[300] = "";
    RvData *x = visual_data(a, d, v, false, why, sizeof why);
    if (!data_ready(a, d, th, area, x, why)) return;
    const ResultSet *rs = &x->rs;
    TblState *ts = tbl_state(d, v->id);
    float rh = px(a, 20), hh = px(a, 22), fh = px(a, 18);
    int nc = rs->ncols; if (nc < 1) return;
    float cw = area.w / (float)nc;
    /* local sort only (LI-042.1): an index, the result is never changed */
    static int idx[MAX_ROWS + 1]; int n = rs->nrows < MAX_ROWS ? rs->nrows : MAX_ROWS;
    for (int i = 0; i < n; i++) idx[i] = i;
    if (ts->sort_col >= 0 && ts->sort_col < nc) { SORT_RS = rs; SORT_COL = ts->sort_col; SORT_DESC = ts->desc; qsort(idx, (size_t)n, sizeof idx[0], sort_cmp); }
    const SemMetricDef *md = v->sql ? NULL : metric_def(v);
    for (int c = 0; c < nc; c++) {
        Rect h = rect_make(area.x + cw * c, area.y, cw, hh);
        const char *name = rs->cols[c];
        char lab[96];
        if (!v->sql) { if (c == nc - 1) snprintf(lab, sizeof lab, "%s", md ? md->name : name); else if (v->grain >= 0 && c == 0) snprintf(lab, sizeof lab, "%s", L(d, "Período", "Period")); else { const SemDataset *ds = dataset_of(v); const SemColumn *sc = sem_column(ds, v->dim); snprintf(lab, sizeof lab, "%s", sc && sc->label[0] ? sc->label : v->dim); } }
        else snprintf(lab, sizeof lab, "%s", name);
        if (ts->sort_col == c) strncat(lab, ts->desc ? " \xE2\x96\xBE" : " \xE2\x96\xB8", sizeof lab - strlen(lab) - 1);
        draw_text_fit(a->font_small, r, lab, -1, rect_make(h.x + px(a, 4), h.y, h.w - px(a, 8), h.h), C(th->muted), c == nc - 1 && !v->sql ? ALIGN_RIGHT : ALIGN_LEFT);
        if (interactive && rect_contains(h, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; if (ts->sort_col == c) ts->desc = !ts->desc; else { ts->sort_col = c; ts->desc = c == nc - 1; } }
    }
    draw_rect(r, rect_make(area.x, area.y + hh, area.w, px(a, 1)), C(th->grid));
    Rect body = rect_make(area.x, area.y + hh + px(a, 1), area.w, area.h - hh - fh - px(a, 2));
    float total = n * rh;
    if (interactive && rect_contains(body, a->in.mouse_x, a->in.mouse_y) && a->in.wheel_y != 0) { ts->scroll -= a->in.wheel_y * rh * 2; a->in.wheel_y = 0; }
    if (ts->scroll > total - body.h) ts->scroll = total - body.h; if (ts->scroll < 0) ts->scroll = 0;
    clip_push(r, body);
    int first = (int)(ts->scroll / rh), last = (int)((ts->scroll + body.h) / rh) + 1; if (last > n) last = n;   /* virtualized (LI-042.2) */
    for (int i = first; i < last; i++) {
        float y = body.y + i * rh - ts->scroll;
        for (int c = 0; c < nc; c++) {
            const char *cell = rs_cell(rs, idx[i], c);
            char o[128];
            bool numcol = !v->sql && c == nc - 1;
            if (numcol) { double val; bool ok = cell_num(rs, idx[i], c, &val); fmt_v(d, v, val, !ok, o, sizeof o); }
            else if (!v->sql && v->grain >= 0 && c == 0 && cell) rpt_fmt_period(cell, v->grain, d->r.locale, o, sizeof o);
            else snprintf(o, sizeof o, "%s", cell ? cell : "NULL");
            draw_text_fit(a->font_small, r, o, -1, rect_make(area.x + cw * c + px(a, 4), y, cw - px(a, 8), rh), C(cell ? th->text : th->muted), numcol ? ALIGN_RIGHT : ALIGN_LEFT);
        }
        draw_rect(r, rect_make(area.x, y + rh - px(a, 1), area.w, px(a, 1)), C(th->grid));
    }
    clip_pop(r);
    char foot[200];
    if (rs->truncated && !rs->complete) snprintf(foot, sizeof foot, L(d, "%d linhas carregadas (limite %d); total não calculado", "%d rows loaded (limit %d); total not computed"), rs->nrows, MAX_ROWS);
    else snprintf(foot, sizeof foot, L(d, "%d linhas (total)", "%d rows (total)"), rs->nrows);
    draw_text_fit(a->font_small, r, foot, -1, rect_make(area.x + px(a, 4), area.y + area.h - fh, area.w - px(a, 8), fh), C(th->muted), ALIGN_LEFT);
}
static void draw_text_card(App *a, RvDoc *d, const Theme *th, const RptVisual *v, Rect area) {
    SDL_Renderer *r = a->renderer;
    const char *t = v->text ? v->text : "";
    float y = area.y;
    clip_push(r, area);
    while (*t && y < area.y + area.h) {
        const char *e = strchr(t, '\n'); int len = e ? (int)(e - t) : (int)strlen(t);
        char line[1200]; snprintf(line, sizeof line, "%.*s", len, t);
        bool bullet = !strncmp(line, "- ", 2);
        char shown[1210]; snprintf(shown, sizeof shown, "%s%s", bullet ? "\xE2\x80\xA2 " : "", bullet ? line + 2 : line);
        float hgt = results_draw_wrapped(a->font_ui, r, shown, rect_make(area.x + (bullet ? px(a, 8) : 0), y, area.w - px(a, 8), area.y + area.h - y), C(th->text), 0, true);
        y += fmaxf(hgt, px(a, 18)) + px(a, 2);
        t += len; if (*t == '\n') t++;
    }
    clip_pop(r);
    (void)d;
}
/* the card's title: its own, else what it shows ("Receita líquida por Região", "Pedidos mensal") */
static const char *card_title(RvDoc *d, const RptVisual *v, char *auto_t, size_t n) {
    if (v->title[0]) return v->title;
    auto_t[0] = 0;
    const SemMetricDef *md = metric_def(v);
    static const char *const gpt[] = { "diário", "semanal", "mensal", "trimestral", "anual" }, *const gen[] = { "daily", "weekly", "monthly", "quarterly", "yearly" };
    if (md) {
        const SemColumn *dc = v->dim[0] ? sem_column(dataset_of(v), v->dim) : NULL;   /* the business label, not the column id */
        if (v->dim[0]) snprintf(auto_t, n, "%s %s %s", md->name, L(d, "por", "by"), dc && dc->label[0] ? dc->label : v->dim);
        else if (v->grain >= 0) snprintf(auto_t, n, "%s %s", md->name, pt_locale(d) ? gpt[v->grain] : gen[v->grain]);
        else snprintf(auto_t, n, "%s", md->name);
    } else if (v->kind != RK_TEXT) snprintf(auto_t, n, "%s", rpt_kind_name(v->kind));
    return auto_t;
}
/* one card: frame, header, body by kind; returns its rect */
static void draw_card(App *a, RvDoc *d, const Theme *th0, RptVisual *v, Rect rc, bool interactive, bool editing) {
    SDL_Renderer *r = a->renderer;
    bool sel = editing && !strcmp(d->sel, v->id);
    /* the theme's card look, then the card's own style on top */
    CardStyle cs; card_style(v, &cs);
    const RTheme *rt = &d->rt;
    Theme thl = *th0; const Theme *th = &thl;
    if (cs.text_set) { thl.text = cs.text; thl.muted = mix(cs.text, cs.fill_set ? cs.fill : th0->card, 0.35f); }
    if (cs.accent_set) { thl.accent = cs.accent; thl.series[0] = cs.accent; }
    float rad = px(a, cs.radius >= 0 ? cs.radius : rt->radius);
    if (v->kind == RK_SHAPE) {   /* a decorative shape: rectangle, line or circle */
        uint32_t f = cs.fill_set ? cs.fill : th0->accent;
        Color fc = C(f); fc.a = (uint8_t)(255 * cs.opacity);
        if (cs.shape == 1) draw_rect(r, rect_make(rc.x, rc.y + rc.h / 2 - px(a, 1), rc.w, px(a, 2)), fc);
        else if (cs.shape == 2) { float dm = fminf(rc.w, rc.h); draw_rrect(r, rect_make(rc.x + (rc.w - dm) / 2, rc.y + (rc.h - dm) / 2, dm, dm), dm / 2, fc); }
        else if (cs.fill2_set) draw_vgrad(r, rc, f, cs.fill2, cs.opacity);
        else draw_rrect(r, rc, rad, fc);
        if (sel) draw_rect_outline(r, rc, px(a, 2), C(th->sel));
        return;
    }
    int variant = cs.variant >= 0 && cs.variant <= 3 ? cs.variant : cs.variant == 20 ? 20 : rt->card_style;
    int kpi_style = cs.variant >= 10 && cs.variant <= 12 ? cs.variant - 10 : rt->kpi_style;
    uint32_t bgc = cs.fill_set ? cs.fill : (v->kind == RK_KPI && kpi_style == RKS_TINTED) ? mix(th0->card, thl.accent, 0.10f) : th0->card;
    thl.card = bgc;
    if (variant != 20) {
        if (variant == RCS_SHADOW) for (int k = rt->shadow; k >= 1; k--) draw_rrect(r, rect_make(rc.x - k * px(a, 1), rc.y + k * px(a, 1.5f), rc.w + k * px(a, 2), rc.h + k * px(a, 1)), rad + k * px(a, 1), color_alpha(color_hex(0x000000), 14));
        bool bordered = variant == RCS_BORDER || variant == RCS_ACCENT_TOP || (variant == RCS_SHADOW && rt->shadow == 0);
        uint32_t bc = cs.border_set ? cs.border : th0->border;
        if (bordered || cs.border_set) { draw_rrect(r, rc, rad, C(bc)); draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), fmaxf(0, rad - px(a, 1)), C(bgc)); }
        else draw_rrect(r, rc, rad, C(bgc));
        if (cs.fill2_set) draw_vgrad(r, rect_inset(rc, px(a, 1), px(a, 1)), bgc, cs.fill2, 1);
        if (variant == RCS_ACCENT_TOP) draw_rect(r, rect_make(rc.x + px(a, 1), rc.y + px(a, 1), rc.w - px(a, 2), px(a, 4)), C(thl.accent));
        if (v->kind == RK_KPI && kpi_style == RKS_ACCENT_BAR) draw_rrect(r, rect_make(rc.x + px(a, 1), rc.y + px(a, 8), px(a, 4), rc.h - px(a, 16)), px(a, 2), C(thl.accent));
    }
    if (sel) draw_rect_outline(r, rc, px(a, 2), C(th->sel));
    if (v->kind == RK_IMAGE) {   /* the theme's logo or a file next to the report */
        char ip[1400] = "";
        if (!strcmp(v->src, "theme:logo")) theme_logo_path(d, ip, sizeof ip);
        else if (v->src[0]) { char dd[1024]; doc_dir(d, dd, sizeof dd); snprintf(ip, sizeof ip, "%s/%s", dd, v->src); }
        Rect ia = rect_inset(rc, px(a, 6), px(a, 6));
        if (ip[0]) draw_image_fit(a, ip, ia, cs.align == 0 ? -1 : cs.align == 2 ? 1 : 0);
        else draw_text_fit(a->font_small, r, L(d, "Sem imagem (o tema não tem logo)", "No image (the theme has no logo)"), -1, ia, C(th->muted), ALIGN_CENTER);
        return;
    }
    /* a narrow card (a KPI in a row of four) keeps its title readable: less padding, the "?" shows on
     * hover only, a smaller font when a word does not fit, two lines before anything is cut */
    bool narrow = rc.w < px(a, 170);
    float pad = px(a, narrow ? 8 : 12);
    char auto_t[160]; const char *title = card_title(d, v, auto_t, sizeof auto_t);
    float hy = rc.y + pad;
    bool card_hover = rect_contains(rc, a->in.mouse_x, a->in.mouse_y);
    if (title[0]) {
        float tw = rc.w - pad * 2 - (narrow ? 0 : px(a, 26));
        Font *tf = ft(a);
        if (narrow) {   /* the longest word must fit, or the smaller font */
            int len = (int)strlen(title), ws = 0; float longest = 0;
            for (int i = 0; i <= len; i++) if (i == len || title[i] == ' ') { float ww = font_width(tf, title + ws, i - ws); if (ww > longest) longest = ww; ws = i + 1; }
            if (longest > tw) tf = a->font_small;
        }
        float fh = font_height(tf);
        int cut = -1, len = (int)strlen(title);
        if (font_width(tf, title, -1) > tw && rc.h > fh * 4)
            for (int i = 0; i < len; i++) if (title[i] == ' ' && font_width(tf, title, i) <= tw) cut = i;
        if (cut > 0) {
            draw_text_fit(tf, r, title, cut, rect_make(rc.x + pad, hy, tw, fh), C(th->text), ALIGN_LEFT); hy += fh;
            draw_text_fit(tf, r, title + cut + 1, -1, rect_make(rc.x + pad, hy, rc.w - pad * 2, fh), C(th->text), ALIGN_LEFT); hy += fh + px(a, 2);
        } else { draw_text_fit(tf, r, title, -1, rect_make(rc.x + pad, hy, tw, fh), C(th->text), ALIGN_LEFT); hy += fh + px(a, 2); }
    }
    if (v->subtitle[0]) { draw_text_fit(a->font_small, r, v->subtitle, -1, rect_make(rc.x + pad, hy, rc.w - pad * 2, px(a, 14)), C(th->muted), ALIGN_LEFT); hy += px(a, 16); }
    if (v->sql && d->r.nparams) {   /* which report filters this card follows */
        char fl[200] = ""; size_t fw = 0;
        for (int i = 0; i < d->r.nparams; i++) { char tok[40]; snprintf(tok, sizeof tok, ":%s", d->r.params[i].name); const char *hit = strstr(v->sql, tok); if (hit && !(hit[strlen(tok)] >= 'a' && hit[strlen(tok)] <= 'z') && hit[strlen(tok)] != '_') fw += (size_t)snprintf(fl + fw, sizeof fl - fw, "%s%s%s", fw ? ", " : "", d->r.params[i].label, d->r.params[i].value[0] ? "\xE2\x9C\x93" : ""); }
        if (fl[0]) { char ln[260]; snprintf(ln, sizeof ln, "%s %s", L(d, "Filtros:", "Filters:"), fl); draw_text_fit(a->font_small, r, ln, -1, rect_make(rc.x + pad, hy, rc.w - pad * 2, px(a, 14)), C(th->accent), ALIGN_LEFT); hy += px(a, 16); }
    }
    /* "Como foi calculado?" (LI-048.2) */
    if (v->kind != RK_TEXT && v->kind != RK_UNKNOWN && v->kind != RK_IMAGE && v->kind != RK_SHAPE) {
        Rect ib = rect_make(rc.x + rc.w - pad - px(a, 18), rc.y + pad, px(a, 18), px(a, 18));
        bool hi = rect_contains(ib, a->in.mouse_x, a->in.mouse_y);
        if (!narrow || card_hover || !strcmp(d->prov, v->id)) {   /* narrow: over the title only while hovered */
            draw_rrect(r, ib, px(a, 9), C(hi ? th->accent : th->grid));
            draw_text_fit(a->font_small, r, "?", -1, ib, C(hi ? 0xFFFFFF : th->muted), ALIGN_CENTER);
        }
        char lab[48]; snprintf(lab, sizeof lab, "rpt:how:%s", v->id); ui_record(lab, ib);
        if (interactive && hi && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; snprintf(d->prov, sizeof d->prov, "%s", strcmp(d->prov, v->id) ? v->id : ""); }
    }
    if (v->ai_sql && !v->ai_suggested) {
        const char *badge = L(d, "SQL da IA \xC2\xB7 não certificado", "AI-written SQL \xC2\xB7 not certified");
        draw_text_fit(a->font_small, r, badge, -1, rect_make(rc.x + pad, rc.y + rc.h - px(a, 18), rc.w - pad * 2, px(a, 14)), C(th->muted), ALIGN_LEFT);
    }
    if (v->ai_suggested) {
        const char *badge = L(d, "Sugestão da IA: revise e aprove", "AI suggestion: review and approve");
        draw_text_fit(a->font_small, r, badge, -1, rect_make(rc.x + pad, rc.y + rc.h - px(a, 18), rc.w - pad * 2, px(a, 14)), C(th->accent), ALIGN_LEFT);
    }
    Rect area = rect_make(rc.x + pad, hy + px(a, 4), rc.w - pad * 2, rc.y + rc.h - hy - px(a, 4) - pad - (v->ai_suggested || v->ai_sql ? px(a, 14) : 0));
    if (area.h < px(a, 10)) return;
    clip_push(r, area);
    switch (v->kind) {
    case RK_KPI: draw_kpi(a, d, th, v, area, interactive); break;
    case RK_LINE: draw_xy(a, d, th, v, area, true, interactive); break;
    case RK_BAR: draw_xy(a, d, th, v, area, false, interactive); break;
    case RK_TABLE: draw_table(a, d, th, v, area, interactive); break;
    case RK_TEXT: draw_text_card(a, d, th, v, area); break;
    default: {
        char m[300]; snprintf(m, sizeof m, L(d, "Visual '%s' não é suportado por este renderizador: a especificação foi preservada sem alterações.", "Visual '%s' is not supported by this renderer: its specification is kept unchanged."), v->kind_name);
        state_text(a, th, area, m, th->muted);
    }
    }
    clip_pop(r);
}

/* ---- page ---- */
static Rect cell_rect(const RvDoc *d, const RptVisual *v) {
    return rect_make(d->canvas.x + d->gap + v->x * (d->colw + d->gap), d->canvas.y + d->head_h + d->gap + v->y * (d->rowh + d->gap) - d->scroll, v->w * d->colw + (v->w - 1) * d->gap, v->h * d->rowh + (v->h - 1) * d->gap);
}
static void layout_grid(App *a, RvDoc *d, Rect canvas) {
    d->canvas = canvas; d->gap = px(a, GAP_PT); d->rowh = px(a, ROW_PT);
    theme_of(d); d->head_h = d->rt.header ? px(a, d->rt.header_h) : 0;   /* the theme's header band sits above the grid */
    d->colw = (canvas.w - d->gap * (RPT_COLS + 1)) / RPT_COLS;
}
static float page_height(const RvDoc *d) { int rows = 0; const RptPage *p = &d->r.pages[d->page]; for (int i = 0; i < p->nv; i++) if (p->v[i].y + p->v[i].h > rows) rows = p->v[i].y + p->v[i].h; return d->head_h + d->gap + rows * (d->rowh + d->gap); }
static void provenance_line(App *a, RvDoc *d, char *o, int n) {
    char label[80]; SemPeriod c, b; rpt_period(&d->r, RC_NONE, &c, &b, label, sizeof label);
    /* sources and freshness of the page's data */
    char src[200] = ""; double newest = 0; int uncert = 0;
    const RptPage *p = &d->r.pages[d->page];
    for (int i = 0; i < p->nv; i++) {
        const RptVisual *v = &p->v[i];
        if (v->sql) { if (!strstr(src, "SQL")) strncat(src, src[0] ? ", SQL" : "SQL", sizeof src - strlen(src) - 1); continue; }
        const SemDataset *ds = dataset_of(v); const SemMetricDef *md = metric_def(v);
        if (md && md->certification != SC_CERTIFIED) uncert++;
        if (ds && !strstr(src, ds->name[0] ? ds->name : ds->id)) { if (src[0]) strncat(src, ", ", sizeof src - strlen(src) - 1); strncat(src, ds->name[0] ? ds->name : ds->id, sizeof src - strlen(src) - 1); }
    }
    for (int i = 0; i < MAX_DATA; i++) if (d->data[i].used && d->data[i].status == RD_OK && d->data[i].fetched_at > newest) newest = d->data[i].fetched_at;
    char ts[40] = "-"; if (newest > 0) { time_t t = (time_t)newest; struct tm tm; localtime_r(&t, &tm); strftime(ts, sizeof ts, pt_locale(d) ? "%d/%m/%Y %H:%M" : "%m/%d/%Y %H:%M", &tm); }
    const RptFilter *f = page_filter(d);
    char fl[200]; if (f && f->value[0]) snprintf(fl, sizeof fl, "%s = %s", f->field, f->value); else snprintf(fl, sizeof fl, "%s", L(d, "nenhum", "none"));
    char cert[80] = ""; if (uncert) snprintf(cert, sizeof cert, L(d, "  \xC2\xB7  %d métrica(s) não certificada(s)", "  \xC2\xB7  %d uncertified metric(s)"), uncert);
    snprintf(o, (size_t)n, L(d, "Período: %s  \xC2\xB7  Filtros: %s  \xC2\xB7  Fonte: %s  \xC2\xB7  Dados de %s (consulta ao vivo)%s  \xC2\xB7  %s rev %d", "Period: %s  \xC2\xB7  Filters: %s  \xC2\xB7  Source: %s  \xC2\xB7  Data as of %s (live query)%s  \xC2\xB7  %s rev %d"),
             label, fl, src[0] ? src : "-", ts, cert, d->r.status == RS_REVIEWED ? "Reviewed" : "Draft", d->r.revision);
    (void)a;
}
/* draws every card of the current page in canvas; interactive: hover / sort / "?"; editing: selection */
static void draw_page(App *a, RvDoc *d, Rect canvas, bool interactive, bool editing) {
    const Theme *th = theme_of(d);
    draw_rect(a->renderer, canvas, C(th->bg));
    layout_grid(a, d, canvas);
    RptPage *p = &d->r.pages[d->page];
    if (d->head_h > 0) {   /* the theme's header band: logo, report title, page and period */
        SDL_Renderer *r = a->renderer;
        clip_push(r, canvas);
        Rect hb = rect_make(canvas.x, canvas.y - d->scroll, canvas.w, d->head_h);
        draw_rect(r, hb, C(d->rt.header_bg));
        draw_rect(r, rect_make(hb.x, hb.y + hb.h - px(a, 1), hb.w, px(a, 1)), color_alpha(C(d->rt.header_text), 40));
        float padx = px(a, 22), lx = hb.x + padx, rx = hb.x + hb.w - padx;
        char lp[1400]; theme_logo_path(d, lp, sizeof lp);
        if (lp[0]) {
            float lh = fminf(px(a, d->rt.logo_h), hb.h - px(a, 12)), lw = lh * 4;
            Rect lr = d->rt.logo_pos == RLP_RIGHT ? rect_make(rx - lw, hb.y + (hb.h - lh) / 2, lw, lh) : rect_make(lx, hb.y + (hb.h - lh) / 2, lw, lh);
            int iw = 0, ih = 0;
            if (image_tex(a, lp, &iw, &ih) && ih > 0) { float rw = lh * iw / ih; if (d->rt.logo_pos == RLP_RIGHT) { lr.x = rx - rw; rx -= rw + px(a, 16); } else { lx += rw + px(a, 16); } lr.w = rw; }
            draw_image_fit(a, lp, lr, d->rt.logo_pos == RLP_RIGHT ? 1 : -1);
            ui_record("rpt:logo", lr);
        }
        float ty = hb.y + hb.h / 2 - font_height(G.f_title ? G.f_title : a->font_ui) + px(a, 2);
        draw_text_fit(G.f_title ? G.f_title : a->font_ui, r, d->r.title, -1, rect_make(lx, ty, rx - lx, font_height(G.f_title ? G.f_title : a->font_ui)), C(d->rt.header_text), ALIGN_LEFT);
        char label[80]; SemPeriod pc, pb; rpt_period(&d->r, RC_NONE, &pc, &pb, label, sizeof label);
        char sub[200]; snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", p->name, label);
        draw_text_fit(a->font_small, r, sub, -1, rect_make(lx, hb.y + hb.h / 2 + px(a, 4), rx - lx, px(a, 16)), color_alpha(C(d->rt.header_text), 180), ALIGN_LEFT);
        ui_record("rpt:header", hb);
        clip_pop(r);
    }
    if (!p->nv) {
        const char *m = d->read_mode ? L(d, "Página vazia.", "Empty page.") : L(d, "Página vazia: use Adicionar (KPI, Linha, Barras, Tabela, Texto) ou peça ao assistente de IA.", "Empty page: use Add (KPI, Line, Bar, Table, Text) or ask the AI assistant.");
        draw_text_fit(a->font_ui, a->renderer, m, -1, rect_inset(canvas, px(a, 20), px(a, 20)), C(th->muted), ALIGN_CENTER);
    }
    clip_push(a->renderer, canvas);
    if (editing && d->drag) {   /* the grid while moving (LI-044.1) */
        for (int c = 0; c <= RPT_COLS; c++) { float x = canvas.x + d->gap / 2 + c * (d->colw + d->gap); draw_rect(a->renderer, rect_make(x, canvas.y, px(a, 1), canvas.h), C(th->grid)); }
    }
    for (int i = 0; i < p->nv; i++) {
        RptVisual *v = &p->v[i];
        if (editing && d->drag && !strcmp(d->drag_id, v->id)) continue;
        Rect rc = cell_rect(d, v);
        if (rc.y > canvas.y + canvas.h || rc.y + rc.h < canvas.y) continue;   /* painted on demand (LI-092) */
        draw_card(a, d, th, v, rc, interactive, editing);
        char lab[48]; snprintf(lab, sizeof lab, "rpt:card:%s", v->id); ui_record(lab, rc);
    }
    if (editing && d->drag) {
        int pg; RptVisual *v = rpt_find(&d->r, d->drag_id, &pg);
        if (v) { RptVisual g = *v; g.x = d->gx; g.y = d->gy; g.w = d->gw; g.h = d->gh; Rect rc = cell_rect(d, &g); draw_card(a, d, th, v, rc, false, editing); draw_rect_outline(a->renderer, rc, px(a, 2), C(th->accent)); }
    }
    clip_pop(a->renderer);
}
/* "Como foi calculado?" (LI-022 / 048): metric, version, formula, filters, period, query, time */
static void draw_provenance(App *a, RvDoc *d, Rect canvas) {
    int pg; RptVisual *v = rpt_find(&d->r, d->prov, &pg);
    if (!v || pg != d->page) { d->prov[0] = 0; return; }
    SDL_Renderer *r = a->renderer;
    Rect card = cell_rect(d, v);
    float w = px(a, 420), h = px(a, 300);
    Rect b = rect_make(fminf(card.x + card.w - w, canvas.x + canvas.w - w - px(a, 8)), card.y + px(a, 36), w, h);
    if (b.x < canvas.x) b.x = canvas.x + px(a, 8);
    if (b.y + h > canvas.y + canvas.h) b.y = canvas.y + canvas.h - h - px(a, 8);
    draw_rrect(r, rect_make(b.x - 1, b.y - 1, b.w + 2, b.h + 2), px(a, 6), C(T_BORDER));
    draw_rrect(r, b, px(a, 6), C(T_PANEL_ALT));
    char t[3000]; int w2 = 0;
    const SemMetric *m = sem_metric_c(&G.cat, v->metric); const SemMetricDef *md = metric_def(v); const SemDataset *ds = dataset_of(v);
    char label[80]; SemPeriod cur, base; rpt_period(&d->r, RC_NONE, &cur, &base, label, sizeof label);
    if (v->sql) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s\n%s\n\n", L(d, "Tabela a partir do SQL do usuário (somente leitura).", "Table from the user's SQL (read-only)."), v->sql);
    else if (md) {
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s v%d (%s)%s\n", L(d, "Métrica:", "Metric:"), md->name, md->version, sem_cert_name(md->certification), m && sem_cur(m)->version != md->version ? L(d, "  - existe versão mais nova no catálogo", "  - a newer version exists in the catalog") : "");
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s(%s)", L(d, "Cálculo:", "Calculation:"), sem_agg_name(md->aggregation), md->column[0] ? md->column : "*");
        for (int i = 0; i < md->nfilters; i++) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s%s %s %s", i ? " AND " : L(d, " onde ", " where "), md->filters[i].column, md->filters[i].op, md->filters[i].value);
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "\n%s %s  \xC2\xB7  %s %s\n", L(d, "Unidade:", "Unit:"), sem_unit_name(md->unit), L(d, "Responsável:", "Owner:"), md->owner);
        if (ds) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s (%s%s%s)  \xC2\xB7  %s %s\n", L(d, "Fonte:", "Source:"), ds->name, ds->schema, ds->schema[0] ? "." : "", ds->table, L(d, "fuso:", "timezone:"), ds->timezone[0] ? ds->timezone : "?");
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s [%s, %s)", L(d, "Período:", "Period:"), label, cur.from, cur.to);
        if (v->grain >= 0) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "  \xC2\xB7  %s %s", L(d, "granularidade", "grain"), sem_grain_name(v->grain));
        if (v->compare) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "  \xC2\xB7  %s %s", L(d, "comparação", "comparison"), rpt_compare_name(v->compare));
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "\n");
        if (md->change_note[0]) w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s (%s)\n", L(d, "Mudança desta versão:", "Change in this version:"), md->change_note, md->changed_at);
    }
    char why[300]; RvData *x = visual_data(a, d, v, false, why, sizeof why);
    if (x) {
        char ts[40] = "-"; if (x->fetched_at > 0) { time_t tt = (time_t)x->fetched_at; struct tm tm; localtime_r(&tt, &tm); strftime(ts, sizeof ts, "%Y-%m-%d %H:%M:%S", &tm); }
        w2 += snprintf(t + w2, sizeof t - (size_t)w2, "%s %s  \xC2\xB7  %s %.0f ms  \xC2\xB7  %s %d  \xC2\xB7  %s\n\n%s\n", L(d, "Consultado em", "Queried at"), ts, L(d, "tempo", "time"), x->ms, L(d, "linhas", "rows"), x->rs.nrows, RD_NAME[x->status], x->sql);
    }
    clip_push(r, b);
    results_draw_wrapped(a->font_small, r, t, rect_inset(b, px(a, 10), px(a, 8)), C(T_TEXT), 0, true);
    clip_pop(r);
    Rect ob = rect_make(b.x + b.w - px(a, 96), b.y + b.h - px(a, 30), px(a, 86), px(a, 22));
    if (x && ui_button(a, ob, "Open SQL", BTN_NORMAL, true)) { char tt[48]; snprintf(tt, sizeof tt, "%s.sql", v->id); panels_open_tab(a->panels, a, tt, x->conn, x->sql); }
    Rect cb = rect_make(b.x + b.w - px(a, 190), b.y + b.h - px(a, 30), px(a, 86), px(a, 22));
    if (ui_button(a, cb, "Close", BTN_QUIET, true)) d->prov[0] = 0;
}

/* ---- menus ---- */
enum { MK_ADD = 1, MK_THEME, MK_PERIOD, MK_METRIC, MK_DIM, MK_GRAIN, MK_COMPARE, MK_FORMAT, MK_EXPORT, MK_OPEN, MK_FILTER, MK_SAVE, MK_PARAM, MK_MORE };
enum { TBA_ADD = 1, TBA_UNDO, TBA_REDO, TBA_THEME, TBA_PERIOD, TBA_REFRESH, TBA_PREVIEW, TBA_SAVE, TBA_SAVE_AS, TBA_ASSISTANT, TBA_EXPORT, TBA_ZOOM_OUT, TBA_ZOOM_100, TBA_ZOOM_IN };
enum { EX_PDF = 1, EX_PNG, EX_PNG_HI, EX_PNG_CARD, EX_CSV_CARD, EX_XLSX, EX_SAVE_AS, EX_OPEN, EX_THEME_OPEN, EX_THEME_SAVE, EX_LOGO, EX_IMAGE, EX_ATTACH, EX_HTML };
static bool is_open_action(int ac) { return ac == EX_OPEN || ac == EX_THEME_OPEN || ac == EX_LOGO || ac == EX_IMAGE || ac == EX_ATTACH; }
static void ask_path(App *a, RvDoc *d, int action, const char *visual);
static bool ws_theme_path(char *out, size_t n);
static bool file_exists_(const char *p);
static void rel_to_doc(const RvDoc *d, const char *abs, char *out, size_t n);
static void theme_set_ref(RvDoc *d, const char *ref);
static bool theme_write_file(RvDoc *d, const char *path, char *why, int wn);
static bool theme_set_logo(RvDoc *d, const char *img, char *why, int wn);
static bool copy_file(const char *src, const char *dst);
/* an image attached to the conversation (drag & drop onto the chat, or "Attach image..."): a PNG / JPEG
 * the assistant may make the theme's logo. The file stays here; the model gets its name and colours */
static bool chat_attach(const char *path, char *why, int wn) {
    struct stat st; if (stat(path, &st) != 0 || !S_ISREG(st.st_mode)) { snprintf(why, (size_t)wn, "The image could not be read"); return false; }
    if (st.st_size > 8 * 1024 * 1024) { snprintf(why, (size_t)wn, "The image is larger than 8 MB"); return false; }
    FILE *f = fopen(path, "rb"); unsigned char head[8] = { 0 }; size_t got = f ? fread(head, 1, 8, f) : 0; if (f) fclose(f);
    const char *kind = NULL;
    if (!image_sniff(head, got, &kind)) { snprintf(why, (size_t)wn, "Only PNG or JPEG images can be attached"); return false; }
    snprintf(G.attach, sizeof G.attach, "%s", path);
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    int k = 0; for (const char *p = b; *p && k < (int)sizeof G.attach_name - 1; p++) G.attach_name[k++] = (*p == '"' || *p == '\\' || (unsigned char)*p < 32) ? '_' : *p;
    G.attach_name[k] = 0;
    return true;
}
/* theme.header.logo = "attachment" in a design: taken out of the theme (the file is set on approval) */
static bool take_logo_attachment(char *json) {
    for (char *p = strstr(json, "\"logo\""); p; p = strstr(p + 1, "\"logo\"")) {
        char *q = p + 6; while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
        if (*q != ':') continue; q++; while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
        if (strncmp(q, "\"attachment\"", 12)) continue;
        q += 12;
        const char *rp = "\"logoAttached\":1"; size_t rl = strlen(rp), span = (size_t)(q - p);
        if (rl > span) continue;   /* never longer than what it replaces */
        memcpy(p, rp, rl); memset(p + rl, ' ', span - rl);
        return true;
    }
    return false;
}
static void menu_add(PopupMenu *m, int id, const char *label, bool en) { int k = m->n; if (k >= POPUP_MAX) return; snprintf(G.menu_lab[k], sizeof G.menu_lab[k], "%s", label); popup_add(m, id, G.menu_lab[k], false, en); }
static void open_menu(App *a, RvDoc *d, int kind, float x, float y) {
    popup_open(&G.menu, x, y); G.menu_kind = kind; G.menu_doc = d;
    int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg);
    switch (kind) {
    case MK_MORE: for (int i = 0; i < G.nmore; i++) menu_add(&G.menu, G.more_id[i], G.more_lab[i], G.more_en[i]); break;
    case MK_ADD: menu_add(&G.menu, 1, "KPI", true); menu_add(&G.menu, 2, L(d, "Linha", "Line"), true); menu_add(&G.menu, 3, L(d, "Barras", "Bars"), true); menu_add(&G.menu, 4, L(d, "Tabela", "Table"), true); menu_add(&G.menu, 5, L(d, "Texto / nota", "Text / note"), true); menu_add(&G.menu, 6, L(d, "Nova página", "New page"), true); break;
    case MK_THEME: {
        menu_add(&G.menu, 1, "Light", true); menu_add(&G.menu, 2, "Dark", true); menu_add(&G.menu, 3, "Executive", true);
        char wp[1200]; bool ws = ws_theme_path(wp, sizeof wp) && file_exists_(wp);
        menu_add(&G.menu, 10, L(d, "Tema do workspace (Brand Kit)", "Workspace theme (Brand Kit)"), ws);
        menu_add(&G.menu, 11, L(d, "Abrir arquivo de tema...", "Open theme file..."), true);
        menu_add(&G.menu, 12, L(d, "Salvar tema como arquivo...", "Save theme as file..."), true);
        menu_add(&G.menu, 13, L(d, "Usar como tema do workspace (Brand Kit)", "Use as the workspace theme (Brand Kit)"), ws_theme_path(wp, sizeof wp));
        menu_add(&G.menu, 15, L(d, "Escolher logo do tema...", "Choose the theme's logo..."), !d->rt.locked && (!strcmp(d->r.theme_ref, "workspace") || !strncmp(d->r.theme_ref, "file:", 5)));
        menu_add(&G.menu, 16, L(d, "Inserir logo como card", "Insert the logo as a card"), d->rt.logo[0] != 0);
        menu_add(&G.menu, 17, L(d, "Inserir imagem...", "Insert image..."), true);
        menu_add(&G.menu, 14, L(d, "Desvincular (manter só no relatório)", "Detach (keep it in the report only)"), !strcmp(d->r.theme_ref, "workspace") || !strncmp(d->r.theme_ref, "file:", 5));
        break;
    }
    case MK_PERIOD: { static const char *const k[] = { "last_12_months", "ytd", "last_30_days", "previous_year", "all" }; static const char *const pt[] = { "Últimos 12 meses", "Ano até hoje", "Últimos 30 dias", "Ano anterior", "Todo o período" }; static const char *const en[] = { "Last 12 months", "Year to date", "Last 30 days", "Previous year", "All time" }; for (int i = 0; i < 5; i++) menu_add(&G.menu, i + 1, pt_locale(d) ? pt[i] : en[i], true); (void)k; break; }
    case MK_METRIC:
        for (int i = 0; i < G.cat.nm && i < POPUP_MAX; i++) { const SemMetricDef *md = sem_cur(&G.cat.m[i]); char l[96]; snprintf(l, sizeof l, "%s (v%d%s)", md->name, md->version, md->certification == SC_CERTIFIED ? "" : md->certification == SC_DEPRECATED ? ", deprecated" : ", draft"); menu_add(&G.menu, 100 + i, l, md->certification != SC_DEPRECATED); }
        if (!G.cat.nm) menu_add(&G.menu, -1, L(d, "Catálogo semântico vazio (semantic.json)", "Semantic catalog is empty (semantic.json)"), false);
        break;
    case MK_DIM: case MK_FILTER: {
        menu_add(&G.menu, 99, L(d, "(nenhuma)", "(none)"), true);
        const SemDataset *ds = v ? dataset_of(v) : NULL;
        if (kind == MK_FILTER && !ds) for (int i = 0; i < d->r.pages[d->page].nv && !ds; i++) ds = dataset_of(&d->r.pages[d->page].v[i]);
        for (int i = 0; ds && i < ds->ncols; i++) if (sem_is_dimension(ds, ds->cols[i].name) && ds->cols[i].role == SR_DIMENSION) menu_add(&G.menu, 100 + i, ds->cols[i].label[0] ? ds->cols[i].label : ds->cols[i].name, true);
        break; }
    case MK_GRAIN: { menu_add(&G.menu, 99, L(d, "(nenhuma)", "(none)"), true); const SemMetricDef *md = v ? metric_def(v) : NULL; for (int g = 0; g < SG_N; g++) menu_add(&G.menu, 100 + g, sem_grain_name(g), md && (md->grains & (1u << g))); break; }
    case MK_COMPARE: menu_add(&G.menu, 1, L(d, "Sem comparação", "No comparison"), true); menu_add(&G.menu, 2, L(d, "Período anterior", "Previous period"), true); menu_add(&G.menu, 3, L(d, "Mesmo período do ano anterior (YoY)", "Same period last year (YoY)"), true); break;
    case MK_FORMAT: for (int f = 0; f < RF_N; f++) menu_add(&G.menu, 1 + f, rpt_fmt_name(f), true); break;
    case MK_EXPORT:
        menu_add(&G.menu, 7, L(d, "Relatório como página HTML (texto copiável, gráficos nítidos)...", "Report as an HTML page (copyable text, sharp charts)..."), true);
        menu_add(&G.menu, 1, L(d, "Página como PDF (imagem)...", "Page as PDF (image)..."), true);
        menu_add(&G.menu, 2, L(d, "Página como imagem PNG...", "Page as PNG image..."), true);
        menu_add(&G.menu, 3, L(d, "Página como PNG em alta resolução...", "Page as high-resolution PNG..."), true);
        menu_add(&G.menu, 4, L(d, "Card selecionado como PNG...", "Selected card as PNG..."), v != NULL);
        menu_add(&G.menu, 5, L(d, "Dados do card selecionado (CSV)...", "Selected card data (CSV)..."), v != NULL && has_data(v));
        menu_add(&G.menu, 6, L(d, "Dados da página no Excel (XLSX, uma aba por card)...", "Page data to Excel (XLSX, one sheet per card)..."), true);
        break;
    case MK_PARAM: {
        RptParam *pm = G.menu_param >= 0 && G.menu_param < d->r.nparams ? &d->r.params[G.menu_param] : NULL;
        menu_add(&G.menu, 1000, L(d, "Todos", "All"), true);
        if (pm && pm->options_sql[0]) {
            RvData *x = request_data(d, pm->conn, pm->options_sql);
            if (x->status == RD_OK) for (int i = 0; i < x->rs.nrows && i < 40; i++) { const char *ov = rs_cell(&x->rs, i, 0); if (ov) { snprintf(G.menu_lab[i], sizeof G.menu_lab[0], "%s", ov); menu_add(&G.menu, 1001 + i, G.menu_lab[i], true); } }
            else if (x->status == RD_ERROR) menu_add(&G.menu, -1, x->err[0] ? x->err : x->rs.error, false);
            else menu_add(&G.menu, -1, L(d, "Carregando opções...", "Loading options..."), false);
        }
        break;
    }
    case MK_SAVE:
        menu_add(&G.menu, 1, L(d, "Salvar", "Save"), !d->read_mode);
        menu_add(&G.menu, 2, L(d, "Salvar como...", "Save As..."), !d->read_mode);
        break;
    }
    (void)a; (void)pg;
}
static bool export_image(App *a, RvDoc *d, const char *path, const char *visual, int width_px, bool pdf, char *why, int wn);
static bool export_png(App *a, RvDoc *d, const char *path, const char *visual, int width_px, char *why, int wn) { return export_image(a, d, path, visual, width_px, false, why, wn); }
static void ui_save(App *a, RvDoc *d);
static void tb_act(App *a, RvDoc *d, int id, float mx, float my);
/* ---- Save As / Export with the system's save dialog ---- */
static SDL_Mutex *g_dlg_mu;
static struct { int action; char doc_id[40], visual[24]; char path[1024]; bool ready, pending; } g_dlg;
static void file_name_of(const char *title, const char *ext, char *o, size_t n) {
    size_t k = 0;
    for (const char *s = title && *title ? title : "report"; *s && k + 8 < n && k < 80; s++) { unsigned char c = (unsigned char)*s; o[k++] = strchr("/\\:*?\"<>|", c) || c < 32 ? '-' : (char)c; }
    while (k && (o[k - 1] == ' ' || o[k - 1] == '.')) k--;
    snprintf(o + k, n - k, ".%s", ext);
}
static const char *ext_of(int action) { return action == EX_HTML ? "html" : action == EX_PDF ? "pdf" : action == EX_CSV_CARD ? "csv" : action == EX_XLSX ? "xlsx" : action == EX_SAVE_AS || action == EX_OPEN ? "lrpt" : action == EX_THEME_OPEN || action == EX_THEME_SAVE ? "lrtheme" : "png"; }
/* the dialog's own wake-up event: a raw SDL_EVENT_USER is the first registered type, which belonged to
 * the About (every save / export / open from a report opened the About) */
static Uint32 g_wake_ev;
static void push_wake(void) { if (!g_wake_ev) g_wake_ev = SDL_RegisterEvents(1); SDL_Event e; SDL_zero(e); e.type = g_wake_ev ? g_wake_ev : SDL_EVENT_USER; if (g_wake_ev) SDL_PushEvent(&e); }
static void SDLCALL on_dialog(void *ud, const char *const *files, int filter) {
    (void)ud; (void)filter;
    SDL_LockMutex(g_dlg_mu);
    g_dlg.pending = false;
    if (files && files[0]) { snprintf(g_dlg.path, sizeof g_dlg.path, "%s", files[0]); g_dlg.ready = true; }
    SDL_UnlockMutex(g_dlg_mu);
    push_wake();   /* wakes the main loop */
}
static RvDoc *doc_by_id(const char *id) { for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] && !strcmp(G.docs[i]->r.id, id)) return G.docs[i]; return NULL; }
static bool export_data(App *a, RvDoc *d, const char *path, const char *visual, bool xlsx, char *why, int wn) {
    RptPage *p = &d->r.pages[d->page];
    const ResultSet *sets[40]; const char *names[40]; int n = 0;
    for (int i = 0; i < p->nv && n < 40; i++) {
        RptVisual *v = &p->v[i];
        if (!has_data(v) || (visual && strcmp(v->id, visual))) continue;
        char w2[300]; RvData *x = visual_data(a, d, v, false, w2, sizeof w2);
        if (!x) continue;
        if (x->status == RD_QUEUED || x->status == RD_RUNNING) { snprintf(why, (size_t)wn, "%s", L(d, "Exportação adiada: há cards ainda consultando dados (nenhum arquivo foi gerado)", "Export postponed: cards are still querying (no file was written)")); return false; }
        if (x->status != RD_OK) continue;
        /* catalog cards come back as p / d / v: the exported header uses the business names */
        static ResultSet views[40]; static char *colv[40][16]; static char hdr[40][3][96];
        if (!v->ai_sql && !v->sql && x->rs.ncols <= 16) {
            views[n] = x->rs; views[n].cols = colv[n];
            const SemMetricDef *md = metric_def(v); const SemDataset *ds = dataset_of(v); const SemColumn *dc = ds && v->dim[0] ? sem_column(ds, v->dim) : NULL;
            for (int c = 0; c < x->rs.ncols; c++) {
                const char *cn = x->rs.cols[c] ? x->rs.cols[c] : "";
                if (!strcmp(cn, "p")) { snprintf(hdr[n][0], sizeof hdr[n][0], "%s", L(d, "Período", "Period")); colv[n][c] = hdr[n][0]; }
                else if (!strcmp(cn, "d")) { snprintf(hdr[n][1], sizeof hdr[n][1], "%s", dc && dc->label[0] ? dc->label : v->dim); colv[n][c] = hdr[n][1]; }
                else if (!strcmp(cn, "v")) { snprintf(hdr[n][2], sizeof hdr[n][2], "%s", md ? md->name : v->metric); colv[n][c] = hdr[n][2]; }
                else colv[n][c] = x->rs.cols[c];
            }
            sets[n] = &views[n];
        } else sets[n] = &x->rs;
        names[n] = v->title[0] ? v->title : v->id; n++;
    }
    if (!n) { snprintf(why, (size_t)wn, "%s", L(d, "Nenhum card com dados carregados para exportar", "No card with loaded data to export")); return false; }
    bool ok = xlsx ? xlsx_write_sheets(path, names, sets, n, why, wn) : csv_write_rs(path, sets[0], why, wn);
    if (ok) { G.exports++; audit(a, "export_report", d, xlsx ? "xlsx" : "csv", 0, visual ? "scope=visual" : "scope=page"); LOG_I("report: exported data (%s, %d card(s))", xlsx ? "xlsx" : "csv", n); }
    return ok;
}

/* ---- HTML export: the report as one self-contained web page (copyable text, vector charts, no script) ---- */
typedef struct HB HB;
struct HB { char *b; size_t w, cap; };
/* placeholder mode (the design review, roadmap phase 3): the same page with fake data, for the model */
static bool HFAKE; static unsigned HSEED;
static double fake_num(void) { HSEED = HSEED * 1103515245u + 12345u; return 1000.0 + (double)((HSEED >> 8) % 90000) / 10.0; }
static void fake_label(int i, char *o, size_t n) { snprintf(o, n, "Categoria %c%s", 'A' + i % 26, i >= 26 ? "+" : ""); }
static void hb_put(HB *h, const char *t, size_t n) { if (h->w + n + 1 > h->cap) { while (h->w + n + 1 > h->cap) h->cap = h->cap ? h->cap * 2 : 65536; h->b = realloc(h->b, h->cap); } memcpy(h->b + h->w, t, n); h->w += n; h->b[h->w] = 0; }
static void hb_cat(HB *h, const char *t) { hb_put(h, t, strlen(t)); }
static void hb_fmt(HB *h, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
static void hb_fmt(HB *h, const char *fmt, ...) { char tmp[2048]; va_list ap; va_start(ap, fmt); int n = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap); if (n > 0) hb_put(h, tmp, (size_t)(n < (int)sizeof tmp ? n : (int)sizeof tmp - 1)); }
static void hb_esc(HB *h, const char *t) {   /* text and attribute values: nothing of the data can become markup */
    for (const char *p = t ? t : ""; *p; p++) {
        switch (*p) { case '&': hb_cat(h, "&amp;"); break; case '<': hb_cat(h, "&lt;"); break; case '>': hb_cat(h, "&gt;"); break; case '"': hb_cat(h, "&quot;"); break; case '\'': hb_cat(h, "&#39;"); break; default: hb_put(h, p, 1); }
    }
}
static void hb_b64file(HB *h, const char *path, const char *mime) {
    FILE *f = fopen(path, "rb"); if (!f) return;
    static const char T[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";
    hb_fmt(h, "data:%s;base64,", mime);
    unsigned char in[3]; size_t n; char o[4];
    while ((n = fread(in, 1, 3, f)) > 0) {
        unsigned v = (unsigned)in[0] << 16 | (n > 1 ? (unsigned)in[1] << 8 : 0) | (n > 2 ? in[2] : 0);
        o[0] = T[v >> 18 & 63]; o[1] = T[v >> 12 & 63]; o[2] = n > 1 ? T[v >> 6 & 63] : '='; o[3] = n > 2 ? T[v & 63] : '=';
        hb_put(h, o, 4);
    }
    fclose(f);
}
static const char *img_mime(const char *path) { FILE *f = fopen(path, "rb"); unsigned char hd[8] = { 0 }; size_t g = f ? fread(hd, 1, 8, f) : 0; if (f) fclose(f); const char *k = NULL; if (!image_sniff(hd, g, &k)) return NULL; return !strcmp(k, "PNG") ? "image/png" : "image/jpeg"; }
static void html_xy(HB *h, RvDoc *d, const RptVisual *v, const RvData *x, bool line) {
    Series *sr = &SER; series_of(d, v, x, sr);
    if (HFAKE) { if (sr->n > 24) sr->n = 24; for (int i = 0; i < sr->n; i++) { sr->v[i] = fake_num(); sr->nul[i] = false; if (v->grain < 0) fake_label(i, sr->lab[i], sizeof sr->lab[i]); } }
    if (!sr->n) { hb_fmt(h, "<p class=\"muted\">%s</p>", L(d, "Sem dados no período e filtros escolhidos.", "No data for the chosen period and filters.")); return; }
    double lo = 0, hi = 0; bool any = false;
    for (int i = 0; i < sr->n; i++) if (!sr->nul[i]) { if (!any || sr->v[i] > hi) hi = sr->v[i]; if (!any || sr->v[i] < lo) lo = sr->v[i]; any = true; }
    if (lo > 0) lo = 0;
    double t[12]; int nt = ticks(lo, hi, t, 12); lo = t[0]; hi = t[nt - 1]; if (hi <= lo) hi = lo + 1;
    bool horiz = !line && v->dim[0] && sr->n > 6;
    char lb[64], vl[64];
    if (horiz) {   /* many categories: horizontal bars, as on screen */
        float W = 600, labw = 150, rowh = 22, H = rowh * sr->n + 8, pw = W - labw - 90;
        hb_fmt(h, "<svg class=\"chart\" viewBox=\"0 0 %.0f %.0f\" role=\"img\">", W, H);
        for (int i = 0; i < sr->n; i++) {
            float y = 4 + rowh * i;
            hb_fmt(h, "<text class=\"axis\" x=\"%.0f\" y=\"%.1f\" text-anchor=\"end\">", labw - 6, y + rowh * 0.68f); hb_esc(h, sr->lab[i]); hb_cat(h, "</text>");
            if (sr->nul[i]) { hb_fmt(h, "<text class=\"axis\" x=\"%.0f\" y=\"%.1f\">%s</text>", labw + 4, y + rowh * 0.68f, L(d, "sem dado", "no data")); continue; }
            float w = (float)(sr->v[i] / (hi - lo)) * pw; if (w < 1 && sr->v[i] > 0) w = 1; if (w < 0) w = 0;
            fmt_v(d, v, sr->v[i], false, vl, sizeof vl);
            hb_fmt(h, "<rect class=\"s0\" x=\"%.0f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\"><title>", labw, y + rowh * 0.15f, w, rowh * 0.7f); hb_esc(h, sr->lab[i]); hb_cat(h, ": "); hb_esc(h, vl); hb_cat(h, "</title></rect>");
            hb_fmt(h, "<text class=\"val\" x=\"%.1f\" y=\"%.1f\">", labw + w + 4, y + rowh * 0.68f); hb_esc(h, vl); hb_cat(h, "</text>");
        }
        hb_cat(h, "</svg>");
        return;
    }
    float W = 640, H = 260, L0 = 64, T0 = 8, PW = W - L0 - 8, PH = H - T0 - 28;
    hb_fmt(h, "<svg class=\"chart\" viewBox=\"0 0 %.0f %.0f\" role=\"img\">", W, H);
    for (int i = 0; i < nt; i++) {
        float y = T0 + PH - (float)((t[i] - lo) / (hi - lo)) * PH;
        hb_fmt(h, "<line class=\"%s\" x1=\"%.0f\" x2=\"%.0f\" y1=\"%.1f\" y2=\"%.1f\"/>", t[i] == 0 ? "zero" : "grid", L0, L0 + PW, y, y);
        fmt_axis(d, v, t[i], lb, sizeof lb);
        hb_fmt(h, "<text class=\"axis\" x=\"%.0f\" y=\"%.1f\" text-anchor=\"end\">", L0 - 6, y + 4); hb_esc(h, lb); hb_cat(h, "</text>");
    }
    float step = PW / (float)sr->n; int every = (int)ceilf(60.0f / fmaxf(step, 1)); if (every < 1) every = 1;
    float zero = T0 + PH - (float)((0 - lo) / (hi - lo)) * PH;
    HB path = { 0 }; bool pen = false;
    for (int i = 0; i < sr->n; i++) {
        float cx = L0 + step * (i + 0.5f);
        if (i % every == 0) { hb_fmt(h, "<text class=\"axis\" x=\"%.1f\" y=\"%.0f\" text-anchor=\"middle\">", cx, H - 8); hb_esc(h, sr->lab[i]); hb_cat(h, "</text>"); }
        if (sr->nul[i]) { pen = false; continue; }   /* a gap, never a zero */
        float y = T0 + PH - (float)((sr->v[i] - lo) / (hi - lo)) * PH;
        fmt_v(d, v, sr->v[i], false, vl, sizeof vl);
        if (line) { hb_fmt(&path, "%c%.1f %.1f ", pen ? 'L' : 'M', cx, y); pen = true; }
        else { float bw = fmaxf(2, step * 0.7f); hb_fmt(h, "<rect class=\"s0\" x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\"><title>", cx - bw / 2, sr->v[i] >= 0 ? y : zero, bw, fabsf(zero - y)); hb_esc(h, sr->lab[i]); hb_cat(h, ": "); hb_esc(h, vl); hb_cat(h, "</title></rect>"); }
    }
    if (line && path.b) { hb_cat(h, "<path class=\"ln\" d=\""); hb_cat(h, path.b); hb_cat(h, "\"/>"); }
    free(path.b);
    if (line) for (int i = 0; i < sr->n; i++) {
        if (sr->nul[i]) continue;
        float cx = L0 + step * (i + 0.5f), y = T0 + PH - (float)((sr->v[i] - lo) / (hi - lo)) * PH;
        fmt_v(d, v, sr->v[i], false, vl, sizeof vl);
        hb_fmt(h, "<circle class=\"pt\" cx=\"%.1f\" cy=\"%.1f\" r=\"4\"><title>", cx, y); hb_esc(h, sr->lab[i]); hb_cat(h, ": "); hb_esc(h, vl); hb_cat(h, "</title></circle>");
    }
    hb_cat(h, "</svg>");
    /* the same values as a table that can be copied, folded under the chart */
    hb_fmt(h, "<details><summary>%s</summary><table><tbody>", L(d, "Valores", "Values"));
    for (int i = 0; i < sr->n; i++) { fmt_v(d, v, sr->v[i], sr->nul[i], vl, sizeof vl); hb_cat(h, "<tr><td>"); hb_esc(h, sr->lab[i]); hb_cat(h, "</td><td class=\"num\">"); hb_esc(h, vl); hb_cat(h, "</td></tr>"); }
    hb_cat(h, "</tbody></table></details>");
}
static void html_table(HB *h, RvDoc *d, const RptVisual *v, const RvData *x) {
    const ResultSet *rs = &x->rs; int nc = rs->ncols; if (nc < 1) return;
    const SemMetricDef *md = v->sql ? NULL : metric_def(v);
    hb_cat(h, "<div class=\"tw\"><table><thead><tr>");
    for (int c = 0; c < nc; c++) {
        const char *name = rs->cols[c] ? rs->cols[c] : ""; char lab[96];
        if (!v->sql && c == nc - 1) snprintf(lab, sizeof lab, "%s", md ? md->name : name);
        else if (!v->sql && v->grain >= 0 && c == 0) snprintf(lab, sizeof lab, "%s", L(d, "Período", "Period"));
        else if (!v->sql) { const SemDataset *ds = dataset_of(v); const SemColumn *dc = ds && v->dim[0] ? sem_column(ds, v->dim) : NULL; snprintf(lab, sizeof lab, "%s", dc && dc->label[0] ? dc->label : name); }
        else snprintf(lab, sizeof lab, "%s", name);
        hb_fmt(h, "<th%s>", !v->sql && c == nc - 1 ? " class=\"num\"" : ""); hb_esc(h, lab); hb_cat(h, "</th>");
    }
    hb_cat(h, "</tr></thead><tbody>");
    int n = rs->nrows < MAX_ROWS ? rs->nrows : MAX_ROWS;
    if (HFAKE && n > 6) n = 6;
    for (int i = 0; i < n; i++) {
        hb_cat(h, "<tr>");
        for (int c = 0; c < nc; c++) {
            const char *cell = rs_cell(rs, i, c); char o[128];
            bool numcol = !v->sql && c == nc - 1;
            if (HFAKE) { double dv; if (numcol || (cell && cell_num(rs, i, c, &dv))) fmt_v(d, v, fake_num(), false, o, sizeof o); else if (!v->sql && v->grain >= 0 && c == 0 && cell) rpt_fmt_period(cell, v->grain, d->r.locale, o, sizeof o); else fake_label(i, o, sizeof o); }
            else if (numcol) { double val; bool ok = cell_num(rs, i, c, &val); fmt_v(d, v, val, !ok, o, sizeof o); }
            else if (!v->sql && v->grain >= 0 && c == 0 && cell) rpt_fmt_period(cell, v->grain, d->r.locale, o, sizeof o);
            else snprintf(o, sizeof o, "%s", cell ? cell : "NULL");
            hb_fmt(h, "<td%s>", numcol ? " class=\"num\"" : !cell ? " class=\"muted\"" : ""); hb_esc(h, o); hb_cat(h, "</td>");
        }
        hb_cat(h, "</tr>");
    }
    hb_cat(h, "</tbody></table></div><p class=\"foot\">");
    char foot[200];
    if (rs->truncated && !rs->complete) snprintf(foot, sizeof foot, L(d, "%d linhas carregadas (limite %d); total não calculado", "%d rows loaded (limit %d); total not computed"), rs->nrows, MAX_ROWS);
    else snprintf(foot, sizeof foot, L(d, "%d linhas (total)", "%d rows (total)"), rs->nrows);
    hb_esc(h, foot); hb_cat(h, "</p>");
}
static void html_kpi(HB *h, RvDoc *d, RptVisual *v, const RvData *x, const RTheme *t) {
    double cur = 0; bool cn = x->rs.nrows < 1 || !cell_num(&x->rs, 0, x->rs.ncols - 1, &cur);
    if (HFAKE) { cur = fake_num() * 10; cn = false; }
    char val[96]; fmt_v(d, v, cur, cn, val, sizeof val);
    hb_cat(h, "<div class=\"kpi\">"); hb_esc(h, val); hb_cat(h, "</div>");
    if (v->compare == RC_NONE) return;
    char w2[300]; RvData *bx = visual_data(G.a, d, v, true, w2, sizeof w2);
    const char *cmpname = v->compare == RC_YOY ? L(d, "vs mesmo período do ano anterior", "vs same period last year") : L(d, "vs período anterior", "vs previous period");
    if (!bx || bx->status != RD_OK) { hb_fmt(h, "<p class=\"muted\">%s</p>", L(d, "Comparação indisponível", "Comparison unavailable")); return; }
    double base = 0; bool bn = bx->rs.nrows < 1 || !cell_num(&bx->rs, 0, bx->rs.ncols - 1, &base);
    if (HFAKE) { base = cur * 0.9; bn = false; }
    double ab, rel; bool aok, rok; rpt_delta(cur, cn, base, bn, &ab, &rel, &aok, &rok);
    const SemMetricDef *md = metric_def(v);
    char bs[96]; fmt_v(d, v, base, bn, bs, sizeof bs);
    if (rok) {
        uint32_t col = t->muted; if (md && md->direction) col = (rel > 0) == (md->direction > 0) ? t->pos : t->neg;
        char pc[48]; rpt_fmt_value(rel, false, NULL, RF_PERCENT, 1, d->r.locale, pc, sizeof pc);
        hb_fmt(h, "<div class=\"delta\" style=\"color:#%06X\">%s %s%s</div>", col, rel < 0 ? "&#9660;" : rel > 0 ? "&#9650;" : "", rel > 0 ? "+" : "", pc);
        hb_cat(h, "<div class=\"muted\">"); hb_esc(h, cmpname); hb_cat(h, "</div>");
    } else hb_fmt(h, "<div class=\"muted\">%s</div>", L(d, "Sem variação calculável", "No computable change"));
    hb_cat(h, "<div class=\"muted\">"); hb_esc(h, L(d, "Base: ", "Base: ")); hb_esc(h, bs); hb_cat(h, "</div>");
}
static bool html_build(App *a, RvDoc *d, bool fake, HB *out, char *why, int wn);
static bool export_html(App *a, RvDoc *d, const char *path, char *why, int wn) {
    HB h = { 0 };
    if (!html_build(a, d, false, &h, why, wn)) { free(h.b); return false; }
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.part", path);
    FILE *f = fopen(tmp, "wb"); bool ok = f && fwrite(h.b, 1, h.w, f) == h.w; if (f) ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); free(h.b); snprintf(why, (size_t)wn, "%s", L(d, "Não foi possível gravar o arquivo", "The file could not be written")); return false; }
    G.exports++; audit(a, "export_report", d, "html", h.w, "scope=report");
    LOG_I("report: exported as HTML (%zu bytes, %d page(s))", h.w, d->r.np);
    free(h.b);
    return true;
}
static bool html_build(App *a, RvDoc *d, bool fake, HB *out, char *why, int wn) {
    HFAKE = fake; HSEED = 20261101u;
    theme_resolve(d);
    const RTheme *t = &d->rt;
    /* every card's data must be there (other pages' cards are queried now and the export waits) */
    int pending = 0;
    for (int pg = 0; pg < d->r.np; pg++) for (int i = 0; i < d->r.pages[pg].nv; i++) {
        RptVisual *v = &d->r.pages[pg].v[i]; if (!has_data(v)) continue;
        int keep = d->page; d->page = pg; char w2[300]; RvData *x = visual_data(a, d, v, false, w2, sizeof w2); d->page = keep;
        if (x && (x->status == RD_QUEUED || x->status == RD_RUNNING)) pending++;
    }
    if (pending) { snprintf(why, (size_t)wn, L(d, "Exportação adiada: %d card(s) ainda consultando dados (nenhum arquivo foi gerado); tente de novo em instantes", "Export postponed: %d card(s) still querying (no file written); try again in a moment"), pending); return false; }
    HB h = { 0 };
    hb_cat(&h, "<!doctype html>\n<html lang=\"");
    if (fake) hb_cat(&h, "<!-- PLACEHOLDER DATA: every number and category label in this page is fake; layout, titles and theme are the report's own -->\n"); hb_cat(&h, pt_locale(d) ? "pt-BR" : "en"); hb_cat(&h, "\"><head><meta charset=\"utf-8\">\n");
    hb_cat(&h, "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; img-src data:; style-src 'unsafe-inline'\">\n");
    hb_cat(&h, "<meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n<meta name=\"generator\" content=\"Lume Data\">\n<title>"); hb_esc(&h, d->r.title); hb_cat(&h, "</title>\n<style>\n");
    hb_fmt(&h, ":root{--bg:#%06X;--surface:#%06X;--border:#%06X;--text:#%06X;--muted:#%06X;--grid:#%06X;--accent:#%06X;--s0:#%06X;--hbg:#%06X;--htx:#%06X;--r:%.0fpx}\n",
           t->bg, t->surface, t->border, t->text, t->muted, t->grid, t->accent, t->nseries ? t->series[0] : t->accent, t->header_bg, t->header_text, t->radius);
    hb_cat(&h, "*{box-sizing:border-box}body{margin:0;background:var(--bg);color:var(--text);font:14px/1.45 -apple-system,BlinkMacSystemFont,'Segoe UI',Inter,Roboto,Helvetica,Arial,sans-serif}\n"
               ".page{max-width:1280px;margin:0 auto;padding:16px 20px 28px}.hdr{display:flex;align-items:center;gap:16px;background:var(--hbg);color:var(--htx);padding:16px 20px;border-radius:var(--r)}\n"
               ".hdr img{max-height:48px}.hdr h1{margin:0;font-size:22px}.hdr .sub{opacity:.8;font-size:13px}.prov{color:var(--muted);font-size:12px;margin:10px 2px 14px}\n"
               ".grid{display:grid;grid-template-columns:repeat(12,1fr);grid-auto-rows:44px;gap:12px}.card{background:var(--surface);border-radius:var(--r);padding:12px 14px;overflow:hidden;display:flex;flex-direction:column}\n"
               ".card.shadow{box-shadow:0 1px 3px rgba(0,0,0,.12)}.card.border{border:1px solid var(--border)}.card.accent-top{border-top:4px solid var(--accent)}.card.flat{}\n"
               ".card h2{margin:0 0 2px;font-size:15px}.card .st{color:var(--muted);font-size:12px;margin-bottom:6px}.kpi{font-size:34px;font-weight:600;letter-spacing:-.5px}.delta{font-weight:600}\n"
               ".muted,.foot{color:var(--muted);font-size:12px}.badge{color:var(--muted);font-size:11px;margin-top:auto;padding-top:6px}.chart{width:100%;height:auto;flex:1}\n"
               ".chart .grid{stroke:var(--grid);stroke-width:1}.chart .zero{stroke:var(--muted);stroke-width:1}.chart .axis{fill:var(--muted);font-size:11px}.chart .val{fill:var(--text);font-size:11px}\n"
               ".chart .s0{fill:var(--s0)}.chart .ln{fill:none;stroke:var(--s0);stroke-width:2}.chart .pt{fill:var(--s0)}.chart rect:hover,.chart .pt:hover{fill:var(--accent)}\n"
               ".tw{overflow:auto;flex:1}table{border-collapse:collapse;width:100%;font-size:13px}th{color:var(--muted);text-align:left;font-weight:500;border-bottom:1px solid var(--grid);padding:4px 6px;position:sticky;top:0;background:var(--surface)}\n"
               "td{border-bottom:1px solid var(--grid);padding:4px 6px}.num{text-align:right;font-variant-numeric:tabular-nums}details{margin-top:4px;font-size:12px}summary{color:var(--muted);cursor:pointer}\n"
               ".text p{margin:0 0 6px}.text ul{margin:0;padding-left:18px}footer{color:var(--muted);font-size:11px;margin-top:18px}\n"
               "@media print{@page{size:A4 landscape;margin:10mm}body{background:#fff}.page{max-width:none;padding:0;break-after:page}.card{break-inside:avoid}.tw{overflow:visible}details{display:none}}\n"
               "</style></head><body>\n");
    static const char *const CS[] = { "flat", "shadow", "border", "accent-top" };
    char lp[1400]; theme_logo_path(d, lp, sizeof lp);
    int keep = d->page;
    for (int pg = 0; pg < d->r.np; pg++) {
        d->page = pg;
        RptPage *p = &d->r.pages[pg];
        hb_cat(&h, "<section class=\"page\">\n<header class=\"hdr\">");
        if (fake && t->header && lp[0]) hb_cat(&h, "<span class=\"muted\">[logo]</span>");
        else if (t->header && lp[0]) { const char *mm = img_mime(lp); if (mm) { hb_cat(&h, "<img alt=\"logo\" src=\""); hb_b64file(&h, lp, mm); hb_cat(&h, "\">"); } }
        hb_cat(&h, "<div><h1>"); hb_esc(&h, d->r.title); hb_cat(&h, "</h1><div class=\"sub\">"); hb_esc(&h, p->name);
        char plabel[120] = ""; SemPeriod pc0, pb0; if (rpt_period(&d->r, RC_NONE, &pc0, &pb0, plabel, sizeof plabel) && plabel[0]) { hb_cat(&h, " &middot; "); hb_esc(&h, plabel); }
        hb_cat(&h, "</div></div></header>\n<p class=\"prov\">");
        char prov[1000]; if (fake) snprintf(prov, sizeof prov, "%s", "(provenance line: period, filters, source, data time)"); else provenance_line(a, d, prov, sizeof prov); hb_esc(&h, prov);
        hb_cat(&h, "</p>\n<main class=\"grid\">\n");
        for (int i = 0; i < p->nv; i++) {
            RptVisual *v = &p->v[i];
            int rows = v->h < 1 ? 1 : v->h;
            hb_fmt(&h, "<article class=\"card %s\" style=\"grid-column:%d / span %d;grid-row:%d / span %d\">", CS[t->card_style >= 0 && t->card_style < 4 ? t->card_style : 1], v->x + 1, v->w < 1 ? 1 : v->w, v->y + 1, rows);
            if (v->kind == RK_SHAPE) { hb_cat(&h, "</article>\n"); continue; }
            { char at[160]; const char *ct = card_title(d, v, at, sizeof at); if (ct[0]) { hb_cat(&h, "<h2>"); hb_esc(&h, ct); hb_cat(&h, "</h2>"); } }
            if (v->subtitle[0]) { hb_cat(&h, "<div class=\"st\">"); hb_esc(&h, v->subtitle); hb_cat(&h, "</div>"); }
            if (v->kind == RK_TEXT) {
                hb_cat(&h, "<div class=\"text\">");
                const char *tx = v->text ? v->text : ""; bool ul = false;
                while (*tx) {
                    const char *e = strchr(tx, '\n'); int len = e ? (int)(e - tx) : (int)strlen(tx);
                    char ln[1200]; snprintf(ln, sizeof ln, "%.*s", len, tx);
                    if (fake) for (char *q = ln; *q; q++) if (isdigit((unsigned char)*q)) *q = '9';
                    if (!strncmp(ln, "- ", 2)) { if (!ul) { hb_cat(&h, "<ul>"); ul = true; } hb_cat(&h, "<li>"); hb_esc(&h, ln + 2); hb_cat(&h, "</li>"); }
                    else { if (ul) { hb_cat(&h, "</ul>"); ul = false; } if (ln[0]) { hb_cat(&h, "<p>"); hb_esc(&h, ln); hb_cat(&h, "</p>"); } }
                    tx += len; if (*tx == '\n') tx++;
                }
                if (ul) hb_cat(&h, "</ul>");
                hb_cat(&h, "</div>");
            } else if (v->kind == RK_IMAGE) {
                char ip[1400] = ""; if (!strcmp(v->src, "theme:logo")) snprintf(ip, sizeof ip, "%s", lp); else if (v->src[0] && !strchr(v->src, '/')) { char dd[1024]; doc_dir(d, dd, sizeof dd); snprintf(ip, sizeof ip, "%s/%s", dd, v->src); }
                const char *mm = ip[0] && !fake ? img_mime(ip) : NULL;
                if (fake && ip[0]) hb_cat(&h, "<span class=\"muted\">[image]</span>");
                if (mm) { hb_cat(&h, "<img alt=\"\" style=\"max-width:100%;max-height:100%;object-fit:contain\" src=\""); hb_b64file(&h, ip, mm); hb_cat(&h, "\">"); }
            } else if (has_data(v)) {
                char w2[300]; RvData *x = visual_data(a, d, v, false, w2, sizeof w2);
                if (!x || x->status != RD_OK) { hb_cat(&h, "<p class=\"muted\">"); hb_esc(&h, x ? x->err : w2); hb_cat(&h, "</p>"); }
                else if (v->kind == RK_KPI) html_kpi(&h, d, v, x, t);
                else if (v->kind == RK_LINE || v->kind == RK_BAR) html_xy(&h, d, v, x, v->kind == RK_LINE);
                else if (v->kind == RK_TABLE) html_table(&h, d, v, x);
                if (v->ai_sql) { hb_cat(&h, "<div class=\"badge\">"); hb_esc(&h, L(d, "SQL da IA · não certificado", "AI-written SQL · not certified")); hb_cat(&h, "</div>"); }
            }
            hb_cat(&h, "</article>\n");
        }
        hb_cat(&h, "</main>\n</section>\n");
    }
    d->page = keep;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm); char ts[40]; strftime(ts, sizeof ts, pt_locale(d) ? "%d/%m/%Y %H:%M" : "%Y-%m-%d %H:%M", &tm);
    hb_cat(&h, "<footer class=\"page\">");
    char ft[400]; snprintf(ft, sizeof ft, L(d, "Gerado pelo Lume Data em %s · relatório rev %d · contém os dados do momento da exportação", "Generated by Lume Data on %s · report rev %d · holds the data of the moment it was exported"), ts, d->r.revision);
    hb_esc(&h, ft); hb_cat(&h, "</footer>\n</body></html>\n");
    *out = h; HFAKE = false;
    return true;
}
static void do_path_action(App *a, RvDoc *d, int action, const char *visual, const char *path0) {
    char path[1100]; snprintf(path, sizeof path, "%s", path0);
    const char *ext = ext_of(action); size_t L0 = strlen(path), el = strlen(ext);
    if (!is_open_action(action) && (L0 < el + 1 || path[L0 - el - 1] != '.' || strcasecmp(path + L0 - el, ext))) snprintf(path + L0, sizeof path - L0, ".%s", ext);   /* the extension of the chosen format */
    char why[300] = ""; bool ok = false;
    switch (action) {
    case EX_OPEN: { RvDoc *nd = open_report(a, path, false); ok = nd != NULL; if (!ok) snprintf(why, sizeof why, "could not open %s", path); break; }
    case EX_THEME_OPEN: {
        if (!d) return;
        RTheme t; ok = rtheme_load_file(&t, path, why, sizeof why);
        if (ok) { char rel[1100], ref[1200]; rel_to_doc(d, path, rel, sizeof rel); snprintf(ref, sizeof ref, "file:%s", rel); theme_set_ref(d, ref); set_msg(d, "%s %s", L(d, "Tema aplicado:", "Theme applied:"), t.name); }
        else set_msg(d, "%s", why);
        if (getenv("LUME_SCRIPT")) { printf("rptpath action=%d ok=%d path=%s why=\"%s\"\n", action, ok, path, ok ? "" : why); fflush(stdout); }
        return;
    }
    case EX_THEME_SAVE: {
        if (!d) return;
        ok = theme_write_file(d, path, why, sizeof why);
        if (ok) { char rel[1100], ref[1200]; rel_to_doc(d, path, rel, sizeof rel); snprintf(ref, sizeof ref, "file:%s", rel); before_change(d); free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_set_ref(d, ref); }
        break;
    }
    case EX_LOGO: ok = d && theme_set_logo(d, path, why, sizeof why); break;
    case EX_ATTACH: ok = chat_attach(path, why, sizeof why); break;
    case EX_IMAGE: {
        if (!d) return;
        FILE *f = fopen(path, "rb"); unsigned char head[8] = { 0 }; size_t got = f ? fread(head, 1, 8, f) : 0; if (f) fclose(f);
        const char *kind = NULL;
        if (!image_sniff(head, got, &kind)) { snprintf(why, sizeof why, "%s", L(d, "A imagem precisa ser PNG ou JPEG", "The image must be a PNG or JPEG")); break; }
        doc_paths(a, d);
        const char *base = strrchr(path, '/'); base = base ? base + 1 : path;
        char dd[1024], dst[1400]; doc_dir(d, dd, sizeof dd); snprintf(dst, sizeof dst, "%s/%s", dd, base);
        if (strcmp(dst, path) && !copy_file(path, dst)) { snprintf(why, sizeof why, "%s", L(d, "Não foi possível copiar a imagem para a pasta do relatório", "The image could not be copied next to the report")); break; }
        before_change(d); RptVisual *nv = rpt_add_visual(&d->r, d->page, RK_IMAGE); if (nv) { snprintf(nv->src, sizeof nv->src, "%s", base); snprintf(d->sel, sizeof d->sel, "%s", nv->id); } after_change(d);
        ok = true; break;
    }
    case EX_SAVE_AS:
        if (!d) return;
        { char old[1100]; snprintf(old, sizeof old, "%s", d->path); bool olduser = d->user_path;
          /* a theme referenced relative to the report (file:x.lrtheme) must still resolve from the new folder */
          char oldtp[1400] = "", olddir[1024]; doc_dir(d, olddir, sizeof olddir);
          bool rel_theme = !strncmp(d->r.theme_ref, "file:", 5) && d->r.theme_ref[5] != '/' && theme_file_path(d, d->r.theme_ref, oldtp, sizeof oldtp);
          d->user_path = true;
          ok = save_doc(a, d, path);
          if (!ok) { snprintf(d->path, sizeof d->path, "%s", old); d->user_path = olduser; }
          char newdir[1024]; doc_dir(d, newdir, sizeof newdir);
          if (ok && rel_theme && strcmp(olddir, newdir)) {
              char rd[1100]; reports_dir(a, rd, sizeof rd);
              const char *base = strrchr(oldtp, '/'); base = base ? base + 1 : oldtp;
              if (!strncmp(oldtp, rd, strlen(rd))) {   /* a theme Lume created in its own folder: it moves with the report, logo included */
                  char np[1400]; snprintf(np, sizeof np, "%s/%s", newdir, base);
                  RTheme t; char tw[200];
                  if (copy_file(oldtp, np)) {
                      if (rtheme_load_file(&t, oldtp, tw, sizeof tw) && t.logo[0] && !strchr(t.logo, '/')) { char ls[1400], ld[1400]; snprintf(ls, sizeof ls, "%s/%s", olddir, t.logo); snprintf(ld, sizeof ld, "%s/%s", newdir, t.logo); copy_file(ls, ld); }
                      char ref[1200]; snprintf(ref, sizeof ref, "file:%s", base); theme_set_ref(d, ref);
                  }
              } else { char ref[1500]; snprintf(ref, sizeof ref, "file:%s", oldtp); theme_set_ref(d, ref); }   /* the team's theme stays where it is */
              save_doc(a, d, d->path);
          } }
        if (getenv("LUME_SCRIPT")) { printf("rptpath action=%d ok=%d path=%s why=\"\"\n", action, ok, path); fflush(stdout); }
        return;   /* save_doc set the message */
    case EX_HTML: ok = d && export_html(a, d, path, why, sizeof why); break;
    case EX_PDF: ok = d && export_image(a, d, path, NULL, 2400, true, why, sizeof why); break;
    case EX_PNG: ok = d && export_image(a, d, path, NULL, 1600, false, why, sizeof why); break;
    case EX_PNG_HI: ok = d && export_image(a, d, path, NULL, 3200, false, why, sizeof why); break;
    case EX_PNG_CARD: ok = d && export_image(a, d, path, visual, 1600, false, why, sizeof why); break;
    case EX_CSV_CARD: ok = d && export_data(a, d, path, visual, false, why, sizeof why); break;
    case EX_XLSX: ok = d && export_data(a, d, path, NULL, true, why, sizeof why); break;
    }
    if (d) { if (ok) set_msg(d, L(d, "Exportado: %s", "Exported: %s"), path); else set_msg(d, "%s", why); }
    if (!ok && !d) app_status(a, "%s", why);
    if (getenv("LUME_SCRIPT")) { printf("rptpath action=%d ok=%d path=%s why=\"%s\"\n", action, ok, path, ok ? "" : why); fflush(stdout); }
}
/* the save dialog for an action; tests set LUME_TEST_SAVE_DIR and get <dir>/<default name> without a dialog */
static void ask_path(App *a, RvDoc *d, int action, const char *visual) {
    char base[300]; snprintf(base, sizeof base, "%s", d ? d->r.title : "report");
    if (d && visual && (action == EX_PNG_CARD || action == EX_CSV_CARD)) { const RptVisual *cv = rpt_find(&d->r, visual, NULL); if (cv) { const SemMetric *mm = cv->metric[0] ? sem_metric_c(&G.cat, cv->metric) : NULL; const SemMetricDef *md = mm ? sem_cur(mm) : NULL; snprintf(base, sizeof base, "%.120s - %.120s", d->r.title, cv->title[0] ? cv->title : md ? md->name : cv->id); } }
    char name[200]; file_name_of(base, ext_of(action), name, sizeof name);
    const char *td = getenv("LUME_TEST_SAVE_DIR");
    if (is_open_action(action) && getenv("LUME_TEST_OPEN_FILE")) { do_path_action(a, d, action, visual, getenv("LUME_TEST_OPEN_FILE")); return; }
    if (td && *td && !is_open_action(action)) { char p[1100]; snprintf(p, sizeof p, "%s/%s", td, name); do_path_action(a, d, action, visual, p); push_wake(); return; }   /* the same wake-up as a real dialog */
    if (!g_dlg_mu) g_dlg_mu = SDL_CreateMutex();
    SDL_LockMutex(g_dlg_mu);
    bool busy = g_dlg.pending;
    if (!busy) { g_dlg.action = action; snprintf(g_dlg.doc_id, sizeof g_dlg.doc_id, "%s", d ? d->r.id : ""); snprintf(g_dlg.visual, sizeof g_dlg.visual, "%s", visual ? visual : ""); g_dlg.ready = false; g_dlg.pending = true; }
    SDL_UnlockMutex(g_dlg_mu);
    if (busy) return;
    static SDL_DialogFileFilter flt[1]; static char fname[24], fext[8];
    snprintf(fext, sizeof fext, "%s", ext_of(action));
    snprintf(fname, sizeof fname, "%s", action == EX_HTML ? "Web page (HTML)" : action == EX_PDF ? "PDF" : action == EX_CSV_CARD ? "CSV" : action == EX_XLSX ? "Excel" : action == EX_SAVE_AS || action == EX_OPEN ? "Lume report" : action == EX_THEME_OPEN || action == EX_THEME_SAVE ? "Lume theme" : action == EX_LOGO || action == EX_IMAGE || action == EX_ATTACH ? "Image" : "PNG image");
    if (action == EX_LOGO || action == EX_IMAGE || action == EX_ATTACH) snprintf(fext, sizeof fext, "png;jpg;jpeg");
    flt[0].name = fname; flt[0].pattern = fext;
    char start[1300];
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    if (action == EX_SAVE_AS && d && d->user_path) snprintf(start, sizeof start, "%s", d->path);
    else snprintf(start, sizeof start, "%s%s", docs ? docs : "", is_open_action(action) ? "" : name);
    if (is_open_action(action)) SDL_ShowOpenFileDialog(on_dialog, NULL, a->window, flt, 1, start, false);
    else SDL_ShowSaveFileDialog(on_dialog, NULL, a->window, flt, 1, start);
}
static void poll_dialog(App *a) {
    if (!g_dlg_mu) return;
    SDL_LockMutex(g_dlg_mu);
    bool ready = g_dlg.ready; int action = g_dlg.action; char id[40], vis[24], path[1024];
    snprintf(id, sizeof id, "%s", g_dlg.doc_id); snprintf(vis, sizeof vis, "%s", g_dlg.visual); snprintf(path, sizeof path, "%s", g_dlg.path);
    g_dlg.ready = false;
    SDL_UnlockMutex(g_dlg_mu);
    if (ready) do_path_action(a, id[0] ? doc_by_id(id) : NULL, action, vis[0] ? vis : NULL, path);
}
/* Save: the first time a new report is saved, ask where and under which name (tests keep the default) */
static void ui_save(App *a, RvDoc *d) {
    if (!d || d->read_mode) return;
    if (!d->user_path && !getenv("LUME_SCRIPT")) { ask_path(a, d, EX_SAVE_AS, NULL); return; }
    if (!d->user_path && getenv("LUME_TEST_SAVE_DIR")) { ask_path(a, d, EX_SAVE_AS, NULL); return; }
    save_doc(a, d, NULL);
}
static void set_visual_kind(RvDoc *d, RptVisual *v, int kind) {
    v->kind = kind; snprintf(v->kind_name, sizeof v->kind_name, "%s", rpt_kind_name(kind));
    if (kind == RK_KPI) { v->grain = -1; v->dim[0] = 0; }
    else { v->compare = RC_NONE; if (kind == RK_LINE) { v->dim[0] = 0; if (v->grain < 0) v->grain = SG_MONTH; } else if ((kind == RK_BAR || kind == RK_TABLE) && v->grain < 0 && !v->dim[0]) v->grain = SG_MONTH; }
    int mw, mh; rpt_min_size(kind, &mw, &mh); if (v->w < mw) v->w = mw; if (v->h < mh) v->h = mh;
    (void)d;
}
static void menu_choose(App *a, RvDoc *d, int kind, int id) {
    int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg);
    switch (kind) {
    case MK_ADD:
        before_change(d);
        if (id == 6) { RptPage *p = rpt_add_page(&d->r, NULL); if (p) d->page = d->r.np - 1; after_change(d); break; }
        { RptVisual *nv = rpt_add_visual(&d->r, d->page, id == 1 ? RK_KPI : id == 2 ? RK_LINE : id == 3 ? RK_BAR : id == 4 ? RK_TABLE : RK_TEXT);
          if (nv && nv->kind == RK_TEXT) rpt_visual_set_text(nv, L(d, "Título da seção\n- Nota metodológica", "Section title\n- Methodology note"));
          if (nv && nv->kind == RK_BAR) { nv->grain = SG_MONTH; }
          if (nv && nv->kind == RK_TABLE) { nv->grain = SG_MONTH; }
          if (nv && nv->kind != RK_TEXT && G.cat.nm) { snprintf(nv->metric, sizeof nv->metric, "%s", G.cat.m[0].id); nv->metric_version = sem_cur(&G.cat.m[0])->version; }
          if (nv) snprintf(d->sel, sizeof d->sel, "%s", nv->id); }
        after_change(d); break;
    case MK_THEME:
        if (id >= 1 && id <= 3) theme_set_ref(d, id == 1 ? "builtin:light" : id == 2 ? "builtin:dark" : "builtin:executive");
        else if (id == 10) theme_set_ref(d, "workspace");
        else if (id == 11) ask_path(a, d, EX_THEME_OPEN, NULL);
        else if (id == 12) ask_path(a, d, EX_THEME_SAVE, NULL);
        else if (id == 13) { char wp[1200], why[300]; if (ws_theme_path(wp, sizeof wp) && theme_write_file(d, wp, why, sizeof why)) { before_change(d); free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_set_ref(d, "workspace"); set_msg(d, "%s", L(d, "Tema do workspace atualizado: os relatórios que o usam mudam juntos", "Workspace theme updated: the reports using it change together")); } else set_msg(d, "%s", why); }
        else if (id == 14) { theme_resolve(d); char *js = rtheme_to_json(&d->rt); before_change(d); free(d->r.theme_snapshot); d->r.theme_snapshot = js; free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_set_ref(d, "snapshot"); }
        else if (id == 15) ask_path(a, d, EX_LOGO, NULL);
        else if (id == 16) { before_change(d); RptVisual *nv = rpt_add_visual(&d->r, d->page, RK_IMAGE); if (nv) { snprintf(nv->src, sizeof nv->src, "theme:logo"); snprintf(d->sel, sizeof d->sel, "%s", nv->id); } after_change(d); }
        else if (id == 17) ask_path(a, d, EX_IMAGE, NULL);
        break;
    case MK_PERIOD: { static const char *const k[] = { "last_12_months", "ytd", "last_30_days", "previous_year", "all" }; before_change(d); snprintf(d->r.period, sizeof d->r.period, "%s", k[id - 1]); after_change(d); break; }
    case MK_METRIC: if (v && id >= 100) { before_change(d); const SemMetric *m = &G.cat.m[id - 100]; snprintf(v->metric, sizeof v->metric, "%s", m->id); v->metric_version = sem_cur(m)->version; const SemDataset *ds = dataset_of(v); if (v->dim[0] && !sem_is_dimension(ds, v->dim)) v->dim[0] = 0; if (v->grain >= 0 && !(sem_cur(m)->grains & (1u << v->grain))) v->grain = -1; after_change(d); } break;
    case MK_DIM: if (v) { before_change(d); const SemDataset *ds = dataset_of(v); if (id == 99) v->dim[0] = 0; else if (ds && id - 100 < ds->ncols) { snprintf(v->dim, sizeof v->dim, "%s", ds->cols[id - 100].name); if (v->kind == RK_BAR) v->grain = -1; if (v->kind == RK_TABLE) v->grain = -1; } after_change(d); } break;
    case MK_GRAIN: if (v) { before_change(d); v->grain = id == 99 ? -1 : id - 100; if (v->grain >= 0 && v->kind == RK_BAR) v->dim[0] = 0; after_change(d); } break;
    case MK_COMPARE: if (v) { before_change(d); v->compare = id - 1; after_change(d); } break;
    case MK_FORMAT: if (v) { before_change(d); v->fmt = id - 1; after_change(d); } break;
    case MK_EXPORT: ask_path(a, d, id == 7 ? EX_HTML : id == 1 ? EX_PDF : id == 2 ? EX_PNG : id == 3 ? EX_PNG_HI : id == 4 ? EX_PNG_CARD : id == 5 ? EX_CSV_CARD : EX_XLSX, v ? v->id : NULL); break;
    case MK_SAVE: if (id == 2) ask_path(a, d, EX_SAVE_AS, NULL); else ui_save(a, d); break;
    case MK_MORE: tb_act(a, d, id, G.more_x, G.more_y); break;
    case MK_PARAM: {
        RptParam *pm = G.menu_param >= 0 && G.menu_param < d->r.nparams ? &d->r.params[G.menu_param] : NULL;
        if (!pm) break;
        before_change(d);
        if (id == 1000) pm->value[0] = 0; else if (id > 1000 && id - 1001 < POPUP_MAX) snprintf(pm->value, sizeof pm->value, "%s", G.menu_lab[id - 1001]);
        after_change(d);
        audit(a, "report_filter", d, pm->value[0] ? "set" : "all", 0, "");
        break;
    }
    case MK_FILTER: {
        before_change(d);
        RptFilter *f = NULL; for (int i = 0; i < d->r.nf; i++) if (!strcmp(d->r.filters[i].page, d->r.pages[d->page].id)) f = &d->r.filters[i];
        if (id == 99) { if (f) { *f = d->r.filters[--d->r.nf]; } }
        else {
            const SemDataset *ds = NULL; for (int i = 0; i < d->r.pages[d->page].nv && !ds; i++) ds = dataset_of(&d->r.pages[d->page].v[i]);
            if (ds && id - 100 < ds->ncols) { if (!f && d->r.nf < RPT_MAX_FILTERS) { f = &d->r.filters[d->r.nf++]; memset(f, 0, sizeof *f); snprintf(f->id, sizeof f->id, "f%d", d->r.nf); snprintf(f->page, sizeof f->page, "%s", d->r.pages[d->page].id); } if (f) snprintf(f->field, sizeof f->field, "%s", ds->cols[id - 100].name); }
        }
        after_change(d); break; }
    }
}

/* ---- text fields bound to the selection (committed while typing, one undo step per pause) ---- */
static double last_commit; static TextField *last_field;
static void commit_field(RvDoc *d, TextField *f, char *dst, size_t n) {
    if (!strcmp(f->buf, dst)) return;
    if (last_field != f || now_s() - last_commit > 1.5) before_change(d);
    snprintf(dst, n, "%s", f->buf); after_change(d);
    last_commit = now_s(); last_field = f;
}
static int focus_id; /* 1 title, 2 subtitle, 3 text, 4 report title, 5 page name, 6 filter value, 7 assistant question */
/* ---- theme editing ---- */
static void theme_dirty(RvDoc *d) { d->rt_key[0] = 0; }
static bool ws_theme_path(char *out, size_t n) {
    const char *wd = G.a && G.a->toolbar ? toolbar_workspace_dir(G.a->toolbar) : "";
    if (!wd || !wd[0]) return false;
    snprintf(out, n, "%stheme.lrtheme", wd); return true;
}
static bool file_exists_(const char *p) { struct stat st; return stat(p, &st) == 0 && S_ISREG(st.st_mode); }
/* the theme without the report's overrides */
static void theme_base(RvDoc *d, RTheme *out) {
    char *ov = d->r.theme_overrides; d->r.theme_overrides = NULL; theme_dirty(d);
    theme_resolve(d); *out = d->rt;
    d->r.theme_overrides = ov; theme_dirty(d); theme_resolve(d);
}
/* a change on top of the theme, kept as the minimal overrides (a locked theme takes none) */
static bool theme_change(RvDoc *d, void (*fn)(RTheme *, const void *), const void *arg) {
    theme_resolve(d);
    if (d->rt.locked) { set_msg(d, "%s", L(d, "Tema travado pela marca: ajustes no relatório não são permitidos", "Theme locked by the brand: reports cannot change it")); return false; }
    RTheme base; theme_base(d, &base);
    RTheme mod = d->rt; fn(&mod, arg);
    char why[300];
    if (!rtheme_check_contrast(&mod, why, sizeof why)) { set_msg(d, "%s: %s", L(d, "Ajuste recusado", "Change refused"), why); return false; }
    char *diff = rtheme_diff_json(&base, &mod);
    before_change(d);
    free(d->r.theme_overrides); d->r.theme_overrides = diff;
    after_change(d); theme_dirty(d);
    return true;
}
static void tc_accent(RTheme *t, const void *a) { t->accent = *(const uint32_t *)a; t->series[0] = t->accent; }
static void tc_header(RTheme *t, const void *a) { t->header = *(const bool *)a; }
static void tc_header_bg(RTheme *t, const void *a) { t->header_bg = *(const uint32_t *)a; t->header_text = rtheme_contrast(0xFFFFFF, t->header_bg) >= rtheme_contrast(0x111111, t->header_bg) ? 0xFFFFFF : 0x111111; }
static void tc_card(RTheme *t, const void *a) { t->card_style = *(const int *)a; }
static void tc_kpi(RTheme *t, const void *a) { t->kpi_style = *(const int *)a; }
static void tc_radius(RTheme *t, const void *a) { t->radius = *(const float *)a; }
static void tc_logo_pos(RTheme *t, const void *a) { t->logo_pos = *(const int *)a; }
static void theme_set_ref(RvDoc *d, const char *ref) {
    before_change(d);
    snprintf(d->r.theme_ref, sizeof d->r.theme_ref, "%s", ref);
    if (!strncmp(ref, "builtin:", 8)) { snprintf(d->r.theme, sizeof d->r.theme, "%s", ref + 8); for (int i = 0; i < d->r.np; i++) d->r.pages[i].theme[0] = 0; }
    after_change(d); theme_dirty(d); theme_resolve(d);
}
/* a path relative to the report's folder when the file is inside it */
static void rel_to_doc(const RvDoc *d, const char *abs, char *out, size_t n) {
    char dd[1024]; doc_dir(d, dd, sizeof dd); size_t L = strlen(dd);
    if (!strncmp(abs, dd, L) && abs[L] == '/') snprintf(out, n, "%s", abs + L + 1); else snprintf(out, n, "%s", abs);
}
static bool copy_file(const char *src, const char *dst) {
    FILE *a = fopen(src, "rb"); if (!a) return false;
    FILE *b = fopen(dst, "wb"); if (!b) { fclose(a); return false; }
    char buf[65536]; size_t k; bool ok = true;
    while ((k = fread(buf, 1, sizeof buf, a)) > 0) if (fwrite(buf, 1, k, b) != k) { ok = false; break; }
    fclose(a); ok = (fclose(b) == 0) && ok; return ok;
}
/* writes the resolved theme (overrides folded in) as a theme file, with its logo next to it */
static bool theme_write_file(RvDoc *d, const char *path, char *why, int wn) {
    theme_resolve(d);
    RTheme t = d->rt;
    char lp[1400]; theme_logo_path(d, lp, sizeof lp);
    if (lp[0]) {
        char dst[1400]; snprintf(dst, sizeof dst, "%s", path); char *sl = strrchr(dst, '/'); if (sl) sl[1] = 0; else dst[0] = 0;
        size_t L = strlen(dst); snprintf(dst + L, sizeof dst - L, "%s", t.logo);
        if (strcmp(dst, lp)) copy_file(lp, dst);
    }
    if (!t.id[0] || rtheme_is_builtin(t.id)) snprintf(t.id, sizeof t.id, "custom");
    return rtheme_save_file(&t, path, why, wn);
}
/* the theme's logo from an image file: copied next to the theme file, shown in the header */
static bool theme_set_logo(RvDoc *d, const char *img, char *why, int wn) {
    theme_resolve(d);
    char tp[1400]; const char *ref = d->r.theme_ref;
    if (!theme_file_path(d, ref, tp, sizeof tp)) { snprintf(why, (size_t)wn, "%s", L(d, "Salve o tema como arquivo (ou use o do workspace) para ter um logo", "Save the theme as a file (or use the workspace's) to give it a logo")); return false; }
    if (d->rt.locked) { snprintf(why, (size_t)wn, "%s", L(d, "Tema travado: o logo é definido pela marca", "Locked theme: the logo is set by the brand")); return false; }
    FILE *f = fopen(img, "rb"); unsigned char head[8] = { 0 }; size_t got = f ? fread(head, 1, 8, f) : 0; if (f) fclose(f);
    const char *kind = NULL;
    if (!image_sniff(head, got, &kind)) { snprintf(why, (size_t)wn, "%s", L(d, "O logo precisa ser PNG ou JPEG", "The logo must be a PNG or JPEG")); return false; }
    char dst[1400]; snprintf(dst, sizeof dst, "%s", tp); char *sl = strrchr(dst, '/'); if (sl) sl[1] = 0; else dst[0] = 0;
    char name[32]; snprintf(name, sizeof name, "logo.%s", !strcmp(kind, "PNG") ? "png" : "jpg");
    size_t L = strlen(dst); snprintf(dst + L, sizeof dst - L, "%s", name);
    if (strcmp(dst, img) && !copy_file(img, dst)) { snprintf(why, (size_t)wn, "%s", L(d, "Não foi possível copiar o logo para a pasta do tema", "The logo could not be copied next to the theme")); return false; }
    RTheme t; if (!rtheme_load_file(&t, tp, why, wn)) return false;
    snprintf(t.logo, sizeof t.logo, "%s", name); t.header = true;
    if (!rtheme_save_file(&t, tp, why, wn)) return false;
    theme_dirty(d); theme_resolve(d);
    return true;
}
static const char *theme_source_label(RvDoc *d, char *buf, size_t n) {
    const char *ref = d->r.theme_ref;
    if (!strcmp(ref, "workspace")) snprintf(buf, n, "%s", L(d, "Tema do workspace (Brand Kit)", "Workspace theme (Brand Kit)"));
    else if (!strncmp(ref, "file:", 5)) { const char *b = strrchr(ref + 5, '/'); snprintf(buf, n, "%s %s", L(d, "Arquivo", "File"), b ? b + 1 : ref + 5); }
    else if (!strcmp(ref, "snapshot")) snprintf(buf, n, "%s", L(d, "Cópia guardada no relatório", "Copy kept in the report"));
    else snprintf(buf, n, "%s (%s)", L(d, "Embutido", "Built-in"), d->rt.name);
    return buf;
}
/* ---- the selected card's query: SQL (editable for SQL cards, compiled and read-only for catalog
 * cards), its last run (rows, time, the database's error), and the actions around it ---- */
static void ai_send(App *a, RvDoc *d);
/* a one-line query (the assistant's usually are) shown one clause per line, indented; only
 * whitespace changes (the formatter's own token check), anything it refuses is shown as written */
static void pretty_sql(const char *sql, int dialect, char *out, size_t n) {
    snprintf(out, n, "%s", sql);
    if (strchr(sql, '\n') || strlen(sql) < 60) return;
    SqlStyle st; sql_style_default(&st); FmtResult fr; memset(&fr, 0, sizeof fr);
    if (sql_format(sql, (int)strlen(sql), dialect, &st, -1, -1, &fr) && !fr.refused && fr.text && (size_t)fr.len < n) snprintf(out, n, "%s", fr.text);
    fmt_free(&fr);
}
static void card_sql_apply(App *a, RvDoc *d, RptVisual *v) {
    if (!G.ced || !v->sql) return;
    int len = 0; char *t = editor_text(G.ced, &len);
    while (len > 0 && (t[len - 1] == ' ' || t[len - 1] == '\n' || t[len - 1] == ';' || t[len - 1] == '\t' || t[len - 1] == '\r')) t[--len] = 0;
    if (!strcmp(t, v->sql) || sql_same_tokens(t, len, v->sql, (int)strlen(v->sql), dialect_of_conn(a, v->conn))) { free(t); set_msg(d, "%s", L(d, "Nada mudou na consulta", "The query did not change")); return; }
    char why[300];
    if (len == 0 || len > RPT_AI_SQL_MAX) { free(t); set_msg(d, "%s", L(d, "Consulta vazia ou longa demais", "The query is empty or too long")); return; }
    if (!sqlguard_check(t, dialect_of_conn(a, v->conn), why, sizeof why)) { set_msg(d, "%s: %s", L(d, "Recusado pela política somente-leitura", "Refused by the read-only policy"), why); free(t); return; }
    before_change(d);
    free(v->sql); v->sql = t;
    if (v->ai_sql) v->sql_edited = true;
    after_change(d);
    snprintf(G.ced_orig, sizeof G.ced_orig, "%s", v->sql);
    audit(a, "card_sql", d, "edited", strlen(v->sql), "");
    set_msg(d, "%s", L(d, "Consulta aplicada: o card está atualizando", "Query applied: the card is refreshing"));
    app_wake_in(a, 16);
}
static void card_query_section(App *a, RvDoc *d, RptVisual *v, float x, float *py, float w) {
    SDL_Renderer *r = a->renderer;
    float y = *py, rh = px(a, 26), lh = px(a, 16);
    char sql[RPT_AI_SQL_MAX + 1] = "", conn[16] = "", why[300] = "";
    bool editable = v->sql && !v->locked;
    if (v->sql) { snprintf(sql, sizeof sql, "%s", v->sql); snprintf(conn, sizeof conn, "%s", v->conn); }
    else if (!visual_sql(a, d, v, false, sql, sizeof sql, conn, why, sizeof why)) sql[0] = 0;
    /* (re)bind the editor to this card */
    if (!G.ced) { G.ced = editor_create(); editor_set_strip(G.ced, false); }
    if (strcmp(G.ced_doc, d->r.id) || strcmp(G.ced_vid, v->id) || (!editable && strcmp(G.ced_orig, sql)) || G.ced_ro != !editable) {
        { char shown[RPT_AI_SQL_MAX + 1]; pretty_sql(sql, dialect_of_conn(a, conn), shown, sizeof shown); editor_set_text(G.ced, shown); }
        snprintf(G.ced_orig, sizeof G.ced_orig, "%s", sql);
        snprintf(G.ced_doc, sizeof G.ced_doc, "%s", d->r.id); snprintf(G.ced_vid, sizeof G.ced_vid, "%s", v->id);
        editor_set_read_only(G.ced, !editable); G.ced_ro = !editable;
        Connection *c = conn_of(a, conn); if (c) editor_set_dialect(G.ced, dialect_for_driver(c->driver));
    }
    const char *src = !v->sql ? L(d, "SQL compilado pelo Lume (catálogo)", "SQL compiled by Lume (catalog)") : v->sql_edited ? L(d, "SQL editado \xC2\xB7 não certificado", "Edited SQL \xC2\xB7 not certified") : v->ai_sql ? L(d, "SQL da IA \xC2\xB7 não certificado", "AI-written SQL \xC2\xB7 not certified") : L(d, "SQL do usuário", "User SQL");
    ui_label(a, rect_make(x, y, w, lh), L(d, "Consulta", "Query"), ALIGN_LEFT, T_TEXT_MUTED);
    ui_label(a, rect_make(x, y, w, lh), src, ALIGN_RIGHT, T_TEXT_DIM);
    y += lh + px(a, 4);
    Rect er = rect_make(x, y, w, px(a, 170));
    draw_rect(r, er, C(T_EDITOR_BG));
    G.ced_rect = er;
    /* the editor types only while it has the focus (the query tabs keep theirs otherwise) */
    Input *in = &a->in;
    if (in->mouse_pressed && !in->mouse_consumed) { if (rect_contains(er, in->mouse_x, in->mouse_y)) { G.ced_focus = true; a->focus = FOCUS_NONE; focus_id = 0; } else G.ced_focus = false; }
    if (G.ced_focus && a->focus != FOCUS_NONE) G.ced_focus = false;   /* the user went to another panel */
    Editor *prev = a->editor; int pf = a->focus;
    if (G.ced_focus) { a->editor = G.ced; a->focus = FOCUS_EDITOR; }
    editor_update(G.ced, a, er);
    if (G.ced_focus) { a->editor = prev; a->focus = pf; }
    ui_record("rpt:card:sql", er);
    if (G.ced_focus) draw_rect_outline(r, er, px(a, 1), C(T_ACCENT));
    y += er.h + px(a, 6);
    if (G.ced_apply) { G.ced_apply = false; if (editable) card_sql_apply(a, d, v); }
    float bw = (w - px(a, 12)) / 3;
    if (ui_button(a, rect_make(x, y, bw, rh), L(d, "Aplicar  \xE2\x8C\x98\xE2\x86\xA9", "Apply  \xE2\x8C\x98\xE2\x86\xA9"), BTN_PRIMARY, editable)) card_sql_apply(a, d, v);
    if (ui_button(a, rect_make(x + bw + px(a, 6), y, bw, rh), L(d, "Descartar", "Discard"), BTN_NORMAL, editable)) { char shown[RPT_AI_SQL_MAX + 1]; pretty_sql(v->sql ? v->sql : "", dialect_of_conn(a, v->conn), shown, sizeof shown); editor_set_text(G.ced, shown); }
    if (ui_button(a, rect_make(x + (bw + px(a, 6)) * 2, y, bw, rh), L(d, "Abrir em aba", "Open in a tab"), BTN_NORMAL, sql[0] != 0)) { char tt[64]; snprintf(tt, sizeof tt, "%s.sql", v->id); panels_open_tab(a->panels, a, tt, conn, sql); }
    y += rh + px(a, 8);
    /* the last run: rows / time / when, or the database's error */
    RvData *x2 = sql[0] ? visual_data(a, d, v, false, why, sizeof why) : NULL;
    char st[400];
    if (!x2) { snprintf(st, sizeof st, "%s", why[0] ? why : L(d, "Sem consulta", "No query")); ui_label(a, rect_make(x, y, w, lh), st, ALIGN_LEFT, T_YELLOW); y += lh + px(a, 6); }
    else if (x2->status == RD_QUEUED || x2->status == RD_RUNNING) { ui_label(a, rect_make(x, y, w, lh), L(d, "Consultando...", "Querying..."), ALIGN_LEFT, T_TEXT_MUTED); y += lh + px(a, 6); app_wake_in(a, 200); }
    else if (x2->status == RD_OK) {
        char ts[16] = ""; if (x2->fetched_at > 0) { time_t tt = (time_t)x2->fetched_at; struct tm tm; localtime_r(&tt, &tm); strftime(ts, sizeof ts, "%H:%M:%S", &tm); }
        snprintf(st, sizeof st, "\xE2\x9C\x93 %d %s \xC2\xB7 %.0f ms \xC2\xB7 %s %s", x2->rs.nrows, L(d, "linha(s)", "row(s)"), x2->ms, L(d, "às", "at"), ts);
        ui_label(a, rect_make(x, y, w - px(a, 96), lh), st, ALIGN_LEFT, T_GREEN);
        if (ui_button(a, rect_make(x + w - px(a, 92), y - px(a, 3), px(a, 92), px(a, 22)), L(d, "Atualizar", "Refresh"), BTN_QUIET, true)) { data_free(x2); app_wake_in(a, 16); }
        y += lh + px(a, 8);
    } else {
        const char *e = x2->err[0] ? x2->err : x2->rs.error[0] ? x2->rs.error : L(d, "falhou", "failed");
        y += results_draw_wrapped(a->font_small, r, e, rect_make(x, y, w, px(a, 90)), C(T_RED), 0, true) + px(a, 6);
        if (ui_button(a, rect_make(x, y, bw, px(a, 24)), L(d, "Copiar erro", "Copy error"), BTN_QUIET, true)) { SDL_SetClipboardText(e); app_status(a, "Error copied"); }
        if (ui_button(a, rect_make(x + bw + px(a, 6), y, bw * 2 + px(a, 6), px(a, 24)), L(d, "Pedir à IA para corrigir", "Ask the AI to fix it"), BTN_NORMAL, v->sql != NULL)) {
            char q[700]; snprintf(q, sizeof q, L(d, "A consulta do card \"%s\" falhou com o erro: %.300s. Corrija só esse card.", "The query of the card \"%s\" failed with: %.300s. Fix that card only."), v->title[0] ? v->title : v->id, e);
            textfield_set(&G.q, q); G.choice[0] = 0; G.adoc = d; panels_show(a->panels, a, "lume.data.assistant"); ai_send(a, d);
        }
        if (ui_button(a, rect_make(x + w - px(a, 92), y + px(a, 28), px(a, 92), px(a, 22)), L(d, "Tentar de novo", "Try again"), BTN_QUIET, true)) { data_free(x2); app_wake_in(a, 16); }
        y += px(a, 54);
    }
    *py = y;
}
static float g_props_end;   /* bottom of the inspector content, relative to its top (for its scroll) */
static void props_body(App *a, RvDoc *d, Rect pr, float scroll);
/* The inspector beside the canvas: clipped to its own rectangle and scrolled when taller, so nothing (Delete
 * card included) is drawn over, or clickable on top of, the panels below it. */
static void props_panel(App *a, RvDoc *d, Rect pr) {
    static float scroll; static ScrollBar sb; static RvDoc *last;
    SDL_Renderer *r = a->renderer; Input *in = &a->in;
    if (last != d) { scroll = 0; last = d; }
    draw_rect(r, pr, C(T_PANEL));
    draw_rect(r, rect_make(pr.x, pr.y, px(a, 1), pr.h), C(T_BORDER));
    float content = g_props_end + px(a, 12), maxs = content - pr.h;
    bool inside = rect_contains(pr, in->mouse_x, in->mouse_y);
    if (inside && in->wheel_y != 0 && maxs > 0) { scroll -= in->wheel_y * px(a, 30); in->wheel_y = 0; ui_scrollbar_touch(&sb); }
    ui_scrollbar_input(a, &sb, pr, content, &scroll);
    if (scroll > maxs) scroll = maxs;
    if (scroll < 0) scroll = 0;
    float mx = in->mouse_x, my = in->mouse_y;
    if (!inside) { in->mouse_x = -1e6f; in->mouse_y = -1e6f; }   /* the clipped-away part is not clickable */
    clip_push(r, pr);
    props_body(a, d, pr, scroll);
    clip_pop(r);
    in->mouse_x = mx; in->mouse_y = my;
    ui_scrollbar_draw(a, &sb, pr, content, scroll);
}
static void props_body(App *a, RvDoc *d, Rect pr, float scroll) {
    SDL_Renderer *r = a->renderer;
    float x = pr.x + px(a, 12), w = pr.w - px(a, 24), y = pr.y + px(a, 10) - scroll, rh = px(a, 26), lh = px(a, 16);
    int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg);
    if (v && pg != d->page) v = NULL;
    /* rebind the fields when the selection or the document changed under them (undo / AI) */
    if (strcmp(d->bound, d->sel) || d->bound_rev != d->r.revision) {
        if (v) { if (focus_id != 1 || strcmp(d->bound, d->sel)) textfield_set(&d->f_title, v->title); if (focus_id != 2 || strcmp(d->bound, d->sel)) textfield_set(&d->f_sub, v->subtitle); if (focus_id != 3 || strcmp(d->bound, d->sel)) { char t[256]; snprintf(t, sizeof t, "%s", v->text ? v->text : ""); for (char *p = t; *p; p++) if (*p == '\n') *p = '|'; textfield_set(&d->f_text, t); } }
        if (focus_id != 4) textfield_set(&d->f_rtitle, d->r.title);
        if (focus_id != 5) textfield_set(&d->f_page, d->r.pages[d->page].name);
        const RptFilter *f = page_filter(d); if (focus_id != 6) textfield_set(&d->f_fval, f ? f->value : "");
        snprintf(d->bound, sizeof d->bound, "%s", d->sel); d->bound_rev = d->r.revision;
    }
    #define LABEL(s) do { ui_label(a, rect_make(x, y, w, lh), s, ALIGN_LEFT, T_TEXT_MUTED); y += lh; } while (0)
    #define FIELD(tf, id, ph) do { Rect fr = rect_make(x, y, w, rh); if (ui_textfield(a, tf, fr, ph, focus_id == id)) focus_id = id; y += rh + px(a, 8); } while (0)
    if (v) {
        LABEL(L(d, "Tipo de visual", "Visual type"));
        static const char *const kinds[] = { "KPI", "Line", "Bar", "Table", "Text" };
        int k = ui_segmented(a, rect_make(x, y, w, rh), kinds, 5, v->kind < RK_UNKNOWN ? v->kind : -1);
        if (k >= 0 && k != v->kind && !v->locked) { before_change(d); set_visual_kind(d, v, k); after_change(d); v = rpt_find(&d->r, d->sel, &pg); }
        y += rh + px(a, 10);
        LABEL(L(d, "Título", "Title")); FIELD(&d->f_title, 1, L(d, "automático pela métrica", "automatic from the metric"));
        if (focus_id == 1) commit_field(d, &d->f_title, v->title, sizeof v->title);
        LABEL(L(d, "Subtítulo", "Subtitle")); FIELD(&d->f_sub, 2, "");
        if (focus_id == 2) commit_field(d, &d->f_sub, v->subtitle, sizeof v->subtitle);
        v = rpt_find(&d->r, d->sel, &pg); if (!v) return;
        if (v->kind == RK_TEXT) {
            LABEL(L(d, "Texto ( | quebra linha, \"- \" item )", "Text ( | new line, \"- \" bullet )")); FIELD(&d->f_text, 3, "");
            if (focus_id == 3) { char t[256]; snprintf(t, sizeof t, "%s", d->f_text.buf); for (char *p = t; *p; p++) if (*p == '|') *p = '\n'; if (strcmp(t, v->text ? v->text : "")) { if (last_field != &d->f_text || now_s() - last_commit > 1.5) before_change(d); rpt_visual_set_text(v, t); after_change(d); last_commit = now_s(); last_field = &d->f_text; } }
            if (v->ai_suggested && ui_button(a, rect_make(x, y, w, rh), L(d, "Aprovar texto sugerido pela IA", "Approve the AI-suggested text"), BTN_PRIMARY, true)) { before_change(d); v->ai_suggested = false; after_change(d); audit(a, "propose_report_patch", d, "text-approved", 0, ""); }
            if (v->ai_suggested) y += rh + px(a, 8);
        } else if (v->kind != RK_UNKNOWN && !v->sql) {
            const SemMetricDef *md = metric_def(v);
            char b[160];
            LABEL(L(d, "Métrica (catálogo semântico)", "Metric (semantic catalog)"));
            snprintf(b, sizeof b, "%s%s", md ? md->name : L(d, "escolher...", "choose..."), md && md->certification != SC_CERTIFIED ? L(d, "  (não certificada)", "  (not certified)") : "");
            if (ui_button(a, rect_make(x, y, w, rh), b, BTN_NORMAL, !v->locked)) open_menu(a, d, MK_METRIC, x, y + rh);
            y += rh + px(a, 8);
            if (v->kind != RK_KPI) {
                LABEL(L(d, "Dimensão", "Dimension"));
                if (ui_button(a, rect_make(x, y, w, rh), v->dim[0] ? v->dim : L(d, "(nenhuma)", "(none)"), BTN_NORMAL, !v->locked && v->kind != RK_LINE)) open_menu(a, d, MK_DIM, x, y + rh);
                y += rh + px(a, 8);
                LABEL(L(d, "Granularidade de tempo", "Time grain"));
                if (ui_button(a, rect_make(x, y, w, rh), v->grain >= 0 ? sem_grain_name(v->grain) : L(d, "(nenhuma)", "(none)"), BTN_NORMAL, !v->locked)) open_menu(a, d, MK_GRAIN, x, y + rh);
                y += rh + px(a, 8);
            } else {
                LABEL(L(d, "Comparação", "Comparison"));
                if (ui_button(a, rect_make(x, y, w, rh), v->compare == RC_YOY ? "YoY" : v->compare == RC_PREV ? L(d, "Período anterior", "Previous period") : L(d, "Sem comparação", "No comparison"), BTN_NORMAL, !v->locked)) open_menu(a, d, MK_COMPARE, x, y + rh);
                y += rh + px(a, 8);
            }
            LABEL(L(d, "Formato do número", "Number format"));
            if (ui_button(a, rect_make(x, y, w, rh), rpt_fmt_name(v->fmt), BTN_NORMAL, !v->locked)) open_menu(a, d, MK_FORMAT, x, y + rh);
            y += rh + px(a, 8);
            char why[300];
            if (!rpt_visual_check(&d->r, v, &G.cat, why, sizeof why)) { float hh = results_draw_wrapped(a->font_small, r, why, rect_make(x, y, w, px(a, 60)), C(T_RED), 0, true); y += hh + px(a, 6); }
            if (md && sem_metric_c(&G.cat, v->metric) && sem_cur(sem_metric_c(&G.cat, v->metric))->version != md->version) {
                snprintf(b, sizeof b, L(d, "Validado com v%d; catálogo em v%d", "Validated with v%d; catalog at v%d"), md->version, sem_cur(sem_metric_c(&G.cat, v->metric))->version);
                ui_label(a, rect_make(x, y, w, lh), b, ALIGN_LEFT, T_YELLOW); y += lh;
                if (ui_button(a, rect_make(x, y, w, rh), L(d, "Atualizar para a versão atual", "Move to the current version"), BTN_NORMAL, !v->locked)) { before_change(d); v->metric_version = sem_cur(sem_metric_c(&G.cat, v->metric))->version; after_change(d); }
                y += rh + px(a, 8);
            }
        }
        if (v->sql && has_data(v)) {
            LABEL(L(d, "Formato do número", "Number format"));
            if (ui_button(a, rect_make(x, y, w, rh), rpt_fmt_name(v->fmt), BTN_NORMAL, !v->locked)) open_menu(a, d, MK_FORMAT, x, y + rh);
            y += rh + px(a, 8);
        }
        if (has_data(v)) { draw_rect(r, rect_make(x, y, w, px(a, 1)), C(T_BORDER)); y += px(a, 8); card_query_section(a, d, v, x, &y, w); v = rpt_find(&d->r, d->sel, &pg); if (!v) return; }
        if (ui_checkbox(a, rect_make(x, y, w, rh), L(d, "Bloquear posição e conteúdo", "Lock position and content"), v->locked, true)) { before_change(d); v->locked = !v->locked; after_change(d); }
        y += rh + px(a, 6);
        if (ui_button(a, rect_make(x, y, w, rh), L(d, "Excluir card", "Delete card"), BTN_DANGER, !v->locked)) { before_change(d); rpt_remove_visual(&d->r, v->id); d->sel[0] = 0; after_change(d); }
        y += rh + px(a, 6);
    } else {
        LABEL(L(d, "Título do relatório", "Report title")); FIELD(&d->f_rtitle, 4, "");
        if (focus_id == 4 && strcmp(d->f_rtitle.buf, d->r.title)) { commit_field(d, &d->f_rtitle, d->r.title, sizeof d->r.title); panels_set_tab_title(a->panels, d->tab_id, d->r.title); }
        LABEL(L(d, "Nome da página", "Page name")); FIELD(&d->f_page, 5, "");
        if (focus_id == 5) commit_field(d, &d->f_page, d->r.pages[d->page].name, sizeof d->r.pages[d->page].name);
        LABEL(L(d, "Idioma e formatos", "Language and formats"));
        static const char *const loc[] = { "pt-BR", "en-US" };
        int li = ui_segmented(a, rect_make(x, y, w, rh), loc, 2, pt_locale(d) ? 0 : 1);
        if (li >= 0) { before_change(d); snprintf(d->r.locale, sizeof d->r.locale, "%s", loc[li]); after_change(d); }
        y += rh + px(a, 10);
        const RptFilter *f = page_filter(d);
        LABEL(L(d, "Filtro da página (dimensão = valor)", "Page filter (dimension = value)"));
        if (ui_button(a, rect_make(x, y, w, rh), f && f->field[0] ? f->field : L(d, "(sem filtro)", "(no filter)"), BTN_NORMAL, true)) open_menu(a, d, MK_FILTER, x, y + rh);
        y += rh + px(a, 6);
        if (f && f->field[0]) {
            FIELD(&d->f_fval, 6, L(d, "valor", "value"));
            if (focus_id == 6) { RptFilter *mf = (RptFilter *)f; commit_field(d, &d->f_fval, mf->value, sizeof mf->value); }
        }
        char info[300]; snprintf(info, sizeof info, L(d, "Catálogo: %d métrica(s), %d dataset(s). Clique num card para editá-lo.", "Catalog: %d metric(s), %d dataset(s). Click a card to edit it."), G.cat.nm, G.cat.nds);
        y += results_draw_wrapped(a->font_small, r, info, rect_make(x, y, w, px(a, 60)), C(T_TEXT_DIM), 0, true) + px(a, 12);
        /* ---- Theme: where it comes from, its lock, quick changes kept as overrides ---- */
        theme_resolve(d);
        draw_rect(r, rect_make(x, y, w, px(a, 1)), C(T_BORDER)); y += px(a, 8);
        char src[200]; theme_source_label(d, src, sizeof src);
        char tl[300]; snprintf(tl, sizeof tl, "%s: %s%s", L(d, "Tema", "Theme"), src, d->rt.locked ? L(d, "  \xC2\xB7  travado", "  \xC2\xB7  locked") : "");
        ui_label(a, rect_make(x, y, w, lh), tl, ALIGN_LEFT, T_TEXT); y += lh + px(a, 2);
        if (d->rt_warn[0]) y += results_draw_wrapped(a->font_small, r, d->rt_warn, rect_make(x, y, w, px(a, 40)), C(T_YELLOW), 0, true) + px(a, 4);
        if (d->r.theme_overrides && !d->rt.locked) {
            ui_label(a, rect_make(x, y, w - px(a, 150), lh), L(d, "Com ajustes neste relatório", "With changes in this report"), ALIGN_LEFT, T_TEXT_DIM);
            if (ui_button(a, rect_make(x + w - px(a, 146), y - px(a, 3), px(a, 146), px(a, 22)), L(d, "Descartar ajustes", "Discard changes"), BTN_QUIET, true)) { before_change(d); free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_dirty(d); }
            y += lh + px(a, 6);
        }
        if (d->rt.locked) { y += results_draw_wrapped(a->font_small, r, L(d, "As cores, a tipografia e o cabeçalho vêm da marca. O layout e o conteúdo continuam editáveis.", "Colours, type and header come from the brand. Layout and content stay editable."), rect_make(x, y, w, px(a, 40)), C(T_TEXT_DIM), 0, true) + px(a, 6); }
        else {
            LABEL(L(d, "Cards", "Cards"));
            static const char *const cst[] = { "Flat", "Shadow", "Border", "Accent" };
            int ci = ui_segmented(a, rect_make(x, y, w, rh), cst, 4, d->rt.card_style);
            if (ci >= 0 && ci != d->rt.card_style) theme_change(d, tc_card, &ci);
            y += rh + px(a, 8);
            LABEL("KPI");
            static const char *const kst[] = { "Plain", "Accent bar", "Tinted" };
            int ki = ui_segmented(a, rect_make(x, y, w, rh), kst, 3, d->rt.kpi_style);
            if (ki >= 0 && ki != d->rt.kpi_style) theme_change(d, tc_kpi, &ki);
            y += rh + px(a, 8);
            LABEL(L(d, "Cor de destaque e cantos", "Accent colour and corners"));
            static const uint32_t sw[] = { 0x1F6FEB, 0x0B7A75, 0x6D28D9, 0xC2410C, 0xB42318, 0x0B1F3A, 0x15803D, 0x334155 };
            float bx = x, bwid = px(a, 22);
            for (int i = 0; i < 8; i++) {
                Rect b = rect_make(bx, y, bwid, bwid);
                draw_rrect(r, b, px(a, 11), C(sw[i]));
                if (d->rt.accent == sw[i]) draw_rect_outline(r, rect_inset(b, -px(a, 2), -px(a, 2)), px(a, 2), C(T_TEXT));
                char lab[40]; snprintf(lab, sizeof lab, "rpt:accent:%06X", sw[i]); ui_record(lab, b);
                if (rect_contains(b, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; uint32_t c = sw[i]; theme_change(d, tc_accent, &c); }
                bx += bwid + px(a, 6);
            }
            if (ui_button(a, rect_make(x + w - px(a, 60), y - px(a, 1), px(a, 28), px(a, 24)), "\xE2\x80\x93", BTN_QUIET, d->rt.radius > 0)) { float rr = fmaxf(0, d->rt.radius - 2); theme_change(d, tc_radius, &rr); }
            if (ui_button(a, rect_make(x + w - px(a, 30), y - px(a, 1), px(a, 28), px(a, 24)), "+", BTN_QUIET, d->rt.radius < 24)) { float rr = fminf(24, d->rt.radius + 2); theme_change(d, tc_radius, &rr); }
            y += bwid + px(a, 10);
            bool hdr = d->rt.header;
            if (ui_checkbox(a, rect_make(x, y, w, px(a, 22)), L(d, "Cabeçalho com logo e título", "Header with logo and title"), hdr, true)) { bool nv = !hdr; theme_change(d, tc_header, &nv); }
            y += px(a, 26);
            if (d->rt.header) {
                static const uint32_t hb[] = { 0xFFFFFF, 0x0B1F3A, 0x111827, 0x1F6FEB, 0x0B7A75, 0x6D28D9 };
                bx = x;
                for (int i = 0; i < 6; i++) {
                    Rect b = rect_make(bx, y, bwid, bwid);
                    draw_rrect(r, b, px(a, 4), C(T_BORDER)); draw_rrect(r, rect_inset(b, px(a, 1), px(a, 1)), px(a, 3), C(hb[i]));
                    if (d->rt.header_bg == hb[i]) draw_rect_outline(r, rect_inset(b, -px(a, 2), -px(a, 2)), px(a, 2), C(T_TEXT));
                    if (rect_contains(b, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; uint32_t c = hb[i]; theme_change(d, tc_header_bg, &c); }
                    bx += bwid + px(a, 6);
                }
                const char *lpos[] = { L(d, "Logo à esquerda", "Logo left"), L(d, "à direita", "right") };
                int lp = ui_segmented(a, rect_make(bx + px(a, 6), y, x + w - bx - px(a, 6), bwid), lpos, 2, d->rt.logo_pos);
                if (lp >= 0 && lp != d->rt.logo_pos) theme_change(d, tc_logo_pos, &lp);
                y += bwid + px(a, 8);
            }
        }
        if (ui_button(a, rect_make(x, y, w, rh), L(d, "Mais opções de tema...", "More theme options..."), BTN_NORMAL, true)) open_menu(a, d, MK_THEME, x, y + rh);
        y += rh + px(a, 6);
    }
    g_props_end = y + scroll - pr.y;
    #undef LABEL
    #undef FIELD
}

/* ---- page zoom ---- */
static const float ZOOMS[] = { 0.5f, 0.67f, 0.75f, 0.9f, 1.0f, 1.1f, 1.25f, 1.5f, 1.75f, 2.0f };
static float zoom_of(const RvDoc *d) { return d->zoom > 0 ? d->zoom : 1.0f; }
static void zoom_step(RvDoc *d, int dir) {
    float z = zoom_of(d); int n = (int)(sizeof ZOOMS / sizeof ZOOMS[0]), k = 4;
    for (int i = 0; i < n; i++) if (fabsf(ZOOMS[i] - z) < 0.01f) k = i;
    if (dir == 0) k = 4; else { k += dir; if (k < 0) k = 0; if (k >= n) k = n - 1; }
    d->zoom = ZOOMS[k];
}
/* One action of the report bar (a button, or the same entry in its "More..." menu). */
static void tb_act(App *a, RvDoc *d, int id, float mx, float my) {
    switch (id) {
    case TBA_ADD: open_menu(a, d, MK_ADD, mx, my); break;
    case TBA_UNDO: if (rpt_undo(&d->h, &d->r)) { mark_changed(d); if (d->page >= d->r.np) d->page = 0; } break;
    case TBA_REDO: if (rpt_redo(&d->h, &d->r)) mark_changed(d); break;
    case TBA_THEME: open_menu(a, d, MK_THEME, mx, my); break;
    case TBA_PERIOD: open_menu(a, d, MK_PERIOD, mx, my); break;
    case TBA_REFRESH: invalidate_data(d); break;
    case TBA_PREVIEW: d->preview = !d->preview; d->drag = 0; break;
    case TBA_SAVE: ui_save(a, d); break;
    case TBA_SAVE_AS: ask_path(a, d, EX_SAVE_AS, NULL); break;
    case TBA_ASSISTANT: panels_show(a->panels, a, "lume.data.assistant"); G.adoc = d; break;
    case TBA_EXPORT: open_menu(a, d, MK_EXPORT, mx, my); break;
    case TBA_ZOOM_OUT: zoom_step(d, -1); break;
    case TBA_ZOOM_100: zoom_step(d, 0); break;
    case TBA_ZOOM_IN: zoom_step(d, 1); break;
    }
}
static void zoom_fonts(App *a, float sc) {
    if (G.zf_ui && fabsf(G.zscale - sc) < 0.001f) return;
    if (G.zf_ui) { font_destroy(G.zf_ui); font_destroy(G.zf_small); font_destroy(G.zf_title); font_destroy(G.zf_kpi); }
    char fp[1200]; snprintf(fp, sizeof fp, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
    G.zf_ui = font_load(a->renderer, fp, roundf(M_FONT_UI * sc)); G.zf_small = font_load(a->renderer, fp, roundf(M_FONT_SMALL * sc));
    G.zf_title = font_load(a->renderer, fp, roundf(16 * sc)); G.zf_kpi = font_load(a->renderer, fp, roundf(30 * sc));
    G.zscale = sc;
}
static void canvas_input(App *a, RvDoc *d, Rect canvas) {
    Input *in = &a->in;
    RptPage *p = &d->r.pages[d->page];
    float maxs = fmaxf(0, page_height(d) - canvas.h + d->gap);
    if (rect_contains(canvas, in->mouse_x, in->mouse_y) && in->wheel_y != 0 && (app_modstate(a) & SDL_KMOD_GUI)) { zoom_step(d, in->wheel_y > 0 ? 1 : -1); in->wheel_y = 0; app_wake_in(a, 16); }
    if (rect_contains(canvas, in->mouse_x, in->mouse_y) && (in->wheel_x != 0 || (in->wheel_y != 0 && (app_modstate(a) & SDL_KMOD_SHIFT))) && zoom_of(d) > 1.0f) { d->hscroll -= (in->wheel_x != 0 ? -in->wheel_x : in->wheel_y) * px(a, 40); in->wheel_x = 0; in->wheel_y = 0; }
    d->content_h = page_height(d) + d->gap;
    ui_scrollbar_input(a, &d->sb, d->viewport.w > 0 ? d->viewport : canvas, d->content_h, &d->scroll);
    if (rect_contains(canvas, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { d->scroll -= in->wheel_y * px(a, 22); in->wheel_y = 0; ui_scrollbar_touch(&d->sb); }
    if (d->scroll > maxs) d->scroll = maxs; if (d->scroll < 0) d->scroll = 0;
    bool editing = !d->preview && !d->read_mode;
    if (!editing) { d->drag = 0; return; }
    if (d->drag) {
        int pg; RptVisual *v = rpt_find(&d->r, d->drag_id, &pg);
        if (!v) { d->drag = 0; return; }
        int dcol = (int)lroundf((in->mouse_x - d->mx0) / (d->colw + d->gap)), drow = (int)lroundf((in->mouse_y - d->my0) / (d->rowh + d->gap));
        int mw, mh; rpt_min_size(v->kind, &mw, &mh);
        if (d->drag == 1) { d->gx = d->ox + dcol; d->gy = d->oy + drow; if (d->gx < 0) d->gx = 0; if (d->gx + d->gw > RPT_COLS) d->gx = RPT_COLS - d->gw; if (d->gy < 0) d->gy = 0; }
        else { d->gw = d->ow + dcol; d->gh = d->oh + drow; if (d->gw < mw) d->gw = mw; if (d->gx + d->gw > RPT_COLS) d->gw = RPT_COLS - d->gx; if (d->gh < mh) d->gh = mh; }
        a->animating = true;
        if (!in->mouse_down) {
            if (d->gx != v->x || d->gy != v->y || d->gw != v->w || d->gh != v->h) {
                before_change(d);
                if (rpt_place(&d->r, d->page, d->drag_id, d->gx, d->gy, d->gw, d->gh)) after_change(d);
                else { RptHist *h = &d->h; free(h->u[--h->nu]); set_msg(d, "%s", L(d, "Não cabe ali: um card bloqueado está no caminho", "It does not fit there: a locked card is in the way")); }
            }
            d->drag = 0;
        }
        return;
    }
    if (!in->mouse_pressed || in->mouse_consumed || !rect_contains(canvas, in->mouse_x, in->mouse_y)) return;
    for (int i = p->nv - 1; i >= 0; i--) {
        RptVisual *v = &p->v[i];
        Rect rc = cell_rect(d, v);
        if (!rect_contains(rc, in->mouse_x, in->mouse_y)) continue;
        in->mouse_consumed = true;
        snprintf(d->sel, sizeof d->sel, "%s", v->id);
        focus_id = 0;
        if (v->locked || v->kind == RK_UNKNOWN) return;
        Rect handle = rect_make(rc.x + rc.w - px(a, 16), rc.y + rc.h - px(a, 16), px(a, 16), px(a, 16));
        bool resize = rect_contains(handle, in->mouse_x, in->mouse_y);
        bool header = in->mouse_y < rc.y + px(a, 34);
        if (!resize && !header) return;   /* the body keeps its own interactions (table sort, tooltips) */
        d->drag = resize ? 2 : 1; snprintf(d->drag_id, sizeof d->drag_id, "%s", v->id);
        d->mx0 = in->mouse_x; d->my0 = in->mouse_y; d->ox = d->gx = v->x; d->oy = d->gy = v->y; d->ow = d->gw = v->w; d->oh = d->gh = v->h;
        return;
    }
    in->mouse_consumed = true; d->sel[0] = 0; focus_id = 0;
}

void rptview_tab_body(App *a, RvDoc *d, Rect body) {
    double t0 = now_s();
    if (!G.cat_loaded) load_catalog(a);
    ensure_fonts(a);
    SDL_Renderer *r = a->renderer;
    G.a = a;
    bool editing = !d->preview && !d->read_mode;
    float bh = px(a, 34), ih = px(a, 22);
    Rect bar = rect_make(body.x, body.y, body.w, bh);
    draw_rect(r, bar, C(T_PANEL_ALT));
    draw_rect(r, rect_make(body.x, body.y + bh - px(a, 1), body.w, px(a, 1)), C(T_BORDER));
    float x = body.x + px(a, 8), y = body.y + px(a, 5), h = bh - px(a, 10);
    /* pages (LI-039) */
    for (int i = 0; i < d->r.np; i++) {
        float w = font_width(a->font_ui, d->r.pages[i].name, -1) + px(a, 20);
        char lab[80]; snprintf(lab, sizeof lab, "%s", d->r.pages[i].name);
        if (ui_button(a, rect_make(x, y, w, h), lab, i == d->page ? BTN_SELECTED : BTN_QUIET, true)) { d->page = i; d->sel[0] = 0; d->scroll = 0; }
        x += w + px(a, 4);
    }
    x += px(a, 8);
    float xmax = body.x + body.w - px(a, 6);
    clip_push(r, bar);
    /* the bar's actions: what does not fit goes into a "More..." menu at the end (never drawn over the
     * next panel, never dropped without a way to reach it) */
    char zl[16]; snprintf(zl, sizeof zl, "%d%%", (int)roundf(zoom_of(d) * 100));
    struct { int id; const char *lab; bool en; int style; } it[16]; int ni = 0;
    #define IT(i_, l_, e_, s_) do { it[ni].id = i_; it[ni].lab = l_; it[ni].en = e_; it[ni].style = s_; ni++; } while (0)
    if (editing) {
        IT(TBA_ADD, L(d, "Adicionar", "Add"), true, BTN_NORMAL);
        IT(TBA_UNDO, L(d, "Desfazer", "Undo"), d->h.nu > 0, BTN_QUIET);
        IT(TBA_REDO, L(d, "Refazer", "Redo"), d->h.nr > 0, BTN_QUIET);
        IT(TBA_THEME, L(d, "Tema", "Theme"), true, BTN_QUIET);
    }
    IT(TBA_PERIOD, L(d, "Período", "Period"), true, BTN_QUIET);
    IT(TBA_REFRESH, L(d, "Atualizar", "Refresh"), true, BTN_QUIET);
    if (!d->read_mode) IT(TBA_PREVIEW, d->preview ? L(d, "Editar", "Edit") : L(d, "Pré-visualizar", "Preview"), true, d->preview ? BTN_SELECTED : BTN_QUIET);
    if (!d->read_mode) IT(TBA_SAVE, L(d, "Salvar", "Save"), true, d->dirty ? BTN_PRIMARY : BTN_QUIET);
    if (!d->read_mode) IT(TBA_SAVE_AS, L(d, "Salvar como...", "Save As..."), true, BTN_QUIET);
    IT(TBA_ASSISTANT, L(d, "Assistente", "Assistant"), true, BTN_QUIET);
    IT(TBA_EXPORT, L(d, "Exportar", "Export"), true, BTN_QUIET);
    IT(TBA_ZOOM_OUT, "\xE2\x80\x93", zoom_of(d) > ZOOMS[0] + 0.001f, BTN_QUIET);
    IT(TBA_ZOOM_100, zl, true, BTN_QUIET);
    IT(TBA_ZOOM_IN, "+", zoom_of(d) < 2.0f - 0.001f, BTN_QUIET);
    #undef IT
    float total = 0; for (int i = 0; i < ni; i++) total += font_width(a->font_ui, it[i].lab, -1) + px(a, 22);
    const char *more_lab = L(d, "Mais...", "More...");
    float more_w = font_width(a->font_ui, more_lab, -1) + px(a, 18);
    float lim = x + total <= xmax ? xmax : xmax - more_w - px(a, 4);
    G.nmore = 0;
    for (int i = 0; i < ni; i++) {
        float w_ = font_width(a->font_ui, it[i].lab, -1) + px(a, 18);
        if (x + w_ <= lim && G.nmore == 0) { if (ui_button(a, rect_make(x, y, w_, h), it[i].lab, it[i].style, it[i].en)) tb_act(a, d, it[i].id, x, y + h); x += w_ + px(a, 4); }
        else if (G.nmore < 16) { G.more_id[G.nmore] = it[i].id; G.more_en[G.nmore] = it[i].en; snprintf(G.more_lab[G.nmore], sizeof G.more_lab[0], "%s", it[i].lab); G.nmore++; }
    }
    if (G.nmore) { Rect mb = rect_make(x, y, more_w, h); ui_record("rpt:more", mb); if (ui_button(a, mb, more_lab, BTN_QUIET, true)) open_menu(a, d, MK_MORE, x, y + h); G.more_x = x; G.more_y = y + h; x += more_w + px(a, 4); }
    clip_pop(r);
    if (d->read_mode) ui_label(a, rect_make(x + px(a, 6), y, body.x + body.w - x - px(a, 12), h), L(d, "Modo leitura: nada é alterado; os dados respeitam sua conexão", "Read mode: nothing changes; data follows your connection"), ALIGN_LEFT, T_TEXT_MUTED);
    /* provenance strip (LI-048.1) */
    Rect info = rect_make(body.x, body.y + bh, body.w, ih);
    draw_rect(r, info, C(T_PANEL));
    char pl[700]; provenance_line(a, d, pl, sizeof pl);
    draw_text_fit(a->font_small, r, d->msg[0] ? d->msg : pl, -1, rect_make(info.x + px(a, 10), info.y, info.w - px(a, 20), info.h), color_hex(d->msg[0] ? T_YELLOW : T_TEXT_MUTED), ALIGN_LEFT);
    if (d->msg[0] && rect_contains(info, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed) d->msg[0] = 0;
    float pw = editing ? px(a, 280) : 0;
    /* the report's filters: label and the value chosen ("all" = every value) */
    float fh = d->r.nparams ? px(a, 34) : 0;
    if (fh > 0) {
        Rect fb = rect_make(body.x, body.y + bh + ih, body.w, fh);
        draw_rect(r, fb, C(T_PANEL_ALT));
        float fx = fb.x + px(a, 10);
        for (int i = 0; i < d->r.nparams; i++) {
            RptParam *pm = &d->r.params[i];
            float lw = font_width(a->font_small, pm->label, -1) + px(a, 8);
            ui_label(a, rect_make(fx, fb.y, lw, fh), pm->label, ALIGN_LEFT, T_TEXT_MUTED); fx += lw;
            const char *val = pm->value[0] ? pm->value : L(d, "Todos", "All");
            float vw = fminf(px(a, 200), font_width(a->font_ui, val, -1) + px(a, 34));
            Rect vb = rect_make(fx, fb.y + px(a, 5), vw, fh - px(a, 10));
            char lab[64]; snprintf(lab, sizeof lab, "rpt:filter:%s", pm->name);
            if (ui_button(a, vb, val, pm->value[0] ? BTN_SELECTED : BTN_NORMAL, true) && !d->read_mode) { G.menu_param = i; open_menu(a, d, MK_PARAM, vb.x, vb.y + vb.h); }
            ui_record(lab, vb);
            fx += vw + px(a, 16);
        }
    }
    Rect canvas = rect_make(body.x, body.y + bh + ih + fh, body.w - pw, body.h - bh - ih - fh);
    /* page zoom: everything on the page scales together; wider than the view it scrolls sideways, narrower it is centred */
    float z = zoom_of(d), base = a->scale; Font *sui = a->font_ui, *ssm = a->font_small, *sti = G.f_title, *skp = G.f_kpi;
    Rect vc = canvas;
    if (fabsf(z - 1.0f) > 0.001f) {
        zoom_fonts(a, base * z);
        if (G.zf_ui && G.zf_small && G.zf_title && G.zf_kpi) { a->scale = base * z; a->font_ui = G.zf_ui; a->font_small = G.zf_small; G.f_title = G.zf_title; G.f_kpi = G.zf_kpi; }
        vc.w = canvas.w * z;
        if (z > 1.0f) { float mx = vc.w - canvas.w; if (d->hscroll > mx) d->hscroll = mx; if (d->hscroll < 0) d->hscroll = 0; vc.x = canvas.x - d->hscroll; }
        else { d->hscroll = 0; vc.x = canvas.x + (canvas.w - vc.w) / 2; }
        draw_rect(r, canvas, C(theme_of(d)->bg));
    } else d->hscroll = 0;
    clip_push(r, canvas);
    d->viewport = canvas;
    layout_grid(a, d, vc);
    if (!G.menu.open) canvas_input(a, d, vc);
    draw_page(a, d, vc, !G.menu.open && !d->drag, editing);
    if (d->prov[0]) draw_provenance(a, d, vc);
    clip_pop(r);
    a->scale = base; a->font_ui = sui; a->font_small = ssm; G.f_title = sti; G.f_kpi = skp;
    if (!G.menu.open) ui_scrollbar_draw(a, &d->sb, canvas, d->content_h, d->scroll);
    if (editing) props_panel(a, d, rect_make(canvas.x + canvas.w, canvas.y, pw, canvas.h));
    /* keyboard: undo / redo / delete when no text field edits */
    if (editing && !ui_text_editing()) {
        if (input_key(&a->in, SDLK_Z, SDL_KMOD_GUI)) { if (rpt_undo(&d->h, &d->r)) mark_changed(d); }
        else if (input_key(&a->in, SDLK_Z, SDL_KMOD_GUI | SDL_KMOD_SHIFT)) { if (rpt_redo(&d->h, &d->r)) mark_changed(d); }
        else if (d->sel[0] && (input_key(&a->in, SDLK_DELETE, 0) || input_key(&a->in, SDLK_BACKSPACE, 0))) { int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg); if (v && !v->locked) { before_change(d); rpt_remove_visual(&d->r, d->sel); d->sel[0] = 0; after_change(d); } }
    }
    if (G.job) app_wake_in(a, 100);
    G.render_ms = (now_s() - t0) * 1000.0;
}

/* ---- PNG export (LI-070): the page or one card, rendered offscreen at a chosen width ---- */
static bool export_image(App *a, RvDoc *d, const char *path, const char *visual, int width_px, bool pdf, char *why, int wn) {
    SDL_Renderer *r = a->renderer;
    /* everything the export shows must be loaded: a card still querying is not exported as if complete */
    RptPage *p = &d->r.pages[d->page];
    for (int i = 0; i < p->nv; i++) {
        const RptVisual *v = &p->v[i];
        if (visual && strcmp(v->id, visual)) continue;
        if (!has_data(v)) continue;
        char w2[300]; RvData *x = visual_data(a, d, v, false, w2, sizeof w2);
        if (x && (x->status == RD_QUEUED || x->status == RD_RUNNING)) { snprintf(why, (size_t)wn, "%s", L(d, "Exportação adiada: há cards ainda consultando dados (nenhum arquivo parcial foi gerado)", "Export postponed: cards are still querying (no partial file was written)")); return false; }
    }
    float save_scale = a->scale; Font *su = a->font_ui, *ss = a->font_small, *sm = a->font_mono, *st = G.f_title, *sk = G.f_kpi; float sfs = G.fonts_scale;
    float logical = 1200.0f;
    float sc = (float)width_px / logical;
    char fp[1200]; snprintf(fp, sizeof fp, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
    a->scale = sc;
    a->font_ui = font_load(r, fp, roundf(M_FONT_UI * sc)); a->font_small = font_load(r, fp, roundf(M_FONT_SMALL * sc));
    G.f_title = font_load(r, fp, roundf(16 * sc)); G.f_kpi = font_load(r, fp, roundf(30 * sc)); G.fonts_scale = sc;
    bool ok = a->font_ui && a->font_small && G.f_title && G.f_kpi;
    int W = width_px, H = 0;
    RvDoc tmp_geom = *d;   /* geometry only */
    layout_grid(a, &tmp_geom, rect_make(0, 0, (float)W, 1000));
    theme_of(d);
    bool theme_head = d->rt.header;
    float head = theme_head ? px(a, 30) : px(a, 70), foot = px(a, 26);
    const RptVisual *one = visual ? rpt_find(&d->r, visual, NULL) : NULL;
    if (visual && !one) { ok = false; snprintf(why, (size_t)wn, "no such card"); }
    if (one) { W = (int)(one->w * tmp_geom.colw + (one->w - 1) * tmp_geom.gap + tmp_geom.gap * 2); H = (int)(head + one->h * tmp_geom.rowh + (one->h - 1) * tmp_geom.gap + tmp_geom.gap * 2 + foot); }
    else { float ph = 0; for (int i = 0; i < p->nv; i++) { float b2 = tmp_geom.gap + (p->v[i].y + p->v[i].h) * (tmp_geom.rowh + tmp_geom.gap); if (b2 > ph) ph = b2; } H = (int)(head + ph + tmp_geom.gap + foot); }
    if (H < 200) H = 200; if (H > 16000) H = 16000;
    SDL_Texture *tex = ok ? SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_TARGET, W, H) : NULL;
    if (ok && !tex) { ok = false; snprintf(why, (size_t)wn, "export: cannot create an offscreen image (%s)", SDL_GetError()); }
    if (ok) {
        SDL_Texture *prev = SDL_GetRenderTarget(r);
        SDL_SetRenderTarget(r, tex);
        SDL_SetRenderClipRect(r, NULL);
        const Theme *th = theme_of(d);
        draw_rect(r, rect_make(0, 0, (float)W, (float)H), C(th->bg));
        /* header: title, page, period, source and data time (LI-070.2) */
        char pl[700]; provenance_line(a, d, pl, sizeof pl);
        if (theme_head) {   /* the theme draws title and logo on the page: here only the provenance */
            draw_text_fit(a->font_small, r, pl, -1, rect_make(px(a, 14), px(a, 6), (float)W - px(a, 28), px(a, 18)), C(th->muted), ALIGN_LEFT);
        } else {
            draw_rect(r, rect_make(0, 0, (float)W, head), C(th->band));
            draw_text(G.f_title, r, d->r.title, px(a, 14), px(a, 10), C(th->band_text));
            char sub[900]; snprintf(sub, sizeof sub, "%s  \xC2\xB7  %s", d->r.pages[d->page].name, pl);
            draw_text_fit(a->font_small, r, sub, -1, rect_make(px(a, 14), px(a, 38), (float)W - px(a, 28), px(a, 18)), C(th->band_text), ALIGN_LEFT);
        }
        RvDoc *g = d; float sscroll = g->scroll; g->scroll = 0;
        Rect canvas = rect_make(0, head, (float)W, (float)H - head - foot);
        if (one) {
            layout_grid(a, g, rect_make(0, 0, (float)W * 12, 1000)); g->colw = tmp_geom.colw; g->gap = tmp_geom.gap; g->rowh = tmp_geom.rowh;
            RptVisual cp = *one; cp.x = 0; cp.y = 0; g->canvas = canvas;
            draw_card(a, g, th, (RptVisual *)&cp, cell_rect(g, &cp), false, false);
        } else draw_page(a, g, canvas, false, false);
        g->scroll = sscroll;
        time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); char ts[40]; strftime(ts, sizeof ts, "%Y-%m-%d %H:%M", &tm);
        bool any_ai = false; for (int i = 0; i < p->nv; i++) if (p->v[i].ai_sql && (!visual || !strcmp(p->v[i].id, visual))) any_ai = true;
        char ft2[400]; snprintf(ft2, sizeof ft2, any_ai ? L(d, "Gerado pelo Lume Data em %s  \xC2\xB7  relatório %s rev %d  \xC2\xB7  inclui SQL escrito pela IA, não certificado", "Generated by Lume Data on %s  \xC2\xB7  report %s rev %d  \xC2\xB7  includes AI-written SQL, not certified")
                                                      : L(d, "Gerado pelo Lume Data em %s  \xC2\xB7  relatório %s rev %d  \xC2\xB7  valores calculados pelo catálogo semântico", "Generated by Lume Data on %s  \xC2\xB7  report %s rev %d  \xC2\xB7  values computed from the semantic catalog"), ts, d->r.id, d->r.revision);
        draw_text_fit(a->font_small, r, ft2, -1, rect_make(px(a, 14), (float)H - foot, (float)W - px(a, 28), foot), C(th->muted), ALIGN_LEFT);
        SDL_Surface *s = SDL_RenderReadPixels(r, NULL);
        SDL_SetRenderTarget(r, prev);
        SDL_Surface *c = s ? SDL_ConvertSurface(s, SDL_PIXELFORMAT_RGBA32) : NULL;
        if (!c) { ok = false; snprintf(why, (size_t)wn, "export: cannot read the image back"); }
        else if (pdf) ok = pdf_write_rgba(path, d->r.title, (const uint8_t *)c->pixels, c->w, c->h, c->pitch, why, wn);
        else ok = png_write_rgba(path, (const uint8_t *)c->pixels, c->w, c->h, c->pitch, why, wn);
        if (c) SDL_DestroySurface(c);
        if (s) SDL_DestroySurface(s);
        SDL_DestroyTexture(tex);
    }
    font_destroy(a->font_ui); font_destroy(a->font_small); font_destroy(G.f_title); font_destroy(G.f_kpi);
    a->scale = save_scale; a->font_ui = su; a->font_small = ss; a->font_mono = sm; G.f_title = st; G.f_kpi = sk; G.fonts_scale = sfs;
    layout_grid(a, d, d->canvas);
    if (ok) { G.exports++; audit(a, "export_report", d, pdf ? "pdf" : "png", 0, visual ? "scope=visual" : "scope=page"); LOG_I("report: exported %s (%dx%d)", visual ? "a card" : "a page", W, H); }
    else audit(a, "export_report", d, "failed", 0, "");
    return ok;
}

/* ---- assistant (LI-009..014 / 021 / 051 / 059 / 079) ---- */
/* while the answer streams: the readable part so far (an explanation, a rationale, a question),
 * shown in the live bubble; the JSON itself is applied only when complete */
static void live_text(App *a) {
    (void)a;
    static char raw[16384]; size_t n = ai_partial(G.ai, raw, sizeof raw);
    if (!n) return;
    static const char *const keys[] = { "\"explanation\"", "\"text\"", "\"rationale\"", "\"question\"", "\"reason\"", "\"summary\"", NULL };
    const char *best = NULL;
    for (int k = 0; keys[k]; k++) { const char *p = strstr(raw, keys[k]); if (p && (!best || p > best)) best = p; }
    char shown[400]; size_t w = 0;
    if (best) {
        const char *p = strchr(best + 1, ':'); if (p) p = strchr(p, '"');
        for (p = p ? p + 1 : NULL; p && *p && *p != '"' && w + 2 < sizeof shown; p++) {
            if (*p == '\\' && p[1]) { p++; shown[w++] = *p == 'n' ? ' ' : *p; continue; }
            shown[w++] = *p;
        }
    }
    shown[w] = 0;
    if (w) snprintf(G.amsg, sizeof G.amsg, "Writing: %s", w > 300 ? shown + w - 300 : shown);
    else snprintf(G.amsg, sizeof G.amsg, "Writing the answer (%zu characters so far)...", n);
}
/* the assistant's requests stream (the answer shows as it is written); tests and setup do not need to */
static const AiConfig *stream_cfg(void) { static AiConfig c; c = G.cfg; c.stream = true; if (G.quick && c.model_fast[0]) snprintf(c.model, sizeof c.model, "%s", c.model_fast); return &c; }
static void ai_reset_plan(void) { G.steps_open = false; G.steps_closed = false; G.nsteps = 0; G.napplied = 0; ag_answer_free(&G.ans); memset(&G.ans, 0, sizeof G.ans); }
static void add_step(const char *tool, int st, const char *sql, const char *fmt, ...) __attribute__((format(printf, 4, 5)));
static void add_step(const char *tool, int st, const char *sql, const char *fmt, ...) {
    if (G.nsteps >= 24) return;
    Step *s = &G.steps[G.nsteps++]; memset(s, 0, sizeof *s);
    snprintf(s->tool, sizeof s->tool, "%s", tool); s->st = st; if (sql) snprintf(s->sql, sizeof s->sql, "%s", sql);
    va_list ap; va_start(ap, fmt); vsnprintf(s->detail, sizeof s->detail, fmt, ap); va_end(ap);
}
static char *chat_history(void);
/* the logo's main colours, found here (only hex codes go to the model, never the image) */
static int logo_colors(const char *path, uint32_t out[3]) {
    struct stat st; if (stat(path, &st) != 0 || st.st_size > 8 * 1024 * 1024) return 0;
    FILE *f = fopen(path, "rb"); if (!f) return 0;
    unsigned char *b = malloc((size_t)st.st_size); size_t got = fread(b, 1, (size_t)st.st_size, f); fclose(f);
    Image im; char err[200]; bool ok = image_decode(b, got, &im, err, sizeof err); free(b);
    if (!ok) return 0;
    static int hist[4096]; memset(hist, 0, sizeof hist);
    for (int i = 0; i < im.w * im.h; i++) {
        const unsigned char *p = im.rgba + (size_t)i * 4;
        if (p[3] < 128) continue;
        int mx = p[0] > p[1] ? (p[0] > p[2] ? p[0] : p[2]) : (p[1] > p[2] ? p[1] : p[2]), mn = p[0] < p[1] ? (p[0] < p[2] ? p[0] : p[2]) : (p[1] < p[2] ? p[1] : p[2]);
        if ((mn > 235) || (mx < 20)) continue;   /* near white / black backgrounds */
        hist[((p[0] >> 4) << 8) | ((p[1] >> 4) << 4) | (p[2] >> 4)]++;
    }
    image_free(&im);
    int n = 0;
    for (int k = 0; k < 3; k++) {
        int best = -1, bc = 0; for (int i = 0; i < 4096; i++) if (hist[i] > bc) { bc = hist[i]; best = i; }
        if (best < 0 || bc < 4) break;
        out[n++] = ((uint32_t)(((best >> 8) & 15) * 17) << 16) | ((uint32_t)(((best >> 4) & 15) * 17) << 8) | (uint32_t)((best & 15) * 17);
        hist[best] = 0;
    }
    return n;
}
static void design_summary(RvDoc *d, char *out, size_t n) {
    theme_of(d);
    const RTheme *t = &d->rt;
    char lp[1400]; theme_logo_path(d, lp, sizeof lp);
    uint32_t lc[3]; int nl = lp[0] ? logo_colors(lp, lc) : 0;
    size_t w = (size_t)snprintf(out, n, "{\"theme\":\"%s\",\"locked\":%s,\"accent\":\"#%06X\",\"background\":\"#%06X\",\"surface\":\"#%06X\",\"text\":\"#%06X\",\"header\":%s,\"headerBackground\":\"#%06X\",\"cardStyle\":\"%s\",\"kpiStyle\":\"%s\",\"radius\":%.0f,\"logo\":%s",
        t->name, t->locked ? "true" : "false", t->accent, t->bg, t->surface, t->text, t->header ? "true" : "false", t->header_bg, rtheme_card_style_name(t->card_style), rtheme_kpi_style_name(t->kpi_style), t->radius, lp[0] ? "true" : "false");
    if (nl && w < n) { w += (size_t)snprintf(out + w, n - w, ",\"logoColors\":["); for (int i = 0; i < nl && w < n; i++) w += (size_t)snprintf(out + w, n - w, "%s\"#%06X\"", i ? "," : "", lc[i]); if (w < n) w += (size_t)snprintf(out + w, n - w, "]"); }
    if (G.attach[0] && w < n) {
        uint32_t ac[3]; int na = logo_colors(G.attach, ac);
        w += (size_t)snprintf(out + w, n - w, ",\"attachment\":{\"name\":\"%s\",\"kind\":\"image\",\"colors\":[", G.attach_name);
        for (int i = 0; i < na && w < n; i++) w += (size_t)snprintf(out + w, n - w, "%s\"#%06X\"", i ? "," : "", ac[i]);
        if (w < n) w += (size_t)snprintf(out + w, n - w, "]}");
    }
    if (w < n) snprintf(out + w, n - w, "}");
}
/* ---- the database structure the user agreed to share: names, types, keys; never rows ---- */
static Connection *schema_connection(App *a) {
    if (G.schema_main[0]) { Connection *c = conn_of(a, G.schema_main); if (c) return c; }
    for (int i = 0; i < G.cat.nds; i++) { Connection *c = conn_of(a, G.cat.ds[i].conn); if (c) return c; }   /* the catalog's own connection */
    char t[128]; Connection *c = a->panels ? panels_tab_connection(a->panels, a, t, sizeof t) : NULL;   /* the active editor's */
    if (c) return c;
    for (int i = 0; a->conns && i < a->conns->n; i++) if (a->conns->items[i]->db) return a->conns->items[i];
    return a->conns && a->conns->n ? a->conns->items[0] : NULL;
}
/* the connection that runs the assistant's SQL: the read-only login mapped to c, else c itself */
static Connection *ai_exec_of(App *a, Connection *c) {
    if (c && c->ai_conn[0]) { Connection *x = conn_of(a, c->ai_conn); if (x) return x; }
    return c;
}
static void grants_check(Connection *x) {
    if (!x || !x->db) return;
    G.gr_state = caps_write_grants(x, G.gr_note, sizeof G.gr_note);
    snprintf(G.gr_conn, sizeof G.gr_conn, "%s", x->id);
    LOG_I("report: AI query connection %s: %s", x->name, G.gr_note);
}
static void sb_add(char **b, size_t *w, size_t *cap, const char *s) {
    size_t L = strlen(s);
    if (*w + L + 1 > *cap) { while (*w + L + 1 > *cap) *cap *= 2; *b = realloc(*b, *cap); }
    memcpy(*b + *w, s, L + 1); *w += L;
}
/* names come from the user's database: quotes and angle brackets are neutralised so they cannot close the block */
static void sb_name(char **b, size_t *w, size_t *cap, const char *s) { char t[300]; size_t k = 0; for (; *s && k + 1 < sizeof t; s++) t[k++] = (*s == '<' || *s == '>' || *s == '"') ? '_' : *s; t[k] = 0; sb_add(b, w, cap, t); }
/* columns whose values are personal: never sampled, whatever the consent */
static bool personal_column(const char *name) {
    static const char *const k[] = { "name", "nome", "mail", "cpf", "cnpj", "rg", "ssn", "phone", "fone", "celular", "mobile", "address", "endereco", "street", "rua", "cep", "zip", "postal",
                                     "birth", "nasc", "password", "senha", "token", "secret", "card", "cartao", "iban", "account", "conta", "document", "documento", "ip", "login", "user", "usuario", "lat", "lon", NULL };
    char f[128]; sem_fold(name, f, sizeof f);
    for (int i = 0; k[i]; i++) if (strstr(f, k[i])) return true;
    return false;
}
static bool texty(const char *type) {
    char f[64]; sem_fold(type ? type : "", f, sizeof f);
    return strstr(f, "char") || strstr(f, "text") || strstr(f, "string") || strstr(f, "enum") || !f[0];
}
/* up to 20 distinct values of a short text column, from a sample of its first 5000 rows; "" when there are more */
static void column_values(Connection *c, const char *table, const char *col, char *out, size_t on) {
    out[0] = 0;
    int dl = dialect_for_driver(c->driver);
    char qt[300], qc[160]; c->db->drv->quote_ident(table, qt, sizeof qt); c->db->drv->quote_ident(col, qc, sizeof qc);
    if (strchr(table, '.') && dl != DIALECT_SQLITE) {   /* schema-qualified: quote each part */
        char sch[128], tb[160]; const char *dot = strchr(table, '.'); snprintf(sch, sizeof sch, "%.*s", (int)(dot - table), table); snprintf(tb, sizeof tb, "%s", dot + 1);
        char a[140], b[170]; c->db->drv->quote_ident(sch, a, sizeof a); c->db->drv->quote_ident(tb, b, sizeof b); snprintf(qt, sizeof qt, "%s.%s", a, b);
    }
    char sql[900];
    if (dl == DIALECT_MSSQL) snprintf(sql, sizeof sql, "SELECT DISTINCT TOP 21 %s FROM (SELECT TOP 5000 %s FROM %s WHERE %s IS NOT NULL) s", qc, qc, qt, qc);
    else if (dl == DIALECT_ORACLE) snprintf(sql, sizeof sql, "SELECT DISTINCT %s FROM (SELECT %s FROM %s WHERE %s IS NOT NULL FETCH FIRST 5000 ROWS ONLY) FETCH FIRST 21 ROWS ONLY", qc, qc, qt, qc);
    else snprintf(sql, sizeof sql, "SELECT DISTINCT %s FROM (SELECT %s FROM %s WHERE %s IS NOT NULL LIMIT 5000) s LIMIT 21", qc, qc, qt, qc);
    ResultSet rs; c->db->drv->exec(c->db, sql, -1, 21, &rs);
    if (rs.ok && rs.nrows > 0 && rs.nrows <= 20) {
        size_t w = 0;
        for (int i = 0; i < rs.nrows && w + 50 < on; i++) {
            const char *v = rs_cell(&rs, i, 0); if (!v) continue;
            char t[44]; size_t k = 0; for (; *v && k < 40; v++) t[k++] = (*v == '<' || *v == '>' || *v == '\'' || *v == '\n') ? '_' : *v; t[k] = 0;
            w += (size_t)snprintf(out + w, on - w, "%s'%s'", w ? "," : "", t);
        }
    }
    rs_free(&rs);
}
static bool schema_build(Connection *c, char *why, int wn) {
    if (!c->db) { snprintf(why, (size_t)wn, "%s is not connected", c->name); return false; }
    if (c->db->busy) { snprintf(why, (size_t)wn, "%s is running a query: try again in a moment", c->name); return false; }
    size_t cap = 8192, w = 0; char *b = malloc(cap); b[0] = 0;
    char h[200]; snprintf(h, sizeof h, "<schema connection=\"%s\" dialect=\"%s\" untrusted=\"true\">\n", c->name, c->db->drv->name); sb_add(&b, &w, &cap, h);
    int nt = c->ntables < 120 ? c->ntables : 120, ncatv = 0;
    for (int i = 0; i < nt && w < 40000; i++) {
        DbTable *t = &c->tables[i];
        if (!t->cols_loaded) conn_load_columns(c, t);
        sb_add(&b, &w, &cap, t->is_view ? "view " : "table "); sb_name(&b, &w, &cap, t->name); sb_add(&b, &w, &cap, "(");
        for (int k = 0; k < t->ncols && k < 80; k++) {
            if (k) sb_add(&b, &w, &cap, ", ");
            sb_name(&b, &w, &cap, t->cols[k].name); sb_add(&b, &w, &cap, " "); sb_name(&b, &w, &cap, t->cols[k].type ? t->cols[k].type : "?");
            if (t->cols[k].pk) sb_add(&b, &w, &cap, " PK");
        }
        sb_add(&b, &w, &cap, ")\n");
        /* category values, only with their own consent: short text columns that are not personal */
        for (int k = 0; G.cx.on[CX_CATVALUES] && k < t->ncols && k < 80 && ncatv < 60; k++) {
            if (t->cols[k].pk || personal_column(t->cols[k].name) || !texty(t->cols[k].type)) continue;
            char vals[1100]; column_values(c, t->name, t->cols[k].name, vals, sizeof vals); ncatv++;
            if (!vals[0]) continue;
            sb_add(&b, &w, &cap, "  values "); sb_name(&b, &w, &cap, t->cols[k].name); sb_add(&b, &w, &cap, ": "); sb_add(&b, &w, &cap, vals); sb_add(&b, &w, &cap, "\n");
        }
        if (c->db->drv->list_fks && !t->is_view) {
            DbFk *fk = NULL; int nf = 0; char e[200];
            if (c->db->drv->list_fks(c->db, t->name, &fk, &nf, e, sizeof e)) {
                for (int k = 0; k < nf && k < 30; k++) { sb_add(&b, &w, &cap, "  fk "); sb_name(&b, &w, &cap, fk[k].from_col ? fk[k].from_col : "?"); sb_add(&b, &w, &cap, " -> "); sb_name(&b, &w, &cap, fk[k].to_table ? fk[k].to_table : "?"); sb_add(&b, &w, &cap, "."); sb_name(&b, &w, &cap, fk[k].to_col ? fk[k].to_col : "?"); sb_add(&b, &w, &cap, "\n"); }
                db_fks_free(fk, nf);
            }
        }
    }
    if (c->ntables > nt || w >= 40000) { char m[120]; snprintf(m, sizeof m, "(%d more tables not listed)\n", c->ntables - nt); sb_add(&b, &w, &cap, m); }
    sb_add(&b, &w, &cap, "</schema>");
    free(G.schema_text); G.schema_text = b;
    snprintf(G.schema_conn, sizeof G.schema_conn, "%s", c->id);
    return true;
}
static void chat_push(char role, const char *text);
/* ---- large databases (P0): the structure is read once, in the background, into a local index; each
 * question gets only the tables that matter (src/schidx.c). SQLite keeps the direct read above. ---- */
enum { SX_NONE, SX_BUILDING, SX_READY, SX_ERROR };
static const char *sx_dialect(const Connection *c) {
    if (!strcmp(c->driver, "postgres")) return "postgres";
    if (!strcmp(c->driver, "oracle")) return "oracle";
    if (!strcmp(c->driver, "mysql") || !strcmp(c->driver, "mariadb")) return "mysql";
    if (!strcmp(c->driver, "sqlserver")) return "mssql";
    return NULL;
}
static bool sx_supported(const Connection *c) { return c && sx_dialect(c) != NULL; }
static void sx_cache_path(App *a, const Connection *c, char *o, size_t n) {
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    char cd[1200]; snprintf(cd, sizeof cd, "%s/cache", dir); mkdir(cd, 0700);
    snprintf(o, n, "%s/schidx-%s.idx", cd, c->id);
}
static bool sx_ready_for(const Connection *c) { return c && G.sx_state == SX_READY && !strcmp(G.sx_conn, c->id); }
static void sx_reset(void) {
    if (G.sx_job) { exec_free(G.sx_job); G.sx_job = NULL; }
    for (int i = 0; i < G.sx_nst; i++) { free(G.sx_stmts[i]); G.sx_stmts[i] = NULL; }
    G.sx_nst = 0; schidx_free(&G.sx); G.sx_state = SX_NONE; G.sx_conn[0] = 0;
    for (int i = 0; i < G.ncatv; i++) { free(G.catv_key[i]); free(G.catv_val[i]); }
    G.ncatv = 0;
}
static void scope_path(App *a, char *o, size_t n) { char dir[1100]; pref_dir(a, dir, sizeof dir); snprintf(o, n, "%s/ai-scope.txt", dir); }
/* ai-scope.txt: "<connection id>\t<schemas|pin|block>\t<comma list>" per line; names only */
static void scope_load(App *a, const Connection *c) {
    if (G.scope_loaded && !strcmp(G.scope_conn, c->id)) return;
    G.scope_loaded = true; snprintf(G.scope_conn, sizeof G.scope_conn, "%s", c->id);
    G.scope_schemas[0] = G.scope_pin[0] = G.scope_block[0] = G.scope_consent[0] = 0; G.scope_has_consent = false;
    char p[1200]; scope_path(a, p, sizeof p);
    FILE *f = fopen(p, "r"); if (!f) goto fields;
    char line[700];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        char *t1 = strchr(line, '\t'); if (!t1) continue; *t1++ = 0;
        char *t2 = strchr(t1, '\t'); if (!t2) continue; *t2++ = 0;
        if (strcmp(line, c->id)) continue;
        if (!strcmp(t1, "schemas")) snprintf(G.scope_schemas, sizeof G.scope_schemas, "%s", t2);
        else if (!strcmp(t1, "pin")) snprintf(G.scope_pin, sizeof G.scope_pin, "%s", t2);
        else if (!strcmp(t1, "block")) snprintf(G.scope_block, sizeof G.scope_block, "%s", t2);
        else if (!strcmp(t1, "consent")) { snprintf(G.scope_consent, sizeof G.scope_consent, "%s", t2); G.scope_has_consent = true; }
    }
    fclose(f);
fields:
    textfield_set(&G.f_sch, G.scope_schemas); textfield_set(&G.f_pin, G.scope_pin); textfield_set(&G.f_blk, G.scope_block);
}
static void scope_save(App *a, const Connection *c) {
    char p[1200], tmp[1300]; scope_path(a, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *in = fopen(p, "r"); int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) { if (in) fclose(in); if (fd >= 0) close(fd); return; }
    char line[700];
    while (in && fgets(line, sizeof line, in)) { size_t L = strlen(c->id); if (!strncmp(line, c->id, L) && line[L] == '\t') continue; fputs(line, out); }
    if (in) fclose(in);
    for (char *q = G.scope_schemas; *q; q++) if (*q == '\t') *q = ' ';
    if (G.scope_schemas[0]) fprintf(out, "%s\tschemas\t%s\n", c->id, G.scope_schemas);
    if (G.scope_pin[0]) fprintf(out, "%s\tpin\t%s\n", c->id, G.scope_pin);
    if (G.scope_block[0]) fprintf(out, "%s\tblock\t%s\n", c->id, G.scope_block);
    if (G.scope_has_consent) fprintf(out, "%s\tconsent\t%s\n", c->id, G.scope_consent[0] ? G.scope_consent : "none");
    fclose(out); rename(tmp, p);
}
/* consents remembered per connection (structure, category values); the card's values stay per session */
static void consent_save(App *a, Connection *c) {
    if (!c) return;
    scope_load(a, c);
    snprintf(G.scope_consent, sizeof G.scope_consent, "%s%s%s", G.cx.on[CX_SCHEMA] ? "schema" : "", G.cx.on[CX_SCHEMA] && G.cx.on[CX_CATVALUES] ? "," : "", G.cx.on[CX_CATVALUES] ? "catvalues" : "");
    G.scope_has_consent = true;
    scope_save(a, c);
}
static void consent_apply(App *a, Connection *c) {
    if (!c) return;
    scope_load(a, c);
    if (!G.scope_has_consent) return;
    G.cx.on[CX_SCHEMA] = strstr(G.scope_consent, "schema") != NULL;
    G.cx.on[CX_CATVALUES] = strstr(G.scope_consent, "catvalues") != NULL;
}
/* "a, b ,c" -> trimmed items */
static int scope_items(const char *list, char out[][200], int max) {
    int n = 0; char buf[300]; snprintf(buf, sizeof buf, "%s", list);
    for (char *t = strtok(buf, ",;"); t && n < max; t = strtok(NULL, ",;")) { while (*t == ' ') t++; size_t L = strlen(t); while (L && t[L - 1] == ' ') t[--L] = 0; if (L) snprintf(out[n++], 200, "%s", t); }
    return n;
}
static bool scope_blocked(const SxTable *t) {
    char it[24][200]; int n = scope_items(G.scope_block, it, 24);
    char q[300]; snprintf(q, sizeof q, "%s.%s", t->schema, t->name);
    for (int i = 0; i < n; i++) if (!strcasecmp(it[i], t->name) || !strcasecmp(it[i], q)) return true;
    return false;
}
static int drop_blocked(SxPick *p, int n) { int k = 0; for (int i = 0; i < n; i++) if (!scope_blocked(&G.sx.t[p[i].t])) p[k++] = p[i]; return k; }
static int sql_tables(const char *q, char names[][200], int n, int max);
/* the user's own queries on this connection (query history, last 2000) teach the index which words go
 * with which tables; the text stays in the local index and is never sent */
static void sx_feed_history(App *a, const Connection *c) {
    char path[1200]; snprintf(path, sizeof path, "%shistory.tsv", a->conns ? a->conns->dir : "");
    FILE *f = fopen(path, "r"); if (!f) return;
    static char line[16384]; int fed = 0, seen = 0;
    while (fgets(line, sizeof line, f) && seen < 20000) {
        seen++;
        char *fl[6] = { 0 }; int k = 0; fl[k++] = line;
        for (char *p = line; *p && k < 6; p++) if (*p == '\t') { *p = 0; fl[k++] = p + 1; }
        if (k < 6 || strcmp(fl[1], c->name)) continue;
        char *sql = fl[5]; sql[strcspn(sql, "\r\n")] = 0;
        for (char *p = sql, *o = sql;; p++) { if (*p == '\\' && p[1] == 'n') { *o++ = ' '; p++; continue; } *o++ = *p; if (!*p) break; }
        char tb[16][200]; int nt = sql_tables(sql, tb, 0, 16);
        for (int i = 0; i < nt; i++) { int ti = schidx_find(&G.sx, NULL, tb[i]); if (ti >= 0) { schidx_add_usage(&G.sx, ti, sql); fed++; } }
    }
    fclose(f);
    if (fed) LOG_I("report: the structure index learned from %d use(s) of tables in your query history (local only; %d entries read)", fed, seen);
}
/* the index for c: from the disk cache when it is recent, else a background read (true when ready now) */
static bool sx_ensure(App *a, Connection *c, bool refresh) {
    if (!sx_supported(c)) return false;
    scope_load(G.a ? G.a : a, c);
    if (!refresh && sx_ready_for(c)) return true;
    if (G.sx_state == SX_BUILDING && !strcmp(G.sx_conn, c->id)) return false;
    sx_reset();
    snprintf(G.sx_conn, sizeof G.sx_conn, "%s", c->id);
    char cp[1300]; sx_cache_path(a, c, cp, sizeof cp);
    struct stat st;
    if (!refresh && stat(cp, &st) == 0 && time(NULL) - st.st_mtime < 12 * 3600 && schidx_load(&G.sx, cp)) {
        G.sx_state = SX_READY;
        LOG_I("report: structure index of %s loaded from the cache (%d tables)", c->name, G.sx.n);
        sx_feed_history(a, c);
        return true;
    }
    G.sx_state = SX_BUILDING; G.sx_t0 = now_s(); G.sx_err[0] = 0;
    return false;   /* the read starts in sx_pump, when the session is free */
}
static void sx_pump(App *a) {
    if (G.sx_state != SX_BUILDING) return;
    Connection *c = conn_of(a, G.sx_conn);
    if (!c) { G.sx_state = SX_ERROR; snprintf(G.sx_err, sizeof G.sx_err, "the connection is gone"); return; }
    if (!G.sx_job) {
        if (!c->db) { if (c->state == CONN_FAILED) { G.sx_state = SX_ERROR; snprintf(G.sx_err, sizeof G.sx_err, "%s: %s", c->name, c->error); } else if (c->state != CONN_CONNECTING && c->state != CONN_RECONNECTING) conn_connect_async(c, false); app_wake_in(a, 200); return; }
        if (c->db->busy || G.job) { app_wake_in(a, 120); return; }   /* one statement at a time per session */
        const char *dl = sx_dialect(c); int kind; char sql[16384], why[300];
        G.sx_nst = 0;
        char sch_s[24][200]; int nsch = scope_items(G.scope_schemas, sch_s, 24); const char *sch[24]; for (int i = 0; i < nsch; i++) sch[i] = sch_s[i];
        for (int k = 0; k < 3 && schidx_sql(dl, k, sch, nsch, &kind, sql, sizeof sql); k++) {
            if (!sqlguard_check(sql, dialect_for_driver(c->driver), why, sizeof why)) { LOG_W("report: catalog read refused by the read-only policy: %s", why); continue; }
            G.sx_stmts[G.sx_nst] = strdup(sql); G.sx_kinds[G.sx_nst] = kind; G.sx_nst++;
        }
        if (!G.sx_nst) { G.sx_state = SX_ERROR; snprintf(G.sx_err, sizeof G.sx_err, "no catalog read for this database"); return; }
        G.sx_job = exec_start_list(c->db, (const char *const *)G.sx_stmts, G.sx_nst, 2000000, false, false);
        if (!G.sx_job) { app_wake_in(a, 120); return; }
        LOG_I("report: reading the structure of %s in the background (%d catalog queries)", c->name, G.sx_nst);
        app_wake_in(a, 100); return;
    }
    if (exec_running(G.sx_job)) { app_wake_in(a, 100); return; }
    schidx_init(&G.sx); snprintf(G.sx.dialect, sizeof G.sx.dialect, "%s", sx_dialect(c));
    bool cols_ok = false;
    for (int i = 0; i < G.sx_nst; i++) {
        ResultSet rs; rs_init(&rs);
        if (!exec_take_result(G.sx_job, i, &rs)) continue;
        if (rs.error[0]) { if (G.sx_kinds[i] == SXQ_COLS) snprintf(G.sx_err, sizeof G.sx_err, "%s", rs.error); LOG_W("report: catalog read %d failed: %s", i, rs.error); rs_free(&rs); continue; }
        for (int r = 0; r < rs.nrows; r++) {
            const char *f[8]; for (int k = 0; k < 8; k++) f[k] = k < rs.ncols ? rs_cell(&rs, r, k) : NULL;
            if (G.sx_kinds[i] == SXQ_COLS && rs.ncols >= 8 && f[1] && f[3]) {
                int ti = schidx_table(&G.sx, f[0] ? f[0] : "", f[1], f[2] && atoi(f[2]) != 0);
                schidx_add_col(&G.sx, ti, f[3], f[4] ? f[4] : "?", f[5] && atoi(f[5]) != 0, f[6] && atoi(f[6]) != 0, f[7]);
            } else if (G.sx_kinds[i] == SXQ_TCOMMENT && rs.ncols >= 3 && f[1] && f[2]) {
                int ti = schidx_find(&G.sx, f[0] ? f[0] : "", f[1]); schidx_table_comment(&G.sx, ti, f[2]);
            } else if (G.sx_kinds[i] == SXQ_FKS && rs.ncols >= 5 && f[1] && f[4]) {
                int ti = schidx_find(&G.sx, f[0] ? f[0] : "", f[1]); schidx_add_fk(&G.sx, ti, f[2] ? f[2] : "", f[3], f[4]);
            }
        }
        if (G.sx_kinds[i] == SXQ_COLS) cols_ok = true;
        rs_free(&rs);
    }
    exec_free(G.sx_job); G.sx_job = NULL;
    for (int i = 0; i < G.sx_nst; i++) { free(G.sx_stmts[i]); G.sx_stmts[i] = NULL; } G.sx_nst = 0;
    if (!cols_ok || !G.sx.n) { G.sx_state = SX_ERROR; if (!G.sx_err[0]) snprintf(G.sx_err, sizeof G.sx_err, "the catalog returned no tables visible to this login"); LOG_W("report: structure index of %s failed: %s", c->name, G.sx_err); return; }
    schidx_finish(&G.sx);
    char cp[1300]; sx_cache_path(a, c, cp, sizeof cp);
    if (!schidx_save(&G.sx, cp)) LOG_W("report: the structure index could not be cached");
    G.sx_state = SX_READY;
    sx_feed_history(a, c);
    LOG_I("report: structure index of %s built: %d tables in %.1f s (names, types, keys, descriptions; no rows)", c->name, G.sx.n, now_s() - G.sx_t0);
    app_wake_in(a, 16);
}
/* the FROM / JOIN table names of a SQL text, appended to names[n..max) without duplicates */
static int sql_tables(const char *q, char names[][200], int n, int max) {
    for (const char *p = q; p && *p && n < max; p++) {
        bool kw = (!strncasecmp(p, "from", 4) || !strncasecmp(p, "join", 4)) && (p == q || !isalnum((unsigned char)p[-1])) && isspace((unsigned char)p[4]);
        if (!kw) continue;
        const char *b = p + 4; while (isspace((unsigned char)*b)) b++;
        size_t l = 0; char t[200];
        while (b[l] && (isalnum((unsigned char)b[l]) || b[l] == '_' || b[l] == '.' || b[l] == '$' || b[l] == '#') && l < sizeof t - 1) { t[l] = b[l]; l++; }
        t[l] = 0;
        if (l) { bool dup = false; for (int z = 0; z < n; z++) if (!strcasecmp(names[z], t)) dup = true; if (!dup) snprintf(names[n++], 200, "%s", t); }
    }
    return n;
}
/* tables the report already uses: the FROM / JOIN names of its SQL cards and the catalog's datasets */
static int sx_boost(RvDoc *d, char names[][200], int max) {
    int n = 0;
    for (int i = 0; d && i < G.cat.nds && n < max; i++) { snprintf(names[n], 200, "%s%s%s", G.cat.ds[i].schema, G.cat.ds[i].schema[0] ? "." : "", G.cat.ds[i].table); n++; }
    for (int pg = 0; d && pg < d->r.np; pg++)
        for (int k = 0; k < d->r.pages[pg].nv && n < max; k++) {
            const char *q = d->r.pages[pg].v[k].sql; if (!q) continue;
            for (const char *p = q; *p && n < max; p++) {
                bool kw = (!strncasecmp(p, "from", 4) || !strncasecmp(p, "join", 4)) && (p == q || !isalnum((unsigned char)p[-1])) && isspace((unsigned char)p[4]);
                if (!kw) continue;
                const char *b = p + 4; while (isspace((unsigned char)*b)) b++;
                size_t l = 0; char t[200];
                while (b[l] && (isalnum((unsigned char)b[l]) || b[l] == '_' || b[l] == '.' || b[l] == '$' || b[l] == '#') && l < sizeof t - 1) { t[l] = b[l]; l++; }
                t[l] = 0;
                if (l) { bool dup = false; for (int z = 0; z < n; z++) if (!strcasecmp(names[z], t)) dup = true; if (!dup) snprintf(names[n++], 200, "%s", t); }
            }
        }
    return n;
}
static const char *catv_cached(Connection *c, const char *table, const char *col, char *buf, size_t bn) {
    char key[400]; snprintf(key, sizeof key, "%s.%s", table, col);
    for (int i = 0; i < G.ncatv; i++) if (!strcmp(G.catv_key[i], key)) return G.catv_val[i];
    column_values(c, table, col, buf, bn);
    if (G.ncatv < 160) { G.catv_key[G.ncatv] = strdup(key); G.catv_val[G.ncatv] = strdup(buf); G.ncatv++; }
    return buf;
}
/* the <schema> text for this question from the index (and the results of the model's searches) */
/* the workspace glossary: <workspace>/glossary.txt (else <prefs>/glossary.txt), "term = where / meaning" per
 * line, # comments; reloaded when the file changes. It goes with every request and steers table picking. */
static const char *glossary_text(App *a) {
    const char *wd = a && a->toolbar ? toolbar_workspace_dir(a->toolbar) : "";
    char p[1200];
    if (wd && wd[0]) snprintf(p, sizeof p, "%sglossary.txt", wd); else { char dir[1100]; pref_dir(a, dir, sizeof dir); snprintf(p, sizeof p, "%s/glossary.txt", dir); }
    snprintf(G.gloss_path, sizeof G.gloss_path, "%s", p);
    struct stat st; if (stat(p, &st) != 0) { G.glossary[0] = 0; G.gloss_mtime = 0; return ""; }
    if ((double)st.st_mtime == G.gloss_mtime) return G.glossary;
    G.gloss_mtime = (double)st.st_mtime; G.glossary[0] = 0;
    FILE *f = fopen(p, "r"); if (!f) return "";
    char line[600]; size_t w = 0;
    while (fgets(line, sizeof line, f) && w + strlen(line) + 2 < sizeof G.glossary) {
        if (line[0] == '#' || !strchr(line, '=')) continue;
        for (char *q = line; *q; q++) if (*q == '<' || *q == '>') *q = ' ';
        w += (size_t)snprintf(G.glossary + w, sizeof G.glossary - w, "%s", line);
    }
    fclose(f);
    return G.glossary;
}
/* the question plus the definitions of the glossary terms it uses: table names written there are then picked */
static void glossary_expand(App *a, const char *q, char *out, size_t n) {
    size_t w = (size_t)snprintf(out, n, "%s", q);
    const char *g = glossary_text(a);
    char qt[32][32]; int nq = schidx_tokens(q, qt, 32);
    for (const char *ln = g; ln && *ln && w + 4 < n;) {
        const char *nl = strchr(ln, '\n'); size_t L = nl ? (size_t)(nl - ln) : strlen(ln);
        const char *eq = memchr(ln, '=', L);
        if (eq) {
            char term[200]; snprintf(term, sizeof term, "%.*s", (int)(eq - ln), ln);
            char tt[8][32]; int nt = schidx_tokens(term, tt, 8); bool hit = nt > 0;
            for (int i = 0; i < nt && hit; i++) { bool f = false; for (int k = 0; k < nq; k++) if (!strcmp(tt[i], qt[k])) f = true; hit = f; }
            if (hit) w += (size_t)snprintf(out + w, n - w, " %.*s", (int)(L - (size_t)(eq + 1 - ln)), eq + 1);
        }
        ln = nl ? nl + 1 : NULL;
    }
}
static int sql_tables(const char *q, char names[][200], int n, int max);
static void sx_schema_text_ex(App *a, Connection *c, RvDoc *d, const char *question, const char *editor_sql) {
    char boost_s[24][200]; int nb = scope_items(G.scope_pin, boost_s, 12);   /* pinned tables first */
    if (editor_sql) nb = sql_tables(editor_sql, boost_s, nb, 24);           /* then the tables of the query being edited */
    static char qx[4096]; glossary_expand(a, question, qx, sizeof qx); question = qx;   /* the glossary's definitions steer the picking */
    nb += sx_boost(d, boost_s + nb, 24 - nb);
    const char *boost[24]; for (int i = 0; i < nb; i++) boost[i] = boost_s[i];
    SxPick p[40]; SxQuery q = { question, boost, nb, 30 };
    int n = drop_blocked(p, schidx_pick(&G.sx, &q, p, 40)), shown = 0;
    char *t = schidx_render(&G.sx, p, n, c->name, 36000, &shown);
    G.est_tables = shown;
    size_t cap = strlen(t) + 9000 + (G.sx_extra ? strlen(G.sx_extra) : 0), w = 0;
    char *b = malloc(cap); w = (size_t)snprintf(b, cap, "%s", t); free(t);
    if (G.cx.on[CX_CATVALUES]) {   /* category values: only the picked tables, read once per session */
        int nc = 0;
        for (int i = 0; i < shown && i < 10 && nc < 24; i++) {
            const SxTable *tb = &G.sx.t[p[i].t];
            for (int k = 0; k < tb->ncols && nc < 24; k++) {
                const SxCol *cl = &tb->cols[k];
                if (cl->pk || personal_column(cl->name) || !texty(cl->type)) continue;
                char qn[300], vals[1100]; snprintf(qn, sizeof qn, "%s%s%s", tb->schema, tb->schema[0] ? "." : "", tb->name);
                const char *v = catv_cached(c, qn, cl->name, vals, sizeof vals); nc++;
                if (v[0] && w + strlen(v) + 300 < cap) w += (size_t)snprintf(b + w, cap - w, "\n<values table=\"%s\" column=\"%s\">%s</values>", qn, cl->name, v);
            }
        }
    }
    if (G.sx_extra && w + strlen(G.sx_extra) + 4 < cap) w += (size_t)snprintf(b + w, cap - w, "\n%s", G.sx_extra);
    free(G.schema_text); G.schema_text = b; G.est_bytes = (int)strlen(b);
    snprintf(G.schema_conn, sizeof G.schema_conn, "%s", c->id);
    LOG_I("report: %d of %d tables picked for the question (%zu bytes)", shown, G.sx.n, strlen(b));
}
static void sx_schema_text(App *a, Connection *c, RvDoc *d, const char *question) { sx_schema_text_ex(a, c, d, question, NULL); }
/* the model asked for more of the structure: answered from the index, then the request goes again */
static bool sx_answer_search(App *a, RvDoc *d) {
    Connection *c = conn_of(a, G.sx_conn);
    if (!c || !sx_ready_for(c)) return false;
    if (G.sx_rounds >= 2) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant did not find the tables it needed after two searches of the structure. Name the tables (or add their descriptions / the glossary) and ask again."); return true; }
    SxPick p[30]; int n = 0; char head[400];
    size_t cap = 30000; char *b = malloc(cap); size_t w = 0;
    if (G.ans.type == AA_SEARCH_SCHEMA) {
        n = drop_blocked(p, schidx_search(&G.sx, G.ans.terms, p, 25));
        char *l = schidx_render_list(&G.sx, p, n, 12000);
        snprintf(head, sizeof head, "Searched the structure for \"%s\": %d table(s)", G.ans.terms, n);
        w = (size_t)snprintf(b, cap, "<schema_search terms=\"%s\" untrusted=\"true\">\n%s%s</schema_search>", G.ans.terms, l ? l : "", n ? "" : "(nothing matched)\n");
        free(l);
    } else {
        char list[300]; snprintf(list, sizeof list, "%s", G.ans.terms);
        int nf = 0;
        for (char *tok = strtok(list, ","); tok && n < 12; tok = strtok(NULL, ",")) { int ti = schidx_find(&G.sx, NULL, tok); if (ti >= 0 && scope_blocked(&G.sx.t[ti])) { nf++; continue; } if (ti >= 0) { p[n].t = ti; p[n].score = 1; p[n].why[0] = 0; n++; } else nf++; }
        int shown = 0; char *t = schidx_render(&G.sx, p, n, c->name, 20000, &shown);
        char *body = strstr(t, "\n"); body = body ? body + 1 : t;
        char *end = strstr(body, "</schema>"); if (end) *end = 0;
        snprintf(head, sizeof head, "Described %d table(s)%s", n, nf ? " (some names were not found)" : "");
        w = (size_t)snprintf(b, cap, "<schema_search tables=\"%s\" untrusted=\"true\">\n%s%s</schema_search>", G.ans.terms, body, nf ? "(some names are not in the index)\n" : "");
        free(t);
    }
    (void)w;
    for (char *q = b; *q; q++) if (*q == '"' && 0) *q = '\'';
    if (G.sx_extra) { size_t L = strlen(G.sx_extra) + strlen(b) + 2; char *m = malloc(L); snprintf(m, L, "%s\n%s", G.sx_extra, b); free(G.sx_extra); free(b); G.sx_extra = m; }
    else G.sx_extra = b;
    G.sx_rounds++;
    LOG_I("report: %s answered from the local structure index: %d table(s), nothing queried", G.ans.type == AA_SEARCH_SCHEMA ? "search_schema" : "describe_tables", n);
    add_step(G.ans.type == AA_SEARCH_SCHEMA ? "search_schema" : "describe_tables", ST_DONE, NULL, "%s (local index, nothing queried)", head);
    chat_push('a', head);
    audit(a, G.ans.type == AA_SEARCH_SCHEMA ? "search_schema" : "describe_tables", d, "local", strlen(G.sx_extra), "");
    G.ast = AS_IDLE; G.amsg[0] = 0; G.resend = true; G.choice[0] = 0;
    if (!G.q.buf[0]) textfield_set(&G.q, G.sent_q);
    ai_send(a, d);
    return true;
}
static bool active_query_tab(App *a, TabInfo *out);
static void build_sql_payload(App *a, const TabInfo *ti);
static bool html_build(App *a, RvDoc *d, bool fake, HB *out, char *why, int wn);
static void build_payload(App *a, RvDoc *d) {
    { TabInfo ti; if (active_query_tab(a, &ti)) { build_sql_payload(a, &ti); return; } }
    free(G.payload); G.payload = NULL;
    static char q[1600]; snprintf(q, sizeof q, "%s%s%s", G.q.buf[0] ? G.q.buf : G.sent_q, G.choice[0] ? "\nClarified by the user: " : "", G.choice);
    G.cx.question = q; G.cx.history = chat_history(); G.cx.glossary = glossary_text(a);
    bool review = G.design_review && d; G.design_review = false;
    static char design[800]; if (d) design_summary(d, design, sizeof design); else design[0] = 0; G.cx.design_text = design;
    if (G.cx.on[CX_SCHEMA]) {
        Connection *sc = ai_exec_of(a, schema_connection(a)); char sw[200];
        if (sc && sx_supported(sc) && sx_ready_for(sc)) sx_schema_text(a, sc, d, q);   /* large databases: the tables of this question */
        else if (sc && !sx_supported(sc)) schema_build(sc, sw, sizeof sw);              /* fresh at every send, as the AI's login sees it */
    }
    G.cx.schema_text = G.cx.on[CX_SCHEMA] && G.schema_text ? G.schema_text : ""; G.cx.card_id = d && d->sel[0] ? d->sel : "";
    /* aggregated values of the selected card, only when the user ticked them (LI-032.2) */
    static char vals[2048]; vals[0] = 0;
    if (d && G.cx.on[CX_VALUES] && d->sel[0]) {
        int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg); char why[300];
        RvData *x = v ? visual_data(a, d, v, false, why, sizeof why) : NULL;
        if (x && x->status == RD_OK) { int w = snprintf(vals, sizeof vals, "["); for (int i = 0; i < x->rs.nrows && i < 60 && w < (int)sizeof vals - 80; i++) { const char *l = x->rs.ncols > 1 ? rs_cell(&x->rs, i, 0) : ""; const char *val = rs_cell(&x->rs, i, x->rs.ncols - 1); w += snprintf(vals + w, sizeof vals - (size_t)w, "%s[\"%s\",%s]", i ? "," : "", l ? l : "", val ? val : "null"); } snprintf(vals + w, sizeof vals - (size_t)w, "]"); }
    }
    G.cx.values_json = vals;
    G.payload = ag_message(&G.cat, d ? &d->r : NULL, d ? d->page : 0, &G.cx);
    if (review) {   /* phase 3: the page as HTML with placeholder data, so the model can judge the layout */
        HB hp = { 0 }; char hw[300];
        if (html_build(a, d, true, &hp, hw, sizeof hw)) {
            const char *note = "\n<design_review>The HTML in <report_html> is this report rendered with PLACEHOLDER data: every number and category label is fake, only the layout, titles, card kinds and theme are real. "
                               "Judge the design: hierarchy, card sizes and positions on the 12-column grid, readability, whether each chart fits its data, consistency with the theme. "
                               "Answer with a proposal of update_visual / move_visual / set_theme actions (never data or SQL changes), and say why in the rationale.</design_review>\n";
            size_t L = strlen(G.payload) + hp.w + strlen(note) + 80; char *m = malloc(L);
            snprintf(m, L, "%s\n<report_html placeholders=\"true\" untrusted=\"true\">\n%s</report_html>%s", G.payload, hp.b, note);
            free(G.payload); G.payload = m;
            audit(a, "design_review", d, "placeholder-html", hp.w, "");
        }
        free(hp.b);
    }
    G.cx.question = ""; G.cx.card_id = "";
}
/* ---- conversation ---- */
/* greedy word wrap of text into lines that fit width: starts / lengths in bytes; '\n' always breaks */
static int wrap_text(Font *f, const char *t, float width, int *st, int *ln, int max) {
    int n = 0, i = 0, L = (int)strlen(t);
    while (i <= L && n < max) {
        int s0 = i, last_space = -1, j = i;
        while (j < L && t[j] != '\n') {
            int k = j + 1; while (k < L && ((unsigned char)t[k] & 0xC0) == 0x80) k++;   /* whole UTF-8 characters */
            if (font_width(f, t + s0, k - s0) > width && j > s0) break;
            if (t[j] == ' ') last_space = j;
            j = k;
        }
        int e = j;
        if (j < L && t[j] != '\n' && last_space > s0) e = last_space;   /* break at the last space that fits */
        st[n] = s0; ln[n] = e - s0; n++;
        if (e >= L) break;
        i = (t[e] == '\n' || t[e] == ' ') ? e + 1 : e;
    }
    return n;
}
static const char *msg_text(int m) { return m < G.nchat ? G.chat[m].text : G.amsg; }
static bool sel_before(int am, int ao, int bm, int bo) { return am < bm || (am == bm && ao <= bo); }
/* the selected text (malloc'd), messages separated by a blank line */
static char *sel_text(void) {
    if (!G.sel_on) return NULL;
    int am = G.sel_am, ao = G.sel_ao, bm = G.sel_bm, bo = G.sel_bo;
    if (!sel_before(am, ao, bm, bo)) { int t1 = am; am = bm; bm = t1; t1 = ao; ao = bo; bo = t1; }
    size_t cap = 256, w = 0; char *o = malloc(cap); o[0] = 0;
    for (int m = am; m <= bm && m <= G.nchat; m++) {
        const char *t = msg_text(m); int L = (int)strlen(t);
        int a0 = m == am ? ao : 0, b0 = m == bm ? bo : L; if (a0 > L) a0 = L; if (b0 > L) b0 = L; if (b0 < a0) b0 = a0;
        size_t need = (size_t)(b0 - a0) + 4;
        if (w + need >= cap) { while (w + need >= cap) cap *= 2; o = realloc(o, cap); }
        if (m != am) { memcpy(o + w, "\n\n", 2); w += 2; }
        memcpy(o + w, t + a0, (size_t)(b0 - a0)); w += (size_t)(b0 - a0); o[w] = 0;
    }
    return o;
}
/* draws a message's text at (x, y) within width, with the selection, and handles press / drag on it; returns its height */
static float sel_text_block(App *a, SDL_Renderer *r, int m, const char *t, float x, float y, float width, Color col, Rect clip, bool *hit) {
    Font *f = a->font_small; float lh = font_height(f);
    static int st[400], ln[400];
    int n = wrap_text(f, t, width, st, ln, 400);
    int am = G.sel_am, ao = G.sel_ao, bm = G.sel_bm, bo = G.sel_bo;
    if (!sel_before(am, ao, bm, bo)) { int t1 = am; am = bm; bm = t1; t1 = ao; ao = bo; bo = t1; }
    for (int i = 0; i < n; i++) {
        float ly = y + i * lh;
        if (G.sel_on && m >= am && m <= bm) {   /* selection highlight of this line */
            int s0 = st[i], e0 = st[i] + ln[i];
            int a0 = m == am ? ao : 0, b0 = m == bm ? bo : 1 << 30;
            int x0 = a0 > s0 ? a0 : s0, x1 = b0 < e0 ? b0 : e0;
            bool eol = b0 > e0 && i < n - 1;
            if (x1 > x0 || (eol && x0 <= e0)) {
                float px0 = x + font_width(f, t + s0, x0 - s0), px1 = x + font_width(f, t + s0, (x1 > x0 ? x1 : x0) - s0) + (eol ? px(a, 4) : 0);
                draw_rect(r, rect_make(px0, ly, px1 - px0, lh), C(T_SELECTED));
            }
        }
        font_draw(f, r, t + st[i], ln[i], x, ly, col);
    }
    float h = n * lh;
    /* hit test: the offset under the pointer */
    float mx = a->in.mouse_x, my = a->in.mouse_y;
    bool over = mx >= x - px(a, 6) && mx <= x + width + px(a, 6) && my >= y && my < y + h && rect_contains(clip, mx, my);
    if (hit) *hit = over;
    if (over || (G.sel_drag && a->in.mouse_down)) {
        int li = (int)((my - y) / lh); if (li < 0) li = 0; if (li >= n) li = n - 1;
        int off = st[li], e = st[li] + ln[li];
        while (off < e) { int k = off + 1; while (k < e && ((unsigned char)t[k] & 0xC0) == 0x80) k++; float wpos = font_width(f, t + st[li], k - st[li]); if (x + wpos - (wpos - font_width(f, t + st[li], off - st[li])) / 2 > mx) break; off = k; }
        if (my < y) off = 0;
        if (my >= y + h) off = (int)strlen(t);
        if (over && a->in.mouse_pressed && !a->in.mouse_consumed) {
            a->in.mouse_consumed = true; focus_id = 0;
            if (a->in.clicks >= 2) {   /* a word */
                int w0 = off, w1 = off, L = (int)strlen(t);
                while (w0 > 0 && t[w0 - 1] != ' ' && t[w0 - 1] != '\n') w0--;
                while (w1 < L && t[w1] != ' ' && t[w1] != '\n') w1++;
                G.sel_am = G.sel_bm = m; G.sel_ao = w0; G.sel_bo = w1; G.sel_on = w1 > w0; G.sel_drag = false;
            } else { G.sel_am = G.sel_bm = m; G.sel_ao = G.sel_bo = off; G.sel_on = false; G.sel_drag = true; }
        } else if (G.sel_drag && a->in.mouse_down && (over || (my >= y && my < y + h))) {
            G.sel_bm = m; G.sel_bo = off; G.sel_on = G.sel_am != G.sel_bm || G.sel_ao != G.sel_bo;
        }
    }
    return h;
}
static float g_card_top;   /* the turn's state card, from the top of the conversation */
static void chat_clear(void) { G.tok_in = G.tok_out = G.nreq = 0; for (int i = 0; i < G.nchat; i++) free(G.chat[i].text); G.nchat = G.turn_start = 0; G.chat_scroll = 0; G.chat_stick = true; G.sel_on = G.sel_drag = false; }
static void chat_push(char role, const char *text) {
    if (!text || !*text) return;
    if (G.nchat == 48) { free(G.chat[0].text); memmove(&G.chat[0], &G.chat[1], sizeof G.chat[0] * 47); G.nchat--; if (G.turn_start > 0) G.turn_start--; }
    G.chat[G.nchat].role = role; G.chat[G.nchat].text = strdup(text); G.nchat++;
    G.sel_on = G.sel_drag = false;   /* indices moved */
    G.chat_stick = true;
}
/* the finished turn becomes a message in the history: what the assistant said, its choices, its plan */
static void tok_fmt(int n, char *o, size_t on) { if (n >= 10000) snprintf(o, on, "%.1fk", n / 1000.0); else snprintf(o, on, "%d", n); }
static void chat_push(char role, const char *text);
/* the tokens of the answer just shown, as a small line under it ("t" entries: not a message, never sent back) */
static void push_tokens(void) {
    if (!G.tok_pending) return;
    G.tok_pending = false;
    char a1[16], a2[16], a3[16], line[160]; tok_fmt(G.last_tin, a1, sizeof a1); tok_fmt(G.last_tout, a2, sizeof a2); tok_fmt(G.tok_in + G.tok_out, a3, sizeof a3);
    snprintf(line, sizeof line, "\xE2\x86\x91 %s in \xC2\xB7 \xE2\x86\x93 %s out tokens \xC2\xB7 conversation %s", a1, a2, a3);
    chat_push('t', line);
}
/* a choice (radio row): a ring, filled when chosen, and the whole name; clearly not a button */
static bool radio_row(App *a, Rect rc, const char *label, bool on, bool en) {
    SDL_Renderer *r = a->renderer;
    bool hov = en && rect_contains(rc, a->in.mouse_x, a->in.mouse_y);
    if (hov) draw_rect(r, rc, C(T_HOVER));
    float d = px(a, 14), cx = rc.x + px(a, 6), cy = rc.y + (rc.h - d) / 2;
    draw_rrect(r, rect_make(cx, cy, d, d), d / 2, C(on ? T_ACCENT : T_TEXT_DIM));
    draw_rrect(r, rect_make(cx + px(a, 2), cy + px(a, 2), d - px(a, 4), d - px(a, 4)), (d - px(a, 4)) / 2, C(hov ? T_HOVER : T_PANEL));
    if (on) draw_rrect(r, rect_make(cx + px(a, 4), cy + px(a, 4), d - px(a, 8), d - px(a, 8)), (d - px(a, 8)) / 2, C(T_ACCENT));
    ui_label(a, rect_make(cx + d + px(a, 8), rc.y, rc.w - d - px(a, 14), rc.h), label, ALIGN_LEFT, en ? T_TEXT : T_TEXT_DIM);
    bool click = hov && a->in.mouse_pressed && !a->in.mouse_consumed;
    if (click) a->in.mouse_consumed = true;
    return click;
}
static void chat_archive_live(void) {
    if (G.ast == AS_IDLE && !G.amsg[0]) return;
    if (G.ast == AS_THINKING) return;
    char b[4096]; size_t w = 0; b[0] = 0;
    #define AP(...) do { if (w < sizeof b) w += (size_t)snprintf(b + w, sizeof b - w, __VA_ARGS__); } while (0)
    if (G.amsg[0]) AP("%s", G.amsg);
    if (G.ast == AS_CLARIFY || G.ast == AS_LOCAL_CLARIFY)
        for (int i = 0; i < G.nopt; i++) { const SemMetricDef *md = sem_cur(sem_metric_c(&G.cat, G.opts[i])); AP("\n\xE2\x80\xA2 %s", md ? md->name : G.opts[i]); }
    if ((G.ast == AS_PLAN || G.ast == AS_QUERYING || G.ast == AS_DONE || G.ast == AS_CANCELLED) && G.ans.rationale[0]) AP("%sWhy these charts: %s", w ? "\n" : "", G.ans.rationale);
    for (int i = 0; i < G.nsteps && i < 8; i++) AP("\n[%s] %s: %.160s", ST_NAME[G.steps[i].st], G.steps[i].tool, G.steps[i].detail);
    if (G.ast == AS_PLAN) AP("\n(not approved: nothing was changed)");
    #undef AP
    chat_push('a', b);
    G.amsg[0] = 0;
    push_tokens();
}
/* earlier turns for the model (the user's words and the assistant's replies, newest last), at most ~4 KB */
static char *chat_history(void) {
    static char h[4600]; h[0] = 0;
    int from = G.turn_start; size_t total = 0;
    while (from > 0 && total + strlen(G.chat[from - 1].text) < 4000 && G.turn_start - from < 10) { total += strlen(G.chat[from - 1].text) + 12; from--; }
    size_t w = 0;
    for (int i = from; i < G.turn_start; i++) if (G.chat[i].role != 't') w += (size_t)snprintf(h + w, sizeof h - w, "%s: %s\n", G.chat[i].role == 'u' ? "User" : "Assistant", G.chat[i].text);
    return h;
}

/* ---- the assistant in a query tab (roadmap P0 foundation 8-10, P1 12-14) ---- */
static bool active_query_tab(App *a, TabInfo *out) {
    if (!a->panels || panels_active_report(a->panels, a) || panels_active_designer(a->panels, a)) return false;
    int n = panels_tab_count(a->panels);
    for (int k = 0; k < n; k++) {
        TabInfo ti; if (!panels_tab_info(a->panels, a, k, &ti)) continue;
        if (ti.active && ti.ed) { if (out) *out = ti; return true; }
    }
    return false;
}
/* the tab's last run, when it failed: the Output log of the active connection, its latest execution */
static bool tab_last_error(App *a, const Connection *c, char *err, size_t en, char *sql, size_t sn, char *cols, size_t coln) {
    (void)c;
    if (en) err[0] = 0; if (sn) sql[0] = 0; if (coln) cols[0] = 0;
    OutLog *l = a->output ? output_current(a->output, a) : NULL;
    if (!l || !l->n) return false;
    int last = l->ev[l->n - 1].exec_id;
    for (int i = l->n - 1; i >= 0 && l->ev[i].exec_id == last; i--) {
        const OutEvent *e = &l->ev[i];
        if (e->type != OT_STMT_FAILED) continue;
        snprintf(err, en, "%s", e->message ? e->message : "");
        if (e->sql) snprintf(sql, sn, "%s", e->sql);
        return err[0] != 0;
    }
    return false;
}
static void build_sql_payload(App *a, const TabInfo *ti) {
    free(G.payload); G.payload = NULL;
    Connection *c = conn_of(a, ti->conn_id);
    static char q[1600]; snprintf(q, sizeof q, "%s%s%s", G.q.buf[0] ? G.q.buf : G.sent_q, G.choice[0] ? "\nClarified by the user: " : "", G.choice);
    int len = 0, sl = 0, slen = 0, sstart = -1;
    char *text = G.cx.on[CX_EDITOR] ? editor_text(ti->ed, &len) : NULL;
    char *sel = G.cx.on[CX_EDITOR] ? editor_selection(ti->ed, &sl) : NULL;
    char *stmt = editor_statement_at_cursor_pos(ti->ed, &slen, &sstart);
    G.qrev = editor_revision(ti->ed);
    G.qsel_s = sel && sl > 0 ? editor_selection_start(ti->ed) : -1; G.qsel_e = G.qsel_s >= 0 ? G.qsel_s + sl : -1;
    G.qstmt_s = stmt ? sstart : -1; G.qstmt_e = stmt ? sstart + slen : -1;
    free(stmt);
    if (text && len > 60000) { text[60000] = 0; }
    static char err[600], lsql[2000], cols[1200]; err[0] = lsql[0] = cols[0] = 0;
    if (G.cx.on[CX_EDITOR]) tab_last_error(a, c, err, sizeof err, lsql, sizeof lsql, cols, sizeof cols);
    if (G.cx.on[CX_EDITOR] && !err[0] && a->results) { char e2[8], s2[8], cn[64]; results_current_info(a->results, e2, sizeof e2, s2, sizeof s2, cn, sizeof cn, cols, sizeof cols); if (c && strcmp(cn, c->name)) cols[0] = 0; }
    const char *schema = "";
    if (G.cx.on[CX_SCHEMA] && c) {
        Connection *sc = ai_exec_of(a, c); char sw[200];
        if (sx_supported(sc) && sx_ready_for(sc)) { sx_schema_text_ex(a, sc, NULL, q, text); schema = G.schema_text ? G.schema_text : ""; }
        else if (!sx_supported(sc)) { schema_build(sc, sw, sizeof sw); schema = G.schema_text ? G.schema_text : ""; }
    }
    char *hist = chat_history();
    static char summ[6000]; summ[0] = 0;
    if (G.cx.on[CX_RESULT] && a->results) {   /* only a result of this tab's connection */
        char e3[8], s3[8], cn3[64], c3[8];
        if (results_current_info(a->results, e3, sizeof e3, s3, sizeof s3, cn3, sizeof cn3, c3, sizeof c3) && c && !strcmp(cn3, c->name)) results_current_summary(a->results, summ, sizeof summ);
    }
    char *plan = G.cx.on[CX_EDITOR] && c ? planview_context_for(c->name) : NULL;   /* the plan open for this connection, if any */
    AgSqlCtx x = { q, hist, c ? c->driver : "", c ? c->name : "", ti->title,
                   text ? text : (G.cx.on[CX_EDITOR] ? "" : NULL), sel && sl > 0 ? sel : NULL, err, lsql, cols[0] ? cols : NULL, summ[0] ? summ : NULL, plan, G.vstate == 3 && G.vfb[0] ? G.vfb : NULL, schema, glossary_text(a)[0] ? glossary_text(a) : NULL };
    G.payload = ag_sql_message(&x);
    free(plan);
    free(text); free(sel);   /* hist is chat_history()'s static buffer */
}
/* does any statement of this text change data or structure? (several SELECTs are fine here: the editor runs them one by one) */
static bool sql_writes(const char *sql, int dialect, char *why, size_t wn) {
    size_t L = strlen(sql); char *b = malloc(L + 1); size_t k = 0; bool q1 = false, q2 = false, any = false;
    for (size_t i = 0; i <= L; i++) {
        char c = sql[i];
        if (c == '\'' && !q2) q1 = !q1; else if (c == '"' && !q1) q2 = !q2;
        if (c == '-' && sql[i + 1] == '-' && !q1 && !q2) { while (sql[i] && sql[i] != '\n') i++; c = sql[i]; }
        if ((c == ';' && !q1 && !q2) || !c) {
            b[k] = 0; char *t = b; while (*t && isspace((unsigned char)*t)) t++;
            if (*t && !sqlguard_check(t, dialect, why, (int)wn)) { any = true; break; }
            k = 0; if (!c) break; continue;
        }
        b[k++] = c;
    }
    free(b);
    return any;
}
/* ---- validate (BL-056): plans, hypothetical index, same result, time; numbers back to the model ---- */
enum { VK_PLAN_O, VK_PLAN_C, VK_PLAN_I, VK_RUN_O, VK_RUN_C, VK_DONE };
static void vfb(const char *fmt, ...) __attribute__((format(printf, 1, 2)));
static void vfb(const char *fmt, ...) { if (G.vfbw >= sizeof G.vfb - 2) return; va_list ap; va_start(ap, fmt); int n = vsnprintf(G.vfb + G.vfbw, sizeof G.vfb - G.vfbw, fmt, ap); va_end(ap); if (n > 0) G.vfbw += (size_t)n; if (G.vfbw > sizeof G.vfb - 1) G.vfbw = sizeof G.vfb - 1; }
static void v_free_stmts(void) { for (int i = 0; i < G.vnst; i++) { free(G.vst[i]); G.vst[i] = NULL; } G.vnst = 0; }
static void v_reset(void) {
    if (G.vjob) { exec_free(G.vjob); G.vjob = NULL; }
    v_free_stmts(); free(G.vorig); free(G.vcand); G.vorig = G.vcand = NULL;
    G.vstate = 0; G.vfbw = 0; G.vfb[0] = 0; G.vline[0] = 0; G.vapproved = false;
}
/* the statements of a step; false when this step does not apply (then it is skipped) */
static bool v_prepare(Connection *c, int kind, char *why, size_t wn) {
    v_free_stmts();
    int pd = plan_dialect_for_driver(c->driver), dl = dialect_for_driver(c->driver);
    const char *sql = kind == VK_PLAN_O || kind == VK_RUN_O ? G.vorig : G.vcand;
    if (kind <= VK_PLAN_I) {
        if (pd < 0) { snprintf(why, wn, "no execution plans for %s", c->driver); return false; }
        if (kind == VK_PLAN_I && (!G.vidx[0] || strcmp(c->driver, "postgres"))) return false;
        PlanSql ps; if (!plan_capture_sql(pd, PM_ESTIMATED, sql, 0, &ps, why, (int)wn)) return false;
        int k = 0;
        if (kind == VK_PLAN_I) {   /* the candidate with the hypothetical index: created for this EXPLAIN only, then reset */
            char esc[700]; size_t w = 0; for (const char *q = G.vidx; *q && w + 3 < sizeof esc; q++) { if (*q == '\'') esc[w++] = '\''; esc[w++] = *q; } esc[w] = 0;
            size_t L = strlen(esc) + 64; G.vst[k] = malloc(L); snprintf(G.vst[k], L, "SELECT * FROM hypopg_create_index('%s')", esc); k++;
        }
        for (int i = 0; i < ps.n && k < 7; i++) G.vst[k++] = strdup(ps.sql[i]);
        G.vres = ps.result + (kind == VK_PLAN_I ? 1 : 0); G.vfin = ps.finally_last;
        if (kind == VK_PLAN_I) { G.vst[k++] = strdup("SELECT hypopg_reset()"); G.vfin = true; }
        G.vnst = k; plan_sql_free(&ps);
        return true;
    }
    /* a run: inside Lume's read-only envelope, with a timeout; refused when it could write */
    if (!sqlguard_check(sql, dl, why, (int)wn)) return false;
    char note[300]; char *st[6]; int res = 0; bool fin = false;
    int n = sqlguard_wrap(dl, sql, 60, st, 6, &res, &fin, note, sizeof note);
    for (int i = 0; i < n; i++) G.vst[i] = st[i];
    G.vnst = n; G.vres = res; G.vfin = fin;
    return n > 0;
}
static unsigned long long row_hash(const ResultSet *rs, int r) {
    unsigned long long h = 1469598103934665603ULL;
    for (int c = 0; c < rs->ncols; c++) { const char *v = rs_cell(rs, r, c); for (const char *q = v ? v : "\x01NULL"; *q; q++) { h ^= (unsigned char)*q; h *= 1099511628211ULL; } h ^= 0x1f; h *= 1099511628211ULL; }
    return h;
}
static const char *const VK_NAME[] = { "original plan", "candidate plan", "candidate plan with the hypothetical index", "original run", "candidate run" };
static void v_finish_step(App *a, Connection *c) {
    int n = G.vjob->n; ResultSet rs[8]; ResultSet *rp[8]; char err[400] = "";
    for (int i = 0; i < n && i < 8; i++) { rs_init(&rs[i]); exec_take_result(G.vjob, i, &rs[i]); rp[i] = &rs[i]; if (!err[0] && rs[i].error[0] && (i == G.vres || i != n - 1)) snprintf(err, sizeof err, "%s", rs[i].error); }
    int k = G.vkind;
    if (err[0]) vfb("%s: failed: %s\n", VK_NAME[k], err);
    else if (k <= VK_PLAN_I) {
        Plan p; plan_init(&p); char perr[300] = "";
        if (plan_parse(plan_dialect_for_driver(c->driver), PM_ESTIMATED, rp, n, G.vres, &p, perr, sizeof perr)) {
            char sum[700], cost[48]; plan_summary(&p, sum, sizeof sum); plan_num_fmt(p.total_cost, cost, sizeof cost, 2);
            snprintf(G.vcost[k], sizeof G.vcost[k], "%s", cost);
            PlanFinding f[16]; int nf = plan_diagnose(&p, NULL, f, 16);
            vfb("%s: total cost %s, %d operator(s); %s\n", VK_NAME[k], cost, p.n, sum);
            for (int i = 0; i < nf && i < 6; i++) vfb("  finding: %s | %s\n", f[i].title, f[i].evidence);
        } else vfb("%s: could not be read: %s\n", VK_NAME[k], perr);
        plan_free(&p);
    } else {
        int s = k == VK_RUN_O ? 0 : 1; const ResultSet *r = &rs[G.vres < n ? G.vres : n - 1];
        unsigned long long sum = 0; for (int i = 0; i < r->nrows; i++) sum += row_hash(r, i);   /* order does not matter */
        G.vrows[s] = r->nrows; G.vsum[s] = sum; G.vms[s] = r->exec_ms + r->fetch_ms; G.vtrunc[s] = r->truncated && !r->complete; G.vran[s] = true;
        vfb("%s: %d row(s)%s, checksum %016llx, %.1f ms\n", VK_NAME[k], r->nrows, G.vtrunc[s] ? " (first rows only: the limit stopped it)" : "", sum, G.vms[s]);
    }
    for (int i = 0; i < n && i < 8; i++) rs_free(&rs[i]);
    exec_free(G.vjob); G.vjob = NULL; v_free_stmts();
    (void)a;
}
static void sql_send(App *a);
/* one frame of the checks: start the next step when the session is free, read it when it is done */
static void v_pump(App *a) {
    if (G.vstate != 1) return;
    Connection *c = conn_of(a, G.qconn);
    if (!c || !c->db) { vfb("checks stopped: the connection is closed\n"); G.vstep = VK_DONE; }
    if (G.vjob) { if (exec_running(G.vjob)) { app_wake_in(a, 80); return; } v_finish_step(a, c); G.vstep++; }
    while (G.vstep < VK_DONE) {
        if ((G.vstep == VK_RUN_O || G.vstep == VK_RUN_C) && !G.vapproved) {
            if (G.vwant_results) {   /* reading data waits for the user's approval */
                G.vstate = 2; G.ast = AS_PLAN; G.qkind = AA_VALIDATE;
                snprintf(G.amsg, sizeof G.amsg, "The free checks are done. To compare results and time, Lume runs both queries read-only (it reads data; nothing is sent but counts, a checksum and times).");
                return;
            }
            G.vstep = VK_DONE; break;
        }
        if (c->db->busy || G.job) { app_wake_in(a, 120); return; }
        char why[300] = "";
        G.vkind = G.vstep;
        if (!v_prepare(c, G.vstep, why, sizeof why)) { if (why[0]) vfb("%s: not run: %s\n", VK_NAME[G.vstep], why); G.vstep++; continue; }
        G.vjob = exec_start_list(c->db, (const char *const *)G.vst, G.vnst, G.vkind >= VK_RUN_O ? 200000 : 100000, true, G.vfin);
        if (!G.vjob) { v_free_stmts(); app_wake_in(a, 120); return; }
        snprintf(G.amsg, sizeof G.amsg, "Checking the hypothesis: %s...", VK_NAME[G.vkind]);
        app_wake_in(a, 80); return;
    }
    /* all done: the numbers to the chat and back to the model */
    if (G.vran[0] && G.vran[1]) vfb("same result: %s\n", G.vrows[0] == G.vrows[1] && G.vsum[0] == G.vsum[1] ? (G.vtrunc[0] || G.vtrunc[1] ? "yes for the rows read (the limit stopped the runs)" : "yes (row count and checksum equal)") : "NO (row count or checksum differ)");
    size_t w = 0;
    bool costs = G.vcost[0][0] && G.vcost[1][0] && strcmp(G.vcost[0], "n/a") && strcmp(G.vcost[1], "n/a") && strstr(G.vcost[0], "not") == NULL;
    if (costs) w += (size_t)snprintf(G.vline + w, sizeof G.vline - w, "Checked: plan cost %s \xE2\x86\x92 %s", G.vcost[0], G.vcost[1]);
    else w += (size_t)snprintf(G.vline + w, sizeof G.vline - w, "Checked: both plans captured (this database gives no costs)");
    if (G.vcost[2][0]) w += (size_t)snprintf(G.vline + w, sizeof G.vline - w, " (with the index %s)", G.vcost[2]);
    if (G.vran[0] && G.vran[1]) w += (size_t)snprintf(G.vline + w, sizeof G.vline - w, "; %s result (%lld rows); %.0f ms \xE2\x86\x92 %.0f ms", G.vrows[0] == G.vrows[1] && G.vsum[0] == G.vsum[1] ? "same" : "DIFFERENT", G.vrows[1], G.vms[0], G.vms[1]);
    chat_push('a', G.vline);
    audit(a, "assistant_sql", NULL, "validated", G.vfbw, "");
    G.vstate = 3; G.vround++;
    G.ast = AS_IDLE; G.amsg[0] = 0; G.resend = true;
    textfield_set(&G.q, "Conclua com base em <validation>: proponha a versão validada (edit_sql ou new_tab) ou explique por que não vale a pena.");
    sql_send(a);
}
static void v_start(App *a, const TabInfo *ti) {
    v_reset();
    Editor *ed = ti ? ti->ed : NULL;
    int len = 0; char *t = ed ? editor_text(ed, &len) : NULL;
    int nsemi = 0; for (int i = 0; t && i < len; i++) if (t[i] == ';') nsemi++;
    if (t && nsemi > 1) { int sl = 0, ss = 0; char *st = editor_statement_at_cursor_pos(ed, &sl, &ss); if (st) { free(t); t = st; } }
    G.vorig = t; G.vcand = strdup(G.ans.text); snprintf(G.vidx, sizeof G.vidx, "%s", G.ans.terms);
    G.vwant_results = !strcmp(G.ans.target, "results");
    memset(G.vcost, 0, sizeof G.vcost); memset(G.vran, 0, sizeof G.vran);
    G.vstep = VK_PLAN_O; G.vstate = 1;
    vfb("hypothesis: %s\n", G.ans.rationale[0] ? G.ans.rationale : "(not given)");
    if (G.ans.rationale[0]) chat_push('a', G.ans.rationale);
    add_step("validate", ST_DONE, G.vcand, "Lume checks the hypothesis: estimated plans%s%s", G.vidx[0] ? ", the hypothetical index" : "", G.vwant_results ? "; then, with your approval, both queries read-only (result and time)" : "");
    G.ast = AS_CHECKING; snprintf(G.amsg, sizeof G.amsg, "Checking the hypothesis...");
    app_wake_in(a, 16);
}
static void sql_reset_edit(void) { free(G.qedit_sql); G.qedit_sql = NULL; G.qkind = 0; G.qwrites = false; G.qwrite_why[0] = 0; if (G.qtable_set) { td_free(&G.qtable); G.qtable_set = false; } }
/* an identifier the model proposes: letters, digits, _ $ # (dots only between schema and table) */
static bool ident_ok(const char *s, bool dotted) { if (!s[0] || strlen(s) > 120) return false; for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && *p != '_' && *p != '$' && *p != '#' && !(dotted && *p == '.')) return false; return true; }
static bool type_ok(const char *s) { if (!s[0] || strlen(s) > 60) return false; for (const char *p = s; *p; p++) if (!isalnum((unsigned char)*p) && !strchr(" _(),", *p)) return false; return true; }
/* design_table -> a Table Designer draft; false with why on anything it cannot take as is */
static bool table_from_json(const char *js, const char *dschema, TdTable *t, char *why, int wn) {
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, js, strlen(js))) { snprintf(why, (size_t)wn, "the table is not valid JSON"); return false; }
    td_init(t); bool ok = false;
    char nm[128] = "", sc[128] = ""; jr_str(&j, 0, "name", nm, sizeof nm); jr_str(&j, 0, "schema", sc, sizeof sc);
    if (!ident_ok(nm, false) || (sc[0] && !ident_ok(sc, false))) { snprintf(why, (size_t)wn, "the table name is not a plain identifier"); goto out; }
    snprintf(t->name, sizeof t->name, "%s", nm); snprintf(t->schema, sizeof t->schema, "%s", sc[0] ? sc : dschema);
    jr_str(&j, 0, "comment", t->comment, sizeof t->comment);
    int cols = jr_get(&j, 0, "columns");
    for (int i = 0; i < 80; i++) {
        int c = jr_child(&j, cols, i); if (c < 0) break;
        char cn[128] = "", ct[128] = ""; jr_str(&j, c, "name", cn, sizeof cn); jr_str(&j, c, "type", ct, sizeof ct);
        if (!ident_ok(cn, false) || !type_ok(ct)) { snprintf(why, (size_t)wn, "column %d: name or type is not plain (%.40s %.40s)", i + 1, cn, ct); goto out; }
        TdCol *col = td_add_col(t, cn, ct);
        bool b = false; if (jr_bool(&j, c, "notNull", &b)) col->notnull = b;
        if (jr_bool(&j, c, "identity", &b) && b) col->identity = 2;
        jr_str(&j, c, "default", col->def, sizeof col->def); jr_str(&j, c, "comment", col->comment, sizeof col->comment);
        if (strchr(col->def, ';')) { snprintf(why, (size_t)wn, "column %s: a default with ';' is refused", cn); goto out; }
    }
    if (!t->ncols) { snprintf(why, (size_t)wn, "the table has no columns"); goto out; }
    /* keys by column name */
    #define COLID(nmx) ({ int id_ = -1; for (int q_ = 0; q_ < t->ncols; q_++) if (!strcasecmp(t->cols[q_].name, nmx)) id_ = t->cols[q_].id; id_; })
    int pk = jr_get(&j, 0, "primaryKey");
    if (pk >= 0) { TdKey *k = td_add_key(t, TK_PK, ""); for (int i = 0; i < TD_MAXKC; i++) { int e = jr_child(&j, pk, i); if (e < 0) break; char cn[128]; jr_node_str(&j, e, cn, sizeof cn); int id = COLID(cn); if (id < 0) { snprintf(why, (size_t)wn, "primary key: no column %.40s", cn); goto out; } k->cols[k->ncols++] = id; } if (!k->ncols) td_del_key(t, k->id); }
    int fks = jr_get(&j, 0, "foreignKeys");
    for (int f = 0; f < 16; f++) {
        int e = jr_child(&j, fks, f); if (e < 0) break;
        char rt[256] = ""; jr_str(&j, e, "refTable", rt, sizeof rt);
        if (!ident_ok(rt, true)) { snprintf(why, (size_t)wn, "foreign key %d: the referenced table is not plain", f + 1); goto out; }
        TdKey *k = td_add_key(t, TK_FK, "");
        const char *dot = strrchr(rt, '.'); if (dot) { snprintf(k->ref_schema, sizeof k->ref_schema, "%.*s", (int)(dot - rt), rt); snprintf(k->ref_table, sizeof k->ref_table, "%s", dot + 1); } else { snprintf(k->ref_schema, sizeof k->ref_schema, "%s", t->schema); snprintf(k->ref_table, sizeof k->ref_table, "%s", rt); }
        int fc = jr_get(&j, e, "columns"), rc = jr_get(&j, e, "refColumns");
        for (int i = 0; i < TD_MAXKC; i++) { int x = jr_child(&j, fc, i); if (x < 0) break; char cn[128]; jr_node_str(&j, x, cn, sizeof cn); int id = COLID(cn); if (id < 0) { snprintf(why, (size_t)wn, "foreign key %d: no column %.40s", f + 1, cn); goto out; } k->cols[k->ncols++] = id; }
        for (int i = 0; i < TD_MAXKC; i++) { int x = jr_child(&j, rc, i); if (x < 0) break; char cn[128]; jr_node_str(&j, x, cn, sizeof cn); if (!ident_ok(cn, false)) { snprintf(why, (size_t)wn, "foreign key %d: referenced column is not plain", f + 1); goto out; } snprintf(k->ref_cols[k->nref++], sizeof k->ref_cols[0], "%s", cn); }
        if (!k->ncols || k->ncols != k->nref) { snprintf(why, (size_t)wn, "foreign key %d: columns and referenced columns do not match", f + 1); goto out; }
    }
    int ix = jr_get(&j, 0, "indexes");
    for (int f = 0; f < 16; f++) {
        int e = jr_child(&j, ix, f); if (e < 0) break;
        TdKey *k = td_add_key(t, TK_INDEX, ""); bool u = false; if (jr_bool(&j, e, "unique", &u)) k->unique = u;
        int fc = jr_get(&j, e, "columns");
        for (int i = 0; i < TD_MAXKC; i++) { int x = jr_child(&j, fc, i); if (x < 0) break; char cn[128]; jr_node_str(&j, x, cn, sizeof cn); int id = COLID(cn); if (id < 0) { snprintf(why, (size_t)wn, "index %d: no column %.40s", f + 1, cn); goto out; } k->cols[k->ncols++] = id; }
        if (!k->ncols) td_del_key(t, k->id);
    }
    #undef COLID
    ok = true;
out:
    json_free(&j);
    if (!ok) td_free(t);
    return ok;
}
static void sql_send(App *a) {
    TabInfo ti;
    if (!active_query_tab(a, &ti)) { snprintf(G.amsg, sizeof G.amsg, "Open a query tab (or a report) first."); return; }
    if (!G.q.buf[0]) { snprintf(G.amsg, sizeof G.amsg, "Write a question or a change first."); return; }
    if (G.ai) return;
    if (G.chat_tab != ti.id + 1 || G.chat_doc) { chat_clear(); G.chat_doc = NULL; G.chat_tab = ti.id + 1; }
    if (!G.choice[0] && !G.resend) {
        chat_archive_live(); G.turn_start = G.nchat; chat_push('u', G.q.buf);
        if (!G.nqhist || strcmp(G.qhist[G.nqhist - 1], G.q.buf)) {
            if (G.nqhist == 50) { free(G.qhist[0]); memmove(&G.qhist[0], &G.qhist[1], sizeof G.qhist[0] * 49); G.nqhist--; }
            G.qhist[G.nqhist++] = strdup(G.q.buf);
        }
        G.qpos = -1; G.sx_rounds = 0; free(G.sx_extra); G.sx_extra = NULL;
        v_reset(); G.vround = 0;
    }
    G.resend = false; ai_reset_plan(); sql_reset_edit();
    G.qtab = ti.id; snprintf(G.qconn, sizeof G.qconn, "%s", ti.conn_id); snprintf(G.qtitle, sizeof G.qtitle, "%s", ti.title);
    Connection *c = conn_of(a, ti.conn_id);
    if (c) snprintf(G.schema_main, sizeof G.schema_main, "%s", c->id);   /* the structure follows the tab's connection */
    if (c) consent_apply(a, ai_exec_of(a, c));                            /* what was allowed for it before */
    if (G.cx.on[CX_SCHEMA] && c) {
        Connection *sc = ai_exec_of(a, c);
        if (sx_supported(sc) && !sx_ensure(a, sc, false)) {
            if (G.sx_state == SX_ERROR) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The structure of %s could not be read: %s. Nothing was sent.", sc->name, G.sx_err); return; }
            G.sx_wait = true; G.ast = AS_INDEXING;
            snprintf(G.amsg, sizeof G.amsg, "Reading the structure of %s in the background (once, then cached). Only the tables this request needs will be sent; no rows.", sc->name);
            app_wake_in(a, 100); return;
        }
    }
    snprintf(G.sent_q, sizeof G.sent_q, "%s", G.q.buf);
    build_sql_payload(a, &ti);
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    telemetry_count(TM_AI); G.ai = ai_start(stream_cfg(), ag_sql_system_prompt(), G.payload, dir);
    if (!G.ai) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The request could not start."); return; }
    G.req_qmode = true; G.ast = AS_THINKING; G.amsg[0] = 0;
    textfield_set(&G.q, "");
    audit(a, "assistant_sql", NULL, "sent", strlen(G.payload), c ? c->driver : "");
    app_wake_in(a, 150);
}
static bool sx_answer_search(App *a, RvDoc *d);
static void poll_sql(App *a) {
    const AiResult *res = ai_result(G.ai);
    bool search = false;
    if (!res->text) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s", res->err[0] ? res->err : "No answer"); audit(a, "assistant_sql", NULL, "provider-error", 0, ""); }
    else if (!ag_parse_sql_answer(res->text, &G.ans)) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s", G.ans.err); audit(a, "assistant_sql", NULL, "refused-invalid-answer", 0, ""); }
    else if (G.ans.type == AA_EDIT_SQL || G.ans.type == AA_NEW_TAB) {
        Connection *c = conn_of(a, G.qconn);
        G.qkind = G.ans.type; G.qedit_sql = strdup(G.ans.text); snprintf(G.qtarget, sizeof G.qtarget, "%s", G.ans.target);
        G.qwrites = sql_writes(G.qedit_sql, c ? dialect_for_driver(c->driver) : DIALECT_GENERIC, G.qwrite_why, sizeof G.qwrite_why);
        if (G.ans.rationale[0]) chat_push('a', G.ans.rationale);
        add_step(G.qkind == AA_EDIT_SQL ? "propose_sql_edit" : "propose_new_tab", ST_DONE, G.qedit_sql, "%s", G.qkind == AA_NEW_TAB ? (G.ans.summary[0] ? G.ans.summary : "a new query tab") : !strcmp(G.qtarget, "selection") ? "replaces the selection" : !strcmp(G.qtarget, "statement") ? "replaces the statement at the cursor" : "replaces the tab's text");
        if (G.qwrites) add_step("validate_query_policy", ST_DONE, NULL, "this SQL changes data or structure (%s): Lume never runs it; review it before you run it", G.qwrite_why);
        G.ast = AS_PLAN;
        snprintf(G.amsg, sizeof G.amsg, "%s", G.qkind == AA_NEW_TAB ? "Review the query, then open it in a new tab. Lume does not run it." : "Review the change, then apply it to the editor (Cmd+Z undoes it). Lume does not run it.");
        audit(a, "assistant_sql", NULL, G.qkind == AA_NEW_TAB ? "proposed-new-tab" : "proposed-edit", strlen(G.qedit_sql), "");
    }
    else if (G.ans.type == AA_DESIGN_TABLE) {
        Connection *c = conn_of(a, G.qconn); char why[240] = "";
        const char *dschema = c && !strcmp(c->driver, "postgres") ? "public" : "";
        if (!c || !tdes_supported(c)) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant designed a table, but there is no Table Designer for this connection."); }
        else if (!table_from_json(G.ans.text, dschema, &G.qtable, why, sizeof why)) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The table design was refused: %s. Nothing opened.", why); }
        else {
            G.qtable_set = true; G.qkind = AA_DESIGN_TABLE;
            int npk = 0, nfk = 0, nix = 0; for (int i = 0; i < G.qtable.nkeys; i++) { if (G.qtable.keys[i].kind == TK_PK) npk++; else if (G.qtable.keys[i].kind == TK_FK) nfk++; else if (G.qtable.keys[i].kind == TK_INDEX) nix++; }
            if (G.ans.rationale[0]) chat_push('a', G.ans.rationale);
            add_step("design_table", ST_DONE, NULL, "%s%s%s: %d column(s)%s, %d foreign key(s), %d index(es); opens in the Table Designer, nothing is created until you review and apply it there", G.qtable.schema, G.qtable.schema[0] ? "." : "", G.qtable.name, G.qtable.ncols, npk ? ", primary key" : "", nfk, nix);
            G.ast = AS_PLAN; snprintf(G.amsg, sizeof G.amsg, "Review the design, then open it in the Table Designer. Nothing is created until you apply it there.");
            audit(a, "assistant_sql", NULL, "proposed-table", strlen(G.ans.text), c->driver);
        }
    }
    else if (G.ans.type == AA_VALIDATE) {
        if (G.vround >= 2) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant asked for a third round of checks: stopped after two. The numbers so far are above."); }
        else { TabInfo ti; if (active_query_tab(a, &ti)) v_start(a, &ti); else { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "Open the query tab to check the hypothesis."); } }
    }
    else if (G.ans.type == AA_CREATE_REPORT) {   /* a dashboard / report asked from a query tab: a report is made for it */
        free(G.qcreate); G.qcreate = strdup(G.ans.text); snprintf(G.qcreate_title, sizeof G.qcreate_title, "%s", G.ans.summary[0] ? G.ans.summary : "Report from a question");
        chat_push('a', "This is a report: creating one and building it there (cards, charts, filters). You approve the plan before anything is added.");
        push_tokens();
        G.ast = AS_IDLE; G.amsg[0] = 0;
        audit(a, "assistant_sql", NULL, "create-report", strlen(G.qcreate), "");
    }
    else if (G.ans.type == AA_EXPLAIN) { chat_push('a', G.ans.text); push_tokens(); G.ast = AS_IDLE; G.amsg[0] = 0; audit(a, "assistant_sql", NULL, "explained", strlen(G.ans.text), ""); }
    else if (G.ans.type == AA_CLARIFY) { G.ast = AS_CLARIFY; G.nopt = 0; snprintf(G.amsg, sizeof G.amsg, "%s", G.ans.question); }
    else if (G.ans.type == AA_CANNOT) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant could not answer: %s", G.ans.reason); }
    else if (G.ans.type == AA_SEARCH_SCHEMA || G.ans.type == AA_DESCRIBE_TABLES) {
        if (!G.cx.on[CX_SCHEMA]) { Connection *c = conn_of(a, G.qconn); G.ast = AS_NEED_SCHEMA; snprintf(G.amsg, sizeof G.amsg, "To answer, the assistant needs the structure of %s: table and column names, types and keys. No rows or values.", c ? c->name : "the database"); }
        else search = true;
    }
    ai_free(G.ai); G.ai = NULL;
    if (G.qcreate) {   /* after this job is freed: the new report, then the request in the report flow */
        char *rq = G.qcreate; G.qcreate = NULL;
        RvDoc *nd = create_report(a, G.qcreate_title);
        if (nd) { G.adoc = nd; panels_activate_tab(a->panels, a, nd->tab_id); textfield_set(&G.q, rq); G.choice[0] = 0; G.req_qmode = false; ai_send(a, nd); }
        free(rq);
        return;
    }
    if (search && !sx_answer_search(a, NULL)) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant asked to search the structure, but there is no index of it for this connection."); }
}
static void sql_apply(App *a) {
    if (G.ast == AS_PLAN && G.qkind == AA_VALIDATE && G.vstate == 2) {   /* run both read-only */
        G.vapproved = true; G.vstate = 1; G.ast = AS_CHECKING; snprintf(G.amsg, sizeof G.amsg, "Running both queries read-only...");
        audit(a, "assistant_sql", NULL, "validation-runs-approved", 0, "");
        return;
    }
    if (G.ast == AS_PLAN && G.qkind == AA_DESIGN_TABLE && G.qtable_set) {
        Connection *c = conn_of(a, G.qconn);
        if (c && tdes_new_table_from(a, c, &G.qtable, "Designed by Lume Intelligence: review the columns and keys, then Review and Apply")) {
            G.ast = AS_DONE; snprintf(G.amsg, sizeof G.amsg, "Opened in the Table Designer as a new, unsaved table. Nothing was created.");
            audit(a, "assistant_sql", NULL, "table-opened-in-designer", 0, c->driver);
        } else { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The Table Designer could not open the design."); }
        sql_reset_edit(); return;
    }
    if (G.ast != AS_PLAN || !G.qedit_sql) return;
    int L = (int)strlen(G.qedit_sql);
    if (G.qkind == AA_NEW_TAB) {
        char tt[64]; snprintf(tt, sizeof tt, "%.40s.sql", G.ans.summary[0] ? G.ans.summary : "assistant");
        int nt = panels_open_tab(a->panels, a, tt, G.qconn, G.qedit_sql);
        if (nt > 0) { panels_activate_tab(a->panels, a, nt); G.chat_tab = nt + 1; G.qtab = nt; }   /* the conversation follows the new tab */
        G.ast = AS_DONE; snprintf(G.amsg, sizeof G.amsg, "Opened in a new tab. Lume did not run it.");
        audit(a, "assistant_sql", NULL, "new-tab-opened", (size_t)L, ""); sql_reset_edit(); return;
    }
    Editor *ed = panels_tab_editor(a->panels, G.qtab);
    if (!ed) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The tab was closed: nothing was applied."); return; }
    if (editor_revision(ed) != G.qrev) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The tab changed since the request: nothing was applied. Ask again so the change matches what is there now."); return; }
    if (!strcmp(G.qtarget, "selection") && G.qsel_s >= 0 && editor_select_range(ed, G.qsel_s, G.qsel_e)) editor_insert(ed, G.qedit_sql, L);
    else if (!strcmp(G.qtarget, "statement") && G.qstmt_s >= 0 && editor_select_range(ed, G.qstmt_s, G.qstmt_e)) editor_insert(ed, G.qedit_sql, L);
    else editor_replace_all(ed, G.qedit_sql, L);
    panels_activate_tab(a->panels, a, G.qtab);
    G.ast = AS_DONE; snprintf(G.amsg, sizeof G.amsg, "Applied to the editor (Cmd+Z undoes it). Lume did not run it.");
    audit(a, "assistant_sql", NULL, "applied-to-editor", (size_t)L, "");
    sql_reset_edit();
}
static void sql_reject(App *a) {
    if (G.ast != AS_PLAN) return;
    if (G.qkind == AA_VALIDATE && G.vstate == 2) { G.vwant_results = false; G.vstate = 1; G.vstep = VK_DONE; G.ast = AS_CHECKING; vfb("results and time: not checked (the user declined running the queries)\n"); return; }
    sql_reset_edit(); ai_reset_plan();
    G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Rejected: the editor is unchanged.");
    audit(a, "assistant_sql", NULL, "rejected-by-user", 0, "");
}
/* quick requests from the panel (Explain, Fix the error) */
/* the design review's size before it is sent: the placeholder page's bytes / 4 (refreshed when the report changes) */
static int design_review_estimate(App *a, RvDoc *d) {
    if (G.rev_est_doc == d && G.rev_est_rev == d->r.revision) return G.rev_est_tokens;
    HB hp = { 0 }; char hw[300]; G.rev_est_tokens = 0;
    if (html_build(a, d, true, &hp, hw, sizeof hw)) G.rev_est_tokens = (int)(hp.w / 4) + 3000;   /* + the usual context */
    free(hp.b); G.rev_est_doc = d; G.rev_est_rev = d->r.revision;
    return G.rev_est_tokens;
}
static void design_review(App *a, RvDoc *d) {
    textfield_set(&G.q, "Revise o design deste relatório: hierarquia, tamanhos e posições dos cards, legibilidade, escolha dos gráficos e consistência com o tema.");
    G.choice[0] = 0; G.design_review = true; G.quick = true;
    ai_send(a, d);
    G.quick = false;
}
void rptview_ask_sql(App *a, const char *q) {
    if (!active_query_tab(a, NULL)) { app_status(a, "Lume Intelligence: open the query tab of this plan first"); return; }
    panels_show(a->panels, a, "lume.data.assistant");
    textfield_set(&G.q, q); G.choice[0] = 0; G.quick = true; sql_send(a); G.quick = false;
}
static void sql_quick(App *a, const char *q) { textfield_set(&G.q, q); G.choice[0] = 0; G.quick = true; sql_send(a); G.quick = false; }
static void ai_send(App *a, RvDoc *d) {
    if (active_query_tab(a, NULL)) { sql_send(a); return; }   /* a query tab is active: the SQL assistant */
    G.req_qmode = false;
    if (!d) { snprintf(G.amsg, sizeof G.amsg, "Open or create a report first (Report > New Report)."); return; }
    if (!G.q.buf[0]) { snprintf(G.amsg, sizeof G.amsg, "Write a question or a change first."); return; }
    if (G.ai) return;
    if (G.chat_doc != d) { chat_clear(); G.chat_doc = d; }
    if (!G.choice[0] && !G.resend) {   /* a new turn (a clarified resend continues it) */
        chat_archive_live(); G.turn_start = G.nchat; chat_push('u', G.q.buf);
        G.sx_rounds = 0; free(G.sx_extra); G.sx_extra = NULL;
        if (!G.nqhist || strcmp(G.qhist[G.nqhist - 1], G.q.buf)) {
            if (G.nqhist == 50) { free(G.qhist[0]); memmove(&G.qhist[0], &G.qhist[1], sizeof G.qhist[0] * 49); G.nqhist--; }
            G.qhist[G.nqhist++] = strdup(G.q.buf);
        }
        G.qpos = -1;
    }
    G.resend = false; G.retries = 0; G.probed = false;
    if (!G.choice[0]) G.allow_global = false;   /* page-wide changes are authorised per request, when asked */
    if (G.design_review) G.allow_global = true;   /* a design review is about the whole page by nature */
    G.adoc = d; snprintf(G.acard, sizeof G.acard, "%s", d->sel);
    ai_reset_plan();
    /* business ambiguity is resolved before anything is sent (LI-021 / Gherkin "Termo ambíguo") */
    if (!G.choice[0]) {
        char opts[6][48], term[48]; int n = 0;
        if (sem_ambiguous(&G.cat, G.q.buf, opts, 6, &n, term, sizeof term)) {
            G.nopt = n; for (int i = 0; i < n; i++) snprintf(G.opts[i], sizeof G.opts[i], "%s", opts[i]);
            snprintf(G.term, sizeof G.term, "%s", term);
            G.ast = AS_LOCAL_CLARIFY;
            snprintf(G.amsg, sizeof G.amsg, "'%s' can mean more than one certified metric: which one? Nothing was sent or queried yet.", term);
            audit(a, "search_business_metrics", d, "clarify-before-sending", 0, "");
            return;
        }
    }
    { Connection *sc = schema_connection(a); if (sc) consent_apply(a, ai_exec_of(a, sc)); }   /* consents remembered for this connection */
    /* nothing in the catalog and no structure shared yet: ask first, before anything is sent */
    if (G.cat.nm == 0 && !G.cx.on[CX_SCHEMA]) {
        Connection *sc = schema_connection(a);
        G.ast = AS_NEED_SCHEMA;
        snprintf(G.amsg, sizeof G.amsg, "There are no certified metrics yet. To build this from your database, the assistant needs its structure%s%s: table and column names, types and keys. No rows or values are sent. Nothing was sent yet.", sc ? " of " : "", sc ? sc->name : "");
        audit(a, "propose_report_patch", d, "needs-schema-local", 0, "");
        return;
    }
    char why[400];
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    ai_config_load(dir, &G.cfg);
    if (!ai_ready(&G.cfg, why, sizeof why)) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s Reports, the catalog and manual editing keep working without it.", why); audit(a, "propose_report_patch", d, "not-sent-not-configured", 0, ""); return; }
    {   /* a large database: its structure is indexed once, in the background, before the first question goes */
        Connection *sc = G.cx.on[CX_SCHEMA] ? ai_exec_of(a, schema_connection(a)) : NULL;
        if (sc && sx_supported(sc) && !sx_ensure(a, sc, false)) {
            if (G.sx_state == SX_ERROR) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The structure of %s could not be read: %s. Nothing was sent.", sc->name, G.sx_err); return; }
            G.sx_wait = true; G.ast = AS_INDEXING;
            snprintf(G.amsg, sizeof G.amsg, "Reading the structure of %s in the background (%d tables; once, then it is cached). Only the tables this question needs will be sent; no rows.", sc->name, sc->ntables);
            app_wake_in(a, 100);
            return;
        }
    }
    build_payload(a, d);
    telemetry_count(TM_AI); G.ai = ai_start(stream_cfg(), ag_system_prompt(), G.payload, dir);
    G.ast = AS_THINKING; G.at0 = now_s(); snprintf(G.sent_q, sizeof G.sent_q, "%s", G.q.buf);
    textfield_set(&G.q, "");   /* chat: the question moved into the conversation */
    snprintf(G.amsg, sizeof G.amsg, "Thinking (%s)...", strcmp(G.cfg.provider, "local") ? "cloud provider" : "local endpoint, nothing leaves this machine");
    char ex[200]; snprintf(ex, sizeof ex, "provider=%s model=%s shared=catalog:%d,dims:%d,tables:%d,report:%d,card:%d,values:%d,schema:%d", G.cfg.provider, G.cfg.model, G.cx.on[CX_CATALOG], G.cx.on[CX_DIMENSIONS], G.cx.on[CX_TABLES], G.cx.on[CX_REPORT], G.cx.on[CX_CARD], G.cx.on[CX_VALUES], G.cx.on[CX_SCHEMA]);
    audit(a, "propose_report_patch", d, "sent", strlen(G.payload), ex);
    LOG_I("report: AI request sent to %s (%s), %zu bytes", G.cfg.provider, G.cfg.model, strlen(G.payload));
}
/* what an approved proposal will do: tools, queries, policy (LI-012) */
static void plan_from_answer(App *a, RvDoc *d) {
    Report t; rpt_copy(&t, &d->r);
    if (G.ans.period[0]) snprintf(t.period, sizeof t.period, "%s", G.ans.period);
    RptApply ap; char why[300];
    bool scoped = G.acard[0] && !G.allow_global;
    const char *sqlc = G.cx.on[CX_SCHEMA] && G.schema_conn[0] ? G.schema_conn : NULL;
    if (!rpt_apply_proposal_ex(&t, &G.cat, G.ans.proposal, strlen(G.ans.proposal), d->page, scoped ? G.acard : "", G.allow_global || !G.acard[0], sqlc, &ap, why, sizeof why)) {
        rpt_free(&t);
        if (scoped) {   /* only the selected-card scope stood in the way: ask the user, do not refuse */
            Report t2; rpt_copy(&t2, &d->r); RptApply ap2; char w2[300];
            if (G.ans.period[0]) snprintf(t2.period, sizeof t2.period, "%s", G.ans.period);
            bool fine = rpt_apply_proposal_ex(&t2, &G.cat, G.ans.proposal, strlen(G.ans.proposal), d->page, "", true, sqlc, &ap2, w2, sizeof w2);
            rpt_free(&t2);
            if (fine) {
                const RptVisual *sv = rpt_find(&d->r, G.acard, NULL);
                G.ast = AS_SCOPE;
                snprintf(G.amsg, sizeof G.amsg, "The card \"%s\" is selected, but the assistant's proposal also changes the page (%d card(s) added or changed). Allow it for this request?", sv && sv->title[0] ? sv->title : G.acard, ap2.added + ap2.updated + ap2.moved);
                audit(a, "propose_report_patch", d, "asks-page-scope", 0, "");
                return;
            }
        }
        G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The proposal was refused by Lume's validation: %s. Nothing changed.", why);
        audit(a, "propose_report_patch", d, "refused-invalid", 0, "");
        return;
    }
    /* the AI's SQL: read-only policy first, then a local run that reads no rows (WHERE 1=0), before any plan */
    if (sqlc && G.cfg.ro_required) {
        Connection *xc = conn_of(a, sqlc);
        if (!xc || strcmp(G.gr_conn, sqlc) || G.gr_state != AG_READONLY) {
            rpt_free(&t);
            G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The assistant's SQL must run on a read-only login (require_readonly_ai_sql=1): %s is not verified as one. Nothing ran, nothing changed.", xc ? xc->name : "the connection");
            audit(a, "propose_report_patch", d, "refused-not-readonly-login", 0, "");
            return;
        }
    }
    if (!G.probed) {
        for (int i = 0; i < G.nprobes; i++) free(G.probes[i].probe);
        G.nprobes = 0;
        for (int i = 0; i < ap.nchanged && G.nprobes < 12; i++) {
            int pg; RptVisual *v = rpt_find(&t, ap.changed[i], &pg);
            if (!v || !v->ai_sql || !v->sql) continue;
            int dl = dialect_of_conn(a, v->conn);
            if (!sqlguard_check(v->sql, dl, why, sizeof why)) {
                rpt_free(&t);
                G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The proposal was refused: the SQL of '%s' breaks the read-only policy (%s). Nothing ran, nothing changed.", v->title[0] ? v->title : v->id, why);
                audit(a, "propose_report_patch", d, "refused-ai-sql-policy", 0, "");
                return;
            }
            char bound[RPT_AI_SQL_MAX * 2];
            if (!rpt_bind_params(&t, v->sql, bound, sizeof bound, NULL, 0, why, sizeof why)) {
                rpt_free(&t);
                G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The proposal was refused: the SQL of '%s' uses %s. Nothing ran, nothing changed.", v->title[0] ? v->title : v->id, why);
                audit(a, "propose_report_patch", d, "refused-ai-sql-params", 0, "");
                return;
            }
            const char *q = bound; while (*q == ' ' || *q == '\n' || *q == '\t' || *q == '\r') q++;
            bool cte = !strncasecmp(q, "with", 4);
            size_t L = strlen(bound) + 80; char *p = malloc(L);
            if (cte && dl == DIALECT_MSSQL) snprintf(p, L, "%s", bound);   /* SQL Server takes no CTE in a derived table: runs as written (read-only) */
            else snprintf(p, L, "SELECT * FROM (\n%s\n) lume_probe WHERE 1=0", bound);
            snprintf(G.probes[G.nprobes].vid, sizeof G.probes[0].vid, "%s", v->id);
            snprintf(G.probes[G.nprobes].title, sizeof G.probes[0].title, "%s", v->title);
            G.probes[G.nprobes].kind = v->kind; snprintf(G.probes[G.nprobes].conn, sizeof G.probes[0].conn, "%s", v->conn);
            G.probes[G.nprobes].probe = p;
            RvData *x = find_data(d, v->conn, p); if (x && x->status != RD_OK) data_free(x);   /* a fresh check every time */
            request_data(d, v->conn, p);
            G.nprobes++;
        }
        /* new filters: their options query passes the same policy and local check */
        for (int i = 0; i < ap.nfilter_names && G.nprobes < 12; i++) {
            RptParam *pm = NULL; for (int k = 0; k < t.nparams; k++) if (!strcmp(t.params[k].name, ap.filter_names[i])) pm = &t.params[k];
            if (!pm || !pm->options_sql[0]) continue;
            int dl = dialect_of_conn(a, pm->conn);
            if (!sqlguard_check(pm->options_sql, dl, why, sizeof why)) {
                rpt_free(&t);
                G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The proposal was refused: the options query of the filter '%s' breaks the read-only policy (%s). Nothing ran, nothing changed.", pm->label, why);
                audit(a, "propose_report_patch", d, "refused-ai-sql-policy", 0, "");
                return;
            }
            size_t L = strlen(pm->options_sql) + 80; char *p = malloc(L);
            snprintf(p, L, "SELECT * FROM (\n%s\n) lume_probe WHERE 1=0", pm->options_sql);
            snprintf(G.probes[G.nprobes].vid, sizeof G.probes[0].vid, "%s", pm->name);
            snprintf(G.probes[G.nprobes].title, sizeof G.probes[0].title, "filter %s", pm->label);
            G.probes[G.nprobes].kind = RK_TABLE; snprintf(G.probes[G.nprobes].conn, sizeof G.probes[0].conn, "%s", pm->conn);
            G.probes[G.nprobes].probe = p;
            RvData *x = find_data(d, pm->conn, p); if (x && x->status != RD_OK) data_free(x);
            request_data(d, pm->conn, p);
            G.nprobes++;
        }
        if (G.nprobes) {
            rpt_free(&t);
            G.ast = AS_CHECKING;
            snprintf(G.amsg, sizeof G.amsg, "Checking the assistant's SQL on %s: read-only, no rows are read (%d quer%s)...", conn_of(a, G.probes[0].conn) ? conn_of(a, G.probes[0].conn)->name : "the database", G.nprobes, G.nprobes == 1 ? "y" : "ies");
            return;
        }
    }
    G.pending_logo = false;
    if (ap.theme_json) {   /* set_theme: the brand's lock, then readability, before any plan */
        G.pending_logo = take_logo_attachment(ap.theme_json);
        if (G.pending_logo && !G.attach[0]) {
            free(ap.theme_json); rpt_free(&t); G.pending_logo = false;
            G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The design was refused: it uses an attached image, but none is attached. Drop the logo on the chat (or Attach image...) and ask again. Nothing changed.");
            audit(a, "propose_report_patch", d, "refused-design", 0, "");
            return;
        }
        theme_resolve(d);
        RTheme mod = d->rt; char tw[300] = "";
        if (d->rt.locked && ap.actions > 1) {   /* the brand's lock: the restyle is left out, the rest of the proposal stays */
            free(ap.theme_json); ap.theme_json = NULL; G.pending_logo = false;
            free(G.pending_theme); G.pending_theme = NULL;
            add_step("set_theme", ST_SKIPPED, NULL, "left out: the theme is locked by the brand (the cards' own changes stay in the plan)");
            goto theme_done;
        }
        bool okt = !d->rt.locked && rtheme_apply_overrides(&mod, ap.theme_json, strlen(ap.theme_json), tw, sizeof tw) && rtheme_check_contrast(&mod, tw, sizeof tw);
        if (!okt) {
            if (d->rt.locked) snprintf(tw, sizeof tw, "the theme is locked by the brand");
            free(ap.theme_json); rpt_free(&t); G.pending_logo = false;
            G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The design was refused: %s. Nothing changed.", tw);
            audit(a, "propose_report_patch", d, "refused-design", 0, "");
            return;
        }
        free(G.pending_theme); G.pending_theme = ap.theme_json; ap.theme_json = NULL;
        add_step("set_theme", ST_DONE, NULL, "accent #%06X, header %s (#%06X), cards %s, KPI %s, corners %.0f; text contrast checked (4.5:1)", mod.accent, mod.header ? "on" : "off", mod.header_bg, rtheme_card_style_name(mod.card_style), rtheme_kpi_style_name(mod.kpi_style), mod.radius);
        if (G.pending_logo) {
            char tp[1400];
            if (!strcmp(d->r.theme_ref, "workspace")) add_step("set_logo", ST_DONE, NULL, "%s (attached) becomes the logo of the workspace theme (Brand Kit): every report using it shows it", G.attach_name);
            else if (theme_file_path(d, d->r.theme_ref, tp, sizeof tp)) add_step("set_logo", ST_DONE, NULL, "%s (attached) is copied next to the theme file and becomes its logo", G.attach_name);
            else add_step("set_logo", ST_DONE, NULL, "%s (attached) becomes the logo; this report's theme is saved as its own file next to the report to hold it", G.attach_name);
        }
    } else { free(G.pending_theme); G.pending_theme = NULL; }
theme_done:
    for (int i = 0; i < ap.nfilter_names; i++) { RptParam *pm = NULL; for (int k = 0; k < t.nparams; k++) if (!strcmp(t.params[k].name, ap.filter_names[i])) pm = &t.params[k]; if (pm) add_step("add_filter", ST_DONE, pm->options_sql, "%s (:%s), options from a read-only query", pm->label, pm->name); }
    add_step("propose_report_patch", ST_DONE, NULL, "%s: %d added, %d changed, %d moved, %d narrative(s)%s", ap.summary[0] ? ap.summary : G.ans.summary, ap.added, ap.updated, ap.moved, ap.narratives, ap.metric_changed ? " - CHANGES A METRIC" : "");
    if (G.ans.period[0]) add_step("create_report_draft", ST_DONE, NULL, "period: %s", G.ans.period);
    for (int i = 0; i < ap.nchanged; i++) {
        int pg; RptVisual *v = rpt_find(&t, ap.changed[i], &pg);
        if (!v || !has_data(v)) continue;
        snprintf(G.applied[G.napplied++], sizeof G.applied[0], "%s", v->id);
        char sql[4096], conn[16];
        RvDoc tmpd = *d; tmpd.r = t;
        if (!visual_sql(a, &tmpd, v, false, sql, sizeof sql, conn, why, sizeof why)) { add_step("propose_sql_query", ST_REFUSED, NULL, "%s: %s", v->id, why); continue; }
        const SemMetricDef *md = metric_def(v);
        if (v->ai_sql) add_step("propose_sql_query", ST_DONE, sql, "%s %s: %s (SQL written by the AI from the shared schema: NOT certified; checked locally, no rows read)", rpt_kind_name(v->kind), v->id, v->title[0] ? v->title : "SQL");
        else add_step("propose_sql_query", ST_DONE, sql, "%s %s: %s%s%s%s%s (compiled by Lume from the catalog)", rpt_kind_name(v->kind), v->id, md ? md->name : v->metric, v->dim[0] ? " by " : "", v->dim, v->grain >= 0 ? " per " : "", v->grain >= 0 ? sem_grain_name(v->grain) : "");
        bool ok = sqlguard_check(sql, dialect_of_conn(a, conn), why, sizeof why);
        int lim_s = G.timeout_s > 0 ? G.timeout_s : a->report_timeout_s > 0 ? a->report_timeout_s : QUERY_TIMEOUT_S;
        char pol[160]; snprintf(pol, sizeof pol, "read-only, one statement, %s, timeout %d s, max 10000 rows", v->ai_sql ? "read-only transaction" : "catalog columns only", lim_s);
        add_step("validate_query_policy", ok ? ST_OK : ST_REFUSED, NULL, "%s: %s", v->id, ok ? pol : why);
        if (!ok) continue;
        RvData *have = find_data(d, conn, sql);
        add_step("execute_readonly_query", have && have->status == RD_OK ? ST_CACHED : ST_PENDING, NULL, "%s: %s", v->id, have && have->status == RD_OK ? "already loaded: no new query" : "runs after your approval");
        if (v->compare) add_step("execute_readonly_query", ST_PENDING, NULL, "%s: comparison window (%s)", v->id, rpt_compare_name(v->compare));
    }
    rpt_free(&t);
    G.ast = AS_PLAN;
    snprintf(G.amsg, sizeof G.amsg, "Proposal ready: review the plan, then approve or refuse. Nothing has changed yet.");
}
static void poll_sql(App *a);
static void poll_ai(App *a) {
    if (!G.ai || !ai_done(G.ai)) return;
    { const AiResult *ur = ai_result(G.ai); if (ur->text || ur->in_tokens) { G.tok_in += ur->in_tokens; G.tok_out += ur->out_tokens; G.nreq++; G.last_tin = ur->in_tokens; G.last_tout = ur->out_tokens; G.tok_pending = true; } }   /* this conversation's use */
    if (G.req_qmode) { poll_sql(a); return; }
    const AiResult *res = ai_result(G.ai);
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (!res->text) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s", res->err[0] ? res->err : "No answer"); audit(a, "propose_report_patch", d, "provider-error", 0, ""); }
    else if (!d) { G.ast = AS_IDLE; }
    else if (!ag_parse_answer(res->text, &G.cat, &G.ans)) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s", G.ans.err); audit(a, "propose_report_patch", d, "refused-invalid-answer", 0, ""); LOG_W("report: AI answer refused: %s", G.ans.err); }
    else if (G.ans.type == AA_CLARIFY) { G.ast = AS_CLARIFY; G.nopt = G.ans.nopt; for (int i = 0; i < G.nopt; i++) snprintf(G.opts[i], sizeof G.opts[i], "%s", G.ans.options[i]); snprintf(G.amsg, sizeof G.amsg, "%s", G.ans.question); }
    else if (G.ans.type == AA_CANNOT) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant could not answer: %s", G.ans.reason); }
    else if (G.ans.type == AA_NEED_SCHEMA) {
        if (G.cx.on[CX_SCHEMA]) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant could not answer from the shared structure: %s", G.ans.reason); }
        else { Connection *sc = schema_connection(a); G.ast = AS_NEED_SCHEMA; snprintf(G.amsg, sizeof G.amsg, "%s To answer, the assistant needs the structure%s%s: table and column names, types and keys. No rows or values are sent.", G.ans.reason, sc ? " of " : "", sc ? sc->name : ""); }
    }
    else if (G.ans.type == AA_SEARCH_SCHEMA || G.ans.type == AA_DESCRIBE_TABLES) G.sx_search = true;   /* answered after this job is freed */
    else { free(G.last_answer); G.last_answer = strdup(res->text); plan_from_answer(a, d); }
    ai_free(G.ai); G.ai = NULL;
    if (G.sx_search) { G.sx_search = false; if (!d || !sx_answer_search(a, d)) { G.ast = AS_CANNOT; snprintf(G.amsg, sizeof G.amsg, "The assistant asked to search the structure, but there is no index of it for this connection."); } }
}
/* the user agreed to share the structure: read it (connecting first when needed), then send again */
static void schema_allow(App *a) {
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : any_doc(a);
    Connection *m = schema_connection(a);
    if (!m) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "There is no connection to read the structure from: add one in the Explorer."); return; }
    snprintf(G.schema_main, sizeof G.schema_main, "%s", m->id);
    Connection *c = ai_exec_of(a, m);   /* the read-only login when one is mapped (BL-051) */
    if (c != m && !c->read_only) { c->read_only = true; conns_save(a->conns); LOG_I("report: %s is the AI query connection of %s: marked read-only in Lume", c->name, m->name); }
    snprintf(G.schema_conn, sizeof G.schema_conn, "%s", c->id);
    if (!c->db) { G.schema_wait = true; if (c->state != CONN_CONNECTING) conn_connect_async(c, false); snprintf(G.amsg, sizeof G.amsg, "Connecting to %s to read its structure...", c->name); return; }
    if (c->read_only && !c->ro_level) c->ro_level = caps_apply_read_only(c);
    grants_check(c);
    if (G.cfg.ro_required && G.gr_state != AG_READONLY) {
        G.schema_wait = false; G.ast = AS_ERROR;
        snprintf(G.amsg, sizeof G.amsg, "This setup requires a read-only login for the assistant's SQL (require_readonly_ai_sql=1), and %s is not one (%s). Map %s to a SELECT-only connection under \"Run the assistant's queries as\". Nothing was shared.", c->name, G.gr_note, m->name);
        audit(a, "context", d, "refused-not-readonly-login", 0, c->driver);
        return;
    }
    char why[300];
    if (!sx_supported(c) && !schema_build(c, why, sizeof why)) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "%s", why); return; }
    G.schema_wait = false;
    G.cx.on[CX_SCHEMA] = true;
    consent_save(a, c);   /* remembered for this connection */
    if (sx_supported(c)) sx_ensure(a, c, false);   /* large databases: read once in the background; each send picks its tables */
    size_t shared = G.schema_text && !sx_supported(c) ? strlen(G.schema_text) : 0;
    audit(a, "context", d, G.cx.on[CX_CATVALUES] ? "schema-shared-with-category-values" : "schema-shared", shared, c->driver);
    LOG_I("report: database structure of %s shared with the assistant (%s, no rows)", c->name, sx_supported(c) ? "indexed locally; only the tables each question needs are sent" : "whole structure");
    if (!G.q.buf[0]) textfield_set(&G.q, G.sent_q);
    { char msg[300]; snprintf(msg, sizeof msg, "\xE2\x9C\x93 Structure of %s shared%s%s: %d table(s) and view(s), names, types and keys only; no rows.", m->name, c != m ? ", as seen by the read-only login " : "", c != m ? c->name : "", c->ntables); chat_push('a', msg); }
    G.ast = AS_IDLE; G.amsg[0] = 0; G.resend = true; G.choice[0] = 0;
    ai_send(a, d);
}
/* the local checks of the AI's SQL finished: errors / shapes go back to the model (never values), or the plan is built */
static void track_checking(App *a) {
    if (G.schema_wait) {
        Connection *c = conn_of(a, G.schema_conn);
        if (c && c->db) schema_allow(a);
        else if (!c || c->state == CONN_FAILED) { G.schema_wait = false; G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "Could not connect to read the structure: %s", c && c->error[0] ? c->error : "connection failed"); }
        else app_wake_in(a, 150);
    }
    if (G.ast != AS_CHECKING) return;
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (!d) { G.ast = AS_IDLE; return; }
    size_t cap = 2048, w = 0; char *rep = malloc(cap); rep[0] = 0;
    int bad = 0;
    for (int i = 0; i < G.nprobes; i++) {
        RvData *x = find_data(d, G.probes[i].conn, G.probes[i].probe);
        if (x && (x->status == RD_QUEUED || x->status == RD_RUNNING)) { free(rep); app_wake_in(a, 100); return; }
        char line[900];
        if (!x || x->status != RD_OK || !x->rs.ok) {
            bad++;
            const char *e = x ? (x->err[0] ? x->err : x->rs.error) : "not run";
            snprintf(line, sizeof line, "- visual \"%s\" (%s): ERROR: %.600s\n", G.probes[i].title, rpt_kind_name(G.probes[i].kind), e && e[0] ? e : "failed");
        } else {
            int nc = x->rs.ncols;
            bool shape = G.probes[i].kind == RK_KPI ? nc >= 1 : G.probes[i].kind == RK_TABLE ? nc >= 1 : nc == 2;
            int k = snprintf(line, sizeof line, "- visual \"%s\" (%s): %s, columns:", G.probes[i].title, rpt_kind_name(G.probes[i].kind), shape ? "ok" : G.probes[i].kind == RK_LINE || G.probes[i].kind == RK_BAR ? "ERROR: a line or bar needs exactly two columns (label or period, value)" : "ERROR: wrong result shape for this kind");
            for (int c = 0; c < nc && k < (int)sizeof line - 80; c++) k += snprintf(line + k, sizeof line - (size_t)k, " %s%s%s", x->rs.cols[c] ? x->rs.cols[c] : "?", x->rs.types && x->rs.types[c] && x->rs.types[c][0] ? ":" : "", x->rs.types && x->rs.types[c] ? x->rs.types[c] : "");
            snprintf(line + k, sizeof line - (size_t)k, "\n");
            if (!shape) bad++;
        }
        if (w + strlen(line) + 1 > cap) { cap = (w + strlen(line) + 1) * 2; rep = realloc(rep, cap); }
        memcpy(rep + w, line, strlen(line) + 1); w += strlen(line);
    }
    if (bad && G.retries < 2 && G.last_answer) {
        /* one more turn with the model: its own answer and what failed (error texts and column names only) */
        build_payload(a, d);
        size_t L = strlen(G.payload) + strlen(G.last_answer) + w + 400; char *m = malloc(L);
        snprintf(m, L, "%s\n<previous_answer untrusted=\"true\">\n%s\n</previous_answer>\n<sql_check untrusted=\"true\">\n%s</sql_check>\nFix the visuals whose SQL failed and answer again with the complete proposal JSON.\n", G.payload, G.last_answer, rep);
        char dir[1100]; pref_dir(a, dir, sizeof dir);
        telemetry_count(TM_AI); G.ai = ai_start(stream_cfg(), ag_system_prompt(), m, dir);
        audit(a, "propose_report_patch", d, "sql-check-retry", strlen(m), "");
        LOG_I("report: the AI's SQL failed the local check (%d of %d); asking for a correction (%d)", bad, G.nprobes, G.retries + 1);
        free(m);
        G.retries++;
        G.ast = AS_THINKING; G.at0 = now_s();
        snprintf(G.amsg, sizeof G.amsg, "The SQL of %d card(s) failed the local check: asking the assistant to correct it (%d of 2)...", bad, G.retries);
        free(rep);
        return;
    }
    if (bad) {
        G.ast = AS_ERROR;
        snprintf(G.amsg, sizeof G.amsg, "The assistant's SQL still fails after %d correction(s), so nothing was changed:\n%.400s", G.retries, rep);
        audit(a, "propose_report_patch", d, "refused-ai-sql-check", 0, "");
        free(rep);
        return;
    }
    free(rep);
    G.probed = true;
    plan_from_answer(a, d);
    if (G.ast == AS_PLAN) {
        add_step("check_ai_sql", ST_OK, NULL, "%d AI-written quer%s ran locally with no rows read: tables, columns and result shape are valid", G.nprobes, G.nprobes == 1 ? "y" : "ies");
        Connection *xc = conn_of(a, G.schema_conn);
        if (xc) add_step("ai_connection", G.gr_state == AG_READONLY ? ST_OK : ST_DONE, NULL, "runs as %s: %s", xc->name, !strcmp(G.gr_conn, xc->id) ? G.gr_note : "grants not checked");
    }
}
static void ai_approve(App *a) {
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (!d || G.ast != AS_PLAN || !G.ans.proposal) return;
    RptApply ap; char why[300];
    before_change(d);
    bool scoped = G.acard[0] && !G.allow_global;
    if (!rpt_apply_proposal_ex(&d->r, &G.cat, G.ans.proposal, strlen(G.ans.proposal), d->page, scoped ? G.acard : "", G.allow_global || !G.acard[0], G.cx.on[CX_SCHEMA] && G.schema_conn[0] ? G.schema_conn : NULL, &ap, why, sizeof why)) {
        free(d->h.u[--d->h.nu]);
        G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "Not applied: %s", why);
        audit(a, "propose_report_patch", d, "conflict", 0, "");
        return;
    }
    if (G.ans.period[0]) snprintf(d->r.period, sizeof d->r.period, "%s", G.ans.period);
    if (ap.theme_json) {   /* the design: kept as the report's minimal theme overrides (one undo step with the rest) */
        RTheme base, mod; char tw[300];
        theme_base(d, &base); theme_resolve(d); mod = d->rt;
        bool want_logo = take_logo_attachment(ap.theme_json) && G.pending_logo && G.attach[0];
        if (!d->rt.locked && rtheme_apply_overrides(&mod, ap.theme_json, strlen(ap.theme_json), tw, sizeof tw)) { free(d->r.theme_overrides); d->r.theme_overrides = rtheme_diff_json(&base, &mod); theme_dirty(d); }
        free(ap.theme_json);
        if (want_logo) {   /* the attached image: copied next to the theme file (a report on a built-in theme gets its own file) */
            char tp[1400], lw[300] = "";
            theme_resolve(d);
            if (!theme_file_path(d, d->r.theme_ref, tp, sizeof tp)) {
                doc_paths(a, d);
                char dd[1024], base2[256]; doc_dir(d, dd, sizeof dd);
                const char *b = strrchr(d->path, '/'); snprintf(base2, sizeof base2, "%s", b ? b + 1 : d->path);
                char *dot = strstr(base2, ".lrpt"); if (dot) *dot = 0;
                snprintf(tp, sizeof tp, "%s/%s.lrtheme", dd, base2);
                if (theme_write_file(d, tp, lw, sizeof lw)) { char rel[1100], ref[1200]; rel_to_doc(d, tp, rel, sizeof rel); snprintf(ref, sizeof ref, "file:%s", rel); free(d->r.theme_overrides); d->r.theme_overrides = NULL; snprintf(d->r.theme_ref, sizeof d->r.theme_ref, "%s", ref); theme_dirty(d); }
            }
            if (!lw[0] && theme_set_logo(d, G.attach, lw, sizeof lw)) { G.attach[0] = 0; G.attach_name[0] = 0; }
            else if (lw[0]) LOG_W("report: the attached logo was not set: %s", lw);
        }
    }
    mark_changed(d);
    for (int i = 0; i < G.nsteps; i++) if (G.steps[i].st == ST_PENDING) G.steps[i].st = ST_RUNNING;
    if (G.choice[0] && d->r.nchoices < 4) snprintf(d->r.choices[d->r.nchoices++], sizeof d->r.choices[0], "%s", G.choice);
    if (ap.nchanged) snprintf(d->sel, sizeof d->sel, "%s", ap.changed[0]);
    G.ast = AS_QUERYING; snprintf(G.amsg, sizeof G.amsg, "Applied as a draft (Undo restores the previous state). Querying the data...");
    audit(a, "propose_report_patch", d, "approved", strlen(G.ans.proposal), "");
    LOG_I("report: AI proposal approved (%d action(s))", ap.actions);
}
static void ai_refuse(App *a) {
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (G.ast != AS_PLAN) return;
    ai_reset_plan();
    G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Proposal refused: nothing changed and no query ran.");
    audit(a, "propose_report_patch", d, "refused-by-user", 0, "");
}
static void ai_cancel_all(App *a) {
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (G.ai) { ai_cancel(G.ai); ai_free(G.ai); G.ai = NULL; }
    if (G.ast == AS_QUERYING && d) {
        /* stop the active query and the next ones of this request (LI-014.2) */
        if (G.job && G.job_doc == d) exec_cancel(G.job);
        for (int i = 0; i < MAX_DATA; i++) if (d->data[i].used && d->data[i].status == RD_QUEUED) d->data[i].status = RD_CANCELLED;
        for (int i = 0; i < G.nsteps; i++) if (G.steps[i].st == ST_RUNNING || G.steps[i].st == ST_PENDING) G.steps[i].st = ST_SKIPPED;
    }
    G.ast = AS_CANCELLED; snprintf(G.amsg, sizeof G.amsg, "Cancelled: no further tool runs; the active query was asked to stop.");
    audit(a, "propose_report_patch", d, "cancelled", 0, "");
}
static void ai_choose(App *a, int i) {
    if (i < 0 || i >= G.nopt) return;
    const SemMetricDef *md = sem_cur(sem_metric_c(&G.cat, G.opts[i]));
    snprintf(G.choice, sizeof G.choice, "'%s' = %s (%s v%d)", G.term[0] ? G.term : "metric", md ? md->name : G.opts[i], G.opts[i], md ? md->version : 0);
    chat_archive_live(); chat_push('u', md ? md->name : G.opts[i]);
    G.ast = AS_IDLE;
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : any_doc(a);
    audit(a, "search_business_metrics", d, "clarified", 0, "");
    ai_send(a, d);
}
static void track_querying(App *a) {
    if (G.ast != AS_QUERYING) return;
    RvDoc *d = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
    if (!d) { G.ast = AS_IDLE; return; }
    int pending = 0, failed = 0;
    for (int i = 0; i < G.napplied; i++) {
        int pg; RptVisual *v = rpt_find(&d->r, G.applied[i], &pg); if (!v) continue;
        char why[300]; RvData *x = visual_data(a, d, v, false, why, sizeof why);
        if (!x) { failed++; continue; }
        if (x->status == RD_QUEUED || x->status == RD_RUNNING) pending++;
        else if (x->status != RD_OK) failed++;
        if (v->compare) { RvData *b = visual_data(a, d, v, true, why, sizeof why); if (b && (b->status == RD_QUEUED || b->status == RD_RUNNING)) pending++; }
    }
    if (pending) return;
    for (int i = 0; i < G.nsteps; i++) if (G.steps[i].st == ST_RUNNING) G.steps[i].st = ST_DONE;
    G.ast = AS_DONE;
    snprintf(G.amsg, sizeof G.amsg, failed ? "Done with %d card(s) in error: see the cards. The rest is valid." : "Done: the page is a draft; review, adjust or undo.", failed);
    audit(a, "render_chart_preview", d, failed ? "done-with-errors" : "done", 0, "");
}
static void set_ai_enabled(App *a, bool on) {
    char dir[1100], p[1200]; pref_dir(a, dir, sizeof dir); snprintf(p, sizeof p, "%s/ai.txt", dir);
    /* keep the other settings, change only the provider line (never a key: keys stay in the environment) */
    char buf[4096] = ""; size_t w = 0; FILE *f = fopen(p, "r");
    if (f) { char line[512]; while (fgets(line, sizeof line, f)) { if (!strncmp(line, "provider=", 9)) continue; size_t l = strlen(line); if (w + l < sizeof buf - 1) { memcpy(buf + w, line, l); w += l; buf[w] = 0; } } fclose(f); }
    f = fopen(p, "w"); if (!f) return;
    fprintf(f, "provider=%s\n%s", on ? (G.cfg.provider[0] && strcmp(G.cfg.provider, "none") ? G.cfg.provider : "anthropic") : "none", buf);
    fclose(f);
    if (!on) { ai_cancel_all(a); G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Lume Intelligence is off: no AI process or request remains. Reports keep working."); }
    ai_config_load(dir, &G.cfg);
    LOG_I("report: Lume Intelligence %s", on ? "enabled" : "disabled");
}

/* ---- AI setup (LI-001 / 003 / 004) ---- */
enum { TS_NONE, TS_RUNNING, TS_OK, TS_FAIL };
enum { SP_ANTHROPIC, SP_OPENAI, SP_LOCAL };
static const char *setup_provider(void) { return G.sprov == SP_LOCAL ? "local" : G.sprov == SP_OPENAI ? (G.oa_mode == 0 ? "chatgpt" : "openai") : "anthropic"; }
static void setup_config(AiConfig *c) {
    ai_config_default(c);
    ai_provider_defaults(c, setup_provider());
    if (G.sprov == SP_OPENAI) c->model[0] = 0;
    if (G.s_model.buf[0]) snprintf(c->model, sizeof c->model, "%s", G.s_model.buf);
    if (G.sprov == SP_LOCAL) snprintf(c->endpoint, sizeof c->endpoint, "%s", G.s_endpoint.buf);
    else if (G.sprov == SP_ANTHROPIC && strcmp(G.cfg.provider, "openai") && strcmp(G.cfg.provider, "chatgpt") && strcmp(G.cfg.provider, "local") && G.cfg.endpoint[0]) snprintf(c->endpoint, sizeof c->endpoint, "%s", G.cfg.endpoint);   /* an endpoint in ai.txt (proxy / tests) */
    snprintf(c->effort, sizeof c->effort, "low"); c->max_tokens = 64; c->timeout_s = 30;
    if (!strcmp(c->provider, "chatgpt")) c->timeout_s = 60;
    if ((G.sprov == SP_ANTHROPIC || (G.sprov == SP_OPENAI && G.oa_mode == 1)) && G.s_key.buf[0]) c->key = G.s_key.buf;   /* tested before it is saved */
}
static const char *key_account(void) { return G.sprov == SP_OPENAI ? AI_OPENAI_KEY_ACCOUNT : AI_KEY_ACCOUNT; }
static int key_source(void) { return ai_key_source_for(G.sprov == SP_OPENAI ? "openai" : "anthropic"); }
/* the pasted key leaves memory: buffer and its undo / redo copies */
static void wipe_key_field(void) { volatile unsigned char *p = (volatile unsigned char *)&G.s_key; for (size_t i = 0; i < sizeof G.s_key; i++) p[i] = 0; G.s_key.password = true; }
static void models_join(void) { if (G.mstate) { pthread_join(G.mth, NULL); G.mstate = 0; } }
static void set_provider(int sp, int mode) {
    if (sp == G.sprov && mode == G.oa_mode) return;
    bool was_openai = G.sprov == SP_OPENAI;
    G.sprov = sp; G.oa_mode = mode; G.test_state = TS_NONE; G.test_msg[0] = 0; G.replacing_key = false; wipe_key_field();
    if (sp == SP_ANTHROPIC && (was_openai || !G.s_model.buf[0])) textfield_set(&G.s_model, "claude-opus-5-5");
    if (sp == SP_OPENAI && !was_openai) textfield_set(&G.s_model, "");
    if (sp == SP_OPENAI) { G.nmodels = 0; G.models_open = false; G.mmsg[0] = 0; }
}
static void setup_open(App *a) {
    char dir[1100]; pref_dir(a, dir, sizeof dir); ai_config_load(dir, &G.cfg);
    G.setup = true; G.test_state = TS_NONE; G.test_msg[0] = 0; G.replacing_key = false; wipe_key_field(); ai_key_changed();
    G.sprov = !strcmp(G.cfg.provider, "local") ? SP_LOCAL : !strcmp(G.cfg.provider, "openai") || !strcmp(G.cfg.provider, "chatgpt") ? SP_OPENAI : SP_ANTHROPIC;
    G.oa_mode = !strcmp(G.cfg.provider, "openai") ? 1 : 0;
    G.nmodels = 0; G.models_open = false; G.mmsg[0] = 0;
    textfield_set(&G.s_model, G.cfg.model); textfield_set(&G.s_fast, G.cfg.model_fast);
    textfield_set(&G.s_endpoint, G.sprov == SP_LOCAL ? G.cfg.endpoint : "http://127.0.0.1:11434/v1/messages");
}
static void *models_th(void *arg) {
    (void)arg;
    G.mcfg.key = G.mkey;
    char err[300] = "";
    char (*out)[96] = calloc(40, 96);
    int n = ai_list_models(&G.mcfg, out, 40, err, sizeof err);
    memcpy(G.models, out, sizeof G.models); free(out);
    G.nmodels = n;
    if (n) snprintf(G.mmsg, sizeof G.mmsg, "%d models available to this account", n); else snprintf(G.mmsg, sizeof G.mmsg, "%s", err);
    if (G.mkey) { volatile char *q = G.mkey; while (*q) *q++ = 0; free(G.mkey); G.mkey = NULL; }
    __atomic_store_n(&G.mstate, 2, __ATOMIC_SEQ_CST);
    return NULL;
}
static void models_load(App *a) {
    (void)a;
    if (G.mstate == 1) return;
    models_join();
    setup_config(&G.mcfg);
    G.mkey = G.mcfg.key && *G.mcfg.key ? strdup(G.mcfg.key) : NULL; G.mcfg.key = NULL;
    G.nmodels = 0; snprintf(G.mmsg, sizeof G.mmsg, "Loading models...");
    G.mstate = 1;
    if (pthread_create(&G.mth, NULL, models_th, NULL)) { G.mstate = 0; snprintf(G.mmsg, sizeof G.mmsg, "Could not start the request"); if (G.mkey) { free(G.mkey); G.mkey = NULL; } }
    else G.models_open = true;
}
static void signin_start(App *a) {
    char url[4096], why[400];
    if (!chatgpt_signin_begin(url, sizeof url, why, sizeof why)) { G.test_state = TS_FAIL; snprintf(G.test_msg, sizeof G.test_msg, "%s", why); return; }
    G.signing = true; G.test_state = TS_NONE; G.test_msg[0] = 0;
    chatgpt_open_browser(url);
    audit(a, "chatgpt-signin", NULL, "started", 0, "");
}
static void signin_poll(App *a) {
    if (!G.signing) return;
    char m[300]; int st = chatgpt_signin_status(m, sizeof m);
    if (st == CS_DONE || st == CS_FAILED || st == CS_IDLE) {
        G.signing = false; ai_key_changed();
        G.test_state = st == CS_FAILED ? TS_FAIL : TS_NONE; snprintf(G.test_msg, sizeof G.test_msg, "%s", m);
        audit(a, "chatgpt-signin", NULL, st == CS_DONE ? "signed-in" : "not-completed", 0, "");
        LOG_I("report: ChatGPT sign-in %s", st == CS_DONE ? "completed" : "not completed");
        if (st == CS_DONE) models_load(a);   /* the plan's models, to choose one */
    }
}
static void setup_test(App *a) {
    AiConfig c; setup_config(&c);
    char why[400];
    if (!ai_ready(&c, why, sizeof why)) { G.test_state = TS_FAIL; snprintf(G.test_msg, sizeof G.test_msg, "%s", why); return; }
    if (G.testj) { ai_free(G.testj); G.testj = NULL; }
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    /* a fixed sentence: no report, catalog, SQL or data of the user goes in a test (LI-001.1) */
    G.testj = ai_start(&c, "This is a connection test from Lume Data. Reply with the single word OK.", "Reply with the single word OK.", dir);
    G.test_state = TS_RUNNING; G.test_t0 = now_s();
    snprintf(G.test_msg, sizeof G.test_msg, "Testing %s...", G.sprov == SP_LOCAL ? c.endpoint : ai_provider_label(c.provider));
    char pv[40]; snprintf(pv, sizeof pv, "provider=%s", c.provider);
    audit(a, "ai-connection-test", NULL, "sent", 0, pv);
}
static void setup_poll(App *a) {
    signin_poll(a);
    if (G.mstate == 2) models_join();
    if (!G.testj || !ai_done(G.testj)) return;
    const AiResult *r = ai_result(G.testj);
    if (r->text) { G.test_state = TS_OK; snprintf(G.test_msg, sizeof G.test_msg, "Connected: %s answered in %.1f s. Nothing of yours was sent.", r->model[0] ? r->model : G.s_model.buf, ai_elapsed(G.testj)); }
    else { G.test_state = TS_FAIL; snprintf(G.test_msg, sizeof G.test_msg, "%s", r->err[0] ? r->err : "No answer"); }
    audit(a, "ai-connection-test", NULL, G.test_state == TS_OK ? "ok" : "failed", 0, "");
    ai_free(G.testj); G.testj = NULL;
}
static void setup_save(App *a) {
    if (G.test_state != TS_OK) return;
    if ((G.sprov == SP_ANTHROPIC || (G.sprov == SP_OPENAI && G.oa_mode == 1)) && G.s_key.buf[0]) {   /* the tested key goes to the OS secret store, nowhere else */
        char why[300];
        if (!secstore_set(AI_KEY_SERVICE, key_account(), G.s_key.buf, why, sizeof why)) { G.test_state = TS_FAIL; snprintf(G.test_msg, sizeof G.test_msg, "%s. Nothing was enabled.", why); return; }
        wipe_key_field(); ai_key_changed(); G.replacing_key = false;
        audit(a, "ai-key", NULL, "saved", 0, secstore_name());
        LOG_I("report: AI key saved in the %s", secstore_name());
    }
    char dir[1100], p[1200], tmp[1300]; pref_dir(a, dir, sizeof dir); snprintf(p, sizeof p, "%s/ai.txt", dir); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    /* keeps other settings (effort, limits); never a key: keys stay in the OS secret store */
    char keep[4096] = ""; size_t w = 0; FILE *f = fopen(p, "r");
    if (f) { char line[512]; while (fgets(line, sizeof line, f)) { if (!strncmp(line, "provider=", 9) || !strncmp(line, "model=", 6) || !strncmp(line, "model_fast=", 11) || !strncmp(line, "endpoint=", 9)) continue; size_t l = strlen(line); if (w + l < sizeof keep - 1) { memcpy(keep + w, line, l); w += l; keep[w] = 0; } } fclose(f); }
    f = fopen(tmp, "w"); if (!f) { snprintf(G.test_msg, sizeof G.test_msg, "Could not write %s", p); return; }
    AiConfig c; setup_config(&c);
    fprintf(f, "provider=%s\nmodel=%s\n", c.provider, c.model);
    if (G.s_fast.buf[0]) { char fm[64]; snprintf(fm, sizeof fm, "%s", G.s_fast.buf); for (char *q = fm; *q; q++) if (*q == '\n' || *q == '=') *q = '_'; fprintf(f, "model_fast=%s\n", fm); }
    { AiConfig d; ai_config_default(&d); ai_provider_defaults(&d, c.provider); if (G.sprov == SP_LOCAL || strcmp(c.endpoint, d.endpoint)) fprintf(f, "endpoint=%s\n", c.endpoint); }   /* a proxy / custom endpoint survives saving */
    fputs(keep, f); fclose(f); rename(tmp, p);
    ai_config_load(dir, &G.cfg);
    G.setup = false; G.test_state = TS_NONE;
    G.welcome = !strcmp(c.provider, "chatgpt");
    snprintf(G.amsg, sizeof G.amsg, "Lume Intelligence is on (%s, %s).", ai_provider_label(c.provider), G.cfg.model);
    char pv[40]; snprintf(pv, sizeof pv, "provider=%s", c.provider);
    audit(a, "ai-config", NULL, "enabled", 0, pv);
    LOG_I("report: Lume Intelligence enabled after a successful connection test (%s)", c.provider);
}
static void as_draw(void *view, void *ctx, DockRect drc, bool focused) {
    (void)view; (void)focused;
    App *a = (App *)ctx; SDL_Renderer *r = a->renderer;
    Rect rc = rect_make(drc.x, drc.y, drc.w, drc.h);
    draw_rect(r, rc, C(T_PANEL));
    RvDoc *d = active_doc(a); if (!d && G.adoc && doc_index(G.adoc) >= 0) d = G.adoc;
    static float ascroll, content_h;
    float wheel0 = rect_contains(rc, a->in.mouse_x, a->in.mouse_y) ? a->in.wheel_y : 0;   /* the chat area scrolls by itself */
    if (rect_contains(rc, a->in.mouse_x, a->in.mouse_y) && a->in.wheel_y != 0 && !(G.show_payload && a->in.mouse_y > rc.y + px(a, 300))) { ascroll -= a->in.wheel_y * px(a, 40); a->in.wheel_y = 0; }
    if (ascroll > content_h - rc.h + px(a, 20)) ascroll = content_h - rc.h + px(a, 20);
    if (ascroll < 0) ascroll = 0;
    static ScrollBar asb;   /* setup / off screens: the panel itself scrolls */
    if (content_h > 0) ui_scrollbar_input(a, &asb, rc, content_h + px(a, 20), &ascroll);
    clip_push(r, rc);
    float x = rc.x + px(a, 10), w = rc.w - px(a, 20), y = rc.y + px(a, 8) - ascroll, rh = px(a, 26);
    float y_top = y;
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    /* a widget only takes input when it is entirely inside the panel (nothing clickable under other panels) */
    #define IN(rr) ((rr).y >= rc.y && (rr).y + (rr).h <= rc.y + rc.h)
    #define BTN(rr, lab, st, en) (IN(rr) ? ui_button(a, rr, lab, st, en) : (ui_label(a, rr, lab, ALIGN_LEFT, T_TEXT_DIM), false))
    #define CHK(rr, lab, val) (IN(rr) ? ui_checkbox(a, rr, lab, val, true) : false)
    if (!G.cfg.provider[0]) ai_config_load(dir, &G.cfg);
    setup_poll(a); if (G.testj) app_wake_in(a, 200);
    bool off = !strcmp(G.cfg.provider, "none") || !G.cfg.provider[0];
    #define WRAP(txt, col, maxh) do { float hh_ = results_draw_wrapped(a->font_small, r, txt, rect_make(x, y, w, maxh), C(col), 0, true); y += hh_ + px(a, 6); } while (0)
    if (G.setup) {   /* 1 provider -> 2 what it needs -> 3 test -> 4 enable */
        ui_label(a, rect_make(x, y, w, rh), "Set up Lume Intelligence", ALIGN_LEFT, T_TEXT); y += rh + px(a, 4);
        ui_label(a, rect_make(x, y, w, px(a, 16)), "1. Provider", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
        static const char *const provs[] = { "Anthropic", "OpenAI", "Local model" };
        Rect sg = rect_make(x, y, w, rh);
        if (IN(sg)) { int k = ui_segmented(a, sg, provs, 3, G.sprov); if (k >= 0 && k != G.sprov) set_provider(k, 0); }
        y += rh + px(a, 6);
        WRAP(G.sprov == SP_ANTHROPIC ? "Requests go to Anthropic's API over HTTPS, only when you press Send and after you see what is sent."
             : G.sprov == SP_OPENAI ? "Requests go to OpenAI over HTTPS, only when you press Send and after you see what is sent. Use your ChatGPT plan, or an OpenAI API key."
             : "Requests go to a model running on this computer (an endpoint that speaks the Messages API). Nothing leaves this machine.", T_TEXT_DIM, px(a, 50));
        ui_label(a, rect_make(x, y, w, px(a, 16)), "2. What it needs", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
        char st_why[300] = ""; bool store_ok = secstore_available(st_why, sizeof st_why);
        if (G.sprov == SP_OPENAI && G.oa_mode == 0) {   /* the user's ChatGPT plan */
            char email[160];
            if (chatgpt_signed_in(email, sizeof email)) {
                char m[260]; snprintf(m, sizeof m, "\xE2\x9C\x93 Signed in%s%s \xC2\xB7 Using ChatGPT plan", email[0] ? " as " : "", email);
                WRAP(m, T_GREEN, px(a, 34));
                if (BTN(rect_make(x, y, px(a, 130), rh), "Manage usage", BTN_NORMAL, true)) chatgpt_open_url("https://chatgpt.com/settings/usage");
                if (BTN(rect_make(x + px(a, 136), y, px(a, 100), rh), "Sign out", BTN_DANGER, true)) {
                    chatgpt_sign_out(); ai_key_changed(); G.test_state = TS_NONE; G.nmodels = 0;
                    snprintf(G.test_msg, sizeof G.test_msg, "Signed out: the session was removed from the %s.", secstore_name());
                    audit(a, "chatgpt-signin", NULL, "signed-out", 0, "");
                }
                y += rh + px(a, 8);
            } else if (!store_ok) {
                WRAP(st_why, T_YELLOW, px(a, 60));
            } else if (G.signing) {
                char m[300]; chatgpt_signin_status(m, sizeof m);
                WRAP(m, T_TEXT_MUTED, px(a, 34));
                if (BTN(rect_make(x, y, px(a, 100), rh), "Cancel", BTN_NORMAL, true)) { chatgpt_signin_cancel(); G.signing = false; }
                y += rh + px(a, 8);
                app_wake_in(a, 200);
            } else {
                Rect cb = rect_make(x, y, px(a, 220), rh);
                if (BTN(cb, "Continue with ChatGPT", BTN_PRIMARY, true)) signin_start(a);
                ui_record("ai:setup:chatgpt", cb);
                y += rh + px(a, 6);
                char m[300]; snprintf(m, sizeof m, "Opens your browser to sign in with your ChatGPT account. Lume uses your plan's included usage; the session is kept in the %s.", secstore_name());
                WRAP(m, T_TEXT_DIM, px(a, 50));
            }
            if (!G.signing && BTN(rect_make(x, y, px(a, 200), rh), "Use an API key instead", BTN_QUIET, true)) set_provider(SP_OPENAI, 1);
            y += rh + px(a, 6);
        } else if (G.sprov != SP_LOCAL) {
            int src = key_source();
            bool oa = G.sprov == SP_OPENAI;
            if (src == AK_ENV) {
                WRAP(oa ? "\xE2\x9C\x93 API key from the environment (OPENAI_API_KEY). It takes precedence over a saved key." : "\xE2\x9C\x93 API key from the environment (ANTHROPIC_API_KEY / LUME_AI_KEY). It takes precedence over a saved key.", T_GREEN, px(a, 40));
            } else if (src == AK_STORE && !G.replacing_key) {
                char m[200]; snprintf(m, sizeof m, "\xE2\x9C\x93 API key saved in the %s", secstore_name());
                WRAP(m, T_GREEN, px(a, 34));
                if (BTN(rect_make(x, y, px(a, 130), rh), "Replace key", BTN_NORMAL, true)) { G.replacing_key = true; G.test_state = TS_NONE; G.test_msg[0] = 0; wipe_key_field(); }
                if (BTN(rect_make(x + px(a, 136), y, px(a, 130), rh), "Remove key", BTN_DANGER, true)) {
                    secstore_delete(AI_KEY_SERVICE, key_account()); ai_key_changed(); G.test_state = TS_NONE;
                    snprintf(G.test_msg, sizeof G.test_msg, "Key removed from the %s.", secstore_name());
                    audit(a, "ai-key", NULL, "removed", 0, secstore_name());
                }
                y += rh + px(a, 8);
            } else if (!store_ok) {
                WRAP(st_why, T_YELLOW, px(a, 60));
            } else {
                ui_label(a, rect_make(x, y, w, px(a, 16)), oa ? "Your OpenAI API key" : "Your Anthropic API key", ALIGN_LEFT, T_TEXT_DIM); y += px(a, 18);
                Rect kr = rect_make(x, y, w, rh);
                G.s_key.password = true;
                if (IN(kr) && ui_textfield(a, &G.s_key, kr, oa ? "Paste it here (sk-...)" : "Paste it here (sk-ant-...)", focus_id == 10)) focus_id = 10;
                ui_record("ai:setup:key", kr); y += rh + px(a, 4);
                if (!oa && G.s_key.buf[0] && strncmp(G.s_key.buf, "sk-ant-", 7)) WRAP("This does not look like an Anthropic API key (they start with sk-ant-). Test it to be sure.", T_YELLOW, px(a, 34));
                if (oa && G.s_key.buf[0] && (strncmp(G.s_key.buf, "sk-", 3) || !strncmp(G.s_key.buf, "sk-ant-", 7))) WRAP("This does not look like an OpenAI API key (they start with sk-). Test it to be sure.", T_YELLOW, px(a, 34));
                char m[300]; snprintf(m, sizeof m, "Create one at %s. It is tested first, then saved in the %s when you press Enable; never in a file, a log or on screen.", oa ? "platform.openai.com > API keys" : "console.anthropic.com > API Keys", secstore_name());
                WRAP(m, T_TEXT_DIM, px(a, 50));
            }
            if (oa) { if (BTN(rect_make(x, y, px(a, 220), rh), "Use your ChatGPT plan instead", BTN_QUIET, true)) set_provider(SP_OPENAI, 0); y += rh + px(a, 6); }
        } else {
            ui_label(a, rect_make(x, y, w, px(a, 16)), "Endpoint (this computer only)", ALIGN_LEFT, T_TEXT_DIM); y += px(a, 18);
            Rect er = rect_make(x, y, w, rh);
            if (IN(er) && ui_textfield(a, &G.s_endpoint, er, "http://127.0.0.1:11434/v1/messages", focus_id == 8)) focus_id = 8;
            ui_record("ai:setup:endpoint", er); y += rh + px(a, 6);
        }
        ui_label(a, rect_make(x, y, w, px(a, 16)), "Model", ALIGN_LEFT, T_TEXT_DIM); y += px(a, 18);
        bool oa = G.sprov == SP_OPENAI;
        Rect mr = rect_make(x, y, oa ? w - px(a, 116) : w, rh);
        if (IN(mr) && ui_textfield(a, &G.s_model, mr, G.sprov == SP_ANTHROPIC ? "claude-opus-5-5" : oa ? "choose from Load models" : "model name of the local server", focus_id == 9)) focus_id = 9;
        ui_record("ai:setup:model", mr);
        if (oa) {
            bool can = G.oa_mode == 0 ? chatgpt_signed_in(NULL, 0) : (G.s_key.buf[0] || (key_source() != AK_NONE && !G.replacing_key));
            if (BTN(rect_make(x + w - px(a, 110), y, px(a, 110), rh), G.mstate == 1 ? "Loading..." : "Load models", BTN_NORMAL, can && G.mstate != 1)) models_load(a);
        }
        y += rh + px(a, 6);
        {   /* roadmap item 25 (the Intelligence settings): a fast model for quick requests */
            ui_label(a, rect_make(x, y, w, px(a, 16)), "Fast model for quick requests: Explain, Fix the error, Cmd+K (optional)", ALIGN_LEFT, T_TEXT_DIM); y += px(a, 18);
            Rect fr = rect_make(x, y, w, rh);
            if (IN(fr) && ui_textfield(a, &G.s_fast, fr, "same as the model", focus_id == 24)) focus_id = 24;
            ui_record("ai:setup:fastmodel", fr); y += rh + px(a, 6);
        }
        if (oa && (G.mmsg[0] || G.mstate == 1)) { WRAP(G.mmsg, G.nmodels ? T_TEXT_DIM : G.mstate == 1 ? T_TEXT_MUTED : T_RED, px(a, 34)); if (G.mstate == 1) app_wake_in(a, 200); }
        if (oa && G.models_open && G.nmodels) {   /* the account's models: one click picks */
            float bx = x, bw = (w - px(a, 6)) / 2;
            for (int i = 0; i < G.nmodels && i < 12; i++) {
                Rect br = rect_make(bx, y, bw, rh);
                if (BTN(br, G.models[i], !strcmp(G.models[i], G.s_model.buf) ? BTN_SELECTED : BTN_NORMAL, true)) { textfield_set(&G.s_model, G.models[i]); G.test_state = TS_NONE; G.test_msg[0] = 0; }
                if (i % 2) { bx = x; y += rh + px(a, 4); } else bx = x + bw + px(a, 6);
            }
            if (G.nmodels % 2 && G.nmodels < 12) y += rh + px(a, 4);
        }
        y += px(a, 4);
        ui_label(a, rect_make(x, y, w, px(a, 16)), "3. Test the connection", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
        WRAP("Sends one fixed sentence; none of your reports, SQL or data.", T_TEXT_DIM, px(a, 34));
        if (BTN(rect_make(x, y, px(a, 150), rh), G.test_state == TS_RUNNING ? "Testing..." : "Test connection", BTN_NORMAL, G.test_state != TS_RUNNING && G.s_model.buf[0] && (G.sprov == SP_LOCAL || (G.sprov == SP_OPENAI && G.oa_mode == 0 ? chatgpt_signed_in(NULL, 0) : (G.s_key.buf[0] || (key_source() != AK_NONE && !G.replacing_key)))))) setup_test(a);
        y += rh + px(a, 6);
        if (G.test_msg[0]) WRAP(G.test_msg, G.test_state == TS_OK ? T_GREEN : G.test_state == TS_FAIL ? T_RED : T_TEXT_MUTED, px(a, 70));
        ui_label(a, rect_make(x, y, w, px(a, 16)), "4. Enable", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
        if (BTN(rect_make(x, y, px(a, 150), rh), "Enable", BTN_PRIMARY, G.test_state == TS_OK)) setup_save(a);
        if (BTN(rect_make(x + px(a, 156), y, px(a, 100), rh), "Cancel", BTN_NORMAL, true)) { G.setup = false; wipe_key_field(); if (G.testj) { ai_free(G.testj); G.testj = NULL; } if (G.signing) { chatgpt_signin_cancel(); G.signing = false; } }
        y += rh + px(a, 6);
        if (G.test_state != TS_OK) WRAP("Enable becomes available after a successful test.", T_TEXT_DIM, px(a, 30));
        y += rh;
        content_h = y - y_top; clip_pop(r); ui_scrollbar_draw(a, &asb, rc, content_h + px(a, 20), ascroll);
        return;
    }
    if (off) {   /* nothing configured: one clear entry point */
        ui_label(a, rect_make(x, y, w, rh), "Lume Intelligence is off", ALIGN_LEFT, T_TEXT); y += rh;
        WRAP("An optional assistant that builds and changes reports from questions in plain language. Reports, the semantic catalog and manual editing work without it.", T_TEXT_DIM, px(a, 70));
        if (BTN(rect_make(x, y, px(a, 160), rh), "Set up...", BTN_PRIMARY, true)) setup_open(a);
        y += rh + px(a, 6);
        content_h = y - y_top; clip_pop(r); ui_scrollbar_draw(a, &asb, rc, content_h + px(a, 20), ascroll);
        return;
    }
    /* on: provider and model, with its settings; not ready says what is missing */
    char why[400]; bool ready = ai_ready(&G.cfg, why, sizeof why);
    char hdr[200]; snprintf(hdr, sizeof hdr, "%s \xC2\xB7 %s", ai_provider_label(G.cfg.provider), G.cfg.model);
    if (G.nreq) { char tt[16]; tok_fmt(G.tok_in + G.tok_out, tt, sizeof tt); size_t hl = strlen(hdr); snprintf(hdr + hl, sizeof hdr - hl, " \xC2\xB7 %s tokens", tt); }
    ui_label(a, rect_make(x, y, w - px(a, 190), rh), hdr, ALIGN_LEFT, T_TEXT);
    if (BTN(rect_make(x + w - px(a, 184), y, px(a, 90), rh), "Settings...", BTN_QUIET, true)) setup_open(a);
    if (BTN(rect_make(x + w - px(a, 90), y, px(a, 90), rh), "Turn off", BTN_QUIET, true)) set_ai_enabled(a, false);
    y += rh + px(a, 4);
    if (G.welcome && !strcmp(G.cfg.provider, "chatgpt")) {
        WRAP("You\xE2\x80\x99re using your ChatGPT plan. Requests from Lume count toward your plan's usage limits; see them in Manage usage.", T_TEXT, px(a, 50));
        if (BTN(rect_make(x, y, px(a, 90), rh), "Got it", BTN_PRIMARY, true)) G.welcome = false;
        if (BTN(rect_make(x + px(a, 96), y, px(a, 130), rh), "Manage usage", BTN_QUIET, true)) chatgpt_open_url("https://chatgpt.com/settings/usage");
        y += rh + px(a, 8);
    }
    if (!ready) {
        WRAP(why, T_YELLOW, px(a, 60));
        if (BTN(rect_make(x, y, px(a, 160), rh), "Open settings", BTN_PRIMARY, true)) setup_open(a);
        y += rh + px(a, 8);
    }
    /* a click anywhere outside the panel takes the keyboard away from its fields (the editor gets it) */
    if (a->in.mouse_pressed && !rect_contains(rc, a->in.mouse_x, a->in.mouse_y) && (focus_id == 7 || (focus_id >= 8 && focus_id <= 10) || (focus_id >= 21 && focus_id <= 24))) focus_id = 0;
    TabInfo qti; G.qmode = active_query_tab(a, &qti);
    if (G.qmode) d = NULL;   /* a query tab is active: the SQL assistant, about that tab */
    if (!d && !G.qmode) {   /* no report yet: what to do, as buttons */
        if (panels_active_designer(a->panels, a)) {   /* the Table Designer is in front: what the assistant does there */
            WRAP("Table Designer: Lume Intelligence reviews the table (names, types, keys, indexes) and proposes changes you apply in the designer.", T_TEXT_MUTED, px(a, 60));
            if (BTN(rect_make(x, y, fminf(w, px(a, 200)), rh), "Review this table", BTN_PRIMARY, commands_enabled("intelligence.review_table", NULL, 0))) commands_invoke(a, "intelligence.review_table");
            y += rh + px(a, 8);
        }
        WRAP("Open a report, or start one from a question.", T_TEXT_MUTED, px(a, 40));
        bool stack = w < px(a, 332);   /* narrow panel: one button per line, never cut */
        float b1 = stack ? fminf(w, px(a, 220)) : px(a, 200);
        if (BTN(rect_make(x, y, b1, rh), "New report from a question", panels_active_designer(a->panels, a) ? BTN_NORMAL : BTN_PRIMARY, true)) { RvDoc *nd = create_report(a, "Report from a question"); if (nd) { G.adoc = nd; focus_id = 7; } }
        if (stack) y += rh + px(a, 6);
        if (BTN(rect_make(stack ? x : x + px(a, 206), y, stack ? b1 : px(a, 120), rh), "New report", BTN_QUIET, true)) create_report(a, NULL);
        y += rh + px(a, 8);
        y += rh; content_h = y - y_top; clip_pop(r); ui_scrollbar_draw(a, &asb, rc, content_h + px(a, 20), ascroll);
        return;
    }
    /* ---- chat layout: the header above, the conversation in the middle (it scrolls), the composer below ---- */
    if (ascroll != 0) { ascroll = 0; app_wake_in(a, 16); }
    if (G.qmode) { if (G.chat_tab != qti.id + 1 || G.chat_doc) { chat_clear(); G.chat_doc = NULL; G.chat_tab = qti.id + 1; G.ast = G.ai ? G.ast : AS_IDLE; } }
    else if (G.chat_doc != d || G.chat_tab) { chat_clear(); G.chat_doc = d; G.chat_tab = 0; }
    bool busy = G.ai || G.ast == AS_QUERYING;
    float comp_h = px(a, 22) + px(a, 4) + rh + px(a, 6) + rh + px(a, 8) + (G.attach[0] ? px(a, 26) : 0);
    float mid_top = y + px(a, 2);
    float mid_bot = rc.y + rc.h - comp_h - px(a, 6);
    if (mid_bot < mid_top + px(a, 60)) mid_bot = mid_top + px(a, 60);
    Rect mid = rect_make(rc.x, mid_top, rc.w, mid_bot - mid_top);
    draw_rect(r, rect_make(rc.x, mid.y - 1, rc.w, 1), C(T_BORDER));
    draw_rect(r, rect_make(rc.x, mid.y + mid.h, rc.w, 1), C(T_BORDER));
    if (rect_contains(mid, a->in.mouse_x, a->in.mouse_y) && wheel0 != 0 && !(G.show_payload && G.ctx_open)) { G.chat_scroll -= wheel0 * px(a, 22); G.chat_stick = false; ui_scrollbar_touch(&G.chat_sb); }
    if (ui_scrollbar_input(a, &G.chat_sb, mid, G.chat_total, &G.chat_scroll) && G.chat_sb.drag) G.chat_stick = false;
    /* clamped before drawing (with the last frame's height), so a scroll never draws past an end and snaps back */
    { float m0 = G.chat_total > mid.h ? G.chat_total - mid.h : 0; if (G.chat_scroll > m0) { G.chat_scroll = m0; G.chat_stick = true; } if (G.chat_scroll < 0) G.chat_scroll = 0; }
    /* new content (a message, a state, a plan step) brings the view to the newest */
    unsigned sig = (unsigned)G.nchat * 131u + (unsigned)G.ast * 17u + (unsigned)G.nsteps * 7u + (unsigned)strlen(G.amsg) + (unsigned)G.ctx_open * 3u + (unsigned)G.steps_open * 5u;
    if (sig != G.chat_sig) { G.chat_sig = sig; G.chat_stick = true; }
    clip_push(r, mid);
    float total;
    {
        Rect rc = mid;   /* the IN / BTN / CHK macros now test against the conversation area */
        float y = mid.y + px(a, 8) - G.chat_scroll, y0 = y;
        float pad = px(a, 8);
        if (!G.nchat && G.ast == AS_IDLE && !G.amsg[0]) {
            if (G.qmode) WRAP("Ask about this query: write it, change it, fix its error or explain it. You review every change before it goes into the editor, and Lume never runs it. Cmd+K opens this from the editor.", T_TEXT_DIM, px(a, 70));
            else WRAP(d && d->sel[0] ? "Ask about the selected card: change its chart, metric, period or breakdown." : "Ask for a report or a change in plain language, e.g. \"Receita dos últimos 12 meses por região\". You see the plan and approve it before anything changes.", T_TEXT_DIM, px(a, 60));
        }
        for (int i = 0; i < G.nchat; i++) {
            if (G.chat[i].role == 't') {   /* the tokens of the answer above */
                ui_label(a, rect_make(x + px(a, 2), y - px(a, 4), w, px(a, 16)), G.chat[i].text, ALIGN_LEFT, T_TEXT_DIM);
                y += px(a, 16); continue;
            }
            bool me = G.chat[i].role == 'u';
            float copy_w = px(a, 50);   /* room beside the bubble for its Copy button */
            float maxw = me ? w * 0.82f : w - copy_w;
            float tw = font_width(a->font_small, G.chat[i].text, -1);
            float bw = me && !strchr(G.chat[i].text, '\n') && tw + 2 * pad + px(a, 6) < maxw ? tw + 2 * pad + px(a, 6) : maxw;   /* slack: the wrapper breaks a line that exactly fits */
            static int ws[400], wl[400];
            float th = wrap_text(a->font_small, G.chat[i].text, bw - 2 * pad, ws, wl, 400) * font_height(a->font_small);
            Rect br = rect_make(me ? x + w - bw : x, y, bw, th + 2 * px(a, 6));
            draw_rect(r, br, C(me ? T_ACCENT_DIM : T_PANEL_ALT));
            sel_text_block(a, r, i, G.chat[i].text, br.x + pad, br.y + px(a, 6), bw - 2 * pad, C(T_TEXT), mid, NULL);
            Rect row = rect_make(x, br.y, w, br.h);
            if (rect_contains(row, a->in.mouse_x, a->in.mouse_y) && !G.sel_drag) {   /* copy the whole message: beside the bubble, never over its text */
                Rect cb = me ? rect_make(br.x - copy_w, br.y + px(a, 2), copy_w - px(a, 4), px(a, 20)) : rect_make(br.x + br.w + px(a, 4), br.y + px(a, 2), copy_w - px(a, 4), px(a, 20));
                if (IN(cb) && ui_button(a, cb, "Copy", BTN_QUIET, true)) { SDL_SetClipboardText(G.chat[i].text); app_status(a, "Message copied"); }
            }
            y += br.h + px(a, 8);
        }
        /* the current turn: its state and what it lets you do */
        if (G.ast != AS_IDLE || G.amsg[0]) {
            float ly0 = y; g_card_top = y - y0;   /* where the state card starts, in the conversation */
            float xs = x; x += px(a, 10); w -= px(a, 10);
            char st[200]; snprintf(st, sizeof st, "%s", AS_LABEL[G.ast]);
            if (G.ast == AS_THINKING && G.ai) { snprintf(st, sizeof st, "Thinking (%.1f s)", ai_elapsed(G.ai)); app_wake_in(a, 250); }
            uint32_t sc = G.ast == AS_ERROR ? T_RED : G.ast == AS_DONE ? T_GREEN : G.ast == AS_PLAN || G.ast == AS_CLARIFY || G.ast == AS_LOCAL_CLARIFY ? T_YELLOW : T_TEXT_MUTED;
            if (G.ast != AS_IDLE) { ui_label(a, rect_make(x, y, w, px(a, 18)), st, ALIGN_LEFT, sc); y += px(a, 20); }
            if (G.amsg[0]) { y += sel_text_block(a, r, G.nchat, G.amsg, x, y, w, C(T_TEXT), mid, NULL) + px(a, 6); }
            if ((G.ast == AS_CANNOT || G.ast == AS_ERROR) && G.nqhist && !G.ai) {   /* recover: ask again, or reword first */
                const char *lq = G.qhist[G.nqhist - 1];
                if (BTN(rect_make(x, y, px(a, 100), px(a, 24)), "Try again", BTN_NORMAL, true)) { textfield_set(&G.q, lq); ai_send(a, d); }
                if (BTN(rect_make(x + px(a, 106), y, px(a, 130), px(a, 24)), "Edit the question", BTN_QUIET, true)) { textfield_set(&G.q, lq); focus_id = 7; }
                y += px(a, 30);
            }
            if (G.ast == AS_NEED_SCHEMA) {
                Connection *sc = schema_connection(a);
                /* which database: the user sees it and can pick another before anything is shared */
                if (a->conns && a->conns->n > 1) {
                    ui_label(a, rect_make(x, y, w, px(a, 16)), "Database (choose one):", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
                    for (int i = 0; i < a->conns->n && i < 12; i++) {
                        Connection *ci = a->conns->items[i];
                        Rect rr = rect_make(x, y, w, px(a, 24)); char tg[48]; snprintf(tg, sizeof tg, "ai:db %s", ci->name); ui_record(tg, rr);
                        if (IN(rr) && radio_row(a, rr, ci->name, ci == sc, !G.schema_wait)) { snprintf(G.schema_main, sizeof G.schema_main, "%s", ci->id); sc = ci; }
                        y += px(a, 25);
                    }
                    y += px(a, 6);
                }
                /* who runs the assistant's SQL: this login, or a SELECT-only connection mapped to it (BL-051) */
                if (sc && a->conns && a->conns->n > 1) {
                    ui_label(a, rect_make(x, y, w, px(a, 16)), "Run the assistant's queries as (choose one):", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
                    { Rect rr = rect_make(x, y, w, px(a, 24)); char sl0[160]; snprintf(sl0, sizeof sl0, "Same login (%s)", sc->name);
                      if (IN(rr) && radio_row(a, rr, sl0, !sc->ai_conn[0], !G.schema_wait)) { sc->ai_conn[0] = 0; conns_save(a->conns); G.gr_conn[0] = 0; }
                      y += px(a, 25); }
                    for (int i = 0; i < a->conns->n && i < 12; i++) {
                        Connection *ci = a->conns->items[i]; if (ci == sc) continue;
                        Rect rr = rect_make(x, y, w, px(a, 24));
                        bool picked = IN(rr) && radio_row(a, rr, ci->name, !strcmp(sc->ai_conn, ci->id), !G.schema_wait);
                        y += px(a, 25);
                        if (picked) { snprintf(sc->ai_conn, sizeof sc->ai_conn, "%s", ci->id); conns_save(a->conns); G.gr_conn[0] = 0; }
                    }
                    y += px(a, 6);
                    Connection *xc = ai_exec_of(a, sc);
                    if (xc && xc->db && strcmp(G.gr_conn, xc->id) && !xc->db->busy) grants_check(xc);
                    if (xc && !strcmp(G.gr_conn, xc->id)) {
                        char gm[300];
                        if (G.gr_state == AG_READONLY) snprintf(gm, sizeof gm, "\xE2\x9C\x93 %s: %s. The database itself refuses writes from the assistant's SQL.", xc->name, G.gr_note);
                        else if (G.gr_state == AG_WRITES) snprintf(gm, sizeof gm, "%s can write (%s). The read-only policy and transaction still apply, but a SELECT-only login is safer: map one above.", xc->name, G.gr_note);
                        else snprintf(gm, sizeof gm, "%s: %s.", xc->name, G.gr_note);
                        WRAP(gm, G.gr_state == AG_READONLY ? T_GREEN : G.gr_state == AG_WRITES ? T_YELLOW : T_TEXT_DIM, px(a, 50));
                    }
                }
                /* the decision: actions, apart from the choices above */
                draw_rect(r, rect_make(x, y, w, px(a, 1)), C(T_BORDER)); y += px(a, 10);
                char lab[120]; snprintf(lab, sizeof lab, "Share the structure of %s", sc ? sc->name : "the connection");
                if (BTN(rect_make(x, y, w, rh + px(a, 4)), lab, BTN_PRIMARY, sc && !G.schema_wait)) schema_allow(a);
                y += rh + px(a, 8);
                if (BTN(rect_make(x, y, w, rh), "Not now", BTN_NORMAL, true)) { G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Nothing was shared or sent."); }
                y += rh + px(a, 8);
                char m[300]; snprintf(m, sizeof m, "Table and column names can themselves be sensitive. The structure goes to %s with each request until you turn it off in Context.", ai_provider_label(G.cfg.provider));
                WRAP(m, T_TEXT_DIM, px(a, 40));
                if (CHK(rect_make(x, y, w, px(a, 20)), "Also share category values (more accurate filters)", G.cx.on[CX_CATVALUES])) { G.cx.on[CX_CATVALUES] = !G.cx.on[CX_CATVALUES]; consent_save(a, ai_exec_of(a, schema_connection(a))); }
                y += px(a, 22);
                WRAP("Up to 20 distinct values of short text columns such as status or channel, e.g. 'cancelled'. Columns that look personal (names, e-mails, documents, phones, addresses) are never sampled. These are data: share them only if that is acceptable for this database.", G.cx.on[CX_CATVALUES] ? T_YELLOW : T_TEXT_DIM, px(a, 60));
            }
            if (G.ast == AS_CHECKING) app_wake_in(a, 150);
            if (G.ast == AS_SCOPE) {
                if (BTN(rect_make(x, y, px(a, 200), rh), "Allow changes to the page", BTN_PRIMARY, true)) { G.allow_global = true; G.ast = AS_IDLE; audit(a, "propose_report_patch", d, "page-scope-allowed", 0, ""); plan_from_answer(a, d); }
                if (BTN(rect_make(x + px(a, 206), y, px(a, 190), rh), "Only the selected card", BTN_NORMAL, true)) { G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Kept to the selected card: nothing changed. Ask again about that card, or click an empty area to deselect it."); }
                y += rh + px(a, 6);
            }
            if (G.ast == AS_CLARIFY || G.ast == AS_LOCAL_CLARIFY) {
                for (int i = 0; i < G.nopt; i++) {
                    const SemMetricDef *md = sem_cur(sem_metric_c(&G.cat, G.opts[i]));
                    char lab[200]; snprintf(lab, sizeof lab, "%s: %s", md ? md->name : G.opts[i], md ? md->description : "");
                    if (BTN(rect_make(x, y, w, rh), lab, BTN_NORMAL, true)) ai_choose(a, i);
                    y += rh + px(a, 4);
                }
                if (G.ast == AS_CLARIFY) WRAP("Answer below; the assistant sees this conversation.", T_TEXT_DIM, px(a, 30));
            }
            if (G.ast == AS_PLAN || G.ast == AS_QUERYING || G.ast == AS_DONE || G.ast == AS_CANCELLED) {
                if (G.qmode || G.req_qmode) {   /* the SQL the assistant proposes, as it would go into the editor */
                    if (G.ast == AS_PLAN && G.qedit_sql) {
                        /* the preview: at most 16 lines (the whole text goes to the editor / the new tab) */
                        static char pv[4096]; int nl = 0, lines = 1; size_t k = 0;
                        for (const char *q = G.qedit_sql; *q; q++) if (*q == '\n') lines++;
                        for (const char *q = G.qedit_sql; *q && k + 2 < sizeof pv; q++) { if (*q == '\n' && ++nl >= 16) break; pv[k++] = *q; }
                        pv[k] = 0;
                        if (lines > 16) { size_t L = strlen(pv); snprintf(pv + L, sizeof pv - L, "\n... %d more line(s): the whole text goes %s", lines - 16, G.qkind == AA_NEW_TAB ? "to the new tab" : "into the editor"); }
                        if (G.qwrites) { char wm[300]; snprintf(wm, sizeof wm, "\xE2\x9A\xA0 This changes data or structure (%s). Lume never runs it: review before you run it.", G.qwrite_why); WRAP(wm, T_YELLOW, px(a, 50)); }
                        float bh = results_draw_wrapped(a->font_mono ? a->font_mono : a->font_small, r, pv, rect_make(x + px(a, 8), y + px(a, 6), w - px(a, 16), px(a, 2000)), C(T_TEXT), 0, false);
                        Rect box = rect_make(x, y, w, bh + px(a, 12));
                        draw_rect(r, box, C(T_EDITOR_BG));
                        results_draw_wrapped(a->font_mono ? a->font_mono : a->font_small, r, pv, rect_make(x + px(a, 8), y + px(a, 6), w - px(a, 16), bh + px(a, 4)), C(T_TEXT), 0, true);
                        ui_record("ai:sql:proposal", box);
                        y += box.h + px(a, 6);
                        {   /* a plan is open for this connection: the rewrite's estimated plan beside it (never run) */
                            int nst = 0; bool q1 = false; for (const char *q = G.qedit_sql; *q; q++) { if (*q == '\'') q1 = !q1; else if (*q == ';' && !q1) { const char *r2 = q + 1; while (*r2 && isspace((unsigned char)*r2)) r2++; if (*r2) nst++; } }
                            Connection *pc = conn_of(a, G.qconn); char *pl = pc && nst == 0 ? planview_context_for(pc->name) : NULL;   /* one statement only: a plan is per statement */
                            if (pl) { Rect cb = rect_make(x, y, px(a, 220), px(a, 24)); ui_record("ai:compare-plans", cb); if (BTN(cb, "Compare plans (estimated)", BTN_NORMAL, true)) { if (planview_compare_sql(a, pc, G.qedit_sql)) panels_show(a->panels, a, "lume.data.plan"); } y += px(a, 30); free(pl); }
                        }
                    }
                } else if (G.ans.rationale[0]) { char rt[700]; snprintf(rt, sizeof rt, "Why these charts: %s", G.ans.rationale); WRAP(rt, T_TEXT_MUTED, px(a, 120)); }
                /* the steps (tools, compiled queries, policy) stay one click away; the decision is in the composer */
                int nref = 0; for (int i = 0; i < G.nsteps; i++) nref += G.steps[i].st == ST_REFUSED;
                bool sopen = G.ast == AS_PLAN ? !G.steps_closed : G.steps_open;   /* before approving, the steps are shown */
                char sl[160]; snprintf(sl, sizeof sl, "%s  %s%d step(s)", sopen ? "\xE2\x96\xBE" : "\xE2\x96\xB8", G.ast == AS_PLAN ? "What will happen: " : "", G.nsteps);
                if (nref) { size_t L = strlen(sl); snprintf(sl + L, sizeof sl - L, " \xC2\xB7 refused: %d", nref); }
                if (G.nsteps) { Rect hr = rect_make(x, y, w, px(a, 22)); ui_record("ai:steps", hr);
                    if (IN(hr) && rect_contains(hr, a->in.mouse_x, a->in.mouse_y)) a->want_cursor = CURSOR_HAND;
                    if (IN(hr) && rect_contains(hr, a->in.mouse_x, a->in.mouse_y) && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; if (G.ast == AS_PLAN) G.steps_closed = !G.steps_closed; else G.steps_open = !G.steps_open; }
                    ui_label(a, hr, sl, ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 24); }
                for (int i = 0; sopen && i < G.nsteps; i++) {
                    char line[600]; snprintf(line, sizeof line, "%d. %s: %s  \xC2\xB7 %s", i + 1, step_verb(G.steps[i].tool), G.steps[i].detail, step_status(G.steps[i].st, G.ast == AS_PLAN));
                    float hh = results_draw_wrapped(a->font_small, r, line, rect_make(x, y, w, px(a, 50)), C(G.steps[i].st == ST_REFUSED ? T_RED : T_TEXT), 0, true);
                    y += hh + px(a, 3);
                }
                y += px(a, 4);
            }
            if (G.ast == AS_DONE) {
                /* LI-013: links to what was produced */
                for (int i = 0; i < G.napplied; i++) {
                    char lab[80]; snprintf(lab, sizeof lab, "Open SQL of %s", G.applied[i]);
                    if (d && BTN(rect_make(x, y, px(a, 150), px(a, 22)), lab, BTN_QUIET, true)) { int pg; RptVisual *v = rpt_find(&d->r, G.applied[i], &pg); char why2[300]; RvData *dx = v ? visual_data(a, d, v, false, why2, sizeof why2) : NULL; if (dx) { char tt[48]; snprintf(tt, sizeof tt, "%s.sql", G.applied[i]); panels_open_tab(a->panels, a, tt, dx->conn, dx->sql); } }
                    char sl[80]; snprintf(sl, sizeof sl, "Show %s", G.applied[i]);
                    if (d && BTN(rect_make(x + px(a, 156), y, px(a, 90), px(a, 22)), sl, BTN_QUIET, true)) { snprintf(d->sel, sizeof d->sel, "%s", G.applied[i]); panels_activate_tab(a->panels, a, d->tab_id); }
                    y += px(a, 24);
                }
                if (d && BTN(rect_make(x, y, px(a, 160), rh), "Undo the AI change", BTN_NORMAL, d->h.nu > 0)) { if (rpt_undo(&d->h, &d->r)) { mark_changed(d); G.ast = AS_IDLE; snprintf(G.amsg, sizeof G.amsg, "Undone: the previous version is back (no query re-run)."); audit(a, "propose_report_patch", d, "undone", 0, ""); } }
                y += rh + px(a, 4);
            }
            draw_rect(r, rect_make(xs, ly0, px(a, 3), y - ly0), C(sc == T_TEXT_MUTED ? T_BORDER : sc));
            x = xs; w += px(a, 10);
            y += px(a, 6);
        }
        /* context (LI-010 / 079), opened from the composer */
        if (G.ctx_open) {
            ui_label(a, rect_make(x, y, w, px(a, 16)), "Context sent with each request", ALIGN_LEFT, T_TEXT_MUTED); y += px(a, 18);
            for (int i = 0; i < CX_N; i++) {
                char lab[160]; snprintf(lab, sizeof lab, "%s%s", ag_ctx_label(i), ag_ctx_sensitive(i) ? "  [separate consent]" : "");
                if (CHK(rect_make(x, y, w, px(a, 20)), lab, G.cx.on[i])) { G.cx.on[i] = !G.cx.on[i]; audit(a, "context", d, G.cx.on[i] ? "item-on" : "item-off", 0, ""); if (i == CX_SCHEMA || i == CX_CATVALUES) consent_save(a, ai_exec_of(a, schema_connection(a))); }
                y += px(a, 21);
            }
            { char tk[200]; snprintf(tk, sizeof tk, "This conversation: %d request(s), %d tokens in, %d out (as the provider reports them).%s", G.nreq, G.tok_in, G.tok_out, G.cfg.model_fast[0] ? " Quick requests use the fast model." : ""); WRAP(tk, T_TEXT_DIM, px(a, 34)); }
            {   /* the workspace glossary: business terms and where they live */
                const char *gl = glossary_text(a); int nt = 0; for (const char *p = gl; *p; p++) nt += *p == '\n';
                char gm[200]; snprintf(gm, sizeof gm, "Glossary: %d term(s). Sent with every request; its definitions also steer which tables are picked.", nt);
                WRAP(gm, T_TEXT_MUTED, px(a, 34));
                if (BTN(rect_make(x, y, px(a, 140), px(a, 24)), "Edit glossary...", BTN_QUIET, true)) {
                    struct stat st; if (stat(G.gloss_path, &st) != 0) { FILE *gf = fopen(G.gloss_path, "w"); if (gf) { fputs("# Business terms of this workspace and where they live, one per line: term = definition\n# beneficiário ativo = TB_USUARIO with DT_CANCELAMENTO IS NULL\n", gf); fclose(gf); } }
                    panels_open_file_tab(a->panels, a, G.gloss_path);
                }
                y += px(a, 30);
            }
            WRAP("Also sent: the earlier turns of this conversation. Never sent: raw rows, credentials, connection details, restricted columns.", T_TEXT_DIM, px(a, 40));
            if (G.cx.on[CX_VALUES]) { char m[300]; snprintf(m, sizeof m, "Data values are ON: the selected card's numbers and labels go to %s. That is your decision and your risk: keep it off for confidential or personal data.", ai_provider_label(G.cfg.provider)); WRAP(m, T_RED, px(a, 50)); }
            {   /* large databases: what of the structure may go, and what the next request would carry */
                Connection *sc = ai_exec_of(a, schema_connection(a));
                if (sc && sx_supported(sc) && G.cx.on[CX_SCHEMA]) {
                    scope_load(a, sc);
                    char st[300];
                    if (G.sx_state == SX_READY && !strcmp(G.sx_conn, sc->id)) {
                        const char *qq = G.q.buf[0] ? G.q.buf : G.sent_q;
                        if (qq[0] && strcmp(G.est_q, qq)) { snprintf(G.est_q, sizeof G.est_q, "%s", qq); sx_schema_text(a, sc, d, qq); }
                        snprintf(st, sizeof st, "Structure of %s: %d tables indexed here. Next request: %d table(s), about %d tokens.", sc->name, G.sx.n, G.est_tables, G.est_bytes / 4);
                    } else snprintf(st, sizeof st, "Structure of %s: %s", sc->name, G.sx_state == SX_BUILDING ? "being read in the background..." : G.sx_state == SX_ERROR ? G.sx_err : "read on the first request (once, then cached).");
                    WRAP(st, T_TEXT_MUTED, px(a, 34));
                    float lw = px(a, 120); bool changed = false;
                    ui_label(a, rect_make(x, y + px(a, 3), lw, px(a, 16)), "Schemas", ALIGN_LEFT, T_TEXT_MUTED);
                    if (ui_textfield(a, &G.f_sch, rect_make(x + lw, y, w - lw, px(a, 24)), "all (comma-separated to limit)", focus_id == 21)) focus_id = 21;
                    ui_record("ai:scope:schemas", rect_make(x + lw, y, w - lw, px(a, 24))); y += px(a, 28);
                    ui_label(a, rect_make(x, y + px(a, 3), lw, px(a, 16)), "Always send", ALIGN_LEFT, T_TEXT_MUTED);
                    if (ui_textfield(a, &G.f_pin, rect_make(x + lw, y, w - lw, px(a, 24)), "tables, comma-separated", focus_id == 22)) focus_id = 22;
                    y += px(a, 28);
                    ui_label(a, rect_make(x, y + px(a, 3), lw, px(a, 16)), "Never send", ALIGN_LEFT, T_TEXT_MUTED);
                    if (ui_textfield(a, &G.f_blk, rect_make(x + lw, y, w - lw, px(a, 24)), "tables, comma-separated", focus_id == 23)) focus_id = 23;
                    y += px(a, 28);
                    if (BTN(rect_make(x, y, px(a, 110), px(a, 24)), "Save scope", BTN_NORMAL, true)) {
                        bool sch_changed = strcmp(G.scope_schemas, G.f_sch.buf) != 0;
                        snprintf(G.scope_schemas, sizeof G.scope_schemas, "%s", G.f_sch.buf); snprintf(G.scope_pin, sizeof G.scope_pin, "%s", G.f_pin.buf); snprintf(G.scope_block, sizeof G.scope_block, "%s", G.f_blk.buf);
                        scope_save(a, sc); G.est_q[0] = 0; changed = true;
                        if (sch_changed) sx_ensure(a, sc, true);   /* other schemas: read again */
                        audit(a, "context", d, "structure-scope-saved", 0, sc->driver);
                    }
                    if (BTN(rect_make(x + px(a, 116), y, px(a, 150), px(a, 24)), "Read structure again", BTN_QUIET, G.sx_state != SX_BUILDING)) { sx_ensure(a, sc, true); G.est_q[0] = 0; }
                    (void)changed;
                    y += px(a, 30);
                }
            }
            if (CHK(rect_make(x, y, w, px(a, 20)), "Show the exact payload", G.show_payload)) { G.show_payload = !G.show_payload; if (G.show_payload) build_payload(a, d); }
            y += px(a, 24);
            if (G.show_payload && G.payload) {
                Rect pr = rect_make(x, y, w, px(a, 160));
                draw_rect(r, pr, C(T_EDITOR_BG));
                if (rect_contains(pr, a->in.mouse_x, a->in.mouse_y) && wheel0) { G.pscroll -= wheel0 * px(a, 30); if (G.pscroll < 0) G.pscroll = 0; }
                clip_push(r, pr); results_draw_wrapped(a->font_small, r, G.payload, rect_inset(pr, px(a, 6), px(a, 4)), C(T_TEXT_MUTED), G.pscroll, true); clip_pop(r);
                y += px(a, 166);
            }
        }
        total = y - y0 + px(a, 8);
    }
    clip_pop(r);
    if (!a->in.mouse_down) G.sel_drag = false;
    if (G.sel_drag) { if (a->in.mouse_y < mid.y) { G.chat_scroll -= px(a, 12); G.chat_stick = false; app_wake_in(a, 30); } else if (a->in.mouse_y > mid.y + mid.h) { G.chat_scroll += px(a, 12); app_wake_in(a, 30); } }   /* drag beyond the edges scrolls */
    if (focus_id != 7 && rect_contains(rc, a->in.mouse_x, a->in.mouse_y)) {
        if (input_key(&a->in, SDLK_A, KM_PRIMARY) && (G.nchat || G.amsg[0])) { G.sel_am = 0; G.sel_ao = 0; G.sel_bm = G.nchat; G.sel_bo = (int)strlen(G.amsg); if (!G.amsg[0] && G.nchat) { G.sel_bm = G.nchat - 1; G.sel_bo = (int)strlen(G.chat[G.nchat - 1].text); } G.sel_on = true; }
    }
    /* Cmd+C copies the conversation's selection, also while the question box has the focus (the box
     * keeps its own copy when text is selected in it) */
    if (G.sel_on && !G.ced_focus && (focus_id != 7 || G.q.cursor == G.q.anchor) && (focus_id == 7 || rect_contains(rc, a->in.mouse_x, a->in.mouse_y)) && input_key(&a->in, SDLK_C, KM_PRIMARY)) { char *t = sel_text(); if (t) { SDL_SetClipboardText(t); app_status(a, "Copied"); free(t); } }
    if (a->in.mouse_pressed && !a->in.mouse_consumed && rect_contains(mid, a->in.mouse_x, a->in.mouse_y)) { G.sel_on = false; focus_id = 0; }   /* a click on empty space clears the selection */
    float maxs = total > mid.h ? total - mid.h : 0;
    G.chat_max = maxs;
    bool grew = total != G.chat_total; G.chat_total = total;
    float before = G.chat_scroll;
    if (G.chat_stick || G.chat_scroll > maxs) G.chat_scroll = maxs;   /* new content: show the newest */
    {   /* an approval taller than the view opens at its title, once; after that the user scrolls freely
         * (scrolling to the end stays at the end) */
        static bool snapped;
        if (G.ast != AS_PLAN) snapped = false;
        else if (!snapped) { snapped = true; if (G.chat_scroll > g_card_top) { G.chat_scroll = g_card_top; G.chat_stick = false; } }
    }
    if (G.chat_scroll < 0) G.chat_scroll = 0;
    if (G.chat_scroll != before || grew) app_wake_in(a, 16);   /* redraw at the corrected position at once */
    ui_record("ai:chat", mid);
    G.chat_rc = rc; G.chat_win = SDL_GetWindowID(SDL_GetRenderWindow(r));
    ui_scrollbar_draw(a, &G.chat_sb, mid, total, G.chat_scroll);
    /* composer: context line, the question, Send / Cancel */
    y = mid.y + mid.h + px(a, 6);
    {
        int non = 0; for (int i = 0; i < CX_N; i++) non += G.cx.on[i];
        char cl[200]; snprintf(cl, sizeof cl, "%s  Context: %d item(s) \xC2\xB7 data values %s%s", G.ctx_open ? "\xE2\x96\xBE" : "\xE2\x96\xB8", non, G.cx.on[CX_VALUES] ? "ON (your risk)" : "off", G.cx.on[CX_SCHEMA] ? " \xC2\xB7 schema" : "");
        if (BTN(rect_make(x, y, w - px(a, 110), px(a, 22)), cl, BTN_QUIET, true)) G.ctx_open = !G.ctx_open;
        if (BTN(rect_make(x + w - px(a, 104), y, px(a, 104), px(a, 22)), "New chat", BTN_QUIET, !busy && (G.nchat || G.amsg[0]))) { chat_clear(); G.chat_doc = d; G.ast = AS_IDLE; G.amsg[0] = 0; ai_reset_plan(); }
        y += px(a, 22) + px(a, 4);
    }
    if (G.attach[0]) {   /* the attached image, until the plan uses it or the user removes it */
        char al[160]; snprintf(al, sizeof al, "Image attached: %s", G.attach_name);
        Rect ar = rect_make(x, y, w - px(a, 90), px(a, 22));
        draw_rect(r, ar, C(T_PANEL_ALT)); ui_label(a, rect_inset(ar, px(a, 8), 0), al, ALIGN_LEFT, T_TEXT);
        ui_record("ai:attachment", ar);
        if (BTN(rect_make(x + w - px(a, 84), y, px(a, 84), px(a, 22)), "Remove", BTN_QUIET, true)) { G.attach[0] = 0; G.attach_name[0] = 0; }
        y += px(a, 26);
    }
    Rect qr = rect_make(x, y, w, rh);
    if (focus_id == 7 && G.nqhist) {   /* Up: the previous question, again for the one before; Down: forward, then back to the draft */
        if (G.qpos < -1 || G.qpos >= G.nqhist) G.qpos = -1;
        if (input_key(&a->in, SDLK_UP, 0)) {
            if (G.qpos == -1) { snprintf(G.qdraft, sizeof G.qdraft, "%s", G.q.buf); G.qpos = G.nqhist - 1; }
            else if (G.qpos > 0) G.qpos--;
            textfield_set(&G.q, G.qhist[G.qpos]);
        } else if (input_key(&a->in, SDLK_DOWN, 0) && G.qpos >= 0) {
            if (G.qpos < G.nqhist - 1) { G.qpos++; textfield_set(&G.q, G.qhist[G.qpos]); }
            else { G.qpos = -1; textfield_set(&G.q, G.qdraft); }
        }
    }
    if (IN(qr) && ui_textfield(a, &G.q, qr, G.qmode ? "Ask about this query..." : d && d->sel[0] ? "Ask about the selected card..." : "Ask about this page...", focus_id == 7)) focus_id = 7;
    ui_record("ai:report:question", qr);
    if (focus_id == 7 && input_key(&a->in, SDLK_RETURN, 0) && !busy) { G.choice[0] = 0; ai_send(a, d); }
    y += rh + px(a, 6);
    if (G.ast == AS_PLAN) {   /* a pending plan: the decision is always in view */
        if (G.req_qmode) {
            if (BTN(rect_make(x, y, px(a, 170), rh), G.qkind == AA_VALIDATE ? "Run both read-only" : G.qkind == AA_DESIGN_TABLE ? "Open in Table Designer" : G.qkind == AA_NEW_TAB ? "Open in new tab" : "Apply to editor", BTN_PRIMARY, true)) sql_apply(a);
            if (BTN(rect_make(x + px(a, 176), y, px(a, 90), rh), G.qkind == AA_VALIDATE ? "Skip" : "Reject", BTN_NORMAL, true)) sql_reject(a);
        } else {
        if (BTN(rect_make(x, y, px(a, 140), rh), "Approve and run", BTN_PRIMARY, true)) ai_approve(a);
        if (BTN(rect_make(x + px(a, 146), y, px(a, 90), rh), "Refuse", BTN_NORMAL, true)) ai_refuse(a);
        }
    } else {
        if (BTN(rect_make(x, y, px(a, 80), rh), "Send", BTN_PRIMARY, !busy && G.q.buf[0] && ready)) { G.choice[0] = 0; ai_send(a, d); }
        if (BTN(rect_make(x + px(a, 86), y, px(a, 80), rh), "Cancel", BTN_NORMAL, busy)) ai_cancel_all(a);
        if (G.qmode) {   /* quick requests about the tab */
            Connection *qc = conn_of(a, qti.conn_id); char e1[300], s1[8], c1[8];
            bool failed = tab_last_error(a, qc, e1, sizeof e1, s1, sizeof s1, c1, sizeof c1);
            float bx = x + px(a, 172);
            if (failed && BTN(rect_make(bx, y, px(a, 110), rh), "Fix the error", BTN_NORMAL, !busy && ready)) sql_quick(a, "Corrija o erro desta query.");
            if (failed) bx += px(a, 116);
            if (bx + px(a, 80) <= x + w && BTN(rect_make(bx, y, px(a, 80), rh), "Explain", BTN_QUIET, !busy && ready)) sql_quick(a, "Explique o que esta query faz e os riscos dela.");
            bx += px(a, 86);
            if (panels_active_obj(a->panels, a) && bx + px(a, 96) <= x + w && BTN(rect_make(bx, y, px(a, 96), rh), "Document", BTN_QUIET, !busy && ready)) commands_invoke(a, "intelligence.document");
        } else {
            if (w > px(a, 300) && BTN(rect_make(x + w - px(a, 130), y, px(a, 130), rh), "Attach image...", BTN_QUIET, !busy)) ask_path(a, d, EX_ATTACH, NULL);
            if (d && w > px(a, 470)) {   /* phase 3: the page with placeholder data, judged by the model; its size said up front */
                char rl[64], et[16]; tok_fmt(design_review_estimate(a, d), et, sizeof et); snprintf(rl, sizeof rl, "Review design (~%s)", et);
                Rect rr = rect_make(x + px(a, 172), y, w - px(a, 172) - px(a, 136), rh);
                ui_record("ai:review-design", rr);
                if (BTN(rr, rl, BTN_QUIET, !busy && ready)) design_review(a, d);
            }
        }
    }
    #undef WRAP
    content_h = 0;
    clip_pop(r);
    #undef IN
    #undef BTN
    #undef CHK
}
static void *as_create(void *ud, void *ctx) { (void)ud; (void)ctx; return &G; }
static void as_destroy(void *view, void *ctx) { (void)view; (void)ctx; }
static void as_vis(void *view, void *ctx, bool shown) { (void)view; (void)ctx; (void)shown; }
static void as_module_init(Panels *P, App *a) {
    DockPanelDesc dsc = { .id = "lume.data.assistant", .title = "Lume Intelligence", .icon = "ai", .region = DOCK_RIGHT,
        .min_w = 300, .min_h = 200, .pref_size = 380, .priority = 3, .start_hidden = true,
        .draw = as_draw, .create = as_create, .destroy = as_destroy, .visibility = as_vis };
    panels_register(P, a, &dsc);
}

/* ---- commands ---- */
static void new_from_result(App *a) {
    Db *db = NULL; const char *sql = a->results ? results_current_sql(a->results, &db) : NULL;
    if (!sql || !db) { app_status(a, "Add to Report: run a SELECT first (the current result has no SQL or its session closed)"); return; }
    Connection *c = NULL; for (int i = 0; a->conns && i < a->conns->n; i++) if (a->conns->items[i]->db == db) c = a->conns->items[i];
    if (!c) { app_status(a, "Add to Report: the result's connection is not a saved connection"); return; }
    char why[300];
    if (!sqlguard_check(sql, dialect_for_driver(c->driver), why, sizeof why)) { app_status(a, "Add to Report refused: %s (reports run read-only queries)", why); return; }
    RvDoc *d = active_doc(a); if (!d) d = any_doc(a); if (!d) d = create_report(a, "Report from a query");
    if (!d) return;
    before_change(d);
    RptVisual *v = rpt_add_visual(&d->r, d->page, RK_TABLE);
    if (v) { v->sql = strdup(sql); snprintf(v->conn, sizeof v->conn, "%s", c->id); snprintf(v->title, sizeof v->title, "%s", L(d, "Resultado da consulta", "Query result")); v->grain = -1; snprintf(d->sel, sizeof d->sel, "%s", v->id); }
    after_change(d);
    panels_activate_tab(a->panels, a, d->tab_id);
    app_status(a, "Added the query as a table card (it keeps the SQL and re-runs it read-only)");
}
static char recent_paths[12][1024];
static void open_menu_reports(App *a) {
    char rd[1100]; reports_dir(a, rd, sizeof rd);
    popup_open(&G.menu, px(a, 200), px(a, 80)); G.menu_kind = MK_OPEN; G.menu_doc = NULL;
    popup_add(&G.menu, 99, "Open file...", false, true);
    /* recent reports saved or opened elsewhere (that still exist) */
    char rp[1200]; snprintf(rp, sizeof rp, "%s/recent.txt", rd);
    FILE *rf = fopen(rp, "r"); int nr = 0;
    static char rlab[12][96];
    if (rf) {
        char line[1100];
        while (fgets(line, sizeof line, rf) && nr < 12) {
            line[strcspn(line, "\r\n")] = 0; struct stat st;
            if (!line[0] || stat(line, &st) != 0) continue;
            snprintf(recent_paths[nr], sizeof recent_paths[0], "%s", line);
            const char *base = strrchr(line, '/'); base = base ? base + 1 : line;
            char dirn[1024]; snprintf(dirn, sizeof dirn, "%.*s", (int)(base - line > 1 ? base - line - 1 : 0), line);
            const char *dshort = strrchr(dirn, '/');
            snprintf(rlab[nr], sizeof rlab[0], "%.60s  (%.30s)", base, dshort ? dshort + 1 : dirn);
            popup_add(&G.menu, 300 + nr, rlab[nr], false, true); nr++;
        }
        fclose(rf);
    }
    DIR *dir = opendir(rd); if (!dir) return;
    struct dirent *e; int n = 0;
    static char files[POPUP_MAX][256];
    while ((e = readdir(dir)) && n < POPUP_MAX) {
        size_t L2 = strlen(e->d_name);
        if (L2 > 5 && !strcmp(e->d_name + L2 - 5, ".lrpt")) { snprintf(files[n], sizeof files[n], "%s", e->d_name); snprintf(G.menu_lab[n], sizeof G.menu_lab[n], "%s", e->d_name); popup_add(&G.menu, 100 + n, G.menu_lab[n], false, true); n++; }
    }
    closedir(dir);
    if (!n && !nr) { popup_add(&G.menu, -1, "No saved reports yet", false, false); }
}
static char open_files_dir[1100];
static void sql_quick(App *a, const char *q);
static bool active_query_tab(App *a, TabInfo *out);
static bool ai_cmd_enabled(const LumeCtx *c, char *why, size_t n) {
    (void)c;
    if (!G.cfg.provider[0] && G.a) { char dir[1100]; pref_dir(G.a, dir, sizeof dir); ai_config_load(dir, &G.cfg); }
    if (!G.cfg.provider[0] || !strcmp(G.cfg.provider, "none")) { snprintf(why, n, "Lume Intelligence is off"); return false; }
    if (G.ai) { snprintf(why, n, "The assistant is busy"); return false; }
    return true;
}
static void inv(void *ud, const LumeCtx *c) {
    (void)c; App *a = G.a; if (!a) return;
    int what = (int)(intptr_t)ud;
    if (what == 1) create_report(a, NULL);
    else if (what == 2) { RvDoc *d = create_report(a, "Report from a question"); if (d) { G.adoc = d; panels_show(a->panels, a, "lume.data.assistant"); focus_id = 7; } }
    else if (what == 3) open_menu_reports(a);
    else if (what == 4) new_from_result(a);
    else if (what == 5) panels_show(a->panels, a, "lume.data.assistant");
    else if (what == 9) {   /* the Table Designer's "Review with Lume Intelligence": its DDL is the active query tab now */
        if (!active_query_tab(a, NULL)) return;
        panels_show(a->panels, a, "lume.data.assistant");
        sql_quick(a, "Revise o modelo desta tabela: tipos, nulos, chaves, FKs sem índice, nomes e comentários. Explique os problemas e, se valer a pena, proponha a versão corrigida.");
    }
    else if (what == 10) {   /* document an object (roadmap item 18) */
        if (!active_query_tab(a, NULL)) return;
        panels_show(a->panels, a, "lume.data.assistant");
        sql_quick(a, "Documente este objeto: responda new_tab com os COMMENT ON (ou o equivalente do dialeto) do objeto e das colunas, em português, curtos e corretos pelo que o SQL mostra.");
    }
    else if (what == 7 || what == 8) {   /* from the Output's failed statement, or the palette: the assistant about the active query tab */
        if (!active_query_tab(a, NULL)) { app_status(a, "Lume Intelligence: open the query tab first"); return; }
        panels_show(a->panels, a, "lume.data.assistant");
        sql_quick(a, what == 7 ? "Corrija o erro desta query." : "Explique o que esta query faz e os riscos dela.");
    }
    else if (what == 6) { load_catalog(a); app_status(a, "Semantic catalog reloaded: %d metric(s)%s%s", G.cat.nm, G.cat.err[0] ? " - " : "", G.cat.err); }
}
void rptview_init(App *a) {
    G.a = a; G.timeout_s = 0; G.qpos = -1;   /* 0: Settings > Reports */
    ag_ctx_default(&G.cx);
    conn_add_close_hook(on_close_conn);
    panels_add_module("lume.reports", as_module_init);
    Command c; memset(&c, 0, sizeof c);
    c.group = "Reports"; c.icon = "report"; c.invoke = inv; c.module = "lume.reports";
    c.id = "report.new"; c.label = "New Report"; c.synonyms = "dashboard report studio"; c.ud = (void *)(intptr_t)1; commands_register(&c);
    c.id = "report.new_ai"; c.label = "New Report from a Question..."; c.synonyms = "ai intelligence ask data business"; c.ud = (void *)(intptr_t)2; commands_register(&c);
    c.id = "report.open"; c.label = "Open Report..."; c.synonyms = "dashboard"; c.ud = (void *)(intptr_t)3; commands_register(&c);
    c.id = "report.add_result"; c.label = "Add Result to Report"; c.synonyms = "chart visualize grid"; c.ud = (void *)(intptr_t)4; commands_register(&c);
    c.group = "Lume Intelligence"; c.icon = "ai";
    c.id = "intelligence.fix"; c.label = "Fix the Last Error with Lume Intelligence"; c.synonyms = "ai assistant fix error sql"; c.ud = (void *)(intptr_t)7; c.enabled = ai_cmd_enabled; commands_register(&c);
    c.id = "intelligence.review_table"; c.label = "Review Table with Lume Intelligence"; c.synonyms = "ai assistant review table model"; c.ud = (void *)(intptr_t)9; commands_register(&c);
    c.id = "intelligence.document"; c.label = "Document This Object with Lume Intelligence"; c.synonyms = "ai assistant comment document object"; c.ud = (void *)(intptr_t)10; commands_register(&c);
    c.id = "intelligence.explain"; c.label = "Explain This Query with Lume Intelligence"; c.synonyms = "ai assistant explain sql"; c.ud = (void *)(intptr_t)8; commands_register(&c);
    c.enabled = NULL; c.group = "Reports"; c.icon = "report";
    c.id = "report.assistant"; c.label = "Show Lume Intelligence"; c.synonyms = "ai assistant copilot"; c.ud = (void *)(intptr_t)5; commands_register(&c);
    c.id = "report.reload_catalog"; c.label = "Reload Semantic Catalog"; c.synonyms = "metrics semantic"; c.ud = (void *)(intptr_t)6; commands_register(&c);
    (void)open_files_dir;
}
void rptview_shutdown(App *a) {
    if (G.ai) { ai_free(G.ai); G.ai = NULL; }
    if (G.job) { exec_cancel(G.job); exec_free(G.job); G.job = NULL; sqlguard_free(G.stmts, G.nst); G.nst = 0; }
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i]) { autosave(a, G.docs[i]); free_doc(G.docs[i]); }
    conn_remove_close_hook(on_close_conn);
    ag_answer_free(&G.ans); free(G.payload); G.payload = NULL;
    font_destroy(G.f_title); font_destroy(G.f_kpi); G.f_title = G.f_kpi = NULL;
}
bool rptview_doc_dirty(const RvDoc *d) { return d && d->dirty; }
void rptview_tab_closed(App *a, RvDoc *d) {
    if (d->dirty) { autosave(a, d); app_status(a, "Report closed: unsaved changes kept in the autosave (Open Report recovers them)"); }
    free_doc(d);
}
void rptview_pre_update(App *a) {
    G.a = a;
    if (G.ced_focus && (input_key(&a->in, SDLK_RETURN, KM_PRIMARY) || input_key(&a->in, SDLK_KP_ENTER, KM_PRIMARY))) { G.ced_apply = true; app_wake_in(a, 16); }
    if (input_key(&a->in, SDLK_K, KM_PRIMARY) && active_query_tab(a, NULL)) {   /* Cmd+K: the assistant about this query tab */
        panels_show(a->panels, a, "lume.data.assistant"); focus_id = 7; app_wake_in(a, 16);
    }
    {   /* page zoom shortcuts while a report is the active tab (the editor's zoom elsewhere) */
        RvDoc *zd = active_doc(a);
        if (zd && !ui_text_editing()) {
            if (input_key(&a->in, SDLK_EQUALS, KM_PRIMARY) || input_key(&a->in, SDLK_PLUS, KM_PRIMARY | KM_SHIFT) || input_key(&a->in, SDLK_EQUALS, KM_PRIMARY | KM_SHIFT)) { zoom_step(zd, 1); app_wake_in(a, 16); }
            else if (input_key(&a->in, SDLK_MINUS, KM_PRIMARY)) { zoom_step(zd, -1); app_wake_in(a, 16); }
            else if (input_key(&a->in, SDLK_0, KM_PRIMARY)) { zoom_step(zd, 0); app_wake_in(a, 16); }
        }
    }
    pump_queries(a);
    sx_pump(a);
    v_pump(a);
    if (G.sx_wait && G.sx_state != SX_BUILDING) {   /* the index a send was waiting for */
        G.sx_wait = false;
        RvDoc *wd = G.adoc && doc_index(G.adoc) >= 0 ? G.adoc : NULL;
        if (G.sx_state == SX_READY && wd) { G.ast = AS_IDLE; G.amsg[0] = 0; G.resend = true; if (!G.q.buf[0]) textfield_set(&G.q, G.sent_q); ai_send(a, wd); }
        else if (G.sx_state == SX_ERROR) { G.ast = AS_ERROR; snprintf(G.amsg, sizeof G.amsg, "The structure could not be read: %s. Nothing was sent.", G.sx_err); }
    }
    if (G.ai && G.ast == AS_THINKING && !ai_done(G.ai)) live_text(a);
    poll_ai(a); if (G.ai) app_wake_in(a, 150);
    track_checking(a);
    poll_dialog(a);
    setup_poll(a); if (G.testj || G.signing || G.mstate == 1) app_wake_in(a, 200);   /* setup jobs finish with the panel hidden too */
    track_querying(a);
    /* AI data requests also run when the report tab is hidden */
    if (G.ast == AS_QUERYING && G.adoc && doc_index(G.adoc) >= 0) for (int i = 0; i < G.napplied; i++) { int pg; RptVisual *v = rpt_find(&G.adoc->r, G.applied[i], &pg); char why[300]; if (v) { visual_data(a, G.adoc, v, false, why, sizeof why); if (v->compare) visual_data(a, G.adoc, v, true, why, sizeof why); } }
    /* every card of the active page loads, on screen or not: export, provenance and the assistant need them
     * (the cache keeps it to one query per distinct SQL) */
    { RvDoc *ad = active_doc(a); if (ad) { RptPage *p = &ad->r.pages[ad->page]; for (int i = 0; i < p->nv; i++) { RptVisual *v = &p->v[i]; if (!has_data(v)) continue; char why[300]; visual_data(a, ad, v, false, why, sizeof why); if (v->kind == RK_KPI && v->compare) visual_data(a, ad, v, true, why, sizeof why); } } }
    for (int i = 0; i < MAX_DOCS; i++) if (G.docs[i] && G.docs[i]->dirty && now_s() - G.docs[i]->changed_at > 2 && G.docs[i]->autosaved_at < G.docs[i]->changed_at) autosave(a, G.docs[i]);
    if (G.menu.open) {
        int id = popup_update(a, &G.menu);
        if (id >= 0) {
            popup_close(&G.menu);
            if (G.menu_kind == MK_OPEN) {
                if (id == 99) ask_path(a, NULL, EX_OPEN, NULL);
                else if (id >= 300 && id < 312) open_report(a, recent_paths[id - 300], false);
                else { char rd[1100]; reports_dir(a, rd, sizeof rd); char p[1400]; snprintf(p, sizeof p, "%s/%s", rd, G.menu_lab[id - 100]); open_report(a, p, false); }
            }
            else if (G.menu_doc && doc_index(G.menu_doc) >= 0) menu_choose(a, G.menu_doc, G.menu_kind, id);
        }
    }
}
void rptview_draw_overlay(App *a) {
    if (G.menu.open) popup_draw(a, &G.menu);
    if (G.ced && G.ced_focus) { Editor *prev = a->editor; int pf = a->focus; a->editor = G.ced; a->focus = FOCUS_EDITOR; editor_draw_overlay(G.ced, a); a->editor = prev; a->focus = pf; }   /* the card editor's completion list */
}

/* ---- harness ---- */
static int kind_arg(const char *s) { return !strcmp(s, "kpi") ? RK_KPI : !strcmp(s, "line") ? RK_LINE : !strcmp(s, "bar") ? RK_BAR : !strcmp(s, "table") ? RK_TABLE : !strcmp(s, "text") ? RK_TEXT : -1; }
void rptview_cmd(App *a, const char *arg) {
    char op[32] = "", rest[1024] = "";
    sscanf(arg, "%31s %1023[^\n]", op, rest);
    RvDoc *d = any_doc(a);
    if (!strcmp(op, "new")) { create_report(a, rest[0] ? rest : NULL); return; }
    if (!strcmp(op, "open")) { char p[1024] = "", m[16] = ""; sscanf(rest, "%1023s %15s", p, m); open_report(a, p, !strcmp(m, "read")); return; }
    if (!strcmp(op, "catalog")) { load_catalog(a); return; }
    if (!strcmp(op, "fromresult")) { new_from_result(a); return; }
    if (!strcmp(op, "timeout")) { G.timeout_s = atoi(rest) > 0 ? atoi(rest) : 0; return; }   /* 0: back to Settings */
    if (!strcmp(op, "ai")) {
        char sub[32] = "", r2[900] = ""; sscanf(rest, "%31s %899[^\n]", sub, r2);
        if (!strcmp(sub, "ask")) { textfield_set(&G.q, r2); G.choice[0] = 0; ai_send(a, d); }
        else if (!strcmp(sub, "choose")) ai_choose(a, atoi(r2));
        else if (!strcmp(sub, "approve")) ai_approve(a);
        else if (!strcmp(sub, "refuse")) ai_refuse(a);
        else if (!strcmp(sub, "cancel")) ai_cancel_all(a);
        else if (!strcmp(sub, "global")) G.allow_global = !strcmp(r2, "on");
        else if (!strcmp(sub, "scope")) { if (!strcmp(r2, "allow") && G.ast == AS_SCOPE) { G.allow_global = true; G.ast = AS_IDLE; plan_from_answer(a, d); } else if (G.ast == AS_SCOPE) G.ast = AS_IDLE; }
        else if (!strcmp(sub, "enable")) set_ai_enabled(a, true);
        else if (!strcmp(sub, "setup")) setup_open(a);
        else if (!strcmp(sub, "provider")) set_provider(!strcmp(r2, "local") ? SP_LOCAL : !strcmp(r2, "openai") || !strcmp(r2, "chatgpt") ? SP_OPENAI : SP_ANTHROPIC, !strcmp(r2, "openai") ? 1 : 0);
        else if (!strcmp(sub, "signin")) signin_start(a);
        else if (!strcmp(sub, "signout")) { chatgpt_sign_out(); ai_key_changed(); }
        else if (!strcmp(sub, "models")) models_load(a);
        else if (!strcmp(sub, "pickmodel")) { int i = atoi(r2); if (i >= 0 && i < G.nmodels) textfield_set(&G.s_model, G.models[i]); }
        else if (!strcmp(sub, "gotit")) G.welcome = false;
        else if (!strcmp(sub, "steps")) {   /* harness: the plan, with each card's SQL on one line */
            for (int i = 0; i < G.nsteps; i++) {
                char one[1300]; snprintf(one, sizeof one, "%s", G.steps[i].sql); for (char *p = one; *p; p++) if (*p == '\n' || *p == '\r') *p = ' ';
                printf("rptstep %s [%s] %s%s%s\n", G.steps[i].tool, ST_NAME[G.steps[i].st], G.steps[i].detail, one[0] ? " || SQL: " : "", one);
            }
        }
        else if (!strcmp(sub, "recall")) {   /* recall up|down: as the arrow keys in the focused question field */
            if (!strcmp(r2, "up")) { if (G.qpos == -1 && G.nqhist) { snprintf(G.qdraft, sizeof G.qdraft, "%s", G.q.buf); G.qpos = G.nqhist - 1; } else if (G.qpos > 0) G.qpos--; if (G.qpos >= 0) textfield_set(&G.q, G.qhist[G.qpos]); }
            else if (G.qpos >= 0) { if (G.qpos < G.nqhist - 1) { G.qpos++; textfield_set(&G.q, G.qhist[G.qpos]); } else { G.qpos = -1; textfield_set(&G.q, G.qdraft); } }
            printf("rptq \"%s\"\n", G.q.buf);
        }
        else if (!strcmp(sub, "type")) textfield_set(&G.q, r2);
        else if (!strcmp(sub, "attach")) { char why[200]; if (!chat_attach(r2, why, sizeof why)) printf("rptattach error=\"%s\"\n", why); }
        else if (!strcmp(sub, "attachment")) printf("rptattach name=\"%s\"\n", G.attach_name);
        else if (!strcmp(sub, "sxscope")) {   /* sxscope schemas|pin|block <list>: as typed in the context panel, then saved */
            Connection *sc = ai_exec_of(a, schema_connection(a));
            if (sc) { scope_load(a, sc); char k[16] = "", v[256] = ""; sscanf(r2, "%15s %255[^\n]", k, v);
                if (!strcmp(k, "schemas")) snprintf(G.scope_schemas, sizeof G.scope_schemas, "%s", v); else if (!strcmp(k, "pin")) snprintf(G.scope_pin, sizeof G.scope_pin, "%s", v); else if (!strcmp(k, "block")) snprintf(G.scope_block, sizeof G.scope_block, "%s", v);
                scope_save(a, sc); if (!strcmp(k, "schemas")) sx_ensure(a, sc, true); }
        }
        else if (!strcmp(sub, "index")) {   /* index build <conn name> | index state | index pick <question> */
            if (!strncmp(r2, "build ", 6)) { Connection *c = NULL; for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, r2 + 6)) c = a->conns->items[i]; if (c) sx_ensure(a, c, true); else printf("rptindex error=\"no connection %s\"\n", r2 + 6); }
            else if (!strncmp(r2, "pick ", 5) && G.sx_state == SX_READY) {
                SxPick p[40]; SxQuery q = { r2 + 5, NULL, 0, 30 }; double t0 = now_s(); int n = schidx_pick(&G.sx, &q, p, 40);
                printf("rptindex pick n=%d ms=%.0f", n, (now_s() - t0) * 1000); for (int i = 0; i < n && i < 8; i++) printf(" %s.%s", G.sx.t[p[i].t].schema, G.sx.t[p[i].t].name); printf("\n");
            }
            else printf("rptindex state=%d tables=%d conn=%s err=\"%s\"\n", G.sx_state, G.sx.n, G.sx_conn, G.sx_err);
        }
        else if (!strcmp(sub, "tabtype")) { Editor *ed = panels_tab_editor(a->panels, G.qtab); if (ed) editor_insert(ed, r2, (int)strlen(r2)); }   /* the user types in that tab */
        else if (!strcmp(sub, "review")) { RvDoc *rd = active_doc(a); if (!rd) rd = d; if (rd) { printf("rptreview estimate=%d\n", design_review_estimate(a, rd)); design_review(a, rd); } }
        else if (!strcmp(sub, "compareplans")) { Connection *pc = conn_of(a, G.qconn); printf("rptcompare %s\n", pc && G.qedit_sql && planview_compare_sql(a, pc, G.qedit_sql) ? "started" : "no"); }
        else if (!strcmp(sub, "tabactivate")) { if (G.qtab > 0) panels_activate_tab(a->panels, a, G.qtab); }   /* back to the query tab */
        else if (!strcmp(sub, "tabundo")) { Editor *ed = panels_tab_editor(a->panels, G.qtab); if (ed) editor_undo(ed); }   /* Cmd+Z in that tab */
        else if (!strcmp(sub, "tabtext")) { TabInfo ti; if (active_query_tab(a, &ti)) { int n = 0; char *t = editor_text(ti.ed, &n); printf("rpttab title=\"%s\" conn=%s text=\"", ti.title, ti.conn_id); for (int i = 0; i < n; i++) putchar(t[i] == '\n' ? ' ' : t[i]); printf("\"\n"); free(t); } else printf("rpttab none\n"); }
        else if (!strcmp(sub, "apply")) sql_apply(a);
        else if (!strcmp(sub, "reject")) sql_reject(a);
        else if (!strcmp(sub, "quick")) sql_quick(a, r2);
        else if (!strcmp(sub, "sqlstate")) printf("rptsql qmode=%d kind=%d target=%s writes=%d shown=%d focus=%d sql=\"%.300s\"\n", active_query_tab(a, NULL), G.qkind, G.qtarget, G.qwrites, panels_is_visible(a->panels, "lume.data.assistant"), focus_id == 7, G.qedit_sql ? G.qedit_sql : "");
        else if (!strcmp(sub, "focus")) { focus_id = 7; G.ced_focus = false; }   /* the question box has the keyboard */
        else if (!strcmp(sub, "selectall")) { G.sel_am = 0; G.sel_ao = 0; G.sel_bm = G.nchat; G.sel_bo = (int)strlen(G.amsg); G.sel_on = G.nchat || G.amsg[0]; }
        else if (!strcmp(sub, "seltext")) { char *t = sel_text(); printf("rptsel %s\n", t ? t : ""); free(t); }
        else if (!strcmp(sub, "chat")) {   /* harness: the conversation so far */
            for (int i = 0; i < G.nchat; i++) printf("rptchat %s: %s\n", G.chat[i].role == 'u' ? "user" : G.chat[i].role == 't' ? "tokens" : "assistant", G.chat[i].text);
            printf("rptchat live [%s]: %s\n", AS_NAME[G.ast], G.amsg);
            printf("rptchat scroll=%.0f max=%.0f stick=%d\n", G.chat_scroll, G.chat_max, G.chat_stick);
        }
        else if (!strcmp(sub, "schema")) {
            if (!strcmp(r2, "allow")) schema_allow(a);
            else if (!strncmp(r2, "conn ", 5)) { for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, r2 + 5)) snprintf(G.schema_main, sizeof G.schema_main, "%s", a->conns->items[i]->id); }
            else if (!strncmp(r2, "aiconn ", 7)) {
                Connection *m = schema_connection(a);
                if (m) { m->ai_conn[0] = 0; for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, r2 + 7)) snprintf(m->ai_conn, sizeof m->ai_conn, "%s", a->conns->items[i]->id); conns_save(a->conns); G.gr_conn[0] = 0; }
            }
            else if (!strcmp(r2, "state")) { Connection *m = schema_connection(a), *x = ai_exec_of(a, m); if (x && x->db) grants_check(x); printf("rptschema main=%s exec=%s grants=%d note=\"%s\" required=%d shared=%d ro=%d\n", m ? m->name : "-", x ? x->name : "-", G.gr_state, G.gr_note, G.cfg.ro_required, G.cx.on[CX_SCHEMA], x ? x->read_only : 0); }
            else { G.cx.on[CX_SCHEMA] = false; consent_save(a, ai_exec_of(a, schema_connection(a))); }
        }
        else if (!strcmp(sub, "model")) textfield_set(&G.s_model, r2);
        else if (!strcmp(sub, "key")) { G.s_key.password = true; textfield_set(&G.s_key, r2); G.test_state = TS_NONE; }
        else if (!strcmp(sub, "removekey")) { secstore_delete(AI_KEY_SERVICE, key_account()); ai_key_changed(); }
        else if (!strcmp(sub, "endpoint")) textfield_set(&G.s_endpoint, r2);
        else if (!strcmp(sub, "test")) setup_test(a);
        else if (!strcmp(sub, "save")) setup_save(a);
        else if (!strcmp(sub, "setupstate")) { char em[160] = ""; bool si = chatgpt_signed_in(em, sizeof em); printf("rptsetup open=%d provider=%s test=%d msg=\"%s\" cfg=%s/%s keysource=%d keyfield=%d signedin=%d email=%s signing=%d models=%d first=%s welcome=%d\n", G.setup, setup_provider(), G.test_state, G.test_msg, G.cfg.provider, G.cfg.model, key_source(), G.s_key.buf[0] != 0, si, em, G.signing, G.nmodels, G.nmodels ? G.models[0] : "-", G.welcome); }
        else if (!strcmp(sub, "disable")) set_ai_enabled(a, false);
        else if (!strcmp(sub, "ctx")) { int i = atoi(r2); char onoff[8] = ""; sscanf(r2, "%*d %7s", onoff); if (i >= 0 && i < CX_N) { G.cx.on[i] = !strcmp(onoff, "on"); audit(a, "context", d, G.cx.on[i] ? "item-on" : "item-off", 0, ""); if (i == CX_SCHEMA || i == CX_CATVALUES) consent_save(a, ai_exec_of(a, schema_connection(a))); } }
        else if (!strcmp(sub, "payload")) { build_payload(a, d); printf("rptpayload\n%s\nrptpayload-end\n", G.payload ? G.payload : ""); }
        else if (!strcmp(sub, "undo") && d) { if (rpt_undo(&d->h, &d->r)) { mark_changed(d); G.ast = AS_IDLE; } }
        return;
    }
    if (!strcmp(op, "openpath")) { RvDoc *nd = open_report(a, rest, false); printf("rptopen ok=%d user=%d\n", nd != NULL, nd ? nd->user_path : 0); return; }
    if (!d) { printf("script: rpt %s: no report open\n", op); return; }
    int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg);
    if (!strcmp(op, "add")) {
        char k[16] = "", m[48] = "", x[64] = "", body[900] = ""; sscanf(rest, "%15s %47s %63s", k, m, x); sscanf(rest, "%*s %899[^\n]", body);
        int kind = kind_arg(k); if (kind < 0) { printf("script: rpt add: unknown kind\n"); return; }
        if (kind == RK_TEXT) { m[0] = x[0] = 0; }
        before_change(d);
        RptVisual *nv = rpt_add_visual(&d->r, d->page, kind);
        if (nv) {
            if (m[0]) { const SemMetric *sm = sem_metric_c(&G.cat, m); snprintf(nv->metric, sizeof nv->metric, "%s", m); nv->metric_version = sm ? sem_cur(sm)->version : 0; }
            if (x[0]) { int g = sem_grain_by_name(x); if (g >= 0) nv->grain = g; else { snprintf(nv->dim, sizeof nv->dim, "%s", x); nv->grain = -1; } }
            if (kind == RK_TEXT && body[0]) { for (char *p = body; *p; p++) if (*p == '|') *p = '\n'; rpt_visual_set_text(nv, body); }
            snprintf(d->sel, sizeof d->sel, "%s", nv->id);
        }
        after_change(d);
    } else if (!strcmp(op, "addsql")) {
        char cid[16] = "", sql[1000] = ""; sscanf(rest, "%15s %999[^\n]", cid, sql);
        before_change(d);
        RptVisual *nv = rpt_add_visual(&d->r, d->page, RK_TABLE);
        if (nv) { nv->sql = strdup(sql); snprintf(nv->conn, sizeof nv->conn, "%s", cid); nv->grain = -1; snprintf(nv->title, sizeof nv->title, "SQL"); snprintf(d->sel, sizeof d->sel, "%s", nv->id); }
        after_change(d);
    } else if (!strcmp(op, "select")) snprintf(d->sel, sizeof d->sel, "%s", rest);
    else if (!strcmp(op, "set") && v) {
        char k[32] = "", val[900] = ""; sscanf(rest, "%31s %899[^\n]", k, val);
        before_change(d);
        if (!strcmp(k, "kind")) set_visual_kind(d, v, kind_arg(val));
        else if (!strcmp(k, "metric")) { const SemMetric *sm = sem_metric_c(&G.cat, val); snprintf(v->metric, sizeof v->metric, "%s", val); v->metric_version = sm ? sem_cur(sm)->version : 0; }
        else if (!strcmp(k, "version")) v->metric_version = atoi(val);
        else if (!strcmp(k, "dim")) { snprintf(v->dim, sizeof v->dim, "%s", strcmp(val, "-") ? val : ""); if (v->dim[0] && v->kind == RK_BAR) v->grain = -1; }
        else if (!strcmp(k, "grain")) v->grain = sem_grain_by_name(val);
        else if (!strcmp(k, "compare")) v->compare = rpt_compare_by_name(val) < 0 ? 0 : rpt_compare_by_name(val);
        else if (!strcmp(k, "title")) snprintf(v->title, sizeof v->title, "%s", val);
        else if (!strcmp(k, "fmt")) v->fmt = rpt_fmt_by_name(val) < 0 ? 0 : rpt_fmt_by_name(val);
        else if (!strcmp(k, "lock")) v->locked = !strcmp(val, "on");
        else if (!strcmp(k, "text")) { for (char *p = val; *p; p++) if (*p == '|') *p = '\n'; rpt_visual_set_text(v, val); }
        else if (!strcmp(k, "approve")) v->ai_suggested = false;
        after_change(d);
    } else if (!strcmp(op, "move") && v) {
        int x, y, w, h; if (sscanf(rest, "%d %d %d %d", &x, &y, &w, &h) == 4) { before_change(d); if (rpt_place(&d->r, d->page, v->id, x, y, w, h)) after_change(d); else { free(d->h.u[--d->h.nu]); set_msg(d, "move refused (locked card in the way or this card is locked)"); } }
    } else if (!strcmp(op, "delete") && v) { before_change(d); rpt_remove_visual(&d->r, v->id); d->sel[0] = 0; after_change(d); }
    else if (!strcmp(op, "undo")) { if (rpt_undo(&d->h, &d->r)) mark_changed(d); }
    else if (!strcmp(op, "redo")) { if (rpt_redo(&d->h, &d->r)) mark_changed(d); }
    else if (!strcmp(op, "period")) { before_change(d); snprintf(d->r.period, sizeof d->r.period, "%s", rest); after_change(d); }
    else if (!strcmp(op, "asof")) { before_change(d); snprintf(d->r.as_of, sizeof d->r.as_of, "%s", rest); after_change(d); }
    else if (!strcmp(op, "locale")) { before_change(d); snprintf(d->r.locale, sizeof d->r.locale, "%s", rest); after_change(d); }
    else if (!strcmp(op, "title")) { before_change(d); snprintf(d->r.title, sizeof d->r.title, "%s", rest); after_change(d); panels_set_tab_title(a->panels, d->tab_id, d->r.title); }
    else if (!strcmp(op, "page")) { char s2[16] = "", nm[64] = ""; sscanf(rest, "%15s %63[^\n]", s2, nm); if (!strcmp(s2, "add")) { before_change(d); if (rpt_add_page(&d->r, nm)) d->page = d->r.np - 1; after_change(d); } else { int i = atoi(s2); if (i >= 0 && i < d->r.np) { d->page = i; d->sel[0] = 0; } } }
    else if (!strcmp(op, "filter")) {
        char f[64] = "", val[128] = ""; sscanf(rest, "%63s %127[^\n]", f, val);
        before_change(d);
        int k = -1; for (int i = 0; i < d->r.nf; i++) if (!strcmp(d->r.filters[i].page, d->r.pages[d->page].id)) k = i;
        if (!strcmp(f, "clear")) { if (k >= 0) d->r.filters[k] = d->r.filters[--d->r.nf]; }
        else { if (k < 0 && d->r.nf < RPT_MAX_FILTERS) { k = d->r.nf++; memset(&d->r.filters[k], 0, sizeof d->r.filters[k]); snprintf(d->r.filters[k].id, sizeof d->r.filters[k].id, "f%d", k + 1); snprintf(d->r.filters[k].page, sizeof d->r.filters[k].page, "%s", d->r.pages[d->page].id); } if (k >= 0) { snprintf(d->r.filters[k].field, sizeof d->r.filters[k].field, "%s", f); snprintf(d->r.filters[k].value, sizeof d->r.filters[k].value, "%s", val); } }
        after_change(d);
    }
    else if (!strcmp(op, "preview")) d->preview = !strcmp(rest, "on");
    else if (!strcmp(op, "refresh")) invalidate_data(d);
    else if (!strcmp(op, "save")) save_doc(a, d, rest[0] ? rest : NULL);
    else if (!strcmp(op, "prov")) snprintf(d->prov, sizeof d->prov, "%s", rest);
    else if (!strcmp(op, "saveas")) { if (d) ask_path(a, d, EX_SAVE_AS, NULL); }
    else if (!strcmp(op, "theme")) {   /* theme ref <r> | open <path> | saveas <path> | ws | detach | logo <path> | accent <hex> | header on|off | card <0..3> | kpi <0..2> | state */
        char sub[16] = "", arg[1024] = ""; sscanf(rest, "%15s %1023[^\n]", sub, arg);
        if (rtheme_is_builtin(sub)) { char ref[40]; snprintf(ref, sizeof ref, "builtin:%s", sub); theme_set_ref(d, ref); }   /* rpt theme dark */
        else if (!strcmp(sub, "ref")) theme_set_ref(d, arg);
        else if (!strcmp(sub, "open")) do_path_action(a, d, EX_THEME_OPEN, NULL, arg);
        else if (!strcmp(sub, "saveas")) do_path_action(a, d, EX_THEME_SAVE, NULL, arg);
        else if (!strcmp(sub, "ws")) { char wp[1200], why[300]; if (ws_theme_path(wp, sizeof wp) && theme_write_file(d, wp, why, sizeof why)) { before_change(d); free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_set_ref(d, "workspace"); } else printf("rpttheme error=\"%s\"\n", why); }
        else if (!strcmp(sub, "detach")) { theme_resolve(d); char *js = rtheme_to_json(&d->rt); before_change(d); free(d->r.theme_snapshot); d->r.theme_snapshot = js; free(d->r.theme_overrides); d->r.theme_overrides = NULL; after_change(d); theme_set_ref(d, "snapshot"); }
        else if (!strcmp(sub, "logo")) do_path_action(a, d, EX_LOGO, NULL, arg);
        else if (!strcmp(sub, "accent")) { unsigned c = 0; if (sscanf(arg[0] == '#' ? arg + 1 : arg, "%x", &c) == 1) { uint32_t cc = c; theme_change(d, tc_accent, &cc); } }   /* RRGGBB (scripts treat # as a comment) */
        else if (!strcmp(sub, "header")) { bool on = !strcmp(arg, "on"); theme_change(d, tc_header, &on); }
        else if (!strcmp(sub, "card")) { int k = atoi(arg); theme_change(d, tc_card, &k); }
        else if (!strcmp(sub, "kpi")) { int k = atoi(arg); theme_change(d, tc_kpi, &k); }
        else if (!strcmp(sub, "state")) {
            theme_dirty(d); theme_resolve(d);
            char lp[1400]; theme_logo_path(d, lp, sizeof lp);
            printf("rpttheme ref=%s name=%s locked=%d accent=#%06X header=%d logo=%s card=%s kpi=%s overrides=%d warn=\"%s\" msg=\"%s\"\n", d->r.theme_ref[0] ? d->r.theme_ref : "-", d->rt.name, d->rt.locked, d->rt.accent, d->rt.header, lp[0] ? "yes" : "no", rtheme_card_style_name(d->rt.card_style), rtheme_kpi_style_name(d->rt.kpi_style), d->r.theme_overrides != NULL, d->rt_warn, d->msg);
        }
    }
    else if (!strcmp(op, "image")) do_path_action(a, d, EX_IMAGE, NULL, rest);
    else if (!strcmp(op, "param")) {   /* param add <name> <type> <conn> <label|optionsSql> | set <name> <value> | all <name> | state */
        char sub[16] = "", nm[40] = "", rest2[1200] = ""; sscanf(rest, "%15s %39s %1199[^\n]", sub, nm, rest2);
        if (!strcmp(sub, "add") && d->r.nparams < RPT_MAX_PARAMS && rpt_param_name_ok(nm)) {
            char ty[16] = "", cn[16] = "", tail[1100] = ""; sscanf(rest2, "%15s %15s %1099[^\n]", ty, cn, tail);
            char *bar = strchr(tail, '|');
            before_change(d);
            RptParam *pm = &d->r.params[d->r.nparams++]; memset(pm, 0, sizeof *pm);
            snprintf(pm->name, sizeof pm->name, "%s", nm); pm->type = !strcmp(ty, "text") ? RPT_P_TEXT : RPT_P_SELECT; snprintf(pm->conn, sizeof pm->conn, "%s", cn);
            if (bar) { *bar = 0; snprintf(pm->label, sizeof pm->label, "%s", tail); snprintf(pm->options_sql, sizeof pm->options_sql, "%s", bar + 1); } else snprintf(pm->label, sizeof pm->label, "%s", tail);
            after_change(d);
        } else if (!strcmp(sub, "set") || !strcmp(sub, "all")) {
            for (int i = 0; i < d->r.nparams; i++) if (!strcmp(d->r.params[i].name, nm)) { before_change(d); snprintf(d->r.params[i].value, sizeof d->r.params[i].value, "%s", !strcmp(sub, "set") ? rest2 : ""); after_change(d); }
        } else if (!strcmp(sub, "options")) {
            for (int i = 0; i < d->r.nparams; i++) if (!strcmp(d->r.params[i].name, nm)) { G.menu_param = i; open_menu(a, d, MK_PARAM, 0, 0); }
        } else if (!strcmp(sub, "state")) {
            for (int i = 0; i < d->r.nparams; i++) { RptParam *pm = &d->r.params[i]; RvData *x = pm->options_sql[0] ? find_data(d, pm->conn, pm->options_sql) : NULL; printf("rptfilter name=%s label=\"%s\" value=\"%s\" options=%d\n", pm->name, pm->label, pm->value, x && x->status == RD_OK ? x->rs.nrows : -1); }
        }
    }
    else if (!strcmp(op, "cardsql")) {   /* cardsql set <sql> | apply | state: the selected card's query editor */
        char sub[16] = "", arg[2000] = ""; sscanf(rest, "%15s %1999[^\n]", sub, arg);
        int pg; RptVisual *v = rpt_find(&d->r, d->sel, &pg);
        if (!v) printf("rptcardsql none\n");
        else if (!strcmp(sub, "set")) { if (G.ced) editor_set_text(G.ced, arg); }
        else if (!strcmp(sub, "apply")) card_sql_apply(a, d, v);
        else if (!strcmp(sub, "state")) {
            char why[300] = ""; RvData *x = has_data(v) ? visual_data(a, d, v, false, why, sizeof why) : NULL;
            int len = 0, nl = 1; char *t = G.ced ? editor_text(G.ced, &len) : NULL;
            if (t) { char *o = t; for (char *p = t; *p; p++) { if (*p == '\n') nl++; bool sp = *p == '\n' || *p == ' ' || *p == '\t'; if (sp && (o == t || o[-1] == ' ')) continue; *o++ = sp ? ' ' : *p; } *o = 0; }   /* whitespace collapsed; lines= says how it is laid out */
            printf("rptcardlines %d\n", nl);
            printf("rptcardsql id=%s ro=%d edited=%d status=%s rows=%d err=\"%s\" editor=\"%.200s\" msg=\"%s\"\n", v->id, G.ced_ro, v->sql_edited, x ? RD_NAME[x->status] : "-", x ? x->rs.nrows : 0, x ? (x->err[0] ? x->err : x->rs.error) : why, t ? t : "", d->msg);
            free(t);
        }
    }
    else if (!strcmp(op, "logocard")) { before_change(d); RptVisual *nv = rpt_add_visual(&d->r, d->page, RK_IMAGE); if (nv) snprintf(nv->src, sizeof nv->src, "theme:logo"); after_change(d); }
    else if (!strcmp(op, "zoom")) { if (!strcmp(rest, "in")) zoom_step(d, 1); else if (!strcmp(rest, "out")) zoom_step(d, -1); else zoom_step(d, 0); }
    else if (!strcmp(op, "uisave")) { if (d) ui_save(a, d); }
    else if (!strcmp(op, "exportas")) {   /* exportas pdf|png|pnghi|cardpng|csv|xlsx [card]: through the save-dialog path */
        char f[16] = "", vis[24] = ""; sscanf(rest, "%15s %23s", f, vis);
        int ac = !strcmp(f, "html") ? EX_HTML : !strcmp(f, "pdf") ? EX_PDF : !strcmp(f, "png") ? EX_PNG : !strcmp(f, "pnghi") ? EX_PNG_HI : !strcmp(f, "cardpng") ? EX_PNG_CARD : !strcmp(f, "csv") ? EX_CSV_CARD : EX_XLSX;
        if (d) ask_path(a, d, ac, vis[0] ? vis : NULL);
    }
    else if (!strcmp(op, "export")) {
        char p[1024] = "", scope[32] = "", res[8] = ""; sscanf(rest, "%1023s %31s %7s", p, scope, res);
        char why[300]; bool ok = export_png(a, d, p, strcmp(scope, "page") && scope[0] ? scope : NULL, !strcmp(res, "2x") ? 3200 : 1600, why, sizeof why);
        printf("rptexport ok=%d why=\"%s\"\n", ok, ok ? "" : why);
    }
    else printf("script: rpt %s: unknown op\n", op);
}
void rptview_dump(App *a) {
    RvDoc *d = any_doc(a);
    int nd = 0; for (int i = 0; i < MAX_DOCS; i++) nd += G.docs[i] != NULL;
    printf("rptstate docs=%d catalog=%d/%d ai=%s ast=%s amsg=\"%s\" steps=%d nopt=%d job=%d queries=%d exports=%d render_ms=%.2f zoom=%d\n", nd, G.cat.nds, G.cat.nm, G.cfg.provider[0] ? G.cfg.provider : "?", AS_NAME[G.ast], G.amsg, G.nsteps, G.nopt, G.job != NULL, d ? d->queries : 0, G.exports, G.render_ms, d ? (int)roundf(zoom_of(d) * 100) : 100);
    for (int i = 0; i < G.nsteps; i++) printf("rptstep %d %s %s \"%s\"\n", i, ST_NAME[G.steps[i].st], G.steps[i].tool, G.steps[i].detail);
    for (int i = 0; i < G.nopt; i++) printf("rptopt %d %s\n", i, G.opts[i]);
    if (!d) return;
    char pl[700]; provenance_line(a, d, pl, sizeof pl);
    printf("rptdoc id=%s title=\"%s\" rev=%d dirty=%d page=%d/%d sel=%s preview=%d read=%d theme=%s locale=%s period=%s asof=%s undo=%d redo=%d choices=%d msg=\"%s\"\n", d->r.id, d->r.title, d->r.revision, d->dirty, d->page, d->r.np, d->sel[0] ? d->sel : "-", d->preview, d->read_mode, d->r.theme, d->r.locale, d->r.period, d->r.as_of[0] ? d->r.as_of : "-", d->h.nu, d->h.nr, d->r.nchoices, d->msg);
    printf("rptprov %s\n", pl);
    RptPage *p = &d->r.pages[d->page];
    for (int i = 0; i < p->nv; i++) {
        RptVisual *v = &p->v[i];
        char why[300] = ""; RvData *x = !has_data(v) ? NULL : visual_data(a, d, v, false, why, sizeof why);
        char val[96] = "-";
        if (x && x->status == RD_OK && x->rs.nrows) { double n; bool ok = cell_num(&x->rs, 0, x->rs.ncols - 1, &n); fmt_v(d, v, n, !ok, val, sizeof val); }
        printf("rptvis %s kind=%s metric=%s v%d dim=%s grain=%s cmp=%s at=%d,%d,%d,%d locked=%d ai=%d data=%s rows=%d first=\"%s\" err=\"%s\"\n", v->id, v->kind_name, v->metric[0] ? v->metric : "-", v->metric_version, v->dim[0] ? v->dim : "-", v->grain >= 0 ? sem_grain_name(v->grain) : "-", rpt_compare_name(v->compare), v->x, v->y, v->w, v->h, v->locked, v->ai_suggested,
               x ? RD_NAME[x->status] : (!has_data(v) && v->kind != RK_UNKNOWN ? rpt_kind_name(v->kind) : "blocked"), x ? x->rs.nrows : 0, val, x ? x->err : why);
        if (v->style) printf("rptstyle %s %s\n", v->id, v->style);
        if (v->ai_sql) { char one[RPT_AI_SQL_MAX + 1]; snprintf(one, sizeof one, "%s", v->sql ? v->sql : ""); for (char *p = one; *p; p++) if (*p == '\n' || *p == '\r') *p = ' '; printf("rptaisql %s conn=%s title=\"%s\" sql=%s\n", v->id, v->conn, v->title, one); }
    }
}

bool rptview_open_path(App *a, const char *path) { return open_report(a, path, false) != NULL; }
/* a file dropped on the assistant's conversation: an image is attached instead of opened */
bool rptview_drop_file(App *a, const char *path, SDL_WindowID win, float x, float y) {
    if (!path || !G.chat_win || win != G.chat_win || !rect_contains(G.chat_rc, x, y)) return false;
    if (win == SDL_GetWindowID(a->window) && !panels_is_visible(a->panels, "lume.data.assistant")) return false;   /* the panel was closed since */
    char why[200];
    if (chat_attach(path, why, sizeof why)) app_status(a, "Image attached to the conversation");
    else app_status(a, "%s", why);
    app_wake_in(a, 16);
    return true;
}
bool rptview_idle(App *a) {
    (void)a;
    if (G.sx_state == SX_BUILDING) return false;
    if (G.vstate == 1) return false;   /* the assistant's own checks (plans, hypothetical index) still running */
    if (G.job || G.ai || G.testj || G.signing || G.mstate == 1 || G.schema_wait || G.sx_wait || G.sx_state == SX_BUILDING || G.ast == AS_THINKING || G.ast == AS_QUERYING || G.ast == AS_CHECKING) return false;
    for (int di = 0; di < MAX_DOCS; di++) if (G.docs[di]) for (int i = 0; i < MAX_DATA; i++) if (G.docs[di]->data[i].used && (G.docs[di]->data[i].status == RD_QUEUED || G.docs[di]->data[i].status == RD_RUNNING)) return false;
    return true;
}

/* ---- Settings > Lume Intelligence (roadmap item 25) ---- */
/* one key of ai.txt (never a key or a secret: those live in the OS secret store) */
static void ai_txt_set(App *a, const char *key, const char *val) {
    char dir[1100], p[1200], tmp[1300]; pref_dir(a, dir, sizeof dir); snprintf(p, sizeof p, "%s/ai.txt", dir); snprintf(tmp, sizeof tmp, "%s.tmp", p);
    char keep[8192] = ""; size_t w = 0, kl = strlen(key); FILE *f = fopen(p, "r");
    if (f) { char line[600]; while (fgets(line, sizeof line, f)) { if (!strncmp(line, key, kl) && line[kl] == '=') continue; size_t l = strlen(line); if (w + l < sizeof keep - 1) { memcpy(keep + w, line, l); w += l; keep[w] = 0; } } fclose(f); }
    f = fopen(tmp, "w"); if (!f) return;
    fputs(keep, f);
    if (val && val[0]) { char v[128]; snprintf(v, sizeof v, "%s", val); for (char *q = v; *q; q++) if (*q == '\n' || *q == '\r') *q = ' '; fprintf(f, "%s=%s\n", key, v); }
    fclose(f); rename(tmp, p);
    ai_config_load(dir, &G.cfg);
}
/* forget what was remembered for one connection (consents, scope) */
static void scope_forget(App *a, const char *conn_id) {
    char p[1200], tmp[1300]; scope_path(a, p, sizeof p); snprintf(tmp, sizeof tmp, "%s.part", p);
    FILE *in = fopen(p, "r"); if (!in) return;
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600); FILE *out = fd >= 0 ? fdopen(fd, "w") : NULL;
    if (!out) { fclose(in); if (fd >= 0) close(fd); return; }
    char line[700]; size_t L = strlen(conn_id);
    while (fgets(line, sizeof line, in)) { if (!strncmp(line, conn_id, L) && line[L] == '\t') continue; fputs(line, out); }
    fclose(in); fclose(out); rename(tmp, p);
    if (!strcmp(G.scope_conn, conn_id)) { G.scope_loaded = false; G.cx.on[CX_SCHEMA] = G.cx.on[CX_CATVALUES] = false; }
    if (!strcmp(G.sx_conn, conn_id)) sx_reset();
    char cp[1300]; Connection *c = conn_of(a, conn_id); if (c) { sx_cache_path(a, c, cp, sizeof cp); remove(cp); }   /* and its structure index */
    LOG_I("intelligence: what was remembered for a connection was forgotten");
}
void rptview_settings_intelligence(App *a, Rect body, float *py) {
    SDL_Renderer *r = a->renderer;
    float x = body.x, w = body.w, y = *py, rh = px(a, 26);
    char dir[1100]; pref_dir(a, dir, sizeof dir);
    if (!G.cfg.provider[0]) ai_config_load(dir, &G.cfg);
    #define SLINE(txt, col, maxh) do { float hh_ = results_draw_wrapped(a->font_small, r, txt, rect_make(x, y, w, maxh), C(col), 0, true); y += hh_ + px(a, 6); } while (0)
    #define SHEAD(txt) do { y += px(a, 6); ui_label(a, rect_make(x, y, w, px(a, 20)), txt, ALIGN_LEFT, T_TEXT); y += px(a, 24); } while (0)
    bool off = !G.cfg.provider[0] || !strcmp(G.cfg.provider, "none");
    SHEAD("Provider and model");
    char pl[300];
    if (off) snprintf(pl, sizeof pl, "Lume Intelligence is off. Reports, the catalog and every editor work without it.");
    else snprintf(pl, sizeof pl, "%s \xC2\xB7 %s. Keys stay in the %s, never in a file.", ai_provider_label(G.cfg.provider), G.cfg.model, secstore_name());
    SLINE(pl, T_TEXT_MUTED, px(a, 40));
    if (ui_button(a, rect_make(x, y, px(a, 230), rh), off ? "Set up Lume Intelligence..." : "Change provider, model or key...", BTN_NORMAL, true)) { settings_close(a); panels_show(a->panels, a, "lume.data.assistant"); setup_open(a); return; }
    if (!off && ui_button(a, rect_make(x + px(a, 236), y, px(a, 100), rh), "Turn off", BTN_QUIET, true)) set_ai_enabled(a, false);
    y += rh + px(a, 8);
    SHEAD("Fast model");
    SLINE("Used for the quick requests (Explain, Fix the error, Cmd+K, Document). Empty: the model above.", T_TEXT_DIM, px(a, 34));
    static TextField fast; static bool fast_init;
    if (!fast_init) { textfield_set(&fast, G.cfg.model_fast); fast_init = true; }
    Rect fr = rect_make(x, y, w - px(a, 96), rh);
    if (ui_textfield(a, &fast, fr, "same as the model", focus_id == 25)) focus_id = 25;
    ui_record("settings:fastmodel", fr);
    if (ui_button(a, rect_make(x + w - px(a, 90), y, px(a, 90), rh), "Save", BTN_PRIMARY, strcmp(fast.buf, G.cfg.model_fast) != 0)) { ai_txt_set(a, "model_fast", fast.buf); app_status(a, "Fast model saved"); }
    y += rh + px(a, 8);
    SHEAD("Answers");
    SLINE("Answers stream as they are written (Anthropic and the ChatGPT plan); an answer is applied only when it is complete and checked.", T_TEXT_DIM, px(a, 34));
    { char tk[200]; snprintf(tk, sizeof tk, "This session: %d request(s), %d tokens in, %d out, as the provider reported them.", G.nreq, G.tok_in, G.tok_out); SLINE(tk, T_TEXT_MUTED, px(a, 30)); }
    SHEAD("Remembered per connection");
    SLINE("What each connection allows (structure, category values) and its scope (schemas, tables always / never sent). The values of a card are asked again every session, on purpose.", T_TEXT_DIM, px(a, 50));
    {
        char p[1200]; scope_path(a, p, sizeof p);
        FILE *f = fopen(p, "r"); char ids[24][16]; int nid = 0; char line[700];
        while (f && fgets(line, sizeof line, f) && nid < 24) { char *t = strchr(line, '\t'); if (!t) continue; *t = 0; bool dup = false; for (int i = 0; i < nid; i++) if (!strcmp(ids[i], line)) dup = true; if (!dup) snprintf(ids[nid++], sizeof ids[0], "%s", line); }
        if (f) fclose(f);
        if (!nid) SLINE("Nothing yet: it is remembered when you allow it in the assistant.", T_TEXT_DIM, px(a, 24));
        for (int i = 0; i < nid; i++) {
            Connection *c = conn_of(a, ids[i]);
            G.scope_loaded = false; scope_load(a, c ? c : &(Connection){ 0 });
            char d2[500]; snprintf(d2, sizeof d2, "%s: structure %s, category values %s%s%s%s%s%s%s", c ? c->name : ids[i],
                strstr(G.scope_consent, "schema") ? "allowed" : "not allowed", strstr(G.scope_consent, "catvalues") ? "allowed" : "not allowed",
                G.scope_schemas[0] ? "; schemas: " : "", G.scope_schemas, G.scope_pin[0] ? "; always: " : "", G.scope_pin, G.scope_block[0] ? "; never: " : "", G.scope_block);
            float hh = results_draw_wrapped(a->font_small, r, d2, rect_make(x, y + px(a, 4), w - px(a, 96), px(a, 40)), C(T_TEXT), 0, true);
            char tg[48]; snprintf(tg, sizeof tg, "settings:forget %s", ids[i]); Rect fb = rect_make(x + w - px(a, 90), y, px(a, 90), px(a, 24)); ui_record(tg, fb);
            if (ui_button(a, fb, "Forget", BTN_QUIET, true)) { scope_forget(a, ids[i]); break; }
            y += fmaxf(hh + px(a, 8), px(a, 28));
        }
        G.scope_loaded = false;
    }
    SHEAD("Glossary");
    {
        const char *gl = glossary_text(a); int nt = 0; for (const char *q = gl; *q; q++) nt += *q == '\n';
        char gm[1400]; snprintf(gm, sizeof gm, "%d term(s) in %s. Sent with every request; its definitions also steer which tables are picked.", nt, G.gloss_path);
        SLINE(gm, T_TEXT_MUTED, px(a, 40));
        if (ui_button(a, rect_make(x, y, px(a, 150), rh), "Edit glossary...", BTN_NORMAL, true)) {
            struct stat st; if (stat(G.gloss_path, &st) != 0) { FILE *gf = fopen(G.gloss_path, "w"); if (gf) { fputs("# Business terms of this workspace and where they live, one per line: term = definition\n", gf); fclose(gf); } }
            settings_close(a); panels_open_file_tab(a->panels, a, G.gloss_path); return;
        }
        y += rh + px(a, 8);
    }
    #undef SLINE
    #undef SHEAD
    *py = y;
}
