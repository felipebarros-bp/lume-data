/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Report Studio document model (LI-038..051 / 059 / 091 / 094, rules R01..R05).
 *
 *   ReportSpec: pages of visual specs on a 12-column grid, theme, locale, report period and page
 *   filters, provenance. Declarative only: a visual names a catalog metric (id + the version it was
 *   validated with), a dimension, a grain and a comparison; Lume compiles the query (sem.h). Nothing
 *   in a report is code, credentials or a DSN (LI-083 / R04): a SQL-backed table keeps the user's own
 *   SQL and a connection id.
 *
 *   AI proposals (R02): the model returns a JSON proposal of typed actions (add / update / move a
 *   visual, update a narrative). rpt_apply_proposal validates every key, id, metric, dimension, grain
 *   and layout against the catalog on a copy, refuses a stale base revision (concurrency, no silent
 *   overwrite) and actions outside the selected visual unless global changes were allowed; only then
 *   the document changes, as one undoable step.
 *
 *   Undo / redo keep whole-document snapshots: undoing never re-runs SQL (data is cached by query).
 *
 * Headless: no UI, no connection.
 */
#include <stdbool.h>
#include <stddef.h>
#include "sem.h"

#define RPT_FORMAT 1
#define RPT_COLS 12
#define RPT_MAX_PAGES 16
#define RPT_MAX_FILTERS 8
#define RPT_MAX_PARAMS 8
/* A report filter the SQL cards use as a named parameter (:name). Its options come from a read-only
 * query (first column) or the values typed; value "" = all (bound as NULL). */
enum { RPT_P_SELECT, RPT_P_TEXT };
typedef struct RptParam { char name[32], label[64]; int type; char conn[16]; char options_sql[1000]; char value[128]; } RptParam;
#define RPT_TEXT_MAX 4000

enum { RK_KPI, RK_LINE, RK_BAR, RK_TABLE, RK_TEXT, RK_IMAGE, RK_SHAPE, RK_UNKNOWN, RK_N };
enum { RC_NONE, RC_PREV, RC_YOY, RC_N };                      /* KPI comparator */
enum { RF_AUTO, RF_NUMBER, RF_COMPACT, RF_PERCENT, RF_CURRENCY, RF_N };
enum { RS_DRAFT, RS_REVIEWED };
enum { RM_LIVE, RM_SNAPSHOT };

const char *rpt_kind_name(int k);    /* "kpi", "line", ... */
int rpt_kind_by_name(const char *s); /* RK_UNKNOWN when not supported */
const char *rpt_compare_name(int c); /* "none", "previous_period", "yoy" */
int rpt_compare_by_name(const char *s);
const char *rpt_fmt_name(int f);
int rpt_fmt_by_name(const char *s);

typedef struct RptVisual {
    char id[24];
    int kind;
    char kind_name[24];            /* as written in the file (kept for unknown kinds) */
    char title[128], subtitle[160];
    char metric[48]; int metric_version;   /* version validated with; 0 = not pinned yet */
    char dim[64];                  /* category dimension (bar / table), "" none */
    int grain;                     /* SG_* for line, -1 none */
    int compare;                   /* RC_* (KPI) */
    int top;                       /* categories / rows cap; 0 = default */
    int fmt, decimals;             /* RF_*, -1 automatic */
    char *text;                    /* text card body (malloc'd); "\n" separates lines, "- " a bullet */
    char *sql; char conn[16];      /* the card's own SQL: the user's (Add Result to Report, LI-031) or the AI's (ai_sql) */
    bool ai_sql;                   /* SQL written by the AI from the shared schema: not certified, shown as such */
    bool sql_edited;               /* the user changed the AI's SQL by hand (still not certified) */
    char *style;                   /* the card's own look (JSON object: fill, fill2, text, accent, radius, variant, align, size, weight, border), NULL = the theme's */
    char src[256];                 /* image: "theme:logo" or a file next to the report */
    int x, y, w, h; bool locked;   /* grid units: columns 0..11, rows of RPT_ROW points */
    bool ai_suggested;             /* AI text not yet approved (LI-043.2) */
    char *raw;                     /* unknown kind: original JSON, preserved (R05) */
} RptVisual;

typedef struct RptPage { char id[24], name[64], theme[16]; RptVisual *v; int nv, cap; } RptPage;
typedef struct RptFilter { char id[24]; char field[64]; char page[24]; char value[128]; } RptFilter;   /* page "" = whole report */

typedef struct Report {
    int format_version;
    char id[40];
    int revision;                  /* +1 per committed change */
    char title[128];
    char theme[16];                /* light, dark, executive (the built-in theme when no theme_ref) */
    char theme_ref[512];           /* "" / "builtin:<name>", "workspace" (the Brand Kit), "file:<path relative to the report>" */
    char *theme_snapshot;          /* the theme last resolved (JSON): renders where the file is missing */
    char *theme_overrides;         /* local changes on top of the theme (JSON); a locked theme ignores them */
    char locale[8];                /* pt-BR, en-US */
    int data_mode, status;         /* RM_*, RS_* */
    char period[24];               /* last_12_months, ytd, last_30_days, previous_year, all, custom */
    char from[11], to[11];         /* custom period, [from, to) */
    char as_of[11];                /* reference date of relative periods, "" = today */
    RptPage pages[RPT_MAX_PAGES]; int np;
    RptFilter filters[RPT_MAX_FILTERS]; int nf;
    RptParam params[RPT_MAX_PARAMS]; int nparams;   /* report filters bound into SQL cards as :name */
    char created_by[48], created_at[24], approved_at[24];
    char choices[4][120]; int nchoices;   /* clarifications the user made (LI-021.2): "faturamento = Receita líquida (v2)" */
    int next_id;
} Report;

