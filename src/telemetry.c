/* SPDX-License-Identifier: AGPL-3.0-only */
/* Anonymous usage statistics with consent: see telemetry.h. */
#include "telemetry.h"
#include "http.h"
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>
#ifdef _WIN32
#include <windows.h>
#else
#include <sys/utsname.h>
#endif

#ifndef LUME_VERSION
#define LUME_VERSION "0"
#endif
#ifndef LUME_BUILD_ID
#define LUME_BUILD_ID ""
#endif
#ifndef LUME_CHANNEL
#define LUME_CHANNEL "Development"
#endif

#define APP_KEY "A-EU-9511093279"   /* Aptabase app key: public by design (it only names the dashboard) */
#define ENDPOINT "https://eu.aptabase.com/api/v0/events"

static struct {
    bool on;
    char dir[1100];
    char session[24];
    double startup_ms;
    time_t t0;
    volatile int counts[TM_N];
    volatile int slow_frames;
} T;

/* ---- the system, coarsely ---- */
static const char *os_name(void) {
#if defined(__APPLE__)
    return "macOS";
#elif defined(_WIN32)
    return "Windows";
#else
    return "Linux";
#endif
}
static void os_version(char *o, size_t n) {   /* major.minor only */
#ifdef _WIN32
    snprintf(o, n, "10+");   /* Lume needs Windows 10 or later; the exact build is not needed */
#elif defined(__APPLE__)
    FILE *f = popen("/usr/bin/sw_vers -productVersion 2>/dev/null", "r"); char b[32] = "";
    if (f) { if (!fgets(b, sizeof b, f)) b[0] = 0; pclose(f); }
    int ma = 0, mi = 0; if (sscanf(b, "%d.%d", &ma, &mi) >= 1) snprintf(o, n, "%d.%d", ma, mi); else snprintf(o, n, "?");
#else
    struct utsname u; int ma = 0, mi = 0;
    if (!uname(&u) && sscanf(u.release, "%d.%d", &ma, &mi) == 2) snprintf(o, n, "%d.%d", ma, mi); else snprintf(o, n, "?");
#endif
}
static const char *arch(void) {
#if defined(__aarch64__) || defined(_M_ARM64)
    return "arm64";
#elif defined(__x86_64__) || defined(_M_X64)
    return "x86_64";
#else
    return "other";
#endif
}
/* "0", "1-9", "10-99", "100-999", "1000+" */
static const char *count_bucket(int v) { return v <= 0 ? "0" : v < 10 ? "1-9" : v < 100 ? "10-99" : v < 1000 ? "100-999" : "1000+"; }
static const char *ms_bucket(double ms) { return ms < 500 ? "<0.5s" : ms < 1000 ? "0.5-1s" : ms < 2000 ? "1-2s" : ms < 5000 ? "2-5s" : "5s+"; }
static const char *mb_bucket(size_t b) { size_t m = b >> 20; return m < 100 ? "<100MB" : m < 250 ? "100-250MB" : m < 500 ? "250-500MB" : m < 1024 ? "0.5-1GB" : "1GB+"; }
static const char *min_bucket(double s) { double m = s / 60; return m < 5 ? "<5min" : m < 30 ? "5-30min" : m < 120 ? "30min-2h" : "2h+"; }

static void iso_now(char *o, size_t n) {
    struct timespec ts; timespec_get(&ts, TIME_UTC);
    struct tm tm; gmtime_r(&ts.tv_sec, &tm);
    char b[32]; strftime(b, sizeof b, "%Y-%m-%dT%H:%M:%S", &tm);
    snprintf(o, n, "%s.%03ldZ", b, ts.tv_nsec / 1000000);
}
/* one event; props is a JSON object body without braces ("" none) */
static void event_json(char *o, size_t n, const char *name, const char *props) {
    char ts[40], osv[24]; iso_now(ts, sizeof ts); os_version(osv, sizeof osv);
    snprintf(o, n, "{\"timestamp\":\"%s\",\"sessionId\":\"%s\",\"eventName\":\"%s\",\"systemProps\":{\"isDebug\":%s,\"locale\":\"en\",\"osName\":\"%s\",\"osVersion\":\"%s\",\"appVersion\":\"%s\",\"appBuildNumber\":\"%s\",\"sdkVersion\":\"lume-c@1\"},\"props\":{%s}}",
             ts, T.session, name, strcmp(LUME_CHANNEL, "Development") ? "false" : "true", os_name(), osv, LUME_VERSION, LUME_BUILD_ID, props);
}

