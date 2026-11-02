/* SPDX-License-Identifier: AGPL-3.0-only */
/* Report Studio document model: see rpt.h. */
#include "rpt.h"
#include "jw.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <time.h>
#include <unistd.h>

static const char *const KIND[] = { "kpi", "line", "bar", "table", "text", "image", "shape" };
static const char *const CMP[] = { "none", "previous_period", "yoy" };
static const char *const FMT[] = { "auto", "number", "compact", "percent", "currency" };
const char *rpt_kind_name(int k) { return k >= 0 && k < RK_UNKNOWN ? KIND[k] : "unknown"; }
int rpt_kind_by_name(const char *s) { for (int i = 0; i < RK_UNKNOWN; i++) if (s && !strcasecmp(s, KIND[i])) return i; return RK_UNKNOWN; }
const char *rpt_compare_name(int c) { return c >= 0 && c < RC_N ? CMP[c] : "none"; }
int rpt_compare_by_name(const char *s) { for (int i = 0; i < RC_N; i++) if (s && !strcasecmp(s, CMP[i])) return i; if (s && !strcasecmp(s, "previous")) return RC_PREV; return -1; }
const char *rpt_fmt_name(int f) { return f >= 0 && f < RF_N ? FMT[f] : "auto"; }
int rpt_fmt_by_name(const char *s) { for (int i = 0; i < RF_N; i++) if (s && !strcasecmp(s, FMT[i])) return i; return -1; }

static void cp(char *d, size_t n, const char *s) { snprintf(d, n, "%s", s ? s : ""); }
#define CP(d, s) cp(d, sizeof(d), s)

void rpt_today(char out[11]) { time_t t = time(NULL); struct tm tm; localtime_r(&t, &tm); strftime(out, 11, "%Y-%m-%d", &tm); }

static void page_init(RptPage *p, int n, const char *name) { memset(p, 0, sizeof *p); snprintf(p->id, sizeof p->id, "p%d", n); CP(p->name, name); }
void rpt_init(Report *r, const char *title) {
    memset(r, 0, sizeof *r);
    r->format_version = RPT_FORMAT; r->revision = 1; r->next_id = 1;
    CP(r->title, title && *title ? title : "Untitled report");
    CP(r->theme, "light"); CP(r->locale, "pt-BR"); CP(r->period, "last_12_months");
    srand((unsigned)time(NULL) ^ (unsigned)getpid());
    snprintf(r->id, sizeof r->id, "rpt-%08x%04x", (unsigned)time(NULL), (unsigned)(rand() & 0xffff));
    const char *u = getenv("USER"); CP(r->created_by, u ? u : "");
    time_t t = time(NULL); struct tm tm; gmtime_r(&t, &tm); strftime(r->created_at, sizeof r->created_at, "%Y-%m-%dT%H:%M:%SZ", &tm);
    r->np = 1; page_init(&r->pages[0], 1, "Page 1");
}
static void vis_free(RptVisual *v) { free(v->text); free(v->sql); free(v->raw); free(v->style); v->text = v->sql = v->raw = v->style = NULL; }
void rpt_free(Report *r) {
    for (int p = 0; p < r->np; p++) { for (int i = 0; i < r->pages[p].nv; i++) vis_free(&r->pages[p].v[i]); free(r->pages[p].v); }
    free(r->theme_snapshot); free(r->theme_overrides);
    memset(r, 0, sizeof *r);
}
static char *sdup(const char *s) { return s ? strdup(s) : NULL; }
bool rpt_copy(Report *dst, const Report *src) {
    *dst = *src;
    dst->theme_snapshot = src->theme_snapshot ? strdup(src->theme_snapshot) : NULL;
    dst->theme_overrides = src->theme_overrides ? strdup(src->theme_overrides) : NULL;
    for (int p = 0; p < src->np; p++) {
        RptPage *d = &dst->pages[p]; const RptPage *s = &src->pages[p];
        d->v = s->nv ? malloc(sizeof *d->v * (size_t)s->nv) : NULL; d->cap = s->nv;
        for (int i = 0; i < s->nv; i++) { d->v[i] = s->v[i]; d->v[i].text = sdup(s->v[i].text); d->v[i].sql = sdup(s->v[i].sql); d->v[i].raw = sdup(s->v[i].raw); d->v[i].style = sdup(s->v[i].style); }
    }
    return true;
}

/* ---- layout ---- */
void rpt_min_size(int kind, int *w, int *h) {
    switch (kind) {
    case RK_KPI: *w = 2; *h = 3; break;   /* title, value and a provenance line */
    case RK_LINE: case RK_BAR: *w = 3; *h = 4; break;
    case RK_TABLE: *w = 3; *h = 3; break;
    case RK_TEXT: *w = 2; *h = 1; break;
    case RK_IMAGE: case RK_SHAPE: *w = 1; *h = 1; break;
    default: *w = 3; *h = 3;
    }
}
static void default_size(int kind, int *w, int *h) {
    switch (kind) {
    case RK_KPI: *w = 3; *h = 4; break;   /* value, change, comparator and base fit without cutting */
    case RK_LINE: case RK_BAR: *w = 6; *h = 6; break;
    case RK_TABLE: *w = 12; *h = 6; break;
    case RK_TEXT: *w = 12; *h = 2; break;
    case RK_IMAGE: *w = 2; *h = 2; break;
    case RK_SHAPE: *w = 12; *h = 1; break;
    default: *w = 6; *h = 4;
    }
}
static bool inter(const RptVisual *a, const RptVisual *b) { return a->x < b->x + b->w && b->x < a->x + a->w && a->y < b->y + b->h && b->y < a->y + a->h; }
bool rpt_overlaps(const RptPage *p, int *a, int *b) {
    for (int i = 0; i < p->nv; i++) for (int k = i + 1; k < p->nv; k++) if (inter(&p->v[i], &p->v[k])) { if (a) *a = i; if (b) *b = k; return true; }
    return false;
}
static bool slot_free(const RptPage *p, int x, int y, int w, int h, int skip) {
    RptVisual t; memset(&t, 0, sizeof t); t.x = x; t.y = y; t.w = w; t.h = h;
    for (int i = 0; i < p->nv; i++) if (i != skip && inter(&t, &p->v[i])) return false;
    return true;
}
RptPage *rpt_add_page(Report *r, const char *name) {
    if (r->np >= RPT_MAX_PAGES) return NULL;
    int maxn = 0; for (int i = 0; i < r->np; i++) { int k = atoi(r->pages[i].id + 1); if (k > maxn) maxn = k; }
    char nm[64]; if (!name || !*name) { snprintf(nm, sizeof nm, "Page %d", r->np + 1); name = nm; }
    page_init(&r->pages[r->np], maxn + 1, name);
    return &r->pages[r->np++];
}
static RptVisual *push_visual(RptPage *p) {
    if (p->nv == p->cap) { int c = p->cap ? p->cap * 2 : 8; RptVisual *nv = realloc(p->v, sizeof *nv * (size_t)c); if (!nv) return NULL; p->v = nv; p->cap = c; }
    RptVisual *v = &p->v[p->nv++]; memset(v, 0, sizeof *v);
    v->grain = -1; v->decimals = -1;
    return v;
}
RptVisual *rpt_add_visual(Report *r, int page, int kind) {
    if (page < 0 || page >= r->np) return NULL;
    RptPage *p = &r->pages[page];
    RptVisual *v = push_visual(p); if (!v) return NULL;
    v->kind = kind; CP(v->kind_name, rpt_kind_name(kind));
    snprintf(v->id, sizeof v->id, "v%d", r->next_id++);
    default_size(kind, &v->w, &v->h);
    if (kind == RK_LINE) v->grain = SG_MONTH;
    for (int y = 0;; y++) { bool done = false; for (int x = 0; x + v->w <= RPT_COLS; x++) if (slot_free(p, x, y, v->w, v->h, p->nv - 1)) { v->x = x; v->y = y; done = true; break; } if (done) break; }
    return v;
}
RptVisual *rpt_find(Report *r, const char *id, int *page) {
    for (int p = 0; id && p < r->np; p++) for (int i = 0; i < r->pages[p].nv; i++) if (!strcmp(r->pages[p].v[i].id, id)) { if (page) *page = p; return &r->pages[p].v[i]; }
    return NULL;
}
bool rpt_remove_visual(Report *r, const char *id) {
    int pg; RptVisual *v = rpt_find(r, id, &pg); if (!v) return false;
    RptPage *p = &r->pages[pg]; int i = (int)(v - p->v);
    vis_free(v); memmove(&p->v[i], &p->v[i + 1], sizeof *v * (size_t)(p->nv - i - 1)); p->nv--;
    return true;
}
void rpt_visual_set_text(RptVisual *v, const char *text) {
    free(v->text); v->text = NULL;
    if (!text) return;
    size_t L = strlen(text); if (L > RPT_TEXT_MAX) L = RPT_TEXT_MAX;
    v->text = malloc(L + 1); memcpy(v->text, text, L); v->text[L] = 0;
    for (char *p = v->text; *p; p++) if ((unsigned char)*p < 0x20 && *p != '\n') *p = ' ';   /* plain text only */
}
bool rpt_place(Report *r, int page, const char *id, int x, int y, int w, int h) {
    if (page < 0 || page >= r->np) return false;
    RptPage *p = &r->pages[page];
    int k = -1; for (int i = 0; i < p->nv; i++) if (!strcmp(p->v[i].id, id)) k = i;
    if (k < 0 || p->v[k].locked || p->v[k].kind == RK_UNKNOWN) return false;
    int mw, mh; rpt_min_size(p->v[k].kind, &mw, &mh);
    if (w < mw) w = mw; if (w > RPT_COLS) w = RPT_COLS; if (h < mh) h = mh; if (h > 40) h = 40;
    if (x < 0) x = 0; if (x + w > RPT_COLS) x = RPT_COLS - w; if (y < 0) y = 0; if (y > 400) y = 400;
    RptVisual save = p->v[k];
    p->v[k].x = x; p->v[k].y = y; p->v[k].w = w; p->v[k].h = h;
    /* a locked card in the way: refuse rather than move it */
    for (int i = 0; i < p->nv; i++) if (i != k && (p->v[i].locked || p->v[i].kind == RK_UNKNOWN) && inter(&p->v[k], &p->v[i])) { p->v[k] = save; return false; }
    /* push the others down until nothing overlaps (cascade) */
    for (int it = 0; it < 200; it++) {
        int a, b; if (!rpt_overlaps(p, &a, &b)) break;
        int mover = a == k ? b : b == k ? a : (p->v[a].y <= p->v[b].y ? b : a);
        int fixed = mover == a ? b : a;
        if (p->v[mover].locked || p->v[mover].kind == RK_UNKNOWN) { int t = mover; mover = fixed; fixed = t; }
        if (mover == k || p->v[mover].locked) { p->v[k] = save; return false; }
        p->v[mover].y = p->v[fixed].y + p->v[fixed].h;
    }
    return !rpt_overlaps(p, NULL, NULL) || (p->v[k] = save, false);
}

