/* SPDX-License-Identifier: AGPL-3.0-only */
/* draw.c: immediate-mode 2D drawing primitives on top of SDL_Renderer.
 *
 * Every function draws straight into the current render target, in render pixels (callers
 * convert UI points with px()). Shapes that SDL has no primitive for (rounded rectangles,
 * thick lines, triangles) are tessellated into SDL_RenderGeometry triangles. Icons used by the
 * Explorer are composed from those shapes. Text goes through font.c.
 *
 * Clipping is a small global stack (clip_push / clip_pop) so nested widgets can restrict
 * drawing to the intersection of their ancestors' rectangles. That stack is global state:
 * draw only from the main (render) thread, and always pair every push with a pop. */
#include "draw.h"
#include "font.h"

/* Convert an 8-bit Color into SDL's float color (0..1 per channel). */
static SDL_FColor fcolor(Color c) {
    SDL_FColor f = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    return f;
}

/* Fill an axis-aligned rectangle. Opaque colors disable blending (cheaper); translucent ones blend. */
void draw_rect(SDL_Renderer *r, Rect rc, Color c) {
    SDL_FRect fr = { rc.x, rc.y, rc.w, rc.h };
    SDL_SetRenderDrawBlendMode(r, c.a < 255 ? SDL_BLENDMODE_BLEND : SDL_BLENDMODE_NONE);
    SDL_SetRenderDrawColor(r, c.r, c.g, c.b, c.a);
    SDL_RenderFillRect(r, &fr);
}

/* Draw a rectangle border `t` pixels thick, inside `rc`, as four non-overlapping filled strips. */
void draw_rect_outline(SDL_Renderer *r, Rect rc, float t, Color c) {
    draw_rect(r, rect_make(rc.x, rc.y, rc.w, t), c);
    draw_rect(r, rect_make(rc.x, rc.y + rc.h - t, rc.w, t), c);
    draw_rect(r, rect_make(rc.x, rc.y + t, t, rc.h - 2 * t), c);
    draw_rect(r, rect_make(rc.x + rc.w - t, rc.y + t, t, rc.h - 2 * t), c);
}

/* A rounded rectangle's border, `t` pixels thick, inside `rc`; the middle is left untouched (ghost buttons on any
 * background). Drawn as a ring of quads following the same corner arcs as draw_rrect. */
void draw_rrect_outline(SDL_Renderer *r, Rect rc, float radius, float t, Color c) {
    float rad = fminf(radius, fminf(rc.w, rc.h) / 2);
    if (rad < 0.75f) { draw_rect_outline(r, rc, t, c); return; }
    int seg = (int)clampf(rad / 1.5f, 3, 12);
    enum { MAXP = 4 * 13 };
    SDL_Vertex v[MAXP * 2]; int idx[MAXP * 6];
    SDL_FColor fc = fcolor(c);
    float ri = fmaxf(0, rad - t);
    const float cx[4] = { rc.x + rc.w - rad, rc.x + rc.w - rad, rc.x + rad, rc.x + rad };
    const float cy[4] = { rc.y + rad, rc.y + rc.h - rad, rc.y + rc.h - rad, rc.y + rad };
    int np = 0;
    for (int corner = 0; corner < 4; corner++) {
        float a0 = (corner - 1) * (float)M_PI / 2;
        for (int s2 = 0; s2 <= seg; s2++, np++) {
            float an = a0 + (float)M_PI / 2 * s2 / seg, ca = cosf(an), sa = sinf(an);
            v[np * 2].position.x = cx[corner] + ca * rad; v[np * 2].position.y = cy[corner] + sa * rad;
            v[np * 2 + 1].position.x = cx[corner] + ca * ri; v[np * 2 + 1].position.y = cy[corner] + sa * ri;
            for (int k = 0; k < 2; k++) { v[np * 2 + k].color = fc; v[np * 2 + k].tex_coord.x = v[np * 2 + k].tex_coord.y = 0; }
        }
    }
    int ni = 0;
    for (int i = 0; i < np; i++) {
        int j = (i + 1) % np;
        idx[ni++] = i * 2; idx[ni++] = i * 2 + 1; idx[ni++] = j * 2;
        idx[ni++] = i * 2 + 1; idx[ni++] = j * 2 + 1; idx[ni++] = j * 2;
    }
    SDL_RenderGeometry(r, NULL, v, np * 2, idx, ni);
}

