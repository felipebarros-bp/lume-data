/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Docking engine (LUME-PANEL-001)
 * ===============================
 *
 * Pure model of the workspace layout: which panels exist, where each one lives, how much
 * room it takes, and every operation that changes that (show, hide, collapse, move, split,
 * resize, maximize, focus mode, presets, undo, persistence). It knows nothing about SDL,
 * fonts or the database, so it is tested headless (tests/dock_check.c, NFR-09) and the shell
 * (main.c / panels.c) only asks it for rectangles and forwards user intents to it.
 *
 * Vocabulary (spec section 2)
 *   Panel      a tool window: Explorer, Results, Output, Inspector... registered once with a
 *              stable id ("lume.data.results"); its content lives in its module, never here.
 *   Group      tabs of one or more panels inside a region, with one active tab.
 *   Region     left, right, bottom or top edge of the workspace; holds groups split along the
 *              region's axis (stacked in side regions, side by side at the bottom / top).
 *   Center     the editor area: one or more editor groups (split right or down), each with
 *              editor tabs. The center always exists (principle 1).
 *
 * States of a panel
 *   docked     in a group, taking room (unless its group is collapsed)
 *   collapsed  its group is folded to a strip on the region edge; content kept, size kept
 *   hidden     not on screen; reachable from View > Panels, the palette and (if pinned) the bar
 *   autohide   not taking room; opening it shows an overlay anchored to its region edge
 *   floating   in a native utility window kept above the main one (V2, PNL-019)
 *   detached   in an independent native window, e.g. on another monitor (V2, PNL-020);
 *              the shell owns both kinds of window; closing one docks the panel back (PNL-021)
 *
 * Every mutating operation is a small transaction: it checks the request against the current
 * state, builds the new state, and returns false with a reason (dock->err) without touching
 * anything when the result would be invalid (editor below its minimum, unknown panel, ...).
 * Each successful operation pushes the previous state on the undo ring (PNL-045) and marks
 * the layout dirty so the shell saves it, coalesced (PNL-030).
 *
 * Indices and units
 *   Panels are addressed by their index in the registry (p, 0..ndesc-1), groups by their slot
 *   in DockState.group[] (g), editor tabs by the shell's editor ids. Sizes and rectangles are
 *   logical points; the shell multiplies by the display scale. Single-threaded (UI thread).
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

/* capacity limits; the engine refuses operations that would exceed them */
#define DOCK_MAX_PANELS 32
#define DOCK_MAX_GROUPS 24
#define DOCK_MAX_LANES 4          /* PNL-011: side-by-side lanes of groups inside one region */
#define DOCK_MAX_SPLITTERS (DOCK_MAX_GROUPS + DOCK_REGIONS * (1 + DOCK_MAX_LANES) + DOCK_MAX_EDITOR_GROUPS)   /* regions, lanes, groups, editors */
#define DOCK_MAX_TABS 16          /* panels per group */
#define DOCK_MAX_EDITOR_GROUPS 4
#define DOCK_MAX_EDITOR_TABS 24
#define DOCK_UNDO_DEPTH 20        /* layout snapshots kept for dock_undo() */
#define DOCK_SCHEMA_VERSION 1     /* "schemaVersion" of the JSON layout file */

/* Regions (DOCK_CENTER is the editor area, used as DockSplitter.region for editor splitters). */
enum { DOCK_LEFT, DOCK_RIGHT, DOCK_BOTTOM, DOCK_TOP, DOCK_REGIONS, DOCK_CENTER = DOCK_REGIONS };
/* Panel modes (DockPanelState.mode); see "States of a panel" above. */
enum { DOCK_DOCKED, DOCK_HIDDEN, DOCK_AUTOHIDE, DOCK_FLOATING, DOCK_DETACHED, DOCK_MODES };

/* capability flags of a panel type (PNL-002) */
enum {
    DOCK_CAP_HIDE = 1u << 0, DOCK_CAP_MOVE = 1u << 1, DOCK_CAP_COLLAPSE = 1u << 2,
    DOCK_CAP_MAXIMIZE = 1u << 3, DOCK_CAP_FLOAT = 1u << 4, DOCK_CAP_AUTOHIDE = 1u << 5,
    DOCK_CAP_ALL = 0x3F,
};

