/* SPDX-License-Identifier: AGPL-3.0-only */
/* ER diagram model: routing, the diagram file and the text exports (dgm.h). */
#include "dgm.h"
#include "json.h"
#include "jw.h"
#include <ctype.h>
#include <fcntl.h>
#include <math.h>
#include <stdarg.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <unistd.h>

/* ---------------------------------------------------------------- string builder */
typedef struct { char *s; size_t n, cap; } Sb;
static void sb_put(Sb *b, const char *s, size_t l) {
    if (b->n + l + 1 > b->cap) { size_t c = b->cap ? b->cap : 4096; while (c < b->n + l + 1) c *= 2; char *q = realloc(b->s, c); if (!q) return; b->s = q; b->cap = c; }
    memcpy(b->s + b->n, s, l); b->n += l; b->s[b->n] = 0;
}
static void sb_cat(Sb *b, const char *s) { sb_put(b, s, strlen(s)); }
static void sb_fmt(Sb *b, const char *fmt, ...) {
    char tmp[2048]; va_list ap; va_start(ap, fmt); int l = vsnprintf(tmp, sizeof tmp, fmt, ap); va_end(ap);
    if (l < 0) return;
    if ((size_t)l < sizeof tmp) { sb_put(b, tmp, (size_t)l); return; }
    char *big = malloc((size_t)l + 1); if (!big) return;
    va_start(ap, fmt); vsnprintf(big, (size_t)l + 1, fmt, ap); va_end(ap);
    sb_put(b, big, (size_t)l); free(big);
}
/* XML text / attribute: & < > " ' escaped, control characters dropped */
static void sb_xml(Sb *b, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        unsigned char c = (unsigned char)*p;
        if (c == '&') sb_cat(b, "&amp;");
        else if (c == '<') sb_cat(b, "&lt;");
        else if (c == '>') sb_cat(b, "&gt;");
        else if (c == '"') sb_cat(b, "&quot;");
        else if (c == '\'') sb_cat(b, "&#39;");
        else if (c < 0x20 && c != '\n' && c != '\t') continue;
        else sb_put(b, p, 1);
    }
}
static char *sb_take(Sb *b) { if (!b->s) { char *e = malloc(1); if (e) e[0] = 0; return e; } return b->s; }

/* ---------------------------------------------------------------- geometry shared with the view */
float dgm_row_y(const Dgm *d, int node, const char *col) {
    if (node < 0 || node >= d->nn || !col || !*col) return -1;
    const DgNode *n = &d->n[node];
    if (n->kind != DN_TABLE || !d->x || n->ti < 0) return -1;
    int rows[512]; int nr = dgm_card_rows(d, node, rows, 512);
    const SxTable *t = &d->x->t[n->ti];
    char first[200]; const char *c = strchr(col, ','); snprintf(first, sizeof first, "%.*s", c ? (int)(c - col) : (int)strlen(col), col);
    for (int i = n->scroll; i < nr && i - n->scroll < DG_MAX_ROWS; i++)
        if (!strcasecmp(t->cols[rows[i]].name, first)) return n->y + DG_HEAD_H + 4 + (float)(i - n->scroll) * DG_ROW_H + DG_ROW_H / 2;
    return -1;
}
int dgm_route(const Dgm *d, int e, float *p) {
    const DgEdge *g = &d->e[e];
    const DgNode *a = &d->n[g->from], *b = &d->n[g->to];
    float off = (float)g->parallel * 10.0f - (float)(g->nparallel - 1) * 5.0f;   /* parallel edges side by side */
    float ay = dgm_row_y(d, g->from, g->cols), by = dgm_row_y(d, g->to, g->ref_cols);
    if (ay < 0) ay = a->y + fminf(a->h, 60) / 2 + off;
    if (by < 0) by = b->y + fminf(b->h, 60) / 2 + off;
    if (g->from == g->to) {   /* self reference: a loop on the right side */
        float x = a->x + a->w, y0 = ay, y1 = by > ay + 8 ? by : ay + 16;
        p[0] = x; p[1] = y0; p[2] = x + 28 + off; p[3] = y0; p[4] = x + 28 + off; p[5] = y1; p[6] = x; p[7] = y1;
        return 4;
    }
    float acx = a->x + a->w / 2, bcx = b->x + b->w / 2;
    if (d->routing == DRT_STRAIGHT) {
        bool right = bcx >= acx;
        p[0] = right ? a->x + a->w : a->x; p[1] = ay; p[2] = right ? b->x : b->x + b->w; p[3] = by;
        return 2;
    }
    /* orthogonal: leave on the facing sides; overlapping columns go around the right side */
    if (b->x > a->x + a->w + 24) {
        float mx = roundf((a->x + a->w + b->x) / 2) + off;
        p[0] = a->x + a->w; p[1] = ay; p[2] = mx; p[3] = ay; p[4] = mx; p[5] = by; p[6] = b->x; p[7] = by;
    } else if (a->x > b->x + b->w + 24) {
        float mx = roundf((b->x + b->w + a->x) / 2) + off;
        p[0] = a->x; p[1] = ay; p[2] = mx; p[3] = ay; p[4] = mx; p[5] = by; p[6] = b->x + b->w; p[7] = by;
    } else {
        float mx = fmaxf(a->x + a->w, b->x + b->w) + 30 + off;
        p[0] = a->x + a->w; p[1] = ay; p[2] = mx; p[3] = ay; p[4] = mx; p[5] = by; p[6] = b->x + b->w; p[7] = by;
    }
    return 4;
}

/* ---------------------------------------------------------------- the diagram file */
static const char *mode_name(int m) { return m == DM_NAME ? "name" : m == DM_ALL ? "all" : m == DM_KEYS ? "keys" : "global"; }
static int mode_of(const char *s) { return !strcmp(s, "name") ? DM_NAME : !strcmp(s, "all") ? DM_ALL : !strcmp(s, "keys") ? DM_KEYS : DM_GLOBAL; }
static const char *rel_name(int k) { return k == DE_SUGGESTED ? "suggested" : k == DE_LOGICAL ? "logical" : "virtual"; }
static int rel_of(const char *s) { return !strcmp(s, "suggested") ? DE_SUGGESTED : !strcmp(s, "logical") ? DE_LOGICAL : !strcmp(s, "virtual") ? DE_VIRTUAL : -1; }
static const char *card_name(int c) { return c == DC_ONE ? "one" : c == DC_MANY ? "many" : "unknown"; }
static int card_of(const char *s) { return !strcmp(s, "one") ? DC_ONE : !strcmp(s, "many") ? DC_MANY : DC_UNKNOWN; }
static double r1(double v) { return round(v * 10) / 10; }