/* ---- report filters as SQL parameters ---- */
bool rpt_param_name_ok(const char *s) {
    if (!s[0] || strlen(s) > 30 || !((s[0] >= 'a' && s[0] <= 'z') || s[0] == '_')) return false;
    for (const char *p = s; *p; p++) if (!((*p >= 'a' && *p <= 'z') || (*p >= '0' && *p <= '9') || *p == '_')) return false;
    return strcmp(s, "period_start") && strcmp(s, "period_end");
}
static bool ident_ch(char c) { return (c >= 'a' && c <= 'z') || (c >= 'A' && c <= 'Z') || (c >= '0' && c <= '9') || c == '_'; }
bool rpt_bind_params(const Report *r, const char *sql, char *out, size_t n, char *used, size_t un, char *why, int wn) {
    size_t w = 0; if (used && un) used[0] = 0;
    #define PUT(c) do { if (w + 1 >= n) { snprintf(why, (size_t)wn, "the query is too long"); return false; } out[w++] = (c); } while (0)
    SemPeriod cur, base; char label[80]; bool have_period = rpt_period(r, RC_NONE, &cur, &base, label, sizeof label);
    for (const char *p = sql; *p;) {
        char c = *p;
        if (c == '\'' || c == '"' || c == '`' || c == '[') {   /* strings and quoted identifiers are copied untouched */
            char close = c == '[' ? ']' : c; PUT(c); p++;
            while (*p) { PUT(*p); if (*p == close) { if (p[1] == close && close != ']') { p++; PUT(*p); p++; continue; } p++; break; } p++; }
            continue;
        }
        if (c == '-' && p[1] == '-') { while (*p && *p != '\n') { PUT(*p); p++; } continue; }
        if (c == '/' && p[1] == '*') { PUT(*p); p++; PUT(*p); p++; while (*p && !(p[0] == '*' && p[1] == '/')) { PUT(*p); p++; } if (*p) { PUT(*p); p++; PUT(*p); p++; } continue; }
        if (c == ':' && p[1] == ':') { PUT(':'); PUT(':'); p += 2; continue; }   /* PostgreSQL cast */
        if (c == ':' && ((p[1] >= 'a' && p[1] <= 'z') || p[1] == '_') && (p == sql || !ident_ch(p[-1]))) {
            char name[40]; size_t k = 0; const char *q = p + 1;
            while (ident_ch(*q) && k < sizeof name - 1) name[k++] = *q++;
            name[k] = 0;
            const char *val = NULL; bool is_null = false;
            if (!strcmp(name, "period_start") || !strcmp(name, "period_end")) {
                const char *v = !strcmp(name, "period_start") ? cur.from : cur.to;
                if (!have_period || !v[0]) is_null = true; else val = v;
            } else {
                int found = -1; for (int i = 0; i < r->nparams; i++) if (!strcmp(r->params[i].name, name)) found = i;
                if (found < 0) { snprintf(why, (size_t)wn, "unknown parameter :%s (no report filter has that name)", name); return false; }
                if (r->params[found].value[0]) val = r->params[found].value; else is_null = true;
                if (used && un && !strstr(used, name)) { size_t L = strlen(used); snprintf(used + L, un - L, "%s%s", L ? "," : "", name); }
            }
            if (is_null) { const char *s2 = "NULL"; while (*s2) PUT(*s2++); }
            else { PUT('\''); for (const char *v = val; *v; v++) { if (*v == '\'') PUT('\''); PUT(*v); } PUT('\''); }   /* a literal, quotes doubled: never SQL text */
            p = q; continue;
        }
        PUT(c); p++;
    }
    out[w] = 0;
    #undef PUT
    return true;
}

/* ---- card styles ---- */
static bool hex_ok(const char *s) { if (s[0] != '#' || (strlen(s) != 7 && strlen(s) != 4)) return false; for (const char *p = s + 1; *p; p++) if (!strchr("0123456789abcdefABCDEF", *p)) return false; return true; }
bool rpt_style_check(const char *json, size_t len, char *why, int n) {
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, json, len) || j.nodes[0].type != JN_OBJECT) { snprintf(why, (size_t)n, "not a JSON object"); json_free(&j); return false; }
    static const char *const keys[] = { "fill", "fill2", "text", "accent", "border", "radius", "variant", "align", "size", "weight", "shape", "opacity", NULL };
    char bad[64], s[64]; bool ok = jr_keys_allowed(&j, 0, keys, bad, sizeof bad);
    if (!ok) snprintf(why, (size_t)n, "'%.30s' is not a style field", bad);
    static const char *const ck[] = { "fill", "fill2", "text", "accent", "border", NULL };
    for (int i = 0; ok && ck[i]; i++) if (jr_str(&j, 0, ck[i], s, sizeof s) && !hex_ok(s)) { snprintf(why, (size_t)n, "%s: '%.12s' is not a #RRGGBB colour", ck[i], s); ok = false; }
    double d;
    if (ok && jr_num(&j, 0, "radius", &d) && (d < 0 || d > 40)) { snprintf(why, (size_t)n, "radius must be 0..40"); ok = false; }
    if (ok && jr_num(&j, 0, "size", &d) && (d < 8 || d > 96)) { snprintf(why, (size_t)n, "size must be 8..96"); ok = false; }
    if (ok && jr_num(&j, 0, "opacity", &d) && (d < 0 || d > 1)) { snprintf(why, (size_t)n, "opacity must be 0..1"); ok = false; }
    if (ok && jr_str(&j, 0, "align", s, sizeof s) && strcmp(s, "left") && strcmp(s, "center") && strcmp(s, "right")) { snprintf(why, (size_t)n, "align: left, center or right"); ok = false; }
    if (ok && jr_str(&j, 0, "weight", s, sizeof s) && strcmp(s, "regular") && strcmp(s, "bold")) { snprintf(why, (size_t)n, "weight: regular or bold"); ok = false; }
    if (ok && jr_str(&j, 0, "variant", s, sizeof s) && strcmp(s, "flat") && strcmp(s, "shadow") && strcmp(s, "border") && strcmp(s, "accent-top") && strcmp(s, "plain") && strcmp(s, "accent-bar") && strcmp(s, "tinted") && strcmp(s, "none")) { snprintf(why, (size_t)n, "variant: flat, shadow, border, accent-top, plain, accent-bar, tinted or none"); ok = false; }
    if (ok && jr_str(&j, 0, "shape", s, sizeof s) && strcmp(s, "rect") && strcmp(s, "line") && strcmp(s, "circle")) { snprintf(why, (size_t)n, "shape: rect, line or circle"); ok = false; }
    json_free(&j);
    return ok;
}