/* Rectangle in logical points (no SDL dependency). */
typedef struct DockRect { float x, y, w, h; } DockRect;

/* What a tab or a bar button shows next to the title (PNL-037): independent channels, so
 * loading / warning / error / unsaved never depend on colour alone. */
/* count: number shown in the badge (0 = none); loading / warning / error: state flags;
 * unsaved: number of unsaved items (0 = none). */
typedef struct DockBadge { int count; bool loading, warning, error; int unsaved; } DockBadge;

/* One entry a panel adds to its own context menu (PNL-047). The id is the panel's own, passed
 * back to menu_run; the label is copied by the shell. */
typedef struct DockMenuItem { int id; char label[64]; bool enabled; bool sep_before; } DockMenuItem;
#define DOCK_MAX_MENU_ITEMS 12

/* Registration of a panel type (PNL-002 / PNL-047). Strings must outlive the registration.
 * draw / badge / context are opaque to the engine; the shell calls them. */
typedef struct DockPanelDesc {
    const char *id;            /* stable, unique: "lume.data.results" (R-14) */
    const char *title;         /* may change; never used for persistence */
    const char *icon;          /* icon name for the bar ("db", "grid", "log", ...) */
    const char *shortcut;      /* shown in tooltips and the palette, e.g. "Cmd+1" */
    unsigned caps;             /* DOCK_CAP_* */
    int region;                /* preferred region for a first placement */
    float min_w, min_h;        /* minimum content size, logical points */
    float pref_size;           /* preferred region size (width for side regions, height else) */
    int priority;              /* small = keep longer when the window is too small (PNL-038) */
    bool start_hidden;         /* e.g. the Inspector */
    void (*draw)(void *ud, void *ctx, DockRect rc, bool focused);   /* draws the content in rc; ctx is the shell's (App) */
    void (*badge)(void *ud, void *ctx, DockBadge *out);             /* fills the badge; may be NULL */
    void *ud;                  /* user data passed back to draw / badge */
    /* Extension hooks (PNL-047), all optional; the engine stores them and the shell calls them.
     * "view" below is what create returned, or ud when there is no create. */
    void *(*create)(void *ud, void *ctx);                    /* view factory: once, before the first draw or badge */
    void (*destroy)(void *view, void *ctx);                  /* after create, when the panel is unregistered or the app quits */
    void (*visibility)(void *view, void *ctx, bool shown);   /* the content started / stopped being drawn (hidden, collapsed, in a closed window) */
    int (*menu)(void *view, void *ctx, DockMenuItem *items, int max);   /* fills extra context-menu items; returns how many */
    void (*menu_run)(void *view, void *ctx, int item_id);    /* one of those items was chosen */
} DockPanelDesc;

/* --- serialisable state (no pointers: copied by value for undo, focus mode, presets) --- */
/* Where one panel is and how it is shown. Indexed like the registry. */
typedef struct DockPanelState {
    char id[48];               /* copy of the registered id: persistence key */
    int mode;                  /* DOCK_DOCKED / HIDDEN / AUTOHIDE / FLOATING */
    int group;                 /* group index while docked; the group to return to otherwise */
    int home_region;           /* region to return to when its group no longer exists */
    bool in_bar;               /* button on the panel bar (PNL-006) */
    int bar_order;             /* position of its button on the bar, 0 = first */
    float float_x, float_y, float_w, float_h;   /* floating / detached window: screen position and size, points */
} DockPanelState;

/* A tab group inside a region. Slots with used == false are free. */
typedef struct DockGroup {
    bool used;                 /* slot in use */
    int region;                /* DOCK_LEFT..DOCK_TOP */
    int tabs[DOCK_MAX_TABS]; int ntabs;   /* panel indices, in tab order */
    int active;                /* panel index of the active tab */
    float weight;              /* share of its lane along the region's axis */
    bool collapsed;            /* folded to the region's strip (PNL-004) */
    int lane;                  /* PNL-011: lane across the region's axis (0 = first); a region with
                                * groups in two lanes is split both ways (stacked + side by side) */
} DockGroup;

