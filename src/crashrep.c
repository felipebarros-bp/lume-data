/* SPDX-License-Identifier: AGPL-3.0-only */
/* Crash reports with consent: see crashrep.h. */
#include "crashrep.h"
#include "http.h"
#include "log.h"
#include "panels.h"
#include "telemetry.h"
#include "toolbar.h"
#include <ctype.h>
#include <pthread.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

/* GlitchTip project "Lume Data": the client key is public by design (it only names where events go) */
#define STORE_URL "https://app.glitchtip.com/api/29464/store/"
#define SENTRY_KEY "dc45b21dcb0346d7beadaccd176aad68"

static const char *store_url(void) {
    const char *e = getenv("LUME_CRASH_URL");
    if (e && !strncmp(e, "http://127.0.0.1:", 17)) return e;   /* tests: a loopback mock only */
    if (getenv("LUME_SCRIPT")) return NULL;                      /* the harness never reaches the internet */
    return STORE_URL;
}

/* ---- the event ---- */
typedef struct { char *s; size_t len, cap; } Buf;
static void put(Buf *b, const char *t) {
    size_t k = strlen(t);
    if (b->len + k + 1 > b->cap) { b->cap = (b->len + k + 1) * 2; b->s = realloc(b->s, b->cap); }
    memcpy(b->s + b->len, t, k + 1); b->len += k;
}
static void put_str(Buf *b, const char *t) {   /* a JSON string, escaped */
    put(b, "\"");
    for (const unsigned char *p = (const unsigned char *)t; *p; p++) {
        char c[8];
        if (*p == '"' || *p == '\\') { c[0] = '\\'; c[1] = (char)*p; c[2] = 0; }
        else if (*p < 0x20) snprintf(c, sizeof c, "\\u%04x", *p);
        else { c[0] = (char)*p; c[1] = 0; }
        put(b, c);
    }
    put(b, "\"");
}
static const char *base_name(const char *p) { const char *s = strrchr(p, '/'); const char *w = strrchr(p, '\\'); if (w && (!s || w > s)) s = w; return s ? s + 1 : p; }
static const char *sig_name(int s) { return s == SIGSEGV ? "SIGSEGV" : s == SIGABRT ? "SIGABRT" : s == SIGILL ? "SIGILL" : s == SIGFPE ? "SIGFPE" : s == 10 || s == 7 ? "SIGBUS" : "signal"; }
static const char *sig_text(int s) { return s == SIGSEGV ? "segmentation fault" : s == SIGABRT ? "abort" : s == SIGILL ? "illegal instruction" : s == SIGFPE ? "arithmetic error" : "bus error"; }

/* one stack line from backtrace_symbols(_fd) into image / function / address
 *   macOS: "3   lume    0x0000000102f3a1c4 main + 120"
 *   glibc: "/path/to/lume(main+0x1c) [0x55d0c2a1b2c4]"  or  "/lib/x86_64-linux-gnu/libc.so.6(+0x29d90) [0x7f...]" */
static bool parse_frame(const char *ln, char *img, size_t in, char *fn, size_t fnn, char *addr, size_t an) {
    img[0] = fn[0] = addr[0] = 0;
    int idx; char im[512], ad[64]; int off = 0;
    if (sscanf(ln, "%d %511s %63s %n", &idx, im, ad, &off) == 3 && !strncmp(ad, "0x", 2)) {   /* macOS */
        snprintf(img, in, "%s", base_name(im)); snprintf(addr, an, "%s", ad);
        const char *f = ln + off; size_t k = strcspn(f, "+\n"); while (k && f[k - 1] == ' ') k--;
        snprintf(fn, fnn, "%.*s", (int)k, f);
        return true;
    }
    const char *lp = strchr(ln, '('), *rp = lp ? strchr(lp, ')') : NULL, *lb = strchr(ln, '[');
    if (lp && rp && lb) {   /* glibc */
        char path[512]; snprintf(path, sizeof path, "%.*s", (int)(lp - ln), ln);
        snprintf(img, in, "%s", base_name(path));
        size_t k = strcspn(lp + 1, "+)"); snprintf(fn, fnn, "%.*s", (int)k, lp + 1);
        size_t a = strcspn(lb + 1, "]"); snprintf(addr, an, "%.*s", (int)a, lb + 1);
        return true;
    }
    return false;
}

