/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Menu bar: the in-window menu strip at the top of the main window (Lume draws its own
 * menus instead of using the OS menu bar) and its dropdowns.
 *
 * The menu does not execute anything itself. menu_update() returns an ACT_* code for the
 * entry the user clicked and the caller (main loop / panels.c) dispatches it, so the same
 * codes are shared by the menu, the keyboard shortcuts and the command palette.
 *
 * Coordinates are render pixels (UI points already scaled by px()). All calls happen on the
 * UI thread.
 */
#include "lume.h"

/* Action codes produced by menu entries, shortcuts and the command palette. ACT_NONE (0)
   means "nothing chosen"; separators also carry ACT_NONE. */
enum {
    ACT_NONE, ACT_NEW_QUERY, ACT_OPEN_DB, ACT_NEW_CONN, ACT_EDIT_CONN, ACT_TOGGLE_CONN, ACT_QUIT,
    ACT_UNDO, ACT_REDO, ACT_CUT, ACT_COPY, ACT_PASTE, ACT_SELECT_ALL, ACT_COMPLETE,
    ACT_RUN, ACT_RUN_ALL, ACT_REFRESH, ACT_FIND, ACT_EXPORT, ACT_FETCH_100, ACT_FETCH_500, ACT_FETCH_1000, ACT_FETCH_5000, ACT_FETCH_MORE, ACT_FETCH_ALL,
    ACT_RUN_NEW_TAB, ACT_HISTORY, ACT_NEXT_TAB, ACT_PREV_TAB, ACT_CLOSE_TAB, ACT_COLUMNS,
    ACT_BUDGET_64, ACT_BUDGET_256, ACT_BUDGET_1024,
    ACT_FMT_GROUP, ACT_FMT_DEC_RAW, ACT_FMT_DEC_2, ACT_FMT_DEC_4, ACT_FMT_DATE_ISO, ACT_FMT_DATE_DMY, ACT_FMT_DATE_MDY,
    ACT_FMT_TZ_STORED, ACT_FMT_TZ_UTC, ACT_FMT_TZ_LOCAL,
    ACT_TOGGLE_SIDEBAR, ACT_TOGGLE_RESULTS, ACT_ZOOM_IN, ACT_ZOOM_OUT, ACT_ZOOM_RESET,
    ACT_ABOUT, ACT_CHECK_UPDATES, ACT_CANCEL, ACT_SHOW_OUTPUT, ACT_CONTINUE_ON_ERROR,
    /* panels and layouts (LUME-PANEL-001), handled by panels.c */
    ACT_PALETTE, ACT_PANELS, ACT_PANEL_EXPLORER, ACT_PANEL_RESULTS, ACT_PANEL_OUTPUT, ACT_PANEL_INSPECTOR,
    ACT_FOCUS_MODE, ACT_MAXIMIZE_PANEL, ACT_COLLAPSE_PANEL, ACT_HIDE_PANEL, ACT_UNDO_LAYOUT,
    ACT_RESIZE_GROW, ACT_RESIZE_SHRINK, ACT_LAST_PANEL,
    ACT_LAYOUT_DEFAULT, ACT_LAYOUT_QUERY_FOCUS, ACT_LAYOUT_DATA, ACT_LAYOUT_SAVE_AS, ACT_LAYOUTS, ACT_LAYOUT_EXPORT, ACT_LAYOUT_IMPORT,
    ACT_NEW_EDITOR_TAB, ACT_SPLIT_RIGHT, ACT_SPLIT_DOWN, ACT_COMPACT, ACT_TOP_EDGE, ACT_REDUCE_MOTION,
    /* main toolbar (LUME toolbar spec, TBR-xxx): execution, session, navigation, toolbar itself */
    ACT_RUN_SELECTION, ACT_EXPLAIN, ACT_EXPLAIN_ANALYZE, ACT_RUN_PARAMS, ACT_RUN_TO_FILE, ACT_RERUN_LAST, ACT_EXEC_SETTINGS,
    ACT_CONNECT, ACT_DISCONNECT, ACT_RECONNECT, ACT_CANCEL_CONNECT,
    ACT_TX_AUTO, ACT_TX_MANUAL, ACT_COMMIT, ACT_ROLLBACK, ACT_SESSION_DETAILS,
    ACT_SWITCH_TAB, ACT_QUICK_OPEN, ACT_PANEL_HISTORY,
    ACT_TOOLBAR_TOGGLE, ACT_TOOLBAR_COMPACT, ACT_TOOLBAR_LABELS, ACT_TOOLBAR_CUSTOMIZE, ACT_TOOLBAR_RESET, ACT_TOOLBAR_FOCUS, ACT_TOOLBAR_EXPORT,
    ACT_WORKSPACE_NEW, ACT_WORKSPACE_OPEN, ACT_WORKSPACE_SETTINGS, ACT_WORKSPACE_CLOSE,
    ACT_COUNT
};

/* Lets the command registry decide the enabled state of entries (dimmed when its rule says no;
 * a click still reaches do_action, whose gate explains why in the status bar). */
void menu_set_enable_hook(bool (*fn)(int act));
/* Whether an ACT_* setting is on now: its entry shows a check mark. */
void menu_set_check_hook(bool (*fn)(int act));
/* Allocates a menu bar with no dropdown open. Caller owns it; free with menu_destroy(). */
Menu *menu_create(void);
/* Frees a menu created by menu_create(). */
void menu_destroy(Menu *m);
/* True while a dropdown is open (the menu then swallows keyboard input). */
bool menu_is_open(Menu *m);
/* Lays out and draws the menu bar inside `bar` (render pixels) and processes this frame's
   mouse/keyboard input: opens/switches/closes dropdowns and, on a click on an enabled entry,
   returns its ACT_* code (ACT_NONE otherwise). While a dropdown is open it consumes the mouse
   press and clears the frame's key and text events. The dropdown itself is drawn later by
   menu_draw_overlay(). */
int menu_update(Menu *m, App *a, Rect bar);   /* handles input + draws the bar, returns an action */
/* Draws the open dropdown (if any) on top of everything else; call after the rest of the
   frame has been drawn. Uses the bar rect remembered by the last menu_update(). */
void menu_draw_overlay(Menu *m, App *a);      /* dropdown, drawn last */

/* Every menu entry, for the command palette (PNL-036): i in [0, menu_item_count()). */
/* Total number of entries across all menus, separators included. */
int menu_item_count(void);
/* Describes flat entry i: *menu receives the menu title, *label the entry label (both static
   strings, do not free), *action its ACT_* code, and `shortcut` (buffer of n bytes) the
   platform-formatted shortcut or "". Returns false for separators and out-of-range i, in
   which case the outputs are left untouched. Note: the entry's enabled flag is not reported. */
bool menu_item_at(int i, const char **menu, const char **label, char *shortcut, size_t n, int *action);
