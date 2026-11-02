/* SPDX-License-Identifier: AGPL-3.0-only */
/* Schema index for Lume Intelligence: see schidx.h. */
#include "schidx.h"
#include <ctype.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#include <math.h>
#include <fcntl.h>
#include <unistd.h>

static char *dupz(const char *s) { return strdup(s ? s : ""); }

/* ---------------------------------------------------------------- building */
void schidx_init(SchIdx *x) { memset(x, 0, sizeof *x); }
void schidx_free(SchIdx *x) {
    for (int i = 0; i < x->n; i++) {
        SxTable *t = &x->t[i];
        free(t->schema); free(t->name); free(t->comment);
        for (int k = 0; k < t->ncols; k++) { free(t->cols[k].name); free(t->cols[k].type); free(t->cols[k].comment); }
        for (int k = 0; k < t->nfks; k++) { free(t->fks[k].cols); free(t->fks[k].ref_table); }
        free(t->cols); free(t->fks); free(t->usage); free(t->w); free(t->wcat);
    }
    free(x->t); free(x->slot);
    memset(x, 0, sizeof *x);
}
static uint32_t hkey(const char *schema, const char *name) {
    uint32_t h = 2166136261u;
    for (const char *p = schema; p && *p; p++) { h ^= (uint32_t)tolower((unsigned char)*p); h *= 16777619u; }
    h ^= '.'; h *= 16777619u;
    for (const char *p = name; *p; p++) { h ^= (uint32_t)tolower((unsigned char)*p); h *= 16777619u; }
    return h;
}
static void rehash(SchIdx *x) {
    free(x->slot);
    x->nslot = 64; while (x->nslot < x->n * 2 + 16) x->nslot *= 2;
    x->slot = malloc(sizeof(int) * (size_t)x->nslot);
    for (int i = 0; i < x->nslot; i++) x->slot[i] = -1;
    for (int i = 0; i < x->n; i++) {
        uint32_t h = hkey(x->t[i].schema, x->t[i].name) & (uint32_t)(x->nslot - 1);
        while (x->slot[h] >= 0) h = (h + 1) & (uint32_t)(x->nslot - 1);
        x->slot[h] = i;
    }
}
static int find_exact(const SchIdx *x, const char *schema, const char *name) {
    if (!x->slot) return -1;
    uint32_t h = hkey(schema, name) & (uint32_t)(x->nslot - 1);
    while (x->slot[h] >= 0) {
        const SxTable *t = &x->t[x->slot[h]];
        if (!strcasecmp(t->name, name) && !strcasecmp(t->schema, schema ? schema : "")) return x->slot[h];
        h = (h + 1) & (uint32_t)(x->nslot - 1);
    }
    return -1;
}
int schidx_find(const SchIdx *x, const char *schema, const char *name) {
    if (!name || !*name) return -1;
    if ((!schema || !*schema) && strchr(name, '.')) {
        char s[256]; const char *dot = strchr(name, '.');
        snprintf(s, sizeof s, "%.*s", (int)(dot - name), name);
        return find_exact(x, s, dot + 1);
    }
    if (schema && *schema) return find_exact(x, schema, name);
    int hit = -1;   /* no schema: unique match by name */
    for (int i = 0; i < x->n; i++) if (!strcasecmp(x->t[i].name, name)) { if (hit >= 0) return hit; hit = i; }
    return hit;
}
int schidx_table(SchIdx *x, const char *schema, const char *name, bool is_view) {
    int i = find_exact(x, schema ? schema : "", name);
    if (i >= 0) return i;
    if (x->n == x->cap) { x->cap = x->cap ? x->cap * 2 : 256; x->t = realloc(x->t, sizeof *x->t * (size_t)x->cap); }
    SxTable *t = &x->t[x->n]; memset(t, 0, sizeof *t);
    t->schema = dupz(schema); t->name = dupz(name); t->is_view = is_view;
    x->n++;
    if (x->n * 2 >= x->nslot) rehash(x);
    else {
        uint32_t h = hkey(t->schema, t->name) & (uint32_t)(x->nslot - 1);
        while (x->slot[h] >= 0) h = (h + 1) & (uint32_t)(x->nslot - 1);
        x->slot[h] = x->n - 1;
    }
    return x->n - 1;
}
void schidx_table_comment(SchIdx *x, int ti, const char *comment) {
    if (ti < 0 || ti >= x->n || !comment || !*comment) return;
    free(x->t[ti].comment); x->t[ti].comment = dupz(comment);
}
void schidx_add_col(SchIdx *x, int ti, const char *name, const char *type, bool pk, bool notnull, const char *comment) {
    if (ti < 0 || ti >= x->n || !name) return;
    SxTable *t = &x->t[ti];
    for (int k = 0; k < t->ncols; k++) if (!strcasecmp(t->cols[k].name, name)) return;
    if (t->ncols == t->capcols) { t->capcols = t->capcols ? t->capcols * 2 : 8; t->cols = realloc(t->cols, sizeof *t->cols * (size_t)t->capcols); }
    SxCol *c = &t->cols[t->ncols++];
    c->name = dupz(name); c->type = dupz(type); c->comment = comment && *comment ? dupz(comment) : NULL; c->pk = pk; c->notnull = notnull;
}
void schidx_set_col_comment(SchIdx *x, int ti, const char *col, const char *comment) {
    if (ti < 0 || ti >= x->n || !comment || !*comment) return;
    SxTable *t = &x->t[ti];
    for (int k = 0; k < t->ncols; k++) if (!strcasecmp(t->cols[k].name, col)) { free(t->cols[k].comment); t->cols[k].comment = dupz(comment); return; }
}
void schidx_add_fk(SchIdx *x, int ti, const char *cols, const char *ref_schema, const char *ref_name) {
    if (ti < 0 || ti >= x->n || !ref_name) return;
    SxTable *t = &x->t[ti];
    if (t->nfks == t->capfks) { t->capfks = t->capfks ? t->capfks * 2 : 4; t->fks = realloc(t->fks, sizeof *t->fks * (size_t)t->capfks); }
    SxFk *f = &t->fks[t->nfks++];
    char rt[600]; snprintf(rt, sizeof rt, "%s%s%s", ref_schema && *ref_schema ? ref_schema : "", ref_schema && *ref_schema ? "." : "", ref_name);
    f->cols = dupz(cols); f->ref_table = dupz(rt); f->ref = -1;
}
void schidx_add_usage(SchIdx *x, int ti, const char *text) {
    if (ti < 0 || ti >= x->n || !text || !*text) return;
    SxTable *t = &x->t[ti];
    size_t old = t->usage ? strlen(t->usage) : 0, add = strlen(text);
    if (old > 4000) return;   /* enough words per table */
    if (add > 600) add = 600;
    t->usage = realloc(t->usage, old + add + 2);
    if (old) t->usage[old++] = ' ';
    memcpy(t->usage + old, text, add); t->usage[old + add] = 0;
    free(t->w); free(t->wcat); t->w = NULL; t->wcat = NULL; t->nw = 0; x->words_ready = false;
}
void schidx_finish(SchIdx *x) {
    if (!x->slot || x->n * 2 >= x->nslot) rehash(x);
    for (int i = 0; i < x->n; i++) x->t[i].nrefs = 0;
    for (int i = 0; i < x->n; i++)
        for (int k = 0; k < x->t[i].nfks; k++) {
            SxFk *f = &x->t[i].fks[k];
            f->ref = schidx_find(x, NULL, f->ref_table);
            if (f->ref >= 0 && f->ref != i) x->t[f->ref].nrefs++;
        }
    x->built_at = (double)time(NULL);
}

