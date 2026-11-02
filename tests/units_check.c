/* SPDX-License-Identifier: AGPL-3.0-only */
/* fmt_duration: unit by magnitude, three significant digits, rounding across units. */
#include "units.h"
#include <stdio.h>
#include <string.h>

static int fails;
static void check(double ms, const char *want) {
    char b[32]; fmt_duration(b, sizeof b, ms);
    if (strcmp(b, want)) { printf("FAIL  %g ms -> \"%s\" (want \"%s\")\n", ms, b, want); fails++; }
    else printf("ok    %g ms -> %s\n", ms, b);
}

int main(void) {
    check(0.000001, "1ns");
    check(0.0000004, "0.4ns");
    check(0.005, "5\xC2\xB5s");
    check(0.05, "50\xC2\xB5s");
    check(0.0123456, "12.3\xC2\xB5s");
    check(0.9996, "1ms");
    check(1.5, "1.5ms");
    check(100, "100ms");
    check(999.4, "999ms");
    check(999.6, "1s");
    check(1000, "1s");
    check(1234.5, "1.23s");
    check(59990, "1min");     /* 59.99s rounds to 60s */
    check(59400, "59.4s");
    check(59996, "1min");
    check(65000, "1min 5s");
    check(3725000, "1h 2min");
    check(0, "0s");
    check(-1, "-");
    printf("UNITS: %s (%d failures)\n", fails ? "FAIL" : "OK", fails);
    return fails != 0;
}