typedef struct DockEditorGroup {
    int tabs[DOCK_MAX_EDITOR_TABS]; int ntabs;   /* editor instance ids owned by the shell */
    int active;                /* editor id of the active tab */
    float weight;              /* share of the center along the split axis */
} DockEditorGroup;

/* The whole arrangement: what is saved, undone, and swapped by presets and named layouts. */
typedef struct DockState {
    int schema;                           /* DOCK_SCHEMA_VERSION */
    DockPanelState panel[DOCK_MAX_PANELS]; int npanels;   /* registry order */
    DockGroup group[DOCK_MAX_GROUPS];     /* group slots, see DockGroup.used */
    float region_size[DOCK_REGIONS];      /* last size of each region, logical points */
    int group_order[DOCK_REGIONS][DOCK_MAX_GROUPS]; int ngroups_in[DOCK_REGIONS];   /* group slots of each region, top-to-bottom / left-to-right */
    float lane_weight[DOCK_REGIONS][DOCK_MAX_LANES];   /* PNL-011: share of each lane across the region (lanes are kept contiguous: no empty lane) */
    DockEditorGroup editor[DOCK_MAX_EDITOR_GROUPS]; int neditors;   /* center splits; editor tabs are not written to the layout file */
    bool editor_split_down;               /* editor groups side by side (false) or stacked */
    int active_editor_group;              /* index into editor[] */
    bool focus_mode;                      /* PNL-035 */
    bool top_enabled;                     /* docking on the top edge (spec 14: off by default) */
    bool compact;                         /* PNL-038 compact headers */
    char focused_panel[48];               /* id of the panel with keyboard focus, "" = editor */
} DockState;

/* The engine instance: registry, current state, transient view state and undo ring.
 * Plain value type, no heap; initialise with dock_init(). */
typedef struct Dock {
    DockPanelDesc desc[DOCK_MAX_PANELS]; int ndesc;   /* registered panel types */
    DockState st;                         /* current arrangement */
    /* transient, never persisted */
    int maximized_group;                  /* PNL-014: -1 none */
    int overlay_panel;                    /* PNL-017 / 018 / 035: -1 none */
    bool auto_collapsed[DOCK_REGIONS];    /* PNL-038: folded because the window is too small */
    DockState undo[DOCK_UNDO_DEPTH]; int nundo, undo_head;   /* ring buffer: undo_head = newest, nundo valid entries */
    bool dirty;                           /* changed since the last save */
    unsigned long long change_seq;        /* increments on every successful change */
    float last_w, last_h;                 /* workspace size used by the last layout, points */
    char err[160];                        /* reason of the last refused operation */
} Dock;

/* --- setup and registry --- */
/* Resets d to an empty engine (no panels, default sizes, one editor group). */
void dock_init(Dock *d);
/* Registers a panel type; false (dock->err) on a duplicate or empty id, or when full. */
bool dock_register(Dock *d, const DockPanelDesc *desc);
/* Removes a panel type (module disabled, PNL-047): its tab leaves every group, nothing dangles. */
bool dock_unregister(Dock *d, const char *id);
int dock_find(const Dock *d, const char *id);            /* panel index or -1 */
/* Places every registered panel the state does not know yet at its preferred region. */
void dock_adopt_new_panels(Dock *d);

/* --- queries --- */
bool dock_is_visible(const Dock *d, int p);              /* docked, group expanded, region shown */
bool dock_is_collapsed(const Dock *d, int p);            /* docked in a collapsed group */
int dock_region_of(const Dock *d, int p);                /* region of its group / home */
int dock_group_of(const Dock *d, int p);                 /* -1 when not in a group */