/* ---------------------------------------------------------------- words */
/* one UTF-8 Latin letter with an accent -> its ASCII base; 0 when not one */
static int fold_utf8(const unsigned char *p, int *len) {
    if (p[0] == 0xC3 && p[1]) {
        unsigned c = p[1]; *len = 2;
        if ((c >= 0x80 && c <= 0x85) || (c >= 0xA0 && c <= 0xA5)) return 'a';
        if (c == 0x87 || c == 0xA7) return 'c';
        if ((c >= 0x88 && c <= 0x8B) || (c >= 0xA8 && c <= 0xAB)) return 'e';
        if ((c >= 0x8C && c <= 0x8F) || (c >= 0xAC && c <= 0xAF)) return 'i';
        if (c == 0x91 || c == 0xB1) return 'n';
        if ((c >= 0x92 && c <= 0x96) || (c >= 0xB2 && c <= 0xB6)) return 'o';
        if ((c >= 0x99 && c <= 0x9C) || (c >= 0xB9 && c <= 0xBC)) return 'u';
        return '_';
    }
    *len = 1; return 0;
}
static const char *const STOP[] = {
    "de", "da", "do", "das", "dos", "e", "o", "a", "os", "as", "por", "para", "pra", "com", "em", "no", "na", "nos", "nas",
    "um", "uma", "uns", "umas", "que", "qual", "quais", "quanto", "quantos", "quantas", "me", "meu", "minha", "mostre", "mostra",
    "liste", "lista", "listar", "traga", "quero", "ver", "cada", "todo", "todos", "toda", "todas", "ao", "aos", "se", "sem", "mais",
    "the", "of", "by", "per", "and", "for", "in", "on", "to", "show", "list", "all", "each", "how", "many", "much", "what", "which",
    "tb", "tt", "th", "au", "vw", "tmp", NULL };
static bool stopword(const char *w) { for (int i = 0; STOP[i]; i++) if (!strcmp(STOP[i], w)) return true; return false; }
/* light Portuguese / English plural folding: pedidos -> pedido, operacoes -> operacao, valores -> valor */
static void stem(char *w) {
    size_t n = strlen(w);
    if (n > 4 && !strcmp(w + n - 3, "oes")) { strcpy(w + n - 3, "ao"); return; }
    if (n > 4 && !strcmp(w + n - 3, "aes")) { strcpy(w + n - 3, "ao"); return; }
    if (n > 4 && !strcmp(w + n - 3, "ais")) { strcpy(w + n - 3, "al"); return; }
    if (n > 4 && !strcmp(w + n - 3, "eis")) { strcpy(w + n - 3, "el"); return; }
    if (n > 4 && (!strcmp(w + n - 3, "res") || !strcmp(w + n - 3, "zes") || !strcmp(w + n - 3, "ses"))) { w[n - 2] = 0; return; }
    if (n > 3 && w[n - 1] == 's' && w[n - 2] != 's' && w[n - 2] != 'u' && w[n - 2] != 'i') w[n - 1] = 0;
}
int schidx_tokens(const char *text, char out[][32], int max) {
    int n = 0; char cur[32]; int k = 0;
    const unsigned char *p = (const unsigned char *)(text ? text : "");
    for (;;) {
        int len = 1, ch = 0;
        if (*p >= 0x80) { int f = fold_utf8(p, &len); ch = f && f != '_' ? f : 0; if (!f) { len = 1; } }
        else if (isalnum(*p)) ch = tolower(*p);
        bool split_camel = *p && isupper(*p) && k > 0 && p > (const unsigned char *)text && islower(p[-1]);
        if (!ch || split_camel || !*p) {
            if (k > 0) {
                cur[k] = 0; stem(cur);
                bool dup = false; for (int i = 0; i < n; i++) if (!strcmp(out[i], cur)) dup = true;
                if (!dup && !stopword(cur) && (k >= 2 || isdigit((unsigned char)cur[0])) && n < max) snprintf(out[n++], 32, "%s", cur);
                k = 0;
            }
            if (!*p) break;
            if (split_camel) { cur[k++] = (char)tolower(*p); p += len; continue; }
            p += len; continue;
        }
        if (k < 31) cur[k++] = (char)ch;
        p += len;
    }
    return n;
}
void schidx_skeleton(const char *word, char *out, size_t n) {
    size_t k = 0; char last = 0;
    for (const char *p = word; *p && k + 1 < n; p++) {
        char c = (char)tolower((unsigned char)*p);
        if (strchr("aeiouy", c) || !isalpha((unsigned char)c)) continue;
        if (c == last) continue;
        out[k++] = c; last = c;
    }
    out[k] = 0;
}
static int vowels(const char *s) { int v = 0; for (; *s; s++) if (strchr("aeiou", *s)) v++; return v; }

