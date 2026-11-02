/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Markdown notes module: the functional example of the panel extension API (PNL-047)
 * ==================================================================================
 *
 * A module for a language that is not SQL. It adds two panels to the workspace without any
 * change to the shell (panels.c) or the engine (dock.c). It only uses the public API:
 *
 *   lume.md.outline   headings of <prefs>/notes.md, indented by level
 *   lume.md.stats     words, lines, headings, links and code blocks of the same file
 *
 * Both panels share one parsed document (MdDoc). Each panel gets its own view from the view
 * factory (md_create), counted in doc.refs, so the file is read when the first view is created
 * and freed with the last one (md_destroy).
 *
 * The lifecycle hook drives the work. A panel polls the file for changes (once a second) only
 * while it is visible (md_visibility); a hidden or collapsed panel costs nothing. The context-menu
 * hook adds "Reload notes.md", "Copy outline" and "Show file path" to the panel's menu, after
 * the shell's own items.
 *
 * Registration: main.c calls panels_add_module("lume.markdown", markdown_module_init) before
 * panels_create. Turn it off with LUME_DISABLE_MODULES=lume.markdown or the line
 * "lume.markdown=off" in <prefs>/modules.txt.
 *
 * Main thread only: hooks are called from the frame.
 */
#include "ui.h"
#include "mod_markdown.h"
#include <sys/stat.h>
#include "draw.h"
#include "font.h"
#include "theme.h"
#include "conn.h"
#include "log.h"
#include "commands.h"

enum { MD_OUTLINE, MD_STATS };
enum { MDM_RELOAD = 1, MDM_COPY, MDM_PATH };   /* the module's own context-menu ids */

#define MD_MAX_HEADS 256
typedef struct { int level; char title[120]; } MdHead;   /* one "#".."######" heading */

/* The parsed notes file, shared by both panels. */
typedef struct MdDoc {
    char path[1100];
    time_t mtime; off_t size;            /* of the last read, to notice edits */
    MdHead heads[MD_MAX_HEADS]; int nheads;
    int words, lines, links, code_blocks;
    bool missing;                        /* the file could not be read */
    int refs;                            /* views alive */
    Uint64 last_check;                   /* SDL_GetTicks of the last stat() */
} MdDoc;
static MdDoc doc;

/* What the view factory returns for each panel. */
typedef struct MdView { int kind; bool shown; float scroll; ScrollBar sb; } MdView;

static const char *sample =
    "# Lume notes\n\n"
    "Notes kept next to the queries. This panel is a Markdown module that uses the panel API:\n"
    "move it, hide it, float it; the layout remembers it like any other panel.\n\n"
    "## Queries to revisit\n"
    "- [Revenue by city](https://example.com/revenue)\n"
    "- Orders still open\n\n"
    "## Ideas\n"
    "### Panels\n"
    "```sql\nSELECT 1;\n```\n"
    "### Shortcuts\n";

/* Reads and parses doc.path; writes the sample first when the file does not exist. */
static void md_load(void) {
    struct stat st;
    if (stat(doc.path, &st) != 0) {
        FILE *w = fopen(doc.path, "w");
        if (w) { fputs(sample, w); fclose(w); LOG_I("markdown: created %s", doc.path); }
    }
    doc.nheads = doc.words = doc.lines = doc.links = doc.code_blocks = 0;
    FILE *f = fopen(doc.path, "r");
    doc.missing = f == NULL;
    if (!f) return;
    if (stat(doc.path, &st) == 0) { doc.mtime = st.st_mtime; doc.size = st.st_size; }
    char line[2048]; bool fence = false;
    while (fgets(line, sizeof line, f)) {
        doc.lines++;
        if (!strncmp(line, "```", 3)) { fence = !fence; if (fence) doc.code_blocks++; continue; }
        for (const char *p = line; (p = strstr(p, "](")) != NULL; p += 2) doc.links++;
        bool in_word = false;
        for (const char *p = line; *p; p++) {
            bool ws = *p == ' ' || *p == '\t' || *p == '\n' || *p == '\r';
            if (!ws && !in_word) doc.words++;
            in_word = !ws;
        }
        if (fence) continue;
        int lv = 0; while (line[lv] == '#') lv++;
        if (lv >= 1 && lv <= 6 && line[lv] == ' ' && doc.nheads < MD_MAX_HEADS) {
            MdHead *h = &doc.heads[doc.nheads++];
            h->level = lv;
            snprintf(h->title, sizeof h->title, "%s", line + lv + 1);
            h->title[strcspn(h->title, "\r\n")] = 0;
        }
    }
    fclose(f);
}