/* Fill a rounded rectangle. The radius is clamped to half the shorter side; below ~0.75 px it
 * falls back to a plain rectangle. Drawn as a triangle fan from the centre, with 3..12 segments
 * per corner depending on the radius. */
void draw_rrect(SDL_Renderer *r, Rect rc, float radius, Color c) {
    float rad = fminf(radius, fminf(rc.w, rc.h) / 2);
    if (rad < 0.75f) { draw_rect(r, rc, c); return; }
    int seg = (int)clampf(rad / 1.5f, 3, 12);
    enum { MAXV = 4 * 13 + 1 };
    SDL_Vertex v[MAXV];
    int idx[4 * 13 * 3];
    SDL_FColor fc = fcolor(c);
    int n = 0;
    v[n].position.x = rc.x + rc.w / 2; v[n].position.y = rc.y + rc.h / 2; v[n].color = fc; n++;
    const float cx[4] = { rc.x + rc.w - rad, rc.x + rc.w - rad, rc.x + rad, rc.x + rad };
    const float cy[4] = { rc.y + rad, rc.y + rc.h - rad, rc.y + rc.h - rad, rc.y + rad };
    for (int corner = 0; corner < 4; corner++) {
        float a0 = (corner - 1) * (float)M_PI / 2;
        for (int s = 0; s <= seg; s++) {
            float a = a0 + (float)M_PI / 2 * s / seg;
            v[n].position.x = cx[corner] + cosf(a) * rad;
            v[n].position.y = cy[corner] + sinf(a) * rad;
            v[n].color = fc; v[n].tex_coord.x = v[n].tex_coord.y = 0;
            n++;
        }
    }
    int ni = 0;
    for (int i = 1; i < n; i++) {
        idx[ni++] = 0; idx[ni++] = i; idx[ni++] = (i + 1 < n) ? i + 1 : 1;
    }
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(r, NULL, v, n, idx, ni);
}

/* Draw a line segment of thickness `t` (pixels) as a quad perpendicular-offset by t/2 on each side.
 * Ends are square, not capped. Zero-length segments draw nothing. */
void draw_line(SDL_Renderer *r, float x1, float y1, float x2, float y2, float t, Color c) {
    float dx = x2 - x1, dy = y2 - y1, len = sqrtf(dx * dx + dy * dy);
    if (len < 0.001f) return;
    float nx = -dy / len * t / 2, ny = dx / len * t / 2;
    SDL_FColor fc = fcolor(c);
    SDL_Vertex v[4] = {
        { { x1 + nx, y1 + ny }, fc, { 0, 0 } }, { { x2 + nx, y2 + ny }, fc, { 0, 0 } },
        { { x2 - nx, y2 - ny }, fc, { 0, 0 } }, { { x1 - nx, y1 - ny }, fc, { 0, 0 } },
    };
    int idx[6] = { 0, 1, 2, 0, 2, 3 };
    SDL_SetRenderDrawBlendMode(r, SDL_BLENDMODE_BLEND);
    SDL_RenderGeometry(r, NULL, v, 4, idx, 6);
}

/* Filled isosceles triangle, base w by height h, centred on (cx, cy); `down` points the tip down.
 * Uses whatever blend mode the renderer currently has. */
void draw_triangle(SDL_Renderer *r, float cx, float cy, float w, float h, bool down, Color c) {
    SDL_FColor fc = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    SDL_Vertex v[3];
    float top = cy - h / 2, bot = cy + h / 2;
    v[0].position.x = cx - w / 2; v[0].position.y = down ? top : bot;
    v[1].position.x = cx + w / 2; v[1].position.y = down ? top : bot;
    v[2].position.x = cx;         v[2].position.y = down ? bot : top;
    for (int i = 0; i < 3; i++) { v[i].color = fc; v[i].tex_coord.x = v[i].tex_coord.y = 0; }
    SDL_RenderGeometry(r, NULL, v, 3, NULL, 0);
}