/* how well one query word matches one identifier / description word */
static float word_match(const char *q, const char *qskel, const char *w) {
    if (!strcmp(q, w)) return 1.0f;
    size_t lq = strlen(q), lw = strlen(w);
    if (lw >= 3 && lq > lw && !strncmp(q, w, lw)) return 0.75f;      /* "benef" in "beneficiario" */
    if (lq >= 4 && lw > lq && !strncmp(w, q, lq)) return 0.7f;       /* "pedido" in "pedidoitem" */
    if (lw >= 3 && vowels(w) <= 1) {                                 /* abbreviations: "bnf", "prest" */
        char ws[32]; schidx_skeleton(w, ws, sizeof ws);
        if (strlen(ws) >= 3 && !strncmp(qskel, ws, strlen(ws))) return 0.55f;
    }
    return 0;
}

/* ---------------------------------------------------------------- picking */
typedef struct QW { char w[32], sk[32]; } QW;
/* audit copies, temporary, staging, log and backup tables: kept, but after the tables they copy */
static bool side_table(const char *name) {
    char u[160]; size_t k = 0; for (const char *p = name; *p && k < sizeof u - 1; p++) u[k++] = (char)toupper((unsigned char)*p); u[k] = 0;
    static const char *const pre[] = { "AU_", "TT_", "TMP", "TEMP", "BKP", "BAK", "LOG_", "STG_", "OLD_", NULL };
    static const char *const suf[] = { "_LOG", "_BKP", "_BAK", "_OLD", "_TMP", "_TEMP", "_HIST", NULL };
    for (int i = 0; pre[i]; i++) if (!strncmp(u, pre[i], strlen(pre[i]))) return true;
    for (int i = 0; suf[i]; i++) { size_t l = strlen(suf[i]); if (k > l && !strcmp(u + k - l, suf[i])) return true; }
    return strstr(u, "_TEMP_") || strstr(u, "_TMP_");
}
/* word categories and their weights: table name, table description, column names, column
 * descriptions, the user's own queries that use the table */