/* ---- validation ---- */
static const SemMetricDef *pinned(const SemCatalog *c, const RptVisual *v, const SemMetric **mm) {
    const SemMetric *m = sem_metric_c(c, v->metric); if (mm) *mm = m;
    if (!m) return NULL;
    return v->metric_version ? sem_version(m, v->metric_version) : sem_cur(m);
}
bool rpt_visual_check(const Report *r, const RptVisual *v, const SemCatalog *c, char *why, int n) {
    if (v->kind == RK_UNKNOWN) { snprintf(why, (size_t)n, "Visual '%s' is not supported by this renderer: its spec is kept unchanged", v->kind_name); return false; }
    if (v->kind == RK_TEXT) { if (v->text && strlen(v->text) > RPT_TEXT_MAX) { snprintf(why, (size_t)n, "text too long"); return false; } return true; }
    if (v->kind == RK_SHAPE) return true;
    if (v->kind == RK_IMAGE) {
        if (!v->src[0]) { snprintf(why, (size_t)n, "choose an image"); return false; }
        if (strncmp(v->src, "theme:", 6) && (strstr(v->src, "..") || v->src[0] == '/' || strchr(v->src, '\\'))) { snprintf(why, (size_t)n, "the image must be the theme's logo or a file next to the report"); return false; }
        return true;
    }
    if (v->sql && (v->kind == RK_TABLE || v->kind == RK_KPI || v->kind == RK_LINE || v->kind == RK_BAR)) {
        if (!v->conn[0]) { snprintf(why, (size_t)n, "this card has SQL but no connection"); return false; }
        return true;
    }
    const SemMetric *m; const SemMetricDef *d = pinned(c, v, &m);
    if (!v->metric[0]) { snprintf(why, (size_t)n, "choose a metric from the semantic catalog"); return false; }
    if (!m) { snprintf(why, (size_t)n, "metric '%s' is not in the semantic catalog", v->metric); return false; }
    if (!d) { snprintf(why, (size_t)n, "%s v%d is no longer kept in the catalog: review the card (current is v%d)", m->id, v->metric_version, sem_cur(m)->version); return false; }
    const SemDataset *ds = sem_dataset_c(c, d->dataset);
    if (!ds) { snprintf(why, (size_t)n, "dataset %s of %s is not in the catalog", d->dataset, d->name); return false; }
    if (v->dim[0] && !sem_is_dimension(ds, v->dim)) { snprintf(why, (size_t)n, "'%s' is not an authorized dimension of %s", v->dim, d->name); return false; }
    if (v->grain >= 0 && !(d->grains & (1u << v->grain))) { snprintf(why, (size_t)n, "%s is not defined per %s", d->name, sem_grain_name(v->grain)); return false; }
    if (v->grain >= 0 && !ds->time_column[0]) { snprintf(why, (size_t)n, "%s has no time column: no time series", ds->id); return false; }
    switch (v->kind) {
    case RK_KPI:
        if (v->dim[0] || v->grain >= 0) { snprintf(why, (size_t)n, "a KPI shows one value: no dimension or grain"); return false; }
        if (v->compare != RC_NONE && (!ds->time_column[0] || !strcmp(r->period, "all"))) { snprintf(why, (size_t)n, "a comparison needs a time column and a bounded period"); return false; }
        break;
    case RK_LINE:
        if (v->grain < 0) { snprintf(why, (size_t)n, "a line chart needs a time grain"); return false; }
        if (v->dim[0]) { snprintf(why, (size_t)n, "a line chart here shows one series: remove the dimension or use bars"); return false; }
        break;
    case RK_BAR:
        if ((v->grain >= 0) == (v->dim[0] != 0)) { snprintf(why, (size_t)n, "a bar chart needs either a dimension or a time grain"); return false; }
        break;
    case RK_TABLE:
        if (v->grain < 0 && !v->dim[0]) { snprintf(why, (size_t)n, "a table needs a dimension or a time grain"); return false; }
        break;
    }
    if (v->kind != RK_KPI && v->compare != RC_NONE) { snprintf(why, (size_t)n, "comparisons are shown on KPI cards"); return false; }
    return true;
}

/* ---- dates ---- */
static long days_from_civil(int y, int m, int d) {
    y -= m <= 2; long era = (y >= 0 ? y : y - 399) / 400; unsigned yoe = (unsigned)(y - era * 400);
    unsigned doy = (unsigned)((153 * (m + (m > 2 ? -3 : 9)) + 2) / 5 + d - 1); unsigned doe = yoe * 365 + yoe / 4 - yoe / 100 + doy;
    return era * 146097 + (long)doe - 719468;
}
static void civil_from_days(long z, int *y, int *m, int *d) {
    z += 719468; long era = (z >= 0 ? z : z - 146096) / 146097; unsigned doe = (unsigned)(z - era * 146097);
    unsigned yoe = (doe - doe / 1460 + doe / 36524 - doe / 146096) / 365; long yy = (long)yoe + era * 400;
    unsigned doy = doe - (365 * yoe + yoe / 4 - yoe / 100); unsigned mp = (5 * doy + 2) / 153;
    *d = (int)(doy - (153 * mp + 2) / 5 + 1); *m = (int)(mp < 10 ? mp + 3 : mp - 9); *y = (int)(yy + (*m <= 2));
}
static int dim_(int y, int m) { static const int t[] = { 31, 28, 31, 30, 31, 30, 31, 31, 30, 31, 30, 31 }; return m == 2 && ((y % 4 == 0 && y % 100) || y % 400 == 0) ? 29 : t[m - 1]; }
static void iso(int y, int m, int d, char out[11]) { snprintf(out, 11, "%04d-%02d-%02d", y, m, d); }
static void parse_iso(const char *s, int *y, int *m, int *d) { *y = atoi(s); *m = atoi(s + 5); *d = atoi(s + 8); }
static void add_days(const char *s, long n, char out[11]) { int y, m, d; parse_iso(s, &y, &m, &d); civil_from_days(days_from_civil(y, m, d) + n, &y, &m, &d); iso(y, m, d, out); }
static void add_months(const char *s, int n, char out[11]) {
    int y, m, d; parse_iso(s, &y, &m, &d);
    int t = y * 12 + (m - 1) + n; y = t / 12; m = t % 12 + 1; if (d > dim_(y, m)) d = dim_(y, m);
    iso(y, m, d, out);
}
static long days_between(const char *a, const char *b) { int y, m, d; parse_iso(a, &y, &m, &d); long x = days_from_civil(y, m, d); parse_iso(b, &y, &m, &d); return days_from_civil(y, m, d) - x; }

bool rpt_period(const Report *r, int cmp, SemPeriod *cur, SemPeriod *base, char *label, int ln) {
    memset(cur, 0, sizeof *cur); if (base) memset(base, 0, sizeof *base);
    char asof[11]; if (sem_iso_date_ok(r->as_of)) CP(asof, r->as_of); else rpt_today(asof);
    char end[11]; add_days(asof, 1, end);
    int y, m, d; parse_iso(asof, &y, &m, &d);
    bool pt = !strncmp(r->locale, "pt", 2);
    char asofh[16]; rpt_fmt_date(asof, r->locale, asofh, sizeof asofh);
    int shift_months = 0; long shift_days = 0;
    if (!strcmp(r->period, "ytd")) {
        iso(y, 1, 1, cur->from); CP(cur->to, end); shift_months = 12;
        if (label) snprintf(label, (size_t)ln, pt ? "Ano até %s" : "Year to %s", asofh);
    } else if (!strcmp(r->period, "last_30_days")) {
        add_days(asof, -29, cur->from); CP(cur->to, end); shift_days = 30;
        if (label) snprintf(label, (size_t)ln, pt ? "30 dias até %s" : "30 days to %s", asofh);
    } else if (!strcmp(r->period, "previous_year")) {
        iso(y - 1, 1, 1, cur->from); iso(y, 1, 1, cur->to); shift_months = 12;
        if (label) snprintf(label, (size_t)ln, pt ? "Ano de %d" : "Year %d", y - 1);
    } else if (!strcmp(r->period, "all")) {
        if (label) snprintf(label, (size_t)ln, pt ? "Todo o período" : "All time");
        return cmp == RC_NONE;
    } else if (!strcmp(r->period, "custom") && sem_iso_date_ok(r->from) && sem_iso_date_ok(r->to)) {
        CP(cur->from, r->from); CP(cur->to, r->to); shift_days = days_between(r->from, r->to);
        char a[16], b[16], last[11]; add_days(r->to, -1, last); rpt_fmt_date(r->from, r->locale, a, sizeof a); rpt_fmt_date(last, r->locale, b, sizeof b);
        if (label) snprintf(label, (size_t)ln, pt ? "%s a %s" : "%s to %s", a, b);
    } else {   /* last_12_months (default): the 11 previous months plus the current one up to as_of */
        char first[11]; iso(y, m, 1, first); add_months(first, -11, cur->from); CP(cur->to, end); shift_months = 12;
        if (label) snprintf(label, (size_t)ln, pt ? "12 meses até %s" : "12 months to %s", asofh);
    }
    if (base && cmp == RC_YOY) { add_months(cur->from, -12, base->from); add_months(cur->to, -12, base->to); }
    else if (base && cmp == RC_PREV) {
        if (shift_months) { add_months(cur->from, -shift_months, base->from); add_months(cur->to, -shift_months, base->to); }
        else { add_days(cur->from, -shift_days, base->from); CP(base->to, cur->from); }
    }
    return true;
}

