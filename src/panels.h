/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Workspace shell (LUME-PANEL-001)
 * ================================
 *
 * Puts the docking engine (dock.h) on screen: the panel bar, the collapsed strips, panel group
 * headers with their tabs and actions, splitters, drag-and-drop with a preview, the auto-hide
 * overlay, editor groups with query tabs, the command palette, named layouts and the layout
 * file. It owns the Lume Data panels' registration (Explorer, Results, Output, Inspector) and
 * every editor tab; the content of each panel stays in its module (explorer.c, results.c,
 * output.c, editor.c), so moving or hiding a panel never touches its data (R-01 / R-03).
 *
 * Files in the preferences folder:
 *   layout.json    the current arrangement (schema 1), saved coalesced and atomically
 *   layouts.json   named layouts and the layout bound to each connection (PNL-033 / 034)
 *   keymap.txt     optional overrides of the panel shortcuts (PNL-036), "command=Cmd+Shift+X"
 */
#include "lume.h"
#include "dock.h"

/*
 * Units: Rect arguments are render pixels (UI points * a->scale), like the rest of the UI;
 * harness positions are window units. All calls are made from the UI thread.
 */

/* Opaque shell state, owned by main.c through a->panels. */
typedef struct Panels Panels;

/* Creates the shell: registers the panels, takes ownership of a->editor as the first query
 * tab, restores the saved layout and the named layouts. Free with panels_destroy(). */
Panels *panels_create(App *a);            /* after explorer / results / output / editor exist */
/* Frees the shell and every editor tab, closes the panel windows; a->editor becomes NULL. */
void panels_destroy(Panels *p, App *a);   /* saves the layout; frees every editor tab */

/* Lays out and draws the workspace inside `body` (between the menu bar and the status bar). */
void panels_frame(Panels *p, App *a, Rect body);
/* Menus, drag ghost, palette and tooltips: drawn after every panel. */
void panels_draw_overlay(Panels *p, App *a);
/* Keyboard: panel shortcuts (keymap-aware); returns an ACT_* or ACT_NONE. */
int panels_shortcut(Panels *p, App *a);
/* Window-menu and palette actions; true when handled. */
bool panels_action(Panels *p, App *a, int action);
/* Native dialog results (export / import layout). */
bool panels_handle_event(Panels *p, App *a, const SDL_Event *e);
/* A modal of the shell (palette, name prompt, confirmation) owns the keyboard. */
bool panels_modal_open(Panels *p);

/* panel helpers for the rest of the app */
void panels_show(Panels *p, App *a, const char *panel_id);         /* show + activate */
bool panels_is_visible(Panels *p, const char *panel_id);          /* on screen in the main window */
/* After a run: Results comes into view (shown when hidden, unfolded when collapsed, active tab),
 * without taking the keyboard from the editor. Changed 01/11/2026 (Isa): a folded Results made a
 * finished run look like nothing happened. panels_reveal does the same for any panel (Output on errors). */
void panels_reveal_results(Panels *p, App *a);
void panels_reveal(Panels *p, App *a, const char *panel_id);
/* New query tab bound to the current connection (Cmd+T / Cmd+N). */
void panels_new_editor_tab(Panels *p, App *a, const char *text);
const char *panels_status(Panels *p);     /* "Focus mode" etc. for the status bar, or "" */
/* An ACT_* chosen in the command palette, for main.c to run once per frame (ACT_NONE = none). */
int panels_take_action(Panels *p);
/* ---- extension API (PNL-047) ----
 * A module (any language, not only SQL) adds panels without changes to the shell:
 *   1. before panels_create, main.c calls panels_add_module("vendor.module", init);
 *   2. panels_create runs init(P, a) for every enabled module, before reading layout.json, so
 *      the module's panels come back where the user left them; init calls panels_register with
 *      a DockPanelDesc (id, title, icon, caps, draw, and the optional create / destroy /
 *      visibility / menu / menu_run hooks, see dock.h);
 *   3. a module is disabled with LUME_DISABLE_MODULES="id,id" or the line "id=off" in
 *      <prefs>/modules.txt: its init is skipped and saved positions of its panels are ignored;
 *   4. panels_unregister removes a panel at run time: its window closes, visibility(false) and
 *      destroy are called, and no index of it is left in the layout tree.
 * The Lume Data panels themselves cannot be unregistered. */
typedef void (*PanelsModuleInit)(Panels *p, App *a);
void panels_add_module(const char *module_id, PanelsModuleInit init);   /* id and init must outlive the app */
bool panels_register(Panels *p, App *a, const DockPanelDesc *desc);     /* false: duplicate id, registry full, bad id */
bool panels_unregister(Panels *p, App *a, const char *panel_id);

/* Query tab <-> connection (TBR-008 / 009): the active tab's own connection (may be closed;
 * NULL when unbound), with the tab title; an explicit change of the active tab's connection;
 * and the hook for a background connect that finished. */
