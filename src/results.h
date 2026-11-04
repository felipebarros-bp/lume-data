/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Results panel (results.c)
 * =========================
 *
 * Result tabs: drawing and input over the grid model (grid.h). One Results holds every tab
 * (US-006); each tab owns its ResultSet, its grid state, its pending edits and its column
 * layout. Besides the grid, Results owns the overlays that act on a result: value viewer,
 * filter popover, columns panel, export / review / chart dialogs and context menus.
 *
 * Per frame (all on the UI thread): results_pre_update before any panel (overlays take input
 * first), results_update to draw the panel, results_draw_overlay after every panel.
 *
 * Units: Rects passed in are render pixels; the harness positions (results_*_pos) are returned
 * in window units.
 */
#include "lume.h"
#include "db.h"

/* ---- lifetime ------------------------------------------------------------ */

/* Creates the (empty) Results with default export options; freed by results_destroy. */
Results *results_create(void);
/* Frees every tab, the history and the overlay state, then rs itself. */
void results_destroy(Results *rs);

/* ---- landing results ------------------------------------------------------- */

/* Lands a result in a tab (takes ownership of *set). mode = RUN_* from lume.h; sql is kept
 * for the history (US-031); qtable is the quoted identifier for previews (US-012).
 * title may be NULL or "Results" (a title is then derived from the SQL); len < 0 means
 * NUL-terminated. May close the oldest tab (tab limit) and discard others (memory budget). */
void results_set(Results *rs, App *a, ResultSet *set, const char *title, const char *sql, int len, int mode, const char *qtable);

/* ---- per frame ------------------------------------------------------------- */

/* Input of the overlays (menus, popovers, dialogs, cell editor) and any queued server re-run;
 * call once per frame before any panel. */
void results_pre_update(Results *rs, App *a);        /* overlay input; call before any panel */
/* Draws the Results panel (tab strip, meta line, toolbar, grid) inside bounds and handles its
 * input. */
void results_update(Results *rs, App *a, Rect bounds);
/* Draws the open overlays above every panel; draw-only (input ran in results_pre_update). */
void results_draw_overlay(Results *rs, App *a);     /* menus, popovers, viewer, dialogs */

/* ---- actions (menus and shortcuts) ------------------------------------------ */

/* Fetches up to n more rows from the current tab cursor; true when rows arrived. */
bool results_fetch_more(Results *rs, App *a, int n);
bool results_fetch_all(Results *rs, App *a);           /* every remaining row, on a worker */
bool results_fetching(Results *rs);
/* Releases cursors on db and marks its tabs stale; call before the connection closes. */
void results_release_db(Results *rs, Db *db);       /* before a connection closes */
/* Copies the current selection to the clipboard as TSV, without headers. */
void results_copy_selection(Results *rs);           /* TSV */
/* Selects every cell of the current tab. */
void results_select_all(Results *rs);
/* Opens and focuses the find bar of the current tab (the Output find on the Output tab). */
void results_find(Results *rs);                     /* opens the search bar */
/* Opens the export dialog; scope 0 selection, 1 loaded rows, 2 full result. Refused while the
 * tab has unsaved edits. */
void results_export_dialog(Results *rs, App *a, int scope);   /* options first (US-024) */
/* Writes the current tab (or the Output events) to path with the dialog options, once the
 * save dialog returns. Blocks while writing; true when a file was written. */
bool results_export(Results *rs, App *a, const char *path);   /* writes with the chosen options */
/* Toggles the columns panel of the current tab. */
void results_columns_panel(Results *rs);            /* hide / reorder / freeze (US-005) */
/* Opens the History menu: this session's executions, then earlier sessions. Choosing an
 * entry puts its SQL in the editor without running it. */
void results_history_menu(Results *rs, App *a);     /* recent executions (US-031) */
/* Selects the next (dir >= 0) or previous tab, wrapping around. */
void results_next_tab(Results *rs, int dir);
/* Closes the current tab, asking first when it has unsaved edits. */
bool results_close_tab(Results *rs, App *a);        /* current tab; false when none */

/* ---- state queries ----------------------------------------------------------- */

/* True while an overlay of Results owns the keyboard (global shortcuts should wait). */
bool results_modal_open(Results *rs);               /* a viewer, popover or dialog owns the keyboard */
/* True while a Results text field has the keyboard (typing must not trigger shortcuts). */
bool results_text_focus(Results *rs);               /* a text field in the grid has the keyboard */
/* Bytes held by the rows of every live tab. */
size_t results_bytes(Results *rs);                  /* memory held by every tab */

