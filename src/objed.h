/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Object source editor UI (spec "Object Source Editor & Database Object Deployment")
 * ==================================================================================
 *
 * A database object (view, function, procedure, package spec / body, trigger, type) opens in an
 * editor tab of its own, bound to the connection it came from (never to the toolbar's selection):
 *
 *   ┌ Object: FUNCTION public.calc(integer)   [DEV]  Modified · Syntax OK (local)    Problems 2 ┐
 *   │ [Save Draft] [Format ▾] [Validate ▾] [Diff] [Apply…] [⋯]                                   │
 *   ├────────────────────────────────────────────────────────────────────────────────────────────┤
 *   │ the SQL editor (same buffer, undo, highlight, completion as a query tab)                    │
 *   ├ Problems | Dependencies | Original source ──────────────────────────────────────────────────┤
 *   └ ⚠ line 4  INS002  DELETE without WHERE ...                                                  ┘
 *
 * The tab never runs its text as a console (Run is refused there, OBJ-062). Save Draft keeps the
 * text on disk (drafts/ in the preferences folder, restored on reopen, OBJ-009 / 033); Apply…
 * re-reads the server definition, plans (objplan.c), and opens the review: diff, the exact SQL,
 * dependents, risk, atomicity and every blocking reason. Nothing reaches the database before the
 * review is confirmed (and, for destructive plans or PROD, the specific consent is given). A remote
 * change since the editor opened it shows the three-way comparison instead (AT-04).
 *
 * Execution goes through objsrc.c (worker threads); this module only polls it from the frame.
 * Events land in the Output (validate / deploy / compile), an audit line without SQL text in
 * <prefs>/logs/object-audit.log (OBJ-071).
 */
#include "lume.h"
#include "objmodel.h"
#include "objengine.h"

typedef struct ObjDoc ObjDoc;

void objed_init(App *a);
void objed_shutdown(App *a);
/* Per frame, early: modal dialogs and the context menu take the input first; jobs advance. */
void objed_pre_update(App *a);
/* Per frame, last: context menu, review / diff / conflict / dependency dialogs. */
void objed_draw_overlay(App *a);
bool objed_modal_open(void);
bool objed_menu_pos(App *a, const char *label, float *x, float *y);   /* test harness */
void objed_handle_event(App *a, const SDL_Event *e);                   /* native save dialog results */

/* Opens (or focuses) the definition of an object of connection c. Loading is asynchronous. */
bool objed_open(App *a, Connection *c, const ObjKey *k);
/* Body of an object tab (called by the editor group). */
void objed_tab_body(App *a, ObjDoc *d, struct Editor *ed, Rect body);
void objed_tab_closed(App *a, ObjDoc *d);
bool objed_doc_dirty(const ObjDoc *d);

/* Explorer: objects of a schema, loaded on demand (OBJ-004 / 064). */
typedef struct ObjListView { const ObjListItem *items; int n; bool loading, loaded; const char *error; } ObjListView;
ObjListView objed_list(App *a, Connection *c, const char *schema, bool request);
void objed_list_invalidate(Connection *c);
bool objed_has_draft(Connection *c, const ObjKey *k);
/* Context menus (OBJ-059): an object, or the objects group of a schema (templates). */
void objed_object_menu(App *a, Connection *c, const ObjKey *k, float x, float y);
void objed_group_menu(App *a, Connection *c, const char *schema, float x, float y);

/* Commands (palette / toolbar / harness), acting on the active object tab. */
enum { OBJCMD_SAVE_DRAFT = 1, OBJCMD_FORMAT, OBJCMD_FORMAT_SEL, OBJCMD_VALIDATE, OBJCMD_VALIDATE_SERVER, OBJCMD_DIFF, OBJCMD_APPLY,
       OBJCMD_COMPARE, OBJCMD_REFRESH, OBJCMD_NEXT_PROBLEM, OBJCMD_PREV_PROBLEM, OBJCMD_DISCARD, OBJCMD_EXPORT, OBJCMD_DEPLOY_SQL,
       OBJCMD_GOTO_DEF, OBJCMD_SHOW_ORIGINAL, OBJCMD_COMPILE, OBJCMD_DEPENDENCIES, OBJCMD_PROPERTIES, OBJCMD_RECONCILE, OBJCMD_CONSOLE,
       OBJCMD_RENAME, OBJCMD_APPLY_ALL, OBJCMD_MV_REFRESH, OBJCMD_EXTRACT_CTE, OBJCMD_NAMED_PARAMS, OBJCMD_QUALIFY, OBJCMD_SUBMIT_REVIEW };
bool objed_command(App *a, int cmd);
/* Harness: writes the active object tab's state, plan and problems to stdout. */
void objed_dump(App *a);
bool objed_idle(App *a);   /* harness: objawait */
/* Harness: export the active object's text to a path without the native dialog. */
bool objed_export_to(App *a, const char *path);