char *crashrep_event_json(void) {
    const char *path = log_crash_path();
    FILE *f = path[0] ? fopen(path, "r") : NULL;
    if (!f) return NULL;
    char version[64] = "", build[64] = "", channel[32] = ""; int sig = 0;
    char frames[64][3][256]; int nf = 0; bool in_stack = false; char line[1024];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (in_stack) { if (nf < 64 && parse_frame(line, frames[nf][0], 256, frames[nf][1], 256, frames[nf][2], 256)) nf++; continue; }
        if (!strncmp(line, "version=", 8)) snprintf(version, sizeof version, "%s", line + 8);
        else if (!strncmp(line, "build=", 6)) snprintf(build, sizeof build, "%s", line + 6);
        else if (!strncmp(line, "channel=", 8)) snprintf(channel, sizeof channel, "%s", line + 8);
        else if (!strncmp(line, "signal=", 7)) sig = atoi(line + 7);
        else if (!strcmp(line, "stack:")) in_stack = true;
    }
    fclose(f);
    const char *os, *ar; char osv[24]; telemetry_system(&os, osv, sizeof osv, &ar);
    unsigned char rb[16]; FILE *u = fopen("/dev/urandom", "rb"); size_t got = u ? fread(rb, 1, 16, u) : 0; if (u) fclose(u);
    for (size_t i = got; i < 16; i++) rb[i] = (unsigned char)(rand() ^ (int)time(NULL));
    char eid[33]; for (int i = 0; i < 16; i++) snprintf(eid + i * 2, 3, "%02x", rb[i]);
    time_t now = time(NULL); struct tm tm; gmtime_r(&now, &tm); char ts[32]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%SZ", &tm);
    char env[32]; size_t k = 0; for (const char *p = channel[0] ? channel : "development"; *p && k + 1 < sizeof env; p++) env[k++] = *p == ' ' ? '-' : (char)tolower((unsigned char)*p); env[k] = 0;
    char rel[160]; snprintf(rel, sizeof rel, "lume-data@%s%s%s", version[0] ? version : "0", build[0] ? "+" : "", build);
    Buf b = { 0 };
    put(&b, "{\"event_id\":"); put_str(&b, eid);
    put(&b, ",\"timestamp\":"); put_str(&b, ts);
    put(&b, ",\"platform\":\"native\",\"level\":\"fatal\",\"release\":"); put_str(&b, rel);
    put(&b, ",\"environment\":"); put_str(&b, env);
    put(&b, ",\"contexts\":{\"os\":{\"name\":"); put_str(&b, os); put(&b, ",\"version\":"); put_str(&b, osv);
    put(&b, "},\"device\":{\"arch\":"); put_str(&b, ar); put(&b, "}}");
    put(&b, ",\"tags\":{\"arch\":"); put_str(&b, ar); put(&b, "}");
    put(&b, ",\"exception\":{\"values\":[{\"type\":"); put_str(&b, sig_name(sig)); put(&b, ",\"value\":"); put_str(&b, sig_text(sig));
    put(&b, ",\"mechanism\":{\"type\":\"signalhandler\",\"handled\":false},\"stacktrace\":{\"frames\":[");
    for (int i = nf - 1, first = 1; i >= 0; i--, first = 0) {   /* Sentry lists the oldest frame first */
        if (!first) put(&b, ",");
        put(&b, "{\"function\":"); put_str(&b, frames[i][1][0] ? frames[i][1] : "?");
        put(&b, ",\"package\":"); put_str(&b, frames[i][0]);
        put(&b, ",\"instruction_addr\":"); put_str(&b, frames[i][2]); put(&b, "}");
    }
    put(&b, "]}}]}}");
    return b.s;
}

/* ---- sending ---- */
static void *send_th(void *p) {
    char *body = p, *out = NULL, err[200]; int st = 0;
    const char *url = store_url();
    if (url && http_request("POST", url, "Content-Type: application/json\nX-Sentry-Auth: Sentry sentry_version=7, sentry_client=lume-c/1, sentry_key=" SENTRY_KEY,
                            body, strlen(body), 20, false, NULL, NULL, NULL, &out, &st, err, sizeof err)) free(out);
    free(body);
    return NULL;
}
static void drop(void) { const char *p = log_crash_path(); if (p[0]) remove(p); }
static void send_and_drop(void) {
    char *ev = crashrep_event_json(); drop();
    if (!ev) return;
    if (!store_url()) { free(ev); return; }
    pthread_t t; if (pthread_create(&t, NULL, send_th, ev) == 0) pthread_detach(t); else free(ev);
}
static void ask(App *a) {
    char body[600];
    snprintf(body, sizeof body, "Lume Data closed unexpectedly the last time it ran.\n\nSend an anonymous crash report? It holds the version, the operating system, the signal and the function names where it stopped. No SQL, data, names, paths or user information. View report shows exactly what would be sent.");
    toolbar_crash_prompt(a->toolbar, a, body);
}

void crashrep_startup(App *a) {
    const char *p = log_crash_path();
    FILE *f = p[0] ? fopen(p, "r") : NULL;
    if (!f) return;
    fclose(f);
    LOG_I("crash report from the previous run found (crash reports: %s)", a->crash_reports == CRASH_ALWAYS ? "always send" : a->crash_reports == CRASH_NEVER ? "never send" : "ask");
    if (a->crash_reports == CRASH_NEVER) drop();
    else if (a->crash_reports == CRASH_ALWAYS) send_and_drop();
    else ask(a);
}
void crashrep_answer(App *a, int choice) {
    if (choice == 0) { send_and_drop(); app_status(a, "Crash report sent: thank you"); }
    else if (choice == 1) {
        char *ev = crashrep_event_json();
        if (ev) { panels_new_editor_tab(a->panels, a, ev); free(ev); }
        ask(a);   /* the decision is still open */
    }
    else { drop(); app_status(a, "Crash report deleted, not sent"); }
}
