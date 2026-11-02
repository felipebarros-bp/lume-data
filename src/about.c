/* SPDX-License-Identifier: AGPL-3.0-only */
/* About Lume Data: see about.h. */
#include "about.h"
#include "brand.h"
#include "commands.h"
#include "macos_app.h"
#include "draw.h"
#include "font.h"
#include "log.h"
#include "product.h"
#include "theme.h"
#include "ui.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

enum { V_MAIN, V_LICENSES };
/* focusable controls, in tab order */
enum { C_WEBSITE, C_DOCS, C_ISSUE, C_LICENSES, C_NOTES, C_DETAILS, C_COPY_VERSION, C_COPY_INFO, C_CLOSE, C_COUNT };

static struct {
    bool open;
    int view;
    int focus;               /* C_*, -1 none */
    int prev_app_focus;      /* App.focus to give back (ABOUT-005) */
    bool details;            /* Technical Details expanded */
    LumeBuildInfo info;      /* read once per opening (driver probes included) */
    char note[200];          /* last action's feedback, in words */
    char *licenses;          /* third-party text, built when the view opens, freed on close */
    float scroll;
    ScrollBar sb; int nlines;
    Input stash; bool stashed;
    int opens;               /* how many times it was opened (harness: no duplicate instance) */
    Font *title; float title_scale;   /* the product name's larger face, loaded while open only */
} S;

bool about_is_open(void) { return S.open; }

static App *S_app;
static Uint32 S_event;
static void activate(App *a, int c);
/* app.about.* through the global command registry (menus, palette, toolbar, harness) */
static bool en_open(const LumeCtx *c, char *why, size_t n) { (void)c; if (!S.open) { snprintf(why, n, "The About is not open"); return false; } return true; }
static bool en_link(int ctl, char *why, size_t n) { if (!S.open) { snprintf(why, n, "The About is not open"); return false; } return url_allowed(ctl == 0 ? product_current()->website_url : ctl == 1 ? product_current()->docs_url : ctl == 2 ? product_current()->issues_url : product_current()->release_notes_url, why, (int)n); }
static bool en_web(const LumeCtx *c, char *w, size_t n) { (void)c; return en_link(0, w, n); }
static bool en_doc(const LumeCtx *c, char *w, size_t n) { (void)c; return en_link(1, w, n); }
static bool en_iss(const LumeCtx *c, char *w, size_t n) { (void)c; return en_link(2, w, n); }
static bool en_rel(const LumeCtx *c, char *w, size_t n) { (void)c; return en_link(3, w, n); }
static void inv(void *ud, const LumeCtx *c) { (void)c; if (S_app && S.open) activate(S_app, (int)(intptr_t)ud); }
void about_init(App *a) {
    S_app = a;
    static const struct { const char *id, *label, *syn; int ctl; bool (*en)(const LumeCtx *, char *, size_t); } cmds[] = {
        { "app.about.close", "About: Close", "dismiss", 8, en_open },
        { "app.about.copy_info", "About: Copy Info", "diagnostics support version", 7, en_open },
        { "app.about.open_licenses", "About: Third-party Licenses", "legal notices credits", 3, en_open },
        { "app.about.open_website", "About: Website", "site home", 0, en_web },
        { "app.about.open_documentation", "About: Documentation", "docs help manual", 1, en_doc },
        { "app.about.report_issue", "About: Report an Issue", "bug support", 2, en_iss },
        { "app.about.open_release_notes", "About: Release Notes", "changelog news", 4, en_rel },
    };
    for (size_t i = 0; i < sizeof cmds / sizeof cmds[0]; i++) {
        Command c; memset(&c, 0, sizeof c);
        c.id = cmds[i].id; c.label = cmds[i].label; c.group = "Help"; c.synonyms = cmds[i].syn; c.icon = "info";
        c.enabled = cmds[i].en; c.invoke = inv; c.ud = (void *)(intptr_t)cmds[i].ctl; c.module = "lume.about";
        commands_register(&c);
    }
    /* check_updates is not registered: there is no update service (ABOUT-025) */
    /* the native macOS application menu's About opens this one too (BL-022) */
    S_event = SDL_RegisterEvents(1);
    if (macos_route_about(S_event, product_current()->name)) LOG_I("native application menu: About routed to Lume's About");
}
Uint32 about_event_type(void) { return S_event; }

