/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram model: see dgm.h. */
#include "dgm.h"
#include "json.h"
#include "jw.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GRID_CELL 512.0f

/* ---------------------------------------------------------------- helpers */
void dgm_key_of(const SchIdx *x, int ti, char *out, size_t n) {
    if (!x || ti < 0 || ti >= x->n) { if (n) out[0] = 0; return; }
    const SxTable *t = &x->t[ti];
    if (t->schema && t->schema[0]) snprintf(out, n, "%s.%s", t->schema, t->name);
    else snprintf(out, n, "%s", t->name);
}
bool dgm_col_in(const char *list, const char *col) {
    if (!list || !col || !*col) return false;
    size_t cl = strlen(col);
    for (const char *p = list; *p; ) {
        while (*p == ' ') p++;
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        while (l && p[l - 1] == ' ') l--;
        if (l == cl && !strncasecmp(p, col, cl)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}
static int list_count(const char *list) { if (!list || !*list) return 0; int n = 1; for (const char *p = list; *p; p++) n += *p == ','; return n; }
/* the same set of names, in any order */
static bool same_set(const char *a, const char *b) {
    if (list_count(a) != list_count(b) || !list_count(a)) return false;
    for (const char *p = a; *p; ) {
        const char *e = strchr(p, ','); size_t l = e ? (size_t)(e - p) : strlen(p);
        char c[200]; snprintf(c, sizeof c, "%.*s", (int)(l < 199 ? l : 199), p);
        char *s = c; while (*s == ' ') s++;
        if (!dgm_col_in(b, s)) return false;
        if (!e) break;
        p = e + 1;
    }
    return true;
}
static void *grow(void *p, int *cap, int need, size_t sz) {
    if (need <= *cap) return p;
    int c = *cap ? *cap : 8; while (c < need) c *= 2;
    void *q = realloc(p, sz * (size_t)c);
    if (!q) return p;
    *cap = c;
    return q;
}

/* ---------------------------------------------------------------- life */
void dgm_init(Dgm *d) {
    memset(d, 0, sizeof *d);
    d->format = 1; d->mode = DM_KEYS; d->zoom = 1; d->show_types = true; d->show_virtual = true;
}
static void undo_clear(Dgm *d) {
    for (int i = 0; i < d->nundo; i++) { free(d->undo[i].keys); free(d->undo[i].xy); }
    d->nundo = d->iundo = 0;
}
void dgm_free(Dgm *d) {
    free(d->n); free(d->rels); free(d->notes); free(d->hidden); free(d->e);
    free(d->rev); free(d->rev_start); free(d->cell_start); free(d->cell_items);
    undo_clear(d);
    memset(d, 0, sizeof *d);
}

/* ---------------------------------------------------------------- catalog binding */
static void build_rev(Dgm *d) {
    free(d->rev); free(d->rev_start); d->rev = d->rev_start = NULL; d->rev_n = 0;
    const SchIdx *x = d->x; if (!x || !x->n) return;
    d->rev_start = calloc((size_t)x->n + 1, sizeof(int));
    int total = 0;
    for (int i = 0; i < x->n; i++) for (int k = 0; k < x->t[i].nfks; k++) { int r = x->t[i].fks[k].ref; if (r >= 0 && r != i) { d->rev_start[r + 1]++; total++; } }
    for (int i = 0; i < x->n; i++) d->rev_start[i + 1] += d->rev_start[i];
    d->rev = malloc(sizeof(int) * (size_t)(total ? total : 1));
    int *fill = calloc((size_t)x->n, sizeof(int));
    for (int i = 0; i < x->n; i++) for (int k = 0; k < x->t[i].nfks; k++) {
        int r = x->t[i].fks[k].ref;
        if (r < 0 || r == i) continue;
        int pos = d->rev_start[r] + fill[r]++;
        /* one entry per referencing table (several FKs from the same table count once) */
        bool dup = false; for (int z = d->rev_start[r]; z < pos; z++) if (d->rev[z] == i) dup = true;
        if (dup) { fill[r]--; continue; }
        d->rev[pos] = i;
    }
    /* compact: entries past fill[r] are unused */
    int w = 0; int *ns = calloc((size_t)x->n + 1, sizeof(int));
    for (int r = 0; r < x->n; r++) { ns[r] = w; for (int z = 0; z < fill[r]; z++) d->rev[w++] = d->rev[d->rev_start[r] + z]; }
    ns[x->n] = w;
    free(d->rev_start); d->rev_start = ns; d->rev_n = w;
    free(fill);
}
void dgm_bind(Dgm *d, const SchIdx *x) {
    d->x = x;
    build_rev(d);
    for (int i = 0; i < d->nn; i++) {
        DgNode *n = &d->n[i];
        n->ti = -1;
        if (n->kind == DN_ENTITY) continue;
        if (x) n->ti = schidx_find(x, NULL, n->key);
        if (n->kind == DN_TABLE && n->ti < 0 && x) n->status = DS_MISSING;
    }
    dgm_edges(d);
}

/* ---------------------------------------------------------------- nodes */
int dgm_find_node(const Dgm *d, const char *key) {
    for (int i = 0; i < d->nn; i++) if (!strcasecmp(d->n[i].key, key)) return i;
    return -1;
}
static int add_node(Dgm *d, int kind, const char *key, float x, float y) {
    int i = dgm_find_node(d, key);
    if (i >= 0) { if (d->n[i].kind == DN_STUB && kind == DN_TABLE) d->n[i].kind = DN_TABLE; return i; }
    d->n = grow(d->n, &d->capn, d->nn + 1, sizeof *d->n);
    if (d->nn >= d->capn) return -1;
    DgNode *n = &d->n[d->nn];
    memset(n, 0, sizeof *n);
    snprintf(n->key, sizeof n->key, "%s", key);
    n->kind = kind; n->x = x; n->y = y; n->w = 180; n->h = DG_HEAD_H; n->mode = DM_GLOBAL; n->ti = -1;
    d->dirty = true;
    return d->nn++;
}
int dgm_add_table(Dgm *d, int ti, float x, float y) {
    if (!d->x || ti < 0 || ti >= d->x->n) return -1;
    char k[DG_KEY]; dgm_key_of(d->x, ti, k, sizeof k);
    int i = add_node(d, DN_TABLE, k, x, y);
    if (i >= 0) { d->n[i].ti = ti; d->n[i].status = DS_NONE; }
    return i;
}
int dgm_add_entity(Dgm *d, const char *name, float x, float y) {
    char k[DG_KEY]; snprintf(k, sizeof k, "entity:%s", name);
    return add_node(d, DN_ENTITY, k, x, y);
}
void dgm_remove(Dgm *d, int node) {
    if (node < 0 || node >= d->nn) return;
    memmove(&d->n[node], &d->n[node + 1], sizeof *d->n * (size_t)(d->nn - node - 1));
    d->nn--;
    d->dirty = true;
}
int dgm_catalog_neighbours(const Dgm *d, int ti, int *out, int max) {
    const SchIdx *x = d->x; int n = 0;
    if (!x || ti < 0 || ti >= x->n) return 0;
    const SxTable *t = &x->t[ti];
    for (int k = 0; k < t->nfks && n < max; k++) {
        int r = t->fks[k].ref; if (r < 0 || r == ti) continue;
        bool dup = false; for (int z = 0; z < n; z++) if (out[z] == r) dup = true;
        if (!dup) out[n++] = r;
    }
    if (d->rev_start) for (int z = d->rev_start[ti]; z < d->rev_start[ti + 1] && n < max; z++) {
        int r = d->rev[z]; bool dup = false; for (int q = 0; q < n; q++) if (out[q] == r) dup = true;
        if (!dup) out[n++] = r;
    }
    return n;
}
int dgm_expand(Dgm *d, int ti, int hops, int budget, bool dry) {
    const SchIdx *x = d->x; if (!x || ti < 0 || ti >= x->n || hops < 1) return 0;
    int *dist = malloc(sizeof(int) * (size_t)x->n), *q = malloc(sizeof(int) * (size_t)x->n);
    for (int i = 0; i < x->n; i++) dist[i] = -1;
    int qh = 0, qt = 0, added = 0;
    dist[ti] = 0; q[qt++] = ti;
    int nb[4096];
    /* where new cards go: around the origin card */
    int on = -1; for (int i = 0; i < d->nn; i++) if (d->n[i].ti == ti) on = i;
    float ox = on >= 0 ? d->n[on].x : 0, oy = on >= 0 ? d->n[on].y : 0;
    while (qh < qt) {
        int c = q[qh++];
        if (dist[c] >= hops) continue;
        int nn = dgm_catalog_neighbours(d, c, nb, 4096);
        for (int k = 0; k < nn; k++) {
            int r = nb[k]; if (dist[r] >= 0) continue;
            dist[r] = dist[c] + 1; q[qt++] = r;
            char key[DG_KEY]; dgm_key_of(x, r, key, sizeof key);
            if (dgm_find_node(d, key) >= 0) continue;
            if (budget > 0 && added >= budget) { if (dry) { added++; continue; } goto done; }
            if (!dry) dgm_add_table(d, r, ox + 260.0f * (float)dist[r], oy + 40.0f * (float)added);
            added++;
        }
    }
done:
    free(dist); free(q);
    if (!dry && added) dgm_edges(d);
    return added;
}
void dgm_open_focus(Dgm *d, int ti, int cap) {
    if (!d->x || ti < 0) return;
    dgm_key_of(d->x, ti, d->focus, sizeof d->focus);
    dgm_add_table(d, ti, 0, 0);
    dgm_expand(d, ti, 1, cap, false);
    dgm_edges(d);
}
typedef struct { int t, deg; const char *name; } DegItem;
static int deg_cmp(const void *a, const void *b) {
    const DegItem *x = a, *y = b;
    if (x->deg != y->deg) return y->deg - x->deg;
    return strcasecmp(x->name, y->name);
}
int dgm_most_connected(const SchIdx *x, const char *schema, int *out, int max) {
    if (!x || !x->n) return 0;
    DegItem *it = malloc(sizeof *it * (size_t)x->n); int n = 0;
    for (int i = 0; i < x->n; i++) {
        if (schema && *schema && strcasecmp(x->t[i].schema ? x->t[i].schema : "", schema)) continue;
        it[n].t = i; it[n].deg = x->t[i].nfks + x->t[i].nrefs; it[n].name = x->t[i].name; n++;
    }
    qsort(it, (size_t)n, sizeof *it, deg_cmp);
    int k = 0; for (; k < n && k < max; k++) out[k] = it[k].t;
    free(it);
    return k;
}

/* ---------------------------------------------------------------- user relations, notes, hidden edges */
int dgm_add_rel(Dgm *d, int kind, const char *from, const char *to, const char *cols, const char *ref_cols, const char *label) {
    d->rels = grow(d->rels, &d->caprels, d->nrels + 1, sizeof *d->rels);
    if (d->nrels >= d->caprels) return -1;
    DgRel *r = &d->rels[d->nrels];
    memset(r, 0, sizeof *r);
    r->kind = kind;
    snprintf(r->from, sizeof r->from, "%s", from ? from : ""); snprintf(r->to, sizeof r->to, "%s", to ? to : "");
    snprintf(r->cols, sizeof r->cols, "%s", cols ? cols : ""); snprintf(r->ref_cols, sizeof r->ref_cols, "%s", ref_cols ? ref_cols : "");
    snprintf(r->label, sizeof r->label, "%s", label && *label ? label : kind == DE_LOGICAL ? "relates to" : kind == DE_SUGGESTED ? "suggested" : "virtual");
    r->card_from = DC_MANY; r->card_to = DC_ONE;   /* drawn from the referencing side; editable for logical ones */
    d->dirty = true;
    return d->nrels++;
}
void dgm_remove_rel(Dgm *d, int r) {
    if (r < 0 || r >= d->nrels) return;
    memmove(&d->rels[r], &d->rels[r + 1], sizeof *d->rels * (size_t)(d->nrels - r - 1));
    d->nrels--; d->dirty = true;
}
bool dgm_edge_hidden(const Dgm *d, const char *name) {
    if (!name || !*name) return false;
    for (int i = 0; i < d->nhidden; i++) if (!strcmp(d->hidden[i], name)) return true;
    return false;
}
void dgm_toggle_hidden(Dgm *d, const char *name) {
    if (!name || !*name) return;
    for (int i = 0; i < d->nhidden; i++) if (!strcmp(d->hidden[i], name)) { memmove(&d->hidden[i], &d->hidden[i + 1], sizeof *d->hidden * (size_t)(d->nhidden - i - 1)); d->nhidden--; d->dirty = true; dgm_edges(d); return; }
    char (*h)[160] = realloc(d->hidden, sizeof *d->hidden * (size_t)(d->nhidden + 1));
    if (!h) return;
    d->hidden = h; snprintf(d->hidden[d->nhidden++], 160, "%s", name);
    d->dirty = true;
    dgm_edges(d);
}
int dgm_add_note(Dgm *d, bool area, float x, float y, float w, float h, const char *title, const char *text) {
    d->notes = grow(d->notes, &d->capnotes, d->nnotes + 1, sizeof *d->notes);
    if (d->nnotes >= d->capnotes) return -1;
    DgNote *n = &d->notes[d->nnotes];
    memset(n, 0, sizeof *n);
    n->area = area; n->x = x; n->y = y; n->w = w; n->h = h; n->color = area ? 1 : 0;
    snprintf(n->title, sizeof n->title, "%s", title ? title : "");
    snprintf(n->text, sizeof n->text, "%s", text ? text : "");
    d->dirty = true;
    return d->nnotes++;
}

/* ---------------------------------------------------------------- cards */
int dgm_card_mode(const Dgm *d, int node) {
    if (node < 0 || node >= d->nn) return d->mode;
    int m = d->n[node].mode;
    return m == DM_GLOBAL ? d->mode : m;
}
static bool col_is_fk(const SxTable *t, const char *col) { for (int k = 0; k < t->nfks; k++) if (dgm_col_in(t->fks[k].cols, col)) return true; return false; }
static bool col_is_uk(const SxTable *t, const char *col) {
    if (!t->uniques) return false;
    for (const char *p = t->uniques; *p; ) {
        const char *e = strchr(p, ';'); size_t l = e ? (size_t)(e - p) : strlen(p);
        char set[600]; snprintf(set, sizeof set, "%.*s", (int)(l < 599 ? l : 599), p);
        if (dgm_col_in(set, col)) return true;
        if (!e) break;
        p = e + 1;
    }
    return false;
}
int dgm_card_rows(const Dgm *d, int node, int *rows, int max) {
    if (node < 0 || node >= d->nn) return 0;
    const DgNode *n = &d->n[node];
    int m = dgm_card_mode(d, node), k = 0;
    if (m == DM_NAME) return 0;
    if (n->kind == DN_ENTITY) {   /* attribute lines */
        int line = 0;
        for (const char *p = n->attrs; *p && k < max; line++) {
            const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
            bool key = l > 3 && !strncasecmp(p, "PK ", 3);
            if (l && (m == DM_ALL || key)) rows[k++] = line;
            if (!e) break;
            p = e + 1;
        }
        return k;
    }
    if (!d->x || n->ti < 0) return 0;
    const SxTable *t = &d->x->t[n->ti];
    for (int c = 0; c < t->ncols && k < max; c++) {
        const SxCol *col = &t->cols[c];
        if (m == DM_ALL || col->pk || col_is_fk(t, col->name) || col_is_uk(t, col->name)) rows[k++] = c;
    }
    return k;
}
const char *dgm_card_label(const Dgm *d, int node) {
    const DgNode *n = &d->n[node];
    if (d->conceptual && n->alias[0]) return n->alias;
    if (n->kind == DN_ENTITY) return n->key + 7;   /* after "entity:" */
    if (d->x && n->ti >= 0) {
        const SxTable *t = &d->x->t[n->ti];
        /* the schema only when the diagram spans several schemas */
        bool multi = strchr(d->schemas, ',') != NULL;
        return multi ? n->key : t->name;
    }
    return n->key;
}
static float default_w(void *ud, const char *s) { (void)ud; return 7.0f * (float)strlen(s); }
void dgm_measure(Dgm *d, int node, float (*tw)(void *ud, const char *s), void *ud) {
    if (node < 0 || node >= d->nn) return;
    if (!tw) tw = default_w;
    DgNode *n = &d->n[node];
    float w = tw(ud, dgm_card_label(d, node)) + 56;   /* icon, padding, "+N" */
    int rows[512]; int nr = dgm_card_rows(d, node, rows, 512);
    if (n->kind == DN_TABLE && d->x && n->ti >= 0) {
        const SxTable *t = &d->x->t[n->ti];
        for (int i = 0; i < nr; i++) {
            const SxCol *c = &t->cols[rows[i]];
            char line[400]; snprintf(line, sizeof line, "%s  %s", c->name, d->show_types && c->type ? c->type : "");
            float lw = tw(ud, line) + 46;
            if (lw > w) w = lw;
        }
    } else if (n->kind == DN_ENTITY) {
        for (const char *p = n->attrs; *p; ) {
            const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
            char s[300]; snprintf(s, sizeof s, "%.*s", (int)(l < 299 ? l : 299), p);
            float lw = tw(ud, s) + 46; if (lw > w) w = lw;
            if (!e) break;
            p = e + 1;
        }
    }
    if (w < 150) w = 150;
    if (w > 420) w = 420;
    int shown = nr > DG_MAX_ROWS ? DG_MAX_ROWS : nr;
    n->w = roundf(w);
    n->h = DG_HEAD_H + (float)shown * DG_ROW_H + (shown ? 8 : 0);
    if (n->kind == DN_STUB) { n->h = DG_HEAD_H; }
}
void dgm_measure_all(Dgm *d, float (*tw)(void *ud, const char *s), void *ud) { for (int i = 0; i < d->nn; i++) dgm_measure(d, i, tw, ud); d->grid_ok = false; }

/* ---------------------------------------------------------------- edges */
static int cardinality_from(const SxTable *t, const char *cols) {
    if (!t->ncols) return DC_UNKNOWN;
    /* one when the FK columns are exactly the PK or a unique key */
    char pk[600] = ""; size_t w = 0;
    for (int c = 0; c < t->ncols; c++) if (t->cols[c].pk) w += (size_t)snprintf(pk + w, sizeof pk - w, "%s%s", w ? "," : "", t->cols[c].name);
    if (pk[0] && same_set(pk, cols)) return DC_ONE;
    if (t->uniques) for (const char *p = t->uniques; *p; ) {
        const char *e = strchr(p, ';'); size_t l = e ? (size_t)(e - p) : strlen(p);
        char set[600]; snprintf(set, sizeof set, "%.*s", (int)(l < 599 ? l : 599), p);
        if (same_set(set, cols)) return DC_ONE;
        if (!e) break;
        p = e + 1;
    }
    return DC_MANY;
}
static bool fk_optional(const SxTable *t, const char *cols) {
    for (int c = 0; c < t->ncols; c++) if (dgm_col_in(cols, t->cols[c].name) && !t->cols[c].notnull && !t->cols[c].pk) return true;
    return false;
}
static int push_edge(Dgm *d) {
    d->e = grow(d->e, &d->cape, d->ne + 1, sizeof *d->e);
    if (d->ne >= d->cape) return -1;
    memset(&d->e[d->ne], 0, sizeof *d->e);
    d->e[d->ne].rel = -1; d->e[d->ne].fk_t = -1;
    return d->ne++;
}
void dgm_edges(Dgm *d) {
    bool was_dirty = d->dirty;   /* deriving edges (and stubs) never makes the file dirty */
    d->ne = 0;
    const SchIdx *x = d->x;
    /* stubs come back only when still referenced */
    for (int i = d->nn - 1; i >= 0; i--) if (d->n[i].kind == DN_STUB) dgm_remove(d, i);
    int *node_of = NULL;
    if (x && x->n) { node_of = malloc(sizeof(int) * (size_t)x->n); for (int i = 0; i < x->n; i++) node_of[i] = -1; }
    for (int i = 0; i < d->nn; i++) if (node_of && d->n[i].ti >= 0 && d->n[i].ti < x->n) node_of[d->n[i].ti] = i;
    int nn0 = d->nn;
    for (int i = 0; i < nn0; i++) {
        DgNode *n = &d->n[i];
        n->more = 0;
        if (n->kind != DN_TABLE || !x || n->ti < 0) continue;
        const SxTable *t = &x->t[n->ti];
        for (int k = 0; k < t->nfks; k++) {
            const SxFk *f = &t->fks[k];
            int to;
            if (f->ref >= 0) {
                to = node_of[f->ref];
                if (to < 0) { d->n[i].more++; continue; }   /* in the catalog, not on the diagram */
            } else {
                /* outside the scope or unreadable: an external reference stub */
                int s = dgm_find_node(d, f->ref_table ? f->ref_table : "?");
                if (s < 0) {
                    s = add_node(d, DN_STUB, f->ref_table ? f->ref_table : "?", d->n[i].x + d->n[i].w + 80, d->n[i].y);
                    if (s >= 0) d->n[s].w = 180;
                }
                if (s < 0) continue;
                to = s;
            }
            int e = push_edge(d); if (e < 0) continue;
            DgEdge *g = &d->e[e];
            g->from = i; g->to = to; g->origin = DE_FK; g->fk_t = n->ti; g->fk_k = k;
            snprintf(g->name, sizeof g->name, "%s", f->name && f->name[0] ? f->name : "");
            snprintf(g->cols, sizeof g->cols, "%s", f->cols ? f->cols : "");
            snprintf(g->ref_cols, sizeof g->ref_cols, "%s", f->ref_cols ? f->ref_cols : "");
            if (!g->name[0]) snprintf(g->name, sizeof g->name, "%s(%s)", t->name, g->cols);
            g->card_from = cardinality_from(t, g->cols);
            g->card_to = f->ref >= 0 && !x->t[f->ref].ncols ? DC_UNKNOWN : DC_ONE;
            g->optional = fk_optional(t, g->cols);
            g->hidden = dgm_edge_hidden(d, g->name);
        }
        /* tables referencing this one that are not on the diagram */
        if (d->rev_start) for (int z = d->rev_start[n->ti]; z < d->rev_start[n->ti + 1]; z++) if (node_of[d->rev[z]] < 0) n->more++;
    }
    for (int r = 0; r < d->nrels; r++) {
        DgRel *rl = &d->rels[r];
        int a = dgm_find_node(d, rl->from), b = dgm_find_node(d, rl->to);
        if (a < 0 || b < 0) continue;
        if (!d->show_virtual && rl->kind != DE_LOGICAL) continue;
        if (d->conceptual && rl->kind != DE_LOGICAL && d->n[a].kind != DN_ENTITY) { /* still drawn: virtual relations document tables too */ }
        int e = push_edge(d); if (e < 0) continue;
        DgEdge *g = &d->e[e];
        g->from = a; g->to = b; g->origin = rl->kind; g->rel = r;
        snprintf(g->name, sizeof g->name, "%s", rl->label);
        snprintf(g->cols, sizeof g->cols, "%s", rl->cols); snprintf(g->ref_cols, sizeof g->ref_cols, "%s", rl->ref_cols);
        g->card_from = rl->card_from; g->card_to = rl->card_to; g->optional = rl->optional;
        char hk[160]; snprintf(hk, sizeof hk, "rel:%s>%s:%s", rl->from, rl->to, rl->label);
        g->hidden = dgm_edge_hidden(d, hk);
    }
    /* parallel edges between the same two nodes are offset */
    for (int i = 0; i < d->ne; i++) {
        int cnt = 0, idx = 0;
        for (int j = 0; j < d->ne; j++) {
            bool same = (d->e[j].from == d->e[i].from && d->e[j].to == d->e[i].to) || (d->e[j].from == d->e[i].to && d->e[j].to == d->e[i].from);
            if (same) { if (j < i) idx++; cnt++; }
        }
        d->e[i].parallel = idx; d->e[i].nparallel = cnt;
    }
    free(node_of);
    d->grid_ok = false;
    d->dirty = was_dirty;
}

/* ---------------------------------------------------------------- spatial grid */
void dgm_grid_build(Dgm *d) {
    free(d->cell_start); free(d->cell_items); d->cell_start = d->cell_items = NULL;
    d->grid_ok = true;
    if (!d->nn) { d->gw = d->gh = 0; return; }
    float x0 = 1e30f, y0 = 1e30f, x1 = -1e30f, y1 = -1e30f;
    for (int i = 0; i < d->nn; i++) { x0 = fminf(x0, d->n[i].x); y0 = fminf(y0, d->n[i].y); x1 = fmaxf(x1, d->n[i].x + d->n[i].w); y1 = fmaxf(y1, d->n[i].y + d->n[i].h); }
    d->gx0 = (int)floorf(x0 / GRID_CELL); d->gy0 = (int)floorf(y0 / GRID_CELL);
    d->gw = (int)floorf(x1 / GRID_CELL) - d->gx0 + 1; d->gh = (int)floorf(y1 / GRID_CELL) - d->gy0 + 1;
    if (d->gw < 1) d->gw = 1;
    if (d->gh < 1) d->gh = 1;
    if ((long long)d->gw * d->gh > 4000000) { d->gw = d->gh = 0; return; }   /* absurd spread: linear scan */
    int nc = d->gw * d->gh;
    int *count = calloc((size_t)nc + 1, sizeof(int));
    int total = 0;
    for (int i = 0; i < d->nn; i++) {
        int cx0 = (int)floorf(d->n[i].x / GRID_CELL) - d->gx0, cx1 = (int)floorf((d->n[i].x + d->n[i].w) / GRID_CELL) - d->gx0;
        int cy0 = (int)floorf(d->n[i].y / GRID_CELL) - d->gy0, cy1 = (int)floorf((d->n[i].y + d->n[i].h) / GRID_CELL) - d->gy0;
        for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) { count[cy * d->gw + cx + 1]++; total++; }
    }
    for (int c = 0; c < nc; c++) count[c + 1] += count[c];
    d->cell_start = count;
    d->cell_items = malloc(sizeof(int) * (size_t)(total ? total : 1));
    int *fill = calloc((size_t)nc, sizeof(int));
    for (int i = 0; i < d->nn; i++) {
        int cx0 = (int)floorf(d->n[i].x / GRID_CELL) - d->gx0, cx1 = (int)floorf((d->n[i].x + d->n[i].w) / GRID_CELL) - d->gx0;
        int cy0 = (int)floorf(d->n[i].y / GRID_CELL) - d->gy0, cy1 = (int)floorf((d->n[i].y + d->n[i].h) / GRID_CELL) - d->gy0;
        for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) { int c = cy * d->gw + cx; d->cell_items[d->cell_start[c] + fill[c]++] = i; }
    }
    free(fill);
}
int dgm_grid_query(const Dgm *d, float x0, float y0, float x1, float y1, int *out, int max) {
    int n = 0;
    if (!d->grid_ok || !d->cell_start || !d->gw) {   /* linear fallback */
        for (int i = 0; i < d->nn && n < max; i++) if (d->n[i].x < x1 && d->n[i].x + d->n[i].w > x0 && d->n[i].y < y1 && d->n[i].y + d->n[i].h > y0) out[n++] = i;
        return n;
    }
    int cx0 = (int)floorf(x0 / GRID_CELL) - d->gx0, cx1 = (int)floorf(x1 / GRID_CELL) - d->gx0;
    int cy0 = (int)floorf(y0 / GRID_CELL) - d->gy0, cy1 = (int)floorf(y1 / GRID_CELL) - d->gy0;
    if (cx0 < 0) cx0 = 0;
    if (cy0 < 0) cy0 = 0;
    if (cx1 >= d->gw) cx1 = d->gw - 1;
    if (cy1 >= d->gh) cy1 = d->gh - 1;
    unsigned char *seen = calloc((size_t)d->nn, 1);
    for (int cy = cy0; cy <= cy1; cy++) for (int cx = cx0; cx <= cx1; cx++) {
        int c = cy * d->gw + cx;
        for (int z = d->cell_start[c]; z < d->cell_start[c + 1] && n < max; z++) {
            int i = d->cell_items[z];
            if (seen[i]) continue;
            seen[i] = 1;
            if (d->n[i].x < x1 && d->n[i].x + d->n[i].w > x0 && d->n[i].y < y1 && d->n[i].y + d->n[i].h > y0) out[n++] = i;
        }
    }
    free(seen);
    /* deterministic order: node index */
    for (int i = 1; i < n; i++) { int v = out[i], j = i - 1; while (j >= 0 && out[j] > v) { out[j + 1] = out[j]; j--; } out[j + 1] = v; }
    return n;
}
int dgm_hit(const Dgm *d, float wx, float wy) {
    int c[64]; int n = dgm_grid_query(d, wx - 1, wy - 1, wx + 1, wy + 1, c, 64);
    return n ? c[n - 1] : -1;   /* the last drawn is on top */
}
