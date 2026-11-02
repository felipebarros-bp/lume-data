/* SPDX-License-Identifier: AGPL-3.0-only */
/* Headless checks of the docking engine (LUME-PANEL-001, NFR-09 / PNL-044): every layout
 * operation, persistence, validation, geometry and drop targets, without SDL. */
#include "dock.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <unistd.h>
#include <time.h>

static int failures = 0;
static char err[200];
static void check(bool ok, const char *what, const char *detail) {
    printf("%s  %s%s%s\n", ok ? "ok  " : "FAIL", what, detail && *detail ? ": " : "", detail ? detail : "");
    if (!ok) failures++;
    err[0] = 0;   /* a stale reason must not decorate the next line */
}

/* the Lume Data panels, as the shell registers them */
static void register_lume(Dock *d) {
    DockPanelDesc ex = { .id = "lume.data.explorer", .title = "Explorer", .icon = "db", .region = DOCK_LEFT, .min_w = 160, .min_h = 120, .pref_size = 250, .priority = 2 };
    DockPanelDesc rs = { .id = "lume.data.results", .title = "Results", .icon = "grid", .region = DOCK_BOTTOM, .min_w = 240, .min_h = 120, .pref_size = 260, .priority = 1 };
    DockPanelDesc out = { .id = "lume.data.output", .title = "Output", .icon = "log", .region = DOCK_BOTTOM, .min_w = 240, .min_h = 100, .pref_size = 260, .priority = 1 };
    DockPanelDesc in = { .id = "lume.data.inspector", .title = "Inspector", .icon = "info", .region = DOCK_RIGHT, .min_w = 200, .min_h = 120, .pref_size = 300, .priority = 3, .start_hidden = true };
    dock_register(d, &ex); dock_register(d, &rs); dock_register(d, &out); dock_register(d, &in);
}

