/* SPDX-License-Identifier: AGPL-3.0-only */
/* Update check against GitHub releases: see update.h. */
#include "update.h"
#include "http.h"
#include "json.h"
#include "log.h"
#include "product.h"
#include "toolbar.h"
#include <SDL3/SDL.h>
#include <ctype.h>
#include <pthread.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <time.h>

#ifndef LUME_VERSION
#define LUME_VERSION "0"
#endif

#define ENDPOINT "https://api.github.com/repos/tessarotto21/lume-data/releases?per_page=20"   /* newest first; drafts are not listed */
#define DAY (24 * 60 * 60)
#define RETRY (60 * 60)   /* after a failed automatic check: try again in an hour, not every frame */

enum { ST_IDLE, ST_RUNNING, ST_DONE };

static struct {
    char path[1200];          /* <prefs>/update.txt: last=<epoch>, skip=<version> */
    long long last;           /* when the last automatic check started (epoch s) */
    char skip[48];            /* the version the user chose to skip */
    SDL_AtomicInt state;
    bool manual;              /* this check was asked for: always say the outcome */
    bool shown;               /* the current offer is on screen */
    /* the answer (written by the thread before ST_DONE) */
    int http_status;
    char err[200];
    char tag[48], page[600], asset[600];
    char notes[1800];         /* what is new since this version: every newer release's notes, newest first, as plain text */
    int checks;               /* harness: requests started */
} U;

/* ---- versions ---- */
int update_version_cmp(const char *x, const char *y) {
    if (*x == 'v' || *x == 'V') x++;
    if (*y == 'v' || *y == 'V') y++;
    while (*x || *y) {
        long a = 0, b = 0;
        while (isdigit((unsigned char)*x)) a = a * 10 + (*x++ - '0');
        while (isdigit((unsigned char)*y)) b = b * 10 + (*y++ - '0');
        if (a != b) return a < b ? -1 : 1;
        /* a suffix ("-rc1") or the end: only dots continue the comparison */
        bool dx = *x == '.', dy = *y == '.';
        if (!dx && !dy) break;
        if (dx) x++;
        if (dy) y++;
    }
    return 0;
}

/* ---- the state file ---- */
static void state_save(void) {
    if (!U.path[0]) return;
    char tmp[1300]; snprintf(tmp, sizeof tmp, "%s.tmp", U.path);
    FILE *f = fopen(tmp, "w"); if (!f) return;
    fprintf(f, "last=%lld\n", U.last);
    if (U.skip[0]) fprintf(f, "skip=%s\n", U.skip);
    fclose(f); rename(tmp, U.path);
}
void update_init(App *a, const char *prefs_dir) {
    (void)a;
    snprintf(U.path, sizeof U.path, "%supdate.txt", prefs_dir ? prefs_dir : "");
    FILE *f = fopen(U.path, "r"); if (!f) return;
    char line[200];
    while (fgets(line, sizeof line, f)) {
        line[strcspn(line, "\r\n")] = 0;
        if (!strncmp(line, "last=", 5)) U.last = atoll(line + 5);
        else if (!strncmp(line, "skip=", 5)) snprintf(U.skip, sizeof U.skip, "%s", line + 5);
    }
    fclose(f);
}

/* ---- the request ---- */
static const char *endpoint(void) {
    const char *e = getenv("LUME_UPDATE_URL");
    if (e && !strncmp(e, "http://127.0.0.1:", 17)) return e;   /* tests: a loopback mock only */
    if (getenv("LUME_SCRIPT")) return NULL;                      /* the harness never reaches the internet */
    return ENDPOINT;
}
static bool ends_with(const char *s, const char *suf) { size_t n = strlen(s), m = strlen(suf); return n >= m && !strcmp(s + n - m, suf); }
/* the installer for this platform, by the names the release workflow gives them */
static bool asset_fits(const char *name) {
#if defined(__APPLE__)
    return ends_with(name, ".dmg");
#elif defined(_WIN32)
    return strstr(name, "windows") && ends_with(name, ".zip");
#else
#if defined(__aarch64__)
    return (strstr(name, "-linux-aarch64") || strstr(name, "-linux-arm64")) && ends_with(name, ".tar.gz");
#else
    return strstr(name, "-linux-x86_64") && ends_with(name, ".tar.gz");
#endif
#endif
}
static bool key_is(const Json *j, const JsonNode *n, const char *k) { return n->key_len == strlen(k) && !memcmp(j->src + n->key_off, k, n->key_len); }
static void str_of(const Json *j, const JsonNode *n, char *o, size_t m) { if (n->type == JN_STRING) json_unescape(j->src + n->val_off, n->val_len, o, m); else o[0] = 0; }