/* --- operations (true = applied; false = refused, state unchanged, reason in d->err) --- */
bool dock_show(Dock *d, int p);          /* PNL-003: back to its group (or home region), active */
bool dock_hide(Dock *d, int p);          /* PNL-003 */
bool dock_toggle(Dock *d, int p);        /* show when not visible, else hide */
bool dock_collapse_group(Dock *d, int g);    /* PNL-004 */
bool dock_expand_group(Dock *d, int g);      /* undo of collapse */
bool dock_activate(Dock *d, int p);      /* PNL-005: active tab of its group (no focus change) */
bool dock_move_to_region(Dock *d, int p, int region);       /* PNL-008: new group at that edge */
bool dock_move_to_group(Dock *d, int p, int g, int index);  /* PNL-009 / 010: join a group as a tab */
bool dock_split_group(Dock *d, int p, int g, bool after);   /* PNL-011: new group beside g */
/* PNL-011: moves p into a new lane next to group g's lane (before / after it), i.e. a split
 * across the region's axis: side by side in the left / right regions, stacked in bottom / top.
 * The region grows to fit both lanes' minimum. Refused when p is already alone there. */
bool dock_split_lane(Dock *d, int p, int g, bool after);
/* Number of lanes in region r of s (0 when the region has no group). */
int dock_lanes(const DockState *s, int r);
bool dock_reorder_tab(Dock *d, int g, int from, int to);    /* PNL-010 */
bool dock_set_mode(Dock *d, int p, int mode);               /* PNL-016 / 017 / 019 */
bool dock_set_region_size(Dock *d, int region, float size); /* PNL-013 / 015, clamped */
bool dock_set_group_weight(Dock *d, int region, int k, float delta_points); /* splitter inside a region */
/* Like dock_set_group_weight, for the splitter between group_order entries k1 and k2 of the same lane. */
bool dock_set_group_pair_weight(Dock *d, int region, int k1, int k2, float delta_points);
/* PNL-011: drag of the splitter between lanes lane_a and lane_b (the next open one) of a region, in points
 * across the region's axis; false when either lane would go below its minimum. No undo step. */
bool dock_set_lane_weight(Dock *d, int region, int lane_a, int lane_b, float delta_points);
bool dock_reset_region_size(Dock *d, int region);           /* double-click on a splitter */
bool dock_toggle_maximize(Dock *d, int g);                  /* PNL-014 (not persisted) */
bool dock_toggle_focus_mode(Dock *d);                       /* PNL-035 */
bool dock_set_in_bar(Dock *d, int p, bool on);              /* PNL-006 */
bool dock_move_bar_button(Dock *d, int p, int new_order);   /* reorder the bar by drag */
bool dock_open_overlay(Dock *d, int p);                     /* autohide panel / panel in focus mode */
void dock_close_overlay(Dock *d);                           /* no-op when none is open */
bool dock_undo(Dock *d);                                    /* PNL-045 */
/* Pushes one undo snapshot for a continuous gesture (splitter drag): call it on press, then
 * dock_set_region_size / dock_set_group_weight on every move (they push nothing). */
void dock_begin_gesture(Dock *d);

/* editor groups (PNL-027 / 028): tabs are the shell's editor ids; these changes are not
 * undoable and editor tabs are not persisted by the engine */
int dock_editor_add_tab(Dock *d, int egroup, int editor_id);   /* returns the group used */
bool dock_editor_close_tab(Dock *d, int editor_id);            /* refuses to drop the last editor */
bool dock_editor_activate(Dock *d, int editor_id);             /* also makes its group active */
bool dock_editor_split(Dock *d, int editor_id, bool down);     /* move the tab to a new group */
bool dock_editor_move_tab(Dock *d, int editor_id, int egroup, int index);   /* index -1 = last */
int dock_editor_group_of(const Dock *d, int editor_id);       /* editor group index or -1 */

/* --- presets and named layouts (PNL-031 / 033 / 034) --- */
enum { DOCK_PRESET_DEFAULT, DOCK_PRESET_QUERY_FOCUS, DOCK_PRESET_DATA_EXPLORATION, DOCK_PRESETS };
const char *dock_preset_name(int preset);    /* display name, static string */
/* Applies a preset to the panels only: editor tabs and every panel's content are untouched. */
bool dock_apply_preset(Dock *d, int preset);
/* Applies a saved arrangement (panels and sizes) while keeping the current editor groups. */
bool dock_apply_state(Dock *d, const DockState *s);

/* --- persistence (PNL-030 / 032 / 046) --- */
/* JSON text of the state (schema version, ids, modes, groups, sizes; never data). malloc'd. */
char *dock_serialize(const Dock *d, const DockState *s, const char *layout_name);
/* Parses and validates; on any problem returns false with the reason and leaves *out alone.
 * Unknown panel ids are dropped (R-12), registered panels missing from the text are placed
 * at their preferred region. */
