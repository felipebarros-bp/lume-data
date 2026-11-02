/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * menu.c - the in-window menu bar (File, Edit, Query, Format, View, Window, Help).
 *
 * The menus are static tables (MenuDef -> MenuItem[]) compiled into the binary; nothing is
 * allocated per entry. Each frame the main loop calls menu_update() to lay out and draw the
 * bar and to turn a click into an ACT_* code, then menu_draw_overlay() at the very end of the
 * frame so the dropdown covers every other widget. The caller dispatches the returned action
 * (main.c for most, panels.c for the Window menu). The command palette (panels.c) enumerates
 * the same tables through menu_item_count()/menu_item_at().
 *
 * Shortcut strings use a compact notation that format_shortcut() expands per platform:
 *   '#' = Cmd (macOS) / Ctrl,  '^' = Control / Ctrl,  '$' = Shift,  '~' = Option / Alt,
 *   '@' = Return / Enter.  E.g. "$#N" is Shift+Cmd+N.
 * The shortcuts shown here are labels only; the actual key handling lives elsewhere.
 *
 * All geometry is in render pixels: sizes are written in UI points and scaled with px().
 * UI thread only.
 */
#include "menu.h"
#include "brand.h"
#include "ui.h"
#include "draw.h"
#include "font.h"
#include "theme.h"
#include "commands.h"

/* One dropdown entry. label == NULL marks a separator. shortcut is in the compact notation
   above (NULL = none). Disabled entries are drawn dimmed and cannot be clicked. An entry runs
   its ACT_* action, or a registered command (cmd), or opens a submenu (sub). */
typedef struct MenuItem { const char *label; const char *shortcut; int action; bool enabled; const struct MenuItem *sub; int nsub; const char *cmd; } MenuItem;
/* One top-level menu: its title in the bar and its entries. */
typedef struct { const char *title; const MenuItem *items; int n; } MenuDef;
/* Optional: the command registry's rule for an ACT_* (commands_act_enabled), so the menu shows
 * the same enabled state as the toolbar and the palette (TBR-048). NULL = static flags only. */
static bool (*enable_hook)(int act);
void menu_set_enable_hook(bool (*fn)(int act)) { enable_hook = fn; }
/* Optional: whether an ACT_* setting is currently on (a check mark). */
static bool (*check_hook)(int act);
void menu_set_check_hook(bool (*fn)(int act)) { check_hook = fn; }
/* Drawn enabled: the static flag and, for registered commands, the current rule. */
static bool item_on(const MenuItem *it) {
    if (!it->enabled) return false;
    if (it->sub) return true;
    if (it->cmd) { char why[8]; return commands_enabled(it->cmd, why, sizeof why); }
    return !enable_hook || enable_hook(it->action);
}
static bool item_checked(const MenuItem *it) { return !it->sub && !it->cmd && it->action != ACT_NONE && check_hook && check_hook(it->action); }

/* Element count of a static array, as int. */
#define N(a) ((int)(sizeof(a) / sizeof(a[0])))
/* Separator entry. */
#define SEP { NULL, NULL, ACT_NONE, false, NULL, 0, NULL }
/* A submenu entry. */
#define SUB(lbl, arr) { lbl, NULL, ACT_NONE, true, arr, N(arr), NULL }
/* An entry that runs a registered command (by id). */
#define CMD(lbl, sc, id) { lbl, sc, ACT_NONE, true, NULL, 0, id }
#define E3 "\xE2\x80\xA6"   /* … */

