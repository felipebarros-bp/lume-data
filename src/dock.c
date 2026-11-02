/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Docking engine: see dock.h for the model and the vocabulary. This file has no SDL, font or
 * database dependency; tests/dock_check.c drives it headless.
 *
 * Who uses it: panels.c owns the single Dock of the main window. It registers the panel
 * types at startup, calls dock_layout() every frame to get rectangles, forwards clicks,
 * drags and commands to the operations below, and saves the layout (dock_save_file) when
 * d->dirty is set. change_seq lets the shell notice any change, transient ones included.
 *
 * Data: everything that is persisted or undoable is in DockState, a plain struct with no
 * pointers, so snapshots are struct copies (undo ring, focus mode, presets, named layouts).
 * Panels are referred to by index into d->desc / d->st.panel (registry order); groups by
 * index into st.group[] (slots with used == false are free); the order of groups inside a
 * region is st.group_order[region][0..ngroups_in-1]. Editor tabs are opaque ids owned by
 * the shell. Transient view state (maximized group, overlay, auto-collapsed regions) lives
 * in Dock itself and is never saved.
 *
 * Transactions: an operation copies d->st, edits the copy with the state helpers, and hands
 * it to commit(), which runs dock_validate() and either publishes it (pushing the old state
 * on the undo ring) or refuses it with the reason in d->err. A few cheap or continuous
 * changes (active tab, splitter drags, editor tabs, overlay, maximize) write d->st directly
 * without an undo step.
 *
 * Units: sizes are logical points (UI points before HiDPI scaling); the shell converts.
 * Threading: single-threaded, UI thread only. No allocation except dock_serialize()'s
 * returned string and dock_load_file()'s temporary buffer.
 *
 * Sections: state helpers, transactions, setup and registry, queries, operations, editor
 * groups, presets, validation, persistence, geometry, drag and drop.
 */
#include "dock.h"
#include "json.h"
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <unistd.h>

/* Names used in the JSON layout file and in error messages; indexed by DOCK_LEFT.. / DOCK_DOCKED.. */
static const char *const region_names[DOCK_REGIONS] = { "left", "right", "bottom", "top" };
static const char *const mode_names[DOCK_MODES] = { "docked", "hidden", "autohide", "floating", "detached" };

/* default sizes of each region, logical points (spec 4.1) */
static const float default_region_size[DOCK_REGIONS] = { 250, 300, 260, 200 };
#define REGION_MIN_SIZE 80.0f      /* floor for any region size, points */
#define REGION_MAX_SIZE 5000.0f    /* sanity ceiling, also enforced by dock_validate() */

/* True for the left and right regions (vertical axis: groups stacked top to bottom). */
static bool side_region(int r) { return r == DOCK_LEFT || r == DOCK_RIGHT; }
/* forward declaration: dock_set_group_weight() needs the minimum length of a group */
struct DockMetrics;
static float group_min_len(const Dock *d, int g, const DockMetrics *m);

/* ======================================================================== */
/* state helpers                                                             */
/* ======================================================================== */

/* Claims a free group slot in s, puts it in `region` at position `pos` of the region's
 * order (pos < 0 or past the end = append) and returns its index, or -1 when every slot is
 * used. The new group is empty (no tabs, active -1, weight 1); callers attach a panel. */
static int new_group(DockState *s, int region, int pos) {
    for (int g = 0; g < DOCK_MAX_GROUPS; g++) {
        if (s->group[g].used) continue;
        memset(&s->group[g], 0, sizeof s->group[g]);
        s->group[g].used = true; s->group[g].region = region; s->group[g].weight = 1; s->group[g].active = -1;
        int n = s->ngroups_in[region];
        if (pos < 0 || pos > n) pos = n;
        memmove(&s->group_order[region][pos + 1], &s->group_order[region][pos], sizeof(int) * (size_t)(n - pos));
        s->group_order[region][pos] = g;
        s->ngroups_in[region]++;
        return g;
    }
    return -1;
}

/* Position of group g in its region's group_order, or -1 if it is not listed. */
static int order_pos(const DockState *s, int g) {
    int r = s->group[g].region;
    for (int k = 0; k < s->ngroups_in[r]; k++) if (s->group_order[r][k] == g) return k;
    return -1;
}

/* Frees group slot g and removes it from its region's order. Does not touch its panels. */
static void drop_group(DockState *s, int g) {
    int r = s->group[g].region, k = order_pos(s, g);
    if (k >= 0) { memmove(&s->group_order[r][k], &s->group_order[r][k + 1], sizeof(int) * (size_t)(s->ngroups_in[r] - k - 1)); s->ngroups_in[r]--; }
    s->group[g].used = false;
}

/* Takes panel p out of its group (if any); an emptied group disappears (PNL-009). The panel
 * keeps the group index and its region as the place to return to. */
static void detach(DockState *s, int p) {
    DockPanelState *ps = &s->panel[p];
    int g = ps->group;
    if (g < 0 || g >= DOCK_MAX_GROUPS || !s->group[g].used) return;
    DockGroup *gr = &s->group[g];
    int k = -1;
    for (int i = 0; i < gr->ntabs; i++) if (gr->tabs[i] == p) k = i;
    if (k < 0) return;
    memmove(&gr->tabs[k], &gr->tabs[k + 1], sizeof(int) * (size_t)(gr->ntabs - k - 1));
    gr->ntabs--;
    ps->home_region = gr->region;
    if (gr->active == p) gr->active = gr->ntabs ? gr->tabs[k < gr->ntabs ? k : gr->ntabs - 1] : -1;
    if (!gr->ntabs) drop_group(s, g);
}

/* Inserts panel p as a tab of group g at `index` (out of range = last), makes it the active
 * tab and marks it docked there. p must not be in another group (call detach() first). */
static void attach(DockState *s, int p, int g, int index) {
    DockGroup *gr = &s->group[g];
    if (index < 0 || index > gr->ntabs) index = gr->ntabs;
    memmove(&gr->tabs[index + 1], &gr->tabs[index], sizeof(int) * (size_t)(gr->ntabs - index));
    gr->tabs[index] = p; gr->ntabs++;
    gr->active = p;
    s->panel[p].group = g; s->panel[p].mode = DOCK_DOCKED; s->panel[p].home_region = gr->region;
}

/* True when g is a used group and panel p is one of its tabs. */
static bool in_group(const DockState *s, int p, int g) {
    if (g < 0 || g >= DOCK_MAX_GROUPS || !s->group[g].used) return false;
    for (int i = 0; i < s->group[g].ntabs; i++) if (s->group[g].tabs[i] == p) return true;
    return false;
}

/* ---- lanes (PNL-011) ------------------------------------------------------ */

/* See dock.h: lanes in use in region r (groups keep them contiguous from 0). */
int dock_lanes(const DockState *s, int r) {
    int n = 0;
    for (int k = 0; k < s->ngroups_in[r]; k++) { int l = s->group[s->group_order[r][k]].lane; if (l + 1 > n) n = l + 1; }
    return n;
}

/* Keeps every region's lanes contiguous: a lane left without groups (its last tab moved or
 * hidden) disappears and the lanes after it move down with their weights. Runs on every
 * committed state, so all the ways of emptying a lane normalise the tree the same way. */
static void normalize_lanes(DockState *s) {
    for (int r = 0; r < DOCK_REGIONS; r++) {
        bool used[DOCK_MAX_LANES] = { 0 };
        for (int k = 0; k < s->ngroups_in[r]; k++) {
            DockGroup *gr = &s->group[s->group_order[r][k]];
            if (gr->lane < 0 || gr->lane >= DOCK_MAX_LANES) gr->lane = 0;
            used[gr->lane] = true;
        }
        int map[DOCK_MAX_LANES], n = 0; float w[DOCK_MAX_LANES];
        for (int l = 0; l < DOCK_MAX_LANES; l++) if (used[l]) { map[l] = n; float v = s->lane_weight[r][l]; w[n++] = isfinite(v) && v > 0 ? v : 1; }
        for (int k = 0; k < s->ngroups_in[r]; k++) { DockGroup *gr = &s->group[s->group_order[r][k]]; gr->lane = map[gr->lane]; }
        for (int l = 0; l < DOCK_MAX_LANES; l++) s->lane_weight[r][l] = l < n ? w[l] : 1;
    }
}

/* ======================================================================== */
/* transactions                                                              */
/* ======================================================================== */

/* Records the reason of a refused operation in d->err (shown to the user by the shell). */
static void fail(Dock *d, const char *msg) { snprintf(d->err, sizeof d->err, "%s", msg); }

/* Publishes `next` as the new state after checking it; the previous one goes on the undo
 * ring. A state that does not validate is refused and the current one stays. */
static bool commit(Dock *d, const DockState *in) {
    char why[160];
    DockState norm = *in;
    normalize_lanes(&norm);   /* PNL-011: an emptied lane goes away */
    const DockState *next = &norm;
    if (!dock_validate(d, next, why, sizeof why)) { fail(d, why); return false; }
    d->undo_head = (d->undo_head + 1) % DOCK_UNDO_DEPTH;
    d->undo[d->undo_head] = d->st;
    if (d->nundo < DOCK_UNDO_DEPTH) d->nundo++;
    d->st = *next;
    d->dirty = true; d->change_seq++; d->err[0] = 0;
    if (d->maximized_group >= 0 && (!d->st.group[d->maximized_group].used || d->st.group[d->maximized_group].collapsed)) d->maximized_group = -1;
    if (d->overlay_panel >= 0 && d->st.panel[d->overlay_panel].mode == DOCK_DOCKED && !d->st.focus_mode) d->overlay_panel = -1;
    return true;
}

/* Checks that p is a registered panel index; sets d->err otherwise. */
static bool valid_panel(Dock *d, int p) {
    if (p < 0 || p >= d->ndesc || p >= d->st.npanels) { fail(d, "unknown panel"); return false; }
    return true;
}

/* ======================================================================== */
/* setup and registry                                                        */
/* ======================================================================== */

/* Empty engine: no panels, default region sizes, one empty editor group, no undo history. */
void dock_init(Dock *d) {
    memset(d, 0, sizeof *d);
    d->st.schema = DOCK_SCHEMA_VERSION;
    for (int r = 0; r < DOCK_REGIONS; r++) { d->st.region_size[r] = default_region_size[r]; for (int l = 0; l < DOCK_MAX_LANES; l++) d->st.lane_weight[r][l] = 1; }
    d->st.neditors = 1;
    d->st.editor[0].weight = 1; d->st.editor[0].active = -1;
    d->maximized_group = -1; d->overlay_panel = -1;
}

/* Puts registered panel p at its preferred place in state s (used for new panels and presets). */
static void place_default(const Dock *d, DockState *s, int p) {
    const DockPanelDesc *pd = &d->desc[p];
    DockPanelState *ps = &s->panel[p];
    int r = pd->region >= 0 && pd->region < DOCK_REGIONS ? pd->region : DOCK_BOTTOM;
    if (r == DOCK_TOP && !s->top_enabled) r = DOCK_BOTTOM;
    ps->home_region = r; ps->group = -1;
    if (pd->start_hidden) { ps->mode = DOCK_HIDDEN; return; }
    /* panels preferring the same region share its first group as tabs (Results + Output) */
    int g = s->ngroups_in[r] ? s->group_order[r][0] : new_group(s, r, -1);
    if (g < 0) { ps->mode = DOCK_HIDDEN; return; }
    int keep = s->group[g].active;
    attach(s, p, g, -1);
    if (keep >= 0) s->group[g].active = keep;   /* the first panel registered stays in front */
}

/* Copies the descriptor (its strings are borrowed, see dock.h), creates the panel's state
 * (on the bar, bar order = registry index) and places it at its preferred region. caps == 0
 * means "all capabilities". Not an undoable transaction. */
