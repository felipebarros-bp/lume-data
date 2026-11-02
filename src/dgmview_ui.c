/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram tabs: toolbar, find, properties, status strip, menus, dialogs, exports and the assistant.
 * See dgmview.h and docs/diagrams.md. */
#include "dgmview_int.h"
#include "conn.h"
#include "db.h"
#include "json.h"
#include "jw.h"
#include "log.h"
#include "nsx.h"
#include "panels.h"
#include "pdfw.h"
#include "pngw.h"
#include "results.h"
#include "tdes.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <time.h>

#define C(h) color_hex(h)
void dg_dialog_ok(App *a, DgDoc *d);

/* ---------------------------------------------------------------- toolbar */
static float tb_btn(App *a, float *x, float y, float h, const char *label, int style, bool en, bool *clicked, const char *tag) {
    float w = font_width(a->font_ui, label, -1) + px(a, 18);
    Rect r = rect_make(*x, y, w, h);
    ui_record(tag, r);
    *clicked = ui_button(a, r, label, style, en);
    *x += w + px(a, 4);
    return w;
}
void dg_toolbar(App *a, DgDoc *d, Rect bar) {
    SDL_Renderer *R = a->renderer;
    draw_rect(R, bar, C(T_PANEL_ALT));
    draw_rect(R, rect_make(bar.x, bar.y + bar.h - px(a, 1), bar.w, px(a, 1)), C(T_BORDER));
    float x = bar.x + px(a, 8), y = bar.y + px(a, 5), h = bar.h - px(a, 10);
    /* find (DGM-030): the field, the counter, previous / next */
    float fw = fminf(px(a, 240), fmaxf(px(a, 140), bar.w * 0.22f));
    Rect fr = rect_make(x, y, fw, h);
    ui_record("dgm:findfield", fr);
    bool was = d->find_focus;
    bool clicked_in = ui_textfield(a, &d->find, fr, "Find table or column (Cmd+F)", d->find_focus);
    if (clicked_in) { d->find_focus = true; d->find_list = true; }
    if (d->find_focus && !was) d->find_list = true;
    if (d->find_focus) {
        Input *in = &a->in;
        if (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0)) dg_find_step(a, d, 1);
        else if (input_key(in, SDLK_RETURN, KM_SHIFT)) dg_find_step(a, d, -1);
        else if (input_key(in, SDLK_ESCAPE, 0)) { if (d->find_list) d->find_list = false; else d->find_focus = false; }
        else if (input_key(in, SDLK_DOWN, 0)) { if (d->nhits) { d->cur = (d->cur + 1) % (d->nhits < DG_HITS ? d->nhits : DG_HITS); } }
        else if (input_key(in, SDLK_UP, 0)) { if (d->nhits) { d->cur = (d->cur + d->nhits - 1) % d->nhits; } }
    }
    if (strcmp(d->find.buf, d->last_q) || d->find_flags != d->last_flags) dg_find_run(a, d);
    x += fw + px(a, 6);
    char cnt[40];
    if (!d->find.len) cnt[0] = 0;
    else if (!d->total) snprintf(cnt, sizeof cnt, "No match");
    else snprintf(cnt, sizeof cnt, "%d of %d", d->cur + 1, d->total);
    float cw = px(a, 74);
    draw_text_fit(a->font_small, R, cnt, -1, rect_make(x, y, cw, h), C(d->find.len && !d->total ? T_RED : T_TEXT_MUTED), 0);
    ui_record("dgm:findcount", rect_make(x, y, cw, h));
    x += cw;
    bool c;
    tb_btn(a, &x, y, h, "\xE2\x86\x91", BTN_QUIET, d->total > 0, &c, "dgm:prev"); if (c) dg_find_step(a, d, -1);
    tb_btn(a, &x, y, h, "\xE2\x86\x93", BTN_QUIET, d->total > 0, &c, "dgm:next"); if (c) dg_find_step(a, d, 1);
    x += px(a, 8);
    float right = bar.x + bar.w - px(a, 8);
    /* the right side: export / more (always visible) */
    float mw = font_width(a->font_ui, "More", -1) + px(a, 30), ew = font_width(a->font_ui, "Export", -1) + px(a, 30);
    Rect more = rect_make(right - mw, y, mw, h), exr = rect_make(right - mw - ew - px(a, 4), y, ew, h);
    ui_record("dgm:more", more); ui_record("dgm:export", exr);
    if (ui_button(a, more, "More \xE2\x96\xBE", BTN_QUIET, true)) dg_open_menu(a, d, 0, -1, -1, -1, more.x, more.y + more.h);
    if (ui_button(a, exr, "Export \xE2\x96\xBE", BTN_QUIET, d->d.nn > 0)) dg_open_menu(a, d, 4, -1, -1, -1, exr.x, exr.y + exr.h);
    right = exr.x - px(a, 8);
    /* middle: layout, card mode, zoom, fit (dropped from the right when narrow: all also in More) */
    struct { const char *lab; int act; int style; const char *tag; bool en; } it[] = {
        { "Layout", DA_LAYOUT, BTN_QUIET, "dgm:layout", d->d.nn > 0 && !d->lay_th },
        { "Fit", DA_FIT, BTN_QUIET, "dgm:fit", d->d.nn > 0 },
        { "-", DA_ZOOM_OUT, BTN_QUIET, "dgm:zoomout", true },
        { NULL, DA_ZOOM_100, BTN_QUIET, "dgm:zoom", true },
        { "+", DA_ZOOM_IN, BTN_QUIET, "dgm:zoomin", true },
    };
    char zl[16]; snprintf(zl, sizeof zl, "%d%%", (int)lroundf(d->d.zoom * 100)); it[3].lab = zl;
    static const char *const modes[] = { "Name", "Keys", "All" };
    float segw = px(a, 150);
    float need = segw + px(a, 8); for (size_t i = 0; i < sizeof it / sizeof it[0]; i++) need += font_width(a->font_ui, it[i].lab, -1) + px(a, 22);
    if (x + need <= right) {
        int m = ui_segmented(a, rect_make(x, y, segw, h), modes, 3, d->d.mode);
        ui_record("dgm:mode", rect_make(x, y, segw, h));
        if (m >= 0 && m != d->d.mode) dg_action(a, d, m == 0 ? DA_MODE_NAME : m == 1 ? DA_MODE_KEYS : DA_MODE_ALL);
        x += segw + px(a, 8);
        for (size_t i = 0; i < sizeof it / sizeof it[0]; i++) { tb_btn(a, &x, y, h, it[i].lab, it[i].style, it[i].en, &c, it[i].tag); if (c) dg_action(a, d, it[i].act); }
    } else if (x + px(a, 120) <= right) {
        tb_btn(a, &x, y, h, "Layout", BTN_QUIET, d->d.nn > 0 && !d->lay_th, &c, "dgm:layout"); if (c) dg_action(a, d, DA_LAYOUT);
        tb_btn(a, &x, y, h, "Fit", BTN_QUIET, d->d.nn > 0, &c, "dgm:fit"); if (c) dg_action(a, d, DA_FIT);
    }
}

/* ---------------------------------------------------------------- find */
void dg_find_run(App *a, DgDoc *d) {
    DgCat *k = dg_cat(d);
    snprintf(d->last_q, sizeof d->last_q, "%s", d->find.buf); d->last_flags = d->find_flags;
    d->last_cat = k ? k->version : 0;
    d->nhits = d->total = 0; d->cur = 0;
    if (!d->find.len) return;
    const char *fp = strstr(d->find.buf, "from:"), *tp = strstr(d->find.buf, "to:");
    if (fp || tp) {   /* relations by their ends (DGM-039): "from:orders to:customers", either part optional */
        char fa[200] = "", ta[200] = "";
        if (fp) sscanf(fp + 5, "%199s", fa);
        if (tp) sscanf(tp + 3, "%199s", ta);
        for (int e = 0; e < d->d.ne && d->total < DG_HITS; e++) {
            DgEdge *g = &d->d.e[e];
            if (fa[0] && !strcasestr(d->d.n[g->from].key, fa)) continue;
            if (ta[0] && !strcasestr(d->d.n[g->to].key, ta)) continue;
            DgHit *h = &d->hits[d->total++];
            h->ti = -100 - e; h->node = g->from; h->what = DH_CONSTRAINT;
            snprintf(h->text, sizeof h->text, "%s: %s(%s) -> %s%s", g->name, d->d.n[g->from].key, g->cols, d->d.n[g->to].key, g->origin == DE_FK ? "" : g->origin == DE_VIRTUAL ? " (virtual)" : " (suggested)");
        }
        d->nhits = d->total; d->cur = -1;
        return;
    }
    d->total = dgm_find(&d->d, d->find.buf, d->find_flags, d->hits, DG_HITS);
    d->nhits = d->total < DG_HITS ? d->total : DG_HITS;
    d->cur = -1;
    (void)a;
}
void dg_find_step(App *a, DgDoc *d, int dir) {
    if (!d->nhits) { if (d->find.len) dg_msg(d, "No match for \"%s\" in the diagram's scope", d->find.buf); return; }
    d->cur = d->cur < 0 ? (dir > 0 ? 0 : d->nhits - 1) : (d->cur + dir + d->nhits) % d->nhits;
    if (d->cur == 0 && dir > 0 && d->nhits > 1) dg_msg(d, "Back to the first match");
    else if (d->cur == d->nhits - 1 && dir < 0 && d->nhits > 1) dg_msg(d, "Back to the last match");
    DgHit *h = &d->hits[d->cur];
    dg_push_history(d);
    if (h->what == DH_NOTE) {
        int ni = -2 - h->ti;
        if (ni >= 0 && ni < d->d.nnotes) {
            DgNote *n = &d->d.notes[ni]; float cw = d->canvas.w / a->scale, ch = d->canvas.h / a->scale, z = d->d.zoom < 0.5f ? 0.8f : d->d.zoom;
            d->d.zoom = z; d->d.panx = n->x + n->w / 2 - cw / (2 * z); d->d.pany = n->y + n->h / 2 - ch / (2 * z);
            d->flash_note = ni; d->flash[0] = 0;
        }
        return;
    }
    if (h->ti <= -100) {   /* a relation: both ends selected and framed */
        int e = -100 - h->ti;
        if (e < d->d.ne) { for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = i == d->d.e[e].from || i == d->d.e[e].to; dg_fit(a, d, true); }
        return;
    }
    int node = h->node;
    if (node < 0 && h->ti >= 0) {   /* not on the diagram: exactly this table is added (D5) */
        node = dg_add_hit(a, d, h->ti);
        if (node >= 0) {
            dg_msg(d, "%s added to the diagram", d->d.n[node].key);
            /* the other hits keep their order; this one is now on the diagram */
            for (int i = 0; i < d->nhits; i++) if (d->hits[i].ti == h->ti) d->hits[i].node = node;
        }
    }
    if (node < 0) return;
    dg_select_only(d, node);
    snprintf(d->flash, sizeof d->flash, "%s", d->d.n[node].key); d->flash_t = SDL_GetTicks(); d->flash_note = -1;
    if (h->what == DH_COLUMN) {   /* the card opens its columns and scrolls to the row (DGM-031) */
        const char *col = strrchr(h->text, '.');
        if (col && dgm_card_mode(&d->d, node) != DM_ALL) { d->d.n[node].mode = DM_ALL; dg_measure(a, d); dg_touch(d); }
        int rows[512]; int nr = dgm_card_rows(&d->d, node, rows, 512);
        for (int r = 0; col && r < nr && d->d.x && d->d.n[node].ti >= 0; r++) if (!strcasecmp(d->d.x->t[d->d.n[node].ti].cols[rows[r]].name, col + 1)) { d->d.n[node].scroll = r > DG_MAX_ROWS - 4 ? r - DG_MAX_ROWS / 2 : 0; d->focus_row = r; break; }
    }
    dg_center_on(a, d, node, true);
}
void dg_find_list(App *a, DgDoc *d) {
    if (!d->find_focus || !d->find_list || !d->find.len) return;
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    Rect fr; if (!ui_target("dgm:findfield", &fr)) return;
    int show = d->nhits < 12 ? d->nhits : 12;
    float rh = px(a, 24), fh = px(a, 30);
    Rect box = rect_make(fr.x, fr.y + fr.h + px(a, 2), fmaxf(fr.w + px(a, 200), px(a, 440)), fh + rh * (float)(show ? show : 1) + px(a, 26));
    draw_rrect(R, rect_inset(box, -1, -1), px(a, 6), C(T_BORDER));
    draw_rrect(R, box, px(a, 6), C(T_PANEL));
    ui_record("dgm:findlist", box);
    /* filters (DGM-031 / 036) */
    float x = box.x + px(a, 6), y = box.y + px(a, 4);
    struct { const char *lab; int bit; } f[] = { { "Tables", DF_TABLES }, { "Columns", DF_COLUMNS }, { "Constraints", DF_CONSTRAINTS }, { "Notes", DF_NOTES } };
    for (int i = 0; i < 4; i++) {
        float w = font_width(a->font_small, f[i].lab, -1) + px(a, 14); Rect r = rect_make(x, y, w, px(a, 22));
        char tag[40]; snprintf(tag, sizeof tag, "dgm:filter:%s", f[i].lab); ui_record(tag, r);
        if (ui_button(a, r, f[i].lab, d->find_flags & f[i].bit ? BTN_SELECTED : BTN_QUIET, true)) { d->find_flags ^= f[i].bit; if (!(d->find_flags & (DF_TABLES | DF_COLUMNS | DF_CONSTRAINTS | DF_NOTES))) d->find_flags |= DF_TABLES; }
        x += w + px(a, 3);
    }
    x += px(a, 6);
    struct { const char *lab; int bit; } m[] = { { "Contains", 0 }, { "Prefix", DF_PREFIX }, { "Exact", DF_EXACT }, { "Glob", DF_GLOB } };
    int curm = d->find_flags & DF_PREFIX ? 1 : d->find_flags & DF_EXACT ? 2 : d->find_flags & DF_GLOB ? 3 : 0;
    for (int i = 0; i < 4; i++) {
        float w = font_width(a->font_small, m[i].lab, -1) + px(a, 14); Rect r = rect_make(x, y, w, px(a, 22));
        char tag[40]; snprintf(tag, sizeof tag, "dgm:match:%s", m[i].lab); ui_record(tag, r);
        if (ui_button(a, r, m[i].lab, curm == i ? BTN_SELECTED : BTN_QUIET, true)) d->find_flags = (d->find_flags & ~(DF_PREFIX | DF_EXACT | DF_GLOB)) | m[i].bit;
        x += w + px(a, 3);
    }
    { float w = px(a, 30); Rect r = rect_make(x, y, w, px(a, 22)); ui_record("dgm:match:case", r); if (ui_button(a, r, "Aa", d->find_flags & DF_CASE ? BTN_SELECTED : BTN_QUIET, true)) d->find_flags ^= DF_CASE; }
    y = box.y + fh;
    if (!d->nhits) draw_text_fit(a->font_small, R, "No match in this diagram's scope (More > Scope... adds schemas)", -1, rect_make(box.x + px(a, 10), y, box.w - px(a, 20), rh), C(T_TEXT_MUTED), 0);
    int first = d->cur > show - 1 ? d->cur - show + 1 : 0;
    for (int i = 0; i < show && first + i < d->nhits; i++) {
        int hi = first + i; DgHit *h = &d->hits[hi];
        Rect r = rect_make(box.x + px(a, 4), y + rh * (float)i, box.w - px(a, 8), rh);
        bool hv = rect_contains(r, in->mouse_x, in->mouse_y);
        if (hi == d->cur || hv) draw_rrect(R, r, px(a, 4), C(hi == d->cur ? T_SELECTED : T_HOVER));
        const char *state = h->what == DH_NOTE ? "note" : h->node >= 0 ? "on diagram" : "not on diagram";
        float sw = font_width(a->font_small, state, -1) + px(a, 8);
        draw_text_fit(a->font_ui, R, h->text, -1, rect_make(r.x + px(a, 8), r.y, r.w - sw - px(a, 16), r.h), C(T_TEXT), 0);
        draw_text_fit(a->font_small, R, state, -1, rect_make(r.x + r.w - sw - px(a, 4), r.y, sw, r.h), C(h->node >= 0 ? T_TEXT_DIM : T_ACCENT), 2);
        char tag[40]; snprintf(tag, sizeof tag, "dgm:hit:%d", hi); ui_record(tag, r);
        if (hv && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; d->cur = hi - 1; dg_find_step(a, d, 1); }
    }
    char foot[160]; int on = 0; for (int i = 0; i < d->nhits; i++) on += d->hits[i].node >= 0;
    snprintf(foot, sizeof foot, "%d match%s \xC2\xB7 %d on the diagram, %d in the scope only \xC2\xB7 Enter: next, Shift+Enter: previous", d->total, d->total == 1 ? "" : "es", on, d->total - on);
    draw_text_fit(a->font_small, R, foot, -1, rect_make(box.x + px(a, 10), box.y + box.h - px(a, 22), box.w - px(a, 20), px(a, 20)), C(T_TEXT_DIM), 0);
    if (in->mouse_pressed && !in->mouse_consumed && !rect_contains(box, in->mouse_x, in->mouse_y) && !rect_contains(fr, in->mouse_x, in->mouse_y)) d->find_list = false;
    if (rect_contains(box, in->mouse_x, in->mouse_y) && in->mouse_pressed) in->mouse_consumed = true;
}

