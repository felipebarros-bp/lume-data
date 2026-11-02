/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Query History panel (toolbar spec TBR-034)
 * ==========================================
 *
 * A dock panel ("lume.data.history") added through the panel extension API (panels.h) like any
 * module: it lists the executions the Output appends to <prefs>/history.tsv, newest first, and
 * filters them by text, connection, status and time.
 *
 * Privacy: the history file holds the SQL as the Output recorded it: passwords and secret-like
 * values already redacted, parameter values never (runs with parameters store the template).
 *
 * Actions on the selected entry:
 *   Open      a new query tab with that SQL, bound to the entry's connection (nothing runs)
 *   Copy      the SQL to the clipboard
 *   Re-run    the same as Open, then Run All in File through the command registry: the toolbar's
 *             safety rules apply (PROD confirmation, read-only, the captured target)
 *
 * The file is read when the panel becomes visible and re-read (if it changed) at most once a
 * second while it stays visible; a hidden panel costs nothing (visibility hook).
 */
#include "history_panel.h"
#include <strings.h>
#include <sys/stat.h>
#include <time.h>
#include "commands.h"
#include "conn.h"
#include "draw.h"
#include "font.h"
#include "log.h"
#include "theme.h"
#include "ui.h"
#include "units.h"

#define HP_MAX 500
typedef struct HpEntry { long long when; char session[64]; char status[16]; double ms; int nstmts; bool redacted; char *sql; } HpEntry;

typedef struct HistoryView {
    HpEntry e[HP_MAX]; int n;
    time_t mtime; off_t size; Uint64 checked;
    bool shown;
    TextField search;
    int conn_filter;     /* 0 all, k = k-th distinct session name */
    int status_filter;   /* 0 all, 1 ok, 2 failed, 3 cancelled */
    int time_filter;     /* 0 all, 1 last hour, 2 today */
    int sel;             /* index into e[] of the selected entry, -1 none */
    float scroll;
    ScrollBar sb;
    int rows[HP_MAX]; int nrows;
} HistoryView;

/* Undoes the Output's TSV escaping (\t, \n, \\). */
static void unescape(const char *in, char *out, size_t n) {
    size_t o = 0;
    for (; *in && o + 1 < n; in++) {
        if (*in == '\\' && in[1]) { in++; out[o++] = *in == 't' ? '\t' : *in == 'n' ? '\n' : *in; }
        else out[o++] = *in;
    }
    out[o] = 0;
}

static void clear(HistoryView *v) { for (int i = 0; i < v->n; i++) free(v->e[i].sql); v->n = 0; }

/* Reads history.tsv (newest first); keeps the last HP_MAX entries. */
static void load(HistoryView *v, App *a) {
    char path[1100]; snprintf(path, sizeof path, "%shistory.tsv", a->conns ? a->conns->dir : "");
    struct stat st;
    if (stat(path, &st) != 0) { clear(v); return; }
    if (st.st_mtime == v->mtime && st.st_size == v->size && v->n) return;
    v->mtime = st.st_mtime; v->size = st.st_size;
    clear(v);
    FILE *f = fopen(path, "r");
    if (!f) return;
    static char line[40000], sql[40000];
    HpEntry ring[HP_MAX]; int nr = 0, head = 0;
    while (fgets(line, sizeof line, f)) {
        if (line[0] == '#') continue;
        line[strcspn(line, "\n")] = 0;
        char *fld[6]; int k = 0; char *p = line;
        while (k < 6 && p) { fld[k++] = p; p = k < 6 ? strchr(p, '\t') : NULL; if (p) *p++ = 0; }
        if (k < 6) continue;
        HpEntry e; memset(&e, 0, sizeof e);
        e.when = atoll(fld[0]); snprintf(e.session, sizeof e.session, "%s", fld[1]); snprintf(e.status, sizeof e.status, "%s", fld[2]);
        e.ms = atof(fld[3]); e.nstmts = atoi(fld[4]);
        const char *s = fld[5]; if (*s == '\x01') { e.redacted = true; s++; }
        unescape(s, sql, sizeof sql); e.sql = strdup(sql);
        if (nr == HP_MAX) { free(ring[head].sql); ring[head] = e; head = (head + 1) % HP_MAX; }
        else ring[nr++] = e;
    }
    fclose(f);
    /* newest first */
    for (int i = 0; i < nr; i++) v->e[i] = ring[(head + nr - 1 - i) % (nr ? nr : 1)];
    v->n = nr;
    if (v->sel >= v->n) v->sel = -1;
}

/* Distinct session (connection) names, in first-seen order; k-th = filter k + 1. */
static int sessions(HistoryView *v, char out[][64], int max) {
    int n = 0;
    for (int i = 0; i < v->n && n < max; i++) {
        bool dup = false; for (int k = 0; k < n; k++) if (!strcmp(out[k], v->e[i].session)) dup = true;
        if (!dup) snprintf(out[n++], 64, "%s", v->e[i].session);
    }
    return n;
}

