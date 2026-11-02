/* SPDX-License-Identifier: AGPL-3.0-only */
/* JSON writer and typed readers: see jw.h. */
#include "jw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

void jw_init(Jw *j) { memset(j, 0, sizeof *j); }
void jw_free(Jw *j) { free(j->b); memset(j, 0, sizeof *j); }
char *jw_take(Jw *j) {
    char *s = j->b; if (!s) s = strdup("");
    memset(j, 0, sizeof *j);
    return s;
}
static void put(Jw *j, const char *s, size_t n) {
    if (j->oom) return;
    if (j->w + n + 1 > j->cap) {
        size_t c = j->cap ? j->cap * 2 : 1024; while (c < j->w + n + 1) c *= 2;
        char *nb = realloc(j->b, c); if (!nb) { j->oom = true; return; }
        j->b = nb; j->cap = c;
    }
    memcpy(j->b + j->w, s, n); j->w += n; j->b[j->w] = 0;
}
static void puts_(Jw *j, const char *s) { put(j, s, strlen(s)); }
/* a value is about to be written: separator when needed */
static void sep(Jw *j) {
    if (j->depth > 0 && j->depth < 64) { if (j->comma[j->depth]) put(j, ",", 1); j->comma[j->depth] = true; }
}
void jw_obj(Jw *j) { if (!j->ak) sep(j); j->ak = false; put(j, "{", 1); if (j->depth < 63) j->depth++; j->comma[j->depth] = false; }
void jw_end_obj(Jw *j) { put(j, "}", 1); if (j->depth > 0) j->depth--; }
void jw_arr(Jw *j) { if (!j->ak) sep(j); j->ak = false; put(j, "[", 1); if (j->depth < 63) j->depth++; j->comma[j->depth] = false; }
void jw_end_arr(Jw *j) { put(j, "]", 1); if (j->depth > 0) j->depth--; }
static void esc(Jw *j, const char *s) {
    put(j, "\"", 1);
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) {
        char u[8];
        switch (*p) {
        case '"': puts_(j, "\\\""); break;
        case '\\': puts_(j, "\\\\"); break;
        case '\n': puts_(j, "\\n"); break;
        case '\r': puts_(j, "\\r"); break;
        case '\t': puts_(j, "\\t"); break;
        default:
            if (*p < 0x20) { snprintf(u, sizeof u, "\\u%04x", *p); puts_(j, u); }
            else put(j, (const char *)p, 1);
        }
    }
    put(j, "\"", 1);
}
void jw_key(Jw *j, const char *k) { sep(j); esc(j, k); put(j, ":", 1); j->ak = true; }
static void val_begin(Jw *j) { if (!j->ak) sep(j); j->ak = false; }
void jw_str(Jw *j, const char *s) { val_begin(j); if (!s) puts_(j, "null"); else esc(j, s); }
void jw_int(Jw *j, long long v) { val_begin(j); char b[32]; snprintf(b, sizeof b, "%lld", v); puts_(j, b); }
void jw_num(Jw *j, double v) {
    val_begin(j);
    if (!isfinite(v)) { puts_(j, "null"); return; }
    char b[40];
    for (int p = 6; p <= 17; p++) { snprintf(b, sizeof b, "%.*g", p, v); if (strtod(b, NULL) == v) break; }
    puts_(j, b);
}
void jw_bool(Jw *j, bool v) { val_begin(j); puts_(j, v ? "true" : "false"); }
void jw_null(Jw *j) { val_begin(j); puts_(j, "null"); }
void jw_raw(Jw *j, const char *s) { val_begin(j); puts_(j, s); }
void jw_kstr(Jw *j, const char *k, const char *s) { jw_key(j, k); jw_str(j, s); }
void jw_kint(Jw *j, const char *k, long long v) { jw_key(j, k); jw_int(j, v); }
void jw_knum(Jw *j, const char *k, double v) { jw_key(j, k); jw_num(j, v); }
void jw_kbool(Jw *j, const char *k, bool v) { jw_key(j, k); jw_bool(j, v); }

/* ---- reading ---- */

