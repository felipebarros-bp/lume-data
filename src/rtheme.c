/* SPDX-License-Identifier: AGPL-3.0-only */
/* Report themes: see rtheme.h. */
#include "rtheme.h"
#include "json.h"
#include "jw.h"
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>

static const char *const CARD_NAMES[RCS_N] = { "flat", "shadow", "border", "accent-top" };
static const char *const KPI_NAMES[RKS_N] = { "plain", "accent-bar", "tinted" };
const char *rtheme_card_style_name(int s) { return s >= 0 && s < RCS_N ? CARD_NAMES[s] : "shadow"; }
const char *rtheme_kpi_style_name(int s) { return s >= 0 && s < RKS_N ? KPI_NAMES[s] : "plain"; }
static int by_name(const char *const *names, int n, const char *s) { for (int i = 0; i < n; i++) if (!strcmp(names[i], s)) return i; return -1; }

bool rtheme_is_builtin(const char *name) { return !strcmp(name, "light") || !strcmp(name, "dark") || !strcmp(name, "executive"); }
void rtheme_builtin(RTheme *t, const char *name) {
    memset(t, 0, sizeof *t);
    t->format_version = 1;
    t->title_size = 15; t->kpi_size = 30; t->body_size = 12;
    t->radius = 8; t->shadow = 1; t->card_style = RCS_SHADOW; t->kpi_style = RKS_PLAIN;
    t->header = false; t->header_h = 72; t->logo_pos = RLP_LEFT; t->logo_h = 36;
    static const uint32_t s_light[6] = { 0x0072B2, 0xD55E00, 0x009E73, 0xCC79A7, 0x56B4E9, 0xE69F00 };
    static const uint32_t s_dark[6] = { 0x56B4E9, 0xE69F00, 0x3DC49A, 0xE38FC0, 0x8FB8FF, 0xF0C75E };
    static const uint32_t s_exec[6] = { 0x1F4E79, 0xB8860B, 0x5B8DB8, 0x7A5C12, 0x2E8B57, 0x8B3A62 };
    const uint32_t *ser = s_light;
    if (name && !strcmp(name, "dark")) {
        snprintf(t->id, sizeof t->id, "dark"); snprintf(t->name, sizeof t->name, "Dark");
        t->bg = 0x15171B; t->surface = 0x1F2228; t->border = 0x30353D; t->text = 0xE6E8EB; t->muted = 0xA1A9B5; t->grid = 0x2C3038;
        t->accent = 0x5B8DEF; t->header_bg = 0x1F2228; t->header_text = 0xE6E8EB; t->pos = 0x4AC26B; t->neg = 0xF27070; t->sel = 0x5B8DEF; ser = s_dark;
    } else if (name && !strcmp(name, "executive")) {
        snprintf(t->id, sizeof t->id, "executive"); snprintf(t->name, sizeof t->name, "Executive");
        t->bg = 0xEDF0F5; t->surface = 0xFFFFFF; t->border = 0xCBD3DE; t->text = 0x0B1F3A; t->muted = 0x4A5B73; t->grid = 0xE3E8EF;
        t->accent = 0x1F4E79; t->header_bg = 0x0B1F3A; t->header_text = 0xFFFFFF; t->pos = 0x1A7F37; t->neg = 0xB42318; t->sel = 0xC9A227; ser = s_exec;
        t->card_style = RCS_ACCENT_TOP;
    } else {
        snprintf(t->id, sizeof t->id, "light"); snprintf(t->name, sizeof t->name, "Light");
        t->bg = 0xF3F4F6; t->surface = 0xFFFFFF; t->border = 0xD9DDE3; t->text = 0x1F2328; t->muted = 0x59636E; t->grid = 0xE6E9EE;
        t->accent = 0x1F6FEB; t->header_bg = 0xFFFFFF; t->header_text = 0x1F2328; t->pos = 0x1A7F37; t->neg = 0xCF222E; t->sel = 0x1F6FEB;
    }
    for (int i = 0; i < 6; i++) t->series[i] = ser[i];
    t->nseries = 6;
}