/* ---- formatting ---- */
static void group_digits(const char *digits, char sep, char *out, int n) {
    int L = (int)strlen(digits), w = 0;
    for (int i = 0; i < L && w < n - 2; i++) { if (i && (L - i) % 3 == 0) out[w++] = sep; out[w++] = digits[i]; }
    out[w] = 0;
}
static void plain(double v, int dec, bool pt, char *out, int n) {
    char b[64]; snprintf(b, sizeof b, "%.*f", dec, fabs(v));
    char *dot = strchr(b, '.'); char frac[32] = ""; if (dot) { CP(frac, dot + 1); *dot = 0; }
    char g[96]; group_digits(b, pt ? '.' : ',', g, sizeof g);
    bool neg = v < 0 && (strtod(b, NULL) != 0 || (frac[0] && strtod(frac, NULL) != 0));
    if (frac[0]) snprintf(out, (size_t)n, "%s%s%c%s", neg ? "-" : "", g, pt ? ',' : '.', frac);
    else snprintf(out, (size_t)n, "%s%s", neg ? "-" : "", g);
}
static void compact(double v, int dec, bool pt, char *out, int n) {
    double a = fabs(v); const char *suf = ""; double s = v;
    if (a >= 1e9) { s = v / 1e9; suf = pt ? " bi" : "B"; } else if (a >= 1e6) { s = v / 1e6; suf = pt ? " mi" : "M"; } else if (a >= 1e3) { s = v / 1e3; suf = pt ? " mil" : "K"; }
    if (!*suf) { plain(v, dec < 0 ? (v == floor(v) ? 0 : 1) : dec, pt, out, n); return; }
    char b[64]; plain(s, dec < 0 ? 1 : dec, pt, b, sizeof b);
    snprintf(out, (size_t)n, "%s%s", b, suf);
}
void rpt_fmt_value(double v, bool is_null, const SemMetricDef *m, int fmt, int decimals, const char *locale, char *out, int n) {
    bool pt = !locale || !strncmp(locale, "pt", 2);
    if (is_null || isnan(v)) { snprintf(out, (size_t)n, pt ? "sem dado" : "no data"); return; }
    int unit = m ? m->unit : SU_NUMBER;
    int dec = decimals >= 0 ? decimals : m && m->decimals >= 0 ? m->decimals : -1;
    if (fmt == RF_AUTO) fmt = unit == SU_CURRENCY ? RF_CURRENCY : unit == SU_PERCENT ? RF_PERCENT : fabs(v) >= 1e6 ? RF_COMPACT : RF_NUMBER;
    char b[96];
    switch (fmt) {
    case RF_PERCENT: plain(v * 100.0, dec < 0 ? 1 : dec, pt, b, sizeof b); snprintf(out, (size_t)n, "%s%%", b); return;
    case RF_COMPACT: compact(v, dec, pt, b, sizeof b); break;
    case RF_CURRENCY:
        if (fabs(v) >= 1e6 && decimals < 0) compact(v, 2, pt, b, sizeof b);
        else plain(v, dec < 0 ? 2 : dec, pt, b, sizeof b);
        break;
    default: plain(v, dec < 0 ? (v == floor(v) ? 0 : 2) : dec, pt, b, sizeof b);
    }
    if (fmt == RF_CURRENCY || (unit == SU_CURRENCY && fmt == RF_COMPACT)) {
        const char *cur = m && m->currency[0] ? m->currency : "";
        const char *sym = !strcmp(cur, "BRL") ? "R$" : !strcmp(cur, "USD") ? (pt ? "US$" : "$") : cur;   /* the UI font has no euro sign: the ISO code is shown */
        bool neg = b[0] == '-';
        snprintf(out, (size_t)n, "%s%s%s%s", neg ? "-" : "", sym, *sym ? " " : "", neg ? b + 1 : b);
        return;
    }
    if (unit == SU_DURATION) { snprintf(out, (size_t)n, "%s s", b); return; }
    snprintf(out, (size_t)n, "%s", b);
}
static const char *const MON_PT[] = { "jan", "fev", "mar", "abr", "mai", "jun", "jul", "ago", "set", "out", "nov", "dez" };
static const char *const MON_EN[] = { "Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec" };
void rpt_fmt_date(const char *s, const char *locale, char *out, int n) {
    if (!s || strlen(s) < 10) { snprintf(out, (size_t)n, "%s", s ? s : ""); return; }
    int y, m, d; parse_iso(s, &y, &m, &d);
    if (!locale || !strncmp(locale, "pt", 2)) snprintf(out, (size_t)n, "%02d/%02d/%04d", d, m, y);
    else snprintf(out, (size_t)n, "%02d/%02d/%04d", m, d, y);
}
void rpt_fmt_period(const char *s, int grain, const char *locale, char *out, int n) {
    if (!s || strlen(s) < 10 || !sem_iso_date_ok(s)) { snprintf(out, (size_t)n, "%s", s ? s : ""); return; }
    bool pt = !locale || !strncmp(locale, "pt", 2);
    int y, m, d; parse_iso(s, &y, &m, &d);
    switch (grain) {
    case SG_MONTH: snprintf(out, (size_t)n, pt ? "%s/%04d" : "%s %04d", pt ? MON_PT[m - 1] : MON_EN[m - 1], y); break;
    case SG_QUARTER: snprintf(out, (size_t)n, pt ? "T%d %04d" : "Q%d %04d", (m - 1) / 3 + 1, y); break;
    case SG_YEAR: snprintf(out, (size_t)n, "%04d", y); break;
    default: rpt_fmt_date(s, locale, out, n);
    }
}
void rpt_delta(double cur, bool cur_null, double base, bool base_null, double *abs_, double *rel, bool *abs_ok, bool *rel_ok) {
    *abs_ok = !cur_null && !base_null && !isnan(cur) && !isnan(base);
    *abs_ = *abs_ok ? cur - base : 0;
    *rel_ok = *abs_ok && base != 0;
    *rel = *rel_ok ? (cur - base) / fabs(base) : 0;
}