/* While a panel is visible: re-read the file when its time or size changed (checked each second). */
static void md_poll(App *a) {
    Uint64 now = SDL_GetTicks();
    if (now - doc.last_check >= 1000) {
        doc.last_check = now;
        struct stat st;
        if (stat(doc.path, &st) == 0 && (st.st_mtime != doc.mtime || st.st_size != doc.size)) { md_load(); LOG_I("markdown: %s changed, reloaded", doc.path); }
    }
    app_wake_in(a, 1000);   /* only while drawn: a hidden panel stops the polling */
}

/* View factory: one view per panel; the first one reads the file. ud carries the panel kind. */
static void *md_create(void *ud, void *ctx) {
    App *a = ctx;
    MdView *v = calloc(1, sizeof *v);
    v->kind = (int)(intptr_t)ud;
    if (doc.refs++ == 0) {
        snprintf(doc.path, sizeof doc.path, "%snotes.md", a->conns ? a->conns->dir : "");
        md_load();
    }
    LOG_I("markdown: %s view created", v->kind == MD_OUTLINE ? "outline" : "stats");
    return v;
}

/* Frees a view; the last one also drops the document. */
static void md_destroy(void *view, void *ctx) {
    (void)ctx;
    MdView *v = view;
    LOG_I("markdown: %s view destroyed", v->kind == MD_OUTLINE ? "outline" : "stats");
    free(v);
    if (--doc.refs == 0) memset(&doc, 0, sizeof doc);
}

/* Lifecycle: the panel's content started or stopped being drawn. */
static void md_visibility(void *view, void *ctx, bool shown) {
    (void)ctx;
    MdView *v = view;
    v->shown = shown;
    LOG_I("markdown: %s %s", v->kind == MD_OUTLINE ? "outline" : "stats", shown ? "shown (polling notes.md)" : "hidden (polling stopped)");
}

