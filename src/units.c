/* SPDX-License-Identifier: AGPL-3.0-only */
/* Human-readable units: see units.h. */
#include "units.h"
#include <math.h>
#include <stdio.h>
#include <string.h>

/* Writes v with up to three significant digits, dropping trailing zeros ("1.50" -> "1.5"). */
static void sig3(char *buf, size_t n, double v) {
    int dec = v >= 100 ? 0 : v >= 10 ? 1 : 2;
    snprintf(buf, n, "%.*f", dec, v);
    if (strchr(buf, '.')) {
        char *e = buf + strlen(buf) - 1;
        while (*e == '0') *e-- = 0;
        if (*e == '.') *e = 0;
    }
}

/* Rounds v the same way sig3 prints it, so 999.6 is seen as 1000 and moves up one unit. */
static double round3(double v) {
    double f = v >= 100 ? 1 : v >= 10 ? 10 : 100;
    return round(v * f) / f;
}

char *fmt_duration(char *buf, size_t n, double ms) {
    if (!buf || n == 0) return buf;
    if (isnan(ms) || ms < 0) { snprintf(buf, n, "-"); return buf; }
    if (ms == 0) { snprintf(buf, n, "0s"); return buf; }

    /* a minute or more: whole minutes and seconds, then hours and minutes */
    double s = ms / 1000.0;
    if (round3(s) >= 60) {
        long total = lround(s);
        if (total >= 3600) snprintf(buf, n, "%ldh %ldmin", total / 3600, (total % 3600) / 60);
        else if (total % 60) snprintf(buf, n, "%ldmin %lds", total / 60, total % 60);
        else snprintf(buf, n, "%ldmin", total / 60);
        return buf;
    }

    /* below a minute: the largest unit in which the value is at least 1 */
    static const struct { const char *name; double per_ms; } unit[] = {
        { "s", 1e-3 }, { "ms", 1 }, { "\xC2\xB5s", 1e3 }, { "ns", 1e6 },
    };
    int u = 0;
    while (u < 3 && ms * unit[u].per_ms < 1) u++;
    double v = round3(ms * unit[u].per_ms);
    if (v >= 1000 && u > 0) { u--; v = round3(ms * unit[u].per_ms); }   /* 999.6μs -> 1ms */

    char num[32];
    sig3(num, sizeof num, v);
    snprintf(buf, n, "%s%s", num, unit[u].name);
    return buf;
}
