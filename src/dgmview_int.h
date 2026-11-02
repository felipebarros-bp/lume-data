/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* ER diagram tabs: shared between dgmview.c (core), dgmview_canvas.c (drawing / input) and
 * dgmview_ui.c (find, menus, dialogs, properties, exports, assistant). */
#include "dgmview.h"
#include "dgm.h"
#include "schidx.h"
#include "ui.h"
#include "draw.h"
#include "font.h"
#include "theme.h"
#include "exec.h"
#include "ai.h"

#define DG_MAX_DOCS 16
#define DG_MAX_CATS 6
#define DG_HIST 50
#define DG_HITS 4000

enum { CS_NONE, CS_WAIT, CS_RUN, CS_READY, CS_ERROR };
typedef struct DgCat {                  /* one connection + scope: a schidx shared by its diagrams */
    bool used;
    char conn_id[16], scope[1200];
    SchIdx x;
    int state;                          /* CS_* */
    ExecJob *job; char *stmts[4]; int kinds[4]; int nst;
    char err[300];
    double t0, ms;                      /* read started, duration */
    bool stale;                         /* loaded from the cache without a connection (offline) */
    unsigned version;                   /* bumps on every (re)load: docs rebind */
    int statements;                     /* catalog statements run for this scope (DGM-071) */
} DgCat;

enum { DLG_NONE, DLG_CONFIRM_ADD, DLG_PROMPT, DLG_SCOPE, DLG_EXPORT, DLG_PICKER, DLG_AI_CONSENT, DLG_AI_RESULT, DLG_RELATION };
enum { PR_ALIAS, PR_NOTE, PR_AREA, PR_ENTITY, PR_ATTRS, PR_RENAME, PR_REL_LABEL, PR_EVIDENCE, PR_NOTE_EDIT, PR_DIAGRAM_NAME };
enum { AI_EXPLAIN, AI_ORGANISE, AI_CONCEPTUAL };
enum { EXF_PNG, EXF_SVG, EXF_PDF, EXF_MERMAID, EXF_GRAPHML, EXF_DRAWIO, EXF_HTML, EXF_JSON, EXF_COUNT };
enum { EXS_SELECTION, EXS_VISIBLE, EXS_DIAGRAM };

typedef struct DgView { float zoom, panx, pany; char sel[DG_KEY]; } DgView;
typedef struct DgProposal { int kind; char title[80]; char tables[1600]; bool accepted, rejected; } DgProposal;   /* AI_ORGANISE groups */