/* One line of a release's Markdown as plain text: headings lose their #, list items get a bullet,
 * emphasis and code marks go, [text](url) keeps the text. Images, HTML and rules are dropped (""). */
static void md_line(const char *in, size_t n, char *o, size_t m) {
    size_t k = 0;
    while (n && (*in == ' ' || *in == '\t')) { in++; n--; }
    o[0] = 0;
    if (!n || *in == '<' || (n >= 2 && in[0] == '!' && in[1] == '[') || (n >= 3 && (!strncmp(in, "---", 3) || !strncmp(in, "***", 3) || !strncmp(in, "___", 3)))) return;
    if (*in == '#') { while (n && *in == '#') { in++; n--; } while (n && *in == ' ') { in++; n--; } }
    else if (n >= 2 && (in[0] == '-' || in[0] == '*' || in[0] == '+') && in[1] == ' ') { in += 2; n -= 2; k += (size_t)snprintf(o, m, "\xE2\x80\xA2 "); }
    for (size_t i = 0; i < n && k + 4 < m; i++) {
        char c = in[i];
        if (c == '*' || c == '`' || (c == '_' && (i == 0 || in[i - 1] == ' ' || i + 1 == n || in[i + 1] == ' '))) continue;
        if (c == '[') {   /* [text](url) -> text */
            const char *cl = memchr(in + i, ']', n - i);
            if (cl && (size_t)(cl - in) + 1 < n && cl[1] == '(') {
                const char *pe = memchr(cl, ')', n - (size_t)(cl - in));
                if (pe) { size_t tl = (size_t)(cl - in) - i - 1; if (k + tl + 1 < m) { memcpy(o + k, in + i + 1, tl); k += tl; } i = (size_t)(pe - in); continue; }
            }
        }
        if (c != '\r') o[k++] = c;
    }
    o[k] = 0;
}
/* Appends a release's notes under its version (cut at the space left; the rest is in Release Notes). */
static void add_notes(const char *ver, const char *md) {
    size_t k = strlen(U.notes), m = sizeof U.notes;
    if (k + 40 >= m) return;
    k += (size_t)snprintf(U.notes + k, m - k, "%s%s\n", k ? "\n" : "", ver[0] == 'v' ? ver + 1 : ver);
    bool blank = true, any = false;
    for (const char *p = md; *p && k + 8 < m; ) {
        const char *e = strchr(p, '\n'); size_t n = e ? (size_t)(e - p) : strlen(p);
        char line[400]; md_line(p, n, line, sizeof line);
        size_t l = strlen(line);
        if (!l) { if (!blank && any) { k += (size_t)snprintf(U.notes + k, m - k, "\n"); blank = true; } }   /* paragraphs: one blank line */
        else {
            if (k + l + 2 >= m) { snprintf(U.notes + k, m - k, "\xE2\x80\xA6"); return; }   /* the rest: Release Notes */
            k += (size_t)snprintf(U.notes + k, m - k, "%s\n", line);
            blank = false; any = true;
        }
        p = e ? e + 1 : p + n;
    }
    if (!any) k += (size_t)snprintf(U.notes + k, m - k, "(no notes)\n");
    while (k > 1 && U.notes[k - 1] == '\n' && U.notes[k - 2] == '\n') U.notes[--k] = 0;   /* no blank line left at the end */
}
static void asset_of(const Json *j, int arr) {
    for (int k = j->nodes[arr].first_child; k >= 0 && !U.asset[0]; k = j->nodes[k].next_sibling) {
        if (j->nodes[k].type != JN_OBJECT) continue;
        char name[300] = "", url[600] = "";
        for (int m = j->nodes[k].first_child; m >= 0; m = j->nodes[m].next_sibling) {
            if (key_is(j, &j->nodes[m], "name")) str_of(j, &j->nodes[m], name, sizeof name);
            else if (key_is(j, &j->nodes[m], "browser_download_url")) str_of(j, &j->nodes[m], url, sizeof url);
        }
        if (url[0] && asset_fits(name)) snprintf(U.asset, sizeof U.asset, "%s", url);
    }
}
/* The releases list (newest first): the highest published version is the offer; the notes of every
 * version newer than this one are gathered, newest first. Drafts and pre-releases are ignored. */
