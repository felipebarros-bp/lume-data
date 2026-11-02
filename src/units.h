/* SPDX-License-Identifier: AGPL-3.0-only */
#pragma once
/*
 * Human-readable units.
 *
 * fmt_duration() writes a duration in the unit that fits its magnitude, with up to three
 * significant digits and no trailing zeros:
 *
 *     0.000001 ms -> "1ns"      0.005 ms -> "5μs"      0.05 ms -> "50μs"
 *     100 ms      -> "100ms"    1234.5 ms -> "1.23s"   65000 ms -> "1min 5s"
 *
 * The input is in milliseconds because that is what every timer in Lume stores. Negative
 * values (meaning "not measured") write "-". Returns buf so it can be used inline.
 */
#include <stddef.h>

char *fmt_duration(char *buf, size_t n, double ms);
