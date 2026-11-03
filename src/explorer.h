/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Explorer panel
 * ==============
 *
 * The "CONNECTIONS" tree on the left: one root per saved connection, then table / view
 * groups (or one group per schema), tables and, on expand, their columns. Handles mouse,
 * keyboard and a right-click context menu (connect, disconnect, edit, refresh, new).
 * The tree mirrors a->conns and is rebuilt from it; the nodes never own catalog data.
 * UI thread only.
 */
#pragma once
#include "lume.h"

/* Creates an empty explorer; call explorer_rebuild to fill it. Free with explorer_destroy. */
Explorer *explorer_create(void);
/* Frees the tree and the explorer. */
void explorer_destroy(Explorer *e);
/* Rebuilds the tree from a->conns (call after connect/disconnect/add/edit/delete). */
void explorer_rebuild(Explorer *e, App *a);
/* Reloads the active connection's tables, then rebuilds. */
void explorer_refresh(Explorer *e, App *a);
bool explorer_reading(App *a, Db *db);
void explorer_dump_rows(Explorer *e);     /* test harness */   /* the Explorer is reading the catalog on this session */
/* Per-frame input handling and drawing of the panel inside `bounds` (pixels). May switch
 * the active connection, connect, open a preview or the connection dialog. */
void explorer_update(Explorer *e, App *a, Rect bounds);
/* Draws the context menu on top of everything else; call after all panels are drawn. */
void explorer_draw_overlay(Explorer *e, App *a);     /* context menu */
void explorer_dump(Explorer *e);   /* harness: "explorer" + the visible rows ("> Folder (n)", indent by depth) */
Connection *explorer_selected_conn(Explorer *e);     /* connection of the selected row, or NULL */
/* Centre, in window units (not pixels), of the first visible row labelled `label`; false
 * when there is none or it is scrolled out of view. */
bool explorer_node_pos(Explorer *e, App *a, const char *label, float *x, float *y);   /* test harness */
/* Test harness: nsx "<connection>" <op> [arg] drives the Schemas / Others tree (DBX-126). */
void explorer_nsx_cmd(Explorer *e, App *a, const char *arg);
bool explorer_nsx_ready(App *a, const char *arg);   /* harness: nsx "<connection>" await [ms] */