/* the tables leave the submenu / command fields zero where an entry has none */
#pragma GCC diagnostic ignored "-Wmissing-field-initializers"
/* ---- File ---- */
static const MenuItem file_export_items[] = {
    { "Results" E3, "$#E", ACT_EXPORT, true },
};
static const MenuItem file_conn_items[] = {
    { "New Connection" E3, "$#N", ACT_NEW_CONN, true },
    { "Edit Connection" E3, "#E", ACT_EDIT_CONN, true },
    { "Connect / Disconnect", "#D", ACT_TOGGLE_CONN, true },
    CMD("Open the E-commerce Demo Database", NULL, "demo.shop"),
};
static const MenuItem file_ws_items[] = {
    { "New Workspace" E3, NULL, ACT_WORKSPACE_NEW, true },
    { "Open Workspace" E3, NULL, ACT_WORKSPACE_OPEN, true },
    { "Workspace Settings" E3, NULL, ACT_WORKSPACE_SETTINGS, true },
    { "Close Workspace", NULL, ACT_WORKSPACE_CLOSE, true },
};
static const MenuItem file_items[] = {
    { "New Query", "#N", ACT_NEW_QUERY, true },
    { "New SQL Tab", "#T", ACT_NEW_EDITOR_TAB, true },
    CMD("New Report", NULL, "report.new"),
    SEP,
    { "Open" E3, "#O", ACT_OPEN_DB, true },
    CMD("Open Report" E3, NULL, "report.open"),
    { "Quick Open" E3, "#P", ACT_QUICK_OPEN, true },
    SEP,
    CMD("Save", "#S", "file.save"),
    SUB("Export", file_export_items),
    SEP,
    SUB("Connection", file_conn_items),
    SUB("Workspace", file_ws_items),
    SEP,
    { "Quit Lume", "#Q", ACT_QUIT, true },
};
/* ---- Edit ---- */
static const MenuItem edit_items[] = {
    { "Undo", "#Z", ACT_UNDO, false },
    { "Redo", "$#Z", ACT_REDO, false },
    SEP,
    { "Cut", "#X", ACT_CUT, true },
    { "Copy", "#C", ACT_COPY, true },
    { "Paste", "#V", ACT_PASTE, true },
    { "Select All", "#A", ACT_SELECT_ALL, true },
    SEP,
    { "Complete", "^Space", ACT_COMPLETE, true },
};
/* ---- Query ---- */
static const MenuItem q_special_items[] = {
    { "With Parameters" E3, NULL, ACT_RUN_PARAMS, true },
    { "To File" E3, NULL, ACT_RUN_TO_FILE, true },
    { "Re-run Last" E3, NULL, ACT_RERUN_LAST, true },
};
static const MenuItem q_explain_items[] = {
    { "Explain Plan", "~#E", ACT_EXPLAIN, true },
    { "Explain Analyze" E3, NULL, ACT_EXPLAIN_ANALYZE, true },
    CMD("Show Execution Plan", NULL, "plan.show"),
};
static const MenuItem q_session_items[] = {
    { "Connect", NULL, ACT_CONNECT, true },
    { "Reconnect", NULL, ACT_RECONNECT, true },
    { "Disconnect", NULL, ACT_DISCONNECT, true },
    SEP,
    { "Session Details" E3, NULL, ACT_SESSION_DETAILS, true },
};
static const MenuItem q_tx_items[] = {
    { "Auto-commit", NULL, ACT_TX_AUTO, true },
    { "Manual", NULL, ACT_TX_MANUAL, true },
    SEP,
    { "Commit", NULL, ACT_COMMIT, true },
    { "Rollback", NULL, ACT_ROLLBACK, true },
};
static const MenuItem q_fetch_items[] = {
    { "100", NULL, ACT_FETCH_100, true }, { "500", NULL, ACT_FETCH_500, true },
    { "1,000", NULL, ACT_FETCH_1000, true }, { "5,000", NULL, ACT_FETCH_5000, true },
};
static const MenuItem q_budget_items[] = {
    { "64 MB", NULL, ACT_BUDGET_64, true }, { "256 MB", NULL, ACT_BUDGET_256, true }, { "1 GB", NULL, ACT_BUDGET_1024, true },
};
static const MenuItem q_results_items[] = {
    { "Find in Results", "#F", ACT_FIND, true },
    { "Export Results" E3, "$#E", ACT_EXPORT, true },
    SEP,
    { "Fetch More Rows", "$#M", ACT_FETCH_MORE, true },
    { "Fetch All Rows", NULL, ACT_FETCH_ALL, true },
    SUB("Fetch Size", q_fetch_items),
    SUB("Memory Limit", q_budget_items),
};
static const MenuItem q_exec_items[] = {
    { "Continue on Error (scripts)", NULL, ACT_CONTINUE_ON_ERROR, true },
    { "Execution Settings" E3, NULL, ACT_EXEC_SETTINGS, true },
};
static const MenuItem query_items[] = {
    { "Run Statement", "#@", ACT_RUN, true },
    { "Run Selection", NULL, ACT_RUN_SELECTION, true },
    { "Run All", "$#@", ACT_RUN_ALL, true },
    { "Run in New Tab", "~#@", ACT_RUN_NEW_TAB, true },
    SUB("Run Special", q_special_items),
    SUB("Explain", q_explain_items),
    { "Cancel Execution", "#.", ACT_CANCEL, true },
    SEP,
    SUB("Session", q_session_items),
    SUB("Transaction", q_tx_items),
    SEP,
    SUB("Results", q_results_items),
    SUB("Execution", q_exec_items),
    { "Show Execution Output", "$#O", ACT_SHOW_OUTPUT, true },
    { "Execution History" E3, "#Y", ACT_HISTORY, true },
};
/* ---- Format ---- */
static const MenuItem f_num_items[] = {
    { "Thousands Separator", NULL, ACT_FMT_GROUP, true },
    SEP,
    { "Decimals: As Stored", NULL, ACT_FMT_DEC_RAW, true },
    { "Decimals: 2", NULL, ACT_FMT_DEC_2, true },
    { "Decimals: 4", NULL, ACT_FMT_DEC_4, true },
};
static const MenuItem f_date_items[] = {
    { "ISO (As Stored)", NULL, ACT_FMT_DATE_ISO, true },
    { "DD/MM/YYYY", NULL, ACT_FMT_DATE_DMY, true },
    { "MM/DD/YYYY", NULL, ACT_FMT_DATE_MDY, true },
};
static const MenuItem f_tz_items[] = {
    { "As Stored", NULL, ACT_FMT_TZ_STORED, true },
    { "UTC", NULL, ACT_FMT_TZ_UTC, true },
    { "Local", NULL, ACT_FMT_TZ_LOCAL, true },
};
static const MenuItem format_items[] = {
    SUB("Numbers", f_num_items),
    SUB("Dates", f_date_items),
    SUB("Time Zone", f_tz_items),
};
/* ---- View ---- */
static const MenuItem v_tabs_items[] = {
    { "Next Result Tab", "~#]", ACT_NEXT_TAB, true },
    { "Previous Result Tab", "~#[", ACT_PREV_TAB, true },
    { "Close Result Tab", "#W", ACT_CLOSE_TAB, true },
    SEP,
    { "Columns" E3, "$#C", ACT_COLUMNS, true },
};
static const MenuItem v_zoom_items[] = {
    { "Zoom In", "#=", ACT_ZOOM_IN, true },
    { "Zoom Out", "#-", ACT_ZOOM_OUT, true },
    { "Reset Zoom", "#0", ACT_ZOOM_RESET, true },
};
static const MenuItem v_toolbar_items[] = {
    { "Main Toolbar", NULL, ACT_TOOLBAR_TOGGLE, true },
    { "Compact", NULL, ACT_TOOLBAR_COMPACT, true },
    { "Labels", NULL, ACT_TOOLBAR_LABELS, true },
    SEP,
    { "Customize" E3, NULL, ACT_TOOLBAR_CUSTOMIZE, true },
    { "Focus Toolbar", "^F5", ACT_TOOLBAR_FOCUS, true },
    { "Reset to Default", NULL, ACT_TOOLBAR_RESET, true },
    { "Export Settings", NULL, ACT_TOOLBAR_EXPORT, true },
};
static const MenuItem view_items[] = {
    { "Explorer", "#B", ACT_TOGGLE_SIDEBAR, true },
    { "Results", "#J", ACT_TOGGLE_RESULTS, true },
    { "Refresh Explorer", "#R", ACT_REFRESH, true },
    SEP,
    SUB("Result Tabs", v_tabs_items),
    SUB("Zoom", v_zoom_items),
    SUB("Toolbar", v_toolbar_items),
    SEP,
    { "Compact Panel Headers", NULL, ACT_COMPACT, true },
    { "Reduce Motion", NULL, ACT_REDUCE_MOTION, true },
};
/* ---- Tools ---- */
static const MenuItem t_reports_items[] = {
    CMD("New Report", NULL, "report.new"),
    CMD("New Report from a Question" E3, NULL, "report.new_ai"),
    CMD("Open Report" E3, NULL, "report.open"),
    CMD("Add Result to Report", NULL, "report.add_result"),
};
static const MenuItem tools_items[] = {
    SUB("Reports", t_reports_items),
    CMD("Lume Intelligence", NULL, "report.assistant"),
    CMD("Source Control", NULL, "git.panel"),
    SEP,
    CMD("Reload Semantic Catalog", NULL, "report.reload_catalog"),
    SEP,
    CMD("Settings" E3, "#,", "app.settings.open"),
};
/* ---- Window ---- */
static const MenuItem w_show_items[] = {
    { "Explorer", "#1", ACT_PANEL_EXPLORER, true },
    { "Results", "#2", ACT_PANEL_RESULTS, true },
    { "Output", "#3", ACT_PANEL_OUTPUT, true },
    { "Inspector (Record View)", "#4", ACT_PANEL_INSPECTOR, true },
    { "Query History", NULL, ACT_PANEL_HISTORY, true },
    CMD("Lume Intelligence", NULL, "report.assistant"),
    CMD("Source Control", NULL, "git.panel"),
    CMD("Execution Plan", NULL, "plan.show"),
};
static const MenuItem w_panel_items[] = {
    { "Focus Mode", "$#F12", ACT_FOCUS_MODE, true },
    { "Maximize / Restore", "~#M", ACT_MAXIMIZE_PANEL, true },
    { "Collapse / Expand", "~#J", ACT_COLLAPSE_PANEL, true },
    { "Hide", "$Esc", ACT_HIDE_PANEL, true },
    { "Grow", "^~Right", ACT_RESIZE_GROW, true },
    { "Shrink", "^~Left", ACT_RESIZE_SHRINK, true },
    SEP,
    { "Allow Top Edge", NULL, ACT_TOP_EDGE, true },
};
static const MenuItem w_layout_items[] = {
    { "Default (Reset)", NULL, ACT_LAYOUT_DEFAULT, true },
    { "Query Focus", NULL, ACT_LAYOUT_QUERY_FOCUS, true },
    { "Data Exploration", NULL, ACT_LAYOUT_DATA, true },
    SEP,
    { "Save Layout As" E3, NULL, ACT_LAYOUT_SAVE_AS, true },
    { "Manage Layouts" E3, NULL, ACT_LAYOUTS, true },
    { "Export Layout" E3, NULL, ACT_LAYOUT_EXPORT, true },
    { "Import Layout" E3, NULL, ACT_LAYOUT_IMPORT, true },
    SEP,
    { "Undo Layout Change", "~$#Z", ACT_UNDO_LAYOUT, true },
};
static const MenuItem w_editor_items[] = {
    { "New Query Tab", "#T", ACT_NEW_EDITOR_TAB, true },
    { "Split Editor Right", "#\\", ACT_SPLIT_RIGHT, true },
    { "Split Editor Down", "$#\\", ACT_SPLIT_DOWN, true },
};
static const MenuItem window_items[] = {
    { "Command Palette" E3, "$#P", ACT_PALETTE, true },
    { "Switch Tab" E3, "^Tab", ACT_SWITCH_TAB, true },
    { "Panels" E3, "~#P", ACT_PANELS, true },
    SEP,
    SUB("Show Panel", w_show_items),
    { "Back to Last Panel / Editor", "F12", ACT_LAST_PANEL, true },
    SUB("Panel", w_panel_items),
    SUB("Layout", w_layout_items),
    SUB("Editor", w_editor_items),
};
static const MenuItem help_items[] = {
    { "About Lume Data", NULL, ACT_ABOUT, true },
};