/* Draws one panel: a header line with the file name, then the outline or the statistics. */
static void md_draw(void *view, void *ctx, DockRect drc, bool focused) {
    (void)focused;
    MdView *v = view; App *a = ctx;
    SDL_Renderer *r = a->renderer;
    Rect rc = rect_make(drc.x, drc.y, drc.w, drc.h);
    md_poll(a);
    draw_rect(r, rc, color_hex(T_PANEL));
    float pad = px(a, 12), rh = px(a, 24), y = rc.y + px(a, 8);
    char head[200];
    if (doc.missing) snprintf(head, sizeof head, "notes.md could not be read");
    else if (v->kind == MD_OUTLINE) snprintf(head, sizeof head, "notes.md  \xC2\xB7  %d heading%s", doc.nheads, doc.nheads == 1 ? "" : "s");
    else snprintf(head, sizeof head, "notes.md  \xC2\xB7  Markdown");
    draw_text_fit(a->font_small, r, head, -1, rect_make(rc.x + pad, y, rc.w - 2 * pad, rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
    y += rh + px(a, 4);
    Rect list = rect_make(rc.x, y, rc.w, rc.y + rc.h - y);
    if (v->kind == MD_OUTLINE) {
        Input *in = &a->in;
        if (rect_contains(list, in->mouse_x, in->mouse_y) && in->wheel_y != 0) { v->scroll -= in->wheel_y; in->wheel_y = 0; }
        float max = doc.nheads * rh - list.h; if (v->scroll > max) v->scroll = max; if (v->scroll < 0) v->scroll = 0;
        ui_scrollbar_input(a, &v->sb, list, doc.nheads * rh, &v->scroll);
        clip_push(r, list);
        for (int i = 0; i < doc.nheads; i++) {
            float ry = list.y + i * rh - v->scroll;
            if (ry + rh < list.y || ry > list.y + list.h) continue;
            const MdHead *h = &doc.heads[i];
            float ix = rc.x + pad + (h->level - 1) * px(a, 14);
            draw_text_fit(h->level == 1 ? a->font_ui : a->font_small, r, h->title, -1, rect_make(ix, ry, rc.x + rc.w - pad - ix, rh), color_hex(h->level == 1 ? T_TEXT : T_TEXT_MUTED), ALIGN_LEFT);
        }
        clip_pop(r);
        ui_scrollbar_draw(a, &v->sb, list, doc.nheads * rh, v->scroll);
    } else {
        const char *names[] = { "Words", "Lines", "Headings", "Links", "Code blocks" };
        int vals[] = { doc.words, doc.lines, doc.nheads, doc.links, doc.code_blocks };
        for (int i = 0; i < 5; i++, y += rh) {
            char n[32]; snprintf(n, sizeof n, "%d", vals[i]);
            draw_text_fit(a->font_ui, r, names[i], -1, rect_make(rc.x + pad, y, rc.w / 2, rh), color_hex(T_TEXT_MUTED), ALIGN_LEFT);
            draw_text_fit(a->font_ui, r, n, -1, rect_make(rc.x + rc.w / 2, y, rc.w / 2 - pad, rh), color_hex(T_TEXT), ALIGN_RIGHT);
        }
    }
}

/* Context-menu hook: the module's own actions, appended after the shell's. */
static int md_menu(void *view, void *ctx, DockMenuItem *items, int max) {
    (void)view; (void)ctx;
    if (max < 3) return 0;
    items[0] = (DockMenuItem){ .id = MDM_RELOAD, .label = "Reload notes.md", .enabled = true };
    items[1] = (DockMenuItem){ .id = MDM_COPY, .label = "Copy outline", .enabled = doc.nheads > 0 };
    items[2] = (DockMenuItem){ .id = MDM_PATH, .label = "Show file path", .enabled = true };
    return 3;
}

/* Runs one of the module's menu items. */
static void md_menu_run(void *view, void *ctx, int id) {
    (void)view; App *a = ctx;
    if (id == MDM_RELOAD) { md_load(); app_status(a, "notes.md reloaded: %d headings", doc.nheads); LOG_I("markdown: reloaded from the menu"); }
    else if (id == MDM_COPY) {
        char out[MD_MAX_HEADS * 130] = ""; size_t n = 0;
        for (int i = 0; i < doc.nheads && n < sizeof out - 140; i++)
            n += (size_t)snprintf(out + n, sizeof out - n, "%*s%s\n", (doc.heads[i].level - 1) * 2, "", doc.heads[i].title);
        SDL_SetClipboardText(out);
        app_status(a, "Outline copied: %d headings", doc.nheads);
        LOG_I("markdown: outline copied (%d headings)", doc.nheads);
    } else if (id == MDM_PATH) app_status(a, "Markdown notes: %s", doc.path);
}

/* The module's toolbar / palette action (TBR-050): it only receives the context snapshot (no
 * database handle, no credentials) and reloads the notes file. */
static void md_cmd_reload(void *ud, const LumeCtx *c) { (void)ud; (void)c; if (doc.refs) { md_load(); LOG_I("markdown: reloaded from its command"); } }

/* Module init: registers both panels (hidden at first, reachable from the palette and Window >
 * Panels…; the layout file brings them back wherever the user put them). */
void markdown_module_init(Panels *P, App *a) {
    DockPanelDesc outline = { .id = "lume.md.outline", .title = "Outline", .icon = "outline", .region = DOCK_RIGHT,
        .min_w = 180, .min_h = 100, .pref_size = 260, .priority = 4, .start_hidden = true,
        .draw = md_draw, .ud = (void *)(intptr_t)MD_OUTLINE,
        .create = md_create, .destroy = md_destroy, .visibility = md_visibility, .menu = md_menu, .menu_run = md_menu_run };
    DockPanelDesc stats = outline;
    stats.id = "lume.md.stats"; stats.title = "Markdown Stats"; stats.icon = "stats"; stats.ud = (void *)(intptr_t)MD_STATS; stats.priority = 5;
    panels_register(P, a, &outline);
    panels_register(P, a, &stats);
    Command reload = { .id = "md.reload", .label = "Reload notes.md", .icon = "refresh", .group = "Markdown", .synonyms = "notes markdown refresh",
                       .enabled = NULL, .invoke = md_cmd_reload, .module = "lume.markdown" };
    commands_register(&reload);
    /* off the panel bar by default: an example module should not crowd the Lume Data buttons */
    Dock *d = panels_dock(P);
    for (int p = 0; p < d->ndesc; p++)
        if (!strncmp(d->desc[p].id, "lume.md.", 8)) dock_set_in_bar(d, p, false);
}
