/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * http.h on macOS and Linux: the system's libcurl, loaded with dlopen (macOS ships it in /usr/lib; Linux
 * distributions ship libcurl.so.4). No curl headers are needed to build: the few option numbers used
 * are part of libcurl's stable ABI.
 */
#ifndef _WIN32
#include "http.h"
#include <dlfcn.h>
#include <pthread.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

typedef void CURL;
struct curl_slist;
typedef int CURLcode;
enum {   /* libcurl option and info numbers (stable ABI) */
    O_WRITEDATA = 10001, O_URL = 10002, O_ERRORBUFFER = 10010, O_POSTFIELDS = 10015, O_HTTPHEADER = 10023,
    O_CUSTOMREQUEST = 10036, O_XFERINFODATA = 10057, O_WRITEFUNCTION = 20011, O_XFERINFOFUNCTION = 20219,
    O_POSTFIELDSIZE_LARGE = 30120, O_TIMEOUT = 13, O_NOPROGRESS = 43, O_FOLLOWLOCATION = 52, O_MAXREDIRS = 68,
    O_CONNECTTIMEOUT = 78, O_NOSIGNAL = 99, O_PROTOCOLS = 181, O_REDIR_PROTOCOLS = 182,
    I_RESPONSE_CODE = 0x200002, PROTO_HTTP_S = 3, GLOBAL_DEFAULT = 3, E_ABORTED = 42, ERRBUF = 256
};
static struct {
    CURLcode (*global_init)(long);
    CURL *(*easy_init)(void);
    CURLcode (*easy_setopt)(CURL *, int, ...);
    CURLcode (*easy_perform)(CURL *);
    CURLcode (*easy_getinfo)(CURL *, int, ...);
    void (*easy_cleanup)(CURL *);
    struct curl_slist *(*slist_append)(struct curl_slist *, const char *);
    void (*slist_free_all)(struct curl_slist *);
    const char *(*easy_strerror)(CURLcode);
    bool ok; char why[200];
} L;
static pthread_once_t once = PTHREAD_ONCE_INIT;
static void load(void) {
    static const char *const names[] = {
#ifdef __APPLE__
        "/usr/lib/libcurl.4.dylib",
#else
        "libcurl.so.4", "libcurl-gnutls.so.4", "libcurl.so",
#endif
        NULL };
    void *h = NULL;
    for (int i = 0; names[i] && !h; i++) h = dlopen(names[i], RTLD_NOW | RTLD_LOCAL);
    if (!h) { snprintf(L.why, sizeof L.why, "The system's libcurl is not installed (needed for Lume Intelligence; install the libcurl4 package)"); return; }
#define SYM(f, n) *(void **)&L.f = dlsym(h, n)
    SYM(global_init, "curl_global_init"); SYM(easy_init, "curl_easy_init"); SYM(easy_setopt, "curl_easy_setopt");
    SYM(easy_perform, "curl_easy_perform"); SYM(easy_getinfo, "curl_easy_getinfo"); SYM(easy_cleanup, "curl_easy_cleanup");
    SYM(slist_append, "curl_slist_append"); SYM(slist_free_all, "curl_slist_free_all"); SYM(easy_strerror, "curl_easy_strerror");
#undef SYM
    if (!L.global_init || !L.easy_init || !L.easy_setopt || !L.easy_perform || !L.easy_getinfo || !L.easy_cleanup || !L.slist_append || !L.slist_free_all || !L.easy_strerror) {
        snprintf(L.why, sizeof L.why, "The system's libcurl is incomplete"); return;
    }
    L.global_init(GLOBAL_DEFAULT);   /* once, before any thread uses it */
    L.ok = true;
}
bool http_available(char *why, size_t n) {
    pthread_once(&once, load);
    if (!L.ok && why && n) snprintf(why, n, "%s", L.why);
    return L.ok;
}