bool dock_parse(const Dock *d, const char *text, size_t len, DockState *out, char *name, size_t name_n, char *err, size_t errn);
/* Structural check of a state against the registry (invariants of spec 8.4). */
bool dock_validate(const Dock *d, const DockState *s, char *err, size_t errn);
/* Atomic write (temp file + rename). */
bool dock_save_file(const Dock *d, const char *path, char *err, size_t errn);
/* Loads a layout file; when it is invalid the file is renamed to <path>.bad-<time> for
 * diagnosis, the default layout is used, and false is returned with the reason (PNL-032). */
bool dock_load_file(Dock *d, const char *path, char *err, size_t errn);

/* --- geometry --- */
/* A draggable divider produced by dock_layout(). */
typedef struct DockSplitter {
    DockRect r;
    int kind;                  /* 0 region edge, 1 between groups of a lane, 2 between editor groups, 3 between lanes */
    int region, index;         /* region; kind 1: group_order index of the first group; kind 3: the first lane */
    int index2;                /* kind 1: group_order index of the second group (another lane may sit between) */
    bool vertical;             /* true: drag left / right */
} DockSplitter;

/* Every rectangle of one frame (points), output of dock_layout(); rebuilt each frame. */
typedef struct DockLayout {
    DockRect rail;             /* panel bar (left edge); w = 0 when hidden (focus mode) */
    DockRect center;           /* editor area */
    DockRect editor[DOCK_MAX_EDITOR_GROUPS]; int neditors;
    DockRect region[DOCK_REGIONS];             /* w or h 0 when the region shows nothing */
    DockRect strip[DOCK_REGIONS];              /* collapsed strip of a region (0 = none) */
    DockRect group[DOCK_MAX_GROUPS];           /* full rect of a visible group, header included */
    bool group_shown[DOCK_MAX_GROUPS];         /* group[g] is valid and on screen */
    DockSplitter split[DOCK_MAX_SPLITTERS]; int nsplit;
    DockRect overlay; int overlay_panel;       /* -1 when no overlay */
    bool editor_hidden;                        /* a group is maximized */
} DockLayout;

/* Chrome sizes used by the layout. */
typedef struct DockMetrics {
    float rail_w, strip_h, header_h, splitter;   /* points: bar width, collapsed strip, group header, splitter thickness */
    float editor_min_w, editor_min_h;            /* the editor area never gets smaller than this, points */
} DockMetrics;
/* Default metrics (header height follows the compact setting of d). */
DockMetrics dock_default_metrics(const Dock *d);
/* Computes every rectangle for a workspace of w x h points at (x, y). Folds regions that do
 * not fit (PNL-038) in priority order without changing their saved sizes. */
void dock_layout(Dock *d, float x, float y, float w, float h, const DockMetrics *m, DockLayout *out);

/* --- drag and drop targets (PNL-007 / 009 / 011 / 012) --- */
enum { DOCK_DROP_NONE, DOCK_DROP_REGION, DOCK_DROP_TAB, DOCK_DROP_SPLIT };
/* Result of a drop hit test. */
typedef struct DockDrop {
    int kind;                  /* DOCK_DROP_* */
    int region;                /* DROP_REGION: new group at this edge */
    int group, index;          /* DROP_TAB: join group g at tab index; DROP_SPLIT: beside g */
    bool after;                /* DROP_SPLIT: below / right of g */
    bool cross;                /* DROP_SPLIT across the region's axis: a new lane beside g (PNL-011) */
    DockRect preview;          /* where the panel would land (approximate final size) */
} DockDrop;
/* Target under (mx, my) for dragging panel p; DOCK_DROP_NONE when there is no valid target
 * there (invalid targets are never offered). */
DockDrop dock_drop_target(Dock *d, const DockLayout *lay, const DockMetrics *m, int p, float mx, float my);
/* Moves panel p to target t (a transaction); false with d->err when refused or t is NONE. */
bool dock_apply_drop(Dock *d, int p, const DockDrop *t);