enum { WC_NAME, WC_DESC, WC_COL, WC_COLDESC, WC_USAGE, WC_N };
static const float WC_W[WC_N] = { 10.0f, 8.0f, 3.0f, 2.4f, 4.0f };
static void words_add(SxTable *t, int *cap, const char *text, int cat, int max) {
    char tk[96][32]; int n = schidx_tokens(text, tk, max < 96 ? max : 96);
    for (int i = 0; i < n; i++) {
        bool dup = false; for (int k = 0; k < t->nw && !dup; k++) dup = t->wcat[k] == cat && !strcmp(t->w[k], tk[i]);
        if (dup) continue;
        if (t->nw == *cap) { *cap = *cap ? *cap * 2 : 16; t->w = realloc(t->w, sizeof *t->w * (size_t)*cap); t->wcat = realloc(t->wcat, (size_t)*cap); }
        snprintf(t->w[t->nw], sizeof t->w[0], "%s", tk[i]); t->wcat[t->nw] = (unsigned char)cat; t->nw++;
    }
}
static void table_words(SxTable *t) {
    if (t->w) return;
    int cap = 0; t->nw = 0;
    words_add(t, &cap, t->name, WC_NAME, 32);
    if (t->comment) words_add(t, &cap, t->comment, WC_DESC, 64);
    for (int k = 0; k < t->ncols && k < 400; k++) { words_add(t, &cap, t->cols[k].name, WC_COL, 16); if (t->cols[k].comment) words_add(t, &cap, t->cols[k].comment, WC_COLDESC, 32); }
    if (t->usage) words_add(t, &cap, t->usage, WC_USAGE, 96);
    if (!t->w) { t->w = malloc(sizeof *t->w); t->wcat = malloc(1); }   /* computed, even when empty */
}
static void schidx_words(SchIdx *x) {
    if (x->words_ready) return;
    for (int i = 0; i < x->n; i++) table_words(&x->t[i]);
    x->words_ready = true;
}
static float table_score(const SxTable *t, const QW *q, int nq, char *why, size_t wn) {
    float best[WC_N][32]; memset(best, 0, sizeof best);
    for (int k = 0; k < t->nw; k++)
        for (int i = 0; i < nq; i++) { float m = word_match(q[i].w, q[i].sk, t->w[k]); if (m > best[t->wcat[k]][i]) best[t->wcat[k]][i] = m; }
    float s = 0, sc[WC_N] = { 0 }; int covered = 0;
    for (int i = 0; i < nq; i++) {
        float hit = 0;
        for (int c = 0; c < WC_N; c++) { sc[c] += best[c][i] * WC_W[c]; float h = c >= WC_COL ? best[c][i] * 0.5f : best[c][i]; if (h > hit) hit = h; }
        if (hit > 0) covered++;
    }
    for (int c = 0; c < WC_N; c++) s += sc[c];
    if (nq > 1) s *= 0.6f + 0.4f * (float)covered / (float)nq;   /* tables that cover more of the question first */
    if (s > 0) s += 1.5f * log2f(1.0f + (float)t->nrefs);       /* central tables (many references) first */
    if (s > 0 && side_table(t->name)) s *= 0.4f;                 /* audit copies, temporary and log tables after the real ones */
    if (why) snprintf(why, wn, "%s%s%s%s", sc[WC_NAME] > 0 ? "name " : "", sc[WC_DESC] > 0 ? "description " : "", sc[WC_COL] + sc[WC_COLDESC] > 0 ? "columns " : "", sc[WC_USAGE] > 0 ? "your queries" : "");
    return s;
}
static int cmp_pick(const void *a, const void *b) {
    const SxPick *x = a, *y = b;
    return x->score < y->score ? 1 : x->score > y->score ? -1 : x->t - y->t;
}
static int words_of(const char *text, QW *q, int max) {
    char w[32][32]; int n = schidx_tokens(text, w, max < 32 ? max : 32);
    for (int i = 0; i < n; i++) { snprintf(q[i].w, 32, "%s", w[i]); schidx_skeleton(w[i], q[i].sk, 32); }
    return n;
}
int schidx_search(const SchIdx *x, const char *terms, SxPick *out, int max) {
    schidx_words((SchIdx *)x);   /* lazily, once per index (and after new usage) */
    QW q[32]; int nq = words_of(terms, q, 32);
    if (!nq || max <= 0) return 0;
    int n = 0; float worst = 0;
    for (int i = 0; i < x->n; i++) {
        char why[64]; float s = table_score(&x->t[i], q, nq, why, sizeof why);
        if (s <= 0) continue;
        if (n < max) { out[n].t = i; out[n].score = s; snprintf(out[n].why, sizeof out[n].why, "%s", why); n++; if (n == max) { qsort(out, (size_t)n, sizeof *out, cmp_pick); worst = out[n - 1].score; } }
        else if (s > worst) { out[n - 1].t = i; out[n - 1].score = s; snprintf(out[n - 1].why, sizeof out[n - 1].why, "%s", why); qsort(out, (size_t)n, sizeof *out, cmp_pick); worst = out[n - 1].score; }
    }
    qsort(out, (size_t)n, sizeof *out, cmp_pick);
    return n;
}
static bool picked(const SxPick *p, int n, int t) { for (int i = 0; i < n; i++) if (p[i].t == t) return true; return false; }
int schidx_pick(const SchIdx *x, const SxQuery *qq, SxPick *out, int max) {
    int lim = qq->max_tables > 0 ? qq->max_tables : 30; if (lim > max) lim = max;
    int n = 0;
    for (int i = 0; i < qq->nboost && n < lim; i++) {   /* what the user is working on, first */
        int t = schidx_find(x, NULL, qq->boost[i]);
        if (t >= 0 && !picked(out, n, t)) { out[n].t = t; out[n].score = 1000; snprintf(out[n].why, sizeof out[n].why, "in use"); n++; }
    }
    int room = lim - n; if (room <= 0) return n;
    int want = room - room / 4;   /* a quarter is left for foreign-key neighbours */
    SxPick *tmp = malloc(sizeof *tmp * (size_t)(want > 0 ? want : 1));
    int nt = schidx_search(x, qq->question, tmp, want);
    for (int i = 0; i < nt && n < lim; i++) if (!picked(out, n, tmp[i].t)) out[n++] = tmp[i];
    int top = n;
    for (int i = 0; i < top && n < lim; i++) {   /* one hop: what the best tables reference, and who references them */
        const SxTable *t = &x->t[out[i].t];
        for (int k = 0; k < t->nfks && n < lim; k++) {
            int r = t->fks[k].ref;
            if (r >= 0 && !picked(out, n, r)) { out[n].t = r; out[n].score = out[i].score * 0.3f; snprintf(out[n].why, sizeof out[n].why, "referenced by %.40s", t->name); n++; }
        }
    }
    for (int j = 0; j < x->n && n < lim && top > 0; j++) {
        if (picked(out, n, j)) continue;
        for (int k = 0; k < x->t[j].nfks; k++) {
            int r = x->t[j].fks[k].ref;
            if (r >= 0 && r == out[0].t) { out[n].t = j; out[n].score = out[0].score * 0.25f; snprintf(out[n].why, sizeof out[n].why, "references %.40s", x->t[r].name); n++; break; }
        }
    }
    free(tmp);
    if (x->n <= lim)   /* a small database fits whole: the rest goes too, after the best matches */
        for (int j = 0; j < x->n && n < lim; j++) if (!picked(out, n, j)) { out[n].t = j; out[n].score = 0; snprintf(out[n].why, sizeof out[n].why, "small database"); n++; }
    return n;
}

