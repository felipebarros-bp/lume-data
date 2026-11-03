/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Main toolbar (Lume Data toolbar spec, TBR-001..052)
 * ===================================================
 *
 * A row of its own between the menu bar and the workspace. From left to right: workspace and
 * new query; the active tab's datasource, schema and transaction; execution (Run, Stop);
 * a flexible gap; search, panels and the overflow menu "...". The toolbar is a view of the
 * command registry (commands.h) and of the context snapshot: every button invokes a command
 * id, shows that command's enabled state and, when disabled, the reason in its tooltip.
 *
 * It never keeps its own idea of the connection, the panels or the execution: the datasource
 * comes from the active query tab, panels from the Panel Manager, execution from the Output.
 *
 * Preferences (visibility, compact mode, labels, preset, order, hidden and added actions, recent
 * datasources) live in toolbar.json of the active workspace: versioned, written atomically, a
 * corrupt file is set aside and the defaults apply (TBR-042 / 052).
 */
#include "lume.h"

typedef struct Toolbar Toolbar;

Toolbar *toolbar_create(App *a);                 /* reads the workspace and its toolbar.json */
void toolbar_destroy(Toolbar *t, App *a);        /* saves pending preferences */

float toolbar_height(Toolbar *t, App *a);        /* render pixels; 0 while hidden (TBR-006) */
/* Input and drawing of the bar in `bar` (directly under the menu bar); popovers, menus and
 * dialogs take their input here too, before any panel. */
void toolbar_frame(Toolbar *t, App *a, Rect bar);
void toolbar_draw_overlay(Toolbar *t, App *a);   /* dropdowns, popover, tooltip, dialogs: on top */
/* The ACT_* it owns (toolbar, workspace, the new execution / transaction / navigation commands,
 * and Run's safety confirmations). True when handled. */
bool toolbar_action(Toolbar *t, App *a, int act);
bool toolbar_modal_open(Toolbar *t);
bool toolbar_flag(Toolbar *t, int which);
const char *toolbar_workspace_dir(Toolbar *t);   /* the active workspace's folder, with a trailing slash ("" none) */   /* menu check marks: 0 shown, 1 compact, 2 labels */             /* a dialog, popover or dropdown holds the input */
/* The Output reports the end of an execution (schema changes, transaction follow-ups). */
void toolbar_execution_finished(Toolbar *t, App *a, Db *db, int exec_id, int status);

/* test harness */
bool toolbar_menu_pos(Toolbar *t, App *a, const char *label, float *x, float *y);
const char *toolbar_state_text(Toolbar *t, App *a);   /* one line: visible items, overflow, datasource, run state */
bool toolbar_export_prefs(Toolbar *t, App *a, const char *path);
void toolbar_run_to_file(Toolbar *t, App *a, const char *path);   /* Execute to File without the native dialog */
bool toolbar_add_action(Toolbar *t, const char *cmd_id);            /* Customize > Add, by command id */
void toolbar_print_names(Toolbar *t, App *a);                        /* accessible name of each control */
void toolbar_dump_menu(Toolbar *t);                                  /* open dropdown entries and their state */
void toolbar_dump_modal(Toolbar *t);                                 /* open dialog's title and text */
void toolbar_connect_failed(Toolbar *t, App *a, Connection *c);
bool toolbar_update_prompt(Toolbar *t, App *a, const char *title, const char *body, int notice);   /* update.c: a newer version (Download / Release Notes / Later / Skip This Version) or, notice=1, a one-button notice; false when another question is open */
void toolbar_crash_prompt(Toolbar *t, App *a, const char *body);   /* the previous run crashed: Send report / View report / Don't send (crashrep.c) */   /* the failure in a dialog (Retry / Edit connection... / Close) */