static const Dgm *g_sd;
static int node_order(const void *a, const void *b) { return strcmp(g_sd->n[*(const int *)a].key, g_sd->n[*(const int *)b].key); }
static int rel_order(const void *a, const void *b) {
    const DgRel *x = &g_sd->rels[*(const int *)a], *y = &g_sd->rels[*(const int *)b];
    int c = strcmp(x->from, y->from); if (c) return c;
    c = strcmp(x->to, y->to); if (c) return c;
    return strcmp(x->label, y->label);
}
static int str_order(const void *a, const void *b) { return strcmp((const char *)a, (const char *)b); }

char *dgm_to_json(const Dgm *d) {
    Jw j; jw_init(&j);
    jw_obj(&j);
    jw_kint(&j, "formatVersion", 1);
    jw_kstr(&j, "name", d->name);
    jw_key(&j, "connection"); jw_obj(&j); jw_kstr(&j, "id", d->conn_id); jw_kstr(&j, "name", d->conn_name); jw_end_obj(&j);
    jw_kstr(&j, "schemas", d->schemas);
    jw_kstr(&j, "capturedAt", d->captured);
    jw_kstr(&j, "focus", d->focus);
    jw_kstr(&j, "mode", mode_name(d->mode));
    jw_kstr(&j, "notation", d->notation == DNOT_SIMPLE ? "simple" : "crow");
    jw_kstr(&j, "routing", d->routing == DRT_STRAIGHT ? "straight" : "orthogonal");
    jw_key(&j, "options"); jw_obj(&j);
    jw_kbool(&j, "types", d->show_types); jw_kbool(&j, "comments", d->show_comments); jw_kbool(&j, "nullability", d->show_nullability);
    jw_kbool(&j, "defaults", d->show_defaults); jw_kbool(&j, "conceptual", d->conceptual); jw_kbool(&j, "virtual", d->show_virtual); jw_kbool(&j, "snap", d->snap);
    jw_end_obj(&j);
    jw_key(&j, "viewport"); jw_obj(&j); jw_knum(&j, "zoom", round(d->zoom * 1000) / 1000); jw_knum(&j, "x", r1(d->panx)); jw_knum(&j, "y", r1(d->pany)); jw_end_obj(&j);
    int *ord = malloc(sizeof(int) * (size_t)(d->nn + 1)), no = 0;
    for (int i = 0; i < d->nn; i++) if (d->n[i].kind != DN_STUB) ord[no++] = i;
    g_sd = d; qsort(ord, (size_t)no, sizeof(int), node_order);
    jw_key(&j, "nodes"); jw_arr(&j);
    for (int k = 0; k < no; k++) {
        const DgNode *n = &d->n[ord[k]];
        jw_obj(&j);
        jw_kstr(&j, "key", n->kind == DN_ENTITY ? n->key + 7 : n->key);
        jw_kstr(&j, "kind", n->kind == DN_ENTITY ? "entity" : "table");
        jw_knum(&j, "x", r1(n->x)); jw_knum(&j, "y", r1(n->y));
        if (n->mode != DM_GLOBAL) jw_kstr(&j, "mode", mode_name(n->mode));
        if (n->pinned) jw_kbool(&j, "pinned", true);
        if (n->color) jw_kint(&j, "color", n->color);
        if (n->alias[0]) jw_kstr(&j, "alias", n->alias);
        if (n->kind == DN_ENTITY && n->attrs[0]) jw_kstr(&j, "attributes", n->attrs);
        if (n->sig[0]) jw_kstr(&j, "sig", n->sig);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    free(ord);
    ord = malloc(sizeof(int) * (size_t)(d->nrels + 1));
    for (int i = 0; i < d->nrels; i++) ord[i] = i;
    qsort(ord, (size_t)d->nrels, sizeof(int), rel_order);
    jw_key(&j, "relations"); jw_arr(&j);
    for (int k = 0; k < d->nrels; k++) {
        const DgRel *r = &d->rels[ord[k]];
        jw_obj(&j);
        jw_kstr(&j, "kind", rel_name(r->kind)); jw_kstr(&j, "from", r->from); jw_kstr(&j, "to", r->to);
        if (r->cols[0]) jw_kstr(&j, "cols", r->cols);
        if (r->ref_cols[0]) jw_kstr(&j, "refCols", r->ref_cols);
        jw_kstr(&j, "label", r->label);
        if (r->evidence[0]) jw_kstr(&j, "evidence", r->evidence);
        if (r->author[0]) jw_kstr(&j, "author", r->author);
        if (r->verified) jw_kbool(&j, "verified", true);
        jw_kstr(&j, "cardFrom", card_name(r->card_from)); jw_kstr(&j, "cardTo", card_name(r->card_to));
        if (r->optional) jw_kbool(&j, "optional", true);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    free(ord);
    jw_key(&j, "notes"); jw_arr(&j);
    for (int i = 0; i < d->nnotes; i++) {
        const DgNote *n = &d->notes[i];
        jw_obj(&j);
        if (n->area) jw_kbool(&j, "area", true);
        jw_knum(&j, "x", r1(n->x)); jw_knum(&j, "y", r1(n->y)); jw_knum(&j, "w", r1(n->w)); jw_knum(&j, "h", r1(n->h));
        if (n->color) jw_kint(&j, "color", n->color);
        if (n->title[0]) jw_kstr(&j, "title", n->title);
        if (n->text[0]) jw_kstr(&j, "text", n->text);
        jw_end_obj(&j);
    }
    jw_end_arr(&j);
    char (*h)[160] = malloc(sizeof *h * (size_t)(d->nhidden + 1));
    for (int i = 0; i < d->nhidden; i++) memcpy(h[i], d->hidden[i], 160);
    qsort(h, (size_t)d->nhidden, 160, str_order);
    jw_key(&j, "hidden"); jw_arr(&j); for (int i = 0; i < d->nhidden; i++) jw_str(&j, h[i]); jw_end_arr(&j);
    free(h);
    jw_end_obj(&j);
    char *s = jw_take(&j);
    /* one member per line: readable diffs (DGM-058) */
    Sb b = {0}; int depth = 0; bool in_str = false;
    for (const char *p = s; p && *p; p++) {
        char c = *p;
        if (in_str) { sb_put(&b, p, 1); if (c == '\\' && p[1]) { sb_put(&b, p + 1, 1); p++; } else if (c == '"') in_str = false; continue; }
        if (c == '"') { in_str = true; sb_put(&b, p, 1); continue; }
        if (c == '{' || c == '[') {
            sb_put(&b, p, 1);
            if ((c == '[' && p[1] == ']') || (c == '{' && p[1] == '}')) { sb_put(&b, p + 1, 1); p++; continue; }
            depth++;
            /* nodes / relations / notes items stay on one line each */
            if (depth <= 2 || c == '[') { sb_cat(&b, "\n"); for (int i = 0; i < depth; i++) sb_cat(&b, "  "); }
            continue;
        }
        if (c == '}' || c == ']') {
            depth--;
            if (depth <= 1 || c == ']') { sb_cat(&b, "\n"); for (int i = 0; i < depth; i++) sb_cat(&b, "  "); }
            sb_put(&b, p, 1);
            continue;
        }
        if (c == ',') { sb_put(&b, p, 1); if (depth <= 2) { sb_cat(&b, "\n"); for (int i = 0; i < depth; i++) sb_cat(&b, "  "); } else sb_cat(&b, " "); continue; }
        if (c == ':') { sb_cat(&b, ": "); continue; }
        sb_put(&b, p, 1);
    }
    sb_cat(&b, "\n");
    free(s);
    return sb_take(&b);
}

static bool num_ok(double v) { return isfinite(v) && fabs(v) <= 1e6; }
bool dgm_from_json(Dgm *d, const char *text, size_t len, char *why, size_t wn) {
    Json j = {0};
    if (!json_parse(&j, text, len)) { snprintf(why, wn, "not a diagram file: %s at byte %zu", j.err, j.err_off); json_free(&j); return false; }
    static const char *const top[] = { "formatVersion", "name", "connection", "schemas", "capturedAt", "focus", "mode", "notation", "routing", "options", "viewport", "nodes", "relations", "notes", "hidden", NULL };
    char bad[80];
    if (!j.n || j.nodes[0].type != JN_OBJECT) { snprintf(why, wn, "not a diagram file: the top level is not an object"); json_free(&j); return false; }
    long long fv = 0;
    if (!jr_int(&j, 0, "formatVersion", &fv)) { snprintf(why, wn, "not a diagram file: formatVersion is missing"); json_free(&j); return false; }
    if (fv > 1) { snprintf(why, wn, "this diagram was saved by a newer Lume (format %lld): update Lume to open it", fv); json_free(&j); return false; }
    if (fv < 1) { snprintf(why, wn, "unknown diagram format %lld", fv); json_free(&j); return false; }
    if (!jr_keys_allowed(&j, 0, top, bad, sizeof bad)) { snprintf(why, wn, "unknown field \"%s\" in the diagram file", bad); json_free(&j); return false; }
    Dgm t; dgm_init(&t);
    char s[1300];
    jr_str(&j, 0, "name", t.name, sizeof t.name);
    int c = jr_get(&j, 0, "connection");
    if (c >= 0) { jr_str(&j, c, "id", t.conn_id, sizeof t.conn_id); jr_str(&j, c, "name", t.conn_name, sizeof t.conn_name); }
    jr_str(&j, 0, "schemas", t.schemas, sizeof t.schemas);
    jr_str(&j, 0, "capturedAt", t.captured, sizeof t.captured);
    jr_str(&j, 0, "focus", t.focus, sizeof t.focus);
    if (jr_str(&j, 0, "mode", s, sizeof s)) { int m = mode_of(s); t.mode = m == DM_GLOBAL ? DM_KEYS : m; }
    if (jr_str(&j, 0, "notation", s, sizeof s)) t.notation = !strcmp(s, "simple") ? DNOT_SIMPLE : DNOT_CROW;
    if (jr_str(&j, 0, "routing", s, sizeof s)) t.routing = !strcmp(s, "straight") ? DRT_STRAIGHT : DRT_ORTHO;
    int o = jr_get(&j, 0, "options");
    if (o >= 0) {
        jr_bool(&j, o, "types", &t.show_types); jr_bool(&j, o, "comments", &t.show_comments); jr_bool(&j, o, "nullability", &t.show_nullability);
        jr_bool(&j, o, "defaults", &t.show_defaults); jr_bool(&j, o, "conceptual", &t.conceptual); jr_bool(&j, o, "virtual", &t.show_virtual); jr_bool(&j, o, "snap", &t.snap);
    }
    int v = jr_get(&j, 0, "viewport");
    if (v >= 0) { double z = 1, x = 0, y = 0; jr_num(&j, v, "zoom", &z); jr_num(&j, v, "x", &x); jr_num(&j, v, "y", &y); if (num_ok(z) && z >= 0.05 && z <= 8) t.zoom = (float)z; if (num_ok(x)) t.panx = (float)x; if (num_ok(y)) t.pany = (float)y; }
    int arr = jr_get(&j, 0, "nodes");
    bool ok = true;
    if (arr >= 0 && j.nodes[arr].type == JN_ARRAY) {
        if (j.nodes[arr].nchildren > 20000) { snprintf(why, wn, "the diagram has more than 20,000 cards"); ok = false; }
        static const char *const nk[] = { "key", "kind", "x", "y", "mode", "pinned", "color", "alias", "attributes", "sig", NULL };
        for (int i = 0; ok && i < j.nodes[arr].nchildren; i++) {
            int e = jr_child(&j, arr, i);
            if (!jr_keys_allowed(&j, e, nk, bad, sizeof bad)) { snprintf(why, wn, "card %d: unknown field \"%s\"", i + 1, bad); ok = false; break; }
            char key[DG_KEY], kind[16] = "table"; double x = 0, y = 0;
            if (!jr_str(&j, e, "key", key, sizeof key) || !key[0]) { snprintf(why, wn, "card %d has no key", i + 1); ok = false; break; }
            jr_str(&j, e, "kind", kind, sizeof kind);
            if (strcmp(kind, "table") && strcmp(kind, "entity")) { snprintf(why, wn, "card %d: unknown kind \"%s\"", i + 1, kind); ok = false; break; }
            jr_num(&j, e, "x", &x); jr_num(&j, e, "y", &y);
            if (!num_ok(x) || !num_ok(y)) { snprintf(why, wn, "card %d: position out of range", i + 1); ok = false; break; }
            int ni;
            if (!strcmp(kind, "entity")) ni = dgm_add_entity(&t, key, (float)x, (float)y);
            else {
                t.n = realloc(t.n, sizeof *t.n * (size_t)(t.nn + 1)); t.capn = t.nn + 1;
                DgNode *n = &t.n[t.nn]; memset(n, 0, sizeof *n);
                snprintf(n->key, sizeof n->key, "%s", key); n->kind = DN_TABLE; n->x = (float)x; n->y = (float)y; n->w = 180; n->h = DG_HEAD_H; n->mode = DM_GLOBAL; n->ti = -1;
                ni = t.nn++;
            }
            if (ni < 0) continue;
            DgNode *n = &t.n[ni];
            if (jr_str(&j, e, "mode", s, sizeof s)) n->mode = mode_of(s);
            jr_bool(&j, e, "pinned", &n->pinned);
            long long col = 0; if (jr_int(&j, e, "color", &col) && col >= 0 && col <= 8) n->color = (int)col;
            jr_str(&j, e, "alias", n->alias, sizeof n->alias);
            jr_str(&j, e, "attributes", n->attrs, sizeof n->attrs);
            jr_str(&j, e, "sig", n->sig, sizeof n->sig);
        }
    }
    arr = jr_get(&j, 0, "relations");
    if (ok && arr >= 0 && j.nodes[arr].type == JN_ARRAY) {
        static const char *const rk[] = { "kind", "from", "to", "cols", "refCols", "label", "evidence", "author", "verified", "cardFrom", "cardTo", "optional", NULL };
        for (int i = 0; ok && i < j.nodes[arr].nchildren && i < 20000; i++) {
            int e = jr_child(&j, arr, i);
            if (!jr_keys_allowed(&j, e, rk, bad, sizeof bad)) { snprintf(why, wn, "relation %d: unknown field \"%s\"", i + 1, bad); ok = false; break; }
            char kind[16] = "", from[DG_KEY] = "", to[DG_KEY] = "";
            jr_str(&j, e, "kind", kind, sizeof kind); jr_str(&j, e, "from", from, sizeof from); jr_str(&j, e, "to", to, sizeof to);
            int k = rel_of(kind);
            if (k < 0 || !from[0] || !to[0]) { snprintf(why, wn, "relation %d: kind, from and to are required", i + 1); ok = false; break; }
            int r = dgm_add_rel(&t, k, from, to, NULL, NULL, NULL); if (r < 0) continue;
            DgRel *rl = &t.rels[r];
            jr_str(&j, e, "cols", rl->cols, sizeof rl->cols); jr_str(&j, e, "refCols", rl->ref_cols, sizeof rl->ref_cols);
            jr_str(&j, e, "label", rl->label, sizeof rl->label); jr_str(&j, e, "evidence", rl->evidence, sizeof rl->evidence);
            jr_str(&j, e, "author", rl->author, sizeof rl->author); jr_bool(&j, e, "verified", &rl->verified); jr_bool(&j, e, "optional", &rl->optional);
            if (jr_str(&j, e, "cardFrom", s, sizeof s)) rl->card_from = card_of(s);
            if (jr_str(&j, e, "cardTo", s, sizeof s)) rl->card_to = card_of(s);
        }
    }
    arr = jr_get(&j, 0, "notes");
    if (ok && arr >= 0 && j.nodes[arr].type == JN_ARRAY) {
        static const char *const ek[] = { "area", "x", "y", "w", "h", "color", "title", "text", NULL };
        for (int i = 0; ok && i < j.nodes[arr].nchildren && i < 5000; i++) {
            int e = jr_child(&j, arr, i);
            if (!jr_keys_allowed(&j, e, ek, bad, sizeof bad)) { snprintf(why, wn, "note %d: unknown field \"%s\"", i + 1, bad); ok = false; break; }
            double x = 0, y = 0, w = 200, h = 80; bool area = false; char title[80] = "", tx[600] = "";
            jr_num(&j, e, "x", &x); jr_num(&j, e, "y", &y); jr_num(&j, e, "w", &w); jr_num(&j, e, "h", &h); jr_bool(&j, e, "area", &area);
            if (!num_ok(x) || !num_ok(y) || !num_ok(w) || !num_ok(h) || w < 20 || h < 20) { snprintf(why, wn, "note %d: size or position out of range", i + 1); ok = false; break; }
            jr_str(&j, e, "title", title, sizeof title); jr_str(&j, e, "text", tx, sizeof tx);
            int ni = dgm_add_note(&t, area, (float)x, (float)y, (float)w, (float)h, title, tx);
            long long col = 0; if (ni >= 0 && jr_int(&j, e, "color", &col) && col >= 0 && col <= 8) t.notes[ni].color = (int)col;
        }
    }
    arr = jr_get(&j, 0, "hidden");
    if (ok && arr >= 0 && j.nodes[arr].type == JN_ARRAY)
        for (int i = 0; i < j.nodes[arr].nchildren && i < 20000; i++) { char h[160]; jr_node_str(&j, jr_child(&j, arr, i), h, sizeof h); if (h[0]) { char (*nh)[160] = realloc(t.hidden, sizeof *nh * (size_t)(t.nhidden + 1)); if (nh) { t.hidden = nh; memcpy(t.hidden[t.nhidden++], h, 160); } } }
    json_free(&j);
    if (!ok) { dgm_free(&t); return false; }
    t.dirty = false;
    dgm_free(d);
    *d = t;
    return true;
}
bool dgm_save(const Dgm *d, const char *path, char *why, size_t wn) {
    char *s = dgm_to_json(d);
    if (!s) { snprintf(why, wn, "out of memory"); return false; }
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { snprintf(why, wn, "could not write %s", tmp); free(s); return false; }
    size_t l = strlen(s);
    bool ok = fwrite(s, 1, l, f) == l;
    ok = fclose(f) == 0 && ok;
    free(s);
    if (!ok || rename(tmp, path) != 0) { remove(tmp); snprintf(why, wn, "could not save the diagram (disk full or no permission)"); return false; }
    return true;
}
bool dgm_load(Dgm *d, const char *path, char *why, size_t wn) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, wn, "could not open the diagram file"); return false; }
    fseek(f, 0, SEEK_END); long l = ftell(f); fseek(f, 0, SEEK_SET);
    if (l < 0 || l > 64L * 1024 * 1024) { fclose(f); snprintf(why, wn, "the diagram file is too large"); return false; }
    char *b = malloc((size_t)l + 1);
    size_t got = b ? fread(b, 1, (size_t)l, f) : 0; fclose(f);
    if (!b || got != (size_t)l) { free(b); snprintf(why, wn, "could not read the diagram file"); return false; }
    b[l] = 0;
    bool ok = dgm_from_json(d, b, (size_t)l, why, wn);
    free(b);
    return ok;
}