/* Query tabs (toolbar: Switch Tab, workspaces). TabInfo.ed is borrowed for this frame only. */
typedef struct TabInfo { int id; bool active; char title[48], conn_id[16]; struct Editor *ed; } TabInfo;
int panels_tab_count(Panels *p);
bool panels_tab_info(Panels *p, App *a, int k, TabInfo *out);
void panels_activate_tab(Panels *p, App *a, int id);
void panels_close_tab(Panels *p, App *a, int id);   /* no confirmation (workspace restore) */
int panels_open_tab(Panels *p, App *a, const char *title, const char *conn_id, const char *text);
void panels_close_all_tabs(Panels *p, App *a);
/* Object definition tabs (objed.c): the document is owned by objed. */
struct ObjDoc;
int panels_open_object_tab(Panels *p, App *a, const char *title, const char *conn_id, const char *text, struct ObjDoc *obj);
struct ObjDoc *panels_tab_obj(Panels *p, int id);
/* Table designer tabs (tdes.c): the document is owned by tdes. */
struct TdDoc;
struct RvDoc;
struct GdDoc;
int panels_open_designer_tab(Panels *p, App *a, const char *title, const char *conn_id, struct TdDoc *td);
struct TdDoc *panels_active_designer(Panels *p, App *a);
int panels_open_report_tab(Panels *p, App *a, const char *title, struct RvDoc *rv);
struct RvDoc *panels_active_report(Panels *p, App *a);
/* file-backed editor tabs (Source Control, GIT-017 / 064): open (activates if already open), save
 * (atomic; never touches a database), dirty files under a repository root, the active file tab */
int panels_open_file_tab(Panels *p, App *a, const char *path);
bool panels_flag(Panels *p, int which);   /* menu check marks: 0 compact headers, 1 top edge allowed */
bool panels_save_tab(Panels *p, App *a, int id);
bool panels_tab_dirty_file(Panels *p, int id);
const char *panels_tab_path(Panels *p, int id);
int panels_active_file_tab(Panels *p, App *a);
int panels_dirty_files_under(Panels *p, const char *root, char out[][256], int max);
int panels_save_files_under(Panels *p, App *a, const char *root);
int panels_open_diff_tab(Panels *p, App *a, const char *title, struct GdDoc *gd);
/* the buffer vs the file on disk (gitview.c draws it) */
void panels_compare_buffer_disk(App *a, int tab_id);
void panels_set_tab_title(Panels *p, int id, const char *title);
struct ObjDoc *panels_active_obj(Panels *p, App *a);
struct Editor *panels_tab_editor(Panels *p, int id);
bool panels_tab_exists(Panels *p, int id);
Connection *panels_tab_connection(Panels *p, App *a, char *title, size_t tn);
void panels_set_tab_connection(Panels *p, App *a, Connection *c);
void panels_connection_ready(Panels *p, App *a, Connection *c);
Dock *panels_dock(Panels *p);
/* Registered panels with their real state, for the toolbar's Panels menu (TBR-036); toggling
 * uses the Panel Manager's own show / hide, never a second visibility model. */
typedef struct PanelInfo { char id[48], title[48], state[24]; bool visible; DockBadge badge; } PanelInfo;
int panels_panel_count(Panels *p);
bool panels_panel_info(Panels *p, App *a, int i, PanelInfo *out);
void panels_panel_toggle(Panels *p, App *a, int i);
void panels_palette_dump(Panels *p, App *a, const char *query);   /* harness: palette results for a query */             /* engine, for tests and the harness */
bool panels_menu_pos(Panels *p, App *a, const char *label, float *x, float *y);   /* test harness */
/* Writes the current arrangement (no SQL, data or credentials) to path; false on I/O error. */
bool panels_export_layout(Panels *p, App *a, const char *path);
/* Parses and validates path, then opens a confirmation; true when it was readable and valid
 * (the layout is replaced only after the user confirms). */
bool panels_import_layout(Panels *p, App *a, const char *path);   /* asks before replacing */
/* native windows of floating / detached panels (PNL-019 / 020 / 021) */
void panels_input_begin(Panels *p);                  /* per-frame reset of each window's input */
void panels_render_floats(Panels *p, App *a);        /* after the main window is presented */
bool panels_is_floating(Panels *p, const char *panel_id);   /* floating or detached: drawn in its own window */
void panels_shot_float(Panels *p, const char *path); /* harness */
void panels_move_float(Panels *p, int x, int y);     /* harness */
void panels_revalidate_floats(Panels *p);            /* harness / display changes */
bool panels_float_pos(Panels *p, int *x, int *y);    /* harness */
SDL_WindowID panels_float_window_id(Panels *p);     /* harness: close / inspect the first panel window */
