/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram model: layout, alignment, components, find, undo and structure comparison (dgm.h). */
#include "dgm.h"
#include <ctype.h>
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>

#define GAP_X 110.0f
#define GAP_Y 36.0f
#define GAP_BLOCK 140.0f

/* ---------------------------------------------------------------- layout */
typedef struct { int node, layer, comp; float order; } LNode;
static const Dgm *g_sort_d;
static LNode *g_sort_l;
static int key_cmp_idx(const void *a, const void *b) { return strcasecmp(g_sort_d->n[*(const int *)a].key, g_sort_d->n[*(const int *)b].key); }
static int order_cmp(const void *a, const void *b) {
    const LNode *x = &g_sort_l[*(const int *)a], *y = &g_sort_l[*(const int *)b];
    if (x->order != y->order) return x->order < y->order ? -1 : 1;
    return strcasecmp(g_sort_d->n[x->node].key, g_sort_d->n[y->node].key);
}

bool dgm_layout(Dgm *d, bool only_sel, volatile int *cancel) {
    int N = d->nn; if (!N) return true;
    bool *mv = calloc((size_t)N, 1); int nm = 0;
    for (int i = 0; i < N; i++) { mv[i] = !d->n[i].pinned && (!only_sel || d->n[i].sel); nm += mv[i]; }
    if (!nm) { free(mv); return true; }
    /* adjacency among movable nodes (undirected, with the FK direction kept for layering) */
    int *deg = calloc((size_t)N + 1, sizeof(int));
    for (int e = 0; e < d->ne; e++) { const DgEdge *g = &d->e[e]; if (g->hidden || g->from == g->to || !mv[g->from] || !mv[g->to]) continue; deg[g->from + 1]++; deg[g->to + 1]++; }
    for (int i = 0; i < N; i++) deg[i + 1] += deg[i];
    int tot = deg[N]; int *adj = malloc(sizeof(int) * (size_t)(tot ? tot : 1)), *dir = malloc(sizeof(int) * (size_t)(tot ? tot : 1)), *fill = calloc((size_t)N, sizeof(int));
    for (int e = 0; e < d->ne; e++) {
        const DgEdge *g = &d->e[e]; if (g->hidden || g->from == g->to || !mv[g->from] || !mv[g->to]) continue;
        int a = g->from, b = g->to;
        adj[deg[a] + fill[a]] = b; dir[deg[a] + fill[a]] = +1; fill[a]++;   /* referencing -> referenced: one layer right */
        adj[deg[b] + fill[b]] = a; dir[deg[b] + fill[b]] = -1; fill[b]++;
    }
    free(fill);
    LNode *L = calloc((size_t)N, sizeof *L);
    for (int i = 0; i < N; i++) { L[i].node = i; L[i].comp = -1; }
    /* components, each from its root: the focus when it is in it, else the most connected (then by key) */
    int *ord = malloc(sizeof(int) * (size_t)N), no = 0;
    for (int i = 0; i < N; i++) if (mv[i]) ord[no++] = i;
    g_sort_d = d; qsort(ord, (size_t)no, sizeof(int), key_cmp_idx);
    int *q = malloc(sizeof(int) * (size_t)N), ncomp = 0;
    int focus = d->focus[0] ? dgm_find_node(d, d->focus) : -1;
    bool cancelled = false;
    for (int pass = 0; pass < 2; pass++)
    for (int oi = 0; oi < no; oi++) {
        int s = pass == 0 ? focus : ord[oi];
        if (pass == 0 && oi > 0) break;
        if (s < 0 || !mv[s] || L[s].comp >= 0) continue;
        if (pass == 1) {   /* the most connected node of this component is its root */
            int qh = 0, qt = 0; q[qt++] = s; L[s].comp = -2; int best = s;
            while (qh < qt) { int c = q[qh++]; if (deg[c + 1] - deg[c] > deg[best + 1] - deg[best]) best = c; for (int z = deg[c]; z < deg[c + 1]; z++) if (L[adj[z]].comp == -1) { L[adj[z]].comp = -2; q[qt++] = adj[z]; } }
            for (int z = 0; z < qt; z++) L[q[z]].comp = -1;
            s = best;
        }
        int qh = 0, qt = 0; q[qt++] = s; L[s].comp = ncomp; L[s].layer = 0;
        while (qh < qt) {
            if (cancel && *cancel) { cancelled = true; break; }
            int c = q[qh++];
            for (int z = deg[c]; z < deg[c + 1]; z++) { int o = adj[z]; if (L[o].comp >= 0) continue; L[o].comp = ncomp; L[o].layer = L[c].layer + dir[z]; q[qt++] = o; }
        }
        ncomp++;
        if (cancelled) break;
    }
    if (cancelled) { free(mv); free(deg); free(adj); free(dir); free(L); free(ord); free(q); return false; }
    /* new positions, applied only at the end */
    float *nx = malloc(sizeof(float) * (size_t)N), *ny = malloc(sizeof(float) * (size_t)N);
    for (int i = 0; i < N; i++) { nx[i] = d->n[i].x; ny[i] = d->n[i].y; }
    float ox = 0, oy = 0;
    if (only_sel) { float a, b, c, e; if (dgm_bounds(d, true, &a, &b, &c, &e)) { ox = a; oy = b; } }
    float block_y = oy;
    int *mem = malloc(sizeof(int) * (size_t)N);
    for (int c = 0; c < ncomp; c++) {
        int nmem = 0, lmin = 1 << 30, lmax = -(1 << 30);
        for (int i = 0; i < N; i++) if (mv[i] && L[i].comp == c) { mem[nmem++] = i; if (L[i].layer < lmin) lmin = L[i].layer; if (L[i].layer > lmax) lmax = L[i].layer; }
        if (!nmem) continue;
        int nl = lmax - lmin + 1;
        /* initial order: by key */
        g_sort_d = d; qsort(mem, (size_t)nmem, sizeof(int), key_cmp_idx);
        int *cnt = calloc((size_t)nl, sizeof(int));
        for (int k = 0; k < nmem; k++) L[mem[k]].order = (float)(cnt[L[mem[k]].layer - lmin]++);
        /* barycentre sweeps (left to right, right to left) */
        for (int sweep = 0; sweep < 6; sweep++) {
            if (cancel && *cancel) { cancelled = true; break; }
            bool fwd = sweep % 2 == 0;
            for (int li = 1; li < nl; li++) {
                int l = fwd ? lmin + li : lmax - li, ref = fwd ? l - 1 : l + 1;
                int lm[4096]; int nlm = 0;
                for (int k = 0; k < nmem && nlm < 4096; k++) if (L[mem[k]].layer == l) lm[nlm++] = mem[k];
                for (int k = 0; k < nlm; k++) {
                    int v = lm[k]; float sum = 0; int nb = 0;
                    for (int z = deg[v]; z < deg[v + 1]; z++) if (L[adj[z]].layer == ref && L[adj[z]].comp == c) { sum += L[adj[z]].order; nb++; }
                    if (nb) L[v].order = sum / (float)nb + (float)k * 1e-4f;
                }
                g_sort_l = L; qsort(lm, (size_t)nlm, sizeof(int), order_cmp);
                for (int k = 0; k < nlm; k++) L[lm[k]].order = (float)k;
            }
        }
        if (cancelled) { free(cnt); break; }
        /* geometry: columns by layer, each column centred on the tallest */
        float *colw = calloc((size_t)nl, sizeof(float)), *colh = calloc((size_t)nl, sizeof(float));
        for (int k = 0; k < nmem; k++) { int li = L[mem[k]].layer - lmin; DgNode *n = &d->n[mem[k]]; if (n->w > colw[li]) colw[li] = n->w; colh[li] += n->h + GAP_Y; }
        float H = 0; for (int li = 0; li < nl; li++) if (colh[li] > H) H = colh[li];
        float cx = ox;
        for (int li = 0; li < nl; li++) {
            int lm[4096]; int nlm = 0;
            for (int k = 0; k < nmem && nlm < 4096; k++) if (L[mem[k]].layer == lmin + li) lm[nlm++] = mem[k];
            g_sort_l = L; qsort(lm, (size_t)nlm, sizeof(int), order_cmp);
            float y = block_y + (H - colh[li]) / 2;
            for (int k = 0; k < nlm; k++) { DgNode *n = &d->n[lm[k]]; nx[lm[k]] = cx + (colw[li] - n->w) / 2; ny[lm[k]] = y; y += n->h + GAP_Y; }
            cx += colw[li] + GAP_X;
        }
        block_y += H + GAP_BLOCK;
        free(colw); free(colh); free(cnt);
    }
    if (!cancelled) {
        for (int i = 0; i < N; i++) if (mv[i]) {
            float x = roundf(nx[i]), y = roundf(ny[i]);
            if (d->snap) { x = roundf(x / 20) * 20; y = roundf(y / 20) * 20; }
            if (d->n[i].x != x || d->n[i].y != y) d->dirty = true;
            d->n[i].x = x; d->n[i].y = y;
        }
        d->grid_ok = false;
    }
    free(mv); free(deg); free(adj); free(dir); free(L); free(ord); free(q); free(nx); free(ny); free(mem);
    return !cancelled;
}