/* ---- JSON ---- */
static void vis_json(Jw *j, const RptVisual *v) {
    if (v->kind == RK_UNKNOWN && v->raw) { jw_raw(j, v->raw); return; }
    jw_obj(j);
    jw_kstr(j, "id", v->id); jw_kstr(j, "kind", rpt_kind_name(v->kind));
    if (v->title[0]) jw_kstr(j, "title", v->title);
    if (v->subtitle[0]) jw_kstr(j, "subtitle", v->subtitle);
    if (v->metric[0]) { jw_kstr(j, "metric", v->metric); jw_kint(j, "metricVersion", v->metric_version); }
    if (v->dim[0]) jw_kstr(j, "dimension", v->dim);
    if (v->grain >= 0) jw_kstr(j, "grain", sem_grain_name(v->grain));
    if (v->compare) jw_kstr(j, "compare", rpt_compare_name(v->compare));
    if (v->top) jw_kint(j, "top", v->top);
    if (v->fmt || v->decimals >= 0) { jw_key(j, "format"); jw_obj(j); jw_kstr(j, "style", rpt_fmt_name(v->fmt)); if (v->decimals >= 0) jw_kint(j, "decimals", v->decimals); jw_end_obj(j); }
    if (v->text) jw_kstr(j, "text", v->text);
    if (v->sql) { jw_kstr(j, "sql", v->sql); jw_kstr(j, "connection", v->conn); }
    jw_key(j, "layout"); jw_obj(j); jw_kint(j, "x", v->x); jw_kint(j, "y", v->y); jw_kint(j, "w", v->w); jw_kint(j, "h", v->h); if (v->locked) jw_kbool(j, "locked", true); jw_end_obj(j);
    if (v->ai_suggested) jw_kbool(j, "aiSuggested", true);
    if (v->ai_sql) jw_kbool(j, "aiSql", true);
    if (v->sql_edited) jw_kbool(j, "sqlEdited", true);
    if (v->style && v->style[0]) { jw_key(j, "style"); jw_raw(j, v->style); }
    if (v->src[0]) jw_kstr(j, "src", v->src);
    jw_end_obj(j);
}
char *rpt_to_json(const Report *r) {
    Jw j; jw_init(&j);
    jw_obj(&j);
    jw_kint(&j, "formatVersion", RPT_FORMAT); jw_kstr(&j, "reportId", r->id); jw_kint(&j, "revision", r->revision);
    jw_kstr(&j, "title", r->title); jw_kstr(&j, "themeId", r->theme); jw_kstr(&j, "locale", r->locale);
    if (r->nparams) {
        jw_key(&j, "parameters"); jw_arr(&j);
        for (int i = 0; i < r->nparams; i++) {
            const RptParam *pm = &r->params[i];
            jw_obj(&j); jw_kstr(&j, "name", pm->name); jw_kstr(&j, "label", pm->label); jw_kstr(&j, "type", pm->type == RPT_P_TEXT ? "text" : "select");
            if (pm->conn[0]) jw_kstr(&j, "connection", pm->conn);
            if (pm->options_sql[0]) jw_kstr(&j, "optionsSql", pm->options_sql);
            if (pm->value[0]) jw_kstr(&j, "value", pm->value);
            jw_end_obj(&j);
        }
        jw_end_arr(&j);
    }
    if (r->theme_ref[0] || r->theme_snapshot || r->theme_overrides) {
        jw_key(&j, "design"); jw_obj(&j);
        if (r->theme_ref[0]) jw_kstr(&j, "themeRef", r->theme_ref);
        if (r->theme_snapshot) { jw_key(&j, "themeSnapshot"); jw_raw(&j, r->theme_snapshot); }
        if (r->theme_overrides) { jw_key(&j, "themeOverrides"); jw_raw(&j, r->theme_overrides); }
        jw_end_obj(&j);
    }
    jw_kstr(&j, "dataMode", r->data_mode == RM_SNAPSHOT ? "snapshot" : "live"); jw_kstr(&j, "status", r->status == RS_REVIEWED ? "reviewed" : "draft");
    jw_key(&j, "period"); jw_obj(&j); jw_kstr(&j, "preset", r->period);
    if (r->from[0]) jw_kstr(&j, "from", r->from);
    if (r->to[0]) jw_kstr(&j, "to", r->to);
    if (r->as_of[0]) jw_kstr(&j, "asOf", r->as_of);
    jw_end_obj(&j);
    jw_key(&j, "pages"); jw_arr(&j);
    for (int p = 0; p < r->np; p++) {
        const RptPage *pg = &r->pages[p];
        jw_obj(&j); jw_kstr(&j, "id", pg->id); jw_kstr(&j, "name", pg->name); if (pg->theme[0]) jw_kstr(&j, "theme", pg->theme);
        jw_key(&j, "visuals"); jw_arr(&j); for (int i = 0; i < pg->nv; i++) vis_json(&j, &pg->v[i]); jw_end_arr(&j);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    jw_key(&j, "filters"); jw_arr(&j);
    for (int i = 0; i < r->nf; i++) { jw_obj(&j); jw_kstr(&j, "id", r->filters[i].id); jw_kstr(&j, "field", r->filters[i].field); jw_kstr(&j, "scope", r->filters[i].page[0] ? "page" : "report"); if (r->filters[i].page[0]) jw_kstr(&j, "page", r->filters[i].page); jw_kstr(&j, "value", r->filters[i].value); jw_end_obj(&j); }
    jw_end_arr(&j);
    jw_key(&j, "choices"); jw_arr(&j); for (int i = 0; i < r->nchoices; i++) jw_str(&j, r->choices[i]); jw_end_arr(&j);
    jw_key(&j, "provenance"); jw_obj(&j); jw_kstr(&j, "createdBy", r->created_by); jw_kstr(&j, "createdAt", r->created_at); if (r->approved_at[0]) jw_kstr(&j, "approvedAt", r->approved_at); jw_end_obj(&j);
    jw_kint(&j, "nextId", r->next_id);
    jw_end_obj(&j);
    return jw_take(&j);
}

static bool parse_layout(const Json *j, int o, RptVisual *v) {
    int l = jr_get(j, o, "layout"); if (l < 0) return false;
    long long x = 0, y = 0, w = 0, h = 0; bool lk = false;
    if (!jr_int(j, l, "x", &x) || !jr_int(j, l, "y", &y) || !jr_int(j, l, "w", &w) || !jr_int(j, l, "h", &h)) return false;
    jr_bool(j, l, "locked", &lk);
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > RPT_COLS || y > 400 || h > 40) return false;
    v->x = (int)x; v->y = (int)y; v->w = (int)w; v->h = (int)h; v->locked = lk;
    return true;
}
/* one visual from JSON; for_ai: the model's proposal (allowlisted keys, no SQL, no lock changes) */
static const char *g_sql_conn;   /* set by rpt_apply_proposal_ex while a proposal is parsed */
static bool parse_visual(const Json *j, int o, RptVisual *v, bool for_ai, char *why, int n) {
    static const char *const file_keys[] = { "id", "kind", "title", "subtitle", "metric", "metricVersion", "dimension", "grain", "compare", "top", "format", "text", "sql", "connection", "layout", "aiSuggested", "aiSql", "sqlEdited", "accessibility", "style", "src", NULL };
    static const char *const ai_keys[] = { "id", "kind", "title", "subtitle", "metric", "dimension", "grain", "compare", "top", "format", "text", "layout", "accessibility", "style", "src", NULL };
    static const char *const ai_sql_keys[] = { "id", "kind", "title", "subtitle", "metric", "dimension", "grain", "compare", "top", "format", "text", "sql", "layout", "accessibility", "style", "src", NULL };
    char bad[64];
    if (j->nodes[o].type != JN_OBJECT) { snprintf(why, (size_t)n, "a visual must be an object"); return false; }
    char kind[24]; jr_str(j, o, "kind", kind, sizeof kind);
    v->kind = rpt_kind_by_name(kind); CP(v->kind_name, kind);
    if (for_ai && v->kind == RK_UNKNOWN) { snprintf(why, (size_t)n, "visual kind '%.20s' is not supported", kind); return false; }
    if (v->kind != RK_UNKNOWN && !jr_keys_allowed(j, o, for_ai ? (g_sql_conn ? ai_sql_keys : ai_keys) : file_keys, bad, sizeof bad)) { snprintf(why, (size_t)n, "visual field '%.40s' is not allowed%s", bad, for_ai ? " in an AI proposal" : ""); return false; }
    jr_str(j, o, "id", v->id, sizeof v->id);
    char t[200];
    if (jr_str(j, o, "title", t, sizeof t)) { if (strlen(t) >= sizeof v->title) { snprintf(why, (size_t)n, "title too long"); return false; } CP(v->title, t); }
    if (jr_str(j, o, "subtitle", t, sizeof t)) CP(v->subtitle, t);
    jr_str(j, o, "metric", v->metric, sizeof v->metric);
    long long mv = 0; if (!for_ai && jr_int(j, o, "metricVersion", &mv)) v->metric_version = (int)mv;
    jr_str(j, o, "dimension", v->dim, sizeof v->dim);
    char g[24] = "";
    v->grain = -1; if (jr_str(j, o, "grain", g, sizeof g) && g[0]) { v->grain = sem_grain_by_name(g); if (v->grain < 0) { snprintf(why, (size_t)n, "unknown grain '%.20s'", g); return false; } }
    char c[24] = ""; if (jr_str(j, o, "compare", c, sizeof c) && c[0]) { v->compare = rpt_compare_by_name(c); if (v->compare < 0) { snprintf(why, (size_t)n, "unknown comparison '%.20s'", c); return false; } }
    long long top = 0; if (jr_int(j, o, "top", &top)) { if (top < 0 || top > 1000) { snprintf(why, (size_t)n, "top must be 0..1000"); return false; } v->top = (int)top; }
    v->decimals = -1;
    int f = jr_get(j, o, "format");
    if (f >= 0) {
        char st[24]; jr_str(j, f, "style", st, sizeof st);
        if (st[0]) { v->fmt = rpt_fmt_by_name(st); if (v->fmt < 0) { snprintf(why, (size_t)n, "unknown format '%.20s'", st); return false; } }
        long long d; if (jr_int(j, f, "decimals", &d)) { if (d < 0 || d > 6) { snprintf(why, (size_t)n, "decimals must be 0..6"); return false; } v->decimals = (int)d; }
    }
    int stn = jr_get(j, o, "style");
    if (stn >= 0) {
        if (j->nodes[stn].type != JN_OBJECT || j->nodes[stn].val_len > 2000) { snprintf(why, (size_t)n, "style must be a small object"); return false; }
        char sw[300]; if (!rpt_style_check(j->src + j->nodes[stn].val_off, j->nodes[stn].val_len, sw, sizeof sw)) { snprintf(why, (size_t)n, "style: %s", sw); return false; }
        free(v->style); v->style = malloc(j->nodes[stn].val_len + 1); memcpy(v->style, j->src + j->nodes[stn].val_off, j->nodes[stn].val_len); v->style[j->nodes[stn].val_len] = 0;
    }
    jr_str(j, o, "src", v->src, sizeof v->src);
    if (for_ai && v->src[0] && strcmp(v->src, "theme:logo")) { snprintf(why, (size_t)n, "an AI proposal can only place the theme's logo as an image"); return false; }
    char *tx = jr_strdup(j, o, "text"); if (tx) { if (strlen(tx) > RPT_TEXT_MAX) { free(tx); snprintf(why, (size_t)n, "text too long"); return false; } rpt_visual_set_text(v, tx); free(tx); }
    if (for_ai && g_sql_conn && jr_get(j, o, "sql") >= 0) {
        char *q = jr_strdup(j, o, "sql");
        if (!q || !q[0] || strlen(q) > RPT_AI_SQL_MAX) { free(q); snprintf(why, (size_t)n, "the card's SQL is empty or too long"); return false; }
        if (v->kind != RK_KPI && v->kind != RK_LINE && v->kind != RK_BAR && v->kind != RK_TABLE) { free(q); snprintf(why, (size_t)n, "only kpi, line, bar and table cards take SQL"); return false; }
        if (v->metric[0] || v->dim[0] || v->grain >= 0) { free(q); snprintf(why, (size_t)n, "a card with SQL has no metric, dimension or grain"); return false; }
        v->sql = q; snprintf(v->conn, sizeof v->conn, "%s", g_sql_conn); v->ai_sql = true; v->compare = RC_NONE;
    }
    if (!for_ai) {
        v->sql = jr_strdup(j, o, "sql"); jr_str(j, o, "connection", v->conn, sizeof v->conn);
        bool ai = false; jr_bool(j, o, "aiSuggested", &ai); v->ai_suggested = ai;
        bool as = false; jr_bool(j, o, "aiSql", &as); v->ai_sql = as && v->sql;
        bool se = false; jr_bool(j, o, "sqlEdited", &se); v->sql_edited = se && v->sql;
    }
    if (jr_get(j, o, "layout") >= 0 && !parse_layout(j, o, v)) { snprintf(why, (size_t)n, "invalid layout"); return false; }
    if (for_ai && v->locked) v->locked = false;
    if (v->kind == RK_UNKNOWN) {
        size_t L = j->nodes[o].val_len; v->raw = malloc(L + 1); memcpy(v->raw, j->src + j->nodes[o].val_off, L); v->raw[L] = 0;
    }
    return true;
}
bool rpt_parse(Report *r, const char *text, size_t len, bool *migrated, char *why, int n) {
    memset(r, 0, sizeof *r);
    if (migrated) *migrated = false;
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, text, len)) { snprintf(why, (size_t)n, "not a valid report file (JSON error at byte %zu)", j.err_off); json_free(&j); return false; }
    long long fv = -1; jr_int(&j, 0, "formatVersion", &fv);
    if (fv < 0) { snprintf(why, (size_t)n, "not a Lume report (formatVersion missing)"); json_free(&j); return false; }
    if (fv > RPT_FORMAT) { snprintf(why, (size_t)n, "this report was saved by a newer Lume (format %lld, this build reads up to %d): it is not opened, so it cannot be damaged", fv, RPT_FORMAT); json_free(&j); return false; }
    bool v0 = fv == 0;   /* format 0 (preview builds): "cards" and "theme" */
    if (v0 && migrated) *migrated = true;
    r->format_version = RPT_FORMAT; r->next_id = 1;
    jr_str(&j, 0, "reportId", r->id, sizeof r->id);
    long long rev = 1; jr_int(&j, 0, "revision", &rev); r->revision = (int)(rev < 1 ? 1 : rev);
    jr_str(&j, 0, "title", r->title, sizeof r->title);
    if (!jr_str(&j, 0, v0 ? "theme" : "themeId", r->theme, sizeof r->theme) || !r->theme[0]) CP(r->theme, "light");
    if (!jr_str(&j, 0, "locale", r->locale, sizeof r->locale) || !r->locale[0]) CP(r->locale, "pt-BR");
    char b[24]; jr_str(&j, 0, "dataMode", b, sizeof b); r->data_mode = !strcmp(b, "snapshot") ? RM_SNAPSHOT : RM_LIVE;
    jr_str(&j, 0, "status", b, sizeof b); r->status = !strcmp(b, "reviewed") ? RS_REVIEWED : RS_DRAFT;
    int per = jr_get(&j, 0, "period");
    CP(r->period, "last_12_months");
    if (per >= 0) {
        jr_str(&j, per, "preset", r->period, sizeof r->period); if (!r->period[0]) CP(r->period, "last_12_months");
        jr_str(&j, per, "from", r->from, sizeof r->from); jr_str(&j, per, "to", r->to, sizeof r->to); jr_str(&j, per, "asOf", r->as_of, sizeof r->as_of);
        if ((r->from[0] && !sem_iso_date_ok(r->from)) || (r->to[0] && !sem_iso_date_ok(r->to)) || (r->as_of[0] && !sem_iso_date_ok(r->as_of))) { snprintf(why, (size_t)n, "invalid period dates"); json_free(&j); rpt_free(r); return false; }
    }
    int pa2 = jr_get(&j, 0, "parameters");
    for (int i = 0; r->nparams < RPT_MAX_PARAMS; i++) {
        int po = jr_child(&j, pa2, i); if (po < 0) break;
        RptParam *pm = &r->params[r->nparams]; memset(pm, 0, sizeof *pm);
        char ty[16] = ""; jr_str(&j, po, "name", pm->name, sizeof pm->name); jr_str(&j, po, "label", pm->label, sizeof pm->label); jr_str(&j, po, "type", ty, sizeof ty);
        pm->type = !strcmp(ty, "text") ? RPT_P_TEXT : RPT_P_SELECT;
        jr_str(&j, po, "connection", pm->conn, sizeof pm->conn); jr_str(&j, po, "optionsSql", pm->options_sql, sizeof pm->options_sql); jr_str(&j, po, "value", pm->value, sizeof pm->value);
        if (!rpt_param_name_ok(pm->name)) { snprintf(why, (size_t)n, "filter name '%.30s' is not valid", pm->name); json_free(&j); rpt_free(r); return false; }
        r->nparams++;
    }
    int dz = jr_get(&j, 0, "design");
    if (dz >= 0) {
        jr_str(&j, dz, "themeRef", r->theme_ref, sizeof r->theme_ref);
        int sn = jr_get(&j, dz, "themeSnapshot"), ov = jr_get(&j, dz, "themeOverrides");
        if (sn >= 0 && j.nodes[sn].type == JN_OBJECT) { r->theme_snapshot = malloc(j.nodes[sn].val_len + 1); memcpy(r->theme_snapshot, j.src + j.nodes[sn].val_off, j.nodes[sn].val_len); r->theme_snapshot[j.nodes[sn].val_len] = 0; }
        if (ov >= 0 && j.nodes[ov].type == JN_OBJECT) { r->theme_overrides = malloc(j.nodes[ov].val_len + 1); memcpy(r->theme_overrides, j.src + j.nodes[ov].val_off, j.nodes[ov].val_len); r->theme_overrides[j.nodes[ov].val_len] = 0; }
    }
    int pa = jr_get(&j, 0, "pages");
    for (int p = 0; r->np < RPT_MAX_PAGES; p++) {
        int po = jr_child(&j, pa, p); if (po < 0) break;
        RptPage *pg = &r->pages[r->np++]; memset(pg, 0, sizeof *pg);
        jr_str(&j, po, "id", pg->id, sizeof pg->id); jr_str(&j, po, "name", pg->name, sizeof pg->name); jr_str(&j, po, "theme", pg->theme, sizeof pg->theme);
        if (!pg->id[0]) snprintf(pg->id, sizeof pg->id, "p%d", r->np);
        int va = jr_get(&j, po, v0 ? "cards" : "visuals");
        for (int i = 0;; i++) {
            int vo = jr_child(&j, va, i); if (vo < 0) break;
            RptVisual *v = push_visual(pg);
            if (!v || !parse_visual(&j, vo, v, false, why, n)) { json_free(&j); rpt_free(r); return false; }
            if (!v->id[0] || rpt_find(r, v->id, NULL) != v) { snprintf(why, (size_t)n, "visual ids must be present and unique"); json_free(&j); rpt_free(r); return false; }
            int k = atoi(v->id + 1); if (v->id[0] == 'v' && k >= r->next_id) r->next_id = k + 1;
        }
    }
    if (!r->np) { r->np = 1; page_init(&r->pages[0], 1, "Page 1"); }
    int fa = jr_get(&j, 0, "filters");
    for (int i = 0; r->nf < RPT_MAX_FILTERS; i++) {
        int fo = jr_child(&j, fa, i); if (fo < 0) break;
        RptFilter *f = &r->filters[r->nf++];
        jr_str(&j, fo, "id", f->id, sizeof f->id); jr_str(&j, fo, "field", f->field, sizeof f->field); jr_str(&j, fo, "page", f->page, sizeof f->page); jr_str(&j, fo, "value", f->value, sizeof f->value);
    }
    int ca = jr_get(&j, 0, "choices");
    for (int i = 0; r->nchoices < 4; i++) { int c = jr_child(&j, ca, i); if (c < 0) break; jr_node_str(&j, c, r->choices[r->nchoices++], sizeof r->choices[0]); }
    int pv = jr_get(&j, 0, "provenance");
    if (pv >= 0) { jr_str(&j, pv, "createdBy", r->created_by, sizeof r->created_by); jr_str(&j, pv, "createdAt", r->created_at, sizeof r->created_at); jr_str(&j, pv, "approvedAt", r->approved_at, sizeof r->approved_at); }
    long long ni = 0; if (jr_int(&j, 0, "nextId", &ni) && ni > r->next_id) r->next_id = (int)ni;
    json_free(&j);
    if (!r->id[0]) snprintf(r->id, sizeof r->id, "rpt-imported");
    return true;
}
bool rpt_save_atomic(const Report *r, const char *path, char *why, int n) {
    char *s = rpt_to_json(r);
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(s); snprintf(why, (size_t)n, "cannot write the report next to %s", path); return false; }
    size_t L = strlen(s); bool ok = fwrite(s, 1, L, f) == L;
    ok = (fflush(f) == 0) && ok; ok = (fsync(fileno(f)) == 0) && ok; ok = (fclose(f) == 0) && ok;
    free(s);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)n, "the report could not be saved (disk full or no permission); the previous file is unchanged"); return false; }
    return true;
}
bool rpt_load(Report *r, const char *path, bool *migrated, char *why, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, (size_t)n, "cannot open %s", path); return false; }
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    if (L < 0 || L > 32 * 1024 * 1024) { fclose(f); snprintf(why, (size_t)n, "report file too large"); return false; }
    char *b = malloc((size_t)L + 1); size_t got = fread(b, 1, (size_t)L, f); fclose(f); b[got] = 0;
    bool ok = rpt_parse(r, b, got, migrated, why, n);
    free(b);
    return ok;
}

