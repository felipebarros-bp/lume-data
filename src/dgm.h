/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * ER diagram model (docs/diagrams.md): a subset of a connection's catalog drawn as cards and edges.
 * =================================================================================================
 *
 * The catalog is a SchIdx (schidx.h) the caller owns and keeps alive while the diagram uses it; the
 * diagram stores only keys ("schema.table"), positions and what the user adds (virtual / logical
 * relations, notes, aliases, logical entities). Edges are derived from the catalog's foreign keys for
 * the tables on the diagram, plus the user's own relations, each with its origin:
 *
 *   DE_FK         a constraint read from the database: solid line
 *   DE_VIRTUAL    drawn by the user (no constraint): dashed, labelled "virtual"
 *   DE_SUGGESTED  proposed (Lume Intelligence or the user's hypothesis): dotted, "suggested"
 *   DE_LOGICAL    a relationship of the logical / conceptual layer between entities
 *
 * Nothing here runs SQL or draws: pure C, deterministic (same input -> same layout -> same file), safe
 * on a worker thread for layout (dgm_layout works on a copy of positions). Tests drive it headless.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>
#include "schidx.h"

#define DG_KEY 300
enum { DN_TABLE, DN_STUB, DN_ENTITY };                 /* a catalog table, an external / unreadable reference, a logical entity */
enum { DM_GLOBAL = -1, DM_NAME, DM_KEYS, DM_ALL };     /* card modes (per card: DM_GLOBAL follows the diagram) */
enum { DE_FK, DE_VIRTUAL, DE_SUGGESTED, DE_LOGICAL };
enum { DC_UNKNOWN, DC_ONE, DC_MANY };                  /* an edge end's cardinality */
enum { DS_NONE, DS_MISSING, DS_ADDED, DS_CHANGED };    /* compared with the diagram's snapshot (DGM-016) */
enum { DNOT_CROW, DNOT_SIMPLE };                       /* notation */
enum { DRT_ORTHO, DRT_STRAIGHT };                      /* routing */

typedef struct DgNode {
    char key[DG_KEY];          /* "schema.table" as the catalog stores it ("" schema: "table"); entities: "entity:<name>" */
    int kind;                  /* DN_* */
    float x, y, w, h;          /* world units (h: computed by dgm_measure) */
    int mode;                  /* DM_* (DM_GLOBAL = the diagram's mode) */
    bool pinned;
    int color;                 /* 0 = by schema, 1..8 = a tag colour */
    char alias[80];            /* business name (DGM-051); "" none */
    char attrs[1200];          /* entities only: "name type\n..." (DGM-008) */
    int ti;                    /* catalog index, -1 when not in the catalog (resolved by dgm_bind) */
    int status;                /* DS_* after dgm_compare */
    int scroll;                /* first column shown (cards with more columns than fit) */
    int more;                  /* related tables in the catalog not on the diagram (computed) */
    char sig[20];              /* the table's structure digest when the diagram was saved (DGM-016) */
    bool sel;                  /* selected in the view (moves with the node when indexes change) */
} DgNode;

typedef struct DgRel {         /* a user relation: virtual, suggested or logical */
    int kind;                  /* DE_VIRTUAL / DE_SUGGESTED / DE_LOGICAL */
    char from[DG_KEY], to[DG_KEY];
    char cols[200], ref_cols[200];   /* comma lists; may be empty for entity relationships */
    char label[80], evidence[240], author[60];
    bool verified;
    int card_from, card_to;    /* DC_* (logical relationships: chosen by the user) */
    bool optional;
} DgRel;

typedef struct DgNote {
    float x, y, w, h;
    bool area;                 /* a coloured group area with a title, drawn below cards */
    int color;
    char title[80];
    char text[600];
} DgNote;

typedef struct DgEdge {        /* derived by dgm_edges */
    int from, to;              /* node indexes */
    int origin;                /* DE_* */
    int fk_t, fk_k;            /* DE_FK: catalog table and its fk index */
    int rel;                   /* user relations: index in d->rels */
    int card_from, card_to;    /* DC_* */
    bool optional;
    char name[128];            /* constraint name or relation label */
    char cols[200], ref_cols[200];
    bool hidden;               /* hidden by the user (by name), still counted */
    int parallel, nparallel;   /* among edges joining the same two nodes */
} DgEdge;

typedef struct DgHit { int ti; int node; char text[DG_KEY + 140]; int what; } DgHit;   /* find result: ti catalog table (-1 none; notes: -2 - note index), node on the diagram or -1 */
enum { DH_TABLE, DH_COLUMN, DH_CONSTRAINT, DH_NOTE };
enum { DF_TABLES = 1, DF_COLUMNS = 2, DF_CONSTRAINTS = 4, DF_NOTES = 8, DF_EXACT = 16, DF_PREFIX = 32, DF_GLOB = 64, DF_CASE = 128 };

#define DG_UNDO 60
typedef struct DgUndo { int n; char (*keys)[DG_KEY]; float *xy; } DgUndo;

typedef struct Dgm {
    int format;                /* 1 */
    char name[120];
    char conn_id[16], conn_name[80];
    char schemas[1200];        /* scope: comma list ("" = the connection's current schema) */
    char captured[32];         /* when the catalog snapshot was read (ISO) */
    char focus[DG_KEY];        /* the table it was opened on */
    int mode;                  /* DM_NAME / DM_KEYS / DM_ALL */
    int notation, routing;
    bool show_types, show_comments, show_nullability, show_defaults;
    bool conceptual;           /* conceptual view: aliases, entities and logical relations only (DGM-051) */
    bool show_virtual;         /* virtual / suggested edges drawn */
    bool snap;                 /* snap to grid (DGM-056) */
    float zoom, panx, pany;    /* viewport (world -> screen: (p - pan) * zoom) */
    DgNode *n; int nn, capn;
    DgRel *rels; int nrels, caprels;
    DgNote *notes; int nnotes, capnotes;
    char (*hidden)[160]; int nhidden;   /* hidden edges (constraint names / relation labels) */
    /* derived */
    DgEdge *e; int ne, cape;
    const SchIdx *x;           /* bound catalog (not owned) */
    int *rev, *rev_start;      /* reverse FK adjacency over the catalog: tables referencing each table */
    int rev_n;
    /* spatial grid over nodes (DGM-073) */
    int *cell_start, *cell_items; int gx0, gy0, gw, gh; bool grid_ok;
    /* undo / redo of layout operations */
    DgUndo undo[DG_UNDO]; int nundo, iundo;
    bool dirty;
} Dgm;

/* life */
void dgm_init(Dgm *d);
void dgm_free(Dgm *d);
/* binds the catalog (NULL: none) and resolves every node; recomputes edges */
void dgm_bind(Dgm *d, const SchIdx *x);

/* nodes */
int dgm_find_node(const Dgm *d, const char *key);
int dgm_add_table(Dgm *d, int ti, float x, float y);              /* existing node index when already there */
int dgm_add_entity(Dgm *d, const char *name, float x, float y);   /* logical entity (DGM-008) */
void dgm_remove(Dgm *d, int node);                                /* the card only, never the database */
int dgm_catalog_neighbours(const Dgm *d, int ti, int *out, int max);   /* tables linked by FK (both directions), catalog order, no duplicates */
/* adds the tables within hops of ti (BFS), never more than budget; returns how many would be added
 * when dry (count only), else how many were added. */
int dgm_expand(Dgm *d, int ti, int hops, int budget, bool dry);
/* the focused open: ti plus its 1-hop neighbours (capped), laid out */
void dgm_open_focus(Dgm *d, int ti, int cap);
/* the most connected tables of the catalog (degree), for the schema picker */
int dgm_most_connected(const SchIdx *x, const char *schema, int *out, int max);

/* user relations */
int dgm_add_rel(Dgm *d, int kind, const char *from, const char *to, const char *cols, const char *ref_cols, const char *label);
void dgm_remove_rel(Dgm *d, int r);
bool dgm_edge_hidden(const Dgm *d, const char *name);
void dgm_toggle_hidden(Dgm *d, const char *name);
int dgm_add_note(Dgm *d, bool area, float x, float y, float w, float h, const char *title, const char *text);

/* derived data */
/* sizes a card: text_w measures a string in world units (NULL: 7 units per character) */
void dgm_measure(Dgm *d, int node, float (*text_w)(void *ud, const char *s), void *ud);
void dgm_measure_all(Dgm *d, float (*text_w)(void *ud, const char *s), void *ud);
int dgm_card_mode(const Dgm *d, int node);
/* the rows a card shows (catalog column indexes; entities: attribute line numbers), in order */
int dgm_card_rows(const Dgm *d, int node, int *rows, int max);
#define DG_HEAD_H 28.0f
#define DG_ROW_H 18.0f
#define DG_MAX_ROWS 30          /* more scroll inside the card */
void dgm_edges(Dgm *d);                  /* recompute edges, cardinalities, parallel indexes and "more" counts */
/* an edge's polyline in world units (pts: x0,y0,x1,y1,...; at most 8 points); the end points touch the
 * cards, at the FK / key rows when both are shown. Shared by the view and the SVG export. */
int dgm_route(const Dgm *d, int e, float *pts);
/* the world y of a column row's centre on a card (-1 when the row is not shown) */
float dgm_row_y(const Dgm *d, int node, const char *col);
const char *dgm_card_label(const Dgm *d, int node);   /* alias in the conceptual view, else the table name */

/* layout (DGM-052 / 054 / 055): layered around the focus (hop distance, FK direction); pinned and
 * (when only_sel) unselected nodes stay. Deterministic. cancel may be NULL. Returns false when cancelled
 * (positions untouched). */
bool dgm_layout(Dgm *d, bool only_sel, volatile int *cancel);
bool dgm_bounds(const Dgm *d, bool only_sel, float *x0, float *y0, float *x1, float *y1);   /* false: nothing */
void dgm_align(Dgm *d, int how);    /* 0 left 1 centre 2 right 3 top 4 middle 5 bottom 6 distribute h 7 distribute v */
int dgm_components(const Dgm *d, int *comp);         /* connected components over the drawn edges; returns the count */

/* spatial grid (DGM-073) */
void dgm_grid_build(Dgm *d);
int dgm_grid_query(const Dgm *d, float x0, float y0, float x1, float y1, int *out, int max);
int dgm_hit(const Dgm *d, float wx, float wy);        /* topmost card under a world point, -1 */

/* find (DGM-030..040): tables of the catalog (on the diagram or not), columns, constraints, notes.
 * Order: hits on the diagram first, by position (rows of 200 units, then x), then the rest by name. */
int dgm_find(const Dgm *d, const char *text, int flags, DgHit *out, int max);

/* undo / redo of positions */
void dgm_snapshot(Dgm *d);    /* call before a layout operation */
bool dgm_undo(Dgm *d);
bool dgm_redo(Dgm *d);

/* comparison with the catalog the diagram was saved against (DGM-016): marks nodes */
int dgm_compare(Dgm *d);     /* returns how many nodes differ (missing / changed) */
void dgm_resign(Dgm *d);     /* the current structure becomes the reference (on save after a refresh) */
void dgm_table_sig(const SchIdx *x, int ti, char *out, size_t n);   /* columns + keys digest of one table */

/* file (DGM-058): deterministic JSON. why gets the reason on failure. */
char *dgm_to_json(const Dgm *d);                     /* malloc'd */
bool dgm_from_json(Dgm *d, const char *text, size_t len, char *why, size_t wn);
bool dgm_save(const Dgm *d, const char *path, char *why, size_t wn);
bool dgm_load(Dgm *d, const char *path, char *why, size_t wn);

/* exports (DGM-062 / 064 / 069): text formats built from the model; geometry from the caller's layout */
char *dgm_mermaid(const Dgm *d, int *lost);           /* lost: what Mermaid cannot carry (count) */
char *dgm_graphml(const Dgm *d);
char *dgm_drawio(const Dgm *d);
/* SVG of the selected nodes (only_sel) or all: cards, edges, notes; every label escaped, no script */
typedef struct DgSvgOpt { bool dark, transparent, legend, title; float scale; const char *title_text, *footer; } DgSvgOpt;
char *dgm_svg(const Dgm *d, bool only_sel, const DgSvgOpt *o);
/* one self-contained HTML document: the SVG plus search, pan and zoom; no external requests */
char *dgm_html(const Dgm *d, const DgSvgOpt *o);

/* small helpers shared with the view */
void dgm_key_of(const SchIdx *x, int ti, char *out, size_t n);
bool dgm_col_in(const char *list, const char *col);   /* col in a comma list (case-insensitive) */