/* The menu bar, left to right. */
static const MenuDef menus[] = {
    { "File", file_items, N(file_items) },
    { "Edit", edit_items, N(edit_items) },
    { "Query", query_items, N(query_items) },
    { "Format", format_items, N(format_items) },
    { "View", view_items, N(view_items) },
    { "Tools", tools_items, N(tools_items) },
    { "Window", window_items, N(window_items) },
    { "Help", help_items, N(help_items) },
};
#define MENU_COUNT N(menus)
#define MENU_DEPTH 3   /* top dropdown + two submenu levels (Query > Results > Fetch Size) */

struct Menu {
    int open;            /* index of open menu or -1 */
    float title_x[MENU_COUNT], title_w[MENU_COUNT];   /* title hit boxes, recomputed every menu_update() */
    Rect bar;            /* bar rect from the last menu_update(), used to place dropdowns */
    int path[MENU_DEPTH];    /* path[k]: the entry of level k whose submenu is open at level k + 1 (-1 none) */
    int sel[MENU_DEPTH];     /* keyboard / hover highlight per level (-1 none) */
    int hov_l, hov_k; double hov_at;   /* hover dwell before switching an open submenu */
    float mx, my;                      /* the pointer last frame: hover acts only when it moves (keys win otherwise) */
};

/* Allocates the menu state with no dropdown open. Freed by menu_destroy(). */
Menu *menu_create(void) { Menu *m = calloc(1, sizeof *m); m->open = -1; for (int i = 0; i < MENU_DEPTH; i++) m->path[i] = m->sel[i] = -1; m->hov_l = -1; return m; }
/* Frees the menu state. */
void menu_destroy(Menu *m) { free(m); }
/* True while a dropdown is open. */
bool menu_is_open(Menu *m) { return m->open >= 0; }
static void close_from(Menu *m, int level) { for (int i = level; i < MENU_DEPTH; i++) { m->path[i] = -1; if (i > level) m->sel[i] = -1; } }
static void open_menu(Menu *m, int i) { m->open = i; for (int k = 0; k < MENU_DEPTH; k++) m->path[k] = m->sel[k] = -1; m->hov_l = -1; }