/* ---- AI proposals ---- */
static bool apply_layout(Report *r, int page, RptVisual *v, const Json *j, int lo, char *why, int n) {
    long long x, y, w, h;
    if (!jr_int(j, lo, "x", &x) || !jr_int(j, lo, "y", &y) || !jr_int(j, lo, "w", &w) || !jr_int(j, lo, "h", &h)) { snprintf(why, (size_t)n, "layout needs x, y, w, h"); return false; }
    if (x < 0 || y < 0 || w < 1 || h < 1 || x + w > RPT_COLS || y > 400 || h > 40) { snprintf(why, (size_t)n, "layout outside the 12-column grid"); return false; }
    if (v->locked) { snprintf(why, (size_t)n, "card %s is locked: unlock it to move it", v->id); return false; }
    if (!rpt_place(r, page, v->id, (int)x, (int)y, (int)w, (int)h)) { snprintf(why, (size_t)n, "card %s cannot go there (a locked card is in the way)", v->id); return false; }
    return true;
}
static bool apply_proposal(Report *r, const SemCatalog *c, const char *text, size_t len, int page, const char *scope, bool allow_global, RptApply *out, char *why, int n);
bool rpt_apply_proposal(Report *r, const SemCatalog *c, const char *text, size_t len, int page, const char *scope, bool allow_global, RptApply *out, char *why, int n) {
    g_sql_conn = NULL;
    return apply_proposal(r, c, text, len, page, scope, allow_global, out, why, n);
}
bool rpt_apply_proposal_ex(Report *r, const SemCatalog *c, const char *text, size_t len, int page, const char *scope, bool allow_global, const char *sql_conn, RptApply *out, char *why, int n) {
    g_sql_conn = sql_conn && sql_conn[0] ? sql_conn : NULL;
    bool ok = apply_proposal(r, c, text, len, page, scope, allow_global, out, why, n);
    g_sql_conn = NULL;
    return ok;
}
static bool apply_proposal(Report *r, const SemCatalog *c, const char *text, size_t len, int page, const char *scope, bool allow_global, RptApply *out, char *why, int n) {
    memset(out, 0, sizeof *out);
    if (page < 0 || page >= r->np) { snprintf(why, (size_t)n, "no such page"); return false; }
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, text, len)) { snprintf(why, (size_t)n, "the proposal is not valid JSON (byte %zu)", j.err_off); json_free(&j); return false; }
    static const char *const top_keys[] = { "proposalId", "baseReportRevision", "summary", "rationale", "actions", "requiresApproval", "type", NULL };
    char bad[64];
    if (!jr_keys_allowed(&j, 0, top_keys, bad, sizeof bad)) { snprintf(why, (size_t)n, "proposal field '%.40s' is not allowed", bad); json_free(&j); return false; }
    long long base = -1;
    if (!jr_int(&j, 0, "baseReportRevision", &base)) { snprintf(why, (size_t)n, "the proposal has no base revision"); json_free(&j); return false; }
    if (base != r->revision) { snprintf(why, (size_t)n, "conflict: the proposal was made for revision %lld but the report is at revision %d; nothing was overwritten (ask again)", base, r->revision); json_free(&j); return false; }
    jr_str(&j, 0, "summary", out->summary, sizeof out->summary);
    int acts = jr_get(&j, 0, "actions");
    if (acts < 0 || j.nodes[acts].type != JN_ARRAY || !j.nodes[acts].nchildren) { snprintf(why, (size_t)n, "the proposal has no actions"); json_free(&j); return false; }
    if (j.nodes[acts].nchildren > 24) { snprintf(why, (size_t)n, "too many actions in one proposal (24 max)"); json_free(&j); return false; }
    bool scoped = scope && scope[0];
    Report t; rpt_copy(&t, r);
    bool ok = true;
    for (int i = 0; ok; i++) {
        int a = jr_child(&j, acts, i); if (a < 0) break;
        char type[32]; jr_str(&j, a, "type", type, sizeof type);
        char vid[24]; jr_str(&j, a, "visualId", vid, sizeof vid);
        out->actions++;
        if (!strcmp(type, "add_visual")) {
            static const char *const k[] = { "type", "pageId", "visual", NULL };
            if (!jr_keys_allowed(&j, a, k, bad, sizeof bad)) { snprintf(why, (size_t)n, "add_visual: field '%.40s' is not allowed", bad); ok = false; break; }
            if (scoped && !allow_global) { snprintf(why, (size_t)n, "the proposal adds a card, but the request was about card %s only (allow page changes to accept it)", scope); ok = false; break; }
            int vo = jr_get(&j, a, "visual");
            RptVisual tmp; memset(&tmp, 0, sizeof tmp);
            if (vo < 0 || !parse_visual(&j, vo, &tmp, true, why, n)) { vis_free(&tmp); if (vo < 0) snprintf(why, (size_t)n, "add_visual without a visual"); ok = false; break; }
            RptVisual *v = rpt_add_visual(&t, page, tmp.kind);
            char id[24]; CP(id, v->id);
            int x = v->x, y = v->y, w = v->w, h = v->h;
            *v = tmp; CP(v->id, id); CP(v->kind_name, rpt_kind_name(v->kind));
            if (jr_get(&j, vo, "layout") < 0) { v->x = x; v->y = y; v->w = w; v->h = h; }
            else { int lx = v->x, ly = v->y, lw = v->w, lh = v->h; v->x = x; v->y = y; v->w = w; v->h = h; if (!rpt_place(&t, page, v->id, lx, ly, lw, lh)) { snprintf(why, (size_t)n, "add_visual: layout conflicts with a locked card"); ok = false; break; } v = rpt_find(&t, id, NULL); }
            if (v->kind == RK_TEXT) v->ai_suggested = true;
            const SemMetric *m = sem_metric_c(c, v->metric); if (m) v->metric_version = sem_cur(m)->version;
            if (!rpt_visual_check(&t, v, c, why, n)) { ok = false; break; }
            out->added++; if (out->nchanged < 8) CP(out->changed[out->nchanged++], v->id);
        } else if (!strcmp(type, "update_visual")) {
            static const char *const k[] = { "type", "visualId", "patch", NULL };
            static const char *const pk0[] = { "kind", "title", "subtitle", "metric", "dimension", "grain", "compare", "top", "format", "style", NULL };
            static const char *const pk1[] = { "kind", "title", "subtitle", "metric", "dimension", "grain", "compare", "top", "format", "sql", "style", NULL };
            const char *const *pk = g_sql_conn ? pk1 : pk0;
            if (!jr_keys_allowed(&j, a, k, bad, sizeof bad)) { snprintf(why, (size_t)n, "update_visual: field '%.40s' is not allowed", bad); ok = false; break; }
            int pg; RptVisual *v = rpt_find(&t, vid, &pg);
            if (!v || pg != page) { snprintf(why, (size_t)n, "update_visual: card '%.20s' is not on this page", vid); ok = false; break; }
            if (scoped && strcmp(vid, scope) && !allow_global) { snprintf(why, (size_t)n, "the proposal changes card %s, but the request was about card %s only", vid, scope); ok = false; break; }
            if (v->locked) { snprintf(why, (size_t)n, "card %s is locked", vid); ok = false; break; }
            int po = jr_get(&j, a, "patch");
            if (po < 0 || !jr_keys_allowed(&j, po, pk, bad, sizeof bad)) { snprintf(why, (size_t)n, "update_visual: patch field '%.40s' is not allowed", po < 0 ? "(none)" : bad); ok = false; break; }
            char s[200];
            if (jr_str(&j, po, "kind", s, sizeof s)) { int kk = rpt_kind_by_name(s); if (kk == RK_UNKNOWN) { snprintf(why, (size_t)n, "kind '%.20s' is not supported", s); ok = false; break; } v->kind = kk; CP(v->kind_name, s); }
            if (jr_str(&j, po, "title", s, sizeof s)) { if (strlen(s) >= sizeof v->title) { snprintf(why, (size_t)n, "title too long"); ok = false; break; } CP(v->title, s); }
            if (jr_str(&j, po, "subtitle", s, sizeof s)) CP(v->subtitle, s);
            { int stn = jr_get(&j, po, "style");
              if (stn >= 0) {
                  char sw[300];
                  if (j.nodes[stn].type == JN_NULL) { free(v->style); v->style = NULL; }
                  else if (j.nodes[stn].type != JN_OBJECT || j.nodes[stn].val_len > 2000 || !rpt_style_check(j.src + j.nodes[stn].val_off, j.nodes[stn].val_len, sw, sizeof sw)) { snprintf(why, (size_t)n, "update_visual: bad style%s%s", j.nodes[stn].type == JN_OBJECT ? ": " : "", j.nodes[stn].type == JN_OBJECT ? sw : ""); ok = false; break; }
                  else { free(v->style); v->style = malloc(j.nodes[stn].val_len + 1); memcpy(v->style, j.src + j.nodes[stn].val_off, j.nodes[stn].val_len); v->style[j.nodes[stn].val_len] = 0; }
              } }
            if (jr_str(&j, po, "metric", s, sizeof s) && strcmp(s, v->metric)) { const SemMetric *m = sem_metric_c(c, s); CP(v->metric, s); v->metric_version = m ? sem_cur(m)->version : 0; out->metric_changed = true; }
            int dn = jr_get(&j, po, "dimension"); if (dn >= 0) { if (j.nodes[dn].type == JN_NULL) v->dim[0] = 0; else jr_node_str(&j, dn, v->dim, sizeof v->dim); }
            int gn = jr_get(&j, po, "grain"); if (gn >= 0) { if (j.nodes[gn].type == JN_NULL) v->grain = -1; else { jr_node_str(&j, gn, s, sizeof s); v->grain = sem_grain_by_name(s); if (v->grain < 0) { snprintf(why, (size_t)n, "unknown grain"); ok = false; break; } } }
            if (g_sql_conn && jr_get(&j, po, "sql") >= 0) {
                char *q = jr_strdup(&j, po, "sql");
                if (!q || !q[0] || strlen(q) > RPT_AI_SQL_MAX) { free(q); snprintf(why, (size_t)n, "the card's SQL is empty or too long"); ok = false; break; }
                free(v->sql); v->sql = q; snprintf(v->conn, sizeof v->conn, "%s", g_sql_conn); v->ai_sql = true; v->metric[0] = 0; v->metric_version = 0; v->dim[0] = 0; v->grain = -1; v->compare = RC_NONE;
            }
            if (jr_str(&j, po, "compare", s, sizeof s)) { int cc = rpt_compare_by_name(s); if (cc < 0) { snprintf(why, (size_t)n, "unknown comparison"); ok = false; break; } v->compare = cc; }
            long long top; if (jr_int(&j, po, "top", &top)) { if (top < 0 || top > 1000) { snprintf(why, (size_t)n, "top must be 0..1000"); ok = false; break; } v->top = (int)top; }
            int fo = jr_get(&j, po, "format");
            if (fo >= 0) { if (jr_str(&j, fo, "style", s, sizeof s)) { int ff = rpt_fmt_by_name(s); if (ff < 0) { snprintf(why, (size_t)n, "unknown format"); ok = false; break; } v->fmt = ff; } long long d; if (jr_int(&j, fo, "decimals", &d)) { if (d < 0 || d > 6) { snprintf(why, (size_t)n, "decimals 0..6"); ok = false; break; } v->decimals = (int)d; } }
            /* switching kind keeps the data contract when it can: a line becomes bars over the same grain */
            if (v->kind == RK_KPI) { v->grain = -1; v->dim[0] = 0; }
            if (v->kind != RK_KPI || v->sql) v->compare = RC_NONE;
            if (v->sql && v->metric[0]) { free(v->sql); v->sql = NULL; v->conn[0] = 0; v->ai_sql = false; }   /* moved back to a catalog metric */
            if (!rpt_visual_check(&t, v, c, why, n)) { ok = false; break; }
            out->updated++; if (out->nchanged < 8) CP(out->changed[out->nchanged++], v->id);
        } else if (!strcmp(type, "set_theme")) {
            /* the look of the report: theme changes kept as overrides (colours, type, cards, header) */
            static const char *const k[] = { "type", "theme", NULL };
            int to = jr_get(&j, a, "theme");
            if (!jr_keys_allowed(&j, a, k, bad, sizeof bad) || to < 0 || j.nodes[to].type != JN_OBJECT || j.nodes[to].val_len > 4000) { snprintf(why, (size_t)n, "set_theme: needs a \"theme\" object"); ok = false; break; }
            static const char *const tk[] = { "colors", "typography", "shape", "header", NULL };
            if (!jr_keys_allowed(&j, to, tk, bad, sizeof bad)) { snprintf(why, (size_t)n, "set_theme: '%.30s' is not a theme field (colors, typography, shape, header)", bad); ok = false; break; }
            free(out->theme_json); out->theme_json = malloc(j.nodes[to].val_len + 1); memcpy(out->theme_json, j.src + j.nodes[to].val_off, j.nodes[to].val_len); out->theme_json[j.nodes[to].val_len] = 0;
        } else if (!strcmp(type, "add_filter") || !strcmp(type, "remove_filter")) {
            /* report filters: only with a shared connection (their options come from a query on it) */
            if (!g_sql_conn) { snprintf(why, (size_t)n, "%s: filters need the database structure to be shared", type); ok = false; break; }
            if (!strcmp(type, "remove_filter")) {
                static const char *const k[] = { "type", "name", NULL };
                if (!jr_keys_allowed(&j, a, k, bad, sizeof bad)) { snprintf(why, (size_t)n, "remove_filter: field '%.40s' is not allowed", bad); ok = false; break; }
                char nm[40]; jr_str(&j, a, "name", nm, sizeof nm);
                int at = -1; for (int q = 0; q < t.nparams; q++) if (!strcmp(t.params[q].name, nm)) at = q;
                if (at < 0) { snprintf(why, (size_t)n, "remove_filter: no filter '%.30s'", nm); ok = false; break; }
                memmove(&t.params[at], &t.params[at + 1], sizeof t.params[0] * (size_t)(t.nparams - at - 1)); t.nparams--;
                out->filters++;
            } else {
                static const char *const k[] = { "type", "filter", NULL };
                static const char *const fk[] = { "name", "label", "type", "optionsSql", "value", NULL };
                int fo = jr_get(&j, a, "filter");
                if (!jr_keys_allowed(&j, a, k, bad, sizeof bad) || fo < 0 || !jr_keys_allowed(&j, fo, fk, bad, sizeof bad)) { snprintf(why, (size_t)n, "add_filter: field '%.40s' is not allowed", fo < 0 ? "(none)" : bad); ok = false; break; }
                if (t.nparams >= RPT_MAX_PARAMS) { snprintf(why, (size_t)n, "add_filter: at most %d filters", RPT_MAX_PARAMS); ok = false; break; }
                RptParam pm; memset(&pm, 0, sizeof pm);
                char ty[16] = ""; jr_str(&j, fo, "name", pm.name, sizeof pm.name); jr_str(&j, fo, "label", pm.label, sizeof pm.label); jr_str(&j, fo, "type", ty, sizeof ty);
                jr_str(&j, fo, "optionsSql", pm.options_sql, sizeof pm.options_sql); jr_str(&j, fo, "value", pm.value, sizeof pm.value);
                if (!rpt_param_name_ok(pm.name)) { snprintf(why, (size_t)n, "add_filter: '%.30s' is not a valid name (lower-case letters, digits, _; not period_start / period_end)", pm.name); ok = false; break; }
                bool dup = false; for (int q = 0; q < t.nparams; q++) if (!strcmp(t.params[q].name, pm.name)) dup = true;
                if (dup) { snprintf(why, (size_t)n, "add_filter: '%.30s' already exists", pm.name); ok = false; break; }
                if (!pm.label[0]) snprintf(pm.label, sizeof pm.label, "%s", pm.name);
                pm.type = !strcmp(ty, "text") ? RPT_P_TEXT : RPT_P_SELECT;
                if (pm.type == RPT_P_SELECT && !pm.options_sql[0]) { snprintf(why, (size_t)n, "add_filter: a select filter needs optionsSql"); ok = false; break; }
                snprintf(pm.conn, sizeof pm.conn, "%s", g_sql_conn);
                t.params[t.nparams++] = pm;
                out->filters++; if (out->nfilter_names < RPT_MAX_PARAMS) snprintf(out->filter_names[out->nfilter_names++], 32, "%s", pm.name);
            }
        } else if (!strcmp(type, "move_visual")) {
            static const char *const k[] = { "type", "visualId", "layout", NULL };
            if (!jr_keys_allowed(&j, a, k, bad, sizeof bad)) { snprintf(why, (size_t)n, "move_visual: field '%.40s' is not allowed", bad); ok = false; break; }
            int pg; RptVisual *v = rpt_find(&t, vid, &pg);
            if (!v || pg != page) { snprintf(why, (size_t)n, "move_visual: card '%.20s' is not on this page", vid); ok = false; break; }
            if (scoped && strcmp(vid, scope) && !allow_global) { snprintf(why, (size_t)n, "the proposal moves card %s, but the request was about card %s only", vid, scope); ok = false; break; }
            int lo = jr_get(&j, a, "layout");
            if (lo < 0 || !apply_layout(&t, page, v, &j, lo, why, n)) { if (lo < 0) snprintf(why, (size_t)n, "move_visual without a layout"); ok = false; break; }
            out->moved++; if (out->nchanged < 8) CP(out->changed[out->nchanged++], vid);
        } else if (!strcmp(type, "update_narrative")) {
            static const char *const k[] = { "type", "visualId", "text", "evidenceRefs", NULL };
            if (!jr_keys_allowed(&j, a, k, bad, sizeof bad)) { snprintf(why, (size_t)n, "update_narrative: field '%.40s' is not allowed", bad); ok = false; break; }
            int pg; RptVisual *v = rpt_find(&t, vid, &pg);
            if (!v || pg != page || v->kind != RK_TEXT) { snprintf(why, (size_t)n, "update_narrative: '%.20s' is not a text card on this page", vid); ok = false; break; }
            if (scoped && strcmp(vid, scope) && !allow_global) { snprintf(why, (size_t)n, "the proposal rewrites card %s, but the request was about card %s only", vid, scope); ok = false; break; }
            /* evidence must point at cards that exist (no invented citations, LI-088) */
            int ev = jr_get(&j, a, "evidenceRefs");
            for (int e = 0; ev >= 0; e++) { int en = jr_child(&j, ev, e); if (en < 0) break; char rid[24]; jr_node_str(&j, en, rid, sizeof rid); if (!rpt_find(&t, rid, NULL)) { snprintf(why, (size_t)n, "the narrative cites card '%.20s', which does not exist", rid); ok = false; break; } }
            if (!ok) break;
            char *tx = jr_strdup(&j, a, "text");
            if (!tx || strlen(tx) > RPT_TEXT_MAX) { free(tx); snprintf(why, (size_t)n, "narrative text missing or too long"); ok = false; break; }
            rpt_visual_set_text(v, tx); free(tx); v->ai_suggested = true;
            out->narratives++; if (out->nchanged < 8) CP(out->changed[out->nchanged++], vid);
        } else {
            snprintf(why, (size_t)n, "action '%.30s' is not allowed (allowed: add_visual, update_visual, move_visual, update_narrative)", type);
            ok = false;
        }
    }
    json_free(&j);
    if (!ok) { rpt_free(&t); return false; }
    t.revision = r->revision + 1;
    rpt_free(r); *r = t;
    return true;
}