bool dock_register(Dock *d, const DockPanelDesc *desc) {
    if (!desc->id || !desc->id[0] || strlen(desc->id) >= sizeof d->st.panel[0].id) { fail(d, "panel id missing or too long"); return false; }
    if (dock_find(d, desc->id) >= 0) { snprintf(d->err, sizeof d->err, "duplicate panel id \"%s\": second registration rejected", desc->id); return false; }
    if (d->ndesc >= DOCK_MAX_PANELS) { fail(d, "too many panels"); return false; }
    int p = d->ndesc++;
    d->desc[p] = *desc;
    if (d->desc[p].caps == 0) d->desc[p].caps = DOCK_CAP_ALL;
    DockPanelState *ps = &d->st.panel[p];
    memset(ps, 0, sizeof *ps);
    snprintf(ps->id, sizeof ps->id, "%s", desc->id);
    ps->in_bar = true; ps->bar_order = p; ps->group = -1;
    if (d->st.npanels <= p) d->st.npanels = p + 1;
    place_default(d, &d->st, p);
    d->dirty = true;
    return true;
}

/* Linear lookup of a registered panel by its stable id; -1 when unknown. */
int dock_find(const Dock *d, const char *id) {
    for (int i = 0; i < d->ndesc; i++) if (strcmp(d->desc[i].id, id) == 0) return i;
    return -1;
}

/* Removes the panel and shifts every later panel index down by one in the registry, the
 * state and all groups. The undo history is cleared because it holds the old indices. */
bool dock_unregister(Dock *d, const char *id) {
    int p = dock_find(d, id);
    if (p < 0) { fail(d, "unknown panel"); return false; }
    DockState s = d->st;
    detach(&s, p);
    /* compact indices: panel p+1.. move down one slot in the registry and in every group */
    for (int g = 0; g < DOCK_MAX_GROUPS; g++) {
        if (!s.group[g].used) continue;
        for (int i = 0; i < s.group[g].ntabs; i++) if (s.group[g].tabs[i] > p) s.group[g].tabs[i]--;
        if (s.group[g].active > p) s.group[g].active--;
    }
    memmove(&s.panel[p], &s.panel[p + 1], sizeof s.panel[0] * (size_t)(s.npanels - p - 1));
    s.npanels--;
    memmove(&d->desc[p], &d->desc[p + 1], sizeof d->desc[0] * (size_t)(d->ndesc - p - 1));
    d->ndesc--;
    normalize_lanes(&s);   /* its group may have been the last of a lane */
    d->st = s;
    d->nundo = 0;   /* older snapshots refer to the old indices */
    if (d->overlay_panel == p) d->overlay_panel = -1; else if (d->overlay_panel > p) d->overlay_panel--;
    d->maximized_group = -1;
    d->dirty = true; d->change_seq++;
    return true;
}

/* For panels registered after a state was loaded (their id slot is still empty): fill the id
 * and place them by default. Not a transaction. */
void dock_adopt_new_panels(Dock *d) {
    for (int p = 0; p < d->ndesc; p++) if (d->st.panel[p].id[0] == 0) {
        snprintf(d->st.panel[p].id, sizeof d->st.panel[p].id, "%s", d->desc[p].id);
        d->st.panel[p].in_bar = true; d->st.panel[p].bar_order = p;
        place_default(d, &d->st, p);
    }
}

/* ======================================================================== */
/* queries                                                                   */
/* ======================================================================== */

/* Group currently holding docked panel p, or -1 (hidden, autohide, floating, inconsistent). */
int dock_group_of(const Dock *d, int p) {
    if (p < 0 || p >= d->st.npanels || d->st.panel[p].mode != DOCK_DOCKED) return -1;
    int g = d->st.panel[p].group;
    return in_group(&d->st, p, g) ? g : -1;
}
/* Region of p's group when docked, otherwise the home region it would return to. */
int dock_region_of(const Dock *d, int p) {
    int g = dock_group_of(d, p);
    return g >= 0 ? d->st.group[g].region : d->st.panel[p].home_region;
}
/* True when p is docked in a collapsed (folded) group. */
bool dock_is_collapsed(const Dock *d, int p) { int g = dock_group_of(d, p); return g >= 0 && d->st.group[g].collapsed; }
/* True when p's content is actually on screen: shown as the overlay, or the active tab of an
 * expanded group that is not hidden by maximize, focus mode or an auto-collapsed region. */
bool dock_is_visible(const Dock *d, int p) {
    if (d->overlay_panel == p) return true;
    int g = dock_group_of(d, p);
    if (g < 0 || d->st.group[g].collapsed || d->st.group[g].active != p) return false;
    if (d->maximized_group >= 0) return d->maximized_group == g;
    return !d->st.focus_mode && !d->auto_collapsed[d->st.group[g].region];
}

/* ======================================================================== */
/* operations                                                                */
/* ======================================================================== */

/* Puts p back in its group (or a new group in its home region), expanded and active. */
static bool show_docked(Dock *d, int p) {
    DockState s = d->st;
    DockPanelState *ps = &s.panel[p];
    int g = dock_group_of(d, p);
    if (g >= 0) {
        if (!s.group[g].collapsed && s.group[g].active == p && !d->auto_collapsed[s.group[g].region]) return true;
        s.group[g].collapsed = false; s.group[g].active = p;
        return commit(d, &s);
    }
    /* back to its previous group when that group still lives in its home region */
    int r = ps->home_region >= 0 && ps->home_region < DOCK_REGIONS ? ps->home_region : d->desc[p].region;
    if (r == DOCK_TOP && !s.top_enabled) r = DOCK_BOTTOM;
    int target = (ps->group >= 0 && ps->group < DOCK_MAX_GROUPS && s.group[ps->group].used && s.group[ps->group].region == r) ? ps->group : -1;
    if (target < 0) target = new_group(&s, r, -1);
    if (target < 0) { fail(d, "no room for another group"); return false; }
    attach(&s, p, target, -1);
    s.group[target].collapsed = false;
    return commit(d, &s);
}

/* Makes p visible: as an overlay in focus mode or when it is auto-hide, else docked. */
bool dock_show(Dock *d, int p) {
    if (!valid_panel(d, p)) return false;
    /* in focus mode, opening a panel shows it over the editor; the snapshot stays intact */
    if (d->st.focus_mode) return dock_open_overlay(d, p);
    if (d->st.panel[p].mode == DOCK_AUTOHIDE) return dock_open_overlay(d, p);
    return show_docked(d, p);
}

/* Pushes the current state on the undo ring without changing it (start of a splitter drag). */
void dock_begin_gesture(Dock *d) {
    d->undo_head = (d->undo_head + 1) % DOCK_UNDO_DEPTH;
    d->undo[d->undo_head] = d->st;
    if (d->nundo < DOCK_UNDO_DEPTH) d->nundo++;
}

/* Takes p out of its group and marks it hidden, remembering the group index so dock_show()
 * can put it back. Refused for panels without DOCK_CAP_HIDE. */
bool dock_hide(Dock *d, int p) {
    if (!valid_panel(d, p)) return false;
    if (!(d->desc[p].caps & DOCK_CAP_HIDE)) { fail(d, "this panel cannot be hidden"); return false; }
    if (d->overlay_panel == p) d->overlay_panel = -1;
    if (d->st.panel[p].mode == DOCK_HIDDEN) return true;
    DockState s = d->st;
    int g = s.panel[p].group;
    detach(&s, p);
    s.panel[p].group = g;           /* remember where it was */
    s.panel[p].mode = DOCK_HIDDEN;
    return commit(d, &s);
}

/* Hides p when it is visible, shows it otherwise (panel bar buttons, Cmd+1..4). */
bool dock_toggle(Dock *d, int p) { return dock_is_visible(d, p) ? dock_hide(d, p) : dock_show(d, p); }

/* Folds group g to its region's strip; refused if any of its panels lacks DOCK_CAP_COLLAPSE.
 * Un-maximizes the group if needed. */
bool dock_collapse_group(Dock *d, int g) {
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    for (int i = 0; i < d->st.group[g].ntabs; i++) if (!(d->desc[d->st.group[g].tabs[i]].caps & DOCK_CAP_COLLAPSE)) { fail(d, "a panel in this group cannot be collapsed"); return false; }
    if (d->st.group[g].collapsed) return true;
    DockState s = d->st; s.group[g].collapsed = true;
    if (d->maximized_group == g) d->maximized_group = -1;
    return commit(d, &s);
}
/* Unfolds a collapsed group g. */
bool dock_expand_group(Dock *d, int g) {
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    if (!d->st.group[g].collapsed) return true;
    DockState s = d->st; s.group[g].collapsed = false;
    return commit(d, &s);
}

/* Brings p to the front of its group (no undo step); shows it first if it is not docked. */
bool dock_activate(Dock *d, int p) {
    int g = dock_group_of(d, p);
    if (g < 0) return dock_show(d, p);
    if (d->st.group[g].active == p) return true;
    /* switching tabs is not worth an undo step; it is persisted like the rest */
    d->st.group[g].active = p; d->dirty = true; d->change_seq++;
    return true;
}

/* Moves p into a new group of its own at the end of `region`. If p is already alone in a
 * group there, it just expands that group. */
bool dock_move_to_region(Dock *d, int p, int region) {
    if (!valid_panel(d, p)) return false;
    if (region < 0 || region >= DOCK_REGIONS) { fail(d, "unknown region"); return false; }
    if (!(d->desc[p].caps & DOCK_CAP_MOVE)) { fail(d, "this panel cannot be moved"); return false; }
    if (region == DOCK_TOP && !d->st.top_enabled) { fail(d, "docking on the top edge is turned off (View > Panels > Allow Top Edge)"); return false; }
    DockState s = d->st;
    int g = dock_group_of(d, p);
    if (g >= 0 && s.group[g].region == region && s.group[g].ntabs == 1) {   /* already alone there */
        s.group[g].collapsed = false; return commit(d, &s);
    }
    detach(&s, p);
    int ng = new_group(&s, region, -1);
    if (ng < 0) { fail(d, "no room for another group"); return false; }
    attach(&s, p, ng, -1);
    return commit(d, &s);
}

/* Moves p into group g as a tab at `index` (-1 = last). Within its own group this is a
 * reorder. The target group is expanded. */
bool dock_move_to_group(Dock *d, int p, int g, int index) {
    if (!valid_panel(d, p)) return false;
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    if (!(d->desc[p].caps & DOCK_CAP_MOVE)) { fail(d, "this panel cannot be moved"); return false; }
    if (dock_group_of(d, p) == g) {
        int from = -1; for (int i = 0; i < d->st.group[g].ntabs; i++) if (d->st.group[g].tabs[i] == p) from = i;
        return dock_reorder_tab(d, g, from, index);
    }
    if (d->st.group[g].ntabs >= DOCK_MAX_TABS) { fail(d, "this group is full"); return false; }
    DockState s = d->st;
    detach(&s, p);
    if (!s.group[g].used) { fail(d, "group vanished"); return false; }
    attach(&s, p, g, index);
    s.group[g].collapsed = false;
    return commit(d, &s);
}

/* Moves p into a new group placed just before (after = false) or after group g in the same
 * region, inheriting g's weight. Refused when p is already alone in g. */
bool dock_split_group(Dock *d, int p, int g, bool after) {
    if (!valid_panel(d, p)) return false;
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    if (!(d->desc[p].caps & DOCK_CAP_MOVE)) { fail(d, "this panel cannot be moved"); return false; }
    if (dock_group_of(d, p) == g && d->st.group[g].ntabs == 1) { fail(d, "the panel is already alone in its group"); return false; }
    DockState s = d->st;
    int r = s.group[g].region;
    detach(&s, p);
    if (!s.group[g].used) { fail(d, "group vanished"); return false; }
    int pos = order_pos(&s, g) + (after ? 1 : 0);
    int ng = new_group(&s, r, pos);
    if (ng < 0) { fail(d, "no room for another group"); return false; }
    s.group[ng].weight = s.group[g].weight;
    s.group[ng].lane = s.group[g].lane;   /* along the axis: same lane */
    attach(&s, p, ng, -1);
    return commit(d, &s);
}

static float region_min(const Dock *d, const DockState *s, int r);

