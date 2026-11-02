/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
/* TrueType fonts rasterised into a texture atlas (font.c). All sizes and positions are in
 * render pixels. Strings are UTF-8; lengths are in bytes. */

/* Load a font file at `size_px` render pixels. Returns NULL on failure; free with font_destroy(). */
Font *font_load(SDL_Renderer *r, const char *path, float size_px);
/* Free a font and its atlas (NULL is fine). */
void font_destroy(Font *f);
float font_height(const Font *f);   /* line height in pixels */
float font_ascent(const Font *f);  /* distance from the top of a line to the baseline, pixels */
float font_size(const Font *f);    /* pixel size the font was loaded at */
/* Width in pixels of `len` bytes of s; tabs count as 4 spaces, control characters as nothing. */
float font_width(Font *f, const char *s, int len);   /* len < 0 → strlen */
/* Draw `len` bytes of s (len < 0: whole string) with its top-left at (x, y); returns the pen x
 * after the last glyph. */
float font_draw(Font *f, SDL_Renderer *r, const char *s, int len, float x, float y, Color c);

/* UTF-8 helpers on byte indices. utf8_next decodes the code point at byte i into *cp. */
int utf8_next(const char *s, int len, int i, uint32_t *cp);  /* returns index after codepoint */
int utf8_prev(const char *s, int i);                          /* index of previous codepoint start */
