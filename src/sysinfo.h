/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * System information
 * ==================
 *
 * Tiny portable probe used by the status bar / diagnostics to show how much memory Lume is
 * using. One implementation per platform lives in sysinfo.c (Mach on macOS, psapi on Windows,
 * /proc on Linux). Thread-safe: no shared state.
 */
#pragma once
#include <stddef.h>
/* Resident memory of this process in bytes, 0 if unavailable. */
/* Cheap enough to call once per frame or per sample tick. */
size_t sys_rss_bytes(void);