/* "X" mark made of two diagonal lines spanning `size` pixels. */
void draw_cross(SDL_Renderer *r, float cx, float cy, float size, float thickness, Color c) {
    float h = size / 2;
    draw_line(r, cx - h, cy - h, cx + h, cy + h, thickness, c);
    draw_line(r, cx - h, cy + h, cx + h, cy - h, thickness, c);
}

/* Open chevron (two strokes) of `s` pixels: pointing down when `down`, else pointing right. */
void draw_chevron(SDL_Renderer *r, float cx, float cy, float s, float t, bool down, Color c) {
    float h = s / 2;
    if (down) {
        draw_line(r, cx - h, cy - h / 2, cx, cy + h / 2, t, c);
        draw_line(r, cx, cy + h / 2, cx + h, cy - h / 2, t, c);
    } else {
        draw_line(r, cx - h / 2, cy - h, cx + h / 2, cy, t, c);
        draw_line(r, cx + h / 2, cy, cx - h / 2, cy + h, t, c);
    }
}

/* Chevron pointing up or left (the counterparts of draw_chevron's down / right). */
void draw_chevron_back(SDL_Renderer *r, float cx, float cy, float s, float t, bool up, Color c) {
    float h = s / 2;
    if (up) {
        draw_line(r, cx - h, cy + h / 2, cx, cy - h / 2, t, c);
        draw_line(r, cx, cy - h / 2, cx + h, cy + h / 2, t, c);
    } else {
        draw_line(r, cx + h / 2, cy - h, cx - h / 2, cy, t, c);
        draw_line(r, cx - h / 2, cy, cx + h / 2, cy + h, t, c);
    }
}

/* Database icon: a rounded capsule of size `s` with two horizontal bands cut out in `bg`. */
void draw_icon_db(SDL_Renderer *r, float cx, float cy, float s, Color c, Color bg) {
    Rect body = rect_make(cx - s * 0.36f, cy - s * 0.45f, s * 0.72f, s * 0.9f);
    draw_rrect(r, body, s * 0.22f, c);
    draw_rect(r, rect_make(body.x + s * 0.1f, cy - s * 0.12f, body.w - s * 0.2f, s * 0.08f), bg);
    draw_rect(r, rect_make(body.x + s * 0.1f, cy + s * 0.16f, body.w - s * 0.2f, s * 0.08f), bg);
}

/* Table icon: a solid grid with an `accent` header row; grid lines are drawn in `bg`. */
void draw_icon_table(SDL_Renderer *r, float cx, float cy, float s, Color c, Color accent, Color bg) {
    Rect body = rect_make(cx - s * 0.45f, cy - s * 0.38f, s * 0.9f, s * 0.76f);
    draw_rrect(r, body, s * 0.14f, c);
    draw_rect(r, rect_make(body.x, body.y, body.w, s * 0.22f), accent);
    draw_rect(r, rect_make(body.x, body.y + s * 0.22f, body.w, s * 0.06f), bg);
    draw_rect(r, rect_make(body.x, body.y + s * 0.50f, body.w, s * 0.06f), bg);
    draw_rect(r, rect_make(cx - s * 0.03f, body.y + s * 0.22f, s * 0.06f, body.h - s * 0.22f), bg);
}

/* View icon: outlined table with a solid header, distinguishing it from the solid table glyph. */
void draw_icon_view(SDL_Renderer *r, float cx, float cy, float s, Color c, Color bg) {
    /* outlined table with a solid header: reads as "derived" next to the solid table glyph */
    Rect body = rect_make(cx - s * 0.45f, cy - s * 0.38f, s * 0.9f, s * 0.76f);
    draw_rrect(r, body, s * 0.14f, c);
    draw_rrect(r, rect_inset(body, s * 0.08f, s * 0.08f), s * 0.08f, bg);
    draw_rect(r, rect_make(body.x, body.y, body.w, s * 0.22f), c);
    draw_rect(r, rect_make(cx - s * 0.03f, body.y + s * 0.22f, s * 0.06f, body.h - s * 0.22f), c);
}