/* Execution Output integration (docs/execution-output.md) */
/* Shows (on) or leaves the Output tab; with docking on, only queues a request for the shell. */
void results_show_output(Results *rs, bool on);     /* the persistent Output tab */
/* True while the Output tab is the one shown. */
bool results_output_active(Results *rs);
/* Docking (LUME-PANEL-001): Output and the record view become panels of their own. With this on,
 * Results no longer draws an Output tab nor the in-grid record view; requests to show them are
 * collected and taken by the shell once per frame. */
void results_set_panels_external(Results *rs, bool on);
/* Tells Results whether the Inspector panel is visible, every frame. */
void results_set_inspector_shown(Results *rs, bool shown);   /* shell -> label of the record view item */
/* Hands over (and clears) this frame's requests: show Output, show Results, toggle Inspector. */
void results_take_requests(Results *rs, bool *show_output, bool *show_results, bool *toggle_inspector);
/* Panel bar / collapsed strip summary: tab count, unsaved rows across tabs, and whether the
 * current tab holds an error (a cancelled run is not one). */
void results_badge(Results *rs, int *tabs, int *unsaved_rows, bool *error);
/* The current result tab for the assistant: its run's error ("" when it succeeded), the SQL that
 * produced it and the connection name, and its columns as "name type, ..." (never rows).
 * false when there is no result. */
/* A local summary of the current result for the assistant (roadmap item 19): per column the rows,
 * nulls, distinct values (counted up to 1000) and min / max as text; computed here, never rows.
 * false when there is no result. */
bool results_current_summary(Results *rs, char *out, size_t n);
/* Results belong to the query tab that ran them: the owner of the next results_set, the start of a
 * tab's run (its earlier results go, pinned / unsaved ones stay), the active tab changing. */
void results_set_owner(Results *rs, const void *editor);
void results_dump(Results *rs);   /* harness: resstate */
void results_begin_run(Results *rs, App *a, const void *editor);
void results_follow_tab(Results *rs, const void *editor);
bool results_current_info(Results *rs, char *err, size_t en, char *sql, size_t sn, char *conn, size_t cn, char *cols, size_t coln);
/* Draws the Inspector panel (record view of the selected row) inside bounds, with its input. */
void results_draw_inspector(Results *rs, App *a, Rect bounds);
/* exec_id of the current result tab. */
/* The next RUN_REPLACE result lands in the tab with this exec_id (0: the current tab). */
void results_target_replace(Results *r, int exec_id);
int results_current_exec_id(Results *rs);           /* id of the current result tab, 0 none */
/* the SQL that produced the current tab and its session (NULL for derived tabs); owned by the tab */
const char *results_current_sql(Results *rs, struct Db **db);
/* Selects the tab with that exec_id; true when found and its rows are still loaded. */
bool results_select_exec(Results *rs, int exec_id); /* activates the tab with that id */
int results_tab_count(Results *rs);
bool results_tab_info(Results *rs, int i, int *exec_id, char *title, size_t n, bool *pinned, bool *current);
/* Records which Output execution produced the tab tab_exec. */
void results_tag_output(Results *rs, int tab_exec, int out_exec);   /* links a tab to its Output execution */
/* Output execution of the current tab. */
int results_output_of_current(Results *rs);         /* Output execution id of the current tab, 0 none */
/* Word-wraps s inside rc (lines of 1.35 x the font height, shifted up by scroll pixels) and
 * returns the total height in pixels; draws only when draw is true (false = measure). */
float results_draw_wrapped(Font *f, SDL_Renderer *rr, const char *s, Rect rc, Color c, float scroll, bool draw);

/* test harness targets (window units): a visible cell, a column header, an open menu item */
/* Centre of display cell (row, col) of the current tab; false when not on screen. */
bool results_cell_pos(Results *rs, App *a, int row, int col, float *x, float *y);
/* Centre of the header of display column col; false when not visible. */
bool results_head_pos(Results *rs, App *a, int col, float *x, float *y);
/* Centre of the item labelled label in the open context menu; false when absent. */
bool results_menu_pos(Results *rs, App *a, const char *label, float *x, float *y);