/* See dock.h (PNL-011): a new lane next to g's lane, holding p. */
bool dock_split_lane(Dock *d, int p, int g, bool after) {
    if (!valid_panel(d, p)) return false;
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    if (!(d->desc[p].caps & DOCK_CAP_MOVE)) { fail(d, "this panel cannot be moved"); return false; }
    int r = d->st.group[g].region, lane = d->st.group[g].lane;
    if (dock_lanes(&d->st, r) >= DOCK_MAX_LANES) { fail(d, "no room for another side-by-side group in this region"); return false; }
    if (dock_group_of(d, p) == g && d->st.group[g].ntabs == 1) {
        int in_lane = 0;
        for (int k = 0; k < d->st.ngroups_in[r]; k++) if (d->st.group[d->st.group_order[r][k]].lane == lane) in_lane++;
        if (in_lane == 1) { fail(d, "the panel is already alone in its lane"); return false; }
    }
    DockState s = d->st;
    /* open a slot: lanes from the new one on move up by one, with their weights */
    int nl = lane + (after ? 1 : 0);
    for (int k = 0; k < s.ngroups_in[r]; k++) { DockGroup *gr = &s.group[s.group_order[r][k]]; if (gr->lane >= nl) gr->lane++; }
    for (int l = DOCK_MAX_LANES - 1; l > nl; l--) s.lane_weight[r][l] = s.lane_weight[r][l - 1];
    s.lane_weight[r][nl] = s.lane_weight[r][after ? lane : lane + 1];
    detach(&s, p);
    int pos = s.group[g].used ? order_pos(&s, g) + 1 : -1;
    int ng = new_group(&s, r, pos);
    if (ng < 0) { fail(d, "no room for another group"); return false; }
    s.group[ng].lane = nl;
    attach(&s, p, ng, -1);
    /* the region grows by the new lane, so the existing lanes keep their size, and never below
     * every lane's minimum (the window may still fold it: PNL-038) */
    float add = (side_region(r) ? d->desc[p].min_w : d->desc[p].min_h + (s.compact ? 26 : 32)) + 6;
    float before = s.region_size[r], need = region_min(d, &s, r);
    if (need < before + add) need = before + add;
    if (s.region_size[r] < need) s.region_size[r] = need < REGION_MAX_SIZE ? need : REGION_MAX_SIZE;
    /* the new lane's share is the room it added: the lanes that were there keep their size */
    float others = 0; int nl_all = dock_lanes(&s, r);
    for (int l = 0; l < nl_all; l++) if (l != nl) others += s.lane_weight[r][l];
    float grown = s.region_size[r] - before;
    if (before > 1 && grown > 0 && others > 0) s.lane_weight[r][nl] = others * grown / before;
    return commit(d, &s);
}

/* Moves the tab at position `from` of group g to position `to` (clamped); the active tab
 * does not change. */
bool dock_reorder_tab(Dock *d, int g, int from, int to) {
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used) { fail(d, "unknown group"); return false; }
    DockGroup *gr = &d->st.group[g];
    if (from < 0 || from >= gr->ntabs) { fail(d, "unknown tab"); return false; }
    if (to < 0) to = 0;
    if (to >= gr->ntabs) to = gr->ntabs - 1;
    if (from == to) return true;
    DockState s = d->st;
    DockGroup *sg = &s.group[g];
    int p = sg->tabs[from];
    memmove(&sg->tabs[from], &sg->tabs[from + 1], sizeof(int) * (size_t)(sg->ntabs - from - 1));
    memmove(&sg->tabs[to + 1], &sg->tabs[to], sizeof(int) * (size_t)(sg->ntabs - 1 - to));
    sg->tabs[to] = p;   /* active tab unchanged (PNL-010) */
    return commit(d, &s);
}

/* Changes p's mode (docked, hidden, autohide, floating, detached), checking capabilities.
 * Leaving a group remembers it for the return trip; floating / detached get a default
 * window geometry (points) the first time. The shell creates or closes the native windows. */
bool dock_set_mode(Dock *d, int p, int mode) {
    if (!valid_panel(d, p)) return false;
    if (mode < 0 || mode >= DOCK_MODES) { fail(d, "unknown mode"); return false; }
    if (mode == DOCK_AUTOHIDE && !(d->desc[p].caps & DOCK_CAP_AUTOHIDE)) { fail(d, "this panel does not support auto-hide"); return false; }
    if ((mode == DOCK_FLOATING || mode == DOCK_DETACHED) && !(d->desc[p].caps & DOCK_CAP_FLOAT)) { fail(d, "this panel cannot leave the main window"); return false; }
    if (mode == DOCK_DOCKED) {
        if (d->st.panel[p].mode == DOCK_DOCKED) return true;
        if (d->overlay_panel == p) d->overlay_panel = -1;
        return show_docked(d, p);           /* back to its group / home region */
    }
    if (mode == DOCK_HIDDEN) return dock_hide(d, p);
    DockState s = d->st;
    int g = s.panel[p].group;
    detach(&s, p);
    s.panel[p].group = g;
    s.panel[p].mode = mode;
    if ((mode == DOCK_FLOATING || mode == DOCK_DETACHED) && s.panel[p].float_w <= 0) { s.panel[p].float_w = 560; s.panel[p].float_h = 380; s.panel[p].float_x = 160; s.panel[p].float_y = 160; }
    if (d->overlay_panel == p) d->overlay_panel = -1;
    return commit(d, &s);
}

/* Largest content minimum of the panels docked in region r of state s, along the region's
 * axis (width for side regions, height otherwise), never below REGION_MIN_SIZE. Points. */
static float region_min(const Dock *d, const DockState *s, int r) {
    /* PNL-011: lanes sit side by side across this size, so their minimums add up */
    float lane_min[DOCK_MAX_LANES] = { 0 };
    for (int k = 0; k < s->ngroups_in[r]; k++) {
        const DockGroup *gr = &s->group[s->group_order[r][k]];
        int l = gr->lane >= 0 && gr->lane < DOCK_MAX_LANES ? gr->lane : 0;
        for (int i = 0; i < gr->ntabs; i++) {
            float v = side_region(r) ? d->desc[gr->tabs[i]].min_w : d->desc[gr->tabs[i]].min_h;
            if (v > lane_min[l]) lane_min[l] = v;
        }
    }
    int n = dock_lanes(s, r);
    if (n <= 1) return lane_min[0] > REGION_MIN_SIZE ? lane_min[0] : REGION_MIN_SIZE;
    float m = 0, header = side_region(r) ? 0 : (s->compact ? 26 : 32), splitter = 6;
    for (int l = 0; l < n; l++) m += (lane_min[l] > 60 ? lane_min[l] : 60) + (l ? header : 0);
    m += splitter * (float)(n - 1);
    return m > REGION_MIN_SIZE ? m : REGION_MIN_SIZE;
}

/* Sets a region's size (points), clamped between its panels' minimum and what leaves the
 * editor its minimum next to the opposite region (using the workspace size of the last
 * dock_layout()). Writes the state directly: no undo step, see dock_begin_gesture(). */
bool dock_set_region_size(Dock *d, int region, float size) {
    if (region < 0 || region >= DOCK_REGIONS) { fail(d, "unknown region"); return false; }
    if (!isfinite(size)) { fail(d, "invalid size"); return false; }
    DockMetrics m = dock_default_metrics(d);
    float lo = region_min(d, &d->st, region);
    /* the editor keeps its minimum: the opposite region and the editor stay within the window */
    float hi = REGION_MAX_SIZE;
    if (d->last_w > 0) {
        int opp = region == DOCK_LEFT ? DOCK_RIGHT : region == DOCK_RIGHT ? DOCK_LEFT : region == DOCK_BOTTOM ? DOCK_TOP : DOCK_BOTTOM;
        bool opp_shown = false;
        for (int k = 0; k < d->st.ngroups_in[opp]; k++) if (!d->st.group[d->st.group_order[opp][k]].collapsed) opp_shown = true;
        float axis = side_region(region) ? d->last_w - m.rail_w : d->last_h;
        float edmin = side_region(region) ? m.editor_min_w : m.editor_min_h;
        hi = axis - edmin - (opp_shown ? d->st.region_size[opp] : 0) - 2 * m.strip_h;
        if (hi < lo) hi = lo;
    }
    if (size < lo) size = lo;
    if (size > hi) size = hi;
    if (fabsf(size - d->st.region_size[region]) < 0.01f) return true;
    /* resizing is continuous while dragging: one undo step per drag is handled by the shell
     * (dock_set_region_size is applied directly; the shell pushes the snapshot at press) */
    d->st.region_size[region] = size;
    d->dirty = true; d->change_seq++;
    return true;
}

/* Drag of the splitter between groups k and k+1 of a region (k indexes group_order): moves
 * delta_points of length from the second to the first by adjusting their weights. Returns
 * false (without setting d->err) when either side would go below its minimum. No undo step. */
bool dock_set_group_weight(Dock *d, int region, int k, float delta_points) {
    return dock_set_group_pair_weight(d, region, k, k + 1, delta_points);
}

/* See dock.h. The two groups share their lane's length; other lanes are not involved. */
bool dock_set_group_pair_weight(Dock *d, int region, int k1, int k2, float delta_points) {
    if (region < 0 || region >= DOCK_REGIONS || k1 < 0 || k2 < 0 || k1 >= d->st.ngroups_in[region] || k2 >= d->st.ngroups_in[region] || k1 == k2) { fail(d, "no such splitter"); return false; }
    int a = d->st.group_order[region][k1], b = d->st.group_order[region][k2];
    if (d->st.group[a].lane != d->st.group[b].lane) { fail(d, "the groups are in different lanes"); return false; }
    float len = side_region(region) ? d->last_h : d->last_w;
    if (len <= 0) len = 600;
    float total = 0;
    for (int i = 0; i < d->st.ngroups_in[region]; i++) { const DockGroup *gi = &d->st.group[d->st.group_order[region][i]]; if (gi->lane == d->st.group[a].lane) total += gi->weight; }
    float dw = delta_points / len * total;
    float wa = d->st.group[a].weight + dw, wb = d->st.group[b].weight - dw;
    DockMetrics mt = dock_default_metrics(d);
    /* each side keeps the minimum of its panels */
    if (wa < total * group_min_len(d, a, &mt) / len || wb < total * group_min_len(d, b, &mt) / len) return false;
    d->st.group[a].weight = wa; d->st.group[b].weight = wb;
    d->dirty = true; d->change_seq++;
    return true;
}

/* Minimum size of one lane across the region's axis (points): its widest panel minimum, plus
 * the header in bottom / top regions (where lanes are stacked). */
static float lane_min_cross(const Dock *d, const DockState *s, int r, int lane, const DockMetrics *m) {
    float mn = 60;
    for (int k = 0; k < s->ngroups_in[r]; k++) {
        const DockGroup *gr = &s->group[s->group_order[r][k]];
        if (gr->lane != lane || gr->collapsed) continue;
        for (int i = 0; i < gr->ntabs; i++) { float v = side_region(r) ? d->desc[gr->tabs[i]].min_w : d->desc[gr->tabs[i]].min_h; if (v > mn) mn = v; }
    }
    return mn + (side_region(r) ? 0 : m->header_h);
}

/* See dock.h (PNL-011). The lanes share the region's size; each keeps its minimum. */
bool dock_set_lane_weight(Dock *d, int region, int la, int lb, float delta_points) {
    int n = region >= 0 && region < DOCK_REGIONS ? dock_lanes(&d->st, region) : 0;
    if (la < 0 || lb < 0 || la >= n || lb >= n || la == lb) { fail(d, "no such splitter"); return false; }
    float len = d->st.region_size[region];
    float total = 0; for (int l = 0; l < n; l++) total += d->st.lane_weight[region][l];
    float dw = delta_points / (len > 1 ? len : 1) * total;
    float wa = d->st.lane_weight[region][la] + dw, wb = d->st.lane_weight[region][lb] - dw;
    DockMetrics mt = dock_default_metrics(d);
    if (wa < total * lane_min_cross(d, &d->st, region, la, &mt) / len || wb < total * lane_min_cross(d, &d->st, region, lb, &mt) / len) return false;
    d->st.lane_weight[region][la] = wa; d->st.lane_weight[region][lb] = wb;
    d->dirty = true; d->change_seq++;
    return true;
}

/* Double-click on a splitter: restores the region's preferred size (the last docked panel
 * with a pref_size wins, else the default) and equal group weights. Undoable. */