static bool matches(HistoryView *v, const HpEntry *e, const char *conn) {
    if (conn && strcmp(e->session, conn)) return false;
    if (v->status_filter == 1 && strcmp(e->status, "completed")) return false;
    if (v->status_filter == 2 && strcmp(e->status, "failed")) return false;
    if (v->status_filter == 3 && strcmp(e->status, "cancelled")) return false;
    time_t now = time(NULL);
    if (v->time_filter == 1 && now - e->when > 3600) return false;
    if (v->time_filter == 2) { struct tm a1, b1; time_t w = (time_t)e->when; localtime_r(&now, &a1); localtime_r(&w, &b1); if (a1.tm_yday != b1.tm_yday || a1.tm_year != b1.tm_year) return false; }
    if (v->search.len && !strcasestr(e->sql, v->search.buf) && !strcasestr(e->session, v->search.buf)) return false;
    return true;
}

static Connection *conn_named(App *a, const char *name) {
    for (int i = 0; a->conns && i < a->conns->n; i++) if (!strcmp(a->conns->items[i]->name, name)) return a->conns->items[i];
    return NULL;
}

/* Opens the entry in a new query tab bound to its connection; with run, starts Run All through
 * the registry (every safety rule applies). */
static void open_entry(HistoryView *v, App *a, int i, bool run) {
    const HpEntry *e = &v->e[i];
    if (e->redacted && run) { app_status(a, "This entry was stored with secrets masked: open it and fill them in before running"); run = false; }
    Connection *c = conn_named(a, e->session);
    char title[48]; snprintf(title, sizeof title, "history %s", e->session);
    int id = panels_open_tab(a->panels, a, title, c ? c->id : "", e->sql);
    if (id >= 0) panels_activate_tab(a->panels, a, id);
    if (!c) app_status(a, "Opened in a new tab; its connection \"%s\" no longer exists: choose one in the toolbar", e->session);
    else if (run) commands_invoke(a, "sql.run_file");
    else app_status(a, "Opened in a new tab on %s (not run)", c->name);
}

static void *hp_create(void *ud, void *ctx) { (void)ud; (void)ctx; HistoryView *v = calloc(1, sizeof *v); v->sel = -1; return v; }
static void hp_destroy(void *view, void *ctx) { (void)ctx; HistoryView *v = view; clear(v); free(v); }
static void hp_visibility(void *view, void *ctx, bool shown) { HistoryView *v = view; v->shown = shown; if (shown) { v->mtime = 0; load(v, ctx); } }