/* ---------------------------------------------------------------- rendering */
typedef struct SB { char *b; size_t w, cap; } SB;
static void sb_put(SB *s, const char *t, size_t len) {
    if (s->w + len + 1 > s->cap) { while (s->w + len + 1 > s->cap) s->cap = s->cap ? s->cap * 2 : 4096; s->b = realloc(s->b, s->cap); }
    memcpy(s->b + s->w, t, len); s->w += len; s->b[s->w] = 0;
}
static void sb_cat(SB *s, const char *t) { sb_put(s, t, strlen(t)); }
/* untrusted text from the catalog: one line, no markup, capped */
static void sb_text(SB *s, const char *t, size_t max) {
    size_t k = 0;
    for (const unsigned char *p = (const unsigned char *)t; *p && k < max; p++, k++) {
        char c = (char)*p;
        if (c == '<' || c == '>') c = ' ';
        if (c == '\n' || c == '\r' || c == '\t') c = ' ';
        sb_put(s, &c, 1);
    }
    if (k >= max && t[k]) sb_cat(s, "...");
}
static void render_table(SB *s, const SchIdx *x, const SxTable *t, bool full) {
    sb_cat(s, t->is_view ? "view " : "table ");
    if (t->schema[0]) { sb_text(s, t->schema, 128); sb_cat(s, "."); }
    sb_text(s, t->name, 128);
    if (t->comment) { sb_cat(s, " -- "); sb_text(s, t->comment, full ? 160 : 100); }
    sb_cat(s, "\n");
    if (!full) return;
    for (int k = 0; k < t->ncols && k < 120; k++) {
        const SxCol *c = &t->cols[k];
        sb_cat(s, "  "); sb_text(s, c->name, 128); sb_cat(s, " "); sb_text(s, c->type, 64);
        if (c->pk) sb_cat(s, " PK");
        if (c->notnull && !c->pk) sb_cat(s, " NOT NULL");
        if (c->comment) { sb_cat(s, " -- "); sb_text(s, c->comment, 80); }
        sb_cat(s, "\n");
    }
    if (t->ncols > 120) { char m[64]; snprintf(m, sizeof m, "  (%d more columns)\n", t->ncols - 120); sb_cat(s, m); }
    for (int k = 0; k < t->nfks && k < 30; k++) {
        sb_cat(s, "  fk ("); sb_text(s, t->fks[k].cols, 200); sb_cat(s, ") -> "); sb_text(s, t->fks[k].ref_table, 200);
        sb_cat(s, x && t->fks[k].ref < 0 ? " (not in the index)\n" : "\n");
    }
}
typedef struct SC { const char *schema; int n; } SC;
static int cmp_sc(const void *a, const void *b) { const SC *x = a, *y = b; return y->n - x->n; }
char *schidx_render(const SchIdx *x, const SxPick *p, int np, const char *conn_name, size_t max_bytes, int *rendered) {
    SB s = { 0 };
    char h[400];
    snprintf(h, sizeof h, "<schema connection=\"%s\" dialect=\"%s\" untrusted=\"true\" tables_in_database=\"%d\">\n", conn_name ? conn_name : "", x->dialect, x->n);
    sb_cat(&s, h);
    /* the shape of the whole database: schemas and how many tables each holds */
    SC *sc = calloc((size_t)(x->n ? x->n : 1), sizeof *sc); int ns = 0;
    for (int i = 0; i < x->n; i++) {
        int k = 0; for (; k < ns; k++) if (!strcmp(sc[k].schema, x->t[i].schema)) break;
        if (k == ns) { sc[ns].schema = x->t[i].schema; sc[ns].n = 0; ns++; }
        sc[k].n++;
    }
    qsort(sc, (size_t)ns, sizeof *sc, cmp_sc);
    if (ns > 1 || x->n > np) {
        sb_cat(&s, "Database summary: ");
        for (int i = 0; i < ns && i < 20; i++) { char m[200]; snprintf(m, sizeof m, "%s%s (%d)", i ? ", " : "", sc[i].schema[0] ? sc[i].schema : "default", sc[i].n); sb_text(&s, m, 200); }
        if (ns > 20) { char m[64]; snprintf(m, sizeof m, ", and %d more schemas", ns - 20); sb_cat(&s, m); }
        sb_cat(&s, "\n");
    }
    free(sc);
    if (np) sb_cat(&s, x->n > np ? "Tables picked for this question (the rest is not listed):\n" : "Tables:\n");
    int done = 0;
    const char *tail = x->n > np
        ? "More tables exist. To see others, answer {\"type\":\"search_schema\",\"terms\":\"words in the user's language\"} or {\"type\":\"describe_tables\",\"tables\":[\"SCHEMA.TABLE\"]}: Lume searches its local index of names and descriptions and sends them back.\n</schema>"
        : "</schema>";
    size_t reserve = strlen(tail) + 64;
    for (int i = 0; i < np; i++) {
        SB one = { 0 };
        render_table(&one, x, &x->t[p[i].t], true);
        if (s.w + one.w + reserve > max_bytes) {   /* out of room: the rest by name only */
            free(one.b); one = (SB){ 0 }; render_table(&one, x, &x->t[p[i].t], false);
            if (s.w + one.w + reserve > max_bytes) { free(one.b); break; }
        }
        sb_put(&s, one.b, one.w); free(one.b); done++;
    }
    if (np > done) { char m[96]; snprintf(m, sizeof m, "(%d picked tables did not fit)\n", np - done); sb_cat(&s, m); }
    sb_cat(&s, tail);
    if (rendered) *rendered = done;
    return s.b;
}
char *schidx_render_list(const SchIdx *x, const SxPick *p, int np, size_t max_bytes) {
    SB s = { 0 }; sb_cat(&s, "");
    for (int i = 0; i < np; i++) {
        SB one = { 0 }; render_table(&one, x, &x->t[p[i].t], false);
        if (s.w + one.w + 40 > max_bytes) { free(one.b); char m[64]; snprintf(m, sizeof m, "(%d more matches)\n", np - i); sb_cat(&s, m); break; }
        sb_put(&s, one.b, one.w); free(one.b);
    }
    return s.b;
}

