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

/* memmem: a byte string inside another */
static inline void *lume_memmem(const void *h, size_t hl, const void *n, size_t nl) {
    if (!nl) return (void *)h;
    for (const char *p = h; hl >= nl; p++, hl--) if (!memcmp(p, n, nl)) return (void *)p;
    return NULL;
}
#define memmem lume_memmem

/* the thread-safe time conversions (UCRT has the _s forms, arguments swapped) */
static inline struct tm *lume_localtime_r(const time_t *t, struct tm *r) { return localtime_s(r, t) ? NULL : r; }
static inline struct tm *lume_gmtime_r(const time_t *t, struct tm *r) { return gmtime_s(r, t) ? NULL : r; }
#define localtime_r lume_localtime_r
#define gmtime_r lume_gmtime_r
#define timegm _mkgmtime

/* signals and file flags Windows does not have (never raised / not needed there) */
#include <fcntl.h>
#include <signal.h>
#ifndef SIGBUS
#define SIGBUS 10
#endif
#ifndef O_CLOEXEC
#define O_CLOEXEC _O_NOINHERIT
#endif

/* files */
#define fsync(fd) _commit(fd)
#define realpath(p, r) _fullpath((r), (p), 4096)
#define mkdir(p, mode) _mkdir(p)
/* no symbolic links to tell apart: lstat is stat */
#define lstat stat
#ifndef S_ISLNK
#define S_ISLNK(m) 0
#endif
/* mkdtemp: the template's trailing XXXXXX made unique, then created */
static inline char *lume_mkdtemp(char *tpl) {
    size_t L = strlen(tpl);
    for (int tries = 0; tries < 100; tries++) {
        if (L >= 6) for (size_t i = L - 6; i < L; i++) tpl[i] = "abcdefghijklmnopqrstuvwxyz0123456789"[rand() % 36];
        if (_mkdir(tpl) == 0) return tpl;
    }
    return NULL;
}
#define mkdtemp lume_mkdtemp

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