/* ---------------------------------------------------------------- properties pane (DGM-029) */
void dg_props_pane(App *a, DgDoc *d, Rect r) {
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, r, C(T_PANEL));
    draw_rect(R, rect_make(r.x, r.y, px(a, 1), r.h), C(T_BORDER));
    ui_record("dgm:props", r);
    int n = -1; for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel) { n = i; break; }
    float x = r.x + px(a, 12), w = r.w - px(a, 24), y = r.y + px(a, 10) - d->props_scroll;
    clip_push(R, r);
    if (n < 0) {
        DgCat *k = dg_cat(d);
        draw_text_fit(a->font_ui, R, "Diagram", -1, rect_make(x, y, w, px(a, 22)), C(T_TEXT), 0); y += px(a, 26);
        char s[400];
        snprintf(s, sizeof s, "%d tables on the diagram, %d in the scope (%s). Select a card to see its columns and relations.", d->d.nn, k && k->state == CS_READY ? k->x.n : 0, d->d.schemas[0] ? d->d.schemas : "default");
        y += results_draw_wrapped(a->font_small, R, s, rect_make(x, y, w, px(a, 200)), C(T_TEXT_MUTED), 0, true) + px(a, 10);
        snprintf(s, sizeof s, "Lines: solid = foreign key in the database; dashed = virtual (drawn by you); dotted = suggested (a hypothesis). Crow's foot = many; bar = one; circle = optional.");
        y += results_draw_wrapped(a->font_small, R, s, rect_make(x, y, w, px(a, 200)), C(T_TEXT_DIM), 0, true);
        clip_pop(R);
        return;
    }
    DgNode *nd = &d->d.n[n];
    draw_text_fit(a->font_ui, R, nd->kind == DN_ENTITY ? nd->key + 7 : nd->key, -1, rect_make(x, y, w, px(a, 22)), C(T_TEXT), 0); y += px(a, 22);
    const char *kind = nd->kind == DN_ENTITY ? "logical entity" : nd->kind == DN_STUB ? "external reference (outside the scope or not readable)" : d->d.x && nd->ti >= 0 && d->d.x->t[nd->ti].is_view ? "view" : "table";
    draw_text_fit(a->font_small, R, kind, -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_MUTED), 0); y += px(a, 22);
    if (nd->alias[0]) { char s[160]; snprintf(s, sizeof s, "Business name: %s", nd->alias); draw_text_fit(a->font_small, R, s, -1, rect_make(x, y, w, px(a, 18)), C(T_SYN_KEYWORD), 0); y += px(a, 20); }
    /* actions (DGM-084 / 086) */
    struct { const char *lab; int act; bool en; } acts[] = {
        { "Open Data", DA_OPEN_DATA, nd->kind == DN_TABLE }, { "Definition", DA_OPEN_DEFINITION, nd->kind == DN_TABLE }, { "Neighbors", DA_NEIGH1, nd->kind == DN_TABLE && nd->more > 0 },
    };
    float bx = x;
    for (int i = 0; i < 3; i++) { float bw = font_width(a->font_small, acts[i].lab, -1) + px(a, 16); char tag[40]; snprintf(tag, sizeof tag, "dgm:prop:%s", acts[i].lab); Rect br = rect_make(bx, y, bw, px(a, 24)); ui_record(tag, br); if (ui_button(a, br, acts[i].lab, BTN_QUIET, acts[i].en)) dg_action(a, d, acts[i].act); bx += bw + px(a, 4); }
    y += px(a, 32);
    if (nd->kind == DN_TABLE && d->d.x && nd->ti >= 0) {
        const SxTable *t = &d->d.x->t[nd->ti];
        if (t->comment && t->comment[0]) { y += results_draw_wrapped(a->font_small, R, t->comment, rect_make(x, y, w, px(a, 120)), C(T_TEXT_MUTED), 0, true) + px(a, 8); }
        char h[80]; snprintf(h, sizeof h, "COLUMNS (%d)", t->ncols);
        draw_text_fit(a->font_small, R, h, -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_DIM), 0); y += px(a, 20);
        for (int c = 0; c < t->ncols; c++) {
            const SxCol *col = &t->cols[c];
            char s[400]; snprintf(s, sizeof s, "%s%s  %s%s", col->pk ? "PK " : "", col->name, col->type ? col->type : "", col->notnull ? "  not null" : "");
            draw_text_fit(a->font_small, R, s, -1, rect_make(x, y, w, px(a, 18)), C(col->pk ? T_YELLOW : T_TEXT), 0); y += px(a, 18);
            if (col->comment && col->comment[0]) { y += results_draw_wrapped(a->font_small, R, col->comment, rect_make(x + px(a, 12), y, w - px(a, 12), px(a, 60)), C(T_TEXT_DIM), 0, true); }
        }
        y += px(a, 8);
        draw_text_fit(a->font_small, R, "REFERENCES", -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_DIM), 0); y += px(a, 20);
        if (!t->nfks) { draw_text_fit(a->font_small, R, "none (no foreign key)", -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_MUTED), 0); y += px(a, 18); }
        for (int q = 0; q < t->nfks; q++) {
            char s[600]; snprintf(s, sizeof s, "%s(%s) \xE2\x86\x92 %s(%s)", t->fks[q].name ? t->fks[q].name : "", t->fks[q].cols, t->fks[q].ref_table, t->fks[q].ref_cols ? t->fks[q].ref_cols : "key");
            y += results_draw_wrapped(a->font_small, R, s, rect_make(x, y, w, px(a, 60)), C(t->fks[q].ref >= 0 ? T_TEXT : T_TEXT_MUTED), 0, true) + px(a, 2);
        }
        y += px(a, 8);
        int nb[512]; int nn = dgm_catalog_neighbours(&d->d, nd->ti, nb, 512), inc = 0;
        for (int i = 0; i < nn; i++) { bool out = false; for (int q = 0; q < t->nfks; q++) if (t->fks[q].ref == nb[i]) out = true; inc += !out; }
        char rf[120]; snprintf(rf, sizeof rf, "REFERENCED BY (%d)", inc);
        draw_text_fit(a->font_small, R, rf, -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_DIM), 0); y += px(a, 20);
        for (int i = 0, shown = 0; i < nn && shown < 40; i++) {
            bool out = false; for (int q = 0; q < t->nfks; q++) if (t->fks[q].ref == nb[i]) out = true;
            if (out) continue;
            char key[DG_KEY]; dgm_key_of(d->d.x, nb[i], key, sizeof key);
            bool on = dgm_find_node(&d->d, key) >= 0;
            char s[DG_KEY + 30]; snprintf(s, sizeof s, "%s%s", key, on ? "" : "  (not on diagram)");
            Rect rr = rect_make(x, y, w, px(a, 18));
            if (rect_contains(rr, in->mouse_x, in->mouse_y)) { draw_rect(R, rr, C(T_HOVER)); if (in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; int nn2 = dg_add_hit(a, d, nb[i]); if (nn2 >= 0) { dg_push_history(d); dg_select_only(d, nn2); dg_center_on(a, d, nn2, true); } } }
            draw_text_fit(a->font_small, R, s, -1, rr, C(on ? T_TEXT : T_ACCENT), 0); y += px(a, 18); shown++;
        }
        if (t->uniques) { y += px(a, 8); char s[600]; snprintf(s, sizeof s, "Unique keys: %s", t->uniques); y += results_draw_wrapped(a->font_small, R, s, rect_make(x, y, w, px(a, 60)), C(T_TEXT_MUTED), 0, true); }
    } else if (nd->kind == DN_ENTITY) {
        y += results_draw_wrapped(a->font_small, R, nd->attrs[0] ? nd->attrs : "No attributes yet (double-click the card)", rect_make(x, y, w, px(a, 400)), C(T_TEXT), 0, true);
    } else {
        y += results_draw_wrapped(a->font_small, R, "This table is outside the diagram's scope, or this login cannot read it. It is shown as a reference, not as missing. More > Scope... adds its schema.", rect_make(x, y, w, px(a, 200)), C(T_TEXT_MUTED), 0, true);
    }
    float content = y + d->props_scroll - r.y;
    if (rect_contains(r, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { d->props_scroll = fmaxf(0, fminf(content - r.h + px(a, 20), d->props_scroll - in->wheel_y * px(a, 30))); in->wheel_y = 0; }
    clip_pop(R);
}

/* ---------------------------------------------------------------- status strip (DGM-093) */
void dg_footer(App *a, DgDoc *d, char *out, size_t n) {
    DgCat *k = dg_cat(d); (void)a;
    int fk = 0; for (int e = 0; e < d->d.ne; e++) fk += d->d.e[e].origin == DE_FK;
    int partial = 0; for (int i = 0; i < d->d.nn; i++) partial += d->d.n[i].kind == DN_STUB;
    char when[40] = "not read yet";
    if (d->d.captured[0]) snprintf(when, sizeof when, "%.16s", d->d.captured);
    snprintf(out, n, "%d on diagram / %d in scope \xC2\xB7 %d FK%s \xC2\xB7 snapshot %s%s \xC2\xB7 read-only%s",
             d->d.nn, k && k->state == CS_READY ? k->x.n : 0, fk, fk == 1 ? "" : "s", when, k && k->stale ? " (offline: cached)" : "",
             partial ? " \xC2\xB7 external references shown as stubs" : "");
}
void dg_status(App *a, DgDoc *d, Rect r) {
    SDL_Renderer *R = a->renderer;
    draw_rect(R, r, C(T_PANEL_ALT));
    draw_rect(R, rect_make(r.x, r.y, r.w, px(a, 1)), C(T_BORDER));
    char s[600]; dg_footer(a, d, s, sizeof s);
    DgCat *k = dg_cat(d);
    if (k && (k->state == CS_WAIT || k->state == CS_RUN)) snprintf(s, sizeof s, "Reading the structure of the scope (%s)... names, keys and relations only", d->d.schemas[0] ? d->d.schemas : "default");
    float mw = 0;
    bool recent = d->msg[0] && SDL_GetTicks() - d->msg_t < 8000;
    if (recent) { mw = fminf(font_width(a->font_small, d->msg, -1) + px(a, 16), r.w * 0.5f); draw_text_fit(a->font_small, R, d->msg, -1, rect_make(r.x + r.w - mw, r.y, mw - px(a, 8), r.h), C(T_ACCENT), 2); app_wake_in(a, 8100); }
    draw_text_fit(a->font_small, R, s, -1, rect_make(r.x + px(a, 10), r.y, r.w - mw - px(a, 20), r.h), C(k && k->stale ? T_YELLOW : T_TEXT_MUTED), 0);
    ui_record("dgm:status", r);
}

/* ---------------------------------------------------------------- menus (DGM-084) */
enum { MK_CANVAS, MK_CARD, MK_EDGE, MK_NOTE, MK_EXPORT };
static const char *color_names[9] = { "By schema", "Blue", "Green", "Orange", "Purple", "Teal", "Pink", "Yellow", "Grey" };
void dg_open_menu(App *a, DgDoc *d, int kind, int node, int edge, int note, float sx, float sy) {
    PopupMenu *m = &d->cm;
    popup_open(m, sx, sy);
    d->cm_kind = kind; d->cm_node = node; d->cm_edge = edge; d->cm_note = note;
    dg_screen_to_world(d, a, sx, sy, &d->cm_wx, &d->cm_wy);
    bool ro = true; (void)ro;
    int ns = dg_nsel(d);
    if (kind == MK_CARD && node >= 0) {
        DgNode *n = &d->d.n[node];
        bool table = n->kind == DN_TABLE;
        popup_add(m, DA_FOCUS_HERE, "Focus Here", false, table);
        popup_add(m, DA_NEIGH1, "Show Neighbors (1 hop)", false, table);
        popup_add(m, DA_NEIGH2, "Show Neighbors (2 hops)", false, table);
        popup_add(m, DA_NEIGH3, "Show Neighbors (3 hops)", false, table);
        popup_add(m, DA_ISOLATE, d->isolate ? "Reset Focus" : "Isolate Neighborhood", false, true);
        popup_add(m, DA_OPEN_DATA, "Open Table Data", true, table);
        popup_add(m, DA_OPEN_DEFINITION, "Show DDL", false, table);
        popup_add(m, DA_EDIT_TABLE, "Edit Table\xE2\x80\xA6", false, table);
        popup_add(m, DA_REFRESH_TABLE, "Refresh Table", false, table);
        popup_add(m, DA_CARD_NAME, "Card: Name Only", true, true);
        popup_add(m, DA_CARD_KEYS, "Card: Keys", false, true);
        popup_add(m, DA_CARD_ALL, "Card: All Columns", false, true);
        popup_add(m, DA_PIN, n->pinned ? "Unpin Position" : "Pin Position", false, true);
        popup_add(m, DA_COLOR, "Set Color  \xE2\x80\xBA", false, true);
        popup_add(m, DA_ALIAS, "Business Name\xE2\x80\xA6", false, true);
        if (n->kind == DN_ENTITY) popup_add(m, DA_ATTRS, "Attributes\xE2\x80\xA6", false, true);
        popup_add(m, DA_VIRTUAL, n->kind == DN_ENTITY ? "Draw Relationship\xE2\x80\xA6" : "Draw Virtual Relation\xE2\x80\xA6", false, true);
        if (ns > 1) popup_add(m, DA_LAYOUT_SEL, "Arrange Selection", true, true);
        if (ns > 1) popup_add(m, DA_ALIGN_L, "Align  \xE2\x80\xBA", false, true);
        popup_add(m, DA_COPY_NAME, "Copy Qualified Name", true, true);
        popup_add(m, DA_REMOVE, ns > 1 ? "Remove Selected From Diagram" : "Remove From Diagram", false, true);
    } else if (kind == MK_EDGE && edge >= 0) {
        DgEdge *g = &d->d.e[edge];
        popup_add(m, DA_EDGE_DEF, g->origin == DE_FK ? "Show FK Definition" : "Show Relation", false, true);
        popup_add(m, DA_EDGE_FIT, "Highlight Source and Target", false, true);
        popup_add(m, DA_FOLLOW, "Follow to Target", false, true);
        popup_add(m, DA_HIDE_EDGE, "Hide From Diagram", false, true);
        popup_add(m, DA_COPY_EDGE, "Copy Relationship Metadata", false, true);
        if (g->origin != DE_FK) {
            popup_add(m, DA_REL_LOGICAL, "Edit Label\xE2\x80\xA6", true, true);
            popup_add(m, DA_EVIDENCE, "Evidence\xE2\x80\xA6", false, g->origin != DE_LOGICAL);
            popup_add(m, DA_VERIFY_REL, g->rel >= 0 && d->d.rels[g->rel].verified ? "Mark as Unverified" : "Mark as Verified", false, g->origin != DE_LOGICAL);
            popup_add(m, DA_DELETE_REL, "Delete Relation", false, true);
        }
    } else if (kind == MK_NOTE && note >= 0) {
        popup_add(m, DA_EDIT_NOTE, "Edit\xE2\x80\xA6", false, true);
        popup_add(m, DA_COLOR, "Set Color  \xE2\x80\xBA", false, true);
        popup_add(m, DA_DELETE_NOTE, "Delete", true, true);
    } else if (kind == MK_EXPORT) {
        static const char *const fl[] = { "PNG Image\xE2\x80\xA6", "SVG (vector)\xE2\x80\xA6", "PDF\xE2\x80\xA6", "Mermaid\xE2\x80\xA6", "GraphML\xE2\x80\xA6", "draw.io\xE2\x80\xA6", "HTML Documentation\xE2\x80\xA6" };
        for (int i = 0; i < 7; i++) popup_add(m, 1000 + i, fl[i], false, true);
        popup_add(m, DA_COPY_IMAGE, "Copy as Image", true, true);
        popup_add(m, DA_SAVE_SUBSET, "Save Selection as Diagram\xE2\x80\xA6", false, ns > 0);
    } else {
        popup_add(m, DA_FIND, "Add Tables\xE2\x80\xA6 (Find)", false, true);
        popup_add(m, DA_LAYOUT, "Auto Layout", false, d->d.nn > 0);
        popup_add(m, DA_FIT, "Fit Content", false, d->d.nn > 0);
        popup_add(m, DA_SELECT_ALL, "Select All", false, d->d.nn > 0);
        popup_add(m, DA_NOTE, "Add Note", true, true);
        popup_add(m, DA_AREA, "Add Group Area", false, true);
        popup_add(m, DA_ENTITY, "Add Logical Entity", false, true);
        popup_add(m, DA_GROUP_COMPONENTS, "Group by Connected Components", false, d->d.nn > 1);
        popup_add(m, DA_MINIMAP, "View  \xE2\x80\xBA", true, true);
        popup_add(m, DA_SCOPE, "Scope\xE2\x80\xA6", false, true);
        popup_add(m, DA_REFRESH, "Refresh Structure", false, true);
        popup_add(m, DA_COMPARE, "Compare with Saved Snapshot", false, d->d.nn > 0);
        popup_add(m, DA_AI_EXPLAIN, "Lume Intelligence  \xE2\x80\xBA", true, d->d.nn > 0);
        popup_add(m, DA_DUPLICATE, "Duplicate Diagram", true, d->d.nn > 0);
        popup_add(m, DA_RENAME, "Rename Diagram\xE2\x80\xA6", false, true);
        popup_add(m, DA_BACK, "Back", true, d->ihist > 0);
        popup_add(m, DA_FORWARD, "Forward", false, d->ihist < d->nhist - 1);
        popup_add(m, DA_UNDO, "Undo Layout Change", false, d->d.iundo > 0 || (d->d.nundo && d->d.iundo == d->d.nundo));
    }
    app_wake_in(a, 16);
}
static void open_sub(DgDoc *d, int id) {
    PopupMenu *s = popup_open_sub(&d->cm, &d->sub);
    if (id == DA_COLOR) for (int i = 0; i < 9; i++) popup_add(s, 2000 + i, color_names[i], false, true);
    else if (id == DA_ALIGN_L) {
        static const char *const al[] = { "Align Left", "Align Centre", "Align Right", "Align Top", "Align Middle", "Align Bottom", "Distribute Horizontally", "Distribute Vertically" };
        for (int i = 0; i < 8; i++) popup_add(s, DA_ALIGN_L + i, al[i], i == 6, true);
    } else if (id == DA_MINIMAP) {
        snprintf(d->sub_labels[0], 96, "%s Minimap", d->minimap ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[1], 96, "%s Properties", d->props ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[2], 96, "%s Virtual and Suggested Relations", d->d.show_virtual ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[3], 96, "%s Column Types", d->d.show_types ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[4], 96, "%s Comments (hover)", d->d.show_comments ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[5], 96, "%s Nullable Marks (?)", d->d.show_nullability ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[6], 96, "Notation: %s", d->d.notation == DNOT_CROW ? "Crow's Foot" : "Simple");
        snprintf(d->sub_labels[7], 96, "Lines: %s", d->d.routing == DRT_ORTHO ? "Orthogonal" : "Straight");
        snprintf(d->sub_labels[8], 96, "%s Snap to Grid", d->d.snap ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[9], 96, "%s Conceptual View (business names)", d->d.conceptual ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[10], 96, "%s Highlight All Matches", d->highlight_all ? "\xE2\x9C\x93" : "   ");
        snprintf(d->sub_labels[11], 96, "Select Disconnected Tables");
        snprintf(d->sub_labels[12], 96, "Show Hidden Relations (%d)", d->d.nhidden);
        int ids[] = { DA_MINIMAP, DA_PROPS, DA_TOGGLE_VIRTUAL, DA_TYPES, DA_COMMENTS, DA_NULLS, DA_NOTATION, DA_ROUTING, DA_SNAP, DA_CONCEPTUAL, DA_HIGHLIGHT_ALL, DA_DISCONNECTED, DA_SHOW_HIDDEN };
        for (int i = 0; i < 13; i++) popup_add(s, 3000 + ids[i], d->sub_labels[i], i == 6 || i == 11, i != 12 || d->d.nhidden > 0);
    } else if (id == DA_AI_EXPLAIN) {
        popup_add(s, 3000 + DA_AI_EXPLAIN, "Explain These Tables", false, true);
        popup_add(s, 3000 + DA_AI_ORGANISE, "Suggest Domain Groups", false, true);
        popup_add(s, 3000 + DA_AI_CONCEPTUAL, "Suggest Conceptual Map", false, true);
    }
}
void dg_menus(App *a, DgDoc *d) {
    if (!d->cm.open) return;
    int id = popup_update(a, &d->cm);
    if (id >= 0 && d->cm.open && d->cm.pending_sub >= 0 && d->cm.is_sub[d->cm.pending_sub]) { open_sub(d, id); popup_draw(a, &d->cm); return; }
    popup_draw(a, &d->cm);
    if (id < 0) return;
    popup_close(&d->cm);
    if (id >= 3000) { dg_action(a, d, id - 3000); return; }
    if (id >= 2000 && id < 2009) {
        int col = id - 2000;
        if (d->cm_kind == MK_NOTE && d->cm_note >= 0 && d->cm_note < d->d.nnotes) d->d.notes[d->cm_note].color = col;
        else for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel) d->d.n[i].color = col;
        dg_touch(d); return;
    }
    if (id >= 1000 && id < 1007) { dg_open_export(a, d, id - 1000); return; }
    dg_action(a, d, id);
}