void about_open(App *a) {
    S.opens++;
    if (S.open) { S.view = V_MAIN; return; }   /* the same instance comes back (scenario 1) */
    S.open = true; S.view = V_MAIN; S.focus = C_CLOSE; S.details = false; S.note[0] = 0; S.scroll = 0;
    S.prev_app_focus = a->focus;
    Uint64 t0 = SDL_GetTicks();
    product_build_info(&S.info, true);   /* real driver availability (probed once per process), no connection */
    LOG_I("About opened (%s, %s) in %d ms", S.info.version, S.info.channel, (int)(SDL_GetTicks() - t0));
}

void about_close(App *a) {
    if (!S.open) return;
    S.open = false;
    free(S.licenses); S.licenses = NULL;
    if (S.title) { font_destroy(S.title); S.title = NULL; }   /* nothing of the About stays in memory (ABOUT-030) */
    a->focus = S.prev_app_focus;   /* back to the editor / grid that had it */
}

/* ---- links and actions ---- */
#include <stdint.h>

static const char *link_url(int c) {
    const LumeProduct *p = product_current();
    switch (c) {
    case C_WEBSITE: return p->website_url;
    case C_DOCS: return p->docs_url;
    case C_ISSUE: return p->issues_url;
    case C_NOTES: return p->release_notes_url;
    default: return NULL;
    }
}
static bool control_enabled(int c) {
    if (c == C_WEBSITE || c == C_DOCS || c == C_ISSUE || c == C_NOTES) { char why[120]; return url_allowed(link_url(c), why, sizeof why); }
    return true;
}

static char *read_text(App *a, const char *rel) {
    char path[1300]; snprintf(path, sizeof path, "%s/licenses/%s", a->assets_dir, rel);
    size_t n = 0; void *d = SDL_LoadFile(path, &n);
    if (!d) return NULL;
    char *s = malloc(n + 1); memcpy(s, d, n); s[n] = 0; SDL_free(d);
    return s;
}
/* The third-party view: the reviewed manifest, the version this process really uses, and each
 * license text shipped with the app (ABOUT-020) */
static void build_licenses(App *a) {
    free(S.licenses);
    size_t cap = 65536, o = 0; char *b = malloc(cap); b[0] = 0;
#define ADD(...) do { int k_ = snprintf(b + o, cap - o, __VA_ARGS__); if (k_ > 0) o += (size_t)k_ < cap - o ? (size_t)k_ : cap - o - 1; } while (0)
    const LumeProduct *p = product_current();
    ADD("%s: %s\n\n", product_text(PT_LUME_LICENSE), p->license_name ? p->license_name : product_text(PT_LICENSE_UNPUBLISHED));
    char *man = read_text(a, "THIRD_PARTY.txt");
    if (!man) ADD("The third-party manifest is missing from this installation (assets/licenses/THIRD_PARTY.txt).\n");
    for (char *line = man ? strtok(man, "\n") : NULL; line; line = strtok(NULL, "\n")) {
        if (line[0] == '#') continue;
        char f[5][300] = { { 0 } }; int nf = 0; char *s = line;
        while (nf < 5) { char *bar = strstr(s, " | "); int l = bar ? (int)(bar - s) : (int)strlen(s); snprintf(f[nf++], 300, "%.*s", l, s); if (!bar) break; s = bar + 3; }
        const char *ver = "";
        char vbuf[64] = "";
        if (!strcmp(f[4], "sdl")) ver = S.info.sdl;
        else if (!strcmp(f[4], "sqlite")) ver = S.info.sqlite;
        else if (!strcmp(f[4], "libpq") || !strcmp(f[4], "oci")) {
            for (int i = 0; i < S.info.ndrivers; i++)
                if ((!strcmp(f[4], "libpq") && !strcmp(S.info.drivers[i].name, "PostgreSQL")) || (!strcmp(f[4], "oci") && !strcmp(S.info.drivers[i].name, "Oracle")))
                    snprintf(vbuf, sizeof vbuf, "%s%s", S.info.drivers[i].available ? "in use here: " : "", S.info.drivers[i].version);
            ver = vbuf;
        }
        ADD("%s%s%s\n    License: %s\n    Use: %s\n", f[0], ver[0] ? "  \xC2\xB7  " : "", ver, f[1], f[3]);
        if (strcmp(f[2], "-")) {
            char *t = read_text(a, f[2]);
            if (t) { ADD("\n%s\n", t); free(t); } else ADD("    (license file %s missing from this installation)\n", f[2]);
        }
        ADD("\n------------------------------------------------------------\n\n");
    }
    free(man);
#undef ADD
    S.licenses = b; S.nlines = 0;
}