/* ---- sending: a detached thread, never in the way ---- */
static const char *endpoint(void) {
    const char *e = getenv("LUME_TELEMETRY_URL");
    if (e && !strncmp(e, "http://127.0.0.1:", 17)) return e;   /* tests: a loopback mock only */
    if (getenv("LUME_SCRIPT")) return NULL;                      /* the harness never reaches the internet */
    return ENDPOINT;
}
static void *send_th(void *p) {
    char *body = p, *out = NULL, err[200]; int st = 0;
    const char *url = endpoint();
    if (url && http_request("POST", url, "Content-Type: application/json\nApp-Key: " APP_KEY "\nUser-Agent: Lume Data", body, strlen(body), 15, false, NULL, NULL, NULL, &out, &st, err, sizeof err)) free(out);
    free(body);
    return NULL;
}
static void send_async(char *body) {   /* takes body */
    if (!endpoint()) { free(body); return; }
    pthread_t t;
    if (pthread_create(&t, NULL, send_th, body) == 0) pthread_detach(t); else free(body);
}

static void pending_path(char *o, size_t n) { snprintf(o, n, "%stelemetry-pending.json", T.dir); }

void telemetry_start(const char *prefs_dir, bool enabled, double startup_ms) {
    snprintf(T.dir, sizeof T.dir, "%s", prefs_dir ? prefs_dir : "");
    T.on = enabled; T.startup_ms = startup_ms; T.t0 = time(NULL);
    unsigned long long r = 0;
    FILE *u = fopen("/dev/urandom", "rb"); if (u) { if (fread(&r, sizeof r, 1, u) != 1) r = 0; fclose(u); }
    if (!r) r = (unsigned long long)clock() * 2654435761ULL ^ (unsigned long long)time(NULL);
    snprintf(T.session, sizeof T.session, "%llu", (unsigned long long)time(NULL) * 100000000ULL + r % 100000000ULL);   /* Aptabase's format; new every launch */
    char pp[1200]; pending_path(pp, sizeof pp);
    if (!T.on) { remove(pp); return; }
    /* the previous session's summary, then this start */
    size_t cap = 8192, len = 0; char *body = malloc(cap); if (!body) return;
    len += (size_t)snprintf(body + len, cap - len, "[");
    FILE *f = fopen(pp, "r");
    if (f) { char prev[4000]; size_t k = fread(prev, 1, sizeof prev - 1, f); prev[k] = 0; fclose(f); remove(pp); if (k && prev[0] == '{') len += (size_t)snprintf(body + len, cap - len, "%s,", prev); }
    char props[300], ev[2000];
    snprintf(props, sizeof props, "\"arch\":\"%s\",\"startup\":\"%s\"", arch(), ms_bucket(startup_ms));
    event_json(ev, sizeof ev, "app_started", props);
    len += (size_t)snprintf(body + len, cap - len, "%s]", ev);
    send_async(body);
}
void telemetry_count(TelemetryCounter c) { if (c >= 0 && c < TM_N) __atomic_add_fetch(&T.counts[c], 1, __ATOMIC_RELAXED); }
void telemetry_frame(double ms) { if (ms > 50) __atomic_add_fetch(&T.slow_frames, 1, __ATOMIC_RELAXED); }
void telemetry_stop(size_t peak_rss_bytes) {
    if (!T.on || !T.dir[0]) return;
    char props[600], ev[2400];
    snprintf(props, sizeof props, "\"queries\":\"%s\",\"plans\":\"%s\",\"reports\":\"%s\",\"ai_requests\":\"%s\",\"slow_frames\":\"%s\",\"memory\":\"%s\",\"duration\":\"%s\"",
             count_bucket(T.counts[TM_QUERY]), count_bucket(T.counts[TM_PLAN]), count_bucket(T.counts[TM_REPORT]), count_bucket(T.counts[TM_AI]),
             count_bucket(T.slow_frames), mb_bucket(peak_rss_bytes), min_bucket(difftime(time(NULL), T.t0)));
    event_json(ev, sizeof ev, "session_summary", props);
    char pp[1200]; pending_path(pp, sizeof pp);
    FILE *f = fopen(pp, "w"); if (f) { fputs(ev, f); fclose(f); }
}
void telemetry_set_enabled(bool on) {
    T.on = on;
    if (!on) { char pp[1200]; pending_path(pp, sizeof pp); remove(pp); }
}
bool telemetry_enabled(void) { return T.on; }
void telemetry_preview(char *out, size_t n) {
    char props[300]; snprintf(props, sizeof props, "\"arch\":\"%s\",\"startup\":\"%s\"", arch(), ms_bucket(T.startup_ms));
    event_json(out, n, "app_started", props);
}
void telemetry_dump(void) {
    printf("telemetry on=%d queries=%d plans=%d reports=%d ai=%d slow_frames=%d\n", T.on, T.counts[TM_QUERY], T.counts[TM_PLAN], T.counts[TM_REPORT], T.counts[TM_AI], T.slow_frames);
    fflush(stdout);
}
