/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Visual Table Designer: the tab (TBL-001..100). The model, diff, validation and DDL planner are
 * in tbl.h; this is the non-modal designer tab, its review & apply dialog, drafts on disk and the
 * executor that runs a reviewed plan.
 *
 *   draft -> validation -> diff -> review -> apply      (spec 11, "Decisão de produto")
 *
 * Nothing reaches the server before the explicit Apply in the review, except catalog reads of
 * the one table (snapshot, drift check, read-back) and the optional, bounded data checks the user
 * asks for. Every statement runs on the executor's worker; the SDL loop never waits.
 */
#include "lume.h"

typedef struct TdDoc TdDoc;
struct Editor;

void tdes_init(App *a);
void tdes_shutdown(App *a);                         /* keeps unsaved drafts on disk */
/* Modify Table... (after: 0 = edit, 1 = open its CREATE in a SQL tab, 2 = Duplicate Structure,
 * 3 = Drop Table... review) */
bool tdes_open_table(App *a, Connection *c, const char *schema, const char *name, int after);
bool tdes_new_table(App *a, Connection *c, const char *schema);   /* New > Table... */
/* opens the table with an index already in the draft (a proposal from the execution plan): it is
 * applied only through the designer's own review (EPL-060 / 062) */
bool tdes_open_with_index(App *a, Connection *c, const char *schema, const char *name, const char *cols_csv, const char *index_name, const char *why);
bool tdes_supported(const Connection *c);
/* New table from a prepared draft (Lume Intelligence, roadmap item 17): opens the designer with it as an
 * unsaved new table; nothing reaches the server until the user reviews and applies it there. */
struct TdTable;
bool tdes_new_table_from(App *a, Connection *c, const struct TdTable *draft, const char *note);           /* a designer exists for this driver */

/* per frame */
void tdes_pre_update(App *a);                       /* jobs; a dialog owns the input */
void tdes_draw_overlay(App *a);                     /* review / close / drop dialogs, menus */
void tdes_tab_body(App *a, TdDoc *d, struct Editor *ed, Rect body);

/* tab plumbing (panels.c) */
bool tdes_doc_dirty(const TdDoc *d);
void tdes_tab_closed(App *a, TdDoc *d);
bool tdes_request_close(App *a, TdDoc *d, int tab_id);   /* true when the designer asks first (unsaved draft) */
bool tdes_modal_open(void);

/* test harness: td <op> [args]; tdstate */
void tdes_cmd(App *a, const char *arg);
void tdes_dump(App *a);
bool tdes_menu_pos(App *a, const char *label, float *x, float *y);