bool dgm_bounds(const Dgm *d, bool only_sel, float *x0, float *y0, float *x1, float *y1) {
    float a = 1e30f, b = 1e30f, c = -1e30f, e = -1e30f; bool any = false;
    for (int i = 0; i < d->nn; i++) {
        const DgNode *n = &d->n[i];
        if (only_sel && !n->sel) continue;
        a = fminf(a, n->x); b = fminf(b, n->y); c = fmaxf(c, n->x + n->w); e = fmaxf(e, n->y + n->h); any = true;
    }
    if (!only_sel) for (int i = 0; i < d->nnotes; i++) { const DgNote *n = &d->notes[i]; a = fminf(a, n->x); b = fminf(b, n->y); c = fmaxf(c, n->x + n->w); e = fmaxf(e, n->y + n->h); any = true; }
    if (!any) { *x0 = *y0 = *x1 = *y1 = 0; return false; }
    *x0 = a; *y0 = b; *x1 = c; *y1 = e;
    return true;
}

static const Dgm *g_al_d; static int g_al_axis;
static int al_cmp(const void *a, const void *b) {
    const DgNode *x = &g_al_d->n[*(const int *)a], *y = &g_al_d->n[*(const int *)b];
    float vx = g_al_axis ? x->y : x->x, vy = g_al_axis ? y->y : y->x;
    return vx < vy ? -1 : vx > vy ? 1 : strcasecmp(x->key, y->key);
}
void dgm_align(Dgm *d, int how) {
    float x0, y0, x1, y1; if (!dgm_bounds(d, true, &x0, &y0, &x1, &y1)) return;
    int *s = malloc(sizeof(int) * (size_t)d->nn), ns = 0;
    for (int i = 0; i < d->nn; i++) if (d->n[i].sel && !d->n[i].pinned) s[ns++] = i;
    if (how <= 5) for (int k = 0; k < ns; k++) {
        DgNode *n = &d->n[s[k]];
        switch (how) {
        case 0: n->x = x0; break;
        case 1: n->x = roundf((x0 + x1) / 2 - n->w / 2); break;
        case 2: n->x = x1 - n->w; break;
        case 3: n->y = y0; break;
        case 4: n->y = roundf((y0 + y1) / 2 - n->h / 2); break;
        default: n->y = y1 - n->h; break;
        }
    } else if (ns > 2) {   /* distribute: equal gaps between consecutive cards */
        g_al_d = d; g_al_axis = how == 7; qsort(s, (size_t)ns, sizeof(int), al_cmp);
        float span = 0; for (int k = 0; k < ns; k++) span += how == 6 ? d->n[s[k]].w : d->n[s[k]].h;
        float total = how == 6 ? x1 - x0 : y1 - y0, gap = (total - span) / (float)(ns - 1), p = how == 6 ? x0 : y0;
        for (int k = 0; k < ns; k++) { DgNode *n = &d->n[s[k]]; if (how == 6) { n->x = roundf(p); p += n->w + gap; } else { n->y = roundf(p); p += n->h + gap; } }
    }
    free(s);
    d->dirty = true; d->grid_ok = false;
}