/* Expands a compact shortcut string (see the notation at the top of the file) into
   human-readable text: modifier glyphs on macOS, "Ctrl+"/"Shift+"/"Alt+" elsewhere. Other
   characters are copied as is. Always NUL-terminates; stops early, keeping 8 bytes of
   headroom, if `out` (n bytes) gets full. */
static void format_shortcut(const char *sc, char *out, size_t n) {
    out[0] = 0;
    size_t p = 0;
    for (; *sc && p < n - 8; sc++) {
        const char *rep = NULL;
        char tmp[2] = { *sc, 0 };
#ifdef __APPLE__
        if (*sc == '#') rep = "\xE2\x8C\x98";        /* ⌘ */
        else if (*sc == '^') rep = "\xE2\x8C\x83";   /* ⌃ */
        else if (*sc == '$') rep = "\xE2\x87\xA7";   /* ⇧ */
        else if (*sc == '@') rep = "\xE2\x86\xA9";   /* ↩ */
        else if (*sc == '~') rep = "\xE2\x8C\xA5";   /* ⌥ */
#else
        if (*sc == '#') rep = "Ctrl+";
        else if (*sc == '^') rep = "Ctrl+";
        else if (*sc == '$') rep = "Shift+";
        else if (*sc == '@') rep = "Enter";
        else if (*sc == '~') rep = "Alt+";
#endif
        if (!rep) rep = tmp;
        size_t l = strlen(rep);
        memcpy(out + p, rep, l);
        p += l;
        out[p] = 0;
    }
}

