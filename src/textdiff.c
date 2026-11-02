/* SPDX-License-Identifier: AGPL-3.0-only */
/* Line diff (Myers) and three-way conflict marking: see textdiff.h. */
#include "textdiff.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void lines_split(TextLines *l, const char *text, int len) {
    /* one entry per line; a final newline does not open an empty last line; "" has no lines */
    l->text = text; l->len = len; l->n = 0;
    int cap = 64; l->start = malloc(sizeof(int) * (size_t)cap);
    int s = 0;
    while (s < len) {
        if (l->n + 1 >= cap) { cap *= 2; l->start = realloc(l->start, sizeof(int) * (size_t)cap); }
        l->start[l->n++] = s;
        while (s < len && text[s] != '\n') s++;
        s++;   /* past the newline */
    }
    l->start[l->n] = len;
}
void lines_free(TextLines *l) { free(l->start); l->start = NULL; l->n = 0; }

int line_len(const TextLines *l, int i) {
    int s = l->start[i], e = s;
    while (e < l->len && l->text[e] != '\n') e++;
    if (e > s && l->text[e - 1] == '\r') e--;
    return e - s;
}

static bool line_eq(const TextLines *a, int i, const TextLines *b, int j, bool ignore_ws) {
    const char *x = a->text + a->start[i], *y = b->text + b->start[j];
    int xl = line_len(a, i), yl = line_len(b, j);
    if (!ignore_ws) return xl == yl && !memcmp(x, y, (size_t)xl);
    int p = 0, q = 0;
    for (;;) {
        while (p < xl && isspace((unsigned char)x[p])) p++;
        while (q < yl && isspace((unsigned char)y[q])) q++;
        if (p >= xl || q >= yl) return p >= xl && q >= yl;
        if (x[p] != y[q]) return false;
        p++; q++;
    }
}

void diff_free(Diff *d) { free(d->ops); d->ops = NULL; d->nops = 0; }

static void push(Diff *d, int *cap, int kind, int a, int b) {
    if (d->nops && d->ops[d->nops - 1].kind == kind) { d->ops[d->nops - 1].n++; return; }
    if (d->nops == *cap) { *cap = *cap ? *cap * 2 : 64; d->ops = realloc(d->ops, sizeof *d->ops * (size_t)*cap); }
    d->ops[d->nops++] = (DiffOp){ kind, a, b, 1 };
}

bool text_diff(const TextLines *A, const TextLines *B, bool ignore_ws, int max_edits, Diff *out) { return text_diff_ex(A, B, ignore_ws, max_edits, out, NULL); }

bool text_diff_ex(const TextLines *A, const TextLines *B, bool ignore_ws, int max_edits, Diff *out, const volatile int *cancel) {
    memset(out, 0, sizeof *out);
    int N = A->n, M = B->n, MAX = N + M;
    if (max_edits <= 0 || max_edits > MAX) max_edits = MAX;
    /* trim the common head and tail: most source edits are local */
    int head = 0; while (head < N && head < M && line_eq(A, head, B, head, ignore_ws)) head++;
    int tail = 0; while (tail < N - head && tail < M - head && line_eq(A, N - 1 - tail, B, M - 1 - tail, ignore_ws)) tail++;
    int n = N - head - tail, m = M - head - tail;
    int cap = 0;
    for (int i = 0; i < head; i++) push(out, &cap, DIFF_EQ, i, i);
    if (n == 0 || m == 0) {
        for (int i = 0; i < n; i++) { push(out, &cap, DIFF_DEL, head + i, head); out->removed++; }
        for (int j = 0; j < m; j++) { push(out, &cap, DIFF_INS, head + n, head + j); out->added++; }
    } else {
        /* Myers forward pass, keeping each V row for the backtrack */
        int off = n + m, vsz = 2 * (n + m) + 2;
        int dmax = n + m < max_edits ? n + m : max_edits;
        int **trace = malloc(sizeof *trace * (size_t)(dmax + 1));
        int *v = calloc((size_t)vsz, sizeof *v);
        int found = -1;
        for (int D = 0; D <= dmax && found < 0; D++) {
            if (cancel && *cancel) { dmax = D - 1; break; }   /* stopped by the caller: reported like a budget overrun */
            for (int k = -D; k <= D; k += 2) {
                int x = (k == -D || (k != D && v[off + k - 1] < v[off + k + 1])) ? v[off + k + 1] : v[off + k - 1] + 1;
                int y = x - k;
                while (x < n && y < m && line_eq(A, head + x, B, head + y, ignore_ws)) { x++; y++; }
                v[off + k] = x;
                if (x >= n && y >= m) { found = D; break; }
            }
            trace[D] = malloc(sizeof(int) * (size_t)vsz); memcpy(trace[D], v, sizeof(int) * (size_t)vsz);
        }
        free(v);
        if (found < 0) {
            for (int D = 0; D <= dmax; D++) free(trace[D]);
            out->cancelled = cancel && *cancel;
            bool was_cancelled = out->cancelled;
            free(trace); free(out->ops); memset(out, 0, sizeof *out); out->too_large = !was_cancelled; out->cancelled = was_cancelled;
            return false;
        }
        /* backtrack into a reversed edit list */
        int cap2 = n + m + 1, ne = 0;
        DiffOp *rev = malloc(sizeof *rev * (size_t)cap2);
        int x = n, y = m;
        for (int D = found; D > 0; D--) {
            int *vp = trace[D - 1]; int k = x - y;
            int pk = (k == -D || (k != D && vp[off + k - 1] < vp[off + k + 1])) ? k + 1 : k - 1;
            int px = vp[off + pk], py = px - pk;
            while (x > px && y > py) { rev[ne++] = (DiffOp){ DIFF_EQ, x - 1, y - 1, 1 }; x--; y--; }
            if (x == px) rev[ne++] = (DiffOp){ DIFF_INS, x, y - 1, 1 }, y--;
            else rev[ne++] = (DiffOp){ DIFF_DEL, x - 1, y, 1 }, x--;
        }
        while (x > 0 && y > 0) { rev[ne++] = (DiffOp){ DIFF_EQ, x - 1, y - 1, 1 }; x--; y--; }
        for (int D = 0; D <= found; D++) free(trace[D]);
        free(trace);
        for (int i = ne - 1; i >= 0; i--) {
            push(out, &cap, rev[i].kind, head + rev[i].a, head + rev[i].b);
            if (rev[i].kind == DIFF_INS) out->added++;
            if (rev[i].kind == DIFF_DEL) out->removed++;
        }
        free(rev);
    }
    for (int i = 0; i < tail; i++) push(out, &cap, DIFF_EQ, N - tail + i, M - tail + i);
    return true;
}