/* ---------------------------------------------------------------- Mermaid / GraphML / draw.io */
static void ident(const char *s, char *o, size_t n) {   /* Mermaid / XML-safe identifier */
    size_t k = 0;
    for (const char *p = s; *p && k + 1 < n; p++) o[k++] = isalnum((unsigned char)*p) || *p == '_' || *p == '-' ? *p : '_';
    o[k] = 0;
    if (!k) snprintf(o, n, "x");
}
char *dgm_mermaid(const Dgm *d, int *lost) {
    Sb b = {0}; int ls = 0;
    sb_cat(&b, "erDiagram\n");
    for (int i = 0; i < d->nn; i++) {
        const DgNode *n = &d->n[i]; char id[DG_KEY]; ident(n->kind == DN_ENTITY ? n->key + 7 : n->key, id, sizeof id);
        if (n->alias[0]) ls++;
        sb_fmt(&b, "    %s {\n", id);
        if (n->kind == DN_TABLE && d->x && n->ti >= 0) {
            const SxTable *t = &d->x->t[n->ti];
            for (int c = 0; c < t->ncols; c++) {
                char ty[120], nm[200]; ident(t->cols[c].type ? t->cols[c].type : "unknown", ty, sizeof ty); ident(t->cols[c].name, nm, sizeof nm);
                bool fk = false; for (int k = 0; k < t->nfks; k++) if (dgm_col_in(t->fks[k].cols, t->cols[c].name)) fk = true;
                sb_fmt(&b, "        %s %s%s%s%s\n", ty, nm, t->cols[c].pk || fk ? " " : "", t->cols[c].pk ? "PK" : "", fk ? (t->cols[c].pk ? ",FK" : "FK") : "");
                if (t->cols[c].comment && t->cols[c].comment[0]) ls++;
            }
        } else if (n->kind == DN_ENTITY) {
            for (const char *p = n->attrs; *p; ) {
                const char *e = strchr(p, '\n'); size_t l = e ? (size_t)(e - p) : strlen(p);
                char line[300]; snprintf(line, sizeof line, "%.*s", (int)(l < 299 ? l : 299), p);
                bool pk = !strncasecmp(line, "PK ", 3); char *nm = pk ? line + 3 : line; char *sp = strchr(nm, ' ');
                char a[200], ty[120]; if (sp) { *sp = 0; ident(nm, a, sizeof a); ident(sp + 1, ty, sizeof ty); } else { ident(nm, a, sizeof a); snprintf(ty, sizeof ty, "string"); }
                if (a[0]) sb_fmt(&b, "        %s %s%s\n", ty, a, pk ? " PK" : "");
                if (!e) break;
                p = e + 1;
            }
        }
        sb_cat(&b, "    }\n");
    }
    for (int e = 0; e < d->ne; e++) {
        const DgEdge *g = &d->e[e]; if (g->hidden) continue;
        char a[DG_KEY], c[DG_KEY];
        ident(d->n[g->to].kind == DN_ENTITY ? d->n[g->to].key + 7 : d->n[g->to].key, a, sizeof a);
        ident(d->n[g->from].kind == DN_ENTITY ? d->n[g->from].key + 7 : d->n[g->from].key, c, sizeof c);
        const char *left = g->optional ? "|o" : "||";
        const char *right = g->card_from == DC_ONE ? "o|" : "o{";
        const char *line = g->origin == DE_FK ? "--" : "..";
        if (g->origin != DE_FK) ls += g->origin == DE_SUGGESTED;   /* "suggested" has no Mermaid form */
        char lab[200]; snprintf(lab, sizeof lab, "%s", g->name); for (char *q = lab; *q; q++) if (*q == '"') *q = '\'';
        sb_fmt(&b, "    %s %s%s%s %s : \"%s\"\n", a, left, line, right, c, lab);
    }
    ls += d->nnotes;
    if (lost) *lost = ls;
    return sb_take(&b);
}
char *dgm_graphml(const Dgm *d) {
    Sb b = {0};
    sb_cat(&b, "<?xml version=\"1.0\" encoding=\"UTF-8\"?>\n<graphml xmlns=\"http://graphml.graphdrawing.org/xmlns\">\n"
               "  <key id=\"label\" for=\"all\" attr.name=\"label\" attr.type=\"string\"/>\n  <key id=\"kind\" for=\"all\" attr.name=\"kind\" attr.type=\"string\"/>\n"
               "  <key id=\"x\" for=\"node\" attr.name=\"x\" attr.type=\"double\"/>\n  <key id=\"y\" for=\"node\" attr.name=\"y\" attr.type=\"double\"/>\n"
               "  <key id=\"columns\" for=\"edge\" attr.name=\"columns\" attr.type=\"string\"/>\n  <graph id=\"G\" edgedefault=\"directed\">\n");
    for (int i = 0; i < d->nn; i++) {
        sb_fmt(&b, "    <node id=\"n%d\"><data key=\"label\">", i); sb_xml(&b, dgm_card_label(d, i));
        sb_fmt(&b, "</data><data key=\"kind\">%s</data><data key=\"x\">%.1f</data><data key=\"y\">%.1f</data></node>\n", d->n[i].kind == DN_ENTITY ? "entity" : d->n[i].kind == DN_STUB ? "external" : "table", d->n[i].x, d->n[i].y);
    }
    for (int e = 0; e < d->ne; e++) {
        const DgEdge *g = &d->e[e]; if (g->hidden) continue;
        sb_fmt(&b, "    <edge id=\"e%d\" source=\"n%d\" target=\"n%d\"><data key=\"label\">", e, g->from, g->to); sb_xml(&b, g->name);
        sb_fmt(&b, "</data><data key=\"kind\">%s</data><data key=\"columns\">", g->origin == DE_FK ? "foreign key" : rel_name(g->origin));
        sb_xml(&b, g->cols); sb_cat(&b, " -&gt; "); sb_xml(&b, g->ref_cols); sb_cat(&b, "</data></edge>\n");
    }
    sb_cat(&b, "  </graph>\n</graphml>\n");
    return sb_take(&b);
}
char *dgm_drawio(const Dgm *d) {
    Sb b = {0};
    sb_cat(&b, "<mxfile><diagram name=\""); sb_xml(&b, d->name[0] ? d->name : "ER diagram"); sb_cat(&b, "\"><mxGraphModel><root><mxCell id=\"0\"/><mxCell id=\"1\" parent=\"0\"/>\n");
    for (int i = 0; i < d->nn; i++) {
        const DgNode *n = &d->n[i];
        Sb v = {0}; sb_cat(&v, dgm_card_label(d, i));
        int rows[512]; int nr = dgm_card_rows(d, i, rows, 512);
        if (n->kind == DN_TABLE && d->x && n->ti >= 0) for (int r = 0; r < nr; r++) { const SxCol *c = &d->x->t[n->ti].cols[rows[r]]; sb_cat(&v, "\n"); sb_cat(&v, c->pk ? "PK " : ""); sb_cat(&v, c->name); }
        sb_fmt(&b, "<mxCell id=\"n%d\" value=\"", i);
        for (const char *p = v.s ? v.s : ""; *p; p++) { if (*p == '\n') sb_cat(&b, "&#xa;"); else { char one[2] = { *p, 0 }; sb_xml(&b, one); } }
        free(v.s);
        sb_fmt(&b, "\" style=\"rounded=1;whiteSpace=wrap;align=left;verticalAlign=top;spacingLeft=6;%s\" vertex=\"1\" parent=\"1\"><mxGeometry x=\"%.0f\" y=\"%.0f\" width=\"%.0f\" height=\"%.0f\" as=\"geometry\"/></mxCell>\n",
               n->kind == DN_STUB ? "dashed=1;" : "", n->x, n->y, n->w, n->h);
    }
    for (int e = 0; e < d->ne; e++) {
        const DgEdge *g = &d->e[e]; if (g->hidden) continue;
        sb_fmt(&b, "<mxCell id=\"e%d\" value=\"", e); sb_xml(&b, g->name);
        sb_fmt(&b, "\" style=\"edgeStyle=orthogonalEdgeStyle;endArrow=ERmandOne;startArrow=%s;%s\" edge=\"1\" parent=\"1\" source=\"n%d\" target=\"n%d\"><mxGeometry relative=\"1\" as=\"geometry\"/></mxCell>\n",
               g->card_from == DC_ONE ? "ERmandOne" : "ERmany", g->origin == DE_FK ? "" : "dashed=1;", g->from, g->to);
    }
    sb_cat(&b, "</root></mxGraphModel></diagram></mxfile>\n");
    return sb_take(&b);
}