/* ---------------------------------------------------------------- dialogs */
void dg_prompt(DgDoc *d, int kind, int node, const char *title, const char *initial) {
    d->dlg = DLG_PROMPT; d->prompt_kind = kind; d->prompt_node = node;
    snprintf(d->dlg_title, sizeof d->dlg_title, "%s", title);
    textfield_set(&d->pf, initial ? initial : ""); textfield_set(&d->pf2, "");
    d->dlg_body[0] = 0;
    if (kind == PR_ATTRS) snprintf(d->dlg_body, sizeof d->dlg_body, "One attribute per ';' (\"PK id integer; name text\"). PK marks a key.");
    if (kind == PR_NOTE || kind == PR_AREA || kind == PR_NOTE_EDIT) snprintf(d->dlg_body, sizeof d->dlg_body, "Title above, text below. Notes live only in this diagram file; they change nothing in the database.");
    if (kind == PR_EVIDENCE) snprintf(d->dlg_body, sizeof d->dlg_body, "Why do you believe these tables relate? (column values compared, documentation, ...) Shown on hover and in exports.");
    if (kind == PR_ALIAS) snprintf(d->dlg_body, sizeof d->dlg_body, "Shown in the conceptual view and in exports (with the physical name in the legend). Empty removes it.");
}
void dg_open_scope(App *a, DgDoc *d) {
    Connection *c = dg_conn(a, d->conn_id); if (!c) return;
    d->nscope = 0;
    /* the scope's schemas first, then the Explorer's featured and loaded names */
    char cur[1200]; snprintf(cur, sizeof cur, "%s", d->d.schemas);
    for (char *p = strtok(cur, ","); p && d->nscope < 200; p = strtok(NULL, ",")) { while (*p == ' ') p++; if (*p) { snprintf(d->scope_names[d->nscope], 128, "%s", p); d->scope_on[d->nscope++] = true; } }
    NsState *ns = nsx_find(c->id);
    if (ns) { nsx_featured(ns); for (int i = 0; i < ns->nfeatured && d->nscope < 200; i++) { bool dup = false; for (int q = 0; q < d->nscope; q++) if (!strcmp(d->scope_names[q], ns->featured[i].name)) dup = true; if (!dup) { snprintf(d->scope_names[d->nscope], 128, "%s", ns->featured[i].name); d->scope_on[d->nscope++] = false; } } }
    textfield_set(&d->scope_filter, ""); d->scope_scroll = 0;
    d->dlg = DLG_SCOPE; snprintf(d->dlg_title, sizeof d->dlg_title, "Diagram scope");
    snprintf(d->dlg_body, sizeof d->dlg_body, "%s", !strcmp(c->driver, "sqlite") ? "A SQLite file is one scope." : "The schemas whose tables the diagram can show and find. Type a name to add one that is not listed. Never \"all schemas\": large servers have thousands.");
}
void dg_open_picker(App *a, DgDoc *d, const char *schema) {
    DgCat *k = dg_cat(d); if (!k || k->state != CS_READY) return;
    free(d->pick); free(d->pick_on);
    int n = 0;
    for (int i = 0; i < k->x.n; i++) if (!schema || !*schema || !strcasecmp(k->x.t[i].schema ? k->x.t[i].schema : "", schema) || !k->x.t[i].schema[0]) n++;
    d->pick = malloc(sizeof(int) * (size_t)(n ? n : 1)); d->pick_on = calloc((size_t)(n ? n : 1), 1);
    d->npick = dgm_most_connected(&k->x, schema && *schema && strcmp(schema, "main") ? schema : NULL, d->pick, n);
    d->pick_n = 25; d->pick_mode = d->npick <= 150 ? 3 : 1;
    textfield_set(&d->pick_filter, ""); d->pick_scroll = 0;
    d->dlg = DLG_PICKER;
    snprintf(d->dlg_title, sizeof d->dlg_title, "New ER diagram: %s", schema && *schema ? schema : d->d.conn_name);
    snprintf(d->dlg_body, sizeof d->dlg_body, "%d tables in this schema.%s", d->npick, d->npick > 150 ? " A large schema: start from the most connected tables or pick the ones you need (the rest stays searchable)." : "");
    (void)a;
}
void dg_open_export(App *a, DgDoc *d, int fmt) {
    d->dlg = DLG_EXPORT; d->ex_fmt = fmt; d->ex_scope = dg_nsel(d) > 0 ? EXS_SELECTION : EXS_DIAGRAM;
    snprintf(d->dlg_title, sizeof d->dlg_title, "Export diagram");
    (void)a;
}
static Rect dlg_box(App *a, DgDoc *d, float w, float h) {
    Rect c = d->body;
    w = fminf(px(a, w), c.w - px(a, 20)); h = fminf(px(a, h), c.h - px(a, 20));
    return rect_make(c.x + (c.w - w) / 2, c.y + fmaxf(px(a, 40), (c.h - h) / 3), w, h);
}
static void dlg_frame(App *a, Rect b, const char *title) {
    SDL_Renderer *R = a->renderer;
    draw_rrect(R, rect_inset(b, -px(a, 1), -px(a, 1)), px(a, 9), C(T_BORDER));
    draw_rrect(R, b, px(a, 8), C(T_PANEL_ALT));
    draw_text_fit(a->font_ui, R, title, -1, rect_make(b.x + px(a, 18), b.y + px(a, 12), b.w - px(a, 36), px(a, 22)), C(T_TEXT), 0);
}
static int scope_count(DgDoc *d, int sc) {
    int n = 0;
    if (sc == EXS_SELECTION) return dg_nsel(d);
    if (sc == EXS_DIAGRAM) return d->d.nn;
    for (int i = 0; i < d->d.nn; i++) { float sx, sy; dg_world_to_screen(d, DG.a, d->d.n[i].x, d->d.n[i].y, &sx, &sy); float k = dg_k(d, DG.a); if (sx < d->canvas.x + d->canvas.w && sx + d->d.n[i].w * k > d->canvas.x && sy < d->canvas.y + d->canvas.h && sy + d->d.n[i].h * k > d->canvas.y) n++; }
    return n;
}
void dg_dialogs(App *a, DgDoc *d) {
    if (!d->dlg) return;
    SDL_Renderer *R = a->renderer; Input *in = &a->in;
    draw_rect(R, d->body, color_alpha(C(0x000000), 90));
    bool esc = input_key(in, SDLK_ESCAPE, 0);
    bool enter = !ui_text_editing() && (input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0));
    if (esc) { d->dlg = DLG_NONE; return; }
    Rect b; float x, y, w;
    switch (d->dlg) {
    case DLG_CONFIRM_ADD: {
        b = dlg_box(a, d, 460, 170); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        results_draw_wrapped(a->font_small, R, d->dlg_body, rect_make(x, y, w, px(a, 70)), C(T_TEXT_MUTED), 0, true);
        Rect ok = rect_make(b.x + b.w - px(a, 236), b.y + b.h - px(a, 44), px(a, 120), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        char lab[40]; snprintf(lab, sizeof lab, "Add %d", d->confirm_count > 150 ? 150 : d->confirm_count);
        if (ui_button(a, ok, lab, BTN_PRIMARY, true) || enter) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) d->dlg = DLG_NONE;
        break;
    }
    case DLG_PROMPT: {
        bool two = d->prompt_kind == PR_NOTE || d->prompt_kind == PR_AREA || d->prompt_kind == PR_NOTE_EDIT;
        b = dlg_box(a, d, 480, two ? 230 : 190); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        Rect f1 = rect_make(x, y, w, px(a, 28)); ui_record("dgm:dlg:field", f1);
        ui_textfield(a, &d->pf, f1, two ? "Title" : "", true);
        y += px(a, 36);
        if (two) { Rect f2 = rect_make(x, y, w, px(a, 28)); ui_record("dgm:dlg:field2", f2); ui_textfield(a, &d->pf2, f2, "Text (optional)", false); y += px(a, 36); }
        if (d->dlg_body[0]) results_draw_wrapped(a->font_small, R, d->dlg_body, rect_make(x, y, w, px(a, 50)), C(T_TEXT_DIM), 0, true);
        Rect ok = rect_make(b.x + b.w - px(a, 208), b.y + b.h - px(a, 44), px(a, 90), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        bool ret = input_key(in, SDLK_RETURN, 0) || input_key(in, SDLK_KP_ENTER, 0);
        if (ui_button(a, ok, "OK", BTN_PRIMARY, true) || ret) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) d->dlg = DLG_NONE;
        break;
    }
    case DLG_SCOPE: {
        b = dlg_box(a, d, 480, 440); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        y += results_draw_wrapped(a->font_small, R, d->dlg_body, rect_make(x, y, w, px(a, 50)), C(T_TEXT_MUTED), 0, true) + px(a, 8);
        Rect ff = rect_make(x, y, w, px(a, 28)); ui_record("dgm:dlg:field", ff);
        ui_textfield(a, &d->scope_filter, ff, "Filter or type a schema name", true);
        y += px(a, 36);
        Rect list = rect_make(x, y, w, b.y + b.h - px(a, 56) - y);
        clip_push(R, list);
        float ly = list.y - d->scope_scroll; int shown = 0;
        for (int i = 0; i < d->nscope; i++) {
            if (d->scope_filter.len && !strcasestr(d->scope_names[i], d->scope_filter.buf)) continue;
            Rect cr = rect_make(list.x, ly, list.w, px(a, 24));
            char tag[160]; snprintf(tag, sizeof tag, "dgm:scope:%s", d->scope_names[i]); ui_record(tag, cr);
            if (ui_checkbox(a, cr, d->scope_names[i], d->scope_on[i], true)) d->scope_on[i] = !d->scope_on[i];
            ly += px(a, 26); shown++;
        }
        bool exact = false; for (int i = 0; i < d->nscope; i++) if (!strcasecmp(d->scope_names[i], d->scope_filter.buf)) exact = true;
        if (d->scope_filter.len && !exact && d->nscope < 200) {
            char lab[200]; snprintf(lab, sizeof lab, "Add \"%s\"", d->scope_filter.buf);
            Rect ar = rect_make(list.x, ly, px(a, 220), px(a, 26)); ui_record("dgm:scope:add", ar);
            if (ui_button(a, ar, lab, BTN_QUIET, true)) { snprintf(d->scope_names[d->nscope], 128, "%s", d->scope_filter.buf); d->scope_on[d->nscope++] = true; textfield_set(&d->scope_filter, ""); }
        }
        clip_pop(R);
        if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y) { d->scope_scroll = fmaxf(0, d->scope_scroll - in->wheel_y * px(a, 30)); in->wheel_y = 0; }
        (void)shown;
        int on = 0; for (int i = 0; i < d->nscope; i++) on += d->scope_on[i];
        Rect ok = rect_make(b.x + b.w - px(a, 208), b.y + b.h - px(a, 44), px(a, 90), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        if (ui_button(a, ok, "Apply", BTN_PRIMARY, on > 0 || !strcmp(dg_conn(a, d->conn_id) ? dg_conn(a, d->conn_id)->driver : "", "sqlite"))) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) d->dlg = DLG_NONE;
        break;
    }
    case DLG_PICKER: {
        b = dlg_box(a, d, 520, 480); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        y += results_draw_wrapped(a->font_small, R, d->dlg_body, rect_make(x, y, w, px(a, 50)), C(T_TEXT_MUTED), 0, true) + px(a, 8);
        static const char *const modes[] = { "Pick tables", "Most connected", "Empty", "Whole schema" };
        int nm = d->npick <= 150 ? 4 : 3;
        Rect sr = rect_make(x, y, w, px(a, 28)); ui_record("dgm:pick:mode", sr);
        int m = ui_segmented(a, sr, modes, nm, d->pick_mode); if (m >= 0) d->pick_mode = m;
        y += px(a, 38);
        if (d->pick_mode == 0) {
            Rect ff = rect_make(x, y, w, px(a, 28)); ui_record("dgm:dlg:field", ff);
            ui_textfield(a, &d->pick_filter, ff, "Filter tables", true); y += px(a, 34);
            Rect list = rect_make(x, y, w, b.y + b.h - px(a, 56) - y);
            clip_push(R, list); float ly = list.y - d->pick_scroll;
            DgCat *k = dg_cat(d);
            for (int i = 0; k && i < d->npick; i++) {
                const char *nm2 = k->x.t[d->pick[i]].name;
                if (d->pick_filter.len && !strcasestr(nm2, d->pick_filter.buf)) continue;
                if (ly > list.y + list.h) break;
                if (ly + px(a, 24) >= list.y) {
                    char lab[300]; snprintf(lab, sizeof lab, "%s  (%d FK)", nm2, k->x.t[d->pick[i]].nfks + k->x.t[d->pick[i]].nrefs);
                    Rect cr = rect_make(list.x, ly, list.w, px(a, 24));
                    if (ui_checkbox(a, cr, lab, d->pick_on[i], true)) d->pick_on[i] = !d->pick_on[i];
                }
                ly += px(a, 26);
            }
            clip_pop(R);
            if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y) { d->pick_scroll = fmaxf(0, d->pick_scroll - in->wheel_y * px(a, 30)); in->wheel_y = 0; }
        } else if (d->pick_mode == 1) {
            static const char *const ns[] = { "10", "25", "50", "100" }; static const int nv[] = { 10, 25, 50, 100 };
            int cur = d->pick_n == 10 ? 0 : d->pick_n == 50 ? 2 : d->pick_n == 100 ? 3 : 1;
            draw_text_fit(a->font_small, R, "How many (by number of foreign keys):", -1, rect_make(x, y, w, px(a, 20)), C(T_TEXT_MUTED), 0); y += px(a, 24);
            int k2 = ui_segmented(a, rect_make(x, y, px(a, 240), px(a, 28)), ns, 4, cur); if (k2 >= 0) d->pick_n = nv[k2];
        } else {
            results_draw_wrapped(a->font_small, R, d->pick_mode == 2 ? "An empty canvas: find tables (Cmd+F) and press Enter to add them one by one." : "Every table of the schema, laid out by relation.", rect_make(x, y, w, px(a, 60)), C(T_TEXT_MUTED), 0, true);
        }
        Rect ok = rect_make(b.x + b.w - px(a, 208), b.y + b.h - px(a, 44), px(a, 90), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        if (ui_button(a, ok, "Create", BTN_PRIMARY, true)) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) d->dlg = DLG_NONE;
        break;
    }
    case DLG_EXPORT: {
        b = dlg_box(a, d, 560, 330); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        static const char *const fm[] = { "PNG", "SVG", "PDF", "Mermaid", "GraphML", "draw.io", "HTML" };
        Rect fr = rect_make(x, y, w, px(a, 28)); ui_record("dgm:ex:format", fr);
        int f = ui_segmented(a, fr, fm, 7, d->ex_fmt); if (f >= 0) d->ex_fmt = f;
        y += px(a, 38);
        char s0[40], s1[40], s2[40]; snprintf(s0, sizeof s0, "Selection (%d)", scope_count(d, EXS_SELECTION)); snprintf(s1, sizeof s1, "Visible (%d)", scope_count(d, EXS_VISIBLE)); snprintf(s2, sizeof s2, "Diagram (%d)", d->d.nn);
        const char *sc[] = { s0, s1, s2 };
        draw_text_fit(a->font_small, R, "What to export (the catalog itself never: only cards on the diagram)", -1, rect_make(x, y, w, px(a, 18)), C(T_TEXT_MUTED), 0); y += px(a, 22);
        Rect srr = rect_make(x, y, w, px(a, 28)); ui_record("dgm:ex:scope", srr);
        int s = ui_segmented(a, srr, sc, 3, d->ex_scope); if (s >= 0) d->ex_scope = s;
        y += px(a, 38);
        bool img = d->ex_fmt == EXF_PNG || d->ex_fmt == EXF_PDF || d->ex_fmt == EXF_SVG || d->ex_fmt == EXF_HTML;
        if (ui_checkbox(a, rect_make(x, y, w / 2, px(a, 22)), "Dark background", d->ex_dark, img)) d->ex_dark = !d->ex_dark;
        if (ui_checkbox(a, rect_make(x + w / 2, y, w / 2, px(a, 22)), "Transparent (PNG)", d->ex_transparent, d->ex_fmt == EXF_PNG || d->ex_fmt == EXF_SVG)) d->ex_transparent = !d->ex_transparent;
        y += px(a, 26);
        if (ui_checkbox(a, rect_make(x, y, w / 2, px(a, 22)), "Title and legend", d->ex_legend, img)) { d->ex_legend = !d->ex_legend; d->ex_title = d->ex_legend; }
        if (ui_checkbox(a, rect_make(x + w / 2, y, w / 2, px(a, 22)), "Provenance footer", d->ex_footer, img)) d->ex_footer = !d->ex_footer;
        y += px(a, 26);
        static const char *const scl[] = { "1x", "2x" };
        if (d->ex_fmt == EXF_PNG || d->ex_fmt == EXF_PDF) { int k2 = ui_segmented(a, rect_make(x, y, px(a, 120), px(a, 26)), scl, 2, d->ex_scale == 1 ? 0 : 1); if (k2 >= 0) d->ex_scale = k2 + 1; }
        if (d->ex_fmt == EXF_MERMAID) results_draw_wrapped(a->font_small, R, "Mermaid cannot carry comments, business names, notes or \"suggested\" relations: they are left out (counted in the status).", rect_make(x, y, w, px(a, 40)), C(T_TEXT_DIM), 0, true);
        if (d->ex_fmt == EXF_PDF) results_draw_wrapped(a->font_small, R, "PDF holds the diagram as an image (one page). A vector PDF arrives with the vector writer (BL-043).", rect_make(x + px(a, 130), y, w - px(a, 130), px(a, 40)), C(T_TEXT_DIM), 0, true);
        Rect ok = rect_make(b.x + b.w - px(a, 228), b.y + b.h - px(a, 44), px(a, 110), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        if (ui_button(a, ok, "Export\xE2\x80\xA6", BTN_PRIMARY, scope_count(d, d->ex_scope) > 0) || (enter && scope_count(d, d->ex_scope) > 0)) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) d->dlg = DLG_NONE;
        break;
    }
    case DLG_AI_CONSENT: {
        b = dlg_box(a, d, 560, 400); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        y += results_draw_wrapped(a->font_small, R, d->dlg_body, rect_make(x, y, w, px(a, 80)), C(T_TEXT), 0, true) + px(a, 8);
        Rect list = rect_make(x, y, w, b.y + b.h - px(a, 56) - y);
        draw_rrect(R, list, px(a, 4), C(T_EDITOR_BG));
        clip_push(R, list);
        results_draw_wrapped(a->font_mono ? a->font_mono : a->font_small, R, d->ai_payload ? d->ai_payload : "", rect_inset(list, px(a, 8), px(a, 6)), C(T_TEXT_MUTED), d->props_scroll, true);
        clip_pop(R);
        if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y) { d->props_scroll = fmaxf(0, d->props_scroll - in->wheel_y * px(a, 30)); in->wheel_y = 0; }
        Rect ok = rect_make(b.x + b.w - px(a, 208), b.y + b.h - px(a, 44), px(a, 90), px(a, 30)), ca = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:ok", ok); ui_record("dgm:dlg:cancel", ca);
        if (ui_button(a, ok, "Send", BTN_PRIMARY, true)) dg_dialog_ok(a, d);
        else if (ui_button(a, ca, "Cancel", BTN_NORMAL, true)) { d->dlg = DLG_NONE; free(d->ai_payload); d->ai_payload = NULL; }
        break;
    }
    case DLG_AI_RESULT: {
        b = dlg_box(a, d, 640, 480); dlg_frame(a, b, d->dlg_title);
        x = b.x + px(a, 18); y = b.y + px(a, 44); w = b.w - px(a, 36);
        Rect area = rect_make(x, y, w, b.y + b.h - px(a, 56) - y);
        clip_push(R, area);
        if (d->ai) { char part[2000]; size_t n = ai_partial(d->ai, part, sizeof part); results_draw_wrapped(a->font_small, R, n ? part : "Waiting for Lume Intelligence... (Esc closes; the request is cancelled)", area, C(T_TEXT_MUTED), 0, true); app_wake_in(a, 100); }
        else if (d->ai_kind == AI_EXPLAIN || !d->nprops_ai) results_draw_wrapped(a->font_small, R, d->ai_text ? d->ai_text : "", rect_make(area.x, area.y - d->props_scroll, area.w, area.h + d->props_scroll), C(T_TEXT), 0, true);
        else {
            float ly = area.y - d->props_scroll;
            for (int i = 0; i < d->nprops_ai; i++) {
                DgProposal *p = &d->props_ai[i];
                char lab[2000]; snprintf(lab, sizeof lab, "%s%s: %s", p->kind == 0 ? "Group " : p->kind == 1 ? "Name " : "Relation ", p->title, p->tables);
                float hh = results_draw_wrapped(a->font_small, R, lab, rect_make(area.x, ly, area.w - px(a, 190), px(a, 200)), C(p->accepted ? T_GREEN : p->rejected ? T_TEXT_DIM : T_TEXT), 0, true);
                if (!p->accepted && !p->rejected) {
                    char t1[40], t2[40]; snprintf(t1, sizeof t1, "dgm:ai:accept:%d", i); snprintf(t2, sizeof t2, "dgm:ai:reject:%d", i);
                    Rect ar = rect_make(area.x + area.w - px(a, 180), ly, px(a, 86), px(a, 24)), rr = rect_make(area.x + area.w - px(a, 90), ly, px(a, 86), px(a, 24));
                    ui_record(t1, ar); ui_record(t2, rr);
                    if (ui_button(a, ar, "Accept", BTN_NORMAL, true)) dg_ai_accept(a, d, i, true);
                    if (ui_button(a, rr, "Reject", BTN_QUIET, true)) dg_ai_accept(a, d, i, false);
                } else draw_text_fit(a->font_small, R, p->accepted ? "accepted" : "rejected", -1, rect_make(area.x + area.w - px(a, 180), ly, px(a, 176), px(a, 24)), C(T_TEXT_DIM), 2);
                ly += fmaxf(hh, px(a, 24)) + px(a, 8);
            }
        }
        clip_pop(R);
        if (rect_contains(area, in->mouse_x, in->mouse_y) && in->wheel_y) { d->props_scroll = fmaxf(0, d->props_scroll - in->wheel_y * px(a, 30)); in->wheel_y = 0; }
        Rect cp = rect_make(x, b.y + b.h - px(a, 44), px(a, 120), px(a, 30)), cl = rect_make(b.x + b.w - px(a, 108), b.y + b.h - px(a, 44), px(a, 90), px(a, 30));
        ui_record("dgm:dlg:copy", cp); ui_record("dgm:dlg:cancel", cl);
        if (d->ai_kind == AI_EXPLAIN && ui_button(a, cp, "Open in Tab", BTN_NORMAL, d->ai_text != NULL)) { panels_new_editor_tab(a->panels, a, d->ai_text); d->dlg = DLG_NONE; }
        if (ui_button(a, cl, "Close", BTN_NORMAL, true)) { if (d->ai) { ai_free(d->ai); d->ai = NULL; } d->dlg = DLG_NONE; }
        break;
    }
    default: break;
    }
    if (d->dlg && in->mouse_pressed) in->mouse_consumed = true;   /* modal */
}
static void apply_attrs(DgDoc *d, int node, const char *txt) {
    char *o = d->d.n[node].attrs; size_t k = 0, n = sizeof d->d.n[node].attrs;
    for (const char *p = txt; *p && k + 1 < n; p++) {
        if (*p == ';') { while (k && o[k - 1] == ' ') k--; o[k++] = '\n'; while (p[1] == ' ') p++; }
        else o[k++] = *p;
    }
    while (k && (o[k - 1] == '\n' || o[k - 1] == ' ')) k--;
    o[k] = 0;
}
void dg_dialog_ok(App *a, DgDoc *d) {
    int dlg = d->dlg;
    d->dlg = DLG_NONE;
    if (dlg == DLG_CONFIRM_ADD) {
        dgm_snapshot(&d->d);
        int added = dgm_expand(&d->d, d->confirm_ti, d->confirm_hops, 150, false);
        dg_measure(a, d); dg_layout(a, d, false); dg_iso_update(d); dg_touch(d); d->need_fit = true;
        dg_msg(d, "Added %d related tables", added);
    } else if (dlg == DLG_PROMPT) {
        const char *t = d->pf.buf; int n = d->prompt_node;
        switch (d->prompt_kind) {
        case PR_ALIAS: for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel || i == n) snprintf(d->d.n[i].alias, sizeof d->d.n[i].alias, "%s", t); dg_measure(a, d); break;
        case PR_NOTE: case PR_AREA: {
            float x = d->cm_wx, y = d->cm_wy;
            if (!d->cm_wx && !d->cm_wy) dg_screen_to_world(d, a, d->canvas.x + d->canvas.w / 2 - 100, d->canvas.y + d->canvas.h / 2 - 50, &x, &y);
            if (d->prompt_kind == PR_AREA) {   /* an area around the selection when there is one */
                float x0, y0, x1, y1;
                if (dgm_bounds(&d->d, true, &x0, &y0, &x1, &y1)) { dgm_add_note(&d->d, true, x0 - 24, y0 - 40, x1 - x0 + 48, y1 - y0 + 64, t, d->pf2.buf); break; }
                dgm_add_note(&d->d, true, x, y, 360, 240, t, d->pf2.buf);
            } else dgm_add_note(&d->d, false, x, y, 220, 100, t, d->pf2.buf);
            break;
        }
        case PR_NOTE_EDIT: if (n >= 0 && n < d->d.nnotes) { snprintf(d->d.notes[n].title, sizeof d->d.notes[n].title, "%s", t); if (d->pf2.len || !d->d.notes[n].area) snprintf(d->d.notes[n].text, sizeof d->d.notes[n].text, "%s", d->pf2.buf); } break;
        case PR_ENTITY: { if (!*t) break; float x = d->cm_wx, y = d->cm_wy; if (!x && !y) dg_screen_to_world(d, a, d->canvas.x + 80, d->canvas.y + 80, &x, &y); int e = dgm_add_entity(&d->d, t, x, y); if (e >= 0) { snprintf(d->d.n[e].attrs, sizeof d->d.n[e].attrs, "PK id"); dg_select_only(d, e); dg_measure(a, d); dg_prompt(d, PR_ATTRS, e, "Attributes", "PK id"); } break; }
        case PR_ATTRS: if (n >= 0 && n < d->d.nn) { apply_attrs(d, n, t); dg_measure(a, d); } break;
        case PR_RENAME: case PR_DIAGRAM_NAME: if (*t) snprintf(d->d.name, sizeof d->d.name, "%s", t); break;
        case PR_REL_LABEL: if (n >= 0 && n < d->d.nrels && *t) snprintf(d->d.rels[n].label, sizeof d->d.rels[n].label, "%s", t); dgm_edges(&d->d); break;
        case PR_EVIDENCE: if (n >= 0 && n < d->d.nrels) snprintf(d->d.rels[n].evidence, sizeof d->d.rels[n].evidence, "%s", t); break;
        }
        dg_touch(d);
    } else if (dlg == DLG_SCOPE) {
        char s[1200] = ""; size_t k = 0;
        for (int i = 0; i < d->nscope; i++) if (d->scope_on[i]) k += (size_t)snprintf(s + k, sizeof s - k, "%s%s", k ? "," : "", d->scope_names[i]);
        if (strcmp(s, d->d.schemas)) { dg_set_scope(a, d, s); dg_msg(d, "Scope: %s. Tables already on the diagram stay.", s[0] ? s : "default"); }
    } else if (dlg == DLG_PICKER) {
        DgCat *k = dg_cat(d); if (!k) return;
        int added = 0;
        if (d->pick_mode == 0) { for (int i = 0; i < d->npick; i++) if (d->pick_on[i]) { dgm_add_table(&d->d, d->pick[i], 0, (float)added * 10); added++; } }
        else if (d->pick_mode == 1) { for (int i = 0; i < d->npick && i < d->pick_n; i++) { dgm_add_table(&d->d, d->pick[i], 0, (float)added * 10); added++; } }
        else if (d->pick_mode == 3) { for (int i = 0; i < d->npick && i < 150; i++) { dgm_add_table(&d->d, d->pick[i], 0, (float)added * 10); added++; } }
        if (d->npick) dgm_key_of(&k->x, d->pick[0], d->d.focus, sizeof d->d.focus);
        dgm_edges(&d->d); dg_measure(a, d);
        if (added) { dg_layout(a, d, false); d->need_fit = true; }
        dg_touch(d);
        dg_msg(d, added ? "%d tables on the diagram; the rest of the schema stays searchable (Cmd+F)" : "An empty diagram: find tables with Cmd+F", added);
    } else if (dlg == DLG_EXPORT) {
        dg_ask_path(a, d, 0, d->ex_fmt);
    } else if (dlg == DLG_AI_CONSENT) {
        AiConfig cfg; char dir[1100]; snprintf(dir, sizeof dir, "%s", a->conns && a->conns->dir[0] ? a->conns->dir : ".");
        ai_config_load(dir, &cfg);
        char why[300];
        if (!ai_ready(&cfg, why, sizeof why)) { dg_msg(d, "%s (Settings > Lume Intelligence). The diagram keeps working without it.", why); free(d->ai_payload); d->ai_payload = NULL; return; }
        static const char *SYS[3] = {
            "You are Lume Intelligence inside the Lume Data ER diagram. The user sends the STRUCTURE of some database tables (names, column types, keys, foreign keys, comments; never rows). Explain how these tables relate and what business process they likely support, in short paragraphs and a list of the key relations. Separate facts (foreign keys listed) from hypotheses (say \"probably\"). Never invent tables or columns that are not listed. Answer in the user's language when the comments make it clear; otherwise English.",
            "You are Lume Intelligence inside the Lume Data ER diagram. The user sends the STRUCTURE of some database tables. Propose domain groups for a diagram. Answer ONLY one JSON object: {\"groups\":[{\"title\":\"short domain name\",\"tables\":[\"exact table names from the input\"]}]}. Use only table names that appear in the input. At most 8 groups.",
            "You are Lume Intelligence inside the Lume Data ER diagram. The user sends the STRUCTURE of some database tables. Propose a conceptual map: business names for tables and logical relations the foreign keys do not show. Answer ONLY one JSON object: {\"names\":[{\"table\":\"exact table name\",\"alias\":\"business name\"}],\"relations\":[{\"from\":\"exact table name\",\"to\":\"exact table name\",\"label\":\"verb phrase\",\"why\":\"evidence from the structure\"}]}. Every relation is a hypothesis; use only tables from the input.",
        };
        char tmp[1200]; snprintf(tmp, sizeof tmp, "%s/cache", dir);
        d->ai = ai_start(&cfg, SYS[d->ai_kind], d->ai_payload, tmp);
        free(d->ai_text); d->ai_text = NULL; d->nprops_ai = 0; d->props_scroll = 0;
        if (!d->ai) { dg_msg(d, "Lume Intelligence could not start the request"); return; }
        LOG_I("diagram: Lume Intelligence request (%s) with the structure of %d bytes of table definitions (no rows)", d->ai_kind == AI_EXPLAIN ? "explain" : d->ai_kind == AI_ORGANISE ? "domain groups" : "conceptual map", (int)strlen(d->ai_payload));
        d->dlg = DLG_AI_RESULT;
        snprintf(d->dlg_title, sizeof d->dlg_title, "%s", d->ai_kind == AI_EXPLAIN ? "Lume Intelligence: these tables" : d->ai_kind == AI_ORGANISE ? "Lume Intelligence: domain groups (proposals)" : "Lume Intelligence: conceptual map (hypotheses)");
    }
}

