/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Update check: asks GitHub for the latest release of Lume Data (Help > Check for Updates..., and
 * automatically at launch and then once a day while Lume stays open, unless turned off in Settings >
 * Privacy). A newer version is offered in a dialog: Download (the installer for this platform, opened
 * in the browser), Release Notes, Later, Skip This Version.
 *
 * The request is a plain GET of the public releases API: no id, no data about the user. Under the test
 * harness only a loopback LUME_UPDATE_URL is ever used.
 */
#ifndef LUME_UPDATE_H
#define LUME_UPDATE_H
#include "lume.h"

/* After the preferences are loaded: remembers where the last check time and the skipped version live. */
void update_init(App *a, const char *prefs_dir);
/* Every frame (cheap): starts the daily check when due, shows the answer when it arrives. */
void update_tick(App *a);
/* Help > Check for Updates...: checks now and always says the outcome. */
void update_check_now(App *a);
/* The dialog's answer: 0 download, 1 release notes, 2 later, 3 skip this version (-1 Esc: later). */
void update_answer(App *a, int choice);
/* -1 / 0 / 1 like strcmp, on dotted numbers ("v1.10.0" > "1.9.2"; a leading v is ignored). */
int update_version_cmp(const char *x, const char *y);
/* Harness: a request is in flight. */
bool update_busy(void);
/* Harness: state on one line. */
void update_dump(void);

#endif
