/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * About Lume Data (spec "About / Sobre o aplicativo", ABOUT-001..032)
 * ===================================================================
 *
 * One modal panel over the workspace, the same instance from every entry point (Help > About
 * Lume Data, the "Lume" brand in the menu bar, the palette command app.about.open): opening it
 * again only brings it back. It shows the official symbol, product name, tagline, description
 * and the real build / platform / driver information (product.h), links that open only when
 * configured (https, through url_open), Lume's license state, the third-party licenses of what
 * is really distributed or loaded, Copy Info (allowlist) and Copy Version.
 *
 * Keyboard: Tab / Shift+Tab move between enabled controls, Enter / Space activate, Esc closes
 * (or goes back from Licenses); the focus returns to where it was. Closing never touches
 * editors, connections, results or transactions. Nothing runs while it is idle: no timer, no
 * polling, no network, no database query (ABOUT-030).
 */
#include "lume.h"

/* Registers the app.about.* commands (spec 5.2). */
void about_init(App *a);
void about_open(App *a);
/* SDL event type posted by the native application menu's About item (0 when there is none). */
Uint32 about_event_type(void);
void about_close(App *a);
bool about_is_open(void);
/* per frame, early: the open About owns this frame's input */
void about_pre_update(App *a);
/* per frame, last: draws it */
void about_draw(App *a);
/* harness: its state (view, focus, statuses) on stdout */
void about_dump(App *a);