/* ---- colours ---- */
static bool parse_hex(const char *s, uint32_t *out) {
    if (s[0] != '#') return false;
    size_t L = strlen(s);
    if (L != 7 && L != 4) return false;
    unsigned v = 0;
    for (size_t i = 1; i < L; i++) { char c = s[i]; int d = c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; if (d < 0) return false; v = v * 16 + (unsigned)d; }
    if (L == 4) v = ((v >> 8) & 0xF) * 0x110000 + ((v >> 4) & 0xF) * 0x1100 + (v & 0xF) * 0x11;
    *out = v; return true;
}
static double lin(double c) { c /= 255.0; return c <= 0.03928 ? c / 12.92 : pow((c + 0.055) / 1.055, 2.4); }
static double lum(uint32_t c) { return 0.2126 * lin((c >> 16) & 255) + 0.7152 * lin((c >> 8) & 255) + 0.0722 * lin(c & 255); }
double rtheme_contrast(uint32_t a, uint32_t b) { double la = lum(a), lb = lum(b); if (la < lb) { double t = la; la = lb; lb = t; } return (la + 0.05) / (lb + 0.05); }
bool rtheme_check_contrast(const RTheme *t, char *why, int n) {
    struct { uint32_t f, b; const char *what; } pairs[] = {
        { t->text, t->bg, "text on the page background" }, { t->text, t->surface, "text on the cards" },
        { t->header_text, t->header_bg, "header text on the header" },
    };
    for (int i = 0; i < (int)(sizeof pairs / sizeof pairs[0]); i++) {
        if (i == 2 && !t->header) continue;
        double c = rtheme_contrast(pairs[i].f, pairs[i].b);
        if (c < 4.5) { snprintf(why, (size_t)n, "%s is hard to read (contrast %.1f:1, needs 4.5:1)", pairs[i].what, c); return false; }
    }
    return true;
}