bool dock_reset_region_size(Dock *d, int region) {
    if (region < 0 || region >= DOCK_REGIONS) { fail(d, "unknown region"); return false; }
    float pref = default_region_size[region];
    for (int k = 0; k < d->st.ngroups_in[region]; k++) {
        const DockGroup *gr = &d->st.group[d->st.group_order[region][k]];
        for (int i = 0; i < gr->ntabs; i++) if (d->desc[gr->tabs[i]].pref_size > 0) pref = d->desc[gr->tabs[i]].pref_size;
    }
    DockState s = d->st; s.region_size[region] = pref;
    for (int k = 0; k < s.ngroups_in[region]; k++) s.group[s.group_order[region][k]].weight = 1;
    for (int l = 0; l < DOCK_MAX_LANES; l++) s.lane_weight[region][l] = 1;   /* equal lanes too */
    return commit(d, &s);
}

/* Maximizes group g over the whole workspace, or restores when something is already
 * maximized (g is then ignored). Transient: neither persisted nor undoable. */
bool dock_toggle_maximize(Dock *d, int g) {
    if (d->maximized_group >= 0) { d->maximized_group = -1; d->change_seq++; return true; }
    if (g < 0 || g >= DOCK_MAX_GROUPS || !d->st.group[g].used || d->st.group[g].collapsed) { fail(d, "nothing to maximize"); return false; }
    int p = d->st.group[g].active;
    if (p >= 0 && !(d->desc[p].caps & DOCK_CAP_MAXIMIZE)) { fail(d, "this panel cannot be maximized"); return false; }
    d->maximized_group = g;   /* transient: the saved layout is untouched (PNL-014) */
    d->change_seq++;
    return true;
}

/* Focus mode hides the bar and every region, leaving only the editor; panels opened
 * meanwhile appear as overlays. The flag is part of the persisted state. */
bool dock_toggle_focus_mode(Dock *d) {
    DockState s = d->st;
    s.focus_mode = !s.focus_mode;
    d->overlay_panel = -1; d->maximized_group = -1;
    return commit(d, &s);
}

/* Pins or unpins p's button on the panel bar. */
bool dock_set_in_bar(Dock *d, int p, bool on) {
    if (!valid_panel(d, p)) return false;
    if (d->st.panel[p].in_bar == on) return true;
    DockState s = d->st; s.panel[p].in_bar = on;
    return commit(d, &s);
}

/* Moves p's bar button to position new_order and renumbers every panel's bar_order 0..n-1. */
bool dock_move_bar_button(Dock *d, int p, int new_order) {
    if (!valid_panel(d, p)) return false;
    DockState s = d->st;
    /* renumber: buttons sorted by bar_order, p inserted at new_order */
    int order[DOCK_MAX_PANELS], n = 0;
    for (int o = 0; o < 1000 && n < s.npanels; o++)
        for (int i = 0; i < s.npanels; i++) if (s.panel[i].bar_order == o && i != p) order[n++] = i;
    for (int i = 0; i < s.npanels; i++) { bool seen = i == p; for (int k = 0; k < n; k++) if (order[k] == i) seen = true; if (!seen) order[n++] = i; }
    if (new_order < 0) new_order = 0;
    if (new_order > n) new_order = n;
    memmove(&order[new_order + 1], &order[new_order], sizeof(int) * (size_t)(n - new_order));
    order[new_order] = p; n++;
    for (int k = 0; k < n; k++) s.panel[order[k]].bar_order = k;
    return commit(d, &s);
}

/* Shows p as the transient overlay anchored to its region's edge (autohide / focus mode). */
bool dock_open_overlay(Dock *d, int p) {
    if (!valid_panel(d, p)) return false;
    d->overlay_panel = p;    /* transient: dismissing it changes nothing persisted */
    d->change_seq++;
    return true;
}
/* Dismisses the overlay, if any. */
void dock_close_overlay(Dock *d) { if (d->overlay_panel >= 0) { d->overlay_panel = -1; d->change_seq++; } }

/* Restores the most recent snapshot from the undo ring, keeping the current editor groups,
 * and clears maximize and overlay. */
bool dock_undo(Dock *d) {
    if (!d->nundo) { fail(d, "nothing to undo in the layout"); return false; }
    DockEditorGroup keep_ed[DOCK_MAX_EDITOR_GROUPS]; int keep_n = d->st.neditors; bool keep_down = d->st.editor_split_down; int keep_act = d->st.active_editor_group;
    memcpy(keep_ed, d->st.editor, sizeof keep_ed);
    d->st = d->undo[d->undo_head];
    /* undo restores the arrangement, never the editor tabs (their buffers belong to the user) */
    memcpy(d->st.editor, keep_ed, sizeof keep_ed); d->st.neditors = keep_n; d->st.editor_split_down = keep_down; d->st.active_editor_group = keep_act;
    d->undo_head = (d->undo_head + DOCK_UNDO_DEPTH - 1) % DOCK_UNDO_DEPTH;
    d->nundo--;
    d->maximized_group = -1; d->overlay_panel = -1;
    d->dirty = true; d->change_seq++;
    return true;
}

/* ======================================================================== */
/* editor groups                                                             */
/* ======================================================================== */

/* Editor group holding editor tab editor_id, or -1. */
int dock_editor_group_of(const Dock *d, int editor_id) {
    for (int g = 0; g < d->st.neditors; g++)
        for (int i = 0; i < d->st.editor[g].ntabs; i++) if (d->st.editor[g].tabs[i] == editor_id) return g;
    return -1;
}

/* Appends editor tab editor_id to egroup (invalid = the active editor group) and activates
 * it. Returns the group used, or -1 when it is full. Editor tab changes are not undoable. */
int dock_editor_add_tab(Dock *d, int egroup, int editor_id) {
    if (egroup < 0 || egroup >= d->st.neditors) egroup = d->st.active_editor_group;
    DockEditorGroup *e = &d->st.editor[egroup];
    if (e->ntabs >= DOCK_MAX_EDITOR_TABS) { fail(d, "too many editor tabs"); return -1; }
    e->tabs[e->ntabs++] = editor_id; e->active = editor_id;
    d->st.active_editor_group = egroup;
    d->change_seq++;
    return egroup;
}

/* Removes editor_id from editor group g, picking a neighbour as the new active tab; an
 * emptied group disappears unless it is the only one. */
static void editor_remove(DockState *s, int g, int editor_id) {
    DockEditorGroup *e = &s->editor[g];
    int k = -1; for (int i = 0; i < e->ntabs; i++) if (e->tabs[i] == editor_id) k = i;
    if (k < 0) return;
    memmove(&e->tabs[k], &e->tabs[k + 1], sizeof(int) * (size_t)(e->ntabs - k - 1));
    e->ntabs--;
    if (e->active == editor_id) e->active = e->ntabs ? e->tabs[k < e->ntabs ? k : e->ntabs - 1] : -1;
    if (!e->ntabs && s->neditors > 1) {
        memmove(&s->editor[g], &s->editor[g + 1], sizeof s->editor[0] * (size_t)(s->neditors - g - 1));
        s->neditors--;
        if (s->active_editor_group >= s->neditors) s->active_editor_group = s->neditors - 1;
    }
}

/* Closes an editor tab; refused for the very last tab of the window (the center must
 * always hold an editor). The shell frees the editor itself. */
bool dock_editor_close_tab(Dock *d, int editor_id) {
    int g = dock_editor_group_of(d, editor_id);
    if (g < 0) { fail(d, "unknown editor tab"); return false; }
    int total = 0; for (int i = 0; i < d->st.neditors; i++) total += d->st.editor[i].ntabs;
    if (total <= 1) { fail(d, "the last editor cannot be closed"); return false; }
    editor_remove(&d->st, g, editor_id);
    d->change_seq++;
    return true;
}

/* Makes editor_id the active tab of its group and that group the active editor group. */
bool dock_editor_activate(Dock *d, int editor_id) {
    int g = dock_editor_group_of(d, editor_id);
    if (g < 0) { fail(d, "unknown editor tab"); return false; }
    d->st.editor[g].active = editor_id; d->st.active_editor_group = g;
    d->change_seq++;
    return true;
}

/* Moves editor_id into a new editor group right after its own, and sets the split direction
 * of the whole center (down = stacked). Needs a second tab left behind in the source group. */
bool dock_editor_split(Dock *d, int editor_id, bool down) {
    int g = dock_editor_group_of(d, editor_id);
    if (g < 0) { fail(d, "unknown editor tab"); return false; }
    if (d->st.neditors >= DOCK_MAX_EDITOR_GROUPS) { fail(d, "the editor is already split into the maximum number of groups"); return false; }
    if (d->st.editor[g].ntabs < 2) { fail(d, "a split needs a second tab in this group"); return false; }
    DockState *s = &d->st;
    editor_remove(s, g, editor_id);
    int ng = g + 1;
    memmove(&s->editor[ng + 1], &s->editor[ng], sizeof s->editor[0] * (size_t)(s->neditors - ng));
    memset(&s->editor[ng], 0, sizeof s->editor[ng]);
    s->editor[ng].tabs[0] = editor_id; s->editor[ng].ntabs = 1; s->editor[ng].active = editor_id; s->editor[ng].weight = s->editor[g].weight;
    s->neditors++;
    s->editor_split_down = down;
    s->active_editor_group = ng;
    d->change_seq++;
    return true;
}

/* Moves editor_id to editor group egroup at `index` (out of range = last) and activates it.
 * egroup is adjusted if removing the tab made its source group disappear. */
bool dock_editor_move_tab(Dock *d, int editor_id, int egroup, int index) {
    int g = dock_editor_group_of(d, editor_id);
    if (g < 0 || egroup < 0 || egroup >= d->st.neditors) { fail(d, "unknown editor group"); return false; }
    DockState *s = &d->st;
    /* refuse before touching the state: a full destination must leave the tab where it was */
    if (egroup != g && s->editor[egroup].ntabs >= DOCK_MAX_EDITOR_TABS) { fail(d, "that editor group is full"); return false; }
    int n_before = s->neditors;
    editor_remove(s, g, editor_id);
    if (s->neditors < n_before && egroup > g) egroup--;      /* the source group vanished */
    if (egroup >= s->neditors) egroup = s->neditors - 1;
    DockEditorGroup *e = &s->editor[egroup];
    if (e->ntabs >= DOCK_MAX_EDITOR_TABS) { fail(d, "that editor group is full"); return false; }
    if (index < 0 || index > e->ntabs) index = e->ntabs;
    memmove(&e->tabs[index + 1], &e->tabs[index], sizeof(int) * (size_t)(e->ntabs - index));
    e->tabs[index] = editor_id; e->ntabs++; e->active = editor_id;
    s->active_editor_group = egroup;
    d->change_seq++;
    return true;
}

/* ======================================================================== */
/* presets                                                                   */
/* ======================================================================== */

/* Display name of a built-in preset ("?" when out of range). Static string. */
const char *dock_preset_name(int preset) {
    static const char *const names[DOCK_PRESETS] = { "Default", "Query Focus", "Data Exploration" };
    return preset >= 0 && preset < DOCK_PRESETS ? names[preset] : "?";
}

/* A fresh arrangement of every registered panel at its preferred place, keeping the editor
 * groups and the bar choices of `cur`. */
static void base_state(const Dock *d, const DockState *cur, DockState *s) {
    memset(s, 0, sizeof *s);
    s->schema = DOCK_SCHEMA_VERSION;
    for (int r = 0; r < DOCK_REGIONS; r++) { s->region_size[r] = default_region_size[r]; for (int l = 0; l < DOCK_MAX_LANES; l++) s->lane_weight[r][l] = 1; }
    s->npanels = d->ndesc;
    for (int p = 0; p < d->ndesc; p++) {
        snprintf(s->panel[p].id, sizeof s->panel[p].id, "%s", d->desc[p].id);
        s->panel[p].in_bar = cur->panel[p].in_bar; s->panel[p].bar_order = cur->panel[p].bar_order;
        s->panel[p].float_x = cur->panel[p].float_x; s->panel[p].float_y = cur->panel[p].float_y;
        s->panel[p].float_w = cur->panel[p].float_w; s->panel[p].float_h = cur->panel[p].float_h;
        place_default(d, s, p);
    }
    memcpy(s->editor, cur->editor, sizeof s->editor);
    s->neditors = cur->neditors; s->editor_split_down = cur->editor_split_down; s->active_editor_group = cur->active_editor_group;
    s->top_enabled = cur->top_enabled; s->compact = cur->compact;
}