static void parse_releases(const char *body) {
    Json j = {0};
    if (!json_parse(&j, body, strlen(body)) || !j.n || j.nodes[0].type != JN_ARRAY) { snprintf(U.err, sizeof U.err, "GitHub sent an answer Lume could not read"); json_free(&j); return; }
    int best = -1; char best_tag[48] = "";
    for (int r = j.nodes[0].first_child; r >= 0; r = j.nodes[r].next_sibling) {
        if (j.nodes[r].type != JN_OBJECT) continue;
        char tag[48] = ""; bool skip = false;
        for (int i = j.nodes[r].first_child; i >= 0; i = j.nodes[i].next_sibling) {
            const JsonNode *n = &j.nodes[i];
            if (key_is(&j, n, "tag_name")) str_of(&j, n, tag, sizeof tag);
            else if ((key_is(&j, n, "draft") || key_is(&j, n, "prerelease")) && n->type == JN_TRUE) skip = true;
        }
        if (skip || !tag[0]) continue;
        if (best < 0 || update_version_cmp(tag, best_tag) > 0) { best = r; snprintf(best_tag, sizeof best_tag, "%s", tag); }
    }
    if (best >= 0) {
        snprintf(U.tag, sizeof U.tag, "%s", best_tag);
        for (int i = j.nodes[best].first_child; i >= 0; i = j.nodes[i].next_sibling) {
            if (key_is(&j, &j.nodes[i], "html_url")) str_of(&j, &j.nodes[i], U.page, sizeof U.page);
            else if (key_is(&j, &j.nodes[i], "assets") && j.nodes[i].type == JN_ARRAY) asset_of(&j, i);
        }
        /* the notes, newest first (the list's order is by date, so sort by version here) */
        char done[20][48]; int nd = 0;
        for (int round = 0; round < 20; round++) {
            int pick = -1; char ptag[48] = "";
            for (int r = j.nodes[0].first_child; r >= 0; r = j.nodes[r].next_sibling) {
                if (j.nodes[r].type != JN_OBJECT) continue;
                char tag[48] = ""; bool skip = false;
                for (int i = j.nodes[r].first_child; i >= 0; i = j.nodes[i].next_sibling) {
                    if (key_is(&j, &j.nodes[i], "tag_name")) str_of(&j, &j.nodes[i], tag, sizeof tag);
                    else if ((key_is(&j, &j.nodes[i], "draft") || key_is(&j, &j.nodes[i], "prerelease")) && j.nodes[i].type == JN_TRUE) skip = true;
                }
                if (skip || !tag[0] || update_version_cmp(tag, LUME_VERSION) <= 0) continue;
                bool seen = false; for (int d = 0; d < nd; d++) if (!strcmp(done[d], tag)) seen = true;
                if (seen) continue;
                if (pick < 0 || update_version_cmp(tag, ptag) > 0) { pick = r; snprintf(ptag, sizeof ptag, "%s", tag); }
            }
            if (pick < 0) break;
            snprintf(done[nd++], 48, "%s", ptag);
            char *md = NULL;
            for (int i = j.nodes[pick].first_child; i >= 0; i = j.nodes[i].next_sibling)
                if (key_is(&j, &j.nodes[i], "body") && j.nodes[i].type == JN_STRING) { size_t cap = (size_t)j.nodes[i].val_len + 8;   /* json_unescape keeps room for its "…" */ md = malloc(cap); if (md) json_unescape(j.src + j.nodes[i].val_off, j.nodes[i].val_len, md, cap); }
            add_notes(ptag, md ? md : "");
            free(md);
        }
    }
    json_free(&j);
}
static void *check_th(void *p) {
    (void)p;
    char *out = NULL, err[200] = ""; int st = 0;
    const char *url = endpoint();
    if (!url) snprintf(U.err, sizeof U.err, "Update checks are off in this run");
    else if (!http_request("GET", url, "Accept: application/vnd.github+json\nUser-Agent: Lume Data", NULL, 0, 20, true, NULL, NULL, NULL, &out, &st, err, sizeof err))
        snprintf(U.err, sizeof U.err, "GitHub could not be reached: %s", err);
    else {
        U.http_status = st;
        if (st == 200) parse_releases(out);
        else if (st == 404) { /* no release published yet: nothing newer */ }
        else if (st == 403 || st == 429) snprintf(U.err, sizeof U.err, "GitHub asked to wait (too many requests). Try again later.");
        else snprintf(U.err, sizeof U.err, "GitHub answered with status %d", st);
    }
    free(out);
    SDL_SetAtomicInt(&U.state, ST_DONE);
    return NULL;
}
static void start(App *a, bool manual) {
    if (SDL_GetAtomicInt(&U.state) == ST_RUNNING) { if (manual) U.manual = true; return; }
    U.manual = manual; U.http_status = 0; U.err[0] = U.tag[0] = U.page[0] = U.asset[0] = U.notes[0] = 0; U.shown = false;
    U.checks++;
    SDL_SetAtomicInt(&U.state, ST_RUNNING);
    pthread_t t;
    if (pthread_create(&t, NULL, check_th, NULL) == 0) pthread_detach(t);
    else { snprintf(U.err, sizeof U.err, "The check could not start"); SDL_SetAtomicInt(&U.state, ST_DONE); }
    app_wake_in(a, 100);
}