/* ---------------------------------------------------------------- disk cache */
static void put_field(FILE *f, const char *s) {
    for (const char *p = s ? s : ""; *p; p++) {
        if (*p == '\t') fputs("\\t", f); else if (*p == '\n') fputs("\\n", f); else if (*p == '\r') fputs("\\r", f); else if (*p == '\\') fputs("\\\\", f); else fputc(*p, f);
    }
}
static int split_fields(char *line, char **f, int max) {
    int n = 0; char *w = line, *start = line;
    for (char *p = line;; p++) {
        if (*p == '\\' && p[1]) { p++; *w++ = *p == 't' ? '\t' : *p == 'n' ? '\n' : *p == 'r' ? '\r' : *p; continue; }
        if (*p == '\t' || *p == '\n' || !*p) {
            bool end = !*p || *p == '\n';
            *w = 0; if (n < max) f[n++] = start; w++; start = w;
            if (end) break;
            continue;
        }
        *w++ = *p;
    }
    return n;
}
bool schidx_save(const SchIdx *x, const char *path) {
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.part", path);
    int fd = open(tmp, O_WRONLY | O_CREAT | O_TRUNC, 0600);   /* the structure of someone's database: owner only */
    FILE *f = fd >= 0 ? fdopen(fd, "w") : NULL; if (!f) { if (fd >= 0) close(fd); return false; }
    fprintf(f, "LUMESCHIDX\t1\t%s\t%.0f\n", x->dialect, x->built_at);
    for (int i = 0; i < x->n; i++) {
        const SxTable *t = &x->t[i];
        fputs("T\t", f); put_field(f, t->schema); fputc('\t', f); put_field(f, t->name); fprintf(f, "\t%d\t", t->is_view); put_field(f, t->comment); fputc('\n', f);
        for (int k = 0; k < t->ncols; k++) { const SxCol *c = &t->cols[k]; fputs("C\t", f); put_field(f, c->name); fputc('\t', f); put_field(f, c->type); fprintf(f, "\t%d\t%d\t", c->pk, c->notnull); put_field(f, c->comment); fputc('\n', f); }
        for (int k = 0; k < t->nfks; k++) { fputs("F\t", f); put_field(f, t->fks[k].cols); fputc('\t', f); put_field(f, t->fks[k].ref_table); fputc('\n', f); }
    }
    bool ok = fflush(f) == 0 && !ferror(f);
    ok = fclose(f) == 0 && ok;
    if (!ok || rename(tmp, path) != 0) { remove(tmp); return false; }
    return true;
}
bool schidx_load(SchIdx *x, const char *path) {
    FILE *f = fopen(path, "r"); if (!f) return false;
    schidx_init(x);
    size_t cap = 0; char *line = NULL; ssize_t got; int ti = -1; bool ok = false;
    while ((got = getline(&line, &cap, f)) > 0) {
        char *fl[8]; int n = split_fields(line, fl, 8);
        if (!ok) { if (n >= 4 && !strcmp(fl[0], "LUMESCHIDX") && !strcmp(fl[1], "1")) { snprintf(x->dialect, sizeof x->dialect, "%s", fl[2]); x->built_at = atof(fl[3]); ok = true; continue; } break; }
        if (n >= 5 && !strcmp(fl[0], "T")) { ti = schidx_table(x, fl[1], fl[2], atoi(fl[3]) != 0); schidx_table_comment(x, ti, fl[4]); }
        else if (n >= 6 && !strcmp(fl[0], "C")) schidx_add_col(x, ti, fl[1], fl[2], atoi(fl[3]) != 0, atoi(fl[4]) != 0, fl[5]);
        else if (n >= 3 && !strcmp(fl[0], "F") && ti >= 0) { SxTable *t = &x->t[ti]; if (t->nfks == t->capfks) { t->capfks = t->capfks ? t->capfks * 2 : 4; t->fks = realloc(t->fks, sizeof *t->fks * (size_t)t->capfks); } t->fks[t->nfks].cols = dupz(fl[1]); t->fks[t->nfks].ref_table = dupz(fl[2]); t->fks[t->nfks].ref = -1; t->nfks++; }
    }
    free(line); fclose(f);
    if (!ok) { schidx_free(x); return false; }
    double built = x->built_at; schidx_finish(x); x->built_at = built;
    return true;
}

