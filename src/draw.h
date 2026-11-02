/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
#include "lume.h"
/* Immediate-mode drawing primitives (draw.c). Everything is in render pixels and draws
 * straight into the renderer's current target; Colors with alpha < 255 are blended. */

/* Horizontal alignment for draw_text_fit. */
enum { ALIGN_LEFT, ALIGN_CENTER, ALIGN_RIGHT };

/* Fill a rectangle. */
void draw_rect(SDL_Renderer *r, Rect rc, Color c);
/* Rectangle border `thickness` pixels wide, drawn inside rc. */
void draw_rect_outline(SDL_Renderer *r, Rect rc, float thickness, Color c);
/* Fill a rounded rectangle (radius clamped to half the shorter side). */
void draw_rrect(SDL_Renderer *r, Rect rc, float radius, Color c);
void draw_rrect_outline(SDL_Renderer *r, Rect rc, float radius, float t, Color c);   /* border only, middle untouched */
/* Straight line of the given thickness, square ends. */
void draw_line(SDL_Renderer *r, float x1, float y1, float x2, float y2, float thickness, Color c);
/* Filled triangle pointing up or down, w x h centred on (cx, cy). */
void draw_triangle(SDL_Renderer *r, float cx, float cy, float w, float h, bool down, Color c);
/* Open chevron of `size` centred on (cx, cy): pointing down when `down`, else right. */
void draw_chevron(SDL_Renderer *r, float cx, float cy, float size, float thickness, bool down, Color c);
void draw_chevron_back(SDL_Renderer *r, float cx, float cy, float size, float thickness, bool up, Color c);   /* up, or left when !up */
/* Two diagonal strokes (a close / remove mark) of `size` centred on (cx, cy); the UI font has no U+2715. */
void draw_cross(SDL_Renderer *r, float cx, float cy, float size, float thickness, Color c);
/* Explorer icons of size `s` centred on (cx, cy). `bg` is the background colour used to cut
 * details out of the solid shape; `accent` colours the table header row. */
void draw_icon_db(SDL_Renderer *r, float cx, float cy, float s, Color c, Color bg);
void draw_icon_table(SDL_Renderer *r, float cx, float cy, float s, Color c, Color accent, Color bg);
void draw_icon_view(SDL_Renderer *r, float cx, float cy, float s, Color c, Color bg);
void draw_icon_column(SDL_Renderer *r, float cx, float cy, float s, Color c);
void draw_icon_key(SDL_Renderer *r, float cx, float cy, float s, Color c);

/* Nested clipping: clip_push intersects rc with the current clip and applies it; clip_pop
 * restores the previous one. Always pair them (global stack, 32 levels). */
void clip_push(SDL_Renderer *r, Rect rc);
void clip_pop(SDL_Renderer *r);

/* Draw a NUL-terminated string with its top-left at (x, y). */
void draw_text(Font *f, SDL_Renderer *r, const char *s, float x, float y, Color c);
/* Vertically centered in rc, truncated with an ellipsis when too wide. */
void draw_text_fit(Font *f, SDL_Renderer *r, const char *s, int len, Rect rc, Color c, int align);