/* Default: every panel at its preferred place. Query Focus: Explorer collapsed, short
 * bottom region. Data Exploration: wider left, tall bottom, Inspector docked on the right.
 * In all of them Results is the active tab of its group. Undoable. */
bool dock_apply_preset(Dock *d, int preset) {
    if (preset < 0 || preset >= DOCK_PRESETS) { fail(d, "unknown preset"); return false; }
    DockState s;
    base_state(d, &d->st, &s);
    int explorer = dock_find(d, "lume.data.explorer"), results = dock_find(d, "lume.data.results"), inspector = dock_find(d, "lume.data.inspector");
    if (preset == DOCK_PRESET_QUERY_FOCUS) {
        if (explorer >= 0 && s.panel[explorer].mode == DOCK_DOCKED) s.group[s.panel[explorer].group].collapsed = true;
        s.region_size[DOCK_BOTTOM] = 180;
    } else if (preset == DOCK_PRESET_DATA_EXPLORATION) {
        s.region_size[DOCK_LEFT] = 320;
        s.region_size[DOCK_BOTTOM] = 420;
        if (inspector >= 0) {
            int g = new_group(&s, DOCK_RIGHT, -1);
            if (g >= 0) attach(&s, inspector, g, -1);
            s.region_size[DOCK_RIGHT] = 320;
        }
    }
    if (results >= 0 && s.panel[results].mode == DOCK_DOCKED) s.group[s.panel[results].group].active = results;
    d->maximized_group = -1; d->overlay_panel = -1;
    return commit(d, &s);
}

/* Installs a saved arrangement (named layout, import) but keeps the live editor groups;
 * goes through commit(), so an invalid state is refused. */
bool dock_apply_state(Dock *d, const DockState *in) {
    DockState s = *in;
    memcpy(s.editor, d->st.editor, sizeof s.editor);
    s.neditors = d->st.neditors; s.editor_split_down = d->st.editor_split_down; s.active_editor_group = d->st.active_editor_group;
    d->maximized_group = -1; d->overlay_panel = -1;
    return commit(d, &s);
}

/* ======================================================================== */
/* validation                                                                */
/* ======================================================================== */

/* Checks every invariant of a state against the registry: schema, panel ids in registry
 * order, region sizes, group order <-> group slots, each panel in at most one group, docked
 * panels in their group and non-docked ones in none, valid active tabs, weights, floating
 * geometry, editor group count. Writes the first problem to err and returns false. */
bool dock_validate(const Dock *d, const DockState *s, char *err, size_t errn) {
#define BAD(...) do { snprintf(err, errn, __VA_ARGS__); return false; } while (0)
    /* header and regions */
    if (s->schema != DOCK_SCHEMA_VERSION) BAD("unsupported schema version %d", s->schema);
    if (s->npanels != d->ndesc) BAD("panel count %d does not match the %d registered panels", s->npanels, d->ndesc);
    for (int r = 0; r < DOCK_REGIONS; r++) {
        if (!isfinite(s->region_size[r]) || s->region_size[r] < 20 || s->region_size[r] > REGION_MAX_SIZE) BAD("%s region size is not a valid number", region_names[r]);
        if (s->ngroups_in[r] < 0 || s->ngroups_in[r] > DOCK_MAX_GROUPS) BAD("bad group count in %s", region_names[r]);
        if (r == DOCK_TOP && !s->top_enabled && s->ngroups_in[r]) BAD("groups on the top edge while it is disabled");
        /* PNL-011: lanes contiguous from 0, each with a group, sane weights */
        int nl = dock_lanes(s, r); bool has[DOCK_MAX_LANES] = { 0 };
        for (int k = 0; k < s->ngroups_in[r]; k++) {
            int g = s->group_order[r][k];
            if (g < 0 || g >= DOCK_MAX_GROUPS) continue;   /* reported below */
            if (s->group[g].lane < 0 || s->group[g].lane >= DOCK_MAX_LANES) BAD("a group in %s has an invalid lane", region_names[r]);
            has[s->group[g].lane] = true;
        }
        for (int l = 0; l < nl; l++) {
            if (!has[l]) BAD("lane %d of %s is empty", l, region_names[r]);
            if (!isfinite(s->lane_weight[r][l]) || s->lane_weight[r][l] <= 0 || s->lane_weight[r][l] > 1e6) BAD("lane %d of %s has an invalid weight", l, region_names[r]);
        }
    }
    /* group_order must list each used group exactly once, in its own region */
    int seen_group[DOCK_MAX_GROUPS] = { 0 };
    for (int r = 0; r < DOCK_REGIONS; r++)
        for (int k = 0; k < s->ngroups_in[r]; k++) {
            int g = s->group_order[r][k];
            if (g < 0 || g >= DOCK_MAX_GROUPS || !s->group[g].used || s->group[g].region != r) BAD("group order of %s refers to a missing group", region_names[r]);
            if (seen_group[g]++) BAD("group %d appears twice", g);
        }
    /* groups: non-empty, sane weight, tabs refer to distinct panels, active tab among them */
    int owner[DOCK_MAX_PANELS]; for (int i = 0; i < DOCK_MAX_PANELS; i++) owner[i] = -1;
    for (int g = 0; g < DOCK_MAX_GROUPS; g++) {
        const DockGroup *gr = &s->group[g];
        if (!gr->used) continue;
        if (!seen_group[g]) BAD("group %d is not placed in any region", g);
        if (gr->ntabs < 1 || gr->ntabs > DOCK_MAX_TABS) BAD("empty or oversized group %d", g);
        if (!isfinite(gr->weight) || gr->weight <= 0 || gr->weight > 1e6) BAD("group %d has an invalid weight", g);
        bool active_ok = false;
        for (int i = 0; i < gr->ntabs; i++) {
            int p = gr->tabs[i];
            if (p < 0 || p >= s->npanels) BAD("group %d refers to an unknown panel", g);
            if (owner[p] >= 0) BAD("panel %s is in two groups (a cycle or a duplicate)", s->panel[p].id);
            owner[p] = g;
            if (p == gr->active) active_ok = true;
        }
        if (!active_ok) BAD("group %d has no valid active tab", g);
    }
    /* panels: identity, mode consistent with group membership, home region, float geometry */
    for (int p = 0; p < s->npanels; p++) {
        const DockPanelState *ps = &s->panel[p];
        if (strcmp(ps->id, d->desc[p].id) != 0) BAD("panel %d is \"%s\", expected \"%s\"", p, ps->id, d->desc[p].id);
        if (ps->mode < 0 || ps->mode >= DOCK_MODES) BAD("panel %s has an unknown mode", ps->id);
        if (ps->mode == DOCK_DOCKED && (owner[p] < 0 || ps->group != owner[p])) BAD("docked panel %s is not in its group", ps->id);
        if (ps->mode != DOCK_DOCKED && owner[p] >= 0) BAD("panel %s is %s but still in a group", ps->id, mode_names[ps->mode]);
        if (ps->home_region < 0 || ps->home_region >= DOCK_REGIONS) BAD("panel %s has an unknown home region", ps->id);
        if ((ps->mode == DOCK_FLOATING || ps->mode == DOCK_DETACHED) && !(isfinite(ps->float_x) && isfinite(ps->float_y) && isfinite(ps->float_w) && isfinite(ps->float_h) && ps->float_w > 0 && ps->float_h > 0)) BAD("floating geometry of %s is invalid", ps->id);
    }
    if (s->neditors < 1 || s->neditors > DOCK_MAX_EDITOR_GROUPS) BAD("there must be 1 to %d editor groups", DOCK_MAX_EDITOR_GROUPS);
    return true;
#undef BAD
}

/* ======================================================================== */
/* persistence                                                               */
/* ======================================================================== */

/* Growable output buffer for the JSON writer: s is malloc'd, n bytes used, cap allocated. */
typedef struct { char *s; size_t n, cap; } Out;
static void outf(Out *o, const char *fmt, ...) __attribute__((format(printf, 2, 3)));
/* printf into the buffer, growing it as needed (always NUL-terminated). */
static void outf(Out *o, const char *fmt, ...) {
    va_list ap;
    for (;;) {
        va_start(ap, fmt);
        int k = vsnprintf(o->s ? o->s + o->n : NULL, o->s ? o->cap - o->n : 0, fmt, ap);
        va_end(ap);
        if (k < 0) return;
        if (o->s && o->n + (size_t)k < o->cap) { o->n += (size_t)k; return; }
        o->cap = (o->cap + (size_t)k + 256) * 2; o->s = realloc(o->s, o->cap);
    }
}
/* Writes s as a quoted JSON string, escaping quotes, backslashes and control characters. */
static void out_str(Out *o, const char *s) {
    outf(o, "\"");
    for (; *s; s++) { if (*s == '"' || *s == '\\') outf(o, "\\%c", *s); else if ((unsigned char)*s < 0x20) outf(o, "\\u%04x", *s); else outf(o, "%c", *s); }
    outf(o, "\"");
}

/* Writes state s as the layout JSON (schemaVersion, layoutName, regionSize, regions with
 * their groups, panels, flags). Panels are written by id, never by index; editor tabs are
 * not written. layout_name NULL = "Current". Returns a malloc'd string the caller frees. */
char *dock_serialize(const Dock *d, const DockState *s, const char *layout_name) {
    Out o = { NULL, 0, 0 };
    outf(&o, "{\n  \"schemaVersion\": %d,\n  \"layoutName\": ", DOCK_SCHEMA_VERSION);
    out_str(&o, layout_name ? layout_name : "Current");
    outf(&o, ",\n  \"regionSize\": {");
    for (int r = 0; r < DOCK_REGIONS; r++) outf(&o, "%s\"%s\": %.1f", r ? ", " : "", region_names[r], s->region_size[r]);
    outf(&o, "},\n  \"laneWeight\": {");   /* PNL-011: share of each lane across its region */
    for (int r = 0; r < DOCK_REGIONS; r++) {
        int nl = dock_lanes(s, r); if (nl < 1) nl = 1;
        outf(&o, "%s\"%s\": [", r ? ", " : "", region_names[r]);
        for (int l = 0; l < nl; l++) outf(&o, "%s%.4f", l ? ", " : "", s->lane_weight[r][l]);
        outf(&o, "]");
    }
    outf(&o, "},\n  \"regions\": {\n");
    for (int r = 0; r < DOCK_REGIONS; r++) {
        outf(&o, "    \"%s\": [", region_names[r]);
        for (int k = 0; k < s->ngroups_in[r]; k++) {
            const DockGroup *gr = &s->group[s->group_order[r][k]];
            outf(&o, "%s{\"tabs\": [", k ? ", " : "");
            for (int i = 0; i < gr->ntabs; i++) { if (i) outf(&o, ", "); out_str(&o, s->panel[gr->tabs[i]].id); }
            outf(&o, "], \"active\": ");
            out_str(&o, gr->active >= 0 ? s->panel[gr->active].id : "");
            outf(&o, ", \"weight\": %.4f, \"collapsed\": %s, \"lane\": %d}", gr->weight, gr->collapsed ? "true" : "false", gr->lane);
        }
        outf(&o, "]%s\n", r + 1 < DOCK_REGIONS ? "," : "");
    }
    outf(&o, "  },\n  \"panels\": [\n");
    for (int p = 0; p < s->npanels; p++) {
        const DockPanelState *ps = &s->panel[p];
        outf(&o, "    {\"id\": "); out_str(&o, ps->id);
        outf(&o, ", \"mode\": \"%s\", \"home\": \"%s\", \"inBar\": %s, \"barOrder\": %d", mode_names[ps->mode], region_names[ps->home_region], ps->in_bar ? "true" : "false", ps->bar_order);
        if (ps->float_w > 0) outf(&o, ", \"float\": [%.0f, %.0f, %.0f, %.0f]", ps->float_x, ps->float_y, ps->float_w, ps->float_h);
        outf(&o, "}%s\n", p + 1 < s->npanels ? "," : "");
    }
    outf(&o, "  ],\n  \"focusMode\": %s,\n  \"topEnabled\": %s,\n  \"compact\": %s,\n  \"editorSplit\": \"%s\",\n  \"focusedPanel\": ",
         s->focus_mode ? "true" : "false", s->top_enabled ? "true" : "false", s->compact ? "true" : "false", s->editor_split_down ? "down" : "right");
    out_str(&o, s->focused_panel);
    outf(&o, "\n}\n");
    (void)d;
    return o.s;
}

