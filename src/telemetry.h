/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Anonymous usage statistics, only with the user's consent (Settings > Privacy; off by default).
 *
 * What is sent: the app version and build, the OS name / version and architecture, and per session a
 * few counts and timings in coarse buckets ("10-99 queries", "startup 0.5-1 s"). What is never sent:
 * SQL, data, names of connections / tables / files, hosts, users, paths, keys, prompts; no installation
 * id either (the session id is new every launch). Events go to Aptabase (EU region), which uses the IP
 * address only to derive the country and never stores it.
 *
 * The session's summary is written when Lume quits and sent on the next launch, so quitting never waits
 * on the network. Under the test harness nothing leaves the machine: LUME_TELEMETRY_URL may point at a
 * loopback mock, and is ignored otherwise.
 */
#ifndef LUME_TELEMETRY_H
#define LUME_TELEMETRY_H
#include <stdbool.h>
#include <stddef.h>

typedef enum { TM_QUERY, TM_PLAN, TM_REPORT, TM_AI, TM_N } TelemetryCounter;

/* After the preferences are loaded: remembers where to keep the pending summary, sends the previous
 * session's summary and this launch's start event (both only when enabled). */
void telemetry_start(const char *prefs_dir, bool enabled, double startup_ms);
/* Counts one use (cheap, any thread). */
void telemetry_count(TelemetryCounter c);
/* A frame took this long (ms): only frames slower than 50 ms are counted. */
void telemetry_frame(double ms);
/* At quit: writes the session's summary for the next launch (only when enabled). */
void telemetry_stop(size_t peak_rss_bytes);
/* The consent switch. Turning it off deletes anything pending. */
void telemetry_set_enabled(bool on);
bool telemetry_enabled(void);
/* The exact JSON the start event would carry now (for Settings > Privacy "what is sent"). */
void telemetry_preview(char *out, size_t n);
/* Harness: counters and state. */
void telemetry_dump(void);

#endif