int dgm_components(const Dgm *d, int *comp) {
    int N = d->nn; for (int i = 0; i < N; i++) comp[i] = -1;
    int *q = malloc(sizeof(int) * (size_t)(N ? N : 1)), nc = 0;
    for (int s = 0; s < N; s++) {
        if (comp[s] >= 0) continue;
        int qh = 0, qt = 0; q[qt++] = s; comp[s] = nc;
        while (qh < qt) {
            int c = q[qh++];
            for (int e = 0; e < d->ne; e++) {
                if (d->e[e].hidden) continue;
                int o = d->e[e].from == c ? d->e[e].to : d->e[e].to == c ? d->e[e].from : -1;
                if (o >= 0 && comp[o] < 0) { comp[o] = nc; q[qt++] = o; }
            }
        }
        nc++;
    }
    free(q);
    return nc;
}

/* ---------------------------------------------------------------- find */
static bool glob_match(const char *p, const char *s, bool cs) {
    while (*p) {
        if (*p == '*') { p++; if (!*p) return true; for (; *s; s++) if (glob_match(p, s, cs)) return true; return false; }
        if (!*s) return false;
        if (*p != '?' && (cs ? *p != *s : tolower((unsigned char)*p) != tolower((unsigned char)*s))) return false;
        p++; s++;
    }
    return !*s;
}
static bool str_match(const char *hay, const char *needle, int flags) {
    if (!hay || !needle || !*needle) return false;
    bool cs = flags & DF_CASE;
    if (flags & DF_GLOB) return glob_match(needle, hay, cs);
    if (flags & DF_EXACT) return cs ? !strcmp(hay, needle) : !strcasecmp(hay, needle);
    if (flags & DF_PREFIX) return cs ? !strncmp(hay, needle, strlen(needle)) : !strncasecmp(hay, needle, strlen(needle));
    return cs ? strstr(hay, needle) != NULL : strcasestr(hay, needle) != NULL;
}
/* a table matches by its qualified key or its bare name */
static bool table_match(const char *key, const char *name, const char *needle, int flags) { return str_match(key, needle, flags) || (name && str_match(name, needle, flags)); }