typedef struct { char *buf; size_t len, cap; HttpChunkFn chunk; void *ud; volatile int *cancel; } Sink;
static size_t on_write(char *p, size_t sz, size_t nm, void *ud) {
    Sink *s = ud; size_t n = sz * nm;
    if (s->cancel && *s->cancel) return 0;   /* stops the transfer */
    if (s->len + n + 1 > s->cap) { size_t nc = (s->len + n + 1) * 2; char *b = realloc(s->buf, nc); if (!b) return 0; s->buf = b; s->cap = nc; }
    memcpy(s->buf + s->len, p, n); s->len += n; s->buf[s->len] = 0;
    if (s->chunk) s->chunk(s->ud, p, n);
    return n;
}
static int on_progress(void *ud, int64_t dt, int64_t dn, int64_t ut, int64_t un) {
    (void)dt; (void)dn; (void)ut; (void)un;
    Sink *s = ud; return s->cancel && *s->cancel ? 1 : 0;
}

bool http_request(const char *method, const char *url, const char *headers, const char *body, size_t len,
                  int timeout_s, bool follow, volatile int *cancel, HttpChunkFn chunk, void *ud,
                  char **out, int *status, char *err, size_t en) {
    *out = NULL; *status = 0;
    if (!http_available(err, en)) return false;
    if (strncmp(url, "https://", 8) && strncmp(url, "http://", 7)) { snprintf(err, en, "Only http and https addresses are used"); return false; }
    CURL *c = L.easy_init();
    if (!c) { snprintf(err, en, "Could not start the request"); return false; }
    struct curl_slist *hl = NULL;
    if (headers) {   /* one header per line */
        for (const char *p = headers; *p;) {
            const char *nl = strchr(p, '\n'); size_t k = nl ? (size_t)(nl - p) : strlen(p);
            if (k) { char *line = malloc(k + 1); memcpy(line, p, k); line[k] = 0; if (line[k - 1] == '\r') line[k - 1] = 0; hl = L.slist_append(hl, line); memset(line, 0, k); free(line); }
            p += k; if (*p) p++;
        }
    }
    Sink s = { NULL, 0, 0, chunk, ud, cancel };
    char ebuf[ERRBUF] = "";
    L.easy_setopt(c, O_URL, url);
    L.easy_setopt(c, O_NOSIGNAL, 1L);
    L.easy_setopt(c, O_PROTOCOLS, (long)PROTO_HTTP_S);
    L.easy_setopt(c, O_REDIR_PROTOCOLS, (long)PROTO_HTTP_S);
    L.easy_setopt(c, O_FOLLOWLOCATION, follow ? 1L : 0L);
    if (follow) L.easy_setopt(c, O_MAXREDIRS, 5L);
    L.easy_setopt(c, O_TIMEOUT, (long)(timeout_s > 0 ? timeout_s : 60));
    L.easy_setopt(c, O_CONNECTTIMEOUT, 15L);
    L.easy_setopt(c, O_ERRORBUFFER, ebuf);
    L.easy_setopt(c, O_WRITEFUNCTION, on_write);
    L.easy_setopt(c, O_WRITEDATA, &s);
    L.easy_setopt(c, O_NOPROGRESS, 0L);
    L.easy_setopt(c, O_XFERINFOFUNCTION, on_progress);
    L.easy_setopt(c, O_XFERINFODATA, &s);
    if (hl) L.easy_setopt(c, O_HTTPHEADER, hl);
    if (body) { L.easy_setopt(c, O_POSTFIELDSIZE_LARGE, (int64_t)len); L.easy_setopt(c, O_POSTFIELDS, body); }
    if (method && strcmp(method, body ? "POST" : "GET")) L.easy_setopt(c, O_CUSTOMREQUEST, method);
    CURLcode rc = L.easy_perform(c);
    long code = 0; L.easy_getinfo(c, I_RESPONSE_CODE, &code);
    L.easy_cleanup(c);
    if (hl) L.slist_free_all(hl);
    if (cancel && *cancel) { free(s.buf); snprintf(err, en, "Cancelled"); return false; }
    if (rc != 0 && rc != E_ABORTED) { free(s.buf); snprintf(err, en, "%s", ebuf[0] ? ebuf : L.easy_strerror(rc)); return false; }
    if (!s.buf) { s.buf = calloc(1, 1); }
    *out = s.buf; *status = (int)code;
    return true;
}
#else
typedef int lume_http_curl_unused;   /* ISO C: a translation unit is never empty */
#endif