/* ---- parsing helpers over json.c's flat node table ---- */
/* Child of object node obj with the given key, or -1. */
static int jmember(const Json *j, int obj, const char *key) {
    if (obj < 0 || j->nodes[obj].type != JN_OBJECT) return -1;
    size_t kl = strlen(key);
    for (int c = j->nodes[obj].first_child; c >= 0; c = j->nodes[c].next_sibling)
        if (j->nodes[c].key_len == kl && memcmp(j->src + j->nodes[c].key_off, key, kl) == 0) return c;
    return -1;
}
/* Reads number node n into *out; false when n is missing or not a number. */
static bool jnum(const Json *j, int n, double *out) {
    if (n < 0 || j->nodes[n].type != JN_NUMBER) return false;
    char buf[64]; size_t l = j->nodes[n].val_len < 63 ? j->nodes[n].val_len : 63;
    memcpy(buf, j->src + j->nodes[n].val_off, l); buf[l] = 0;
    char *end; *out = strtod(buf, &end);
    return *end == 0;
}
/* Unescapes string node n into buf (bn bytes); false when n is missing or not a string. */
static bool jstr(const Json *j, int n, char *buf, size_t bn) {
    if (n < 0 || j->nodes[n].type != JN_STRING) return false;
    json_unescape(j->src + j->nodes[n].val_off, j->nodes[n].val_len, buf, bn);
    return true;
}
/* Boolean node n, or def when missing or not a boolean. */
static bool jbool(const Json *j, int n, bool def) { return n < 0 ? def : j->nodes[n].type == JN_TRUE ? true : j->nodes[n].type == JN_FALSE ? false : def; }
/* Inverse of region_names / mode_names; -1 when unknown. */
static int region_by_name(const char *s) { for (int r = 0; r < DOCK_REGIONS; r++) if (!strcmp(s, region_names[r])) return r; return -1; }
/* Mode index of a mode name, -1 when unknown. */
static int mode_by_name(const char *s) { for (int m = 0; m < DOCK_MODES; m++) if (!strcmp(s, mode_names[m])) return m; return -1; }

/* Builds a DockState from layout JSON, tolerant of unknown or missing panels but strict on
 * malformed values; the result always passes dock_validate() or the call fails. The editor
 * part of *out is a single empty group: callers keep their live editors. */
bool dock_parse(const Dock *d, const char *text, size_t len, DockState *out, char *name, size_t name_n, char *err, size_t errn) {
    Json j;
    if (!json_parse(&j, text, len)) { snprintf(err, errn, "not valid JSON: %s", j.err); json_free(&j); return false; }
    DockState s;
    memset(&s, 0, sizeof s);
    bool ok = false;
#define BAD(...) do { snprintf(err, errn, __VA_ARGS__); goto done; } while (0)
    {
        int root = 0;
        double v;
        /* header: schema, name, global flags, region sizes */
        if (j.nodes[root].type != JN_OBJECT) BAD("the layout is not a JSON object");
        if (!jnum(&j, jmember(&j, root, "schemaVersion"), &v)) BAD("schemaVersion is missing");
        if ((int)v != DOCK_SCHEMA_VERSION) BAD("schemaVersion %d is not supported (expected %d)", (int)v, DOCK_SCHEMA_VERSION);
        s.schema = DOCK_SCHEMA_VERSION;
        if (name && name_n) { name[0] = 0; jstr(&j, jmember(&j, root, "layoutName"), name, name_n); }
        s.top_enabled = jbool(&j, jmember(&j, root, "topEnabled"), false);
        s.focus_mode = jbool(&j, jmember(&j, root, "focusMode"), false);
        s.compact = jbool(&j, jmember(&j, root, "compact"), false);
        char split[16] = "";
        jstr(&j, jmember(&j, root, "editorSplit"), split, sizeof split);
        s.editor_split_down = !strcmp(split, "down");
        jstr(&j, jmember(&j, root, "focusedPanel"), s.focused_panel, sizeof s.focused_panel);
        int rs = jmember(&j, root, "regionSize");
        for (int r = 0; r < DOCK_REGIONS; r++) {
            if (!jnum(&j, jmember(&j, rs, region_names[r]), &v)) v = default_region_size[r];
            if (!isfinite(v)) BAD("%s region size is not a finite number", region_names[r]);
            s.region_size[r] = (float)v;
        }
        /* PNL-011: lane weights (optional: files without lanes have one lane per region) */
        int lw = jmember(&j, root, "laneWeight");
        for (int r = 0; r < DOCK_REGIONS; r++) {
            for (int l = 0; l < DOCK_MAX_LANES; l++) s.lane_weight[r][l] = 1;
            int arr = jmember(&j, lw, region_names[r]);
            if (arr < 0) continue;
            if (j.nodes[arr].type != JN_ARRAY) BAD("lane weights of %s are not an array", region_names[r]);
            int l = 0;
            for (int e = j.nodes[arr].first_child; e >= 0 && l < DOCK_MAX_LANES; e = j.nodes[e].next_sibling, l++) {
                if (!jnum(&j, e, &v) || !isfinite(v) || v <= 0 || v > 1e6) BAD("a lane weight of %s is invalid", region_names[r]);
                s.lane_weight[r][l] = (float)v;
            }
        }
        /* panels in registry order; unknown ids are ignored (R-12) */
        s.npanels = d->ndesc;
        for (int p = 0; p < d->ndesc; p++) {
            snprintf(s.panel[p].id, sizeof s.panel[p].id, "%s", d->desc[p].id);
            s.panel[p].mode = -1; s.panel[p].group = -1; s.panel[p].home_region = d->desc[p].region; s.panel[p].in_bar = true; s.panel[p].bar_order = p;
        }
        int pa = jmember(&j, root, "panels");
        if (pa >= 0 && j.nodes[pa].type != JN_ARRAY) BAD("panels is not an array");
        for (int c = pa >= 0 ? j.nodes[pa].first_child : -1; c >= 0; c = j.nodes[c].next_sibling) {
            char id[64], mode[16] = "", home[16] = "";
            if (!jstr(&j, jmember(&j, c, "id"), id, sizeof id)) BAD("a panel entry has no id");
            int p = dock_find(d, id);
            if (p < 0) continue;
            if (s.panel[p].mode != -1) BAD("panel %s is listed twice", id);
            jstr(&j, jmember(&j, c, "mode"), mode, sizeof mode);
            int m = mode_by_name(mode);
            if (m < 0) BAD("panel %s has an unknown mode \"%s\"", id, mode);
            s.panel[p].mode = m == DOCK_DOCKED ? DOCK_HIDDEN : m;   /* docked again only when a group lists it */
            if (jstr(&j, jmember(&j, c, "home"), home, sizeof home)) { int r = region_by_name(home); if (r < 0) BAD("panel %s has an unknown home region", id); s.panel[p].home_region = r; }
            s.panel[p].in_bar = jbool(&j, jmember(&j, c, "inBar"), true);
            if (jnum(&j, jmember(&j, c, "barOrder"), &v)) s.panel[p].bar_order = (int)v;
            int fl = jmember(&j, c, "float");
            if (fl >= 0 && j.nodes[fl].type == JN_ARRAY && j.nodes[fl].nchildren == 4) {
                float g4[4]; int k = 0;
                for (int e = j.nodes[fl].first_child; e >= 0 && k < 4; e = j.nodes[e].next_sibling, k++) { if (!jnum(&j, e, &v) || !isfinite(v)) BAD("floating geometry of %s is invalid", id); g4[k] = (float)v; }
                s.panel[p].float_x = g4[0]; s.panel[p].float_y = g4[1]; s.panel[p].float_w = g4[2]; s.panel[p].float_h = g4[3];
            }
        }
        /* regions: rebuild each group from its tab ids; docking happens only here */
        int rg = jmember(&j, root, "regions");
        for (int r = 0; r < DOCK_REGIONS; r++) {
            int arr = jmember(&j, rg, region_names[r]);
            if (arr < 0) continue;
            if (j.nodes[arr].type != JN_ARRAY) BAD("region %s is not an array", region_names[r]);
            for (int c = j.nodes[arr].first_child; c >= 0; c = j.nodes[c].next_sibling) {
                int tabs = jmember(&j, c, "tabs");
                if (tabs < 0 || j.nodes[tabs].type != JN_ARRAY) BAD("a group in %s has no tabs", region_names[r]);
                int g = new_group(&s, r, -1);
                if (g < 0) BAD("too many groups");
                for (int t = j.nodes[tabs].first_child; t >= 0; t = j.nodes[t].next_sibling) {
                    char id[64];
                    if (!jstr(&j, t, id, sizeof id)) BAD("a tab id is not a string");
                    int p = dock_find(d, id);
                    if (p < 0) continue;                                   /* panel not installed */
                    if (s.panel[p].mode == DOCK_DOCKED) BAD("panel %s is in two groups (a cycle or a duplicate)", id);
                    if (s.group[g].ntabs >= DOCK_MAX_TABS) BAD("a group has too many tabs");
                    attach(&s, p, g, -1);
                }
                if (!s.group[g].ntabs) { drop_group(&s, g); continue; }   /* only unknown panels */
                char act[64] = "";
                jstr(&j, jmember(&j, c, "active"), act, sizeof act);
                int ap = dock_find(d, act);
                s.group[g].active = ap >= 0 && in_group(&s, ap, g) ? ap : s.group[g].tabs[0];
                if (jnum(&j, jmember(&j, c, "weight"), &v)) { if (!isfinite(v) || v <= 0) BAD("a group in %s has an invalid weight", region_names[r]); s.group[g].weight = (float)v; }
                s.group[g].collapsed = jbool(&j, jmember(&j, c, "collapsed"), false);
                if (jnum(&j, jmember(&j, c, "lane"), &v)) { if (!isfinite(v) || v < 0 || v >= DOCK_MAX_LANES || v != (int)v) BAD("a group in %s has an invalid lane", region_names[r]); s.group[g].lane = (int)v; }
            }
        }
        /* registered panels the file does not mention: default placement (new modules) */
        for (int p = 0; p < d->ndesc; p++) if (s.panel[p].mode == -1) place_default(d, &s, p);
        s.neditors = 1; s.editor[0].weight = 1; s.editor[0].active = -1;
        normalize_lanes(&s);   /* a lane whose panels are not installed (module off) goes away */
        if (!dock_validate(d, &s, err, errn)) goto done;
        *out = s;
        ok = true;
    }
done:
#undef BAD
    json_free(&j);
    return ok;
}

/* Serializes the current state and replaces `path` atomically (write path.tmp, fsync,
 * rename). On failure the old file is left as it was. */
bool dock_save_file(const Dock *d, const char *path, char *err, size_t errn) {
    char *txt = dock_serialize(d, &d->st, NULL);
    if (!txt) { snprintf(err, errn, "out of memory"); return false; }
    char tmp[1100];
    snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "w");
    if (!f) { snprintf(err, errn, "cannot write %s", tmp); free(txt); return false; }
    size_t n = strlen(txt);
    bool ok = fwrite(txt, 1, n, f) == n;
    ok = fflush(f) == 0 && ok;
    ok = fsync(fileno(f)) == 0 && ok;
    ok = fclose(f) == 0 && ok;
    free(txt);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); snprintf(err, errn, "could not replace %s", path); return false; }
    return true;
}

/* Reads and parses `path` (max 1 MB) and installs it, keeping the live editor groups and
 * clearing undo. A missing file returns false without renaming anything. */
