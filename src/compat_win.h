/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * POSIX pieces Lume uses that Windows (MinGW-w64 / UCRT) lacks or spells differently. Force-included
 * on Windows builds only (CMakeLists.txt); every other platform never sees this file.
 */
#ifndef LUME_COMPAT_WIN_H
#define LUME_COMPAT_WIN_H
#ifdef _WIN32
#include <ctype.h>
#include <direct.h>
#include <io.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <time.h>

/* strcasestr: case-insensitive substring search (ASCII) */
static inline char *lume_strcasestr(const char *h, const char *n) {
    size_t nl = strlen(n);
    if (!nl) return (char *)h;
    for (; *h; h++) {
        size_t i = 0;
        while (i < nl && h[i] && tolower((unsigned char)h[i]) == tolower((unsigned char)n[i])) i++;
        if (i == nl) return (char *)h;
    }
    return NULL;
}
#define strcasestr lume_strcasestr

/* the thread-safe time conversions (UCRT has the _s forms, arguments swapped) */
static inline struct tm *lume_localtime_r(const time_t *t, struct tm *r) { return localtime_s(r, t) ? NULL : r; }
static inline struct tm *lume_gmtime_r(const time_t *t, struct tm *r) { return gmtime_s(r, t) ? NULL : r; }
#define localtime_r lume_localtime_r
#define gmtime_r lume_gmtime_r
#define timegm _mkgmtime

/* files */
#define fsync(fd) _commit(fd)
#define realpath(p, r) _fullpath((r), (p), 4096)
#define mkdir(p, mode) _mkdir(p)

/* getline: a line of any length, grown with realloc (POSIX semantics) */
static inline long long lume_getline(char **lp, size_t *n, FILE *f) {
    if (!lp || !n || !f) return -1;
    if (!*lp || !*n) { *n = 128; *lp = realloc(*lp, *n); if (!*lp) return -1; }
    size_t k = 0; int c;
    while ((c = fgetc(f)) != EOF) {
        if (k + 2 > *n) { size_t nn = *n * 2; char *p = realloc(*lp, nn); if (!p) return -1; *lp = p; *n = nn; }
        (*lp)[k++] = (char)c;
        if (c == '\n') break;
    }
    if (!k) return -1;
    (*lp)[k] = 0;
    return (long long)k;
}
#define getline lume_getline

#endif
#endif
