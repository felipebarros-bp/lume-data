/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Report themes (design, phase 1): the look of a report as data, shared by many reports.
 *
 *   A theme lives in its own file (.lrtheme, JSON) next to its assets (the logo), so a team keeps
 *   one look: reports reference it ("file:themes/brand.lrtheme", relative to the report), the
 *   workspace has a default one ("workspace" = <workspace>/theme.lrtheme, the Brand Kit), and the
 *   built-in ones ("builtin:light|dark|executive") are always there. A report also keeps a snapshot
 *   of the theme it last used, so it renders the same where the file is missing, and may keep local
 *   overrides, which a locked theme ignores (the brand stays the brand).
 *
 * Pure C, no UI: parse / write / validate (WCAG contrast) / apply overrides.
 */
#include <stdbool.h>
#include <stddef.h>
#include <stdint.h>

enum { RCS_FLAT, RCS_SHADOW, RCS_BORDER, RCS_ACCENT_TOP, RCS_N };    /* card styles */
enum { RKS_PLAIN, RKS_ACCENT_BAR, RKS_TINTED, RKS_N };              /* KPI styles */
enum { RLP_LEFT, RLP_RIGHT };                                        /* logo position in the header */

typedef struct RTheme {
    int format_version;
    char id[48], name[80];
    bool locked;                  /* overrides in reports are ignored */
    /* colours, 0xRRGGBB */
    uint32_t bg, surface, border, text, muted, grid, accent, header_bg, header_text, pos, neg, sel;
    uint32_t series[8]; int nseries;
    /* typography (points) */
    float title_size, kpi_size, body_size;
    /* shape */
    float radius;                 /* card corner radius, points (0..24) */
    int shadow;                   /* 0..3 */
    int card_style, kpi_style;    /* RCS_*, RKS_* */
    /* header band at the top of every page */
    bool header;                  /* shown */
    float header_h;               /* points (48..160) */
    char logo[256];               /* image file relative to the theme file ("" = none) */
    int logo_pos;                 /* RLP_* */
    float logo_h;                 /* points (16..120) */
} RTheme;

const char *rtheme_card_style_name(int s);
const char *rtheme_kpi_style_name(int s);
void rtheme_builtin(RTheme *t, const char *name);           /* "light" (default), "dark", "executive" */
bool rtheme_is_builtin(const char *name);
/* parses a .lrtheme (or a snapshot object); unknown keys are ignored, bad values refused with why */
bool rtheme_parse(RTheme *t, const char *json, size_t len, char *why, int n);
char *rtheme_to_json(const RTheme *t);                      /* malloc'd */
/* applies an overrides object (same shape as a theme, any subset); a locked theme takes none.
 * false with why on a bad value (nothing applied) */
bool rtheme_apply_overrides(RTheme *t, const char *json, size_t len, char *why, int n);
/* WCAG 2 contrast ratio of two colours (1..21) */
double rtheme_contrast(uint32_t a, uint32_t b);
/* readable: text on background / surface, header text on header, at least 4.5:1 (AA) */
bool rtheme_check_contrast(const RTheme *t, char *why, int n);
bool rtheme_load_file(RTheme *t, const char *path, char *why, int n);
bool rtheme_save_file(const RTheme *t, const char *path, char *why, int n);   /* atomic */
char *rtheme_diff_json(const RTheme *a, const RTheme *b);   /* overrides that turn a into b (malloc'd), NULL when equal */
