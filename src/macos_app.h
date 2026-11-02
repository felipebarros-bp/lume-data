/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/* macOS application menu: the native "About" item opens Lume's About (macos_app.m). Other
 * platforms have no such menu: the functions are stubs there. */
#include <stdbool.h>
#include <SDL3/SDL.h>

#ifdef __APPLE__
/* Retargets the native About item to post event_type; false when SDL created no such item. */
bool macos_route_about(Uint32 event_type, const char *product_name);
/* Performs the native item's action, exactly as a click would (test harness). */
bool macos_trigger_about(void);
const char *macos_about_title(void);
/* Retargets the native "Settings..." item (greyed out by SDL) to post event_type. */
bool macos_route_settings(Uint32 event_type);
bool macos_trigger_settings(void);
#else
static inline bool macos_route_about(Uint32 event_type, const char *product_name) { (void)event_type; (void)product_name; return false; }
static inline bool macos_trigger_about(void) { return false; }
static inline const char *macos_about_title(void) { return ""; }
static inline bool macos_route_settings(Uint32 event_type) { (void)event_type; return false; }
static inline bool macos_trigger_settings(void) { return false; }
#endif