bool dock_load_file(Dock *d, const char *path, char *err, size_t errn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(err, errn, "no saved layout"); return false; }
    fseek(f, 0, SEEK_END); long n = ftell(f); fseek(f, 0, SEEK_SET);
    if (n < 0 || n > (1 << 20)) { fclose(f); snprintf(err, errn, "layout file too large"); goto bad; }
    char *buf = malloc((size_t)n + 1);
    size_t got = fread(buf, 1, (size_t)n, f); buf[got] = 0;
    fclose(f);
    DockState s;
    bool ok = dock_parse(d, buf, got, &s, NULL, 0, err, errn);
    free(buf);
    if (ok) {
        memcpy(s.editor, d->st.editor, sizeof s.editor);
        s.neditors = d->st.neditors; s.active_editor_group = d->st.active_editor_group;
        d->st = s; d->dirty = false; d->nundo = 0; d->change_seq++;
        return true;
    }
bad:;
    /* keep the broken file for diagnosis; the caller starts from the default layout */
    char bak[1200];
    snprintf(bak, sizeof bak, "%s.bad-%ld", path, (long)time(NULL));
    rename(path, bak);
    size_t l = strlen(err);
    snprintf(err + l, errn - l, " (kept as %s)", bak);
    return false;
}

/* ======================================================================== */
/* geometry                                                                  */
/* ======================================================================== */

/* Fixed chrome sizes in points; the header is shorter in compact mode. */
DockMetrics dock_default_metrics(const Dock *d) {
    DockMetrics m;
    m.rail_w = 40; m.strip_h = 26; m.header_h = d->st.compact ? 26 : 32; m.splitter = 6;
    m.editor_min_w = 320; m.editor_min_h = 140;
    return m;
}

/* DockRect constructor that clamps negative sizes to 0. */
static DockRect rect(float x, float y, float w, float h) { DockRect r = { x, y, w > 0 ? w : 0, h > 0 ? h : 0 }; return r; }

/* A group takes room when it is used, expanded and not empty. */
static bool group_open(const DockState *s, int g) { return s->group[g].used && !s->group[g].collapsed && s->group[g].ntabs > 0; }
/* True when region r has at least one group taking room. */
static bool region_has_open(const DockState *s, int r) { for (int k = 0; k < s->ngroups_in[r]; k++) if (group_open(s, s->group_order[r][k])) return true; return false; }
/* True when region r has a collapsed group (so it needs a strip). */
static bool region_has_collapsed(const DockState *s, int r) { for (int k = 0; k < s->ngroups_in[r]; k++) if (s->group[s->group_order[r][k]].used && s->group[s->group_order[r][k]].collapsed) return true; return false; }
/* Most important (smallest) priority among the panels docked in region r; 1000 when empty.
 * Regions with larger values fold first when the window is too small. */
static int region_priority(const Dock *d, int r) {
    int best = 1000;
    for (int k = 0; k < d->st.ngroups_in[r]; k++) { const DockGroup *gr = &d->st.group[d->st.group_order[r][k]]; for (int i = 0; i < gr->ntabs; i++) if (d->desc[gr->tabs[i]].priority < best) best = d->desc[gr->tabs[i]].priority; }
    return best;
}

/* Smallest length a group can have along its region's axis: the largest minimum of its panels
 * (plus the header in stacked side regions). */
static float group_min_len(const Dock *d, int g, const DockMetrics *m) {
    const DockGroup *gr = &d->st.group[g];
    bool stack = side_region(gr->region);
    float mn = 60;
    for (int i = 0; i < gr->ntabs; i++) { float v = stack ? d->desc[gr->tabs[i]].min_h : d->desc[gr->tabs[i]].min_w; if (v > mn) mn = v; }
    return mn + (stack ? m->header_h : 0);
}

/* Splits `area` among the open groups of one lane of region r (lane < 0: every group) along the
 * region's axis: by weight, then every group is raised to its minimum and the others give way
 * above theirs. Splitters name the two groups they separate (group_order indices). */
static void lay_lane(Dock *d, int r, int lane, DockRect area, const DockMetrics *m, DockLayout *out) {
    const DockState *s = &d->st;
#define IN_LANE(g) (group_open(s, g) && (lane < 0 || s->group[g].lane == lane))
    float total = 0; int n = 0;
    for (int k = 0; k < s->ngroups_in[r]; k++) { int g = s->group_order[r][k]; if (IN_LANE(g)) { total += s->group[g].weight; n++; } }
    if (!n) return;
    bool stack = side_region(r);
    float len = (stack ? area.h : area.w) - m->splitter * (float)(n - 1);
    float pos = stack ? area.y : area.x;
    /* sizes by weight, then minimums */
    float want[DOCK_MAX_GROUPS] = { 0 }, mins[DOCK_MAX_GROUPS] = { 0 }, minsum = 0;
    for (int k = 0; k < s->ngroups_in[r]; k++) {
        int g = s->group_order[r][k];
        if (!IN_LANE(g)) continue;
        want[g] = len * s->group[g].weight / total; mins[g] = group_min_len(d, g, m); minsum += mins[g];
    }
    if (minsum >= len) { for (int g = 0; g < DOCK_MAX_GROUPS; g++) if (mins[g] > 0) want[g] = len * mins[g] / minsum; }
    else for (int pass = 0; pass < 4; pass++) {
        float deficit = 0, spare = 0;
        for (int g = 0; g < DOCK_MAX_GROUPS; g++) if (mins[g] > 0) { if (want[g] < mins[g]) deficit += mins[g] - want[g]; else spare += want[g] - mins[g]; }
        if (deficit <= 0.01f || spare <= 0) break;
        for (int g = 0; g < DOCK_MAX_GROUPS; g++) if (mins[g] > 0) {
            if (want[g] < mins[g]) want[g] = mins[g];
            else want[g] -= (want[g] - mins[g]) / spare * deficit;
        }
    }
    int done = 0, prev_k = -1;
    for (int k = 0; k < s->ngroups_in[r]; k++) {
        int g = s->group_order[r][k];
        if (!IN_LANE(g)) continue;
        float sz = done == n - 1 ? (stack ? area.y + area.h : area.x + area.w) - pos : want[g];
        out->group[g] = stack ? rect(area.x, pos, area.w, sz) : rect(pos, area.y, sz, area.h);
        out->group_shown[g] = true;
        if (prev_k >= 0 && out->nsplit < DOCK_MAX_SPLITTERS) {
            DockSplitter *sp = &out->split[out->nsplit++];
            sp->r = stack ? rect(area.x, pos - m->splitter, area.w, m->splitter) : rect(pos - m->splitter, area.y, m->splitter, area.h);
            sp->kind = 1; sp->region = r; sp->index = prev_k; sp->index2 = k; sp->vertical = !stack;
        }
        pos += sz + m->splitter; done++; prev_k = k;
    }
#undef IN_LANE
}

/* Lays out region r: with one lane (or one lane holding open groups) the whole area goes to
 * it; with more, the area is cut across the region's axis by lane weight (each lane raised to
 * its minimum), with a lane splitter between neighbours, then each lane is laid out. */
static void lay_groups(Dock *d, int r, DockRect area, const DockMetrics *m, DockLayout *out) {
    const DockState *s = &d->st;
    int nl = dock_lanes(s, r), open_lane[DOCK_MAX_LANES], no = 0;
    for (int l = 0; l < nl; l++) {
        bool any = false;
        for (int k = 0; k < s->ngroups_in[r]; k++) { int g = s->group_order[r][k]; if (group_open(s, g) && s->group[g].lane == l) any = true; }
        if (any) open_lane[no++] = l;
    }
    if (no <= 1) { lay_lane(d, r, no == 1 ? open_lane[0] : -1, area, m, out); return; }
    bool side = side_region(r);   /* left / right: lanes side by side; bottom / top: stacked */
    float len = (side ? area.w : area.h) - m->splitter * (float)(no - 1), total = 0, want[DOCK_MAX_LANES], mins[DOCK_MAX_LANES], minsum = 0;
    for (int i = 0; i < no; i++) total += s->lane_weight[r][open_lane[i]];
    for (int i = 0; i < no; i++) { want[i] = len * s->lane_weight[r][open_lane[i]] / total; mins[i] = lane_min_cross(d, s, r, open_lane[i], m); minsum += mins[i]; }
    if (minsum >= len) for (int i = 0; i < no; i++) want[i] = len * mins[i] / minsum;
    else for (int pass = 0; pass < 4; pass++) {
        float deficit = 0, spare = 0;
        for (int i = 0; i < no; i++) { if (want[i] < mins[i]) deficit += mins[i] - want[i]; else spare += want[i] - mins[i]; }
        if (deficit <= 0.01f || spare <= 0) break;
        for (int i = 0; i < no; i++) { if (want[i] < mins[i]) want[i] = mins[i]; else want[i] -= (want[i] - mins[i]) / spare * deficit; }
    }
    float pos = side ? area.x : area.y;
    for (int i = 0; i < no; i++) {
        float sz = i == no - 1 ? (side ? area.x + area.w : area.y + area.h) - pos : want[i];
        DockRect la = side ? rect(pos, area.y, sz, area.h) : rect(area.x, pos, area.w, sz);
        if (i > 0 && out->nsplit < DOCK_MAX_SPLITTERS) {
            DockSplitter *sp = &out->split[out->nsplit++];
            sp->r = side ? rect(pos - m->splitter, area.y, m->splitter, area.h) : rect(area.x, pos - m->splitter, area.w, m->splitter);
            sp->kind = 3; sp->region = r; sp->index = open_lane[i - 1]; sp->index2 = open_lane[i]; sp->vertical = side;
        }
        lay_lane(d, r, open_lane[i], la, m, out);
        pos += sz + m->splitter;
    }
}

/* Fills *out with every rectangle (points) for a workspace at (x, y) of w x h. Also records
 * w and h for later clamping and recomputes d->auto_collapsed. Order of the cut: rail on
 * the left, bottom across the full width, then left / right, then top, the remainder is the
 * center, which is split among the editor groups. */