/* ---------------------------------------------------------------- exports */
/* a diagram holding only the chosen cards (selection or visible) and the relations between them */
static Dgm *subset(App *a, DgDoc *d, int scope) {
    Dgm *s = calloc(1, sizeof *s); dgm_init(s);
    snprintf(s->name, sizeof s->name, "%s", d->d.name); snprintf(s->conn_id, sizeof s->conn_id, "%s", d->d.conn_id); snprintf(s->conn_name, sizeof s->conn_name, "%s", d->d.conn_name);
    snprintf(s->schemas, sizeof s->schemas, "%s", d->d.schemas); snprintf(s->captured, sizeof s->captured, "%s", d->d.captured);
    s->mode = d->d.mode; s->notation = d->d.notation; s->routing = d->d.routing; s->show_types = d->d.show_types; s->conceptual = d->d.conceptual; s->show_virtual = d->d.show_virtual;
    for (int i = 0; i < d->d.nn; i++) {
        DgNode *n = &d->d.n[i]; if (n->kind == DN_STUB) continue;
        bool take = scope == EXS_DIAGRAM || (scope == EXS_SELECTION && n->sel);
        if (scope == EXS_VISIBLE) { float sx, sy; dg_world_to_screen(d, a, n->x, n->y, &sx, &sy); float k = dg_k(d, a); take = sx < d->canvas.x + d->canvas.w && sx + n->w * k > d->canvas.x && sy < d->canvas.y + d->canvas.h && sy + n->h * k > d->canvas.y; }
        if (!take) continue;
        s->n = realloc(s->n, sizeof *s->n * (size_t)(s->nn + 1)); s->capn = s->nn + 1; s->n[s->nn] = *n; s->n[s->nn].sel = false; s->nn++;
    }
    for (int r = 0; r < d->d.nrels; r++) if (dgm_find_node(s, d->d.rels[r].from) >= 0 && dgm_find_node(s, d->d.rels[r].to) >= 0) { int k = dgm_add_rel(s, d->d.rels[r].kind, NULL, NULL, NULL, NULL, NULL); if (k >= 0) s->rels[k] = d->d.rels[r]; }
    for (int i = 0; i < d->d.nnotes; i++) {   /* notes inside the chosen area */
        DgNote *nt = &d->d.notes[i]; bool inside = scope == EXS_DIAGRAM;
        if (!inside) for (int q = 0; q < s->nn; q++) if (nt->x < s->n[q].x + s->n[q].w && nt->x + nt->w > s->n[q].x && nt->y < s->n[q].y + s->n[q].h && nt->y + nt->h > s->n[q].y) inside = true;
        if (inside) { int k = dgm_add_note(s, nt->area, nt->x, nt->y, nt->w, nt->h, nt->title, nt->text); if (k >= 0) s->notes[k].color = nt->color; }
    }
    for (int i = 0; i < d->d.nhidden; i++) { char (*h)[160] = realloc(s->hidden, sizeof *h * (size_t)(s->nhidden + 1)); if (h) { s->hidden = h; memcpy(s->hidden[s->nhidden++], d->d.hidden[i], 160); } }
    dgm_bind(s, d->d.x);
    for (int i = 0; i < s->nn; i++) { float w = s->n[i].w, h = s->n[i].h; (void)w; (void)h; }
    dgm_measure_all(s, NULL, NULL);
    /* keep the on-screen sizes (measured with the real font) */
    for (int i = 0; i < s->nn; i++) { int o = dgm_find_node(&d->d, s->n[i].key); if (o >= 0) { s->n[i].w = d->d.n[o].w; s->n[i].h = d->d.n[o].h; } }
    return s;
}
static bool write_text(const char *path, const char *s, char *why, int wn) {
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb"); if (!f) { snprintf(why, (size_t)wn, "Could not write %s", path); return false; }
    size_t l = strlen(s); bool ok = fwrite(s, 1, l, f) == l; ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); snprintf(why, (size_t)wn, "Could not write the file (disk full or no permission)"); return false; }
    return true;
}
bool dg_export(App *a, DgDoc *d, int fmt, int scope, const char *path, char *why, int wn) {
    if (!path || !*path) { snprintf(why, (size_t)wn, "No file chosen"); return false; }
    char foot[600]; dg_footer(a, d, foot, sizeof foot);
    char title[200]; snprintf(title, sizeof title, "%s%s%s", d->d.name, d->d.conn_name[0] ? " \xE2\x80\x94 " : "", d->d.conn_name);
    if (fmt == EXF_PNG || fmt == EXF_PDF) {
        uint8_t *img = NULL; int w = 0, h = 0;
        bool ok = dg_render_png(a, d, scope, (float)(d->ex_scale ? d->ex_scale : 2), d->ex_dark, fmt == EXF_PNG && d->ex_transparent, fmt == EXF_PNG ? path : NULL, why, wn, fmt == EXF_PDF ? &img : NULL, &w, &h);
        if (ok && fmt == EXF_PDF) ok = pdf_write_rgba(path, title, img, w, h, w * 4, why, wn);
        free(img);
        if (ok) LOG_I("diagram: exported %s (%s)", fmt == EXF_PNG ? "PNG" : "PDF", scope == EXS_SELECTION ? "selection" : scope == EXS_VISIBLE ? "visible" : "diagram");
        return ok;
    }
    Dgm *s = subset(a, d, scope);
    if (!s->nn && fmt != EXF_JSON) { dgm_free(s); free(s); snprintf(why, (size_t)wn, "Nothing to export in this scope"); return false; }
    char *txt = NULL; int lost = 0;
    DgSvgOpt o = { .dark = d->ex_dark, .transparent = d->ex_transparent, .legend = d->ex_legend, .title = d->ex_title, .scale = 1, .title_text = title, .footer = d->ex_footer ? foot : NULL };
    switch (fmt) {
    case EXF_SVG: txt = dgm_svg(s, false, &o); break;
    case EXF_HTML: txt = dgm_html(s, &o); break;
    case EXF_MERMAID: txt = dgm_mermaid(s, &lost); break;
    case EXF_GRAPHML: txt = dgm_graphml(s); break;
    case EXF_DRAWIO: txt = dgm_drawio(s); break;
    case EXF_JSON: txt = dgm_to_json(s); break;
    default: break;
    }
    bool ok = txt && write_text(path, txt, why, wn);
    free(txt); dgm_free(s); free(s);
    if (ok && fmt == EXF_MERMAID && lost) dg_msg(d, "Mermaid saved; %d item%s it cannot carry (comments, business names, notes) left out", lost, lost == 1 ? "" : "s");
    else if (ok) dg_msg(d, "Exported %s", strrchr(path, '/') ? strrchr(path, '/') + 1 : path);
    return ok;
}
static void SDLCALL on_path(void *ud, const char *const *files, int filter) {
    (void)ud; (void)filter;
    SDL_LockMutex(DG.dlg_mu);
    if (files && files[0]) { snprintf(DG.fdlg.path, sizeof DG.fdlg.path, "%s", files[0]); DG.fdlg.ready = true; }
    DG.fdlg.pending = false;
    SDL_UnlockMutex(DG.dlg_mu);
    if (DG.a) app_wake_in(DG.a, 16);
}
/* action 0 export, 1 open a diagram file, 2 save a subset as a diagram */
void dg_ask_path(App *a, DgDoc *d, int action, int fmt) {
    char name[200]; snprintf(name, sizeof name, "%s.%s", d ? d->d.name : "diagram", dg_exf_ext(fmt));
    for (char *p = name; *p; p++) if (*p == '/' || *p == '\\' || *p == ':') *p = '_';
    const char *td = getenv("LUME_TEST_SAVE_DIR");
    if (action == 1 && getenv("LUME_TEST_OPEN_FILE")) { dgmview_open_file(a, getenv("LUME_TEST_OPEN_FILE")); return; }
    if (td && *td && action != 1) {
        char p[1100]; snprintf(p, sizeof p, "%s/%s", td, name);
        char why[300];
        if (action == 2) { Dgm *s = subset(a, d, EXS_SELECTION); bool ok = dgm_save(s, p, why, sizeof why); dgm_free(s); free(s); dg_msg(d, ok ? "Selection saved as %s" : "Could not save: %s", ok ? p : why); }
        else if (!dg_export(a, d, fmt, d->ex_scope, p, why, sizeof why)) dg_msg(d, "Export failed: %s", why);
        printf("dgm: wrote %s\n", p); fflush(stdout);
        return;
    }
    if (!DG.dlg_mu) DG.dlg_mu = SDL_CreateMutex();
    SDL_LockMutex(DG.dlg_mu);
    bool busy = DG.fdlg.pending;
    if (!busy) { DG.fdlg.pending = true; DG.fdlg.ready = false; DG.fdlg.doc = d ? d->id : 0; DG.fdlg.action = action; DG.fdlg.fmt = fmt; }
    SDL_UnlockMutex(DG.dlg_mu);
    if (busy) return;
    static SDL_DialogFileFilter flt[1]; static char fname[40], fext[16];
    snprintf(fext, sizeof fext, "%s", dg_exf_ext(fmt));
    snprintf(fname, sizeof fname, "%s", action == 1 || action == 2 || fmt == EXF_JSON ? "Lume diagram" : fmt == EXF_PNG ? "PNG image" : fmt == EXF_SVG ? "SVG image" : fmt == EXF_PDF ? "PDF" : fmt == EXF_HTML ? "Web page" : fmt == EXF_MERMAID ? "Mermaid" : fmt == EXF_GRAPHML ? "GraphML" : "draw.io");
    flt[0].name = fname; flt[0].pattern = fext;
    const char *docs = SDL_GetUserFolder(SDL_FOLDER_DOCUMENTS);
    char start[1300]; snprintf(start, sizeof start, "%s%s", docs ? docs : "", action == 1 ? "" : name);
    if (action == 1) SDL_ShowOpenFileDialog(on_path, NULL, a->window, flt, 1, start, false);
    else SDL_ShowSaveFileDialog(on_path, NULL, a->window, flt, 1, start);
}
void dg_poll_paths(App *a) {
    if (!DG.dlg_mu) return;
    SDL_LockMutex(DG.dlg_mu);
    bool ready = DG.fdlg.ready; int doc = DG.fdlg.doc, action = DG.fdlg.action, fmt = DG.fdlg.fmt; char path[1100]; snprintf(path, sizeof path, "%s", DG.fdlg.path);
    DG.fdlg.ready = false;
    SDL_UnlockMutex(DG.dlg_mu);
    if (!ready) return;
    if (action == 1) { dgmview_open_file(a, path); return; }
    DgDoc *d = NULL; for (int i = 0; i < DG_MAX_DOCS; i++) if (DG.docs[i] && DG.docs[i]->id == doc) d = DG.docs[i];
    if (!d) return;
    char why[300];
    if (action == 2) { Dgm *s = subset(a, d, EXS_SELECTION); bool ok = dgm_save(s, path, why, sizeof why); dgm_free(s); free(s); if (ok) dg_msg(d, "Selection saved as a diagram"); else dg_msg(d, "Could not save: %s", why); return; }
    if (!dg_export(a, d, fmt, d->ex_scope, path, why, sizeof why)) dg_msg(d, "Export failed: %s", why);
}