typedef struct DgDoc {
    bool used;
    int id;                             /* stable, for the harness */
    Dgm d;
    char path[1200];                    /* the .lume-erd file */
    char conn_id[16];
    int cat;                            /* index in G.cats, -1 none */
    unsigned cat_version;               /* the catalog version this doc was bound to */
    bool laid_out, fitted, need_fit;
    int pending_focus_ti;               /* open on a table once the catalog arrives (-1 none) */
    char pending_table[DG_KEY];         /* "schema.table" to open / add once loaded */
    int pending_kind;                   /* 0 focus open, 1 add to diagram, 2 schema picker, 3 empty */
    char pending_schema[200];
    char *pending_list;                 /* kind 4: table names, one per line (Explorer multi-selection) */
    /* interaction */
    int drag;                           /* 0 none, 1 pan, 2 move cards, 3 marquee, 4 minimap, 5 note resize, 6 note move */
    float mx0, my0, wx0, wy0, pan0x, pan0y;
    float *drag_xy; int ndrag;
    int drag_note;
    float mq_x0, mq_y0, mq_x1, mq_y1;   /* marquee in world units */
    int hover, hover_edge, hover_note;  /* -1 none */
    bool hover_more;                    /* the "+N" badge of the hovered card */
    int focus_node;                     /* keyboard focus (DGM-026), -1 none */
    int focus_row;                      /* -1 the card, else a row index inside it */
    bool isolate; int isolate_hops;     /* DGM-050 */
    bool *iso;                          /* nodes kept by the isolation */
    int lod;                            /* 0 boxes, 1 names, 2 full (with hysteresis) */
    /* find */
    TextField find; bool find_focus, find_list; int find_flags;
    DgHit *hits; int nhits, total, cur; char last_q[200]; int last_flags; unsigned last_cat;
    bool highlight_all;                 /* DGM-038 */
    char flash[DG_KEY]; Uint64 flash_t; /* the hit just navigated to */
    int flash_note;
    /* panes */
    bool props, minimap, legend;
    float props_scroll;
    /* history (DGM-028) */
    DgView hist[DG_HIST]; int nhist, ihist;
    /* camera animation (DGM-033) */
    bool anim; float az0, ax0, ay0, az1, ax1, ay1; Uint64 at0;
    /* menus and dialogs */
    PopupMenu cm, sub; int cm_kind, cm_node, cm_edge, cm_note; float cm_wx, cm_wy;
    char sub_labels[16][96];
    int dlg; TextField pf, pf2; char dlg_title[160], dlg_body[600]; int prompt_kind, prompt_node;
    int confirm_ti, confirm_hops, confirm_count;
    /* scope dialog */
    char scope_names[200][128]; bool scope_on[200]; int nscope; TextField scope_filter; float scope_scroll;
    /* schema picker (DGM-002) */
    int pick_mode; int *pick; int npick; bool *pick_on; TextField pick_filter; float pick_scroll; int pick_n;
    /* export dialog */
    int ex_fmt, ex_scope; bool ex_dark, ex_transparent, ex_legend, ex_title, ex_footer; int ex_scale;
    /* relation drawing (DGM-013 minimal: click source card, then target) */
    bool linking; int link_from, link_kind;
    /* layout on a worker (> 200 cards) */
    SDL_Thread *lay_th; volatile int lay_cancel; SDL_AtomicInt lay_done; Dgm *lay_copy; bool lay_ok;
    /* the assistant */
    AiJob *ai; int ai_kind; char *ai_payload; char ai_tables[4000]; char *ai_text; bool ai_show; char ai_summary[600];
    DgProposal props_ai[24]; int nprops_ai;
    /* saving */
    Uint64 dirty_at; bool save_failed;
    char msg[300]; Uint64 msg_t;
    /* metrics (harness) */
    int drawn_cards, drawn_edges, drawn_labels; double last_ms;
    double frame_ms[240]; int nframe;
    Rect canvas, body;
} DgDoc;

typedef struct DgGlobals {
    App *a;
    DgDoc *docs[DG_MAX_DOCS];
    DgCat cats[DG_MAX_CATS];
    int seq;
    /* fonts per zoom (render pixels -> font) */
    struct { int px; Font *f; Uint64 used; } fonts[12];
    char font_path[1200];
    bool reduce_motion;
    /* save / export dialogs */
    SDL_Mutex *dlg_mu; struct { bool pending, ready; int doc, action, fmt; char path[1100]; } fdlg;
} DgGlobals;
extern DgGlobals DG;

