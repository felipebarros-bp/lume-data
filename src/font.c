/* SPDX-License-Identifier: AGPL-3.0-only */
/* font.c: TrueType text rendering with stb_truetype and one SDL texture atlas per font.
 *
 * font_load() rasterises a fixed glyph set (printable ASCII, Latin-1 Supplement and a short list
 * of extra symbols used by the UI: ellipsis, arrows, macOS modifier glyphs, check / cross marks,
 * angle quotes) at one pixel size into a single white-with-alpha texture. Code points outside
 * that set render as '?'. Sizes are in render pixels: main.c loads each font at
 * points * App.scale, so a change of display density means reloading the fonts.
 *
 * font_draw() batches one quad per glyph into a single SDL_RenderGeometry call. Tabs advance
 * by TAB_COLS spaces; other control characters are skipped. The module also owns the small
 * UTF-8 helpers (utf8_next / utf8_prev) used throughout the editor and widgets.
 *
 * Ownership: a Font owns its texture and the font file bytes; free with font_destroy().
 * Not thread-safe (font_draw keeps static scratch buffers); call from the render thread only. */
#define STB_TRUETYPE_IMPLEMENTATION
#define STBTT_STATIC
#include "stb_truetype.h"
#include "font.h"

/* Glyph ranges packed into the atlas: ASCII 32..126, Latin-1 160..255, plus extra_cps. */
enum { ASCII_FIRST = 32, ASCII_COUNT = 95, LATIN_FIRST = 160, LATIN_COUNT = 96 };
/* Individual non-Latin code points the UI draws: … → ⌘ ⇧ ⌥ ⌃ ↩ • ▸ ▾ ✓ ✕ ‹ › */
static int extra_cps[] = { 0x2026, 0x2192, 0x2318, 0x21E7, 0x2325, 0x2303,
                           0x21A9, 0x2022, 0x25B8, 0x25BE, 0x2713, 0x2715, 0x2039, 0x203A,
                           0x2190, 0x2191, 0x2193, 0x2194, 0x25CF,     /* arrows and the dot of Source Control (ahead / behind, renames, current branch) */
                           0x2013, 0x2014, 0x2018, 0x2019, 0x201C, 0x201D };   /* dashes and typographic quotes (AI-written titles and text) */
#define EXTRA_COUNT ((int)(sizeof extra_cps / sizeof extra_cps[0]))
/* A tab advances by this many space widths (no tab stops: a fixed advance). */
#define TAB_COLS 4

/* One loaded font at one pixel size. */
struct Font {
    /* tex: glyph atlas (RGBA, white with coverage in alpha); tw, th: its size in pixels.
     * size: requested pixel height; ascent / descent / line_h: vertical metrics in pixels;
     * space_w: advance of ' ', used for tabs. ascii / latin / extra: packed glyph records.
     * data: the font file bytes (SDL_LoadFile), kept alive for the font's lifetime. */
    SDL_Texture *tex;
    int tw, th;
    float size, ascent, descent, line_h, space_w;
    stbtt_packedchar ascii[ASCII_COUNT], latin[LATIN_COUNT], extra[EXTRA_COUNT];
    unsigned char *data;
};

/* Look up the packed glyph for a code point; anything not in the atlas maps to '?'. */
static stbtt_packedchar *glyph(Font *f, uint32_t cp) {
    if (cp >= ASCII_FIRST && cp < ASCII_FIRST + ASCII_COUNT) return &f->ascii[cp - ASCII_FIRST];
    if (cp >= LATIN_FIRST && cp < LATIN_FIRST + LATIN_COUNT) return &f->latin[cp - LATIN_FIRST];
    for (int i = 0; i < EXTRA_COUNT; i++)
        if ((uint32_t)extra_cps[i] == cp) return &f->extra[i];
    return &f->ascii['?' - ASCII_FIRST];
}