/* ---- levels: 0 = the open menu's dropdown, 1..2 = submenus along m->path ---- */
static const MenuItem *level_items(Menu *m, int L, int *n) {
    const MenuItem *it = menus[m->open].items; int cnt = menus[m->open].n;
    for (int k = 0; k < L; k++) {
        int p = m->path[k];
        if (p < 0 || p >= cnt || !it[p].sub) { *n = 0; return NULL; }
        int nn = it[p].nsub; it = it[p].sub; cnt = nn;
    }
    *n = cnt; return it;
}
static int open_levels(Menu *m) { int L = 1; while (L < MENU_DEPTH && m->path[L - 1] >= 0) L++; return L; }
#define ITEM_H 26
#define SEP_H 9
#define PAD 6
#define CHECK_W 22   /* the check-mark column */
static float list_w(App *a, const MenuItem *it, int n) {
    float w = px(a, 200);
    for (int k = 0; k < n; k++) {
        if (!it[k].label) continue;
        char sc[32] = ""; if (it[k].shortcut) format_shortcut(it[k].shortcut, sc, sizeof sc);
        float need = px(a, CHECK_W) + font_width(a->font_ui, it[k].label, -1) + px(a, 36) + (it[k].sub ? px(a, 14) : font_width(a->font_small, sc, -1)) + px(a, 16);
        if (need > w) w = need;
    }
    return w;
}
static float list_h(App *a, const MenuItem *it, int n) { float h = px(a, PAD) * 2; for (int k = 0; k < n; k++) h += px(a, it[k].label ? ITEM_H : SEP_H); return h; }
static Rect row_rect_in(App *a, Rect lr, const MenuItem *it, int k) {
    float y = lr.y + px(a, PAD);
    for (int i = 0; i < k; i++) y += px(a, it[i].label ? ITEM_H : SEP_H);
    return rect_make(lr.x + px(a, PAD), y, lr.w - px(a, PAD) * 2, px(a, it[k].label ? ITEM_H : SEP_H));
}
/* where level L is drawn: the first under the title, each submenu beside its parent entry
 * (on the left when there is no room on the right), kept inside the window */