static void copy(App *a, const char *text, const char *what) {
    if (SDL_SetClipboardText(text)) snprintf(S.note, sizeof S.note, "%s: %s", what, product_text(PT_COPIED));
    else snprintf(S.note, sizeof S.note, "The clipboard is not available: %s", SDL_GetError());
    (void)a;
}

static void activate(App *a, int c) {
    char why[200];
    switch (c) {
    case C_WEBSITE: case C_DOCS: case C_ISSUE: case C_NOTES:
        if (url_open(link_url(c), why, sizeof why)) snprintf(S.note, sizeof S.note, "Opened in the browser");
        else snprintf(S.note, sizeof S.note, "%s", why);   /* discreet, never blocking (ABOUT-018) */
        break;
    case C_LICENSES: S.view = V_LICENSES; S.scroll = 0; build_licenses(a); break;
    case C_DETAILS: S.details = !S.details; break;
    case C_COPY_VERSION: { char v[160]; product_version_line(&S.info, v, sizeof v); copy(a, v, "Version"); break; }
    case C_COPY_INFO: { char t[1024]; product_copy_info(product_current(), &S.info, t, sizeof t); copy(a, t, "Application information"); break; }
    case C_CLOSE: about_close(a); break;
    }
}

/* ---- frame ---- */

void about_pre_update(App *a) {
    if (!S.open) return;
    Input *in = &a->in;
    /* keyboard first (ABOUT-027) */
    for (int i = 0; i < in->nkeys; i++) {
        KeyPress *k = &in->keys[i];
        if (k->used) continue;
        if (k->key == SDLK_ESCAPE) { k->used = true; if (S.view == V_LICENSES) { S.view = V_MAIN; S.focus = C_LICENSES; } else { about_close(a); return; } }
        else if (k->key == SDLK_TAB && S.view == V_MAIN) {
            k->used = true;
            int dir = (k->mods & SDL_KMOD_SHIFT) ? -1 : 1;
            for (int n = 0; n < C_COUNT; n++) { S.focus = (S.focus + dir + C_COUNT) % C_COUNT; if (control_enabled(S.focus)) break; }
        } else if ((k->key == SDLK_RETURN || k->key == SDLK_KP_ENTER || k->key == SDLK_SPACE) && S.view == V_MAIN && S.focus >= 0) {
            k->used = true; activate(a, S.focus);
            if (!S.open) return;
        }
    }
    /* the rest of the frame's input belongs to the About: hide it from the workspace */
    S.stash = *in; S.stashed = true;
    in->mouse_pressed = in->mouse_released = in->mouse_rpressed = false; in->mouse_consumed = true;
    in->nkeys = 0; in->ntext = 0; in->wheel_x = in->wheel_y = 0;
}