static const Dgm *g_fd;
static int hit_cmp(const void *a, const void *b) {
    const DgHit *x = a, *y = b;
    bool dx = x->node >= 0, dy = y->node >= 0;
    if (dx != dy) return dx ? -1 : 1;
    if (dx) {   /* on the diagram: reading order (rows of 200 units, then x) */
        const DgNode *p = &g_fd->n[x->node], *q = &g_fd->n[y->node];
        int rp = (int)floorf(p->y / 200), rq = (int)floorf(q->y / 200);
        if (rp != rq) return rp < rq ? -1 : 1;
        if (p->x != q->x) return p->x < q->x ? -1 : 1;
    }
    if (x->what != y->what) return x->what - y->what;
    return strcasecmp(x->text, y->text);
}
int dgm_find(const Dgm *d, const char *text, int flags, DgHit *out, int max) {
    if (!text || !*text) return 0;
    if (!(flags & (DF_TABLES | DF_COLUMNS | DF_CONSTRAINTS | DF_NOTES))) flags |= DF_TABLES;
    const SchIdx *x = d->x;
    int total = 0, n = 0;
    int *node_of = NULL;
    if (x && x->n) { node_of = malloc(sizeof(int) * (size_t)x->n); for (int i = 0; i < x->n; i++) node_of[i] = -1; for (int i = 0; i < d->nn; i++) if (d->n[i].ti >= 0 && d->n[i].ti < x->n) node_of[d->n[i].ti] = i; }
#define ADD_HIT(TI, NODE, WHAT, ...) do { total++; if (n < max) { out[n].ti = (TI); out[n].node = (NODE); out[n].what = (WHAT); snprintf(out[n].text, sizeof out[n].text, __VA_ARGS__); n++; } } while (0)
    if (x) for (int i = 0; i < x->n; i++) {
        const SxTable *t = &x->t[i];
        char key[DG_KEY]; dgm_key_of(x, i, key, sizeof key);
        int nd = node_of ? node_of[i] : -1;
        if ((flags & DF_TABLES) && table_match(key, t->name, text, flags)) ADD_HIT(i, nd, DH_TABLE, "%s", key);
        if (flags & DF_COLUMNS) for (int c = 0; c < t->ncols; c++) if (str_match(t->cols[c].name, text, flags)) ADD_HIT(i, nd, DH_COLUMN, "%s.%s", key, t->cols[c].name);
        if (flags & DF_CONSTRAINTS) for (int k = 0; k < t->nfks; k++) if (t->fks[k].name && str_match(t->fks[k].name, text, flags)) ADD_HIT(i, nd, DH_CONSTRAINT, "%s (%s)", t->fks[k].name, key);
    }
    for (int i = 0; i < d->nn; i++) {   /* what is on the diagram but not in the catalog: entities, stubs */
        const DgNode *nd = &d->n[i];
        if (nd->kind == DN_TABLE && nd->ti >= 0) continue;
        const char *nm = nd->kind == DN_ENTITY ? nd->key + 7 : nd->key;
        if ((flags & DF_TABLES) && (str_match(nm, text, flags) || (nd->alias[0] && str_match(nd->alias, text, flags)))) ADD_HIT(-1, i, DH_TABLE, "%s", nm);
    }
    if (d->conceptual || (flags & DF_TABLES)) for (int i = 0; i < d->nn; i++) {   /* business names of tables */
        const DgNode *nd = &d->n[i];
        if (nd->kind == DN_TABLE && nd->alias[0] && str_match(nd->alias, text, flags) && !table_match(nd->key, NULL, text, flags)) ADD_HIT(nd->ti, i, DH_TABLE, "%s (%s)", nd->alias, nd->key);
    }
    /* notes: ti = -2 - note index, node -1 */
    if (flags & DF_NOTES) for (int i = 0; i < d->nnotes; i++) if (str_match(d->notes[i].title, text, flags) || str_match(d->notes[i].text, text, flags)) ADD_HIT(-2 - i, -1, DH_NOTE, "%s", d->notes[i].title[0] ? d->notes[i].title : d->notes[i].text);
#undef ADD_HIT
    free(node_of);
    g_fd = d; qsort(out, (size_t)n, sizeof *out, hit_cmp);
    return total;
}