/* ---- JSON ---- */
static const char *const COLOR_KEYS[] = { "background", "surface", "border", "text", "muted", "grid", "accent", "headerBackground", "headerText", "positive", "negative", "selection", NULL };
static uint32_t *color_slot(RTheme *t, int i) {
    uint32_t *s[] = { &t->bg, &t->surface, &t->border, &t->text, &t->muted, &t->grid, &t->accent, &t->header_bg, &t->header_text, &t->pos, &t->neg, &t->sel };
    return s[i];
}
/* reads the theme fields present in object o (a theme or an overrides object); false with why */
static bool read_fields(RTheme *t, const Json *j, int o, bool whole, char *why, int n) {
    char s[300];
    if (whole) {
        jr_str(j, o, "id", t->id, sizeof t->id); jr_str(j, o, "name", t->name, sizeof t->name);
        bool lk = false; if (jr_bool(j, o, "locked", &lk)) t->locked = lk;
    }
    int c = jr_get(j, o, "colors");
    if (c >= 0) {
        for (int i = 0; COLOR_KEYS[i]; i++) {
            if (!jr_str(j, c, COLOR_KEYS[i], s, sizeof s)) continue;
            uint32_t v; if (!parse_hex(s, &v)) { snprintf(why, (size_t)n, "colors.%s: '%.20s' is not a #RRGGBB colour", COLOR_KEYS[i], s); return false; }
            *color_slot(t, i) = v;
        }
        int se = jr_get(j, c, "series");
        if (se >= 0) {
            int k = 0;
            for (int i = 0; k < 8; i++) { int e = jr_child(j, se, i); if (e < 0) break; jr_node_str(j, e, s, sizeof s); uint32_t v; if (!parse_hex(s, &v)) { snprintf(why, (size_t)n, "colors.series[%d] is not a #RRGGBB colour", i); return false; } t->series[k++] = v; }
            if (k) t->nseries = k;
        }
    }
    double d;
    int ty = jr_get(j, o, "typography");
    if (ty >= 0) {
        if (jr_num(j, ty, "titleSize", &d)) { if (d < 9 || d > 40) { snprintf(why, (size_t)n, "typography.titleSize must be 9..40"); return false; } t->title_size = (float)d; }
        if (jr_num(j, ty, "kpiSize", &d)) { if (d < 14 || d > 72) { snprintf(why, (size_t)n, "typography.kpiSize must be 14..72"); return false; } t->kpi_size = (float)d; }
        if (jr_num(j, ty, "bodySize", &d)) { if (d < 9 || d > 20) { snprintf(why, (size_t)n, "typography.bodySize must be 9..20"); return false; } t->body_size = (float)d; }
    }
    int sh = jr_get(j, o, "shape");
    if (sh >= 0) {
        if (jr_num(j, sh, "radius", &d)) { if (d < 0 || d > 24) { snprintf(why, (size_t)n, "shape.radius must be 0..24"); return false; } t->radius = (float)d; }
        long long iv; if (jr_int(j, sh, "shadow", &iv)) { if (iv < 0 || iv > 3) { snprintf(why, (size_t)n, "shape.shadow must be 0..3"); return false; } t->shadow = (int)iv; }
        if (jr_str(j, sh, "cardStyle", s, sizeof s)) { int k = by_name(CARD_NAMES, RCS_N, s); if (k < 0) { snprintf(why, (size_t)n, "shape.cardStyle: flat, shadow, border or accent-top"); return false; } t->card_style = k; }
        if (jr_str(j, sh, "kpiStyle", s, sizeof s)) { int k = by_name(KPI_NAMES, RKS_N, s); if (k < 0) { snprintf(why, (size_t)n, "shape.kpiStyle: plain, accent-bar or tinted"); return false; } t->kpi_style = k; }
    }
    int h = jr_get(j, o, "header");
    if (h >= 0) {
        bool b; if (jr_bool(j, h, "show", &b)) t->header = b;
        if (jr_num(j, h, "height", &d)) { if (d < 48 || d > 160) { snprintf(why, (size_t)n, "header.height must be 48..160"); return false; } t->header_h = (float)d; }
        if (jr_str(j, h, "logo", s, sizeof s)) {
            if (strstr(s, "..") || s[0] == '/' || strchr(s, '\\')) { snprintf(why, (size_t)n, "header.logo must be a file next to the theme (no path)"); return false; }
            snprintf(t->logo, sizeof t->logo, "%s", s);
        }
        if (jr_str(j, h, "logoPosition", s, sizeof s)) { if (!strcmp(s, "left")) t->logo_pos = RLP_LEFT; else if (!strcmp(s, "right")) t->logo_pos = RLP_RIGHT; else { snprintf(why, (size_t)n, "header.logoPosition: left or right"); return false; } }
        if (jr_num(j, h, "logoHeight", &d)) { if (d < 16 || d > 120) { snprintf(why, (size_t)n, "header.logoHeight must be 16..120"); return false; } t->logo_h = (float)d; }
    }
    return true;
}
bool rtheme_parse(RTheme *t, const char *json, size_t len, char *why, int n) {
    rtheme_builtin(t, "light");   /* missing fields keep the light defaults */
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, json, len) || j.nodes[0].type != JN_OBJECT) { snprintf(why, (size_t)n, "the theme is not a JSON object"); json_free(&j); return false; }
    long long fv = 1; jr_int(&j, 0, "formatVersion", &fv);
    if (fv > 1) { snprintf(why, (size_t)n, "this theme was made by a newer Lume (format %lld)", fv); json_free(&j); return false; }
    bool ok = read_fields(t, &j, 0, true, why, n);
    json_free(&j);
    if (ok && !t->id[0]) snprintf(t->id, sizeof t->id, "custom");
    return ok;
}
static void hexs(char *o, uint32_t c) { snprintf(o, 8, "#%06X", c & 0xFFFFFF); }
char *rtheme_to_json(const RTheme *t) {
    Jw j; jw_init(&j); jw_obj(&j);
    jw_kint(&j, "formatVersion", 1); jw_kstr(&j, "id", t->id); jw_kstr(&j, "name", t->name);
    if (t->locked) jw_kbool(&j, "locked", true);
    char h[8];
    jw_key(&j, "colors"); jw_obj(&j);
    for (int i = 0; COLOR_KEYS[i]; i++) { hexs(h, *color_slot((RTheme *)t, i)); jw_kstr(&j, COLOR_KEYS[i], h); }
    jw_key(&j, "series"); jw_arr(&j); for (int i = 0; i < t->nseries; i++) { hexs(h, t->series[i]); jw_str(&j, h); } jw_end_arr(&j);
    jw_end_obj(&j);
    jw_key(&j, "typography"); jw_obj(&j); jw_knum(&j, "titleSize", t->title_size); jw_knum(&j, "kpiSize", t->kpi_size); jw_knum(&j, "bodySize", t->body_size); jw_end_obj(&j);
    jw_key(&j, "shape"); jw_obj(&j); jw_knum(&j, "radius", t->radius); jw_kint(&j, "shadow", t->shadow);
    jw_kstr(&j, "cardStyle", rtheme_card_style_name(t->card_style)); jw_kstr(&j, "kpiStyle", rtheme_kpi_style_name(t->kpi_style)); jw_end_obj(&j);
    jw_key(&j, "header"); jw_obj(&j); jw_kbool(&j, "show", t->header); jw_knum(&j, "height", t->header_h);
    if (t->logo[0]) jw_kstr(&j, "logo", t->logo);
    jw_kstr(&j, "logoPosition", t->logo_pos == RLP_RIGHT ? "right" : "left"); jw_knum(&j, "logoHeight", t->logo_h); jw_end_obj(&j);
    jw_end_obj(&j);
    return jw_take(&j);
}
bool rtheme_apply_overrides(RTheme *t, const char *json, size_t len, char *why, int n) {
    if (!json || !len) return true;
    if (t->locked) return true;   /* a locked theme is the brand: reports do not change it */
    Json j; memset(&j, 0, sizeof j);
    if (!json_parse(&j, json, len) || j.nodes[0].type != JN_OBJECT) { snprintf(why, (size_t)n, "the theme overrides are not a JSON object"); json_free(&j); return false; }
    RTheme cp = *t;
    bool ok = read_fields(&cp, &j, 0, false, why, n);
    json_free(&j);
    if (ok) *t = cp;
    return ok;
}
bool rtheme_load_file(RTheme *t, const char *path, char *why, int n) {
    FILE *f = fopen(path, "rb");
    if (!f) { snprintf(why, (size_t)n, "theme file not found"); return false; }
    fseek(f, 0, SEEK_END); long L = ftell(f); fseek(f, 0, SEEK_SET);
    if (L <= 0 || L > 256 * 1024) { fclose(f); snprintf(why, (size_t)n, "the theme file is empty or too large"); return false; }
    char *b = malloc((size_t)L + 1); size_t got = fread(b, 1, (size_t)L, f); fclose(f); b[got] = 0;
    bool ok = rtheme_parse(t, b, got, why, n);
    free(b);
    return ok;
}
bool rtheme_save_file(const RTheme *t, const char *path, char *why, int n) {
    char *s = rtheme_to_json(t);
    char tmp[1200]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(s); snprintf(why, (size_t)n, "cannot write the theme there"); return false; }
    size_t L = strlen(s); bool ok = fwrite(s, 1, L, f) == L; ok = (fclose(f) == 0) && ok; free(s);
    if (!ok || rename(tmp, path) != 0) { unlink(tmp); snprintf(why, (size_t)n, "the theme could not be saved"); return false; }
    return true;
}