int jr_get(const Json *j, int obj, const char *key) {
    if (obj < 0 || obj >= j->n || j->nodes[obj].type != JN_OBJECT) return -1;
    size_t kl = strlen(key);
    for (int c = j->nodes[obj].first_child; c >= 0; c = j->nodes[c].next_sibling) {
        const JsonNode *x = &j->nodes[c];
        if (x->key_len == kl && !memcmp(j->src + x->key_off, key, kl)) return c;
    }
    return -1;
}
int jr_child(const Json *j, int arr, int i) {
    if (arr < 0 || arr >= j->n || (j->nodes[arr].type != JN_ARRAY && j->nodes[arr].type != JN_OBJECT)) return -1;
    int k = 0;
    for (int c = j->nodes[arr].first_child; c >= 0; c = j->nodes[c].next_sibling, k++) if (k == i) return c;
    return -1;
}
/* json_unescape keeps an 8-byte margin for its ellipsis, which would cut short exact fields ("=",
 * "BRL"): decode into a buffer of the right size, then copy whole UTF-8 characters that fit. */
static void decode_exact(const char *raw, size_t len, char *out, size_t n) {
    size_t cap = len * 4 + 16;
    char *tmp = malloc(cap); if (!tmp) { out[0] = 0; return; }
    json_unescape(raw, len, tmp, cap);
    size_t L = strlen(tmp);
    if (L >= n) { L = n - 1; while (L > 0 && ((unsigned char)tmp[L] & 0xC0) == 0x80) L--; }
    memcpy(out, tmp, L); out[L] = 0;
    free(tmp);
}
void jr_node_str(const Json *j, int node, char *out, size_t n) {
    if (!n) return;
    out[0] = 0;
    if (node < 0 || node >= j->n || j->nodes[node].type != JN_STRING) return;
    decode_exact(j->src + j->nodes[node].val_off, j->nodes[node].val_len, out, n);
}
bool jr_str(const Json *j, int obj, const char *key, char *out, size_t n) {
    int c = jr_get(j, obj, key);
    if (n) out[0] = 0;
    if (c < 0 || j->nodes[c].type != JN_STRING) return false;
    jr_node_str(j, c, out, n);
    return true;
}
char *jr_strdup(const Json *j, int obj, const char *key) {
    int c = jr_get(j, obj, key);
    if (c < 0 || j->nodes[c].type != JN_STRING) return NULL;
    size_t n = j->nodes[c].val_len * 4 + 16;
    char *s = malloc(n); if (!s) return NULL;
    decode_exact(j->src + j->nodes[c].val_off, j->nodes[c].val_len, s, n);
    return s;
}
bool jr_num(const Json *j, int obj, const char *key, double *out) {
    int c = jr_get(j, obj, key);
    if (c < 0 || j->nodes[c].type != JN_NUMBER) return false;
    char b[64]; size_t L = j->nodes[c].val_len < 63 ? j->nodes[c].val_len : 63;
    memcpy(b, j->src + j->nodes[c].val_off, L); b[L] = 0;
    *out = strtod(b, NULL);
    return true;
}
bool jr_int(const Json *j, int obj, const char *key, long long *out) {
    double d; if (!jr_num(j, obj, key, &d)) return false;
    if (d != floor(d) || fabs(d) > 9e15) return false;
    *out = (long long)d; return true;
}
bool jr_bool(const Json *j, int obj, const char *key, bool *out) {
    int c = jr_get(j, obj, key);
    if (c < 0) return false;
    if (j->nodes[c].type == JN_TRUE) { *out = true; return true; }
    if (j->nodes[c].type == JN_FALSE) { *out = false; return true; }
    return false;
}
bool jr_keys_allowed(const Json *j, int obj, const char *const *allowed, char *bad, size_t bn) {
    if (bn) bad[0] = 0;
    if (obj < 0 || obj >= j->n || j->nodes[obj].type != JN_OBJECT) return false;
    for (int c = j->nodes[obj].first_child; c >= 0; c = j->nodes[c].next_sibling) {
        const JsonNode *x = &j->nodes[c];
        bool ok = false;
        for (int i = 0; allowed[i]; i++) if (strlen(allowed[i]) == x->key_len && !memcmp(j->src + x->key_off, allowed[i], x->key_len)) { ok = true; break; }
        if (!ok) { if (bn) decode_exact(j->src + x->key_off, x->key_len, bad, bn); return false; }
    }
    return true;
}
bool jr_find_object(const char *text, size_t *off, size_t *len) {
    const char *p = strchr(text, '{');
    while (p) {
        int depth = 0; bool str = false, escp = false; const char *q = p;
        for (; *q; q++) {
            if (str) { if (escp) escp = false; else if (*q == '\\') escp = true; else if (*q == '"') str = false; continue; }
            if (*q == '"') str = true;
            else if (*q == '{') depth++;
            else if (*q == '}') { if (--depth == 0) { *off = (size_t)(p - text); *len = (size_t)(q - p + 1); return true; } }
        }
        p = strchr(p + 1, '{');
    }
    return false;
}