void rpt_init(Report *r, const char *title);
void rpt_free(Report *r);
bool rpt_copy(Report *dst, const Report *src);   /* deep copy (dst uninitialized) */

/* ---- file (LI-050 / 094) ---- */
char *rpt_to_json(const Report *r);              /* malloc'd */
/* false with why: invalid JSON, newer format (opened read-only, never rewritten), bad structure.
 * Older formats are migrated in memory (migrated set). Unknown visual kinds are preserved. */
bool rpt_parse(Report *r, const char *text, size_t len, bool *migrated, char *why, int n);
bool rpt_save_atomic(const Report *r, const char *path, char *why, int n);
bool rpt_load(Report *r, const char *path, bool *migrated, char *why, int n);

/* ---- editing ---- */
RptPage *rpt_add_page(Report *r, const char *name);
RptVisual *rpt_add_visual(Report *r, int page, int kind);  /* placed at the first free grid slot */
RptVisual *rpt_find(Report *r, const char *id, int *page); /* NULL when absent */
bool rpt_remove_visual(Report *r, const char *id);
void rpt_visual_set_text(RptVisual *v, const char *text);
void rpt_min_size(int kind, int *w, int *h);               /* below this a card is unreadable */
/* move / resize on the grid: snapped, clamped, minimum size; other cards pushed down so nothing
 * overlaps silently. false when locked. */
bool rpt_place(Report *r, int page, const char *id, int x, int y, int w, int h);
bool rpt_overlaps(const RptPage *p, int *a, int *b);

/* ---- validation (LI-040 / 041 / 047 / 023) ---- */
/* a visual's data contract against the catalog: metric known (and its pinned version), dimension
 * authorized, grain allowed, comparator meaningful. false with a user-facing reason (the card is
 * blocked and says so, R05 "métrica incompatível"). */
bool rpt_visual_check(const Report *r, const RptVisual *v, const SemCatalog *c, char *why, int n);

/* ---- periods (R03) ---- */
/* the report period as [from, to) for its preset relative to as_of (today when ""), and the
 * comparison window: RC_PREV the same length just before, RC_YOY one year earlier. label says it
 * in words ("12 meses até 31/10/2026"). false for "all" (no bounds) only when cmp != RC_NONE. */
bool rpt_period(const Report *r, int cmp, SemPeriod *cur, SemPeriod *base, char *label, int ln);
void rpt_today(char out[11]);

/* ---- AI proposals (R02 / LI-051 / 059) ---- */
typedef struct RptApply {
    int actions, added, updated, moved, narratives, filters;
    char filter_names[RPT_MAX_PARAMS][32]; int nfilter_names;   /* filters added (their options query is checked by the caller) */
    char *theme_json;                    /* set_theme: the proposed theme changes (malloc'd, the caller validates contrast / lock and frees) */
    char summary[300];
    char changed[8][24]; int nchanged;   /* visual ids touched */
    bool metric_changed;                 /* an update changed a card's metric (shown in the review) */
} RptApply;
/* validates the whole proposal against the catalog and the current revision; applies it only when
 * every action is valid. scope_visual: the selected card ("" = page-level request); actions on other
 * cards need allow_global. Model text becomes ai_suggested until the user approves. */
bool rpt_apply_proposal(Report *r, const SemCatalog *c, const char *json, size_t len, int page, const char *scope_visual, bool allow_global, RptApply *out, char *why, int n);
/* the same, accepting visuals with "sql" (kpi / line / bar / table) bound to connection sql_conn when the
 * user shared that connection's structure; sql_conn NULL refuses them. The SQL itself is checked by the
 * caller (read-only policy, local test run) before anything runs. */
bool rpt_apply_proposal_ex(Report *r, const SemCatalog *c, const char *json, size_t len, int page, const char *scope_visual, bool allow_global, const char *sql_conn, RptApply *out, char *why, int n);
#define RPT_AI_SQL_MAX 8000
/* binds :name parameters into sql: report filters (literal or NULL for "all") and :period_start /
 * :period_end (the report period, [start, end) ISO dates). Strings, quoted identifiers, comments and
 * PostgreSQL :: casts are left alone; an unknown parameter is an error. used (optional) receives the
 * names of the report filters the query uses, comma separated. */
bool rpt_bind_params(const Report *r, const char *sql, char *out, size_t n, char *used, size_t un, char *why, int wn);
bool rpt_param_name_ok(const char *s);
/* a card style object: known keys only, colours #RRGGBB, sizes in range */
bool rpt_style_check(const char *json, size_t len, char *why, int n);

/* ---- undo / redo (LI-044.2 / 059.1) ---- */
#define RPT_UNDO 64
typedef struct RptHist { char *u[RPT_UNDO]; int nu; char *rd[RPT_UNDO]; int nr; } RptHist;
void rpt_hist_push(RptHist *h, const Report *r);   /* before a change */
bool rpt_undo(RptHist *h, Report *r);
bool rpt_redo(RptHist *h, Report *r);
void rpt_hist_free(RptHist *h);

/* ---- formatting (LI-046): display only, the stored value is never changed ---- */
void rpt_fmt_value(double v, bool is_null, const SemMetricDef *m, int fmt, int decimals, const char *locale, char *out, int n);
void rpt_fmt_period(const char *iso, int grain, const char *locale, char *out, int n);   /* "out/2026", "T3 2026", "2026" */
void rpt_fmt_date(const char *iso, const char *locale, char *out, int n);
/* comparison: abs = cur - base; rel = abs / |base| when base is a non-zero number (rel_ok) */
void rpt_delta(double cur, bool cur_null, double base, bool base_null, double *abs, double *rel, bool *abs_ok, bool *rel_ok);