static void hp_draw(void *view, void *ctx, DockRect drc, bool focused) {
    (void)focused;
    HistoryView *v = view; App *a = ctx;
    SDL_Renderer *r = a->renderer; Input *in = &a->in;
    Rect rc = rect_make(drc.x, drc.y, drc.w, drc.h);
    if (SDL_GetTicks() - v->checked > 1000) { v->checked = SDL_GetTicks(); load(v, a); }
    app_wake_in(a, 1000);   /* only while drawn: picks up new executions */
    draw_rect(r, rc, color_hex(T_PANEL));
    float pad = px(a, 8), y = rc.y + pad;
    /* filters: text, status, time, connection */
    ui_textfield(a, &v->search, rect_make(rc.x + pad, y, fminf(px(a, 240), rc.w - 2 * pad), px(a, 26)), "Filter SQL or connection", false);
    float fx = rc.x + pad + px(a, 250);
    static const char *const st[] = { "All", "OK", "Failed", "Cancelled" };
    static const char *const tm[] = { "Any time", "Last hour", "Today" };
    if (rc.w > px(a, 640)) {
        int p = ui_segmented(a, rect_make(fx, y, px(a, 230), px(a, 26)), st, 4, v->status_filter); if (p >= 0) v->status_filter = p;
        p = ui_segmented(a, rect_make(fx + px(a, 238), y, px(a, 220), px(a, 26)), tm, 3, v->time_filter); if (p >= 0) v->time_filter = p;
    }
    char names[16][64]; int nn = sessions(v, names, 16);
    if (v->conn_filter > nn) v->conn_filter = 0;
    char cl[96]; snprintf(cl, sizeof cl, "Connection: %s", v->conn_filter ? names[v->conn_filter - 1] : "all");
    if (ui_button(a, rect_make(rc.x + rc.w - pad - px(a, 200), y, px(a, 200), px(a, 26)), cl, BTN_QUIET, nn > 0)) v->conn_filter = (v->conn_filter + 1) % (nn + 1);
    y += px(a, 34);
    /* rows */
    v->nrows = 0;
    const char *conn = v->conn_filter ? names[v->conn_filter - 1] : NULL;
    for (int i = 0; i < v->n; i++) if (matches(v, &v->e[i], conn)) v->rows[v->nrows++] = i;
    float rh = px(a, 22), foot = px(a, 34);
    Rect list = rect_make(rc.x, y, rc.w, rc.y + rc.h - y - foot);
    if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { v->scroll -= in->wheel_y; in->wheel_y = 0; }
    float max = v->nrows * rh - list.h; if (v->scroll > max) v->scroll = max; if (v->scroll < 0) v->scroll = 0;
    ui_scrollbar_input(a, &v->sb, list, v->nrows * rh, &v->scroll);
    clip_push(r, list);
    for (int k = 0; k < v->nrows; k++) {
        float ry = list.y + k * rh - v->scroll;
        if (ry + rh < list.y || ry > list.y + list.h) continue;
        HpEntry *e = &v->e[v->rows[k]];
        Rect rr = rect_make(rc.x + px(a, 4), ry, rc.w - px(a, 8), rh);
        bool sel = v->sel == v->rows[k];
        if (sel) draw_rrect(r, rr, px(a, 3), color_hex(T_SELECTED));
        if (rect_contains(rr, in->mouse_x, in->mouse_y) && rect_contains(list, in->mouse_x, in->mouse_y) && in->mouse_pressed && !in->mouse_consumed) {
            in->mouse_consumed = true; v->sel = v->rows[k];
            if (in->clicks >= 2) open_entry(v, a, v->sel, false);
        }
        char when[32]; time_t w = (time_t)e->when; struct tm tmv; localtime_r(&w, &tmv); strftime(when, sizeof when, "%d/%m %H:%M:%S", &tmv);
        char dur[24]; fmt_duration(dur, sizeof dur, e->ms);
        uint32_t sc = !strcmp(e->status, "completed") ? T_GREEN : !strcmp(e->status, "failed") ? T_RED : T_YELLOW;
        float x = rr.x + px(a, 6);
        draw_text_fit(a->font_small, r, when, -1, rect_make(x, ry, px(a, 96), rh), color_hex(T_TEXT_DIM), ALIGN_LEFT); x += px(a, 100);
        draw_text_fit(a->font_small, r, e->status, -1, rect_make(x, ry, px(a, 66), rh), color_hex(sc), ALIGN_LEFT); x += px(a, 70);
        draw_text_fit(a->font_small, r, dur, -1, rect_make(x, ry, px(a, 56), rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT); x += px(a, 60);
        draw_text_fit(a->font_small, r, e->session, -1, rect_make(x, ry, px(a, 130), rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT); x += px(a, 134);
        char snip[200]; snprintf(snip, sizeof snip, "%s%s", e->redacted ? "[secrets masked] " : "", e->sql);
        for (char *p = snip; *p; p++) if (*p == '\n' || *p == '\t') *p = ' ';
        draw_text_fit(a->font_mono, r, snip, -1, rect_make(x, ry, rr.x + rr.w - x, rh), color_hex(T_TEXT), ALIGN_LEFT);
        char tg[48]; snprintf(tg, sizeof tg, "hist:%d", k); ui_record(tg, rr);
    }
    clip_pop(r);
    ui_scrollbar_draw(a, &v->sb, list, v->nrows * rh, v->scroll);
    if (!v->nrows) draw_text_fit(a->font_small, r, v->n ? "No execution matches the filters" : "No execution yet: runs appear here, newest first", -1, rect_make(list.x + pad, list.y + pad, list.w - 2 * pad, px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    /* actions on the selection */
    float by = rc.y + rc.h - foot + px(a, 4);
    bool has = v->sel >= 0 && v->sel < v->n;
    if (ui_button(a, rect_make(rc.x + pad, by, px(a, 90), px(a, 26)), "Open", BTN_NORMAL, has)) open_entry(v, a, v->sel, false);
    if (ui_button(a, rect_make(rc.x + pad + px(a, 96), by, px(a, 90), px(a, 26)), "Copy", BTN_NORMAL, has)) { SDL_SetClipboardText(v->e[v->sel].sql); app_status(a, "SQL copied from the history"); }
    if (ui_button(a, rect_make(rc.x + pad + px(a, 192), by, px(a, 90), px(a, 26)), "Re-run", BTN_NORMAL, has)) open_entry(v, a, v->sel, true);
    char cnt[64]; snprintf(cnt, sizeof cnt, "%d of %d", v->nrows, v->n);
    draw_text_fit(a->font_small, r, cnt, -1, rect_make(rc.x + rc.w - pad - px(a, 120), by, px(a, 120), px(a, 26)), color_hex(T_TEXT_DIM), ALIGN_RIGHT);
}

void history_module_init(Panels *P, App *a) {
    DockPanelDesc d = { .id = "lume.data.history", .title = "Query History", .icon = "history", .region = DOCK_BOTTOM,
        .min_w = 320, .min_h = 120, .pref_size = 260, .priority = 3, .start_hidden = true,
        .draw = hp_draw, .create = hp_create, .destroy = hp_destroy, .visibility = hp_visibility };
    panels_register(P, a, &d);
    Dock *dk = panels_dock(P);
    int p = dock_find(dk, d.id); if (p >= 0) dock_set_in_bar(dk, p, false);
}