void dock_layout(Dock *d, float x, float y, float w, float h, const DockMetrics *m, DockLayout *out) {
    memset(out, 0, sizeof *out);
    out->overlay_panel = -1;
    const DockState *s = &d->st;
    d->last_w = w; d->last_h = h;
    for (int r = 0; r < DOCK_REGIONS; r++) d->auto_collapsed[r] = false;

    float rail = s->focus_mode ? 0 : m->rail_w;
    out->rail = rect(x, y, rail, h);
    DockRect ws = rect(x + rail, y, w - rail, h);   /* workspace right of the rail */

    /* a maximized group takes the whole workspace; nothing else is laid out */
    if (d->maximized_group >= 0 && group_open(s, d->maximized_group)) {
        out->group[d->maximized_group] = ws; out->group_shown[d->maximized_group] = true;
        out->editor_hidden = true;
        return;
    }

    /* effective size of each region: saved size raised to its minimum (header included at
       bottom / top) */
    bool open[DOCK_REGIONS], strip[DOCK_REGIONS];
    float size[DOCK_REGIONS];
    for (int r = 0; r < DOCK_REGIONS; r++) {
        open[r] = !s->focus_mode && region_has_open(s, r);
        strip[r] = !s->focus_mode && region_has_collapsed(s, r);
        float lo = region_min(d, s, r) + (side_region(r) ? 0 : m->header_h);
        size[r] = s->region_size[r] > lo ? s->region_size[r] : lo;
    }
    /* PNL-038: when the editor's minimum does not fit, regions shrink to their minimum, then
     * the least important ones fold to their strip; saved sizes are never touched */
    for (int pass = 0; pass < 8; pass++) {
        float need_w = m->editor_min_w, need_h = m->editor_min_h;
        for (int r = 0; r < DOCK_REGIONS; r++) {
            float t = (open[r] ? size[r] + m->splitter : 0) + (strip[r] ? m->strip_h : 0);
            if (side_region(r)) need_w += t; else need_h += t;
        }
        if (need_w <= ws.w && need_h <= ws.h) break;
        bool horiz = need_w > ws.w;
        int cand[2] = { horiz ? DOCK_LEFT : DOCK_BOTTOM, horiz ? DOCK_RIGHT : DOCK_TOP };
        float over = horiz ? need_w - ws.w : need_h - ws.h;
        for (int c = 0; c < 2 && over > 0; c++) {
            int r = cand[c];
            if (!open[r]) continue;
            float lo = region_min(d, s, r) + (side_region(r) ? 0 : m->header_h);
            float take = size[r] - lo < over ? size[r] - lo : over;
            if (take > 0) { size[r] -= take; over -= take; }
        }
        if (over <= 0) continue;
        int pick = -1, pp = -1;
        for (int c = 0; c < 2; c++) { int r = cand[c]; if (open[r] && region_priority(d, r) >= pp) { pp = region_priority(d, r); pick = r; } }
        if (pick < 0) break;
        open[pick] = false; strip[pick] = true; d->auto_collapsed[pick] = true;
    }

    /* bottom spans the workspace width (spec 4.1); left / right / top share the rest */
    DockRect a = ws;
    if (strip[DOCK_BOTTOM]) { out->strip[DOCK_BOTTOM] = rect(a.x, a.y + a.h - m->strip_h, a.w, m->strip_h); a.h -= m->strip_h; }
    if (open[DOCK_BOTTOM]) {
        out->region[DOCK_BOTTOM] = rect(a.x, a.y + a.h - size[DOCK_BOTTOM], a.w, size[DOCK_BOTTOM]);
        DockSplitter *sp = &out->split[out->nsplit++];
        sp->r = rect(a.x, out->region[DOCK_BOTTOM].y - m->splitter, a.w, m->splitter); sp->kind = 0; sp->region = DOCK_BOTTOM; sp->vertical = false;
        a.h -= size[DOCK_BOTTOM] + m->splitter;
    }
    if (strip[DOCK_LEFT]) { out->strip[DOCK_LEFT] = rect(a.x, a.y, m->strip_h, a.h); a.x += m->strip_h; a.w -= m->strip_h; }
    if (strip[DOCK_RIGHT]) { out->strip[DOCK_RIGHT] = rect(a.x + a.w - m->strip_h, a.y, m->strip_h, a.h); a.w -= m->strip_h; }
    if (open[DOCK_LEFT]) {
        out->region[DOCK_LEFT] = rect(a.x, a.y, size[DOCK_LEFT], a.h);
        DockSplitter *sp = &out->split[out->nsplit++];
        sp->r = rect(a.x + size[DOCK_LEFT], a.y, m->splitter, a.h); sp->kind = 0; sp->region = DOCK_LEFT; sp->vertical = true;
        a.x += size[DOCK_LEFT] + m->splitter; a.w -= size[DOCK_LEFT] + m->splitter;
    }
    if (open[DOCK_RIGHT]) {
        out->region[DOCK_RIGHT] = rect(a.x + a.w - size[DOCK_RIGHT], a.y, size[DOCK_RIGHT], a.h);
        DockSplitter *sp = &out->split[out->nsplit++];
        sp->r = rect(out->region[DOCK_RIGHT].x - m->splitter, a.y, m->splitter, a.h); sp->kind = 0; sp->region = DOCK_RIGHT; sp->vertical = true;
        a.w -= size[DOCK_RIGHT] + m->splitter;
    }
    if (strip[DOCK_TOP]) { out->strip[DOCK_TOP] = rect(a.x, a.y, a.w, m->strip_h); a.y += m->strip_h; a.h -= m->strip_h; }
    if (open[DOCK_TOP]) {
        out->region[DOCK_TOP] = rect(a.x, a.y, a.w, size[DOCK_TOP]);
        DockSplitter *sp = &out->split[out->nsplit++];
        sp->r = rect(a.x, a.y + size[DOCK_TOP], a.w, m->splitter); sp->kind = 0; sp->region = DOCK_TOP; sp->vertical = false;
        a.y += size[DOCK_TOP] + m->splitter; a.h -= size[DOCK_TOP] + m->splitter;
    }
    out->center = a;
    for (int r = 0; r < DOCK_REGIONS; r++) if (open[r]) lay_groups(d, r, out->region[r], m, out);

    /* editor groups split the center */
    int ne = s->neditors < 1 ? 1 : s->neditors;
    float etotal = 0; for (int i = 0; i < ne; i++) etotal += s->editor[i].weight > 0 ? s->editor[i].weight : 1;
    bool down = s->editor_split_down;
    float elen = (down ? a.h : a.w) - m->splitter * (float)(ne - 1), epos = down ? a.y : a.x;
    for (int i = 0; i < ne; i++) {
        float wgt = s->editor[i].weight > 0 ? s->editor[i].weight : 1;
        float sz = i == ne - 1 ? (down ? a.y + a.h : a.x + a.w) - epos : elen * wgt / etotal;
        out->editor[i] = down ? rect(a.x, epos, a.w, sz) : rect(epos, a.y, sz, a.h);
        if (i > 0) {
            DockSplitter *sp = &out->split[out->nsplit++];
            sp->r = down ? rect(a.x, epos - m->splitter, a.w, m->splitter) : rect(epos - m->splitter, a.y, m->splitter, a.h);
            sp->kind = 2; sp->region = DOCK_CENTER; sp->index = i - 1; sp->vertical = !down;
        }
        epos += sz + m->splitter;
    }
    out->neditors = ne;

    /* overlay (auto-hide, focus mode): anchored to the panel's region edge, over the editor */
    if (d->overlay_panel >= 0 && d->overlay_panel < d->ndesc) {
        int p = d->overlay_panel, r = dock_region_of(d, p);
        float sz = s->region_size[r];
        DockRect o = ws;
        if (r == DOCK_LEFT) o = rect(ws.x, ws.y, fminf(sz, ws.w * 0.8f), ws.h);
        else if (r == DOCK_RIGHT) o = rect(ws.x + ws.w - fminf(sz, ws.w * 0.8f), ws.y, fminf(sz, ws.w * 0.8f), ws.h);
        else if (r == DOCK_BOTTOM) o = rect(ws.x, ws.y + ws.h - fminf(sz, ws.h * 0.8f), ws.w, fminf(sz, ws.h * 0.8f));
        else o = rect(ws.x, ws.y, ws.w, fminf(sz, ws.h * 0.8f));
        out->overlay = o; out->overlay_panel = p;
    }
}

/* ======================================================================== */
/* drag and drop                                                             */
/* ======================================================================== */

/* Point-in-rect test (half-open on the right and bottom edges). */
static bool inside(DockRect r, float x, float y) { return x >= r.x && y >= r.y && x < r.x + r.w && y < r.y + r.h; }

/* Hit test for a panel drag at (mx, my), points, against the layout of the last frame. In
 * order: a 28-point band at the workspace edges, the body / header of a visible group, then
 * the editor area (nearest edge). The preview approximates where the panel would land. */
DockDrop dock_drop_target(Dock *d, const DockLayout *lay, const DockMetrics *m, int p, float mx, float my) {
    DockDrop t; memset(&t, 0, sizeof t);
    if (p < 0 || p >= d->ndesc || !(d->desc[p].caps & DOCK_CAP_MOVE)) return t;
    const DockState *s = &d->st;
    int own = dock_group_of(d, p);
    DockRect ws = rect(lay->rail.x + lay->rail.w, lay->rail.y, d->last_w - lay->rail.w, d->last_h);
    float band = 28;
    /* 1. outer edges of the workspace: a new group in that region */
    int edge = -1;
    if (mx < ws.x + band) edge = DOCK_LEFT;
    else if (mx > ws.x + ws.w - band) edge = DOCK_RIGHT;
    else if (my > ws.y + ws.h - band) edge = DOCK_BOTTOM;
    else if (s->top_enabled && my < ws.y + band) edge = DOCK_TOP;
    /* 2. over a visible group: centre / header = tab, ends along the region axis = split */
    if (edge < 0) for (int g = 0; g < DOCK_MAX_GROUPS; g++) {
        if (!lay->group_shown[g] || !inside(lay->group[g], mx, my)) continue;
        DockRect gr = lay->group[g];
        bool stack = side_region(s->group[g].region);
        float along = stack ? (my - gr.y) / gr.h : (mx - gr.x) / gr.w;
        bool in_header = my < gr.y + m->header_h;
        bool alone_here = own == g && s->group[g].ntabs == 1;
        if (!in_header && (along < 0.25f || along > 0.75f) && !alone_here) {
            t.kind = DOCK_DROP_SPLIT; t.group = g; t.after = along > 0.75f;
            t.preview = stack ? rect(gr.x, t.after ? gr.y + gr.h / 2 : gr.y, gr.w, gr.h / 2) : rect(t.after ? gr.x + gr.w / 2 : gr.x, gr.y, gr.w / 2, gr.h);
            return t;
        }
        /* PNL-011: the sides across the region's axis: a new lane beside this group */
        float across = stack ? (mx - gr.x) / gr.w : (my - gr.y) / gr.h;
        if (!in_header && (across < 0.22f || across > 0.78f) && dock_lanes(s, s->group[g].region) < DOCK_MAX_LANES) {
            int lane = s->group[g].lane, in_lane = 0;
            for (int k = 0; k < s->ngroups_in[s->group[g].region]; k++) if (s->group[s->group_order[s->group[g].region][k]].lane == lane) in_lane++;
            if (!(alone_here && in_lane == 1)) {
                t.kind = DOCK_DROP_SPLIT; t.cross = true; t.group = g; t.after = across > 0.78f;
                t.preview = stack ? rect(t.after ? gr.x + gr.w / 2 : gr.x, gr.y, gr.w / 2, gr.h) : rect(gr.x, t.after ? gr.y + gr.h / 2 : gr.y, gr.w, gr.h / 2);
                return t;
            }
        }
        if (own == g && in_header) {     /* reorder inside the same group */
            t.kind = DOCK_DROP_TAB; t.group = g; t.index = -1; t.preview = gr; return t;
        }
        if (own == g) return t;          /* dropping on its own group body changes nothing */
        if (s->group[g].ntabs >= DOCK_MAX_TABS) return t;
        t.kind = DOCK_DROP_TAB; t.group = g; t.index = -1; t.preview = gr;
        return t;
    }
    /* 3. over the editor: the nearest edge of the editor area becomes the region */
    if (edge < 0 && inside(lay->center, mx, my)) {
        DockRect c = lay->center;
        float dl = mx - c.x, dr = c.x + c.w - mx, db = c.y + c.h - my, dt = my - c.y;
        edge = DOCK_LEFT; float best = dl;
        if (dr < best) { best = dr; edge = DOCK_RIGHT; }
        if (db < best) { best = db; edge = DOCK_BOTTOM; }
        if (s->top_enabled && dt < best) { edge = DOCK_TOP; }
    }
    if (edge < 0) return t;
    if (edge == DOCK_TOP && !s->top_enabled) return t;
    /* preview with the region's saved size, never leaving the editor under its minimum */
    float sz = s->region_size[edge];
    float room = side_region(edge) ? lay->center.w - m->editor_min_w : lay->center.h - m->editor_min_h;
    bool region_already_open = lay->region[edge].w > 0 && lay->region[edge].h > 0;
    if (!region_already_open && room < region_min(d, s, edge)) return t;   /* would crush the editor */
    if (!region_already_open && sz > room) sz = room;
    t.kind = DOCK_DROP_REGION; t.region = edge;
    if (region_already_open) t.preview = lay->region[edge];
    else if (edge == DOCK_LEFT) t.preview = rect(lay->center.x, lay->center.y, sz, lay->center.h);
    else if (edge == DOCK_RIGHT) t.preview = rect(lay->center.x + lay->center.w - sz, lay->center.y, sz, lay->center.h);
    else if (edge == DOCK_BOTTOM) t.preview = rect(ws.x, ws.y + ws.h - sz, ws.w, sz);
    else t.preview = rect(lay->center.x, lay->center.y, lay->center.w, sz);
    return t;
}

/* Performs the operation a drop target stands for (move to region, join group, split). */
bool dock_apply_drop(Dock *d, int p, const DockDrop *t) {
    switch (t->kind) {
    case DOCK_DROP_REGION: return dock_move_to_region(d, p, t->region);
    case DOCK_DROP_TAB: return dock_move_to_group(d, p, t->group, t->index);
    case DOCK_DROP_SPLIT: return t->cross ? dock_split_lane(d, p, t->group, t->after) : dock_split_group(d, p, t->group, t->after);
    default: fail(d, "no target"); return false;
    }
}