/* dgmview.c */
DgDoc *dg_active(App *a);
DgCat *dg_cat(DgDoc *d);
Connection *dg_conn(App *a, const char *id);
void dg_msg(DgDoc *d, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
void dg_touch(DgDoc *d);                 /* the file changed: autosave soon */
void dg_rebind(DgDoc *d);
void dg_refresh(App *a, DgDoc *d, bool full);
bool dg_save(App *a, DgDoc *d);
void dg_set_scope(App *a, DgDoc *d, const char *schemas);
void dg_layout(App *a, DgDoc *d, bool only_sel);
void dg_measure(App *a, DgDoc *d);
Font *dg_font(App *a, float pt);          /* the UI face at pt points x zoom-independent */
void dg_world_to_screen(const DgDoc *d, App *a, float wx, float wy, float *sx, float *sy);
void dg_screen_to_world(const DgDoc *d, App *a, float sx, float sy, float *wx, float *wy);
float dg_k(const DgDoc *d, App *a);       /* render pixels per world unit */
void dg_zoom_at(App *a, DgDoc *d, float factor, float sx, float sy);
void dg_fit(App *a, DgDoc *d, bool only_sel);
void dg_center_on(App *a, DgDoc *d, int node, bool animate);
void dg_push_history(DgDoc *d);
bool dg_back(App *a, DgDoc *d);
bool dg_forward(App *a, DgDoc *d);
void dg_expand(App *a, DgDoc *d, int ti, int hops);
int dg_add_hit(App *a, DgDoc *d, int ti);   /* adds one table next to its nearest neighbour; returns the node */
void dg_select_only(DgDoc *d, int node);
int dg_nsel(const DgDoc *d);
void dg_iso_update(DgDoc *d);
bool dg_conn_read_only(App *a, DgDoc *d);
void dg_footer(App *a, DgDoc *d, char *out, size_t n);
const char *dg_exf_ext(int fmt);

/* dgmview_canvas.c */
void dg_canvas(App *a, DgDoc *d, Rect r);
void dg_draw_scene(App *a, DgDoc *d, Rect r, bool export_mode);
bool dg_render_png(App *a, DgDoc *d, int scope, float scale, bool dark, bool transparent, const char *path, char *why, int wn, uint8_t **rgba_out, int *w_out, int *h_out);

/* dgmview_ui.c */
void dg_toolbar(App *a, DgDoc *d, Rect bar);
void dg_find_run(App *a, DgDoc *d);
void dg_find_step(App *a, DgDoc *d, int dir);
void dg_find_list(App *a, DgDoc *d);
void dg_props_pane(App *a, DgDoc *d, Rect r);
void dg_status(App *a, DgDoc *d, Rect r);
void dg_open_menu(App *a, DgDoc *d, int kind, int node, int edge, int note, float sx, float sy);
void dg_menus(App *a, DgDoc *d);
void dg_dialogs(App *a, DgDoc *d);
void dg_dialog_ok(App *a, DgDoc *d);
void dg_prompt(DgDoc *d, int kind, int node, const char *title, const char *initial);
void dg_open_scope(App *a, DgDoc *d);
void dg_open_picker(App *a, DgDoc *d, const char *schema);
void dg_open_export(App *a, DgDoc *d, int fmt);
bool dg_export(App *a, DgDoc *d, int fmt, int scope, const char *path, char *why, int wn);
void dg_ask_path(App *a, DgDoc *d, int action, int fmt);
void dg_poll_paths(App *a);
void dg_action(App *a, DgDoc *d, int act);
void dg_ai_start(App *a, DgDoc *d, int kind);
void dg_ai_poll(App *a, DgDoc *d);
void dg_ai_accept(App *a, DgDoc *d, int i, bool accept);

/* actions (menus, commands, toolbar, keys) */
enum {
    DA_NONE, DA_FIND, DA_LAYOUT, DA_LAYOUT_SEL, DA_FIT, DA_FIT_SEL, DA_ZOOM_IN, DA_ZOOM_OUT, DA_ZOOM_100,
    DA_MODE_NAME, DA_MODE_KEYS, DA_MODE_ALL, DA_CARD_NAME, DA_CARD_KEYS, DA_CARD_ALL, DA_CARD_GLOBAL,
    DA_REMOVE, DA_NEIGH1, DA_NEIGH2, DA_NEIGH3, DA_FOCUS_HERE, DA_PIN, DA_COLOR, DA_ALIAS, DA_COPY_NAME,
    DA_OPEN_DATA, DA_EDIT_TABLE, DA_SHOW_DDL, DA_REVEAL, DA_REFRESH_TABLE, DA_REFRESH, DA_SCOPE, DA_EXPORT,
    DA_SELECT_ALL, DA_UNDO, DA_REDO, DA_BACK, DA_FORWARD, DA_MINIMAP, DA_PROPS, DA_LEGEND, DA_ISOLATE, DA_RESET_FOCUS,
    DA_DISCONNECTED, DA_HIDE_EDGE, DA_SHOW_HIDDEN, DA_EDGE_DEF, DA_EDGE_FIT, DA_COPY_EDGE, DA_FOLLOW,
    DA_VIRTUAL, DA_TOGGLE_VIRTUAL, DA_NOTE, DA_AREA, DA_ENTITY, DA_ATTRS, DA_REL_LOGICAL, DA_EDIT_NOTE, DA_DELETE_NOTE,
    DA_NOTATION, DA_ROUTING, DA_TYPES, DA_COMMENTS, DA_NULLS, DA_DEFAULTS, DA_SNAP, DA_CONCEPTUAL,
    DA_ALIGN_L, DA_ALIGN_C, DA_ALIGN_R, DA_ALIGN_T, DA_ALIGN_M, DA_ALIGN_B, DA_DIST_H, DA_DIST_V, DA_GROUP_COMPONENTS,
    DA_DUPLICATE, DA_SAVE_SUBSET, DA_SNAPSHOT, DA_COMPARE, DA_COPY_IMAGE, DA_RENAME, DA_HIGHLIGHT_ALL,
    DA_AI_EXPLAIN, DA_AI_ORGANISE, DA_AI_CONCEPTUAL, DA_VERIFY_REL, DA_EVIDENCE, DA_DELETE_REL, DA_OPEN_DEFINITION,
    DA_COUNT
};