/* ---------------------------------------------------------------- catalog SQL */
/* schemas filter: names are quoted as SQL literals (single quotes doubled) */
static void in_list(const char *col, const char *const *s, int n, bool upper, char *o, size_t on) {
    if (!n) { o[0] = 0; return; }
    size_t w = (size_t)snprintf(o, on, " AND %s IN (", col);
    for (int i = 0; i < n && w + 8 < on; i++) {
        if (i) o[w++] = ',';
        o[w++] = '\'';
        for (const char *p = s[i]; *p && w + 4 < on; p++) { char c = upper ? (char)toupper((unsigned char)*p) : *p; if (c == '\'') o[w++] = '\''; o[w++] = c; }
        o[w++] = '\'';
    }
    if (w + 2 < on) { o[w++] = ')'; o[w] = 0; } else o[on - 1] = 0;
}
bool schidx_sql(const char *d, int k, const char *const *schemas, int ns, int *kind, char *out, size_t n) {
    char f[4096];
    if (!strcmp(d, "postgres")) {
        in_list("n.nspname", schemas, ns, false, f, sizeof f);
        /* only what this login may read (pg_class lists every table to everyone) */
        const char *sys = "n.nspname NOT IN ('pg_catalog','information_schema') AND n.nspname NOT LIKE 'pg\\_toast%' AND n.nspname NOT LIKE 'pg\\_temp%' AND has_table_privilege(c.oid, 'SELECT')";
        if (k == 0) { *kind = SXQ_COLS; snprintf(out, n,
            "SELECT n.nspname, c.relname, CASE WHEN c.relkind IN ('v','m') THEN 1 ELSE 0 END, a.attname, format_type(a.atttypid, a.atttypmod), "
            "CASE WHEN EXISTS (SELECT 1 FROM pg_index i WHERE i.indrelid = c.oid AND i.indisprimary AND a.attnum = ANY (i.indkey)) THEN 1 ELSE 0 END, "
            "CASE WHEN a.attnotnull THEN 1 ELSE 0 END, col_description(c.oid, a.attnum) "
            "FROM pg_class c JOIN pg_namespace n ON n.oid = c.relnamespace JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum > 0 AND NOT a.attisdropped "
            "WHERE c.relkind IN ('r','p','v','m','f') AND %s%s ORDER BY 1, 2, a.attnum", sys, f); return true; }
        if (k == 1) { *kind = SXQ_TCOMMENT; snprintf(out, n,
            "SELECT n.nspname, c.relname, d.description FROM pg_description d JOIN pg_class c ON c.oid = d.objoid AND d.classoid = 'pg_class'::regclass AND d.objsubid = 0 "
            "JOIN pg_namespace n ON n.oid = c.relnamespace WHERE %s%s", sys, f); return true; }
        if (k == 2) { *kind = SXQ_FKS; snprintf(out, n,
            "SELECT n.nspname, c.relname, (SELECT string_agg(a.attname, ',' ORDER BY u.ord) FROM unnest(co.conkey) WITH ORDINALITY u(attnum, ord) JOIN pg_attribute a ON a.attrelid = c.oid AND a.attnum = u.attnum), "
            "rn.nspname, rc.relname FROM pg_constraint co JOIN pg_class c ON c.oid = co.conrelid JOIN pg_namespace n ON n.oid = c.relnamespace "
            "JOIN pg_class rc ON rc.oid = co.confrelid JOIN pg_namespace rn ON rn.oid = rc.relnamespace WHERE co.contype = 'f' AND %s%s", sys, f); return true; }
        return false;
    }
    if (!strcmp(d, "oracle")) {
        char fo[4096]; in_list("c.owner", schemas, ns, true, fo, sizeof fo);
        const char *sys = ns ? "1 = 1" : "c.owner IN (SELECT username FROM all_users WHERE oracle_maintained = 'N')";
        if (k == 0) { *kind = SXQ_COLS; snprintf(out, n,
            "SELECT c.owner, c.table_name, CASE WHEN v.view_name IS NULL THEN 0 ELSE 1 END, c.column_name, "
            "c.data_type || CASE WHEN c.data_type IN ('VARCHAR2','CHAR','NVARCHAR2','NCHAR') THEN '(' || c.char_length || ')' WHEN c.data_type = 'RAW' THEN '(' || c.data_length || ')' "
            "WHEN c.data_type = 'NUMBER' AND c.data_precision IS NOT NULL THEN '(' || c.data_precision || CASE WHEN c.data_scale > 0 THEN ',' || c.data_scale END || ')' END, "
            "CASE WHEN pk.column_name IS NULL THEN 0 ELSE 1 END, CASE c.nullable WHEN 'N' THEN 1 ELSE 0 END, cc.comments "
            "FROM all_tab_columns c LEFT JOIN all_views v ON v.owner = c.owner AND v.view_name = c.table_name "
            "LEFT JOIN all_col_comments cc ON cc.owner = c.owner AND cc.table_name = c.table_name AND cc.column_name = c.column_name "
            "LEFT JOIN (SELECT acc.owner, acc.table_name, acc.column_name FROM all_constraints ac JOIN all_cons_columns acc ON acc.owner = ac.owner AND acc.constraint_name = ac.constraint_name WHERE ac.constraint_type = 'P') pk "
            "ON pk.owner = c.owner AND pk.table_name = c.table_name AND pk.column_name = c.column_name "
            "WHERE %s%s AND c.table_name NOT LIKE 'BIN$%%' ORDER BY c.owner, c.table_name, c.column_id", sys, fo); return true; }
        if (k == 1) { *kind = SXQ_TCOMMENT; snprintf(out, n,
            "SELECT c.owner, c.table_name, c.comments FROM all_tab_comments c WHERE c.comments IS NOT NULL AND %s%s", sys, fo); return true; }
        if (k == 2) { *kind = SXQ_FKS; snprintf(out, n,
            "SELECT c.owner, c.table_name, LISTAGG(cc.column_name, ',') WITHIN GROUP (ORDER BY cc.position), r.owner, r.table_name "
            "FROM all_constraints c JOIN all_cons_columns cc ON cc.owner = c.owner AND cc.constraint_name = c.constraint_name "
            "JOIN all_constraints r ON r.owner = c.r_owner AND r.constraint_name = c.r_constraint_name "
            "WHERE c.constraint_type = 'R' AND %s%s GROUP BY c.owner, c.constraint_name, c.table_name, r.owner, r.table_name", sys, fo); return true; }
        return false;
    }
    if (!strcmp(d, "mysql")) {
        char fm[4096]; in_list("c.table_schema", schemas, ns, false, fm, sizeof fm);
        const char *sys = "c.table_schema NOT IN ('mysql','information_schema','performance_schema','sys')";
        if (k == 0) { *kind = SXQ_COLS; snprintf(out, n,
            "SELECT c.table_schema, c.table_name, CASE WHEN t.table_type = 'VIEW' THEN 1 ELSE 0 END, c.column_name, c.column_type, "
            "CASE WHEN c.column_key = 'PRI' THEN 1 ELSE 0 END, CASE WHEN c.is_nullable = 'NO' THEN 1 ELSE 0 END, c.column_comment "
            "FROM information_schema.columns c JOIN information_schema.tables t ON t.table_schema = c.table_schema AND t.table_name = c.table_name "
            "WHERE %s%s ORDER BY c.table_schema, c.table_name, c.ordinal_position", sys, fm); return true; }
        if (k == 1) { *kind = SXQ_TCOMMENT; snprintf(out, n,
            "SELECT c.table_schema, c.table_name, c.table_comment FROM information_schema.tables c WHERE c.table_comment <> '' AND %s%s", sys, fm); return true; }
        if (k == 2) { *kind = SXQ_FKS; snprintf(out, n,
            "SELECT c.table_schema, c.table_name, GROUP_CONCAT(c.column_name ORDER BY c.ordinal_position), c.referenced_table_schema, c.referenced_table_name "
            "FROM information_schema.key_column_usage c WHERE c.referenced_table_name IS NOT NULL AND %s%s "
            "GROUP BY c.table_schema, c.table_name, c.constraint_name, c.referenced_table_schema, c.referenced_table_name", sys, fm); return true; }
        return false;
    }
    if (!strcmp(d, "mssql")) {
        char fs[4096]; in_list("s.name", schemas, ns, false, fs, sizeof fs);
        if (k == 0) { *kind = SXQ_COLS; snprintf(out, n,
            "SELECT s.name, o.name, CASE WHEN o.type = 'V' THEN 1 ELSE 0 END, c.name, "
            "TYPE_NAME(c.user_type_id) + CASE WHEN TYPE_NAME(c.user_type_id) IN ('varchar','char','varbinary','binary') THEN '(' + CASE WHEN c.max_length = -1 THEN 'max' ELSE CAST(c.max_length AS varchar(10)) END + ')' "
            "WHEN TYPE_NAME(c.user_type_id) IN ('nvarchar','nchar') THEN '(' + CASE WHEN c.max_length = -1 THEN 'max' ELSE CAST(c.max_length / 2 AS varchar(10)) END + ')' "
            "WHEN TYPE_NAME(c.user_type_id) IN ('decimal','numeric') THEN '(' + CAST(c.precision AS varchar(5)) + ',' + CAST(c.scale AS varchar(5)) + ')' ELSE '' END, "
            "CASE WHEN EXISTS (SELECT 1 FROM sys.indexes i JOIN sys.index_columns ic ON ic.object_id = i.object_id AND ic.index_id = i.index_id WHERE i.object_id = o.object_id AND i.is_primary_key = 1 AND ic.column_id = c.column_id) THEN 1 ELSE 0 END, "
            "CASE WHEN c.is_nullable = 0 THEN 1 ELSE 0 END, CAST(ep.value AS nvarchar(400)) "
            "FROM sys.objects o JOIN sys.schemas s ON s.schema_id = o.schema_id JOIN sys.columns c ON c.object_id = o.object_id "
            "LEFT JOIN sys.extended_properties ep ON ep.major_id = o.object_id AND ep.minor_id = c.column_id AND ep.class = 1 AND ep.name = 'MS_Description' "
            "WHERE o.type IN ('U','V') AND o.is_ms_shipped = 0%s ORDER BY s.name, o.name, c.column_id", fs); return true; }
        if (k == 1) { *kind = SXQ_TCOMMENT; snprintf(out, n,
            "SELECT s.name, o.name, CAST(ep.value AS nvarchar(400)) FROM sys.extended_properties ep JOIN sys.objects o ON o.object_id = ep.major_id "
            "JOIN sys.schemas s ON s.schema_id = o.schema_id WHERE ep.class = 1 AND ep.minor_id = 0 AND ep.name = 'MS_Description'%s", fs); return true; }
        if (k == 2) { *kind = SXQ_FKS; snprintf(out, n,
            "SELECT s.name, o.name, STRING_AGG(c.name, ',') WITHIN GROUP (ORDER BY fc.constraint_column_id), rs.name, ro.name "
            "FROM sys.foreign_keys fk JOIN sys.foreign_key_columns fc ON fc.constraint_object_id = fk.object_id "
            "JOIN sys.objects o ON o.object_id = fk.parent_object_id JOIN sys.schemas s ON s.schema_id = o.schema_id "
            "JOIN sys.columns c ON c.object_id = fc.parent_object_id AND c.column_id = fc.parent_column_id "
            "JOIN sys.objects ro ON ro.object_id = fk.referenced_object_id JOIN sys.schemas rs ON rs.schema_id = ro.schema_id "
            "WHERE 1 = 1%s GROUP BY fk.object_id, s.name, o.name, rs.name, ro.name", fs); return true; }
        return false;
    }
    return false;
}
