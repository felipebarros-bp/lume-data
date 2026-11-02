/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Connection dialog: see dialog.h.
 *
 * Two steps when creating: a searchable picker of every registered driver (grouped by category,
 * the drivers already in use first), then the form for that driver. Editing opens the form
 * directly; the driver shows as a chip with "Change...", which goes through the same picker and
 * keeps the values of fields that share a key (host, port, user...).
 *
 * The form: the driver's own fields first, then General (name, with the summary as a live
 * placeholder; environment; read-only in Lume). Drivers with a host and port also get SSH TUNNEL
 * (host, port, user, key file) between the two: see sshtun.h. Test runs on a worker thread (the UI never
 * blocks on a connect), its result stays beside the button until a field changes.
 *
 * State lives in struct Dialog. One frame() function lays out, handles input and draws: it runs
 * live in dialog_update (before every other panel, so the modal consumes the frame's input) and
 * again in dialog_draw with the input neutralised (the visible pass). UI thread only.
 */
#include "dialog.h"
#include "draw.h"
#include "font.h"
#include "theme.h"
#include "ui.h"
#include "results.h"
#include <ctype.h>
#include <math.h>

/* A Test in flight. The worker owns it once abandoned (dialog closed / test restarted). */
typedef struct {
    SDL_AtomicInt state;   /* 0 running, 1 finished (the dialog frees it), 2 abandoned (the worker frees it) */
    Connection c;          /* a copy of the draft: the worker never touches the dialog */
    bool ok; char err[256]; Uint64 ms;
} TestJob;

/* Dialog state; one instance per app. */
struct Dialog {
    bool open;   /* shown this frame */
    Connection *target;        /* NULL when creating */
    Connection draft;   /* edited copy; never connected (db / tables cleared) */
    int driver_idx;   /* index into driver_infos() */
    int step;   /* 0 the driver picker, 1 the form */
    bool changing;   /* the picker was opened from the form's "Change..." (editing, or Back while creating) */
    TextField search;   /* picker filter */
    int pick_sel;   /* keyboard selection in the picker (index into the shown list) */
    float pick_scroll; ScrollBar pick_sb;
    TextField name;   /* connection name editor */
    TextField fields[CONN_MAX_FIELDS];   /* one editor per driver field */
    TextField ssh[4];          /* SSH tunnel: host, port, user, key file */
    int focus;                 /* 0 = name, 1..nfields = fields, then the SSH fields (when shown) */
    int bad_field;   /* the field the last validation stopped at (+1; 0 none): outlined in red */
    char msg[256];   /* result of Test / validation, beside the Test button ("" = none) */
    bool msg_ok;   /* msg is a success (green) rather than an error (red) */
    bool ro_touched;   /* the user set Read-only themselves: PROD does not suggest it again */
    int confirm_delete;   /* 1 after the first Delete click: the next one deletes */
    TestJob *job;   /* Test running */
    unsigned sig;   /* fields' content signature: a change clears the last Test result */
    Rect panel;   /* last frame's dialog rectangle */
};

/* See dialog.h. */
Dialog *dialog_create(void) { return calloc(1, sizeof(Dialog)); }

static void test_abandon(Dialog *d) {
    if (!d->job) return;
    if (!SDL_CompareAndSwapAtomicInt(&d->job->state, 0, 2)) { memset(&d->job->c, 0, sizeof d->job->c); free(d->job); }   /* already finished: ours to free */
    d->job = NULL;
}
/* See dialog.h. */
void dialog_destroy(Dialog *d) { if (d) test_abandon(d); free(d); }
/* See dialog.h. */
bool dialog_is_open(Dialog *d) { return d->open; }

/* Registry entry of the driver currently selected in the dialog. */
static const DriverInfo *drv(Dialog *d) { int n; return &driver_infos(&n)[d->driver_idx]; }
static bool is_file_driver(const DriverInfo *di) { for (int i = 0; i < di->nfields; i++) if (di->fields[i].type == FIELD_FILE) return true; return false; }

/* Copies the draft's values into the text fields and marks password fields for masking. */
static void load_fields(Dialog *d) {
    const DriverInfo *di = drv(d);
    for (int i = 0; i < di->nfields; i++) {
        textfield_set(&d->fields[i], d->draft.values[i]);
        d->fields[i].password = di->fields[i].type == FIELD_PASSWORD;
    }
}

static void close_dialog(Dialog *d) { test_abandon(d); d->open = false; }

/* The SSH section: shown for drivers with host and port; its fields only while the tunnel is on. */
enum { SSH_HOST, SSH_PORT, SSH_USER, SSH_KEY, SSH_NFIELDS };
static int ssh_shown(Dialog *d) { return conn_supports_ssh(drv(d)) && d->draft.ssh ? SSH_NFIELDS : 0; }
/* The editor behind a focus / bad_field index (1-based: driver fields, then the SSH ones). */
static TextField *field_at(Dialog *d, int idx) {
    int nf = drv(d)->nfields;
    if (idx >= 1 && idx <= nf) return &d->fields[idx - 1];
    if (idx > nf && idx <= nf + SSH_NFIELDS) return &d->ssh[idx - nf - 1];
    return &d->name;
}

/* See dialog.h. The draft is a struct copy of the existing connection with its runtime
 * pointers cleared, so editing never touches the live connection. */
void dialog_open(Dialog *d, App *a, Connection *existing) {
    int n;
    const DriverInfo *all = driver_infos(&n);
    test_abandon(d);
    memset(&d->draft, 0, sizeof d->draft);
    d->target = existing;
    if (existing) {
        d->draft = *existing;
        d->draft.db = NULL; d->draft.tables = NULL; d->draft.ntables = 0; d->draft.attempt = NULL;   /* the draft is never connected */
    } else snprintf(d->draft.driver, sizeof d->draft.driver, "sqlite");
    d->driver_idx = 0;
    for (int i = 0; i < n; i++) if (strcmp(all[i].id, d->draft.driver) == 0) d->driver_idx = i;
    textfield_set(&d->name, d->draft.name);
    textfield_set(&d->search, "");
    load_fields(d);
    d->draft.tunnel = NULL;
    textfield_set(&d->ssh[SSH_HOST], d->draft.ssh_host); textfield_set(&d->ssh[SSH_PORT], d->draft.ssh_port);
    textfield_set(&d->ssh[SSH_USER], d->draft.ssh_user); textfield_set(&d->ssh[SSH_KEY], d->draft.ssh_key);
    d->step = existing ? 1 : 0;   /* a new connection starts by choosing its database */
    d->changing = false;
    d->pick_sel = 0; d->pick_scroll = 0;
    d->focus = 0; d->bad_field = 0;
    d->msg[0] = 0; d->msg_ok = true;
    d->ro_touched = existing != NULL;
    d->confirm_delete = 0;
    d->sig = 0;
    d->open = true;
    (void)a;
}

/* Changes the draft's driver. Values of fields with the same key in both drivers are kept (the
 * text fields hold the latest edits); the others take the new driver's defaults. */
static void switch_driver(Dialog *d, int idx) {
    int n;
    const DriverInfo *all = driver_infos(&n);
    if (idx < 0 || idx >= n) return;
    const DriverInfo *old = drv(d), *nw = &all[idx];
    char keep[CONN_MAX_FIELDS][CONN_VALUE_LEN]; int kept = 0;
    for (int j = 0; j < nw->nfields && j < CONN_MAX_FIELDS; j++) {
        keep[j][0] = 0; bool found = false;
        for (int i = 0; i < old->nfields; i++) if (!strcmp(old->fields[i].key, nw->fields[j].key) && d->fields[i].buf[0]) { snprintf(keep[j], CONN_VALUE_LEN, "%s", d->fields[i].buf); found = true; kept++; break; }
        (void)found;   /* not found: left empty, the field shows the driver's default as its placeholder */
    }
    if (idx != d->driver_idx && d->target && kept) snprintf(d->msg, sizeof d->msg, "Now %s: %d matching field(s) kept, the others set to defaults.", nw->name, kept), d->msg_ok = true;
    else d->msg[0] = 0;
    d->driver_idx = idx;
    snprintf(d->draft.driver, sizeof d->draft.driver, "%s", nw->id);
    for (int i = 0; i < CONN_MAX_FIELDS; i++) d->draft.values[i][0] = 0;
    for (int j = 0; j < nw->nfields && j < CONN_MAX_FIELDS; j++) memcpy(d->draft.values[j], keep[j], CONN_VALUE_LEN);
    load_fields(d);
    d->bad_field = 0;
    d->focus = nw->nfields ? 1 : 0;
}

/* Copies the text fields into the draft. */
static void collect(Dialog *d) {
    const DriverInfo *di = drv(d);
    snprintf(d->draft.name, sizeof d->draft.name, "%s", d->name.buf);
    /* an empty field takes the driver's default (shown as its placeholder); never a password */
    for (int i = 0; i < di->nfields; i++) snprintf(d->draft.values[i], CONN_VALUE_LEN, "%s", d->fields[i].buf[0] || di->fields[i].type == FIELD_PASSWORD ? d->fields[i].buf : di->fields[i].def);
    if (!d->draft.name[0]) conn_summary(&d->draft, d->draft.name, sizeof d->draft.name);
    snprintf(d->draft.ssh_host, sizeof d->draft.ssh_host, "%s", d->ssh[SSH_HOST].buf);
    snprintf(d->draft.ssh_port, sizeof d->draft.ssh_port, "%s", d->ssh[SSH_PORT].buf);
    snprintf(d->draft.ssh_user, sizeof d->draft.ssh_user, "%s", d->ssh[SSH_USER].buf);
    snprintf(d->draft.ssh_key, sizeof d->draft.ssh_key, "%s", d->ssh[SSH_KEY].buf);
}

/* Every non-password field must be filled; otherwise says which, outlines and focuses it. */
static bool validate(Dialog *d) {
    const DriverInfo *di = drv(d);
    d->bad_field = 0;
    for (int i = 0; i < di->nfields; i++) {
        if (di->fields[i].type != FIELD_PASSWORD && !d->draft.values[i][0]) {
            snprintf(d->msg, sizeof d->msg, "%s is required.", di->fields[i].label);
            d->msg_ok = false;
            d->focus = i + 1; d->bad_field = i + 1;
            return false;
        }
    }
    if (ssh_shown(d) && !d->draft.ssh_host[0]) {
        snprintf(d->msg, sizeof d->msg, "SSH host is required (or turn the SSH tunnel off).");
        d->msg_ok = false;
        d->focus = d->bad_field = di->nfields + 1 + SSH_HOST;
        return false;
    }
    return true;
}
/* The draft's SSH settings into a saved connection. */
static void copy_ssh(Connection *c, const Connection *from) {
    c->ssh = from->ssh;
    memcpy(c->ssh_host, from->ssh_host, sizeof c->ssh_host); memcpy(c->ssh_port, from->ssh_port, sizeof c->ssh_port);
    memcpy(c->ssh_user, from->ssh_user, sizeof c->ssh_user); memcpy(c->ssh_key, from->ssh_key, sizeof c->ssh_key);
}

/* Writes the draft into the edited connection (or a new one) and closes the dialog.
 * A connection that was open is disconnected first and reconnected afterwards; `connect`
 * also connects a closed one. Does nothing but report when validation fails. */
static void save(Dialog *d, App *a, bool connect) {
    collect(d);
    if (!validate(d)) return;
    Connection *c = d->target;
    if (c) {
        bool was_open = c->db != NULL;
        if (was_open) app_disconnect(a, c);
        snprintf(c->name, sizeof c->name, "%s", d->draft.name);
        snprintf(c->driver, sizeof c->driver, "%s", d->draft.driver);
        memcpy(c->values, d->draft.values, sizeof c->values);
        c->demo = 0;
        c->env = d->draft.env; c->read_only = d->draft.read_only;
        copy_ssh(c, &d->draft);
        if (was_open) connect = true;
    } else {
        c = conns_add(a->conns, d->draft.driver);
        snprintf(c->name, sizeof c->name, "%s", d->draft.name);
        memcpy(c->values, d->draft.values, sizeof c->values);
        c->env = d->draft.env; c->read_only = d->draft.read_only;
        copy_ssh(c, &d->draft);
    }
    app_connections_changed(a);
    close_dialog(d);
    if (connect) app_connect(a, c);
    else app_status(a, "Saved %s", c->name);
}

/* ---- Test, off the UI thread ---- */
static int test_worker(void *p) {
    TestJob *j = p;
    Uint64 t0 = SDL_GetTicks();
    j->ok = conn_test(&j->c, j->err, sizeof j->err);
    j->ms = SDL_GetTicks() - t0;
    if (!SDL_CompareAndSwapAtomicInt(&j->state, 0, 1)) { memset(&j->c, 0, sizeof j->c); free(j); }   /* abandoned meanwhile */
    return 0;
}
static void test_start(Dialog *d) {
    collect(d);
    if (!validate(d)) return;
    test_abandon(d);
    TestJob *j = calloc(1, sizeof *j);
    if (!j) return;
    j->c = d->draft;
    SDL_Thread *th = SDL_CreateThread(test_worker, "lume-conn-test", j);
    if (!th) { memset(&j->c, 0, sizeof j->c); free(j); snprintf(d->msg, sizeof d->msg, "Could not start the test."); d->msg_ok = false; return; }
    SDL_DetachThread(th);
    d->job = j;
    d->msg[0] = 0;
}
static void test_poll(Dialog *d, App *a) {
    if (!d->job) return;
    if (SDL_GetAtomicInt(&d->job->state) != 1) { app_wake_in(a, 120); return; }
    TestJob *j = d->job; d->job = NULL;
    if (j->ok) snprintf(d->msg, sizeof d->msg, "\xE2\x9C\x93 Connected in %llu ms.", (unsigned long long)j->ms);
    else {   /* the driver's message on one line */
        size_t k = 0; bool sp = false;
        for (const char *p = j->err; *p && k + 2 < sizeof d->msg; p++) { if (*p == '\n' || *p == '\r' || *p == '\t' || *p == ' ') { sp = k > 0; continue; } if (sp) { d->msg[k++] = ' '; sp = false; } d->msg[k++] = *p; }
        d->msg[k] = 0;
    }
    d->msg_ok = j->ok;
    memset(&j->c, 0, sizeof j->c); free(j);
}

/* See dialog.h. */
void dialog_file_chosen(Dialog *d, const char *path) {
    const DriverInfo *di = drv(d);
    for (int i = 0; i < di->nfields; i++) if (di->fields[i].type == FIELD_FILE) { textfield_set(&d->fields[i], path); d->focus = i + 1; }
    if (!d->name.len) {
        const char *base = strrchr(path, '/');
        textfield_set(&d->name, base ? base + 1 : path);
    }
}

/* ---- the driver picker ---- */
/* The shown entries: "recent" (drivers of saved connections) first, then every driver matching the
 * search, by category in registry order. Returns the count; ids[i] the driver index, hdr[i] the
 * header to draw before it (NULL none). */
static int picker_list(Dialog *d, App *a, int *ids, const char **hdr, int max) {
    int n; const DriverInfo *all = driver_infos(&n);
    const char *q = d->search.buf; int m = 0;
    if (!q[0] && a->conns) {
        bool used[64] = { 0 };
        for (int k = 0; k < a->conns->n; k++) {
            Connection *c = a->conns->items[k];
            if (c->demo) continue;
            for (int i = 0; i < n && i < 64; i++) if (!used[i] && !strcmp(all[i].id, c->driver)) { used[i] = true; if (m < max) { hdr[m] = m == 0 ? "Recent" : NULL; ids[m++] = i; } }
        }
    }
    const char *cats[16]; int nc = 0;
    for (int i = 0; i < n; i++) { const char *c = all[i].category ? all[i].category : "Other"; bool seen = false; for (int k = 0; k < nc; k++) if (!strcmp(cats[k], c)) seen = true; if (!seen && nc < 16) cats[nc++] = c; }
    for (int k = 0; k < nc; k++) {
        bool first = true;
        for (int i = 0; i < n; i++) {
            const char *c = all[i].category ? all[i].category : "Other";
            if (strcmp(c, cats[k])) continue;
            if (q[0] && !strcasestr(all[i].name, q) && !strcasestr(all[i].id, q) && !(all[i].aliases && strcasestr(all[i].aliases, q))) continue;
            if (m < max) { hdr[m] = first ? cats[k] : NULL; ids[m++] = i; first = false; }
        }
    }
    return m;
}

static void choose_driver(Dialog *d, int idx) {
    switch_driver(d, idx);
    d->step = 1;
    d->changing = false;
}

/* The badge: the driver's colour and its two letters. */
static void draw_badge(App *a, const DriverInfo *di, Rect b) {
    draw_rrect(a->renderer, b, px(a, 6), color_hex(di->color));
    draw_text_fit(a->font_ui, a->renderer, di->abbr ? di->abbr : "DB", -1, b, color_hex(0xFFFFFF), ALIGN_CENTER);
}

static void picker(Dialog *d, App *a, Rect body, bool live) {
    SDL_Renderer *r = a->renderer; Input *in = &a->in;
    int ids[64]; const char *hdr[64];
    int m = picker_list(d, a, ids, hdr, 64);
    int n; const DriverInfo *all = driver_infos(&n);
    if (d->pick_sel >= m) d->pick_sel = m ? m - 1 : 0;
    Rect sr = rect_make(body.x, body.y, body.w, px(a, 32));
    ui_record("conn:search", sr);
    ui_textfield(a, &d->search, sr, "Search databases (name, or pg, mssql, file...)", true);
    float cw = px(a, 172), ch = px(a, 52), gap = px(a, 10);
    int cols = (int)((body.w + gap) / (cw + gap)); if (cols < 1) cols = 1;
    cw = (body.w - gap * (cols - 1)) / cols;
    if (live) {
        for (int k = 0; k < in->nkeys; k++) {
            KeyPress *kp = &in->keys[k]; if (kp->used) continue;
            if (kp->key == SDLK_RIGHT && d->search.cursor >= d->search.len) { d->pick_sel = d->pick_sel + 1 < m ? d->pick_sel + 1 : d->pick_sel; kp->used = true; }
            else if (kp->key == SDLK_LEFT && d->search.cursor >= d->search.len && d->pick_sel > 0) { d->pick_sel--; kp->used = true; }
            else if (kp->key == SDLK_DOWN) { d->pick_sel = d->pick_sel + cols < m ? d->pick_sel + cols : (m ? m - 1 : 0); kp->used = true; }
            else if (kp->key == SDLK_UP) { d->pick_sel = d->pick_sel - cols >= 0 ? d->pick_sel - cols : 0; kp->used = true; }
            else if ((kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) && m) { kp->used = true; choose_driver(d, ids[d->pick_sel]); return; }
        }
    }
    Rect area = rect_make(body.x, body.y + px(a, 44), body.w, body.h - px(a, 44));
    if (live && rect_contains(area, in->mouse_x, in->mouse_y) && in->wheel_y) { d->pick_scroll -= in->wheel_y * px(a, 30); in->wheel_y = 0; }
    /* layout pass: rows by category, a header before each group */
    float y = 0; int col = 0; float sel_y = 0;
    float ys[64], xs[64];
    for (int i = 0; i < m; i++) {
        if (hdr[i]) { if (col) { y += ch + gap; col = 0; } if (i) y += px(a, 6); y += px(a, 22); }
        xs[i] = col * (cw + gap); ys[i] = y;
        if (i == d->pick_sel) sel_y = y;
        if (++col == cols) { col = 0; y += ch + gap; }
    }
    float content = y + (col ? ch : 0);
    float maxs = content > area.h ? content - area.h : 0;
    if (live && in->nkeys) { if (sel_y - d->pick_scroll < 0) d->pick_scroll = sel_y; if (sel_y + ch - d->pick_scroll > area.h) d->pick_scroll = sel_y + ch - area.h; }
    if (live) ui_scrollbar_input(a, &d->pick_sb, area, content, &d->pick_scroll);
    if (d->pick_scroll > maxs) d->pick_scroll = maxs;
    if (d->pick_scroll < 0) d->pick_scroll = 0;
    clip_push(r, area);
    for (int i = 0; i < m; i++) {
        float by = area.y + ys[i] - d->pick_scroll;
        if (hdr[i]) { char up[40]; size_t k = 0; for (const char *p = hdr[i]; *p && k + 1 < sizeof up; p++) up[k++] = (char)toupper((unsigned char)*p); up[k] = 0;
                      draw_text_fit(a->font_small, r, up, -1, rect_make(area.x, by - px(a, 22), area.w, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT); }
        Rect c = rect_make(area.x + xs[i], by, cw, ch);
        const DriverInfo *di = &all[ids[i]];
        bool hov = rect_contains(c, in->mouse_x, in->mouse_y) && rect_contains(area, in->mouse_x, in->mouse_y);
        bool sel = i == d->pick_sel;
        draw_rrect(r, c, px(a, 8), color_hex(sel ? T_ACCENT_DIM : T_BORDER));
        draw_rrect(r, rect_inset(c, px(a, 1), px(a, 1)), px(a, 7), color_hex(hov ? T_HOVER : sel ? T_SELECTED : T_PANEL));
        draw_badge(a, di, rect_make(c.x + px(a, 10), c.y + (ch - px(a, 30)) / 2, px(a, 30), px(a, 30)));
        draw_text_fit(a->font_ui, r, di->name, -1, rect_make(c.x + px(a, 50), c.y, c.w - px(a, 58), ch), color_hex(T_TEXT), ALIGN_LEFT);
        char tg[64]; snprintf(tg, sizeof tg, "conn:driver %s", di->name); ui_record(tg, c);
        if (hov) a->want_cursor = CURSOR_HAND;
        if (live && hov && in->mouse_pressed && !in->mouse_consumed) { in->mouse_consumed = true; d->pick_sel = i; choose_driver(d, ids[i]); clip_pop(r); return; }
    }
    if (!m) draw_text_fit(a->font_ui, r, "No database matches the search.", -1, rect_make(area.x, area.y + px(a, 10), area.w, px(a, 24)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    clip_pop(r);
    if (content > area.h) { ui_scrollbar_touch(&d->pick_sb); ui_scrollbar_draw(a, &d->pick_sb, area, content, d->pick_scroll); }   /* always shown when there is more below */
}

/* ---- the whole dialog, one pass ---- */
static void frame(Dialog *d, App *a, bool live) {
    SDL_Renderer *r = a->renderer; Input *in = &a->in;
    const DriverInfo *di = drv(d);
    float w = px(a, 640), pad = px(a, 24), row = px(a, 40), fh = px(a, 30), lbl_w = px(a, 120);
    float h;
    if (d->step == 0) h = px(a, 470);
    else h = pad + px(a, 34) + (d->target ? row : 0) + px(a, 24) + row * di->nfields + px(a, 30) + row * 2 + px(a, 26) + px(a, 30) + px(a, 50) + px(a, 52) + pad
             + (conn_supports_ssh(di) ? row + (ssh_shown(d) ? row * 3 : 0) : 0);
    if (h > a->height - px(a, 20)) h = a->height - px(a, 20);
    float x = (a->width - w) / 2, y = (a->height - h) / 2 - px(a, 10);
    Rect panel = rect_make(x, y, w, h); d->panel = panel;
    if (!live) {
        draw_rect(r, rect_make(0, 0, (float)a->width, (float)a->height), color_alpha(color_hex(0x000000), 150));
        for (int s = 3; s >= 1; s--)
            draw_rrect(r, rect_make(panel.x - s * px(a, 2), panel.y + s * px(a, 2), panel.w + s * px(a, 4), panel.h + s * px(a, 2)), px(a, 12), color_alpha(color_hex(0x000000), 40));
        draw_rrect(r, panel, px(a, 12), color_hex(T_BORDER));
        draw_rrect(r, rect_inset(panel, px(a, 1), px(a, 1)), px(a, 11), color_hex(T_PANEL_ALT));
    }
    /* keys the dialog owns everywhere */
    if (live) for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k]; if (kp->used) continue;
        if (kp->key == SDLK_ESCAPE) {
            kp->used = true;
            if (d->step == 0 && d->changing) { d->step = 1; d->changing = false; }   /* back to the form, nothing changed */
            else { close_dialog(d); return; }
        }
    }
    /* header: back (picker opened from the form, or the form of a new connection), title, close box */
    float cy = y + pad;
    float tx = x + pad;
    bool can_back = d->step == 1 ? !d->target : d->changing;
    if (can_back) {
        Rect bb = rect_make(x + pad - px(a, 6), cy, px(a, 30), px(a, 28)); ui_record("conn:back", bb);
        if (ui_button(a, bb, "\xE2\x80\xB9", BTN_QUIET, true) && live) {
            if (d->step == 1) { collect(d); d->step = 0; d->changing = true; textfield_set(&d->search, ""); }
            else { d->step = 1; d->changing = false; }
            return;
        }
        tx += px(a, 32);
    }
    char title[160];
    if (d->step == 0) snprintf(title, sizeof title, "%s", d->target ? "Change the database type" : "New Connection: choose the database");
    else if (d->target) snprintf(title, sizeof title, "Edit Connection");
    else snprintf(title, sizeof title, "New %s Connection", di->name);
    draw_text_fit(a->font_ui, r, title, -1, rect_make(tx, cy, w - (tx - x) - pad - px(a, 36), px(a, 28)), color_hex(T_TEXT), ALIGN_LEFT);
    Rect xb = rect_make(x + w - pad - px(a, 26), cy, px(a, 28), px(a, 28)); ui_record("conn:x", xb);
    if (ui_button(a, xb, "\xC3\x97", BTN_QUIET, true) && live) { close_dialog(d); return; }
    cy += px(a, 34) + px(a, 8);

    if (d->step == 0) {
        Rect body = rect_make(x + pad, cy, w - pad * 2, y + h - cy - pad - px(a, 44));
        picker(d, a, body, live);
        if (!d->open) return;
        if (d->step == 0) {
            const char *note = d->target ? "Fields with the same name (host, port, user...) keep their values; the others take the new type's defaults."
                                         : "Type to filter, arrows to move, Enter to continue.";
            draw_text_fit(a->font_small, r, note, -1, rect_make(x + pad, y + h - pad - px(a, 32), w - pad * 2 - px(a, 100), px(a, 32)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
            if (ui_button(a, rect_make(x + w - pad - px(a, 90), y + h - pad - px(a, 32), px(a, 90), px(a, 32)), "Cancel", BTN_NORMAL, true) && live) {
                if (d->changing) { d->step = 1; d->changing = false; } else close_dialog(d);
            }
        }
        if (live) { in->mouse_consumed = true; in->nkeys = 0; in->ntext = 0; if (!rect_contains(panel, in->mouse_x, in->mouse_y)) a->want_cursor = CURSOR_ARROW; }
        return;
    }

    /* ---- the form ---- */
    test_poll(d, a);
    int nfocus = 1 + di->nfields + ssh_shown(d);
    if (live) for (int k = 0; k < in->nkeys; k++) {
        KeyPress *kp = &in->keys[k]; if (kp->used) continue;
        if (kp->key == SDLK_TAB) { int f = d->focus == 0 ? nfocus - 1 : d->focus - 1;   /* order on screen: fields, then name */
            f = (f + ((kp->mods & KM_SHIFT) ? nfocus - 1 : 1)) % nfocus; d->focus = f == nfocus - 1 ? 0 : f + 1; kp->used = true; }
        else if (kp->key == SDLK_RETURN || kp->key == SDLK_KP_ENTER) {
            /* Enter moves on to the next field still empty (the password included) after the focused one;
             * with none left, or with Cmd, it is the default action (Save & Connect) */
            kp->used = true;
            int next = 0;
            if (!(kp->mods & KM_PRIMARY) && d->focus >= 1)
                for (int i = d->focus; i < di->nfields; i++) if (!d->fields[i].buf[0] && !(di->fields[i].def[0] && di->fields[i].type != FIELD_PASSWORD)) { next = i + 1; break; }
            if (next) d->focus = next;
            else { collect(d); if (validate(d)) { save(d, a, true); if (!d->open) return; } }
        }
    }
    float fx = x + pad + lbl_w, fw_full = w - pad * 2 - lbl_w;
    if (d->target) {   /* the database type, changed on purpose only */
        ui_label(a, rect_make(x + pad, cy, lbl_w, fh), "Database", ALIGN_LEFT, T_TEXT_MUTED);
        draw_badge(a, di, rect_make(fx, cy + px(a, 2), px(a, 26), px(a, 26)));
        draw_text_fit(a->font_ui, r, di->name, -1, rect_make(fx + px(a, 34), cy, px(a, 200), fh), color_hex(T_TEXT), ALIGN_LEFT);
        Rect chg = rect_make(fx + px(a, 34) + font_width(a->font_ui, di->name, -1) + px(a, 12), cy + px(a, 2), px(a, 90), px(a, 26)); ui_record("conn:change", chg);
        if (ui_button(a, chg, "Change\xE2\x80\xA6", BTN_QUIET, true) && live) { collect(d); d->step = 0; d->changing = true; textfield_set(&d->search, ""); return; }
        cy += row;
    }
    ui_label(a, rect_make(x + pad, cy, w, px(a, 18)), is_file_driver(di) ? "FILE" : "CONNECTION", ALIGN_LEFT, T_TEXT_DIM);
    cy += px(a, 24);
    unsigned sig = 2166136261u;
    for (int i = 0; i < di->nfields; i++) {
        ui_label(a, rect_make(x + pad, cy, lbl_w, fh), di->fields[i].label, ALIGN_LEFT, T_TEXT_MUTED);
        float fw = fw_full; Rect browse = { 0 };
        if (di->fields[i].type == FIELD_FILE) { fw -= px(a, 96); browse = rect_make(fx + fw + px(a, 8), cy, px(a, 88), fh); }
        if (di->fields[i].type == FIELD_INT) fw = px(a, 110);
        Rect fr = rect_make(fx, cy, fw, fh);
        bool f = d->focus == i + 1;
        if (ui_textfield(a, &d->fields[i], fr, di->fields[i].def[0] ? di->fields[i].def : di->fields[i].label, f) && live) d->focus = i + 1;
        if (di->fields[i].type == FIELD_INT) {   /* digits only */
            int wn = 0;
            for (int q = 0; q < d->fields[i].len; q++) if (d->fields[i].buf[q] >= '0' && d->fields[i].buf[q] <= '9') d->fields[i].buf[wn++] = d->fields[i].buf[q];
            d->fields[i].buf[wn] = 0; d->fields[i].len = wn; if (d->fields[i].cursor > wn) d->fields[i].cursor = wn;
        }
        if (!live) { if (d->bad_field == i + 1) draw_rect_outline(r, fr, px(a, 1), color_hex(T_RED)); else if (f) draw_rect_outline(r, fr, px(a, 1), color_hex(T_ACCENT)); }
        if (di->fields[i].type == FIELD_FILE && ui_button(a, browse, "Browse\xE2\x80\xA6", BTN_NORMAL, true) && live) app_browse_file(a);
        for (const char *p = d->fields[i].buf; *p; p++) sig = (sig ^ (unsigned char)*p) * 16777619u;
        sig = (sig ^ 0xFF) * 16777619u;
        cy += row;
    }
    /* SSH tunnel: the database is reached through the system's ssh (sshtun.h). Compact: the switch on
     * the section's own row, the SSH port beside the SSH host. */
    if (conn_supports_ssh(di)) {
        ui_label(a, rect_make(x + pad, cy, lbl_w, fh), "SSH tunnel", ALIGN_LEFT, T_TEXT_MUTED);
        Rect sc = rect_make(fx, cy, px(a, 170), fh); ui_record("conn:tunnel", sc);
        if (ui_checkbox(a, sc, "Use SSH tunnel", d->draft.ssh, true) && live) {
            d->draft.ssh = !d->draft.ssh;
            if (d->draft.ssh && !d->ssh[SSH_HOST].buf[0]) d->focus = di->nfields + 1 + SSH_HOST;
            else if (!d->draft.ssh && d->focus > di->nfields) d->focus = 0;
        }
        draw_text_fit(a->font_small, r, "keys, ssh-agent and ~/.ssh/config; no SSH password", -1,
                      rect_make(fx + px(a, 178), cy, fw_full - px(a, 178), fh), color_hex(T_TEXT_DIM), ALIGN_LEFT);
        cy += row;
        sig = (sig ^ (unsigned)(d->draft.ssh ? 0xA5 : 0x5A)) * 16777619u;
        if (d->draft.ssh) {
            static const char *const holders[SSH_NFIELDS] = { "host, or a Host from ~/.ssh/config", "22", "from ~/.ssh/config, else your login",
                                                                "~/.ssh/id_ed25519 (empty: ssh-agent / ~/.ssh/config)" };
            float portw = px(a, 90), portlbl = px(a, 44);
            Rect rects[SSH_NFIELDS] = {
                rect_make(fx, cy, fw_full - portw - portlbl - px(a, 8), fh),
                rect_make(fx + fw_full - portw, cy, portw, fh),
                rect_make(fx, cy + row, fw_full, fh),
                rect_make(fx, cy + row * 2, fw_full, fh) };
            ui_label(a, rect_make(x + pad, cy, lbl_w, fh), "SSH host", ALIGN_LEFT, T_TEXT_MUTED);
            ui_label(a, rect_make(rects[SSH_PORT].x - portlbl, cy, portlbl - px(a, 6), fh), "Port", ALIGN_RIGHT, T_TEXT_MUTED);
            ui_label(a, rect_make(x + pad, cy + row, lbl_w, fh), "SSH user", ALIGN_LEFT, T_TEXT_MUTED);
            ui_label(a, rect_make(x + pad, cy + row * 2, lbl_w, fh), "Key file", ALIGN_LEFT, T_TEXT_MUTED);
            for (int k = 0; k < SSH_NFIELDS; k++) {
                int idx = di->nfields + 1 + k;
                Rect fr = rects[k];
                char tg[32]; snprintf(tg, sizeof tg, "conn:ssh%d", k); ui_record(tg, fr);
                bool f = d->focus == idx;
                if (ui_textfield(a, &d->ssh[k], fr, holders[k], f) && live) d->focus = idx;
                if (k == SSH_PORT) {   /* digits only */
                    TextField *t = &d->ssh[k]; int wn = 0;
                    for (int q = 0; q < t->len; q++) if (t->buf[q] >= '0' && t->buf[q] <= '9') t->buf[wn++] = t->buf[q];
                    t->buf[wn] = 0; t->len = wn; if (t->cursor > wn) t->cursor = wn;
                }
                if (!live) { if (d->bad_field == idx) draw_rect_outline(r, fr, px(a, 1), color_hex(T_RED)); else if (f) draw_rect_outline(r, fr, px(a, 1), color_hex(T_ACCENT)); }
                for (const char *p = d->ssh[k].buf; *p; p++) sig = (sig ^ (unsigned char)*p) * 16777619u;
                sig = (sig ^ 0xFF) * 16777619u;
            }
            cy += row * 3;
        }
    }
    /* General: what the connection is called and how Lume treats it */
    cy += px(a, 6);
    ui_label(a, rect_make(x + pad, cy, w, px(a, 18)), "GENERAL", ALIGN_LEFT, T_TEXT_DIM);
    cy += px(a, 24);
    ui_label(a, rect_make(x + pad, cy, lbl_w, fh), "Name", ALIGN_LEFT, T_TEXT_MUTED);
    char ph[200];
    { Connection tmp = d->draft; const DriverInfo *dd = di; for (int i = 0; i < dd->nfields; i++) snprintf(tmp.values[i], CONN_VALUE_LEN, "%s", d->fields[i].buf[0] || dd->fields[i].type == FIELD_PASSWORD ? d->fields[i].buf : dd->fields[i].def);   /* as collect() will save it */ tmp.name[0] = 0; conn_summary(&tmp, ph, sizeof ph); memset(&tmp, 0, sizeof tmp); }
    { char ph2[240]; snprintf(ph2, sizeof ph2, "%s (when left empty)", ph[0] ? ph : "a name from the fields");
      Rect nr = rect_make(fx, cy, fw_full, fh);
      if (ui_textfield(a, &d->name, nr, ph2, d->focus == 0) && live) d->focus = 0;
      if (!live && d->focus == 0) draw_rect_outline(r, nr, px(a, 1), color_hex(T_ACCENT)); }
    cy += row;
    ui_label(a, rect_make(x + pad, cy, lbl_w, fh), "Environment", ALIGN_LEFT, T_TEXT_MUTED);
    { static const char *const envs[] = { "None", "DEV", "HML", "PROD" };
      int pe = ui_segmented(a, rect_make(fx, cy, px(a, 260), fh), envs, 4, d->draft.env);
      if (pe >= 0 && live) {
          d->draft.env = pe;
          if (pe == CONN_ENV_PROD && !d->ro_touched && !d->draft.read_only) { d->draft.read_only = true; snprintf(d->msg, sizeof d->msg, "PROD: Read-only in Lume turned on (you can turn it off)."); d->msg_ok = true; }
      }
      draw_text_fit(a->font_small, r, "HML = homologation / staging", -1, rect_make(fx + px(a, 272), cy, fw_full - px(a, 272), fh), color_hex(T_TEXT_DIM), ALIGN_LEFT); }
    cy += row;
    if (ui_checkbox(a, rect_make(fx, cy, px(a, 200), fh), "Read-only in Lume", d->draft.read_only, true) && live) { d->draft.read_only = !d->draft.read_only; d->ro_touched = true; }
    draw_text_fit(a->font_small, r, "Lume refuses writes; the database login itself is unchanged.", -1, rect_make(fx, cy + fh - px(a, 2), fw_full, px(a, 18)), color_hex(T_TEXT_DIM), ALIGN_LEFT);
    cy += row + px(a, 20);
    /* Test: a tool, with its result beside it (cleared when a field changes) */
    if (live && d->sig && sig != d->sig && !d->job && d->msg[0] && !d->bad_field) d->msg[0] = 0;
    if (live) { d->sig = sig; if (d->bad_field && field_at(d, d->bad_field)->buf[0]) d->bad_field = 0; }
    Rect tb = rect_make(x + pad, cy, px(a, 140), px(a, 30)); ui_record("conn:test", tb);
    if (ui_button(a, tb, d->job ? "Testing\xE2\x80\xA6" : "Test connection", BTN_NORMAL, !d->job) && live) test_start(d);
    if (d->job) {
        Rect cb = rect_make(tb.x + tb.w + px(a, 8), cy, px(a, 80), px(a, 30));
        if (ui_button(a, cb, "Cancel", BTN_QUIET, true) && live) { test_abandon(d); snprintf(d->msg, sizeof d->msg, "Test cancelled."); d->msg_ok = true; }
        else { float t = (float)(SDL_GetTicks() % 1000) / 1000.0f; float dx = cb.x + cb.w + px(a, 12);
               for (int k = 0; k < 3; k++) draw_rrect(r, rect_make(dx + k * px(a, 10), cy + px(a, 12), px(a, 6), px(a, 6)), px(a, 3), color_alpha(color_hex(T_ACCENT), (int)(80 + 175 * (0.5f + 0.5f * sinf((t - k * 0.2f) * 6.283f))))); }
    } else if (d->msg[0]) {
        Rect mr = rect_make(tb.x + tb.w + px(a, 12), cy - px(a, 4), w - pad * 2 - tb.w - px(a, 12), px(a, 40));
        results_draw_wrapped(a->font_small, r, d->msg, mr, color_hex(d->msg_ok ? T_GREEN : T_RED), 0, !live);
        ui_record("conn:msg", mr);
    }
    cy += px(a, 50);
    draw_rect(r, rect_make(x + pad, cy - px(a, 8), w - pad * 2, px(a, 1)), color_hex(T_BORDER));
    /* actions: Delete apart on the left; Cancel, Save, then the primary Connect on the right */
    float bh = px(a, 32), by = cy + px(a, 4);
    if (d->target) {
        Rect del = rect_make(x + pad, by, px(a, 120), bh); ui_record("conn:delete", del);
        if (ui_button(a, del, d->confirm_delete ? "Confirm delete" : "Delete", BTN_DANGER, true) && live) {
            if (!d->confirm_delete) d->confirm_delete = 1;
            else {
                Connection *c = d->target;
                if (a->active == c) app_disconnect(a, c);
                if (a->pending_activate == c) a->pending_activate = NULL;   /* no stale pointer to the freed connection */
                conns_remove(a->conns, c);
                d->target = NULL;
                close_dialog(d);
                app_connections_changed(a);
                app_status(a, "Connection deleted");
                return;
            }
        }
    }
    const char *prim = d->target && d->target->db ? "Save & Reconnect" : "Save & Connect";
    float pw = px(a, 150), bw = px(a, 90), bx = x + w - pad;
    Rect pb = rect_make(bx - pw, by, pw, bh); bx -= pw + px(a, 8);
    Rect sb = rect_make(bx - bw, by, bw, bh); bx -= bw + px(a, 8);
    Rect cb = rect_make(bx - bw, by, bw, bh);
    if (ui_button(a, cb, "Cancel", BTN_NORMAL, true) && live) { close_dialog(d); return; }
    if (ui_button(a, sb, "Save", BTN_NORMAL, true) && live) { save(d, a, false); if (!d->open) return; }
    if (ui_button(a, pb, prim, BTN_PRIMARY, true) && live) { save(d, a, true); if (!d->open) return; }
    if (live) {   /* modal: nothing below reacts to this frame's input */
        in->mouse_consumed = true;
        in->nkeys = 0;
        in->ntext = 0;
        if (!rect_contains(panel, in->mouse_x, in->mouse_y)) a->want_cursor = CURSOR_ARROW;
    }
}

/* See dialog.h. */
void dialog_update(Dialog *d, App *a) {
    if (!d->open) return;
    frame(d, a, true);
}

/* See dialog.h. Repaints with input neutralised, then the caret of the focused field. */
void dialog_draw(Dialog *d, App *a) {
    if (!d->open) return;
    SDL_Renderer *r = a->renderer;
    Input saved = a->in;             /* draw-only pass: widgets must not consume input twice */
    a->in.mouse_pressed = false; a->in.nkeys = 0; a->in.ntext = 0; a->in.wheel_y = 0;
    frame(d, a, false);
    a->in = saved;
    if (!d->open) return;
    TextField *f = d->step == 0 ? &d->search : field_at(d, d->focus);
    bool on = ((SDL_GetTicks() - f->blink) / 530) % 2 == 0;
    char shown[1024];
    if (f->password) { size_t p = 0; for (int i = 0; i < f->len && p + 3 < sizeof shown;) { uint32_t cp; i = utf8_next(f->buf, f->len, i, &cp); memcpy(shown + p, "\xE2\x80\xA2", 3); p += 3; } shown[p] = 0; }
    else snprintf(shown, sizeof shown, "%s", f->buf);
    int cb = f->cursor;
    if (f->password) { int cps = 0; for (int i = 0; i < f->cursor;) { uint32_t cp; i = utf8_next(f->buf, f->len, i, &cp); cps++; } cb = cps * 3; }
    float cx = f->rect.x + px(a, 10) + font_width(a->font_ui, shown, cb);
    if (on) draw_rect(r, rect_make(cx, f->rect.y + px(a, 7), px(a, 1.5f), f->rect.h - px(a, 14)), color_hex(T_ACCENT));
    app_wake_in(a, 530 - (Uint32)((SDL_GetTicks() - f->blink) % 530));
}

/* See dialog.h. Never prints a field value (a password could be one). */
void dialog_dump(Dialog *d) {
    const DriverInfo *di = drv(d);
    int nkept = 0; for (int i = 0; i < di->nfields; i++) if (d->fields[i].buf[0]) nkept++;
    printf("dialogstate open=%d step=%d edit=%d focus=%d driver=%s filled=%d/%d env=%d ro=%d ssh=%d testing=%d msg_ok=%d msg=\"%s\"\n", d->open, d->step, d->target != NULL, d->focus, di->id,
           nkept, di->nfields, d->draft.env, d->draft.read_only, d->draft.ssh, d->job != NULL, d->msg_ok, d->msg);
    fflush(stdout);
}