/* ---- the outcome ---- */
static bool newer(void) { return U.tag[0] && update_version_cmp(U.tag, LUME_VERSION) > 0; }
static void finish(App *a) {
    bool ok = !U.err[0];
    LOG_I("update check (%s): %s", U.manual ? "asked" : "automatic", !ok ? U.err : newer() ? "a newer version is available" : "up to date");
    if (!ok && !U.manual) { U.last = (long long)time(NULL) - DAY + RETRY; state_save(); SDL_SetAtomicInt(&U.state, ST_IDLE); return; }
    if (ok && newer() && (U.manual || strcmp(U.skip, U.tag))) {
        const char *v = U.tag[0] == 'v' ? U.tag + 1 : U.tag;
        char title[120], body[2400];
        snprintf(title, sizeof title, "Lume Data %s is available", v);
        snprintf(body, sizeof body, "You have version %s. Download opens the %s in your browser; install it over this one, your connections and settings stay as they are.%s%s",
                 LUME_VERSION, U.asset[0] ? "installer for this computer" : "release page", U.notes[0] ? "\n\nWHAT'S NEW\n" : "", U.notes);
        if (!toolbar_update_prompt(a->toolbar, a, title, body, 0)) return;   /* another question is open: ask after it */
        U.shown = true;
    } else if (U.manual) {
        char title[120], body[400];
        if (ok) { snprintf(title, sizeof title, "Lume Data is up to date"); snprintf(body, sizeof body, "Version %s is the latest release.", LUME_VERSION); }
        else { snprintf(title, sizeof title, "Could not check for updates"); snprintf(body, sizeof body, "%s", U.err); }
        if (!toolbar_update_prompt(a->toolbar, a, title, body, 1)) return;
    }
    SDL_SetAtomicInt(&U.state, ST_IDLE);
}

void update_tick(App *a) {
    int st = SDL_GetAtomicInt(&U.state);
    if (st == ST_DONE) { finish(a); return; }
    if (st == ST_RUNNING) { app_wake_in(a, 200); return; }
    if (!a->update_check || !U.path[0]) return;
    long long now = (long long)time(NULL);
    if (now - U.last < DAY && U.last <= now) return;   /* a clock set back counts as due */
    if (!endpoint()) return;                             /* the harness without a mock: never */
    U.last = now; state_save();
    start(a, false);
}
void update_check_now(App *a) {
    app_status(a, "Checking for updates…");
    start(a, true);
}
void update_answer(App *a, int choice) {
    char why[200];
    if (!U.shown) return;   /* the "up to date" / error notice: nothing to do */
    U.shown = false;
    if (choice == 0) { if (url_open(U.asset[0] ? U.asset : U.page, why, sizeof why)) app_status(a, "Downloading Lume Data %s in the browser", U.tag); else app_status(a, "%s", why); }
    else if (choice == 1) { if (!url_open(U.page, why, sizeof why)) app_status(a, "%s", why); }
    else if (choice == 3) { snprintf(U.skip, sizeof U.skip, "%s", U.tag); state_save(); app_status(a, "Version %s skipped: Help > Check for Updates still offers it", U.tag); }
}

bool update_busy(void) { return SDL_GetAtomicInt(&U.state) == ST_RUNNING; }
void update_dump(void) {
    printf("updatestate state=%d checks=%d status=%d tag=\"%s\" newer=%d asset=\"%s\" skip=\"%s\" last=%lld err=\"%s\"\n",
           SDL_GetAtomicInt(&U.state), U.checks, U.http_status, U.tag, newer(), U.asset, U.skip, U.last, U.err);
    fflush(stdout);
}