/* ---------------------------------------------------------------- undo / redo of positions */
static void take(const Dgm *d, DgUndo *u) {
    u->n = d->nn;
    u->keys = malloc(sizeof *u->keys * (size_t)(d->nn ? d->nn : 1));
    u->xy = malloc(sizeof(float) * 2 * (size_t)(d->nn ? d->nn : 1));
    for (int i = 0; i < d->nn; i++) { snprintf(u->keys[i], DG_KEY, "%s", d->n[i].key); u->xy[2 * i] = d->n[i].x; u->xy[2 * i + 1] = d->n[i].y; }
}
static void put(Dgm *d, const DgUndo *u) {
    for (int k = 0; k < u->n; k++) { int i = dgm_find_node(d, u->keys[k]); if (i >= 0) { d->n[i].x = u->xy[2 * k]; d->n[i].y = u->xy[2 * k + 1]; } }
    d->dirty = true; d->grid_ok = false;
}
static void drop(DgUndo *u) { free(u->keys); free(u->xy); u->keys = NULL; u->xy = NULL; u->n = 0; }
static void push(Dgm *d) {
    if (d->nundo == DG_UNDO) { drop(&d->undo[0]); memmove(&d->undo[0], &d->undo[1], sizeof d->undo[0] * (DG_UNDO - 1)); d->nundo--; if (d->iundo > 0) d->iundo--; }
    take(d, &d->undo[d->nundo++]);
}
void dgm_snapshot(Dgm *d) {
    while (d->nundo > d->iundo) drop(&d->undo[--d->nundo]);
    push(d);
    d->iundo = d->nundo;
}
bool dgm_undo(Dgm *d) {
    if (d->iundo == d->nundo) { if (!d->nundo) return false; push(d); d->iundo = d->nundo - 1; }
    if (d->iundo <= 0) return false;
    d->iundo--;
    put(d, &d->undo[d->iundo]);
    return true;
}
bool dgm_redo(Dgm *d) {
    if (d->iundo >= d->nundo - 1) return false;
    d->iundo++;
    put(d, &d->undo[d->iundo]);
    if (d->iundo == d->nundo - 1) { drop(&d->undo[--d->nundo]); d->iundo = d->nundo; }   /* back at the present */
    return true;
}

/* ---------------------------------------------------------------- structure digest and comparison */
void dgm_table_sig(const SchIdx *x, int ti, char *out, size_t n) {
    unsigned long long h = 1469598103934665603ULL;
#define MIX(s) do { for (const char *q_ = (s) ? (s) : ""; *q_; q_++) { h ^= (unsigned char)tolower((unsigned char)*q_); h *= 1099511628211ULL; } h ^= '|'; h *= 1099511628211ULL; } while (0)
    if (x && ti >= 0 && ti < x->n) {
        const SxTable *t = &x->t[ti];
        for (int c = 0; c < t->ncols; c++) { MIX(t->cols[c].name); MIX(t->cols[c].type); MIX(t->cols[c].pk ? "p" : "-"); MIX(t->cols[c].notnull ? "n" : "-"); }
        for (int k = 0; k < t->nfks; k++) { MIX(t->fks[k].cols); MIX(t->fks[k].ref_table); }
    }
#undef MIX
    snprintf(out, n, "%016llx", h);
}
int dgm_compare(Dgm *d) {
    int diff = 0;
    for (int i = 0; i < d->nn; i++) {
        DgNode *n = &d->n[i];
        if (n->kind != DN_TABLE) { n->status = DS_NONE; continue; }
        if (n->ti < 0) { n->status = d->x ? DS_MISSING : DS_NONE; diff += n->status != DS_NONE; continue; }
        char s[20]; dgm_table_sig(d->x, n->ti, s, sizeof s);
        n->status = n->sig[0] && strcmp(n->sig, s) ? DS_CHANGED : n->sig[0] ? DS_NONE : DS_ADDED;
        if (n->status == DS_ADDED) n->status = DS_NONE;   /* no reference yet: nothing to compare */
        diff += n->status != DS_NONE;
    }
    return diff;
}
void dgm_resign(Dgm *d) {
    for (int i = 0; i < d->nn; i++) { DgNode *n = &d->n[i]; if (n->kind == DN_TABLE && n->ti >= 0) { dgm_table_sig(d->x, n->ti, n->sig, sizeof n->sig); n->status = DS_NONE; } }
}