/* Column icon: a small filled dot. */
void draw_icon_column(SDL_Renderer *r, float cx, float cy, float s, Color c) {
    draw_rrect(r, rect_make(cx - s * 0.16f, cy - s * 0.16f, s * 0.32f, s * 0.32f), s * 0.16f, c);
}

/* Key icon (primary key marker): a round bow plus a shaft with one tooth. */
void draw_icon_key(SDL_Renderer *r, float cx, float cy, float s, Color c) {
    draw_rrect(r, rect_make(cx - s * 0.3f, cy - s * 0.3f, s * 0.42f, s * 0.42f), s * 0.21f, c);
    draw_line(r, cx, cy, cx + s * 0.4f, cy + s * 0.4f, s * 0.12f, c);
    draw_line(r, cx + s * 0.28f, cy + s * 0.28f, cx + s * 0.4f, cy + s * 0.16f, s * 0.12f, c);
}

/* ---- clipping ---------------------------------------------------------- */

/* Stack of active clip rectangles (render pixels, integer); each entry is already intersected
 * with the one below it. */
static SDL_Rect clip_stack[32];
static int clip_depth = 0;

/* Push a clip rectangle, intersected with the current one, and apply it to the renderer.
 * The rect is expanded outward to whole pixels. Beyond 32 levels the clip is applied but
 * not recorded, so a later pop restores an older rectangle. */
void clip_push(SDL_Renderer *r, Rect rc) {
    SDL_Rect nr = { (int)floorf(rc.x), (int)floorf(rc.y), (int)ceilf(rc.w), (int)ceilf(rc.h) };
    if (clip_depth > 0) {
        SDL_Rect out;
        if (!SDL_GetRectIntersection(&clip_stack[clip_depth - 1], &nr, &out)) out = (SDL_Rect){ 0, 0, 0, 0 };
        nr = out;
    }
    if (clip_depth < 32) clip_stack[clip_depth++] = nr;
    SDL_SetRenderClipRect(r, &nr);
}

/* Pop the innermost clip rectangle and restore the previous one (or no clipping at all). */
void clip_pop(SDL_Renderer *r) {
    if (clip_depth > 0) clip_depth--;
    SDL_SetRenderClipRect(r, clip_depth > 0 ? &clip_stack[clip_depth - 1] : NULL);
}

/* ---- text -------------------------------------------------------------- */

/* Draw a NUL-terminated string with its top-left corner at (x, y), in render pixels. */
void draw_text(Font *f, SDL_Renderer *r, const char *s, float x, float y, Color c) {
    font_draw(f, r, s, -1, x, y, c);
}

/* Draw `len` bytes of `s` (len < 0: whole string) vertically centred in `rc` and aligned with
 * ALIGN_*. When it doesn't fit, it is cut at a code-point boundary and an ellipsis appended
 * (always left-aligned in that case). */
void draw_text_fit(Font *f, SDL_Renderer *r, const char *s, int len, Rect rc, Color c, int align) {
    if (len < 0) len = (int)strlen(s);
    float w = font_width(f, s, len);
    float y = rc.y + (rc.h - font_height(f)) / 2;
    if (w <= rc.w) {
        float x = align == ALIGN_LEFT ? rc.x : align == ALIGN_CENTER ? rc.x + (rc.w - w) / 2 : rc.x + rc.w - w;
        font_draw(f, r, s, len, x, y, c);
        return;
    }
    const char *ell = "\xE2\x80\xA6";
    float ew = font_width(f, ell, -1);
    int n = len;
    while (n > 0 && font_width(f, s, n) + ew > rc.w) n = utf8_prev(s, n);
    float x = font_draw(f, r, s, n, rc.x, y, c);
    font_draw(f, r, ell, -1, x, y, c);
}