static Rect level_rect(Menu *m, App *a, int L) {
    int n; const MenuItem *it = level_items(m, L, &n);
    float w = list_w(a, it, n), h = list_h(a, it, n);
    int ww = 0, wh = 0; SDL_GetCurrentRenderOutputSize(a->renderer, &ww, &wh);
    if (L == 0) {
        float x = m->title_x[m->open] - px(a, 4);
        if (x + w > ww - px(a, 4)) x = ww - px(a, 4) - w;
        return rect_make(x, m->bar.y + m->bar.h - px(a, 2), w, h);
    }
    Rect pr = level_rect(m, a, L - 1);
    int pn; const MenuItem *pit = level_items(m, L - 1, &pn);
    Rect row = row_rect_in(a, pr, pit, m->path[L - 1]);
    float x = pr.x + pr.w - px(a, 4), y = row.y - px(a, PAD);
    if (x + w > ww - px(a, 4)) x = pr.x - w + px(a, 4);
    if (y + h > wh - px(a, 4)) y = wh - px(a, 4) - h;
    if (y < m->bar.y + m->bar.h) y = m->bar.y + m->bar.h;
    return rect_make(x, y, w, h);
}
static int item_at_level(Menu *m, App *a, int L, float mx, float my) {
    int n; const MenuItem *it = level_items(m, L, &n);
    Rect lr = level_rect(m, a, L);
    if (!rect_contains(lr, mx, my)) return -1;
    for (int k = 0; k < n; k++) { if (!it[k].label) continue; if (rect_contains(row_rect_in(a, lr, it, k), mx, my)) return k; }
    return -1;
}
/* the next selectable entry from k in direction d (wraps), -1 when none */
static int step_sel(const MenuItem *it, int n, int k, int d) {
    for (int t = 0; t < n; t++) { k = (k + d + n) % n; if (it[k].label && item_on(&it[k])) return k; }
    return -1;
}
static int activate(Menu *m, App *a, const MenuItem *it) {
    int act = ACT_NONE;
    if (it->cmd) commands_invoke(a, it->cmd);
    else act = it->action;
    m->open = -1;
    return act;
}

/* Index of the menu title under (mx, my) in the bar, or -1. Uses the hit boxes from the
   current frame's layout. */
static int title_at(Menu *m, float mx, float my) {
    if (!rect_contains(m->bar, mx, my)) return -1;
    for (int i = 0; i < MENU_COUNT; i++)
        if (mx >= m->title_x[i] && mx < m->title_x[i] + m->title_w[i]) return i;
    return -1;
}

/* Per-frame entry point: lays out the titles, handles clicks, hover and the keyboard, draws the
   bar and returns the ACT_* code of a chosen enabled entry (ACT_NONE otherwise). See menu.h. */
