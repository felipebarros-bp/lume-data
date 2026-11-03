/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Source Control (GIT-001..071, MVP): the dockable "Source Control" panel, Git diff tabs, file
 * commands, the status-bar indicator, and the harness. The headless parts are in gitcore.h.
 *
 *   Every Git call runs on a worker thread (argv only, timeout, cancel); the SDL loop never waits.
 *   After every operation the repository is read again before anything is shown as done (rule 4).
 *   Operations that touch the working tree first check unsaved editor buffers inside the
 *   repository (GIT-064). Destructive ones are confirmed and audited in <prefs>/audit/git.log.
 *   Nothing here applies DDL or touches a database connection (rule 1).
 */
#include "lume.h"

typedef struct GdDoc GdDoc;

void gitview_init(App *a);
void gitview_shutdown(App *a);
void gitview_pre_update(App *a);       /* jobs, watcher, menus */
void gitview_draw_overlay(App *a);
void gitview_statusbar(App *a, char *out, int n);   /* "Git: main  ↑1 ↓0" or "" */

/* diff tabs (panels.c) */
void gitview_diff_body(App *a, GdDoc *d, Rect body);
void gitview_diff_closed(App *a, GdDoc *d);

/* harness: git <op> [args]; gitstate */
void gitview_cmd(App *a, const char *arg);
void gitview_dump(App *a);
bool gitview_idle(App *a);
int gitview_pull_mode(App *a);                /* Settings: 0 fast-forward only, 1 merge, 2 rebase */
void gitview_set_pull_mode(App *a, int m);
/* Settings > Source Control: time allowed for network operations (fetch, pull, push...) and clone */
void gitview_set_timeouts(int net_ms, int clone_ms);
const char *gitview_identity(void);          /* read from git config; Lume never changes it */