/* Decode the code point starting at byte index i of s (length len bytes) into *cp and return
 * the byte index just past it. Invalid lead bytes or truncated sequences decode as one byte;
 * continuation bytes are not validated. */
int utf8_next(const char *s, int len, int i, uint32_t *cp) {
    unsigned char c = (unsigned char)s[i];
    int n = c < 0x80 ? 1 : (c >> 5) == 6 ? 2 : (c >> 4) == 14 ? 3 : (c >> 3) == 30 ? 4 : 1;
    if (i + n > len) n = 1;
    uint32_t v = n == 1 ? c : n == 2 ? (c & 0x1F) : n == 3 ? (c & 0x0F) : (c & 0x07);
    for (int k = 1; k < n; k++) v = (v << 6) | ((unsigned char)s[i + k] & 0x3F);
    *cp = v;
    return i + n;
}

/* Return the byte index where the code point ending before byte index i starts (0 at the start). */
int utf8_prev(const char *s, int i) {
    if (i <= 0) return 0;
    i--;
    while (i > 0 && (((unsigned char)s[i]) & 0xC0) == 0x80) i--;
    return i;
}

/* Load the TTF/OTF at `path` and build its glyph atlas at `size` render pixels (line height
 * per stb's ScaleForPixelHeight). Returns NULL if the file can't be read or parsed.
 * The caller owns the result and frees it with font_destroy(). */
Font *font_load(SDL_Renderer *r, const char *path, float size) {
    size_t len = 0;
    unsigned char *data = SDL_LoadFile(path, &len);
    if (!data) return NULL;

    stbtt_fontinfo info;
    if (!stbtt_InitFont(&info, data, stbtt_GetFontOffsetForIndex(data, 0))) { SDL_free(data); return NULL; }

    Font *f = calloc(1, sizeof *f);
    f->data = data;
    f->size = size;
    int asc, desc, gap;
    stbtt_GetFontVMetrics(&info, &asc, &desc, &gap);
    float sc = stbtt_ScaleForPixelHeight(&info, size);
    f->ascent = asc * sc;
    f->descent = desc * sc;
    f->line_h = (asc - desc + gap) * sc;

    /* pack all ranges with 2x2 oversampling; double the atlas until everything fits (max 4096) */
    int dim = 512;
    unsigned char *bmp = NULL;
    for (;;) {
        bmp = calloc((size_t)dim * dim, 1);
        stbtt_pack_context pc;
        stbtt_PackBegin(&pc, bmp, dim, dim, 0, 1, NULL);
        stbtt_PackSetOversampling(&pc, 2, 2);
        stbtt_pack_range ranges[3] = {
            { size, ASCII_FIRST, NULL, ASCII_COUNT, f->ascii, 0, 0 },
            { size, LATIN_FIRST, NULL, LATIN_COUNT, f->latin, 0, 0 },
            { size, 0, extra_cps, EXTRA_COUNT, f->extra, 0, 0 },
        };
        int ok = stbtt_PackFontRanges(&pc, data, 0, ranges, 3);
        stbtt_PackEnd(&pc);
        if (ok || dim >= 4096) break;
        free(bmp);
        dim *= 2;
    }

    /* expand the 8-bit coverage bitmap to RGBA white + alpha so the texture can be tinted */
    uint32_t *rgba = malloc((size_t)dim * dim * 4);
    for (int i = 0; i < dim * dim; i++) {
        uint8_t a = bmp[i];
        ((uint8_t *)&rgba[i])[0] = 255; ((uint8_t *)&rgba[i])[1] = 255;
        ((uint8_t *)&rgba[i])[2] = 255; ((uint8_t *)&rgba[i])[3] = a;
    }
    free(bmp);

    f->tw = f->th = dim;
    f->tex = SDL_CreateTexture(r, SDL_PIXELFORMAT_RGBA32, SDL_TEXTUREACCESS_STATIC, dim, dim);
    SDL_UpdateTexture(f->tex, NULL, rgba, dim * 4);
    SDL_SetTextureBlendMode(f->tex, SDL_BLENDMODE_BLEND);
    SDL_SetTextureScaleMode(f->tex, SDL_SCALEMODE_LINEAR);
    free(rgba);

    f->space_w = f->ascii[0].xadvance;
    return f;
}