/* ---------------------------------------------------------------- the assistant (DGM-088..090) */
void dg_ai_start(App *a, DgDoc *d, int kind) {
    DgCat *k = dg_cat(d);
    if (!k || k->state != CS_READY) { dg_msg(d, "The structure is not loaded yet"); return; }
    /* the selected tables, else every table on the diagram; at most 60, never the whole catalog */
    SxPick p[60]; int np = 0; bool any_sel = dg_nsel(d) > 0;
    for (int i = 0; i < d->d.nn && np < 60; i++) { DgNode *n = &d->d.n[i]; if (n->kind != DN_TABLE || n->ti < 0 || (any_sel && !n->sel)) continue; p[np].t = n->ti; p[np].score = 1; p[np].why[0] = 0; np++; }
    if (!np) { dg_msg(d, "Select tables first (or add some to the diagram)"); return; }
    int shown = 0; char *t = schidx_render(&k->x, p, np, d->d.conn_name, 30000, &shown);
    free(d->ai_payload);
    size_t L = (t ? strlen(t) : 0) + 400;
    d->ai_payload = malloc(L);
    snprintf(d->ai_payload, L, "%s\n%s", kind == AI_EXPLAIN ? "Explain how these tables relate." : kind == AI_ORGANISE ? "Group these tables into domains for a diagram." : "Propose business names and logical relations for these tables.", t ? t : "");
    free(t);
    d->ai_kind = kind; d->props_scroll = 0;
    d->dlg = DLG_AI_CONSENT;
    snprintf(d->dlg_title, sizeof d->dlg_title, "Send the structure of %d table%s to Lume Intelligence?", shown, shown == 1 ? "" : "s");
    snprintf(d->dlg_body, sizeof d->dlg_body, "Exactly the text below goes to your configured AI provider: table and column names, types, keys, foreign keys and comments. No rows, no SQL results, no credentials. %s", np >= 60 ? "Only the first 60 tables are included." : "");
    (void)a;
}
static void parse_proposals(DgDoc *d, const char *text) {
    size_t off = 0, len = 0;
    d->nprops_ai = 0;
    if (!text || !jr_find_object(text, &off, &len)) return;
    Json j = {0};
    if (!json_parse(&j, text + off, len)) { json_free(&j); return; }
    DgCat *k = dg_cat(d);
    #define KNOWN(nm) (k && k->state == CS_READY && schidx_find(&k->x, NULL, nm) >= 0)
    if (d->ai_kind == AI_ORGANISE) {
        int arr = jr_get(&j, 0, "groups");
        for (int i = 0; arr >= 0 && i < j.nodes[arr].nchildren && d->nprops_ai < 24; i++) {
            int g = jr_child(&j, arr, i); DgProposal *p = &d->props_ai[d->nprops_ai];
            memset(p, 0, sizeof *p); p->kind = 0;
            jr_str(&j, g, "title", p->title, sizeof p->title);
            int ta = jr_get(&j, g, "tables"); size_t w = 0;
            for (int q = 0; ta >= 0 && q < j.nodes[ta].nchildren; q++) { char nm[200]; jr_node_str(&j, jr_child(&j, ta, q), nm, sizeof nm); if (KNOWN(nm)) w += (size_t)snprintf(p->tables + w, sizeof p->tables - w, "%s%s", w ? ", " : "", nm); }   /* names the model made up are dropped */
            if (p->title[0] && p->tables[0]) d->nprops_ai++;
        }
    } else if (d->ai_kind == AI_CONCEPTUAL) {
        int arr = jr_get(&j, 0, "names");
        for (int i = 0; arr >= 0 && i < j.nodes[arr].nchildren && d->nprops_ai < 24; i++) {
            int g = jr_child(&j, arr, i); DgProposal *p = &d->props_ai[d->nprops_ai]; memset(p, 0, sizeof *p); p->kind = 1;
            jr_str(&j, g, "table", p->tables, sizeof p->tables); jr_str(&j, g, "alias", p->title, sizeof p->title);
            if (p->title[0] && KNOWN(p->tables)) d->nprops_ai++;
        }
        arr = jr_get(&j, 0, "relations");
        for (int i = 0; arr >= 0 && i < j.nodes[arr].nchildren && d->nprops_ai < 24; i++) {
            int g = jr_child(&j, arr, i); DgProposal *p = &d->props_ai[d->nprops_ai]; memset(p, 0, sizeof *p); p->kind = 2;
            char from[200], to[200], why[400]; jr_str(&j, g, "from", from, sizeof from); jr_str(&j, g, "to", to, sizeof to); jr_str(&j, g, "label", p->title, sizeof p->title); jr_str(&j, g, "why", why, sizeof why);
            snprintf(p->tables, sizeof p->tables, "%s -> %s (%s)", from, to, why);
            if (KNOWN(from) && KNOWN(to) && p->title[0]) d->nprops_ai++;
        }
    }
    #undef KNOWN
    json_free(&j);
}
void dg_ai_poll(App *a, DgDoc *d) {
    if (!d->ai || !ai_done(d->ai)) { if (d->ai) app_wake_in(a, 100); return; }
    const AiResult *r = ai_result(d->ai);
    free(d->ai_text); d->ai_text = NULL;
    if (!r || !r->text || r->err[0]) d->ai_text = strdup(r && r->err[0] ? r->err : "Lume Intelligence did not answer");
    else {
        d->ai_text = strdup(r->text);
        if (d->ai_kind != AI_EXPLAIN) { parse_proposals(d, r->text); if (!d->nprops_ai) { free(d->ai_text); d->ai_text = strdup("Lume Intelligence answered without a usable proposal (no table of this diagram was named). Nothing was changed."); } }
    }
    ai_free(d->ai); d->ai = NULL;
    app_wake_in(a, 16);
}
void dg_ai_accept(App *a, DgDoc *d, int i, bool accept) {
    if (i < 0 || i >= d->nprops_ai) return;
    DgProposal *p = &d->props_ai[i];
    if (p->accepted || p->rejected) return;
    if (!accept) { p->rejected = true; return; }
    p->accepted = true;
    DgCat *k = dg_cat(d);
    if (p->kind == 0) {   /* a group: those tables on the diagram, with an area around them */
        for (int q = 0; q < d->d.nn; q++) d->d.n[q].sel = false;
        char list[1600]; snprintf(list, sizeof list, "%s", p->tables);
        for (char *t = strtok(list, ","); t; t = strtok(NULL, ",")) { while (*t == ' ') t++; int ti = k ? schidx_find(&k->x, NULL, t) : -1; if (ti >= 0) { int n = dg_add_hit(a, d, ti); if (n >= 0) d->d.n[n].sel = true; } }
        dg_layout(a, d, true);
        float x0, y0, x1, y1;
        if (dgm_bounds(&d->d, true, &x0, &y0, &x1, &y1)) { int nt = dgm_add_note(&d->d, true, x0 - 24, y0 - 40, x1 - x0 + 48, y1 - y0 + 64, p->title, ""); if (nt >= 0) d->d.notes[nt].color = 1 + i % 8; }
    } else if (p->kind == 1) {
        int ti = k ? schidx_find(&k->x, NULL, p->tables) : -1;
        if (ti >= 0) { int n = dg_add_hit(a, d, ti); if (n >= 0) snprintf(d->d.n[n].alias, sizeof d->d.n[n].alias, "%s", p->title); }
    } else {
        char from[200] = "", to[200] = ""; sscanf(p->tables, "%199s -> %199s", from, to);
        int tf = k ? schidx_find(&k->x, NULL, from) : -1, tt = k ? schidx_find(&k->x, NULL, to) : -1;
        if (tf >= 0 && tt >= 0) {
            int nf = dg_add_hit(a, d, tf), nt = dg_add_hit(a, d, tt);
            if (nf >= 0 && nt >= 0) { int r = dgm_add_rel(&d->d, DE_SUGGESTED, d->d.n[nf].key, d->d.n[nt].key, NULL, NULL, p->title); if (r >= 0) { snprintf(d->d.rels[r].author, sizeof d->d.rels[r].author, "Lume Intelligence"); const char *w = strchr(p->tables, '('); snprintf(d->d.rels[r].evidence, sizeof d->d.rels[r].evidence, "%s", w ? w + 1 : ""); } }
        }
    }
    dgm_edges(&d->d); dg_measure(a, d); dg_touch(d);
}