int main(void) {
    Dock d; dock_init(&d); register_lume(&d);
    int EX = dock_find(&d, "lume.data.explorer"), RS = dock_find(&d, "lume.data.results"), OUT = dock_find(&d, "lume.data.output"), IN = dock_find(&d, "lume.data.inspector");
    DockMetrics m = dock_default_metrics(&d);
    DockLayout L;

    /* PNL-001 / 002: default regions and the registry */
    check(dock_region_of(&d, EX) == DOCK_LEFT && dock_is_visible(&d, EX), "Explorer starts on the left, visible", NULL);
    check(dock_group_of(&d, RS) >= 0 && dock_group_of(&d, RS) == dock_group_of(&d, OUT) && d.st.group[dock_group_of(&d, RS)].active == RS, "Results and Output share the bottom group, Results in front", NULL);
    check(d.st.panel[IN].mode == DOCK_HIDDEN, "Inspector starts hidden", NULL);
    DockPanelDesc dup = { .id = "lume.data.results", .title = "dup" };
    check(!dock_register(&d, &dup) && strstr(d.err, "duplicate"), "duplicate panel id rejected with a diagnosis", d.err);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.region[DOCK_RIGHT].w == 0 && L.region[DOCK_LEFT].w > 0 && L.region[DOCK_BOTTOM].h > 0, "an empty region takes no room", NULL);
    check(L.center.w >= m.editor_min_w && L.center.h >= m.editor_min_h, "the editor keeps its minimum", NULL);

    /* PNL-003: hide / show restores group, tab and size */
    float bottom_before = d.st.region_size[DOCK_BOTTOM];
    int g_out = dock_group_of(&d, OUT);
    dock_activate(&d, OUT);
    check(dock_hide(&d, OUT) && !dock_is_visible(&d, OUT) && d.st.panel[OUT].mode == DOCK_HIDDEN && dock_group_of(&d, RS) == g_out, "hiding Output keeps Results' group", NULL);
    check(dock_show(&d, OUT) && dock_group_of(&d, OUT) == g_out && dock_is_visible(&d, OUT) && d.st.region_size[DOCK_BOTTOM] == bottom_before, "showing Output returns to the same group, active, same size", NULL);

    /* PNL-004: collapse the bottom group, strip appears, size kept */
    int gb = dock_group_of(&d, RS);
    check(dock_collapse_group(&d, gb) && dock_is_collapsed(&d, RS) && dock_is_collapsed(&d, OUT), "collapse folds the whole group", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.region[DOCK_BOTTOM].h == 0 && L.strip[DOCK_BOTTOM].h > 0 && L.center.h > 600, "collapsed: no region, a strip at the bottom, the editor gains the room", NULL);
    check(dock_expand_group(&d, gb) && d.st.region_size[DOCK_BOTTOM] == bottom_before, "expand restores the last size", NULL);

    /* PNL-005: switching tabs changes only the active tab of that group */
    int g_ex = dock_group_of(&d, EX);
    dock_activate(&d, RS);
    check(d.st.group[gb].active == RS && d.st.group[g_ex].active == EX, "activating a tab touches only its group", NULL);

    /* PNL-008 / 026: move Output to the right edge, Results stays, then back as a tab */
    check(dock_move_to_region(&d, OUT, DOCK_RIGHT) && dock_region_of(&d, OUT) == DOCK_RIGHT && dock_group_of(&d, RS) == gb, "Output moved to the right in its own group; Results untouched", NULL);
    check(!dock_move_to_region(&d, OUT, DOCK_TOP) && strstr(d.err, "top"), "top edge refused while disabled, with the reason", d.err);
    check(dock_move_to_group(&d, OUT, gb, -1) && dock_group_of(&d, OUT) == gb && d.st.ngroups_in[DOCK_RIGHT] == 0, "Output back as a tab; the emptied right group is removed", NULL);

    /* PNL-010: reorder keeps the active tab */
    int act = d.st.group[gb].active;
    check(dock_reorder_tab(&d, gb, 1, 0) && d.st.group[gb].tabs[0] == OUT && d.st.group[gb].active == act, "reorder changes only the order", NULL);

    /* PNL-011: split the bottom into two groups side by side */
    check(dock_split_group(&d, OUT, gb, true) && d.st.ngroups_in[DOCK_BOTTOM] == 2, "split creates a second group in the region", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    int g1 = d.st.group_order[DOCK_BOTTOM][0], g2 = d.st.group_order[DOCK_BOTTOM][1];
    check(L.group_shown[g1] && L.group_shown[g2] && L.group[g1].x < L.group[g2].x && fabsf(L.group[g1].y - L.group[g2].y) < 0.1f, "the two groups sit side by side", NULL);
    bool sp_inside = false; for (int i = 0; i < L.nsplit; i++) if (L.split[i].kind == 1 && L.split[i].region == DOCK_BOTTOM) sp_inside = true;
    check(sp_inside, "a splitter between them", NULL);
    check(L.group[g2].w >= d.desc[OUT].min_w - 0.5f && L.group[g1].w >= d.desc[RS].min_w - 0.5f, "each group of the split keeps its panels' minimum width", NULL);
    d.st.group[g2].weight = 0.05f;   /* a saved weight that would crush the Output */
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.group[g2].w >= d.desc[OUT].min_w - 0.5f, "a tiny saved weight is overridden by the minimum", NULL);
    d.st.group[g2].weight = 1;
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    float w1 = d.st.group[g1].weight;
    dock_set_group_weight(&d, DOCK_BOTTOM, 0, 100);
    check(d.st.group[g1].weight > w1, "the splitter between groups resizes them", NULL);
    check(dock_move_to_group(&d, OUT, g1, -1) && d.st.ngroups_in[DOCK_BOTTOM] == 1, "joining the last tab removes the split", NULL);
    gb = dock_group_of(&d, RS);

    /* PNL-013 / 015: region size clamped by the editor minimum */
    dock_set_region_size(&d, DOCK_LEFT, 5000);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.center.w >= m.editor_min_w - 0.5f, "a huge left size never crushes the editor", NULL);
    dock_set_region_size(&d, DOCK_LEFT, 10);
    check(d.st.region_size[DOCK_LEFT] >= 160, "a tiny size stops at the panel minimum", NULL);
    check(dock_reset_region_size(&d, DOCK_LEFT) && d.st.region_size[DOCK_LEFT] == 250, "double-click on the splitter: preferred size", NULL);

    /* PNL-014: maximize / restore is transient */
    DockState before = d.st;
    check(dock_toggle_maximize(&d, gb), "maximize the bottom group", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.editor_hidden && L.group[gb].w > 1200 && L.group[gb].h > 700 && !L.group_shown[g_ex], "maximized group takes the workspace", NULL);
    char *s1 = dock_serialize(&d, &d.st, NULL), *s0 = dock_serialize(&d, &before, NULL);
    check(strcmp(s1, s0) == 0, "maximize changes nothing that is saved", NULL);
    free(s1); free(s0);
    dock_toggle_maximize(&d, gb);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(!L.editor_hidden && L.group_shown[g_ex] && L.group_shown[gb], "restore brings the layout back", NULL);

    /* PNL-035 / Scenario 4: focus mode and exact restore */
    dock_show(&d, IN);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    DockRect r_ex = L.group[g_ex], r_b = L.group[gb], r_in = L.group[dock_group_of(&d, IN)];
    check(dock_toggle_focus_mode(&d), "focus mode on", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    bool none = true; for (int g = 0; g < DOCK_MAX_GROUPS; g++) if (L.group_shown[g]) none = false;
    check(none && L.rail.w == 0 && L.center.w == 1280 && L.center.h == 760, "focus mode: only the editor", NULL);
    check(dock_show(&d, RS) && d.overlay_panel == RS && d.st.focus_mode, "opening a panel in focus mode shows an overlay, focus mode stays", NULL);
    dock_close_overlay(&d);
    dock_toggle_focus_mode(&d);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(!memcmp(&r_ex, &L.group[g_ex], sizeof r_ex) && !memcmp(&r_b, &L.group[gb], sizeof r_b) && !memcmp(&r_in, &L.group[dock_group_of(&d, IN)], sizeof r_in), "focus mode off: the three panels return to the same rectangles", NULL);
    dock_hide(&d, IN);

    /* PNL-017 / 018: auto-hide shows an overlay without squeezing the editor */
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    DockRect c0 = L.center;
    check(dock_set_mode(&d, EX, DOCK_AUTOHIDE) && dock_show(&d, EX) && d.overlay_panel == EX, "auto-hide: opening shows an overlay", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.overlay_panel == EX && L.overlay.x == L.rail.w && L.center.x < c0.x, "overlay anchored at the left edge; the editor is not squeezed by it", NULL);
    dock_close_overlay(&d);
    check(dock_set_mode(&d, EX, DOCK_DOCKED) && dock_region_of(&d, EX) == DOCK_LEFT && dock_is_visible(&d, EX), "back to docked in its home region", NULL);
    g_ex = dock_group_of(&d, EX);

    /* PNL-019 / 020 / 021: floating and detached leave the layout, closing docks back home */
    int home_g = dock_group_of(&d, OUT);
    check(dock_set_mode(&d, OUT, DOCK_FLOATING) && dock_group_of(&d, OUT) < 0 && d.st.panel[OUT].float_w > 0, "Float: Output leaves its group, keeps a window geometry", NULL);
    check(dock_set_mode(&d, OUT, DOCK_DETACHED) && d.st.panel[OUT].mode == DOCK_DETACHED, "Detach: a separate window", NULL);
    char *fj = dock_serialize(&d, &d.st, NULL); DockState fs;
    check(strstr(fj, "\"mode\": \"detached\"") && strstr(fj, "\"float\": [") && dock_parse(&d, fj, strlen(fj), &fs, NULL, 0, err, sizeof err) && fs.panel[OUT].mode == DOCK_DETACHED, "detached mode and its geometry persist", err);
    free(fj);
    check(dock_set_mode(&d, OUT, DOCK_DOCKED) && dock_group_of(&d, OUT) == home_g, "closing the window docks Output back into its group", NULL);

    /* PNL-006: bar button and order */
    check(dock_set_in_bar(&d, OUT, false) && !d.st.panel[OUT].in_bar && dock_group_of(&d, OUT) >= 0, "unpin from the bar: the panel still exists", NULL);
    dock_set_in_bar(&d, OUT, true);
    check(dock_move_bar_button(&d, IN, 0) && d.st.panel[IN].bar_order == 0, "bar button moved first", NULL);

    /* PNL-045: undo */
    int before_region = dock_region_of(&d, OUT);
    dock_move_to_region(&d, OUT, DOCK_LEFT);
    check(dock_undo(&d) && dock_region_of(&d, OUT) == before_region, "undo restores the panel's previous place", NULL);

    /* PNL-038: small window folds the right / left regions, not the bottom first */
    dock_show(&d, IN);
    dock_layout(&d, 0, 0, 760, 500, &m, &L);
    check(!d.auto_collapsed[DOCK_RIGHT] && L.center.w >= m.editor_min_w - 0.5f, "760 pt: regions shrink to their minimum first, nothing folds", NULL);
    dock_layout(&d, 0, 0, 640, 500, &m, &L);
    check(L.center.w >= m.editor_min_w - 0.5f && L.center.h >= m.editor_min_h - 0.5f, "640 pt: the editor keeps its minimum", NULL);
    check(d.auto_collapsed[DOCK_RIGHT] && !d.auto_collapsed[DOCK_LEFT] && L.strip[DOCK_RIGHT].w > 0, "640 pt: the least important region (Inspector) folds to its strip, Explorer stays", NULL);
    dock_layout(&d, 0, 0, 1600, 1000, &m, &L);
    check(!d.auto_collapsed[DOCK_RIGHT] && L.region[DOCK_RIGHT].w > 0, "larger window: it comes back, sizes untouched", NULL);
    dock_hide(&d, IN);

    /* PNL-007 / 009 / 012: drop targets */
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    DockDrop t = dock_drop_target(&d, &L, &m, OUT, 1270, 300);
    check(t.kind == DOCK_DROP_REGION && t.region == DOCK_RIGHT && t.preview.w > 0, "right edge: a new right region with a preview", NULL);
    t = dock_drop_target(&d, &L, &m, OUT, L.group[g_ex].x + L.group[g_ex].w / 2, L.group[g_ex].y + L.group[g_ex].h / 2);
    check(t.kind == DOCK_DROP_TAB && t.group == g_ex, "centre of the Explorer group: join as a tab", NULL);
    t = dock_drop_target(&d, &L, &m, OUT, L.group[g_ex].x + 50, L.group[g_ex].y + L.group[g_ex].h - 20);
    check(t.kind == DOCK_DROP_SPLIT && t.group == g_ex && t.after, "lower end of the Explorer group: split below", NULL);
    t = dock_drop_target(&d, &L, &m, RS, L.center.x + L.center.w - 10, L.center.y + L.center.h / 2);
    check(t.kind == DOCK_DROP_REGION && t.region == DOCK_RIGHT, "over the editor near its right edge: right region", NULL);
    /* Scenario 3: a drag cancelled with Esc applies nothing (the shell never calls apply) */
    char *snap = dock_serialize(&d, &d.st, NULL);
    t = dock_drop_target(&d, &L, &m, EX, 1270, 300);
    char *after_hover = dock_serialize(&d, &d.st, NULL);
    check(strcmp(snap, after_hover) == 0, "computing drop targets while dragging changes nothing (Esc = no apply)", NULL);
    free(snap); free(after_hover);
    check(dock_apply_drop(&d, OUT, &(DockDrop){ .kind = DOCK_DROP_REGION, .region = DOCK_RIGHT }) && dock_region_of(&d, OUT) == DOCK_RIGHT, "Scenario 2: dropping Output on the right edge", NULL);
    dock_undo(&d);

    /* PNL-031 / 034: presets keep editor groups, apply arrangement only */
    int ed1 = 101, ed2 = 102;
    dock_editor_add_tab(&d, 0, ed1); dock_editor_add_tab(&d, 0, ed2);
    check(dock_apply_preset(&d, DOCK_PRESET_DATA_EXPLORATION) && dock_is_visible(&d, IN) && d.st.region_size[DOCK_BOTTOM] == 420 && dock_editor_group_of(&d, ed2) == 0, "Data Exploration: Inspector on the right, large Results, editors kept", NULL);
    check(dock_apply_preset(&d, DOCK_PRESET_QUERY_FOCUS) && dock_is_collapsed(&d, EX) && !dock_is_visible(&d, IN), "Query Focus: Explorer collapsed, Inspector closed", NULL);
    check(dock_apply_preset(&d, DOCK_PRESET_DEFAULT) && dock_is_visible(&d, EX) && d.st.panel[IN].mode == DOCK_HIDDEN && d.st.region_size[DOCK_BOTTOM] == 260, "Default restored", NULL);

    /* PNL-027 / 028: editor groups */
    check(dock_editor_split(&d, ed2, false) && d.st.neditors == 2 && dock_editor_group_of(&d, ed2) == 1, "Split Right moves the tab to a new editor group", NULL);
    dock_layout(&d, 0, 0, 1280, 760, &m, &L);
    check(L.neditors == 2 && L.editor[0].x < L.editor[1].x, "two editor groups side by side", NULL);
    check(dock_editor_move_tab(&d, ed2, 0, -1) && d.st.neditors == 1, "moving the last tab back removes the empty editor group", NULL);
    {   /* a move into a full editor group is refused and leaves the tab where it was */
        for (int i = d.st.editor[0].ntabs; i < DOCK_MAX_EDITOR_TABS; i++) dock_editor_add_tab(&d, 0, 200 + i);
        int ed3 = 300; dock_editor_add_tab(&d, 0, ed3);   /* group 0 full: lands in a new group or is refused */
        if (dock_editor_group_of(&d, ed3) < 0) { dock_editor_close_tab(&d, 200 + DOCK_MAX_EDITOR_TABS - 1); dock_editor_add_tab(&d, 0, ed3); }
        bool split = dock_editor_split(&d, ed3, false);
        for (int i = d.st.editor[0].ntabs; i < DOCK_MAX_EDITOR_TABS; i++) dock_editor_add_tab(&d, 0, 400 + i);
        int before = d.st.editor[1].ntabs;
        bool moved = dock_editor_move_tab(&d, ed3, 0, -1);
        check(split && d.st.editor[0].ntabs == DOCK_MAX_EDITOR_TABS && !moved && dock_editor_group_of(&d, ed3) == 1 && d.st.editor[1].ntabs == before && strstr(d.err, "full"),
              "moving a tab into a full editor group is refused and the tab stays in its group", d.err);
        for (int i = 0; i < 2 * DOCK_MAX_EDITOR_TABS; i++) { dock_editor_close_tab(&d, 200 + i); dock_editor_close_tab(&d, 400 + i); }
        dock_editor_close_tab(&d, ed3);
    }
    check(dock_editor_close_tab(&d, ed1) && !dock_editor_close_tab(&d, ed2) && strstr(d.err, "last editor"), "the last editor cannot be closed", d.err);

    /* PNL-030 / 032: persistence round trip and every kind of corruption */
    dock_move_to_region(&d, OUT, DOCK_RIGHT);
    dock_collapse_group(&d, dock_group_of(&d, EX));
    char *json = dock_serialize(&d, &d.st, "Mine");
    DockState back; char name[64];
    check(dock_parse(&d, json, strlen(json), &back, name, sizeof name, err, sizeof err) && !strcmp(name, "Mine"), "serialized layout parses back", err);
    char *json2 = dock_serialize(&d, &back, "Mine");
    check(strcmp(json, json2) == 0, "round trip is exact", NULL);
    check(!strstr(json, "SELECT") && !strstr(json, "password") && !strstr(json, "0x"), "the file holds ids, modes and sizes only (no SQL, secrets or pointers)", NULL);
    free(json2);
    struct { const char *what, *from, *to; } bad[] = {
        { "NaN size", "\"left\": 250.0", "\"left\": NaN" },
        { "infinite weight", "\"weight\": 1.0000", "\"weight\": 1e999" },
        { "schema version", "\"schemaVersion\": 1", "\"schemaVersion\": 99" },
        { "panel in two groups", "\"tabs\": [\"lume.data.output\"]", "\"tabs\": [\"lume.data.output\", \"lume.data.results\"]" },
        { "truncated JSON", "\"focusMode\"", "\"focusMode" },
        { "unknown mode", "\"mode\": \"hidden\"", "\"mode\": \"teleported\"" },
    };
    for (size_t i = 0; i < sizeof bad / sizeof bad[0]; i++) {
        char *c = strdup(json); char *at = strstr(c, bad[i].from);
        if (!at) { printf("      (pattern not found for %s)\n", bad[i].what); check(false, bad[i].what, "test pattern missing"); free(c); continue; }
        size_t nl = strlen(c) - strlen(bad[i].from) + strlen(bad[i].to) + 1;
        char *mut = malloc(nl);
        snprintf(mut, nl, "%.*s%s%s", (int)(at - c), c, bad[i].to, at + strlen(bad[i].from));
        DockState junk; err[0] = 0;
        bool parsed = dock_parse(&d, mut, strlen(mut), &junk, NULL, 0, err, sizeof err);
        char what[96]; snprintf(what, sizeof what, "corrupt layout rejected: %s", bad[i].what);
        check(!parsed && err[0], what, err);
        free(mut); free(c);
    }
    /* unknown panels are ignored, missing ones get a default place (R-12) */
    char *ext = strdup(json);
    char *at = strstr(ext, "\"lume.data.inspector\"");
    memcpy(at, "\"lume.plugin.gone!!!\"", 21);
    check(dock_parse(&d, ext, strlen(ext), &back, NULL, 0, err, sizeof err) && back.panel[IN].mode == DOCK_HIDDEN, "unknown panel id ignored; the missing Inspector gets its default", err);
    free(ext);
    /* Scenario 6: a broken file on disk → backup + default, the editor still opens */
    char dir[] = "/tmp/lume_dock_XXXXXX"; mkdtemp(dir);
    char path[256]; snprintf(path, sizeof path, "%s/layout.json", dir);
    check(dock_save_file(&d, path, err, sizeof err), "atomic save", err);
    Dock d2; dock_init(&d2); register_lume(&d2);
    check(dock_load_file(&d2, path, err, sizeof err) && dock_region_of(&d2, OUT) == DOCK_RIGHT && dock_is_collapsed(&d2, EX), "restart restores the saved layout", err);
    FILE *f = fopen(path, "w"); fputs("{\"schemaVersion\": 1, \"regions\": {\"left\": [{\"tabs\": [\"lume.data.explorer\", \"lume.data.explorer\"]}]}}", f); fclose(f);
    Dock d3; dock_init(&d3); register_lume(&d3);
    bool loaded = dock_load_file(&d3, path, err, sizeof err);
    char bakpat[300]; snprintf(bakpat, sizeof bakpat, "ls %s/layout.json.bad-* >/dev/null 2>&1", dir);
    check(!loaded && strstr(err, "kept as") && system(bakpat) == 0 && access(path, F_OK) != 0 && dock_is_visible(&d3, EX), "corrupt file: rejected, kept as .bad-*, default layout in use", err);
    free(json);

    /* PNL-011: a split across the region's axis (lanes): side by side in a side region, stacked
     * inside a lane, minimums, independent resize, persistence, undo, normalisation */
    {
        dock_apply_preset(&d, DOCK_PRESET_DEFAULT);
        int gex = dock_group_of(&d, EX);
        check(dock_split_lane(&d, IN, gex, true) && dock_lanes(&d.st, DOCK_LEFT) == 2 && d.st.group[dock_group_of(&d, IN)].lane == 1,
              "Inspector dropped on the right side of Explorer: a second lane in the left region", d.err);
        float need = d.desc[EX].min_w + d.desc[IN].min_w + m.splitter;
        check(d.st.region_size[DOCK_LEFT] >= need - 0.5f && d.st.region_size[DOCK_LEFT] >= 250 + d.desc[IN].min_w, "the region grew by the new lane: both lanes' minimum, the old lane keeps its width", NULL);
        dock_layout(&d, 0, 0, 1600, 900, &m, &L);
        DockRect ge = L.group[dock_group_of(&d, EX)], gi = L.group[dock_group_of(&d, IN)];
        check(ge.x + ge.w <= gi.x && fabsf(ge.y - gi.y) < 0.5f && fabsf(ge.h - gi.h) < 0.5f && ge.w >= d.desc[EX].min_w - 0.5f && gi.w >= d.desc[IN].min_w - 0.5f,
              "Explorer and Inspector side by side, full height, each at least its minimum width", NULL);
        int lane_sp = -1; for (int i = 0; i < L.nsplit; i++) if (L.split[i].kind == 3 && L.split[i].region == DOCK_LEFT) lane_sp = i;
        check(lane_sp >= 0 && L.split[lane_sp].vertical, "a vertical lane splitter separates them", NULL);
        float w0 = d.st.lane_weight[DOCK_LEFT][0];
        dock_layout(&d, 0, 0, 1600, 900, &m, &L);
        float ew = L.group[dock_group_of(&d, EX)].w;
        check(fabsf(ew - 250) < 8, "the lane that was there keeps its width (about 250 points)", NULL);
        check(dock_set_lane_weight(&d, DOCK_LEFT, 0, 1, -20) && d.st.lane_weight[DOCK_LEFT][0] < w0, "the lane splitter resizes the lanes independently", NULL);
        check(!dock_set_lane_weight(&d, DOCK_LEFT, 0, 1, 5000), "a lane never goes below its minimum", NULL);
        /* stacked inside the second lane: Output below the Inspector */
        check(dock_split_group(&d, OUT, dock_group_of(&d, IN), true) && d.st.group[dock_group_of(&d, OUT)].lane == 1,
              "a split along the axis inside a lane stays in that lane", d.err);
        dock_layout(&d, 0, 0, 1600, 900, &m, &L);
        gi = L.group[dock_group_of(&d, IN)]; DockRect go = L.group[dock_group_of(&d, OUT)]; ge = L.group[dock_group_of(&d, EX)];
        check(fabsf(gi.x - go.x) < 0.5f && go.y > gi.y && fabsf(ge.h - (go.y + go.h - gi.y)) < 1.0f, "both directions at once: Explorer | (Inspector over Output)", NULL);
        int inner = -1; for (int i = 0; i < L.nsplit; i++) if (L.split[i].kind == 1 && L.split[i].region == DOCK_LEFT) inner = i;
        float wi0 = d.st.group[dock_group_of(&d, IN)].weight;
        check(inner >= 0 && dock_set_group_pair_weight(&d, DOCK_LEFT, L.split[inner].index, L.split[inner].index2, 20) && d.st.group[dock_group_of(&d, IN)].weight > wi0,
              "the splitter inside the lane resizes its two groups only", NULL);
        /* persistence round trip */
        char *js = dock_serialize(&d, &d.st, NULL); DockState back; char nm[64], er2[200] = "";
        bool parsed = dock_parse(&d, js, strlen(js), &back, nm, sizeof nm, er2, sizeof er2);
        check(parsed && dock_lanes(&back, DOCK_LEFT) == 2 && back.group[back.panel[IN].group].lane == 1 && back.group[back.panel[OUT].group].lane == 1 && fabsf(back.lane_weight[DOCK_LEFT][0] - d.st.lane_weight[DOCK_LEFT][0]) < 0.001f,
              "lanes and lane weights survive save / load", er2);
        free(js);
        const char *bad_lane = "{\"schemaVersion\": 1, \"regions\": {\"left\": [{\"tabs\": [\"lume.data.explorer\"], \"lane\": 9}]}}";
        const char *bad_w = "{\"schemaVersion\": 1, \"laneWeight\": {\"left\": [-1]}}";
        check(!dock_parse(&d, bad_lane, strlen(bad_lane), &back, nm, sizeof nm, er2, sizeof er2) && strstr(er2, "lane"), "a file with an impossible lane is rejected", er2);
        check(!dock_parse(&d, bad_w, strlen(bad_w), &back, nm, sizeof nm, er2, sizeof er2) && strstr(er2, "lane"), "a file with a negative lane weight is rejected", er2);
        /* undo and normalisation */
        check(dock_undo(&d) && dock_group_of(&d, OUT) >= 0 && d.st.group[dock_group_of(&d, OUT)].region != DOCK_LEFT, "undo takes Output back out of the lane", NULL);
        check(dock_move_to_region(&d, IN, DOCK_RIGHT) && dock_lanes(&d.st, DOCK_LEFT) == 1 && d.st.group[dock_group_of(&d, EX)].lane == 0,
              "moving the last tab out of a lane removes the split and normalises the tree", d.err);
        char er3[200]; check(dock_validate(&d, &d.st, er3, sizeof er3), "the normalised state validates", er3);
        /* drop targets: the side across the axis is a lane split; the ends along it stay a group split */
        dock_layout(&d, 0, 0, 1600, 900, &m, &L);
        ge = L.group[dock_group_of(&d, EX)];
        DockDrop t1 = dock_drop_target(&d, &L, &m, IN, ge.x + ge.w * 0.9f, ge.y + ge.h * 0.5f);
        DockDrop t2 = dock_drop_target(&d, &L, &m, IN, ge.x + ge.w * 0.5f, ge.y + ge.h * 0.9f);
        check(t1.kind == DOCK_DROP_SPLIT && t1.cross && t1.after && t1.preview.w < ge.w, "drop on the right side of a left-region group: lane split, with a half-width preview", NULL);
        check(t2.kind == DOCK_DROP_SPLIT && !t2.cross && t2.after, "drop on its bottom end: a group split along the axis (unchanged)", NULL);
        check(dock_apply_drop(&d, IN, &t1) && dock_lanes(&d.st, DOCK_LEFT) == 2, "applying the lane drop target splits the region", d.err);
        dock_apply_preset(&d, DOCK_PRESET_DEFAULT);
        check(dock_lanes(&d.st, DOCK_LEFT) == 1, "Default preset goes back to one lane", NULL);
    }

    /* PNL-047: a non-SQL module registers, moves, persists, and unregisters cleanly */
    DockPanelDesc notes = { .id = "lume.code.problems", .title = "Problems", .icon = "warn", .region = DOCK_BOTTOM, .min_w = 200, .min_h = 80 };
    check(dock_register(&d, &notes), "a non-SQL module registers a panel through the same API", d.err);
    int PB = dock_find(&d, "lume.code.problems");
    check(dock_move_to_region(&d, PB, DOCK_LEFT) && dock_hide(&d, PB) && dock_show(&d, PB) && dock_region_of(&d, PB) == DOCK_LEFT, "it moves, hides and shows like the built-in panels", NULL);
    char *jp = dock_serialize(&d, &d.st, NULL);
    check(strstr(jp, "lume.code.problems") != NULL, "and is persisted by id", NULL);
    free(jp);
    check(dock_unregister(&d, "lume.code.problems") && dock_find(&d, "lume.code.problems") < 0 && dock_validate(&d, &d.st, err, sizeof err), "module disabled: no dangling reference in the tree", err);

    /* PNL-040 / NFR-04: 1,000 move / collapse / expand / resize cycles, no growth, same ids */
    struct rusage ru0, ru1; getrusage(RUSAGE_SELF, &ru0);
    int gb2 = dock_group_of(&d, RS);
    for (int i = 0; i < 1000; i++) {
        dock_move_to_region(&d, OUT, i % 2 ? DOCK_RIGHT : DOCK_LEFT);
        dock_collapse_group(&d, dock_group_of(&d, RS)); dock_expand_group(&d, dock_group_of(&d, RS));
        dock_set_region_size(&d, DOCK_BOTTOM, 200 + (float)(i % 50));
        dock_layout(&d, 0, 0, 1280, 760, &m, &L);
        DockDrop tt = dock_drop_target(&d, &L, &m, OUT, 640, 380); (void)tt;
    }
    getrusage(RUSAGE_SELF, &ru1);
    char det[80]; snprintf(det, sizeof det, "max RSS %ld -> %ld KB", ru0.ru_maxrss / 1024, ru1.ru_maxrss / 1024);
    check(ru1.ru_maxrss - ru0.ru_maxrss < 512 * 1024 && dock_find(&d, "lume.data.output") == OUT && dock_group_of(&d, RS) == gb2 && dock_validate(&d, &d.st, err, sizeof err), "1,000 cycles: no memory growth, same panel identities, state still valid", det);

    /* PNL-041 / NFR-02: latency of single layout operations (engine only), p95 over 2,000 each */
    {
        enum { N = 2000 };
        static double t[N];
        struct { const char *name; int kind; } ops[] = { { "hide + show", 0 }, { "collapse + expand", 1 }, { "move to another region", 2 }, { "layout of the whole workspace", 3 } };
        for (int k = 0; k < 4; k++) {
            for (int i = 0; i < N; i++) {
                struct timespec a0, a1; clock_gettime(CLOCK_MONOTONIC, &a0);
                if (ops[k].kind == 0) { dock_hide(&d, OUT); dock_show(&d, OUT); }
                else if (ops[k].kind == 1) { int g = dock_group_of(&d, RS); dock_collapse_group(&d, g); dock_expand_group(&d, g); }
                else if (ops[k].kind == 2) dock_move_to_region(&d, OUT, i % 2 ? DOCK_RIGHT : DOCK_LEFT);
                else dock_layout(&d, 0, 0, 1440, 900, &m, &L);
                clock_gettime(CLOCK_MONOTONIC, &a1);
                t[i] = (a1.tv_sec - a0.tv_sec) * 1e6 + (a1.tv_nsec - a0.tv_nsec) / 1e3;
            }
            for (int i = 1; i < N; i++) { double v = t[i]; int j = i - 1; while (j >= 0 && t[j] > v) { t[j + 1] = t[j]; j--; } t[j + 1] = v; }
            double p95 = t[N * 95 / 100];
            char det[96]; snprintf(det, sizeof det, "p95 %.1f us, max %.1f us (bar 1,000 us)", p95, t[N - 1]);
            char what[96]; snprintf(what, sizeof what, "engine latency: %s", ops[k].name);
            check(p95 < 1000, what, det);
        }
    }

    printf("%s (%d failures)\n", failures ? "DOCK: FAIL" : "DOCK: OK", failures);
    return failures ? 1 : 0;
}
