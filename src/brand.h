/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Brand assets (Lume Data brand kit v1, assets/brand/)
 * ===================================================
 *
 * The approved three-layer symbol (cyan -> blue -> purple) as textures, the window / Dock icon
 * and a monochrome fallback when an asset is missing (logged once, never a placeholder picture).
 * PNGs are decoded with image.c (bounds-checked), uploaded once per size and reused: nothing is
 * created per frame (ABOUT-002 / 030, spec 5.4).
 */
#include "lume.h"

/* Draws the symbol inside rc (render pixels), keeping its proportions; the asset is the
 * smallest exported size at least as large as rc (crisp on Retina). */
void brand_draw_symbol(App *a, Rect rc);
/* Window icon (the Dock icon of an unbundled app on macOS) from the app-icon export. */
void brand_set_window_icon(App *a);
/* true when the last symbol draw had to use the monochrome fallback */
bool brand_fallback_used(void);
void brand_free(void);