/* ---------------------------------------------------------------- actions */
static int target_node(DgDoc *d) {
    if (d->cm_node >= 0 && d->cm_node < d->d.nn) return d->cm_node;
    for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel) return i;
    return d->focus_node;
}
static void split_key(const char *key, char *sc, size_t sn, char *nm, size_t nn) {
    const char *dot = strrchr(key, '.');
    if (dot) { snprintf(sc, sn, "%.*s", (int)(dot - key), key); snprintf(nm, nn, "%s", dot + 1); } else { sc[0] = 0; snprintf(nm, nn, "%s", key); }
}
void dg_action(App *a, DgDoc *d, int act) {
    int n = target_node(d);
    DgNode *nd = n >= 0 && n < d->d.nn ? &d->d.n[n] : NULL;
    DgCat *k = dg_cat(d);
    Connection *c = dg_conn(a, d->conn_id);
    int e = d->cm_edge >= 0 && d->cm_edge < d->d.ne ? d->cm_edge : -1;
    DgEdge *g = e >= 0 ? &d->d.e[e] : NULL;
    switch (act) {
    case DA_FIND: d->find_focus = true; d->find_list = true; break;
    case DA_LAYOUT: dg_layout(a, d, false); d->need_fit = true; break;
    case DA_LAYOUT_SEL: dg_layout(a, d, true); break;
    case DA_FIT: dg_push_history(d); dg_fit(a, d, false); break;
    case DA_FIT_SEL: dg_push_history(d); dg_fit(a, d, true); break;
    case DA_ZOOM_IN: dg_zoom_at(a, d, 1.25f, d->canvas.x + d->canvas.w / 2, d->canvas.y + d->canvas.h / 2); break;
    case DA_ZOOM_OUT: dg_zoom_at(a, d, 1 / 1.25f, d->canvas.x + d->canvas.w / 2, d->canvas.y + d->canvas.h / 2); break;
    case DA_ZOOM_100: dg_zoom_at(a, d, 1.0f / d->d.zoom, d->canvas.x + d->canvas.w / 2, d->canvas.y + d->canvas.h / 2); break;
    case DA_MODE_NAME: case DA_MODE_KEYS: case DA_MODE_ALL: d->d.mode = act == DA_MODE_NAME ? DM_NAME : act == DA_MODE_KEYS ? DM_KEYS : DM_ALL; dg_measure(a, d); dg_touch(d); break;
    case DA_CARD_NAME: case DA_CARD_KEYS: case DA_CARD_ALL: case DA_CARD_GLOBAL:
        for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel || i == n) d->d.n[i].mode = act == DA_CARD_NAME ? DM_NAME : act == DA_CARD_KEYS ? DM_KEYS : act == DA_CARD_ALL ? DM_ALL : DM_GLOBAL;
        dg_measure(a, d); dg_touch(d); break;
    case DA_REMOVE: {
        int removed = 0;
        dgm_snapshot(&d->d);
        for (int i = d->d.nn - 1; i >= 0; i--) if (d->d.n[i].sel && d->d.n[i].kind != DN_STUB) { dgm_remove(&d->d, i); removed++; }
        if (!removed && nd && nd->kind != DN_STUB) { dgm_remove(&d->d, n); removed = 1; }
        if (removed) { dgm_edges(&d->d); dg_iso_update(d); dg_touch(d); d->focus_node = -1; dg_msg(d, "%d card%s removed from the diagram (the database is not touched)", removed, removed == 1 ? "" : "s"); }
        break;
    }
    case DA_NEIGH1: case DA_NEIGH2: case DA_NEIGH3: if (nd && nd->ti >= 0) dg_expand(a, d, nd->ti, act - DA_NEIGH1 + 1); break;
    case DA_FOCUS_HERE: if (nd) { snprintf(d->d.focus, sizeof d->d.focus, "%s", nd->key); dg_push_history(d); dg_layout(a, d, false); dg_center_on(a, d, dgm_find_node(&d->d, d->d.focus), true); } break;
    case DA_PIN: { bool to = nd ? !nd->pinned : true; for (int i = 0; i < d->d.nn; i++) if (d->d.n[i].sel || i == n) d->d.n[i].pinned = to; dg_touch(d); break; }
    case DA_ALIAS: if (nd) dg_prompt(d, PR_ALIAS, n, "Business name", nd->alias); break;
    case DA_ATTRS: if (nd && nd->kind == DN_ENTITY) { char s[1300]; snprintf(s, sizeof s, "%s", nd->attrs); for (char *p = s; *p; p++) if (*p == '\n') *p = ';'; dg_prompt(d, PR_ATTRS, n, "Attributes", s); } break;
    case DA_COPY_NAME: if (nd) { SDL_SetClipboardText(nd->kind == DN_ENTITY ? nd->key + 7 : nd->key); dg_msg(d, "Copied %s", nd->key); } break;
    case DA_OPEN_DATA: if (nd && c && nd->kind == DN_TABLE) {
            if (!c->db) { dg_msg(d, "Connect first to open the data"); break; }
            char sc[200], nm[200]; split_key(nd->key, sc, sizeof sc, nm, sizeof nm);
            char q[512], sql[900], qs[400];
            c->db->drv->quote_ident(nm, q, sizeof q);
            if (sc[0]) { c->db->drv->quote_ident(sc, qs, sizeof qs); char full[900]; snprintf(full, sizeof full, "%s.%s", qs, q); snprintf(q, sizeof q, "%s", full); }
            c->db->drv->select_sql(q, NULL, NULL, 200, sql, sizeof sql);
            if (a->active != c) app_set_active(a, c);
            app_run_sql_mode(a, c->db, sql, -1, nm, RUN_PREVIEW, q);
        } break;
    case DA_EDIT_TABLE: case DA_OPEN_DEFINITION: case DA_SHOW_DDL: if (nd && c && nd->kind == DN_TABLE) {
            if (!c->db) { dg_msg(d, "Connect first"); break; }
            if (!tdes_supported(c)) { dg_msg(d, "The table designer is not available for this driver"); break; }
            char sc[200], nm[200]; split_key(nd->key, sc, sizeof sc, nm, sizeof nm);
            if (!sc[0]) snprintf(sc, sizeof sc, "%s", !strcmp(c->driver, "sqlite") ? "main" : "");
            tdes_open_table(a, c, sc, nm, act == DA_EDIT_TABLE ? 0 : 1);   /* the designer reviews any change (RN-01) */
        } break;
    case DA_REFRESH_TABLE: case DA_REFRESH: dg_refresh(a, d, act == DA_REFRESH); break;
    case DA_SCOPE: dg_open_scope(a, d); break;
    case DA_EXPORT: dg_open_export(a, d, EXF_PNG); break;
    case DA_SELECT_ALL: for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = true; dg_msg(d, "%d selected", d->d.nn); break;
    case DA_UNDO: if (dgm_undo(&d->d)) { dg_touch(d); dg_msg(d, "Layout change undone"); } else dg_msg(d, "Nothing to undo (undo covers positions; the database is never changed here)"); break;
    case DA_REDO: if (dgm_redo(&d->d)) dg_touch(d); break;
    case DA_BACK: if (!dg_back(a, d)) dg_msg(d, "No earlier view"); break;
    case DA_FORWARD: dg_forward(a, d); break;
    case DA_MINIMAP: d->minimap = !d->minimap; break;
    case DA_PROPS: d->props = !d->props; break;
    case DA_LEGEND: d->legend = !d->legend; break;
    case DA_ISOLATE: if (d->isolate) { d->isolate = false; dg_iso_update(d); } else { if (!dg_nsel(d) && nd) nd->sel = true; d->isolate = true; d->isolate_hops = 1; dg_iso_update(d); dg_msg(d, "Isolated: the selection and its neighbours (Esc resets)"); } break;
    case DA_RESET_FOCUS: d->isolate = false; dg_iso_update(d); break;
    case DA_DISCONNECTED: {
        int cnt = 0;
        for (int i = 0; i < d->d.nn; i++) { bool linked = false; for (int q = 0; q < d->d.ne && !linked; q++) if (!d->d.e[q].hidden && (d->d.show_virtual || d->d.e[q].origin == DE_FK) && (d->d.e[q].from == i || d->d.e[q].to == i) && d->d.e[q].from != d->d.e[q].to) linked = true; d->d.n[i].sel = !linked && d->d.n[i].kind != DN_STUB; cnt += d->d.n[i].sel; }
        int partial = 0; for (int i = 0; i < d->d.nn; i++) partial += d->d.n[i].kind == DN_TABLE && d->d.n[i].sel && d->d.n[i].more == 0 && d->d.x && d->d.n[i].ti >= 0 && !d->d.x->t[d->d.n[i].ti].ncols;
        dg_msg(d, "%d table%s with no relation on this diagram%s", cnt, cnt == 1 ? "" : "s", partial ? " (some have incomplete metadata: no relation shown is not \"no relation\")" : "");
        break;
    }
    case DA_HIDE_EDGE: if (g) { char hk[160]; if (g->origin == DE_FK) snprintf(hk, sizeof hk, "%s", g->name); else snprintf(hk, sizeof hk, "rel:%s>%s:%s", d->d.n[g->from].key, d->d.n[g->to].key, g->name); dgm_toggle_hidden(&d->d, hk); dg_touch(d); dg_msg(d, "Relation hidden from the diagram (More > View > Show Hidden Relations)"); } break;
    case DA_SHOW_HIDDEN: free(d->d.hidden); d->d.hidden = NULL; d->d.nhidden = 0; dgm_edges(&d->d); dg_touch(d); break;
    case DA_EDGE_DEF: if (g) { char s[700]; snprintf(s, sizeof s, "%s %s: %s(%s) -> %s(%s)%s%s", g->origin == DE_FK ? "FOREIGN KEY" : g->origin == DE_VIRTUAL ? "virtual relation" : g->origin == DE_SUGGESTED ? "suggested relation" : "logical relationship", g->name, d->d.n[g->from].key, g->cols, d->d.n[g->to].key, g->ref_cols[0] ? g->ref_cols : "key", g->rel >= 0 && d->d.rels[g->rel].evidence[0] ? " evidence: " : "", g->rel >= 0 ? d->d.rels[g->rel].evidence : ""); dg_msg(d, "%s", s); } break;
    case DA_EDGE_FIT: if (g) { for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = i == g->from || i == g->to; dg_push_history(d); dg_fit(a, d, true); } break;
    case DA_FOLLOW: { int to = g ? g->to : -1; if (!g && nd && nd->kind == DN_STUB) { dg_msg(d, "%s is outside the scope: More > Scope... adds its schema", nd->key); break; } if (to >= 0) { dg_push_history(d); dg_select_only(d, to); dg_center_on(a, d, to, true); } break; }
    case DA_COPY_EDGE: if (g) { char s[800]; snprintf(s, sizeof s, "%s\t%s\t%s\t%s\t%s\t%s", g->name, d->d.n[g->from].key, g->cols, d->d.n[g->to].key, g->ref_cols, g->origin == DE_FK ? "foreign key" : "virtual"); SDL_SetClipboardText(s); dg_msg(d, "Relationship copied"); } break;
    case DA_VIRTUAL: if (nd) { d->linking = true; d->link_from = n; d->link_kind = nd->kind == DN_ENTITY ? DE_LOGICAL : DE_VIRTUAL; dg_msg(d, "Click the card the relation goes to (Esc cancels)"); } break;
    case DA_TOGGLE_VIRTUAL: d->d.show_virtual = !d->d.show_virtual; dgm_edges(&d->d); dg_touch(d); break;
    case DA_REL_LOGICAL: if (g && g->rel >= 0) dg_prompt(d, PR_REL_LABEL, g->rel, "Relation label", d->d.rels[g->rel].label); break;
    case DA_EVIDENCE: if (g && g->rel >= 0) dg_prompt(d, PR_EVIDENCE, g->rel, "Evidence", d->d.rels[g->rel].evidence); break;
    case DA_VERIFY_REL: if (g && g->rel >= 0) { DgRel *r = &d->d.rels[g->rel]; r->verified = !r->verified; if (r->verified && r->kind == DE_SUGGESTED) r->kind = DE_VIRTUAL; dgm_edges(&d->d); dg_touch(d); dg_msg(d, r->verified ? "Marked as verified (a documented virtual relation; still not a constraint)" : "Marked as unverified"); } break;
    case DA_DELETE_REL: if (g && g->rel >= 0) { dgm_remove_rel(&d->d, g->rel); dgm_edges(&d->d); dg_touch(d); } break;
    case DA_NOTE: dg_prompt(d, PR_NOTE, -1, "New note", ""); break;
    case DA_AREA: dg_prompt(d, PR_AREA, -1, dg_nsel(d) ? "Group area around the selection" : "Group area", ""); break;
    case DA_ENTITY: dg_prompt(d, PR_ENTITY, -1, "New logical entity (no table is created)", ""); break;
    case DA_EDIT_NOTE: if (d->cm_note >= 0 && d->cm_note < d->d.nnotes) { dg_prompt(d, PR_NOTE_EDIT, d->cm_note, "Edit note", d->d.notes[d->cm_note].title); textfield_set(&d->pf2, d->d.notes[d->cm_note].text); } break;
    case DA_DELETE_NOTE: if (d->cm_note >= 0 && d->cm_note < d->d.nnotes) { memmove(&d->d.notes[d->cm_note], &d->d.notes[d->cm_note + 1], sizeof *d->d.notes * (size_t)(d->d.nnotes - d->cm_note - 1)); d->d.nnotes--; dg_touch(d); } break;
    case DA_NOTATION: d->d.notation = d->d.notation == DNOT_CROW ? DNOT_SIMPLE : DNOT_CROW; dg_touch(d); break;
    case DA_ROUTING: d->d.routing = d->d.routing == DRT_ORTHO ? DRT_STRAIGHT : DRT_ORTHO; dg_touch(d); break;
    case DA_TYPES: d->d.show_types = !d->d.show_types; dg_measure(a, d); dg_touch(d); break;
    case DA_COMMENTS: d->d.show_comments = !d->d.show_comments; dg_touch(d); break;
    case DA_NULLS: d->d.show_nullability = !d->d.show_nullability; dg_measure(a, d); dg_touch(d); break;
    case DA_DEFAULTS: d->d.show_defaults = !d->d.show_defaults; dg_touch(d); break;
    case DA_SNAP: d->d.snap = !d->d.snap; dg_touch(d); break;
    case DA_CONCEPTUAL: d->d.conceptual = !d->d.conceptual; dg_measure(a, d); dg_touch(d); dg_msg(d, d->d.conceptual ? "Conceptual view: business names where set (physical names in the properties and exports' legend)" : "Physical view"); break;
    case DA_HIGHLIGHT_ALL: d->highlight_all = !d->highlight_all; break;
    case DA_ALIGN_L: case DA_ALIGN_C: case DA_ALIGN_R: case DA_ALIGN_T: case DA_ALIGN_M: case DA_ALIGN_B: case DA_DIST_H: case DA_DIST_V:
        dgm_snapshot(&d->d); dgm_align(&d->d, act - DA_ALIGN_L); dg_touch(d); break;
    case DA_GROUP_COMPONENTS: {
        int *comp = malloc(sizeof(int) * (size_t)(d->d.nn ? d->d.nn : 1)); int nc = dgm_components(&d->d, comp), made = 0;
        dgm_snapshot(&d->d);
        dg_layout(a, d, false);
        for (int ci = 0; ci < nc && made < 12; ci++) {
            int cnt = 0; for (int i = 0; i < d->d.nn; i++) { d->d.n[i].sel = comp[i] == ci; cnt += comp[i] == ci; }
            if (cnt < 2) continue;
            float x0, y0, x1, y1; if (dgm_bounds(&d->d, true, &x0, &y0, &x1, &y1)) { char t[80]; snprintf(t, sizeof t, "Group %d (%d tables)", made + 1, cnt); int nt = dgm_add_note(&d->d, true, x0 - 24, y0 - 40, x1 - x0 + 48, y1 - y0 + 64, t, ""); if (nt >= 0) d->d.notes[nt].color = 1 + made % 8; made++; }
        }
        for (int i = 0; i < d->d.nn; i++) d->d.n[i].sel = false;
        free(comp); dg_touch(d);
        dg_msg(d, "%d connected group%s outlined (areas are only drawings; relations between groups stay visible)", made, made == 1 ? "" : "s");
        break;
    }
    case DA_DUPLICATE: {
        Connection *cc = c; char nm[160]; snprintf(nm, sizeof nm, "%.140s copy", d->d.name);
        char *js = dgm_to_json(&d->d); if (!js) break;
        char p[1300]; { char dir[1150]; const char *slash = strrchr(d->path, '/'); snprintf(dir, sizeof dir, "%.*s", slash ? (int)(slash - d->path) : 1, slash ? d->path : "."); snprintf(p, sizeof p, "%s/%.100s-copy.lume-erd", dir, d->d.name); for (char *q = strrchr(p, '/') + 1; *q; q++) if (*q == ' ') *q = '_'; }
        Dgm t; dgm_init(&t); char why[200];
        if (dgm_from_json(&t, js, strlen(js), why, sizeof why)) { snprintf(t.name, sizeof t.name, "%s", nm); dgm_save(&t, p, why, sizeof why); }
        dgm_free(&t); free(js);
        (void)cc;
        dgmview_open_file(a, p);
        break;
    }
    case DA_SAVE_SUBSET: dg_ask_path(a, d, 2, EXF_JSON); break;
    case DA_SNAPSHOT: dgm_resign(&d->d); dg_touch(d); dg_msg(d, "The current structure is now the reference"); break;
    case DA_COMPARE: {
        int diff = dgm_compare(&d->d), missing = 0, changed = 0;
        for (int i = 0; i < d->d.nn; i++) { missing += d->d.n[i].status == DS_MISSING; changed += d->d.n[i].status == DS_CHANGED; }
        if (!diff) dg_msg(d, "No differences between the saved snapshot and the database (for the tables on the diagram)");
        else dg_msg(d, "%d changed, %d missing since the snapshot (marked on the cards; nothing was removed)", changed, missing);
        break;
    }
    case DA_COPY_IMAGE: {
        uint8_t *img = NULL; int w = 0, h = 0; char why[300];
        char tmp[1200]; snprintf(tmp, sizeof tmp, "%s/cache/dgm-clip.png", a->conns && a->conns->dir[0] ? a->conns->dir : ".");
        int scope = dg_nsel(d) ? EXS_SELECTION : EXS_VISIBLE;
        if (!dg_render_png(a, d, scope, 2, d->ex_dark, false, tmp, why, sizeof why, &img, &w, &h)) { dg_msg(d, "Copy failed: %s", why); break; }
        free(img);
        FILE *f = fopen(tmp, "rb"); long L = 0; char *bytes = NULL;
        if (f) { fseek(f, 0, SEEK_END); L = ftell(f); fseek(f, 0, SEEK_SET); bytes = malloc((size_t)L); if (bytes && fread(bytes, 1, (size_t)L, f) != (size_t)L) { free(bytes); bytes = NULL; } fclose(f); }
        remove(tmp);
        if (!bytes) { dg_msg(d, "Copy failed"); break; }
        static char *clip = NULL; static size_t clip_n = 0; free(clip); clip = bytes; clip_n = (size_t)L;
        static const char *mime[] = { "image/png" };
        SDL_SetClipboardData(NULL, NULL, NULL, mime, 1);
        extern const void *dg_clip_cb(void *ud, const char *mime_type, size_t *size);
        SDL_SetClipboardData((SDL_ClipboardDataCallback)dg_clip_cb, NULL, &clip, mime, 1);
        (void)clip_n;
        LOG_I("diagram: image copied (%d x %d, %s)", w, h, scope == EXS_SELECTION ? "selection" : "visible part");
        dg_msg(d, scope == EXS_SELECTION ? "Selection copied as an image" : "Nothing selected: the visible part was copied as an image");
        printf("dgm: copied image %dx%d\n", w, h); fflush(stdout);
        break;
    }
    case DA_RENAME: dg_prompt(d, PR_RENAME, -1, "Diagram name", d->d.name); break;
    case DA_AI_EXPLAIN: dg_ai_start(a, d, AI_EXPLAIN); break;
    case DA_AI_ORGANISE: dg_ai_start(a, d, AI_ORGANISE); break;
    case DA_AI_CONCEPTUAL: dg_ai_start(a, d, AI_CONCEPTUAL); break;
    case DA_COLOR: if (nd) { nd->color = (nd->color + 1) % 9; dg_touch(d); } break;
    default: break;
    }
    (void)k;
    d->cm_node = -1; d->cm_edge = -1;
    app_wake_in(a, 16);
}
/* the clipboard asks for the PNG bytes only when something is pasted */
const void *dg_clip_cb(void *ud, const char *mime_type, size_t *size) {
    char **clip = ud;
    if (!clip || !*clip || strcmp(mime_type, "image/png")) { *size = 0; return NULL; }
    /* the size is kept next to the pointer by the copy above: read it from the PNG chunks' end (IEND) */
    const unsigned char *p = (const unsigned char *)*clip; size_t n = 8;
    while (1) { size_t len = ((size_t)p[n] << 24) | ((size_t)p[n + 1] << 16) | ((size_t)p[n + 2] << 8) | p[n + 3]; bool end = !memcmp(p + n + 4, "IEND", 4); n += 12 + len; if (end) break; }
    *size = n;
    return *clip;
}