/* Appends formatted text to a growable string. */
typedef struct { char *s; size_t n, cap; } Str;
static void app(Str *o, const char *fmt, int l1, const char *p) {
    size_t need = o->n + (size_t)l1 + 4;
    if (need > o->cap) { o->cap = need * 2; o->s = realloc(o->s, o->cap); }
    o->s[o->n++] = fmt[0];
    memcpy(o->s + o->n, p, (size_t)l1); o->n += (size_t)l1;
    o->s[o->n++] = '\n'; o->s[o->n] = 0;
}

char *diff_unified(const TextLines *a, const TextLines *b, const Diff *d, int context) {
    Str o = { calloc(1, 64), 0, 64 };
    for (int i = 0; i < d->nops; i++) {
        const DiffOp *op = &d->ops[i];
        for (int k = 0; k < op->n; k++) {
            if (op->kind == DIFF_EQ) {
                bool near = context < 0 || (i > 0 && k < context) || (i + 1 < d->nops && op->n - k <= context);
                if (!near) { if (k == context && op->n > 2 * context) app(&o, ".", 2, ".."); continue; }
                app(&o, " ", line_len(a, op->a + k), a->text + a->start[op->a + k]);
            } else if (op->kind == DIFF_DEL) app(&o, "-", line_len(a, op->a + k), a->text + a->start[op->a + k]);
            else app(&o, "+", line_len(b, op->b + k), b->text + b->start[op->b + k]);
        }
    }
    return o.s;
}

/* Marks base lines that diff d (base -> other) deletes or replaces; an insertion marks the
 * base line before which it happens. */
static void mark(const Diff *d, int nbase, bool *touched) {
    for (int i = 0; i < d->nops; i++) {
        const DiffOp *op = &d->ops[i];
        if (op->kind == DIFF_DEL) for (int k = 0; k < op->n; k++) touched[op->a + k] = true;
        else if (op->kind == DIFF_INS) {
            /* an insertion right after a deletion replaces those lines: already marked */
            if (i > 0 && d->ops[i - 1].kind == DIFF_DEL) continue;
            int at = op->a < nbase ? op->a : nbase - 1; if (at >= 0) touched[at] = true;
        }
    }
}

int diff3_conflicts(const TextLines *base, const TextLines *local, const TextLines *remote, bool *conflict, int max_edits) {
    Diff dl, dr;
    if (!text_diff(base, local, false, max_edits, &dl)) return -1;
    if (!text_diff(base, remote, false, max_edits, &dr)) { diff_free(&dl); return -1; }
    int nb = base->n > 0 ? base->n : 1;
    bool *tl = calloc((size_t)nb, 1), *tr = calloc((size_t)nb, 1);
    mark(&dl, base->n, tl); mark(&dr, base->n, tr);
    int c = 0;
    for (int i = 0; i < base->n; i++) { conflict[i] = tl[i] && tr[i]; c += conflict[i]; }
    free(tl); free(tr); diff_free(&dl); diff_free(&dr);
    return c;
}