/* ---------------------------------------------------------------- SVG / HTML */
typedef struct { const char *bg, *card, *head, *text, *muted, *line, *virt, *stub, *accent; } Pal;
static const Pal LIGHT = { "#ffffff", "#ffffff", "#eef1f6", "#1d2330", "#5d6676", "#6b7486", "#b46a00", "#8a8f99", "#3b6fd8" };
static const Pal DARK = { "#16181d", "#1f2229", "#2a2e37", "#e6e8ec", "#9aa2b1", "#8b93a3", "#e0a040", "#6d7380", "#6d9bff" };
static const char *const TAGS[9] = { "", "#3b6fd8", "#2f9e6a", "#c2410c", "#9333ea", "#0e7490", "#be185d", "#a16207", "#475569" };

static void svg_marks(Sb *b, float x, float y, float dx, int card, bool optional, const char *col, int notation) {
    /* dx: +1 the line leaves to the right of the card, -1 to the left */
    if (notation == DNOT_SIMPLE) { if (card == DC_ONE) sb_fmt(b, "<path d=\"M%.1f %.1f l%.1f -5 l0 10 z\" fill=\"%s\"/>", x, y, 9 * dx, col); return; }
    if (card == DC_MANY) sb_fmt(b, "<path d=\"M%.1f %.1f L%.1f %.1f M%.1f %.1f L%.1f %.1f M%.1f %.1f L%.1f %.1f\" stroke=\"%s\" fill=\"none\"/>", x, y - 6, x + 10 * dx, y, x, y + 6, x + 10 * dx, y, x, y, x + 10 * dx, y, col);
    else if (card == DC_ONE) sb_fmt(b, "<path d=\"M%.1f %.1f L%.1f %.1f\" stroke=\"%s\"/>", x + 8 * dx, y - 6, x + 8 * dx, y + 6, col);
    else sb_fmt(b, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"2.5\" fill=\"%s\"/>", x + 6 * dx, y, col);
    if (optional) sb_fmt(b, "<circle cx=\"%.1f\" cy=\"%.1f\" r=\"3.5\" fill=\"none\" stroke=\"%s\"/>", x + 16 * dx, y, col);
    else if (card != DC_UNKNOWN) sb_fmt(b, "<path d=\"M%.1f %.1f L%.1f %.1f\" stroke=\"%s\"/>", x + 15 * dx, y - 6, x + 15 * dx, y + 6, col);
}
static void svg_body(Sb *b, const Dgm *d, bool only_sel, const DgSvgOpt *o, float x0, float y0) {
    const Pal *P = o && o->dark ? &DARK : &LIGHT;
    (void)x0; (void)y0;
    for (int i = 0; i < d->nnotes; i++) if (d->notes[i].area) {
        const DgNote *n = &d->notes[i];
        sb_fmt(b, "<g class=\"area\"><rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"10\" fill=\"%s\" fill-opacity=\"0.08\" stroke=\"%s\" stroke-opacity=\"0.5\"/>", n->x, n->y, n->w, n->h, TAGS[n->color ? n->color : 1], TAGS[n->color ? n->color : 1]);
        sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\" font-weight=\"600\">", n->x + 12, n->y + 20, P->muted); sb_xml(b, n->title); sb_cat(b, "</text></g>\n");
    }
    for (int e = 0; e < d->ne; e++) {
        const DgEdge *g = &d->e[e]; if (g->hidden) continue;
        if (only_sel && !(d->n[g->from].sel && d->n[g->to].sel)) continue;
        float p[8]; int np = dgm_route(d, e, p);
        const char *col = g->origin == DE_FK ? P->line : P->virt;
        const char *dash = g->origin == DE_VIRTUAL ? " stroke-dasharray=\"6 4\"" : g->origin == DE_SUGGESTED ? " stroke-dasharray=\"2 4\"" : "";
        sb_cat(b, "<g class=\"edge\"><title>"); sb_xml(b, g->name); sb_cat(b, ": "); sb_xml(b, g->cols); sb_cat(b, " -&gt; "); sb_xml(b, g->ref_cols); sb_cat(b, "</title>");
        sb_fmt(b, "<polyline points=\"");
        for (int k = 0; k < np; k++) sb_fmt(b, "%s%.1f,%.1f", k ? " " : "", p[2 * k], p[2 * k + 1]);
        sb_fmt(b, "\" fill=\"none\" stroke=\"%s\" stroke-width=\"1.4\"%s/>", col, dash);
        float dxa = p[2] >= p[0] ? 1 : -1, dxb = p[2 * np - 4] >= p[2 * np - 2] ? 1 : -1;
        svg_marks(b, p[0], p[1], dxa, g->card_from, false, col, d->notation);
        svg_marks(b, p[2 * np - 2], p[2 * np - 1], dxb, g->card_to, g->optional, col, d->notation);
        sb_cat(b, "</g>\n");
    }
    for (int i = 0; i < d->nn; i++) {
        const DgNode *n = &d->n[i];
        if (only_sel && !n->sel) continue;
        const char *label = dgm_card_label(d, i);
        sb_cat(b, "<g class=\"card\" data-name=\""); sb_xml(b, label); sb_fmt(b, "\" data-x=\"%.1f\" data-y=\"%.1f\">", n->x + n->w / 2, n->y + n->h / 2);
        const char *dash = n->kind == DN_STUB ? " stroke-dasharray=\"5 3\"" : "";
        sb_fmt(b, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"6\" fill=\"%s\" stroke=\"%s\"%s/>", n->x, n->y, n->w, n->h, P->card, n->kind == DN_STUB ? P->stub : P->line, dash);
        sb_fmt(b, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"6\" fill=\"%s\"/>", n->x + 0.5, n->y + 0.5, n->w - 1, DG_HEAD_H - 1, P->head);
        if (n->color) sb_fmt(b, "<rect x=\"%.1f\" y=\"%.1f\" width=\"4\" height=\"%.1f\" fill=\"%s\"/>", n->x, n->y, DG_HEAD_H, TAGS[n->color]);
        sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\" font-weight=\"600\">", n->x + 10, n->y + 18, n->kind == DN_STUB ? P->muted : P->text);
        sb_xml(b, label);
        if (n->kind == DN_STUB) sb_cat(b, " (external)");
        sb_cat(b, "</text>");
        int rows[512]; int nr = dgm_card_rows(d, i, rows, 512);
        for (int r = n->scroll; r < nr && r - n->scroll < DG_MAX_ROWS; r++) {
            float y = n->y + DG_HEAD_H + 4 + (float)(r - n->scroll) * DG_ROW_H + 13;
            if (n->kind == DN_TABLE && d->x && n->ti >= 0) {
                const SxTable *t = &d->x->t[n->ti]; const SxCol *c = &t->cols[rows[r]];
                bool fk = false; for (int k = 0; k < t->nfks; k++) if (dgm_col_in(t->fks[k].cols, c->name)) fk = true;
                const char *tag = c->pk ? "PK" : fk ? "FK" : "";
                sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\" font-size=\"10\">%s</text>", n->x + 8, y, P->accent, tag);
                sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\"%s>", n->x + 30, y, P->text, c->notnull ? " font-weight=\"600\"" : "");
                sb_xml(b, c->name); sb_cat(b, "</text>");
                if (d->show_types && c->type) { sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\" text-anchor=\"end\">", n->x + n->w - 8, y, P->muted); sb_xml(b, c->type); sb_cat(b, "</text>"); }
            } else if (n->kind == DN_ENTITY) {
                int line = 0; const char *p = n->attrs;
                for (; *p && line < rows[r]; p++) if (*p == '\n') line++;
                const char *e2 = strchr(p, '\n'); char s[300]; snprintf(s, sizeof s, "%.*s", e2 ? (int)(e2 - p) : (int)strlen(p), p);
                sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"%s\">", n->x + 10, y, P->text); sb_xml(b, s); sb_cat(b, "</text>");
            }
        }
        sb_cat(b, "</g>\n");
    }
    for (int i = 0; i < d->nnotes; i++) if (!d->notes[i].area) {
        const DgNote *n = &d->notes[i];
        sb_fmt(b, "<g class=\"note\"><rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" rx=\"4\" fill=\"#fff6c8\" stroke=\"#d8c66a\"/>", n->x, n->y, n->w, n->h);
        sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#3a3420\" font-weight=\"600\">", n->x + 8, n->y + 18); sb_xml(b, n->title); sb_cat(b, "</text>");
        float y = n->y + 36;
        for (const char *p = n->text; *p && y < n->y + n->h - 4; y += 16) {
            const char *e2 = strchr(p, '\n'); size_t l = e2 ? (size_t)(e2 - p) : strlen(p);
            char s[300]; snprintf(s, sizeof s, "%.*s", (int)(l < 299 ? l : 299), p);
            sb_fmt(b, "<text x=\"%.1f\" y=\"%.1f\" fill=\"#3a3420\">", n->x + 8, y); sb_xml(b, s); sb_cat(b, "</text>");
            if (!e2) break;
            p = e2 + 1;
        }
        sb_cat(b, "</g>\n");
    }
}
char *dgm_svg(const Dgm *d, bool only_sel, const DgSvgOpt *o) {
    DgSvgOpt def = { 0 }; def.scale = 1; def.legend = true; if (!o) o = &def;
    const Pal *P = o->dark ? &DARK : &LIGHT;
    float x0, y0, x1, y1;
    if (!dgm_bounds(d, only_sel, &x0, &y0, &x1, &y1)) { x0 = y0 = 0; x1 = y1 = 100; }
    float m = 30, top = o->title && o->title_text && *o->title_text ? 34 : 0, bottom = (o->legend ? 30 : 0) + (o->footer && *o->footer ? 20 : 0);
    float W = x1 - x0 + 2 * m, H = y1 - y0 + 2 * m + top + bottom, sc = o->scale > 0 ? o->scale : 1;
    Sb b = {0};
    sb_fmt(&b, "<svg xmlns=\"http://www.w3.org/2000/svg\" width=\"%.0f\" height=\"%.0f\" viewBox=\"%.1f %.1f %.1f %.1f\" font-family=\"Inter, Helvetica, Arial, sans-serif\" font-size=\"12\">\n", W * sc, H * sc, x0 - m, y0 - m - top, W, H);
    if (!o->transparent) sb_fmt(&b, "<rect x=\"%.1f\" y=\"%.1f\" width=\"%.1f\" height=\"%.1f\" fill=\"%s\"/>\n", x0 - m, y0 - m - top, W, H, P->bg);
    if (top > 0) { sb_fmt(&b, "<text x=\"%.1f\" y=\"%.1f\" font-size=\"16\" font-weight=\"600\" fill=\"%s\">", x0, y0 - m - top + 24, P->text); sb_xml(&b, o->title_text); sb_cat(&b, "</text>\n"); }
    svg_body(&b, d, only_sel, o, x0, y0);
    float ly = y1 + m;
    if (o->legend) {
        sb_fmt(&b, "<g class=\"legend\" font-size=\"11\" fill=\"%s\">", P->muted);
        sb_fmt(&b, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\"/><text x=\"%.1f\" y=\"%.1f\">foreign key (database)</text>", x0, ly + 8, x0 + 30, ly + 8, P->line, x0 + 36, ly + 12);
        sb_fmt(&b, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-dasharray=\"6 4\"/><text x=\"%.1f\" y=\"%.1f\">virtual (no constraint)</text>", x0 + 190, ly + 8, x0 + 220, ly + 8, P->virt, x0 + 226, ly + 12);
        sb_fmt(&b, "<line x1=\"%.1f\" y1=\"%.1f\" x2=\"%.1f\" y2=\"%.1f\" stroke=\"%s\" stroke-dasharray=\"2 4\"/><text x=\"%.1f\" y=\"%.1f\">suggested (hypothesis)</text>", x0 + 380, ly + 8, x0 + 410, ly + 8, P->virt, x0 + 416, ly + 12);
        sb_cat(&b, "</g>\n");
        ly += 26;
    }
    if (o->footer && *o->footer) { sb_fmt(&b, "<text x=\"%.1f\" y=\"%.1f\" font-size=\"10\" fill=\"%s\">", x0, ly + 12, P->muted); sb_xml(&b, o->footer); sb_cat(&b, "</text>\n"); }
    sb_cat(&b, "</svg>\n");
    return sb_take(&b);
}
char *dgm_html(const Dgm *d, const DgSvgOpt *o) {
    char *svg = dgm_svg(d, false, o);
    Sb b = {0};
    sb_cat(&b, "<!doctype html>\n<html lang=\"en\"><head><meta charset=\"utf-8\"><meta name=\"viewport\" content=\"width=device-width, initial-scale=1\">\n"
               "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src 'none'; style-src 'unsafe-inline'; script-src 'unsafe-inline'; img-src data:\">\n<title>");
    sb_xml(&b, d->name[0] ? d->name : "ER diagram");
    sb_cat(&b, "</title>\n<style>html,body{margin:0;height:100%;font:13px Inter,Helvetica,Arial,sans-serif;background:");
    sb_cat(&b, o && o->dark ? "#16181d;color:#e6e8ec" : "#f5f6f8;color:#1d2330");
    sb_cat(&b, "}#bar{position:fixed;top:0;left:0;right:0;display:flex;gap:8px;align-items:center;padding:8px 12px;background:inherit;border-bottom:1px solid #8884;z-index:1}"
               "#bar input{flex:0 1 280px;padding:5px 8px;border:1px solid #8886;border-radius:6px;background:transparent;color:inherit}#bar button{padding:4px 10px;border:1px solid #8886;border-radius:6px;background:transparent;color:inherit;cursor:pointer}"
               "#v{position:absolute;top:44px;left:0;right:0;bottom:0;overflow:hidden;cursor:grab}#v svg{width:100%;height:100%}.hit rect:first-child{stroke:#e8a400;stroke-width:3}</style></head><body>\n"
               "<div id=\"bar\"><strong>");
    sb_xml(&b, d->name[0] ? d->name : "ER diagram");
    sb_cat(&b, "</strong><input id=\"q\" placeholder=\"Find table (Enter: next)\" aria-label=\"Find table\"><span id=\"c\"></span><button id=\"fit\">Fit</button><button id=\"zi\" aria-label=\"Zoom in\">+</button><button id=\"zo\" aria-label=\"Zoom out\">&#8722;</button></div>\n<div id=\"v\">\n");
    sb_cat(&b, svg ? svg : "");
    free(svg);
    sb_cat(&b, "</div>\n<script>\n(function(){var s=document.querySelector('#v svg');s.removeAttribute('width');s.removeAttribute('height');var vb=s.getAttribute('viewBox').split(' ').map(Number),home=vb.slice();"
               "function set(){s.setAttribute('viewBox',vb.join(' '));}"
               "function zoom(f,cx,cy){var w=vb[2]*f,h=vb[3]*f;if(w<50||w>1e6)return;cx=cx===undefined?vb[0]+vb[2]/2:cx;cy=cy===undefined?vb[1]+vb[3]/2:cy;vb=[cx-(cx-vb[0])*f,cy-(cy-vb[1])*f,w,h];set();}"
               "var v=document.getElementById('v'),drag=null;v.addEventListener('mousedown',function(e){drag=[e.clientX,e.clientY];v.style.cursor='grabbing';});"
               "window.addEventListener('mouseup',function(){drag=null;v.style.cursor='grab';});"
               "window.addEventListener('mousemove',function(e){if(!drag)return;var k=vb[2]/v.clientWidth;vb[0]-=(e.clientX-drag[0])*k;vb[1]-=(e.clientY-drag[1])*k;drag=[e.clientX,e.clientY];set();});"
               "v.addEventListener('wheel',function(e){e.preventDefault();var r=v.getBoundingClientRect(),k=vb[2]/r.width;zoom(e.deltaY>0?1.15:1/1.15,vb[0]+(e.clientX-r.left)*k,vb[1]+(e.clientY-r.top)*(vb[3]/r.height));},{passive:false});"
               "document.getElementById('fit').onclick=function(){vb=home.slice();set();};document.getElementById('zi').onclick=function(){zoom(1/1.25);};document.getElementById('zo').onclick=function(){zoom(1.25);};"
               "var q=document.getElementById('q'),c=document.getElementById('c'),hits=[],at=-1,cards=[].slice.call(document.querySelectorAll('g.card'));"
               "function show(){cards.forEach(function(g){g.classList.remove('hit');});if(at<0||!hits.length){c.textContent=q.value?'No match':'';return;}var g=hits[at];g.classList.add('hit');"
               "vb[0]=+g.getAttribute('data-x')-vb[2]/2;vb[1]=+g.getAttribute('data-y')-vb[3]/2;set();c.textContent=(at+1)+' of '+hits.length;}"
               "q.addEventListener('input',function(){var t=q.value.toLowerCase();hits=t?cards.filter(function(g){return g.getAttribute('data-name').toLowerCase().indexOf(t)>=0;}):[];at=hits.length?0:-1;show();});"
               "q.addEventListener('keydown',function(e){if(e.key==='Enter'&&hits.length){at=(at+(e.shiftKey?hits.length-1:1))%hits.length;show();}});})();\n</script>\n</body></html>\n");
    return sb_take(&b);
}
