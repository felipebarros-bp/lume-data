/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * ER diagram tabs (docs/diagrams.md): the canvas, find, menus, properties, minimap, exports and the
 * catalog reads behind them. The model is dgm.h; the catalog is a schidx per connection and scope,
 * read with a few bulk statements on the connection's worker (read-only, cancellable) and cached.
 */
#include "lume.h"

struct DgDoc;
void dgmview_init(App *a);                 /* commands */
void dgmview_shutdown(App *a);
void dgmview_pre_update(App *a);           /* catalog jobs, layout workers, autosave, shortcuts */
void dgmview_draw_overlay(App *a);         /* menus and dialogs above everything */
void dgmview_tab_body(App *a, struct DgDoc *d, Rect body);
void dgmview_tab_closed(App *a, struct DgDoc *d);
bool dgmview_doc_dirty(const struct DgDoc *d);
const char *dgmview_doc_conn(const struct DgDoc *d);

/* entry points (Explorer menus, palette) */
bool dgmview_open_table(App *a, Connection *c, const char *schema, const char *table);   /* DGM-001 */
bool dgmview_new_for_schema(App *a, Connection *c, const char *schema);                  /* DGM-002 picker */
bool dgmview_new_empty(App *a, Connection *c);                                            /* DGM-003 */
bool dgmview_add_to_current(App *a, Connection *c, const char *schema, const char *table);   /* DGM-085 */
bool dgmview_open_file(App *a, const char *path);
bool dgmview_has_diagram_for(App *a, const char *conn_id);

/* harness: dgm <op> [args]; dgmstate */
void dgmview_cmd(App *a, const char *arg);
void dgmview_dump(App *a);
bool dgmview_ready(App *a);                /* the active diagram's catalog is loaded (harness await) */
