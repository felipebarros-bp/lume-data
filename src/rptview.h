/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Report Studio (LI-038..070) and its assistant panel (LI-009..014 / 051 / 059): report tabs with a
 * native SDL3 canvas of KPI / line / bar / table / text cards, themes, period and page filters,
 * provenance, preview / read mode, local save with versions and autosave, PNG export; the
 * "Lume Intelligence" panel docked on the right with visible context, consent, plan approval,
 * states and cancel. Model (rpt.h), catalog and compiler (sem.h), tool contract (agent.h), read-only
 * policy (sqlguard.h). Queries run on the executor's worker inside a read-only transaction; the SDL
 * loop never waits.
 */
#include "lume.h"

typedef struct RvDoc RvDoc;

void rptview_init(App *a);
void rptview_shutdown(App *a);          /* autosaves dirty reports */
void rptview_pre_update(App *a);        /* query runner, AI jobs, exports, menus */
void rptview_draw_overlay(App *a);
void rptview_tab_body(App *a, RvDoc *d, Rect body);

/* tab plumbing (panels.c) */
bool rptview_doc_dirty(const RvDoc *d);
void rptview_tab_closed(App *a, RvDoc *d);   /* keeps the autosave of unsaved changes */

/* harness: rpt <op> [args]; rptstate */
void rptview_cmd(App *a, const char *arg);
void rptview_dump(App *a);
bool rptview_idle(App *a);
bool rptview_open_path(App *a, const char *path);
bool rptview_drop_file(App *a, const char *path, SDL_WindowID win, float x, float y);   /* an image dropped on the assistant's chat is attached; false: not over the chat */
   /* File > Open of a .lrpt */   /* no query queued / running, no AI request in flight (rpt await) */
/* Settings > Lume Intelligence (roadmap item 25): drawn by the assistant module into the Settings screen */
void rptview_settings_intelligence(App *a, Rect body, float *y);
void rptview_ask_sql(App *a, const char *question);   /* the assistant, about the active query tab (from the plan panel) */