int menu_update(Menu *m, App *a, Rect bar) {
    SDL_Renderer *r = a->renderer;
    Input *in = &a->in;
    m->bar = bar;
    int action = ACT_NONE;

    /* layout titles */
    float x = bar.x + px(a, 14);
    const char *brand = "Lume";
    float brand_w = font_width(a->font_ui, brand, -1);
    x += brand_w + px(a, 22);
    for (int i = 0; i < MENU_COUNT; i++) {
        m->title_x[i] = x;
        m->title_w[i] = font_width(a->font_ui, menus[i].title, -1) + px(a, 20);
        x += m->title_w[i];
    }

    int hover_title = title_at(m, in->mouse_x, in->mouse_y);
    if (m->open >= 0) {
        if (hover_title >= 0 && hover_title != m->open) open_menu(m, hover_title);   /* menu-bar scrubbing */
        int lv = open_levels(m), D = lv - 1;
        /* keyboard: Up / Down move, Right opens a submenu, Left closes one, Enter chooses, Esc closes a level */
        { int n; const MenuItem *it = level_items(m, D, &n);
          if (input_key(in, SDLK_DOWN, 0)) m->sel[D] = step_sel(it, n, m->sel[D] < 0 ? -1 : m->sel[D], 1);
          else if (input_key(in, SDLK_UP, 0)) m->sel[D] = step_sel(it, n, m->sel[D] < 0 ? n : m->sel[D], -1);
          else if (input_key(in, SDLK_RIGHT, 0)) { if (m->sel[D] >= 0 && it[m->sel[D]].sub && D + 1 < MENU_DEPTH) { m->path[D] = m->sel[D]; int sn; const MenuItem *si = level_items(m, D + 1, &sn); m->sel[D + 1] = step_sel(si, sn, -1, 1); } else { open_menu(m, (m->open + 1) % MENU_COUNT); } }
          else if (input_key(in, SDLK_LEFT, 0)) { if (D > 0) { m->sel[D] = -1; m->path[D - 1] = -1; } else open_menu(m, (m->open + MENU_COUNT - 1) % MENU_COUNT); }
          else if (input_key(in, SDLK_RETURN, 0) && m->sel[D] >= 0 && item_on(&it[m->sel[D]])) {
              if (it[m->sel[D]].sub && D + 1 < MENU_DEPTH) { m->path[D] = m->sel[D]; int sn; const MenuItem *si = level_items(m, D + 1, &sn); m->sel[D + 1] = step_sel(si, sn, -1, 1); }
              else action = activate(m, a, &it[m->sel[D]]);
          }
          else if (input_key(in, SDLK_ESCAPE, 0)) { if (D > 0) { m->sel[D] = -1; m->path[D - 1] = -1; } else m->open = -1; } }
        /* mouse: the deepest level under the pointer */
        if (m->open >= 0) {
            lv = open_levels(m);
            int L = -1, k = -1;
            for (int l = lv - 1; l >= 0 && L < 0; l--) if (rect_contains(level_rect(m, a, l), in->mouse_x, in->mouse_y)) { L = l; k = item_at_level(m, a, l, in->mouse_x, in->mouse_y); }
            double now = (double)SDL_GetTicks() / 1000.0;
            bool moved = in->mouse_x != m->mx || in->mouse_y != m->my || m->hov_l != L || m->hov_k != k;
            m->mx = in->mouse_x; m->my = in->mouse_y;
            if (L >= 0 && k >= 0 && (moved || now - m->hov_at < 0.25)) {
                int n; const MenuItem *it = level_items(m, L, &n);
                if (m->hov_l != L || m->hov_k != k) { m->hov_l = L; m->hov_k = k; m->hov_at = now; }
                m->sel[L] = k;
                /* another entry of a level whose submenu is open: switch after a short dwell, so a diagonal move to the submenu does not close it */
                bool dwell = m->path[L] < 0 || now - m->hov_at >= 0.18;
                if (it[k].sub && L + 1 < MENU_DEPTH && m->path[L] != k && dwell) { close_from(m, L); m->path[L] = k; }
                else if (!it[k].sub && m->path[L] >= 0 && dwell) close_from(m, L);
                if (!dwell) app_wake_in(a, 60);
            }
            if (in->mouse_pressed) {
                in->mouse_consumed = true;
                if (L >= 0) {
                    if (k >= 0) {
                        int n; const MenuItem *it = level_items(m, L, &n);
                        if (it[k].sub && L + 1 < MENU_DEPTH) { close_from(m, L); m->path[L] = k; }
                        else if (item_on(&it[k])) action = activate(m, a, &it[k]);
                    }
                } else if (hover_title == m->open || hover_title < 0) m->open = -1;
            }
        }
        /* swallow keys while a menu is open */
        in->nkeys = 0;
        in->ntext = 0;
    } else if (hover_title >= 0 && in->mouse_pressed && !in->mouse_consumed) {
        in->mouse_consumed = true;
        open_menu(m, hover_title);
    }

    /* draw bar */
    draw_rect(r, bar, color_hex(T_PANEL_ALT));
    draw_rect(r, rect_make(bar.x, bar.y + bar.h - px(a, 1), bar.w, px(a, 1)), color_hex(T_BORDER));
    float by = bar.y + (bar.h - font_height(a->font_ui)) / 2;
    /* the Lume Data symbol; the brand works as the app menu's About entry (ABOUT-001) */
    Rect brand_rc = rect_make(bar.x + px(a, 8), bar.y + px(a, 3), px(a, 24) + brand_w, bar.h - px(a, 6));
    bool brand_hov = m->open < 0 && rect_contains(brand_rc, in->mouse_x, in->mouse_y);
    if (brand_hov) draw_rrect(r, brand_rc, px(a, M_RADIUS), color_hex(T_HOVER));
    brand_draw_symbol(a, rect_make(bar.x + px(a, 10), bar.y + bar.h / 2 - px(a, 8), px(a, 16), px(a, 16)));
    draw_text(a->font_ui, r, brand, bar.x + px(a, 14) + px(a, 16), by, color_hex(T_TEXT));
    ui_record("menubar:brand", brand_rc);
    if (brand_hov && in->mouse_pressed && !in->mouse_consumed && action == ACT_NONE) { in->mouse_consumed = true; action = ACT_ABOUT; }
    for (int i = 0; i < MENU_COUNT; i++) {
        bool active = m->open == i || (m->open < 0 && hover_title == i);
        Rect t = rect_make(m->title_x[i], bar.y + px(a, 4), m->title_w[i], bar.h - px(a, 8));
        if (active) draw_rrect(r, t, px(a, M_RADIUS), color_hex(T_HOVER));
        draw_text_fit(a->font_ui, r, menus[i].title, -1, t, color_hex(active ? T_TEXT : T_TEXT_MUTED), ALIGN_CENTER);
        char lab[48]; snprintf(lab, sizeof lab, "menubar:%s", menus[i].title); ui_record(lab, t);
    }
    if (hover_title >= 0) a->want_cursor = CURSOR_HAND;
    return action;
}

/* Draws the open dropdown and its open submenus: soft shadow, bordered panel, entries with a
   check column, label, right-aligned shortcut or submenu arrow. Input is handled in menu_update(). */