/* Free a font, its atlas texture and the font file bytes. NULL is allowed. */
void font_destroy(Font *f) {
    if (!f) return;
    if (f->tex) SDL_DestroyTexture(f->tex);
    SDL_free(f->data);
    free(f);
}

/* Vertical metrics, all in render pixels: line height, ascent above the baseline, and the
 * pixel size the font was loaded at. */
float font_height(const Font *f) { return f->line_h; }
float font_ascent(const Font *f) { return f->ascent; }
float font_size(const Font *f) { return f->size; }

/* Advance width in render pixels of `len` bytes of s (len < 0: up to the NUL), measured the
 * same way font_draw lays text out (tabs = TAB_COLS spaces, control characters ignored). */
float font_width(Font *f, const char *s, int len) {
    if (len < 0) len = (int)strlen(s);
    float w = 0;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i = utf8_next(s, len, i, &cp);
        if (cp == '\t') { w += f->space_w * TAB_COLS; continue; }
        if (cp < 32) continue;
        w += glyph(f, cp)->xadvance;
    }
    return w;
}

/* Draw `len` bytes of s (len < 0: whole string) with its top at y and pen starting at x, tinted
 * with c. Returns the pen x after the last glyph, so runs can be chained. The vertex / index
 * buffers are static and grow on demand (never freed). */
float font_draw(Font *f, SDL_Renderer *r, const char *s, int len, float x, float y, Color c) {
    static SDL_Vertex *verts = NULL;
    static int *idx = NULL;
    static int cap = 0;
    if (len < 0) len = (int)strlen(s);
    if (len > cap) {
        cap = len + 64;
        verts = realloc(verts, sizeof *verts * 4 * cap);
        idx = realloc(idx, sizeof *idx * 6 * cap);
    }
    SDL_FColor fc = { c.r / 255.0f, c.g / 255.0f, c.b / 255.0f, c.a / 255.0f };
    float baseline = y + f->ascent;
    int n = 0;
    for (int i = 0; i < len;) {
        uint32_t cp;
        i = utf8_next(s, len, i, &cp);
        if (cp == '\t') { x += f->space_w * TAB_COLS; continue; }
        if (cp < 32) continue;
        stbtt_aligned_quad q;
        stbtt_GetPackedQuad(glyph(f, cp), f->tw, f->th, 0, &x, &baseline, &q, 0);
        SDL_Vertex *v = &verts[n * 4];
        v[0].position.x = q.x0; v[0].position.y = q.y0; v[0].tex_coord.x = q.s0; v[0].tex_coord.y = q.t0;
        v[1].position.x = q.x1; v[1].position.y = q.y0; v[1].tex_coord.x = q.s1; v[1].tex_coord.y = q.t0;
        v[2].position.x = q.x1; v[2].position.y = q.y1; v[2].tex_coord.x = q.s1; v[2].tex_coord.y = q.t1;
        v[3].position.x = q.x0; v[3].position.y = q.y1; v[3].tex_coord.x = q.s0; v[3].tex_coord.y = q.t1;
        for (int k = 0; k < 4; k++) v[k].color = fc;
        int *ix = &idx[n * 6];
        ix[0] = n * 4; ix[1] = n * 4 + 1; ix[2] = n * 4 + 2;
        ix[3] = n * 4; ix[4] = n * 4 + 2; ix[5] = n * 4 + 3;
        n++;
    }
    if (n) SDL_RenderGeometry(r, f->tex, verts, n * 4, idx, n * 6);
    return x;
}