/* ---------------------------------------------------------------- the tab */
void dgmview_tab_body(App *a, DgDoc *d, Rect body) {
    DG.a = a;
    d->body = body;
    float bh = px(a, 34), sh = px(a, 24);
    Rect bar = rect_make(body.x, body.y, body.w, bh);
    float pw = d->props ? fminf(px(a, 300), body.w * 0.4f) : 0;
    Rect canvas = rect_make(body.x, body.y + bh, body.w - pw, body.h - bh - sh);
    Rect status = rect_make(body.x, body.y + body.h - sh, body.w, sh);
    d->canvas = canvas;
    bool modal = d->dlg != DLG_NONE;
    if (modal) { a->in.mouse_consumed = true; }   /* the dialog drawn below owns the pointer */
    if (!modal) dg_canvas(a, d, canvas);
    else { Input saved = a->in; a->in.mouse_pressed = false; a->in.mouse_rpressed = false; a->in.wheel_x = a->in.wheel_y = 0; a->in.mouse_consumed = true; dg_canvas(a, d, canvas); a->in = saved; a->in.mouse_consumed = false; }
    if (d->props) dg_props_pane(a, d, rect_make(body.x + body.w - pw, body.y + bh, pw, body.h - bh - sh));
    dg_status(a, d, status);
    if (!modal) { dg_toolbar(a, d, bar); dg_find_list(a, d); }
    else { Input saved = a->in; a->in.mouse_pressed = false; a->in.nkeys = 0; a->in.ntext = 0; dg_toolbar(a, d, bar); a->in = saved; }
    if (modal) { a->in.mouse_consumed = false; dg_dialogs(a, d); }
}