void menu_draw_overlay(Menu *m, App *a) {
    if (m->open < 0) return;
    SDL_Renderer *r = a->renderer;
    int lv = open_levels(m);
    float rad = px(a, 8);
    for (int L = 0; L < lv; L++) {
        int n; const MenuItem *it = level_items(m, L, &n);
        Rect dd = level_rect(m, a, L);
        for (int s = 3; s >= 1; s--)
            draw_rrect(r, rect_make(dd.x - s * px(a, 2), dd.y - s * px(a, 1) + px(a, 3), dd.w + s * px(a, 4), dd.h + s * px(a, 4)), rad + s * px(a, 2), color_alpha(color_hex(0x000000), 28));
        draw_rrect(r, dd, rad, color_hex(T_BORDER));
        draw_rrect(r, rect_inset(dd, px(a, 1), px(a, 1)), rad - px(a, 1), color_hex(T_PANEL_ALT));
        for (int k = 0; k < n; k++) {
            const MenuItem *e = &it[k];
            Rect rc = row_rect_in(a, dd, it, k);
            if (!e->label) { draw_rect(r, rect_make(dd.x + px(a, 10), rc.y + rc.h / 2, dd.w - px(a, 20), px(a, 1)), color_hex(T_BORDER)); continue; }
            { char lab[96]; snprintf(lab, sizeof lab, "menu:%s", e->label); ui_record(lab, rc); }
            bool on = item_on(e);
            bool hl = on && m->sel[L] == k;
            bool parent = m->path[L] == k;   /* its submenu is open */
            if (hl) draw_rrect(r, rc, px(a, M_RADIUS), color_hex(T_ACCENT));
            else if (parent) draw_rrect(r, rc, px(a, M_RADIUS), color_hex(T_HOVER));
            Color tc = color_hex(!on ? T_TEXT_DIM : hl ? 0xFFFFFF : T_TEXT);
            if (item_checked(e)) draw_text_fit(a->font_ui, r, "\xE2\x9C\x93", -1, rect_make(rc.x + px(a, 6), rc.y, px(a, CHECK_W - 6), rc.h), tc, ALIGN_LEFT);
            draw_text_fit(a->font_ui, r, e->label, -1, rect_make(rc.x + px(a, CHECK_W), rc.y, rc.w - px(a, CHECK_W + 10), rc.h), tc, ALIGN_LEFT);
            if (e->sub) draw_text_fit(a->font_ui, r, "\xE2\x80\xBA", -1, rect_make(rc.x, rc.y, rc.w - px(a, 8), rc.h), color_hex(hl ? 0xFFFFFF : T_TEXT_MUTED), ALIGN_RIGHT);
            else if (e->shortcut) {
                char sc[32]; format_shortcut(e->shortcut, sc, sizeof sc);
                draw_text_fit(a->font_small, r, sc, -1, rect_make(rc.x + px(a, 10), rc.y, rc.w - px(a, 20), rc.h), color_hex(hl ? 0xFFFFFF : T_TEXT_MUTED), ALIGN_RIGHT);
            }
        }
    }
    if (m->sel[open_levels(m) - 1] >= 0 || m->sel[0] >= 0) a->want_cursor = CURSOR_HAND;
}

/* ---- every entry, flattened, for the command palette (PNL-036) and the shortcut lookup ---- */
typedef struct { char path[96]; const MenuItem *it; } FlatItem;
static FlatItem flat[320]; static int nflat = -1;
static void flatten_items(const char *path, const MenuItem *it, int n) {
    for (int k = 0; k < n && nflat < 320; k++) {
        if (it[k].sub) { char p[96]; snprintf(p, sizeof p, "%s \xE2\x80\xBA %s", path, it[k].label); flatten_items(p, it[k].sub, it[k].nsub); continue; }
        snprintf(flat[nflat].path, sizeof flat[0].path, "%s", path); flat[nflat].it = &it[k]; nflat++;
    }
}
static void flat_build(void) { if (nflat >= 0) return; nflat = 0; for (int m = 0; m < MENU_COUNT; m++) flatten_items(menus[m].title, menus[m].items, menus[m].n); }
/* Number of flat entries across all menus and submenus, separators included. */
int menu_item_count(void) { flat_build(); return nflat; }
/* Maps flat index i to its menu path and entry; false for separators, command entries (the
   palette lists those itself) and out-of-range i. See menu.h for ownership of the strings. */
bool menu_item_at(int i, const char **menu, const char **label, char *shortcut, size_t n, int *action) {
    flat_build();
    if (i < 0 || i >= nflat) return false;
    const MenuItem *it = flat[i].it;
    if (!it->label || it->action == ACT_NONE) return false;
    *menu = flat[i].path; *label = it->label; *action = it->action;
    if (it->shortcut) format_shortcut(it->shortcut, shortcut, n); else shortcut[0] = 0;
    return true;
}