/* only what b changes from a (an overrides object), NULL when nothing differs */
char *rtheme_diff_json(const RTheme *a, const RTheme *b) {
    Jw j; jw_init(&j); jw_obj(&j);
    char h[8]; bool any = false, grp;
    grp = false;
    for (int i = 0; COLOR_KEYS[i]; i++) {
        if (*color_slot((RTheme *)a, i) == *color_slot((RTheme *)b, i)) continue;
        if (!grp) { jw_key(&j, "colors"); jw_obj(&j); grp = true; }
        hexs(h, *color_slot((RTheme *)b, i)); jw_kstr(&j, COLOR_KEYS[i], h);
    }
    if (a->nseries != b->nseries || memcmp(a->series, b->series, sizeof a->series)) {
        if (!grp) { jw_key(&j, "colors"); jw_obj(&j); grp = true; }
        jw_key(&j, "series"); jw_arr(&j); for (int i = 0; i < b->nseries; i++) { hexs(h, b->series[i]); jw_str(&j, h); } jw_end_arr(&j);
    }
    if (grp) { jw_end_obj(&j); any = true; }
    if (a->title_size != b->title_size || a->kpi_size != b->kpi_size || a->body_size != b->body_size) {
        jw_key(&j, "typography"); jw_obj(&j);
        if (a->title_size != b->title_size) jw_knum(&j, "titleSize", b->title_size);
        if (a->kpi_size != b->kpi_size) jw_knum(&j, "kpiSize", b->kpi_size);
        if (a->body_size != b->body_size) jw_knum(&j, "bodySize", b->body_size);
        jw_end_obj(&j); any = true;
    }
    if (a->radius != b->radius || a->shadow != b->shadow || a->card_style != b->card_style || a->kpi_style != b->kpi_style) {
        jw_key(&j, "shape"); jw_obj(&j);
        if (a->radius != b->radius) jw_knum(&j, "radius", b->radius);
        if (a->shadow != b->shadow) jw_kint(&j, "shadow", b->shadow);
        if (a->card_style != b->card_style) jw_kstr(&j, "cardStyle", rtheme_card_style_name(b->card_style));
        if (a->kpi_style != b->kpi_style) jw_kstr(&j, "kpiStyle", rtheme_kpi_style_name(b->kpi_style));
        jw_end_obj(&j); any = true;
    }
    if (a->header != b->header || a->header_h != b->header_h || strcmp(a->logo, b->logo) || a->logo_pos != b->logo_pos || a->logo_h != b->logo_h) {
        jw_key(&j, "header"); jw_obj(&j);
        if (a->header != b->header) jw_kbool(&j, "show", b->header);
        if (a->header_h != b->header_h) jw_knum(&j, "height", b->header_h);
        if (strcmp(a->logo, b->logo)) jw_kstr(&j, "logo", b->logo);
        if (a->logo_pos != b->logo_pos) jw_kstr(&j, "logoPosition", b->logo_pos == RLP_RIGHT ? "right" : "left");
        if (a->logo_h != b->logo_h) jw_knum(&j, "logoHeight", b->logo_h);
        jw_end_obj(&j); any = true;
    }
    jw_end_obj(&j);
    char *out = jw_take(&j);
    if (!any) { free(out); return NULL; }
    return out;
}