/* ---- undo / redo ---- */
static void stack_push(char **s, int *n, char *x) { if (*n == RPT_UNDO) { free(s[0]); memmove(s, s + 1, sizeof *s * (RPT_UNDO - 1)); (*n)--; } s[(*n)++] = x; }
void rpt_hist_push(RptHist *h, const Report *r) {
    stack_push(h->u, &h->nu, rpt_to_json(r));
    for (int i = 0; i < h->nr; i++) free(h->rd[i]);
    h->nr = 0;
}
static bool restore(Report *r, const char *json) {
    Report t; char why[200]; bool mig;
    if (!rpt_parse(&t, json, strlen(json), &mig, why, sizeof why)) return false;
    int rev = r->revision;
    rpt_free(r); *r = t;
    r->revision = rev + 1;   /* undo is a change too: proposals made before it are stale */
    return true;
}
bool rpt_undo(RptHist *h, Report *r) {
    if (!h->nu) return false;
    char *cur = rpt_to_json(r);
    char *prev = h->u[--h->nu];
    bool ok = restore(r, prev);
    free(prev);
    if (ok) stack_push(h->rd, &h->nr, cur); else free(cur);
    return ok;
}
bool rpt_redo(RptHist *h, Report *r) {
    if (!h->nr) return false;
    char *cur = rpt_to_json(r);
    char *next = h->rd[--h->nr];
    bool ok = restore(r, next);
    free(next);
    if (ok) stack_push(h->u, &h->nu, cur); else free(cur);
    return ok;
}
void rpt_hist_free(RptHist *h) { for (int i = 0; i < h->nu; i++) free(h->u[i]); for (int i = 0; i < h->nr; i++) free(h->rd[i]); memset(h, 0, sizeof *h); }
