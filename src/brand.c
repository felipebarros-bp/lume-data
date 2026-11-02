/* SPDX-License-Identifier: AGPL-3.0-only */
/* Brand assets: see brand.h. */
#include "brand.h"
#include "draw.h"
#include "image.h"
#include "log.h"
#include "product.h"
#include "theme.h"
#include <stdio.h>
#include <string.h>

static const int sizes[] = { 32, 64, 128, 256, 512 };
#define NSIZES 5
static SDL_Texture *tex[NSIZES];
static bool tried[NSIZES], fallback, warned;

/* Decodes assets/brand/<name> into a surface (caller destroys it), NULL when missing / invalid. */
static SDL_Surface *load_png(App *a, const char *name, Image *keep) {
    char path[1300]; snprintf(path, sizeof path, "%s/brand/%s", a->assets_dir, name);
    size_t n = 0; void *data = SDL_LoadFile(path, &n);
    if (!data) return NULL;
    char err[200];
    bool ok = image_decode(data, n, keep, err, sizeof err);
    SDL_free(data);
    if (!ok) { LOG_W("brand asset %s: %s", name, err); return NULL; }
    return SDL_CreateSurfaceFrom(keep->w, keep->h, SDL_PIXELFORMAT_RGBA32, keep->rgba, keep->w * 4);
}

static SDL_Texture *symbol_tex(App *a, int px_needed) {
    int k = 0; while (k < NSIZES - 1 && sizes[k] < px_needed) k++;
    for (int i = k; i >= 0; i--) {   /* the right size, else the largest smaller one that loads */
        if (!tried[i]) {
            tried[i] = true;
            char name[160]; snprintf(name, sizeof name, "%s-%d.png", product_current()->icon_asset, sizes[i]);
            Image im = { 0 };
            SDL_Surface *s = load_png(a, name, &im);
            if (s) {
                tex[i] = SDL_CreateTextureFromSurface(a->renderer, s);
                if (tex[i]) { SDL_SetTextureBlendMode(tex[i], SDL_BLENDMODE_BLEND); SDL_SetTextureScaleMode(tex[i], SDL_SCALEMODE_LINEAR); }
                SDL_DestroySurface(s);
            }
            image_free(&im);
        }
        if (tex[i]) return tex[i];
    }
    return NULL;
}

/* Three stacked rounded rhombi in one colour: the symbol's shape without its asset. */
static void draw_fallback(App *a, Rect rc) {
    SDL_Renderer *r = a->renderer;
    float w = rc.w * 0.8f, h = w * 0.46f, cx = rc.x + rc.w / 2, gap = rc.h * 0.14f;
    for (int l = 2; l >= 0; l--) {
        float cy = rc.y + rc.h * 0.36f + l * gap;
        Color c = color_alpha(color_hex(T_TEXT_MUTED), (uint8_t)(255 - l * 60));
        draw_triangle(r, cx, cy - h / 4, w, h / 2, false, c);
        draw_triangle(r, cx, cy + h / 4, w, h / 2, true, c);
    }
}

void brand_draw_symbol(App *a, Rect rc) {
    float side = rc.w < rc.h ? rc.w : rc.h;
    Rect sq = rect_make(rc.x + (rc.w - side) / 2, rc.y + (rc.h - side) / 2, side, side);
    SDL_Texture *t = symbol_tex(a, (int)side);
    fallback = t == NULL;
    if (t) { SDL_FRect d = { sq.x, sq.y, sq.w, sq.h }; SDL_RenderTexture(a->renderer, t, NULL, &d); return; }
    if (!warned) { warned = true; LOG_W("brand: the Lume Data symbol asset is missing under %s/brand; drawing the monochrome fallback", a->assets_dir); }
    draw_fallback(a, sq);
}

bool brand_fallback_used(void) { return fallback; }

void brand_set_window_icon(App *a) {
    Image im = { 0 };
    SDL_Surface *s = load_png(a, "lume-data-app-dark-512.png", &im);
    if (!s) { LOG_W("brand: window icon asset missing"); return; }
    SDL_SetWindowIcon(a->window, s);
    SDL_DestroySurface(s);
    image_free(&im);
}

void brand_free(void) {
    for (int i = 0; i < NSIZES; i++) { if (tex[i]) SDL_DestroyTexture(tex[i]); tex[i] = NULL; tried[i] = false; }
}