static float row(App *a, Rect card, float y, const char *k, const char *v) {
    float rh = px(a, 22);
    draw_text_fit(a->font_small, a->renderer, k, -1, rect_make(card.x + px(a, 16), y, px(a, 140), rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    draw_text_fit(a->font_small, a->renderer, v, -1, rect_make(card.x + px(a, 160), y, card.w - px(a, 176), rh), color_hex(T_TEXT), ALIGN_LEFT);
    char tg[80]; snprintf(tg, sizeof tg, "about:value:%s", k); ui_record(tg, rect_make(card.x + px(a, 160), y, card.w - px(a, 176), rh));
    return y + rh;
}

/* a text link: underlined when it opens something (not colour only), dim and explained otherwise */
static bool link(App *a, Rect rc, int c, const char *label) {
    Input *in = &a->in;
    bool en = control_enabled(c), hov = rect_contains(rc, in->mouse_x, in->mouse_y);
    Color col = color_hex(!en ? T_TEXT_DIM : hov || S.focus == c ? T_TEXT : T_ACCENT);
    float tw = font_width(a->font_small, label, -1);
    draw_text_fit(a->font_small, a->renderer, label, -1, rc, col, ALIGN_LEFT);
    if (en) draw_rect(a->renderer, rect_make(rc.x, rc.y + rc.h - px(a, 5), tw, px(a, 1)), col);
    if (S.focus == c) draw_rect_outline(a->renderer, rect_make(rc.x - px(a, 4), rc.y + px(a, 1), tw + px(a, 8), rc.h - px(a, 2)), px(a, 1.5f), color_hex(T_ACCENT));
    char tg[64]; snprintf(tg, sizeof tg, "about:%s", label); ui_record(tg, rc);
    if (hov && !en) {
        /* why it does not open, in words (ABOUT-014 / 015) */
        snprintf(S.note, sizeof S.note, "%s: %s", label, c == C_NOTES || c == C_DOCS || c == C_WEBSITE || c == C_ISSUE ? product_text(PT_NOT_CONFIGURED) : "");
    }
    if (hov && en) a->want_cursor = CURSOR_HAND;
    if (hov && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; S.focus = c; if (en) return true; }
    return false;
}

void about_draw(App *a) {
    if (!S.open) return;
    if (S.stashed) {
        bool down = a->in.mouse_down; float mx = a->in.mouse_x, my = a->in.mouse_y;
        a->in = S.stash; a->in.mouse_down = down; a->in.mouse_x = mx; a->in.mouse_y = my; a->in.nkeys = 0; a->in.ntext = 0;
        S.stashed = false;
    }
    SDL_Renderer *r = a->renderer;
    const LumeProduct *p = product_current();
    /* size: ~500 pt wide, content height, never taller than the window (ABOUT-029) */
    float w = fminf(px(a, 520), a->width - px(a, 32));
    float card_h = px(a, 22) * (6 + (S.details ? 5 : 0)) + px(a, 40);
    float h = S.view == V_LICENSES ? a->height * 0.8f : px(a, 44 + 120 + 60) + card_h + px(a, 150);
    if (h > a->height - px(a, 32)) h = a->height - px(a, 32);
    Rect rc = rect_make((a->width - w) / 2, (a->height - h) / 2, w, h);
    draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 120));
    for (int s = 3; s >= 1; s--) draw_rrect(r, rect_make(rc.x - s * px(a, 2), rc.y + px(a, 3), rc.w + s * px(a, 4), rc.h + s * px(a, 3)), px(a, 12) + s * px(a, 2), color_alpha(color_hex(0x000000), 40));
    draw_rrect(r, rc, px(a, 12), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(rc, px(a, 1), px(a, 1)), px(a, 11), color_hex(T_PANEL_ALT));
    ui_record("about:dialog", rc);
    /* title bar with a close control (ABOUT-005) */
    draw_text_fit(a->font_ui, r, S.view == V_LICENSES ? product_text(PT_THIRD_PARTY) : product_text(PT_ABOUT_TITLE), -1, rect_make(rc.x + px(a, 18), rc.y + px(a, 8), rc.w - px(a, 70), px(a, 28)), color_hex(T_TEXT), ALIGN_LEFT);
    Rect cx = rect_make(rc.x + rc.w - px(a, 38), rc.y + px(a, 10), px(a, 24), px(a, 24));
    bool hx = rect_contains(cx, a->in.mouse_x, a->in.mouse_y);
    if (hx) draw_rrect(r, cx, px(a, 12), color_hex(T_HOVER));
    draw_cross(r, cx.x + cx.w / 2, cx.y + cx.h / 2, px(a, 8), px(a, 1.5f), color_hex(hx ? T_TEXT : T_TEXT_MUTED));
    ui_record("about:x", cx);
    if (hx && a->in.mouse_pressed && !a->in.mouse_consumed) { a->in.mouse_consumed = true; about_close(a); return; }
    draw_rect(r, rect_make(rc.x, rc.y + px(a, 44), rc.w, px(a, 1)), color_hex(T_BORDER));

    if (S.view == V_LICENSES) {
        Rect body = rect_make(rc.x + px(a, 18), rc.y + px(a, 54), rc.w - px(a, 36), rc.h - px(a, 54) - px(a, 60));
        draw_rect(r, body, color_hex(T_EDITOR_BG));
        float lh = font_height(a->font_small) + px(a, 3);
        if (rect_contains(body, a->in.mouse_x, a->in.mouse_y) && a->in.wheel_y != 0) S.scroll -= a->in.wheel_y / lh;
        if (S.scroll < 0) S.scroll = 0;
        if (!S.nlines && S.licenses) { S.nlines = 1; for (const char *q = S.licenses; *q; q++) S.nlines += *q == '\n'; }
        float vis = (body.h - px(a, 6)) / lh;
        if (S.scroll > S.nlines - vis + 1) S.scroll = S.nlines - vis + 1 > 0 ? S.nlines - vis + 1 : 0;
        ui_scrollbar_input_rows(a, &S.sb, body, (float)S.nlines + 1, lh, &S.scroll);
        clip_push(r, body);
        int ln = 0; float y = body.y + px(a, 6);
        for (const char *s = S.licenses ? S.licenses : ""; *s; ln++) {
            const char *e = strchr(s, '\n'); int l = e ? (int)(e - s) : (int)strlen(s);
            if (ln >= (int)S.scroll && y < body.y + body.h) {
                bool head = l > 0 && s[0] != ' ' && s[0] != '-' && (ln == 0 || s[-2] == '\n');
                draw_text_fit(a->font_small, r, s, l, rect_make(body.x + px(a, 10), y, body.w - px(a, 20), lh), color_hex(head ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
                y += lh;
            }
            s = e ? e + 1 : s + l;
        }
        clip_pop(r);
        ui_scrollbar_draw_rows(a, &S.sb, body, (float)S.nlines + 1, lh, S.scroll);
        ui_record("about:licenses", body);
        Rect back = rect_make(rc.x + rc.w - px(a, 118), rc.y + rc.h - px(a, 46), px(a, 100), px(a, 30));
        if (ui_button(a, back, product_text(PT_BACK), BTN_NORMAL, true)) { S.view = V_MAIN; S.focus = C_LICENSES; }
        ui_record("about:back", back);
        a->in.mouse_consumed = true;
        return;
    }

    /* identity: symbol, name, version line, tagline, description (ABOUT-002..004) */
    float y = rc.y + px(a, 62);
    Rect logo = rect_make(rc.x + px(a, 24), y, px(a, 84), px(a, 84));
    brand_draw_symbol(a, logo);
    ui_record("about:logo", logo);
    float tx = logo.x + logo.w + px(a, 20), tw = rc.x + rc.w - tx - px(a, 20);
    if (!S.title || S.title_scale != a->scale) {
        if (S.title) font_destroy(S.title);
        char fp[1300]; snprintf(fp, sizeof fp, "%s/fonts/Inter-Variable.ttf", a->assets_dir);
        S.title = font_load(r, fp, roundf(24 * a->scale)); S.title_scale = a->scale;
    }
    draw_text_fit(S.title ? S.title : a->font_ui, r, p->name, -1, rect_make(tx, y + px(a, 4), tw, px(a, 36)), color_hex(T_TEXT), ALIGN_LEFT);
    ui_record("about:name", rect_make(tx, y + px(a, 8), tw, px(a, 30)));
    char vl[120]; snprintf(vl, sizeof vl, "Version %s \xC2\xB7 %s", S.info.version, S.info.channel);
    draw_text_fit(a->font_small, r, vl, -1, rect_make(tx, y + px(a, 38), tw, px(a, 22)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    y += px(a, 96);
    draw_text_fit(a->font_ui, r, p->tagline, -1, rect_make(rc.x + px(a, 24), y, rc.w - px(a, 48), px(a, 24)), color_hex(T_TEXT), ALIGN_LEFT);
    draw_text_fit(a->font_small, r, p->description, -1, rect_make(rc.x + px(a, 24), y + px(a, 24), rc.w - px(a, 48), px(a, 22)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("about:tagline", rect_make(rc.x + px(a, 24), y, rc.w - px(a, 48), px(a, 46)));
    y += px(a, 58);
    /* the technical card (ABOUT-007..013) */
    Rect card = rect_make(rc.x + px(a, 20), y, rc.w - px(a, 40), card_h);
    draw_rrect(r, card, px(a, 8), color_hex(T_BORDER));
    draw_rrect(r, rect_inset(card, px(a, 1), px(a, 1)), px(a, 7), color_hex(T_PANEL));
    draw_text_fit(a->font_small, r, product_text(PT_APP_INFO), -1, rect_make(card.x + px(a, 16), card.y + px(a, 8), card.w, px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    float cy = card.y + px(a, 30);
    char plat[80]; snprintf(plat, sizeof plat, "%s \xC2\xB7 %s", S.info.platform, S.info.arch);
    cy = row(a, card, cy, product_text(PT_VERSION), S.info.version);
    cy = row(a, card, cy, product_text(PT_BUILD), S.info.build);
    cy = row(a, card, cy, product_text(PT_CHANNEL), S.info.channel);
    cy = row(a, card, cy, product_text(PT_PLATFORM), plat);
    cy = row(a, card, cy, product_text(PT_ENGINE), S.info.engine);
    cy = row(a, card, cy, product_text(PT_DRIVERS), S.info.drivers_text);
    if (S.details) {
        cy = row(a, card, cy, "Configured (UTC)", S.info.configured[0] ? S.info.configured : product_text(PT_NOT_AVAILABLE));
        cy = row(a, card, cy, "Compiler", S.info.compiler);
        cy = row(a, card, cy, "SDL3", S.info.sdl);
        cy = row(a, card, cy, "SQLite", S.info.sqlite[0] ? S.info.sqlite : product_text(PT_NOT_AVAILABLE));
        char dv[200] = ""; int o = 0;
        for (int i = 0; i < S.info.ndrivers; i++) if (strcmp(S.info.drivers[i].name, "SQLite"))
            o += snprintf(dv + o, sizeof dv - (size_t)o, "%s%s: %s", o ? "; " : "", S.info.drivers[i].name, S.info.drivers[i].version);
        cy = row(a, card, cy, "Other drivers", dv);
    }
    (void)cy;
    y = card.y + card.h + px(a, 14);
    /* links (ABOUT-014..017, 020, 013) */
    struct { int c; int t; } L[] = { { C_WEBSITE, PT_WEBSITE }, { C_DOCS, PT_DOCS }, { C_ISSUE, PT_ISSUE }, { C_LICENSES, PT_LICENSES }, { C_NOTES, PT_RELEASE_NOTES }, { C_DETAILS, PT_DETAILS } };
    float lx = rc.x + px(a, 24), ly = y;
    S.note[0] = S.note[0] && strstr(S.note, product_text(PT_NOT_CONFIGURED)) ? 0 : S.note[0];
    for (int i = 0; i < 6; i++) {
        const char *label = product_text(L[i].t);
        char lbl[64]; snprintf(lbl, sizeof lbl, "%s%s", label, L[i].c == C_DETAILS ? (S.details ? " (hide)" : "") : "");
        float lw = font_width(a->font_small, lbl, -1) + px(a, 22);
        if (lx + lw > rc.x + rc.w - px(a, 20)) { lx = rc.x + px(a, 24); ly += px(a, 24); }
        if (link(a, rect_make(lx, ly, lw - px(a, 22), px(a, 24)), L[i].c, lbl)) activate(a, L[i].c);
        lx += lw;
    }
    y = ly + px(a, 34);
    /* legal line: only what is confirmed (ABOUT-019 / 021) */
    char legal[300];
    snprintf(legal, sizeof legal, "%s%s%s", p->copyright ? p->copyright : "", p->copyright ? "  \xC2\xB7  " : "", p->license_name ? p->license_name : product_text(PT_LICENSE_UNPUBLISHED));
    draw_text_fit(a->font_small, r, legal, -1, rect_make(rc.x + px(a, 24), y, rc.w - px(a, 48), px(a, 20)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    ui_record("about:legal", rect_make(rc.x + px(a, 24), y, rc.w - px(a, 48), px(a, 20)));
    /* feedback, in words */
    if (S.note[0]) draw_text_fit(a->font_small, r, S.note, -1, rect_make(rc.x + px(a, 24), rc.y + rc.h - px(a, 76), rc.w - px(a, 48), px(a, 20)), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    ui_record("about:note", rect_make(rc.x + px(a, 24), rc.y + rc.h - px(a, 76), rc.w - px(a, 48), px(a, 20)));
    /* footer buttons */
    float bh = px(a, 30), by = rc.y + rc.h - bh - px(a, 16);
    Rect bclose = rect_make(rc.x + rc.w - px(a, 108), by, px(a, 90), bh);
    Rect binfo = rect_make(bclose.x - px(a, 110), by, px(a, 100), bh);
    Rect bver = rect_make(binfo.x - px(a, 130), by, px(a, 120), bh);
    struct { Rect rc; int c; int t; int style; } B[] = { { bver, C_COPY_VERSION, PT_COPY_VERSION, BTN_QUIET }, { binfo, C_COPY_INFO, PT_COPY_INFO, BTN_NORMAL }, { bclose, C_CLOSE, PT_CLOSE, BTN_PRIMARY } };
    for (int i = 0; i < 3; i++) {
        if (ui_button(a, B[i].rc, product_text(B[i].t), B[i].style, true)) { S.focus = B[i].c; activate(a, B[i].c); if (!S.open) return; }
        if (S.focus == B[i].c) draw_rect_outline(r, rect_inset(B[i].rc, -px(a, 3), -px(a, 3)), px(a, 1.5f), color_hex(T_ACCENT));
        char tg[48]; snprintf(tg, sizeof tg, "about:btn:%s", product_text(B[i].t)); ui_record(tg, B[i].rc);
    }
    a->in.mouse_consumed = true;   /* nothing below the About takes this frame's click */
}

void about_dump(App *a) {
    printf("aboutstate open=%d view=%s focus=%d details=%d opens=%d fallback=%d version=\"%s\" build=\"%s\" channel=\"%s\" platform=\"%s %s\" drivers=\"%s\" note=\"%s\" app_focus=%d\n",
           S.open, S.view == V_LICENSES ? "licenses" : "main", S.focus, S.details, S.opens, brand_fallback_used(), S.info.version, S.info.build, S.info.channel,
           S.info.platform, S.info.arch, S.info.drivers_text, S.note, a->focus);
    fflush(stdout);
}
