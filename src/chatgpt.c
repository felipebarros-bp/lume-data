/* SPDX-License-Identifier: AGPL-3.0-only */
/* "Continue with ChatGPT": see chatgpt.h. */
#include "chatgpt.h"
#include "ai.h"
#include "json.h"
#include "jw.h"
#include "secstore.h"
#include "sha256.h"
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <poll.h>
#include <pthread.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

#define ACC_REFRESH "chatgpt-refresh"   /* refresh token + email */
#define ACC_ACCESS  "chatgpt-access"    /* expiry + access token */
#define SCOPES "openid profile email offline_access resource.invoke chatgpt.tokens.use.direct"
#define RESOURCE "https://api.openai.com/v1"

static char g_prefs[1024];
static pthread_mutex_t g_refresh = PTHREAD_MUTEX_INITIALIZER;   /* refreshes are serialised: tokens rotate */
static int g_signed = -1;

static bool loop_url(const char *u) { return u && (!strncmp(u, "http://127.0.0.1:", 17) || !strncmp(u, "http://localhost:", 17)); }
const char *chatgpt_auth_base(void) { const char *e = getenv("LUME_CHATGPT_AUTH_BASE"); return loop_url(e) ? e : "https://auth.openai.com"; }
const char *openai_api_base(void) { const char *e = getenv("LUME_OPENAI_API_BASE"); return loop_url(e) ? e : "https://api.openai.com/v1"; }
static int port(void) { const char *e = getenv("LUME_CHATGPT_PORT"); int p = e ? atoi(e) : 0; return p > 1024 && p < 65536 ? p : 1455; }
static void redirect_uri(char *out, size_t n) { snprintf(out, n, "http://127.0.0.1:%d/auth/callback", port()); }   /* never "localhost" */
static void wipe(void *p, size_t n) { volatile unsigned char *q = p; while (n--) *q++ = 0; }

/* ---- small helpers ---- */
static void rnd(unsigned char *b, size_t n) {
    int fd = open("/dev/urandom", O_RDONLY);
    size_t w = 0;
    if (fd >= 0) { while (w < n) { ssize_t r = read(fd, b + w, n - w); if (r <= 0) break; w += (size_t)r; } close(fd); }
    for (; w < n; w++) b[w] = (unsigned char)(rand() ^ (int)time(NULL));   /* never reached on a sane system */
}
static void b64url(const unsigned char *p, size_t n, char *out) {
    static const char A[] = "ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789-_";
    size_t w = 0;
    for (size_t i = 0; i < n; i += 3) {
        unsigned v = (unsigned)p[i] << 16 | (i + 1 < n ? (unsigned)p[i + 1] << 8 : 0) | (i + 2 < n ? p[i + 2] : 0);
        out[w++] = A[v >> 18 & 63]; out[w++] = A[v >> 12 & 63];
        if (i + 1 < n) out[w++] = A[v >> 6 & 63];
        if (i + 2 < n) out[w++] = A[v & 63];
    }
    out[w] = 0;
}
static int b64v(int c) { return c >= 'A' && c <= 'Z' ? c - 'A' : c >= 'a' && c <= 'z' ? c - 'a' + 26 : c >= '0' && c <= '9' ? c - '0' + 52 : c == '-' || c == '+' ? 62 : c == '_' || c == '/' ? 63 : -1; }
static size_t b64url_dec(const char *s, size_t n, char *out, size_t cap) {
    unsigned v = 0; int bits = 0; size_t w = 0;
    for (size_t i = 0; i < n && w + 1 < cap; i++) { int d = b64v((unsigned char)s[i]); if (d < 0) continue; v = v << 6 | (unsigned)d; bits += 6; if (bits >= 8) { bits -= 8; out[w++] = (char)(v >> bits & 255); } }
    out[w] = 0; return w;
}
static void rand_token(char *out, size_t bytes) { unsigned char b[64]; rnd(b, bytes); b64url(b, bytes, out); wipe(b, sizeof b); }
static void urlenc(char **b, size_t *w, size_t *cap, const char *s) {
    for (; *s; s++) {
        if (*w + 4 >= *cap) { *cap *= 2; *b = realloc(*b, *cap); }
        unsigned char c = (unsigned char)*s;
        if ((c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') || (c >= '0' && c <= '9') || c == '-' || c == '_' || c == '.' || c == '~') (*b)[(*w)++] = (char)c;
        else *w += (size_t)snprintf(*b + *w, 4, "%%%02X", c);
    }
    (*b)[*w] = 0;
}
static void param(char **b, size_t *w, size_t *cap, const char *k, const char *v) {
    if (*w) { if (*w + 2 >= *cap) { *cap *= 2; *b = realloc(*b, *cap); } (*b)[(*w)++] = '&'; (*b)[*w] = 0; }
    urlenc(b, w, cap, k);
    if (*w + 2 >= *cap) { *cap *= 2; *b = realloc(*b, *cap); }
    (*b)[(*w)++] = '='; (*b)[*w] = 0;
    urlenc(b, w, cap, v);
}
static int hexv(int c) { return c >= '0' && c <= '9' ? c - '0' : c >= 'a' && c <= 'f' ? c - 'a' + 10 : c >= 'A' && c <= 'F' ? c - 'A' + 10 : -1; }
/* value of k in a query string (decoded) */
static bool qget(const char *q, const char *k, char *out, size_t n) {
    size_t kl = strlen(k);
    for (const char *p = q; p && *p;) {
        const char *amp = strchr(p, '&'); size_t L = amp ? (size_t)(amp - p) : strlen(p);
        if (L > kl && !strncmp(p, k, kl) && p[kl] == '=') {
            size_t w = 0;
            for (size_t i = kl + 1; i < L && w + 1 < n; i++) {
                if (p[i] == '%' && i + 2 < L && hexv(p[i + 1]) >= 0 && hexv(p[i + 2]) >= 0) { out[w++] = (char)(hexv(p[i + 1]) * 16 + hexv(p[i + 2])); i += 2; }
                else out[w++] = p[i] == '+' ? ' ' : p[i];
            }
            out[w] = 0; return true;
        }
        p = amp ? amp + 1 : NULL;
    }
    out[0] = 0; return false;
}

/* ---- non-secret registration state: <prefs>/chatgpt.txt (client id issued by registration, host id) ---- */
static void reg_load(char *client, size_t cn, char *host, size_t hn) {
    client[0] = host[0] = 0;
    char p[1200]; snprintf(p, sizeof p, "%s/chatgpt.txt", g_prefs);
    FILE *f = g_prefs[0] ? fopen(p, "r") : NULL;
    if (f) {
        char line[512];
        while (fgets(line, sizeof line, f)) {
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "client_id=", 10)) snprintf(client, cn, "%s", line + 10);
            else if (!strncmp(line, "host_id=", 8)) snprintf(host, hn, "%s", line + 8);
        }
        fclose(f);
    }
}
static void reg_save(const char *client, const char *host) {
    if (!g_prefs[0]) return;
    char p[1200], t[1210]; snprintf(p, sizeof p, "%s/chatgpt.txt", g_prefs); snprintf(t, sizeof t, "%s.tmp", p);
    FILE *f = fopen(t, "w"); if (!f) return;
    fprintf(f, "# Sign in with ChatGPT: this app's registration (no secrets; the session is in the OS secret store)\nclient_id=%s\nhost_id=%s\n", client, host);
    fclose(f); rename(t, p);
}
static void host_id(char *out, size_t n) {
    char client[128], host[128]; reg_load(client, sizeof client, host, sizeof host);
    if (!host[0]) {
        unsigned char b[16]; rnd(b, 16); b[6] = (b[6] & 0x0F) | 0x40; b[8] = (b[8] & 0x3F) | 0x80;
        snprintf(host, sizeof host, "urn:uuid:%02x%02x%02x%02x-%02x%02x-%02x%02x-%02x%02x-%02x%02x%02x%02x%02x%02x",
                 b[0], b[1], b[2], b[3], b[4], b[5], b[6], b[7], b[8], b[9], b[10], b[11], b[12], b[13], b[14], b[15]);
        reg_save(client, host);
    }
    snprintf(out, n, "%s", host);
}

void chatgpt_init(const char *prefs_dir) { snprintf(g_prefs, sizeof g_prefs, "%s", prefs_dir ? prefs_dir : ""); g_signed = -1; }
void chatgpt_changed(void) { g_signed = -1; }

/* ---- the session ---- */
static bool store_tokens(const char *access, long long expires_at, const char *refresh, const char *email, char *why, int wn) {
    if (refresh) {
        size_t n = strlen(refresh) + (email ? strlen(email) : 0) + 4; char *s = malloc(n);
        snprintf(s, n, "%s\n%s", refresh, email ? email : "");
        bool ok = secstore_set(AI_KEY_SERVICE, ACC_REFRESH, s, why, wn);
        wipe(s, n); free(s);
        if (!ok) return false;
    }
    size_t n = strlen(access) + 32; char *s = malloc(n);
    snprintf(s, n, "%lld\n%s", expires_at, access);
    bool ok = secstore_set(AI_KEY_SERVICE, ACC_ACCESS, s, why, wn);
    wipe(s, n); free(s);
    g_signed = -1;
    return ok;
}
static char g_email[160];
bool chatgpt_signed_in(char *email, int en) {
    if (g_signed < 0) {   /* read once from the store; chatgpt_changed / sign-in / sign-out reset it */
        char *s = secstore_get(AI_KEY_SERVICE, ACC_REFRESH);
        g_signed = s && *s && s[0] != '\n';
        g_email[0] = 0;
        if (g_signed) { const char *nl = strchr(s, '\n'); snprintf(g_email, sizeof g_email, "%s", nl ? nl + 1 : ""); }
        secstore_free(s);
    }
    if (email && en) snprintf(email, (size_t)en, "%s", g_signed ? g_email : "");
    return g_signed == 1;
}
void chatgpt_sign_out(void) {
    secstore_delete(AI_KEY_SERVICE, ACC_ACCESS);
    secstore_delete(AI_KEY_SERVICE, ACC_REFRESH);
    g_signed = 0;
}
static char *email_of(const char *id_token) {
    if (!id_token) return NULL;
    const char *a = strchr(id_token, '.'); if (!a) return NULL;
    const char *b = strchr(a + 1, '.'); if (!b) return NULL;
    size_t L = (size_t)(b - a - 1); char *pl = malloc(L + 4);
    size_t n = b64url_dec(a + 1, L, pl, L + 4);
    Json j; memset(&j, 0, sizeof j); char *e = NULL;
    if (json_parse(&j, pl, n)) e = jr_strdup(&j, 0, "email");
    json_free(&j); free(pl);
    return e;
}
/* POST to the token endpoint; on success stores the session. err_code: the OAuth error */
static bool token_call(const char *form, char *err_code, size_t ecn, char *why, int wn) {
    char url[400]; snprintf(url, sizeof url, "%s/api/accounts/oauth/token", chatgpt_auth_base());
    char *out = NULL; int http = 0;
    if (err_code) err_code[0] = 0;
    if (!ai_http("POST", url, "content-type: application/x-www-form-urlencoded\naccept: application/json\n", form, 30, &out, &http, why, wn)) return false;
    Json j; memset(&j, 0, sizeof j);
    bool ok = false;
    if (!json_parse(&j, out, strlen(out))) snprintf(why, (size_t)wn, "The sign-in answer is not JSON (HTTP %d)", http);
    else if (http >= 400 || jr_get(&j, 0, "error") >= 0) {
        char code[96] = "", desc[200] = "";
        jr_str(&j, 0, "error", code, sizeof code); jr_str(&j, 0, "error_description", desc, sizeof desc);
        if (err_code) snprintf(err_code, ecn, "%s", code);
        snprintf(why, (size_t)wn, "ChatGPT sign-in was refused (%s%s%s)", code[0] ? code : "HTTP error", desc[0] ? ": " : "", desc);
    } else {
        char *access = jr_strdup(&j, 0, "access_token"), *refresh = jr_strdup(&j, 0, "refresh_token"), *idt = jr_strdup(&j, 0, "id_token");
        char scope[400] = ""; jr_str(&j, 0, "scope", scope, sizeof scope);
        long long exp = 3600; jr_int(&j, 0, "expires_in", &exp);
        if (!access) snprintf(why, (size_t)wn, "The sign-in answer has no access token");
        else if (scope[0] && !strstr(scope, "chatgpt.tokens.use.direct")) snprintf(why, (size_t)wn, "Plan usage was not granted for Lume (the sign-in did not include it)");
        else {
            char *email = email_of(idt);
            char oldmail[160] = "";
            if (!email) { g_signed = -1; chatgpt_signed_in(oldmail, sizeof oldmail); }   /* a refresh may carry no id token */
            ok = store_tokens(access, (long long)time(NULL) + exp, refresh, email ? email : oldmail, why, wn);
            free(email);
        }
        if (access) secstore_free(access);
        if (refresh) secstore_free(refresh);
        if (idt) secstore_free(idt);
    }
    json_free(&j);
    wipe(out, strlen(out)); free(out);
    return ok;
}
bool chatgpt_access_token(char **token, char *why, int wn) {
    *token = NULL;
    pthread_mutex_lock(&g_refresh);
    char *s = secstore_get(AI_KEY_SERVICE, ACC_ACCESS);
    long long exp = s ? atoll(s) : 0;
    const char *nl = s ? strchr(s, '\n') : NULL;
    if (s && nl && exp - (long long)time(NULL) > 120) { *token = strdup(nl + 1); secstore_free(s); pthread_mutex_unlock(&g_refresh); return true; }
    secstore_free(s);
    /* refresh: the refresh token rotates, so the replacement is stored before anything else */
    char *r = secstore_get(AI_KEY_SERVICE, ACC_REFRESH);
    if (!r || !*r || r[0] == '\n') { secstore_free(r); snprintf(why, (size_t)wn, "Not signed in to ChatGPT: Settings > Continue with ChatGPT"); pthread_mutex_unlock(&g_refresh); return false; }
    char *rnl = strchr(r, '\n'); if (rnl) *rnl = 0;
    char client[128], host[128]; reg_load(client, sizeof client, host, sizeof host);
    size_t cap = 512, w = 0; char *form = malloc(cap); form[0] = 0;
    param(&form, &w, &cap, "grant_type", "refresh_token");
    param(&form, &w, &cap, "client_id", client[0] ? client : "dynamic_agent_client");
    param(&form, &w, &cap, "refresh_token", r);
    param(&form, &w, &cap, "resource", RESOURCE);
    secstore_free(r);
    char code[96];
    bool ok = token_call(form, code, sizeof code, why, wn);
    wipe(form, w); free(form);
    if (!ok && (!strcmp(code, "invalid_grant") || !strcmp(code, "refresh_token_reused") || !strcmp(code, "refresh_token_expired") || !strcmp(code, "refresh_token_invalidated") || !strcmp(code, "invalid_client"))) {
        chatgpt_sign_out();
        snprintf(why, (size_t)wn, "The ChatGPT session ended (%s): Settings > Continue with ChatGPT to sign in again", code);
    }
    if (ok) {
        s = secstore_get(AI_KEY_SERVICE, ACC_ACCESS);
        nl = s ? strchr(s, '\n') : NULL;
        if (nl) *token = strdup(nl + 1); else { ok = false; snprintf(why, (size_t)wn, "The refreshed session could not be read back from the %s", secstore_name()); }
        secstore_free(s);
    }
    pthread_mutex_unlock(&g_refresh);
    return ok;
}

/* ---- sign-in: the loopback listener ---- */
static struct {
    pthread_mutex_t mu;
    pthread_t th; bool running;
    int state; char msg[300];
    int lfd; volatile int cancel;
    char verifier[64], st[48], client_sent[128];
} S = { .mu = PTHREAD_MUTEX_INITIALIZER, .lfd = -1 };

static void set_state(int st, const char *msg) { pthread_mutex_lock(&S.mu); S.state = st; snprintf(S.msg, sizeof S.msg, "%s", msg ? msg : ""); pthread_mutex_unlock(&S.mu); }
static void reply(int c, int code, const char *title, const char *text) {
    char body[1200];
    int bl = snprintf(body, sizeof body, "<!doctype html><meta charset=utf-8><title>Lume</title><body style=\"font:16px -apple-system,system-ui,sans-serif;margin:15vh auto;max-width:30em;text-align:center\"><h2>%s</h2><p>%s</p></body>", title, text);
    char h[300]; int hl = snprintf(h, sizeof h, "HTTP/1.1 %d %s\r\nContent-Type: text/html; charset=utf-8\r\nContent-Length: %d\r\nCache-Control: no-store\r\nConnection: close\r\n\r\n", code, code == 200 ? "OK" : code == 404 ? "Not Found" : "Bad Request", bl);
    (void)!write(c, h, (size_t)hl); (void)!write(c, body, (size_t)bl);
}
static void *listen_th(void *arg) {
    (void)arg;
    time_t deadline = time(NULL) + 600;   /* ten minutes to finish in the browser */
    for (;;) {
        if (S.cancel) { set_state(CS_FAILED, "Sign-in cancelled"); break; }
        if (time(NULL) > deadline) { set_state(CS_FAILED, "Sign-in timed out: try Continue with ChatGPT again"); break; }
        struct pollfd pf = { S.lfd, POLLIN, 0 };
        if (poll(&pf, 1, 200) <= 0) continue;
        int c = accept(S.lfd, NULL, NULL);
        if (c < 0) continue;
        struct timeval tv = { 5, 0 }; setsockopt(c, SOL_SOCKET, SO_RCVTIMEO, &tv, sizeof tv);
        char req[8192]; size_t w = 0; ssize_t r;
        while (w + 1 < sizeof req && (r = read(c, req + w, sizeof req - 1 - w)) > 0) { w += (size_t)r; req[w] = 0; if (strstr(req, "\r\n\r\n")) break; }
        req[w] = 0;
        char path[4096] = ""; sscanf(req, "GET %4095s", path);
        if (strncmp(path, "/auth/callback", 14)) { reply(c, 404, "Not found", ""); close(c); continue; }
        const char *q = strchr(path, '?'); q = q ? q + 1 : "";
        char code[2048], state[128], client[128], err[96], edesc[300];
        qget(q, "code", code, sizeof code); qget(q, "state", state, sizeof state); qget(q, "client_id", client, sizeof client);
        qget(q, "error", err, sizeof err); qget(q, "error_description", edesc, sizeof edesc);
        if (strcmp(state, S.st)) { reply(c, 400, "Sign-in not recognised", "This answer does not belong to the sign-in Lume started. Close this tab and try again."); close(c); set_state(CS_FAILED, "The sign-in answer did not match (state): try again"); break; }
        if (err[0] || !code[0]) {
            reply(c, 400, "Sign-in not completed", "You can close this tab and return to Lume.");
            close(c);
            char m[400]; snprintf(m, sizeof m, "ChatGPT sign-in was not completed%s%s%s%s", err[0] ? " (" : "", err, edesc[0] ? ": " : "", err[0] ? edesc : "");
            if (err[0]) strcat(m, ")");
            set_state(CS_FAILED, m); break;
        }
        /* the issued registration: kept and reused; a different one on a re-sign-in is refused */
        char saved[128], host[128]; reg_load(saved, sizeof saved, host, sizeof host);
        if (saved[0] && client[0] && strcmp(saved, client)) { reply(c, 400, "Sign-in not recognised", "Close this tab and try again."); close(c); set_state(CS_FAILED, "ChatGPT returned a different app registration: sign-in refused"); wipe(code, sizeof code); break; }
        reply(c, 200, "You're signed in", "Lume can now use your ChatGPT plan. You can close this tab and return to Lume.");
        close(c);
        set_state(CS_EXCHANGING, "Finishing sign-in…");
        const char *cid = client[0] ? client : saved[0] ? saved : S.client_sent;
        if (client[0] && !saved[0]) reg_save(client, host);
        char ru[128]; redirect_uri(ru, sizeof ru);
        size_t cap = 1024, fw = 0; char *form = malloc(cap); form[0] = 0;
        param(&form, &fw, &cap, "grant_type", "authorization_code");
        param(&form, &fw, &cap, "client_id", cid);
        param(&form, &fw, &cap, "code", code);
        param(&form, &fw, &cap, "code_verifier", S.verifier);
        param(&form, &fw, &cap, "redirect_uri", ru);
        param(&form, &fw, &cap, "resource", RESOURCE);
        wipe(code, sizeof code);
        char why[400] = "", ec[96];
        bool ok = token_call(form, ec, sizeof ec, why, sizeof why);
        wipe(form, fw); free(form);
        if (ok) { char email[160]; chatgpt_signed_in(email, sizeof email); char m[300]; snprintf(m, sizeof m, "Signed in%s%s", email[0] ? " as " : "", email); set_state(CS_DONE, m); }
        else set_state(CS_FAILED, why);
        break;
    }
    close(S.lfd); S.lfd = -1;
    wipe(S.verifier, sizeof S.verifier);
    return NULL;
}
static void join_old(void) { if (S.running) { S.cancel = 1; pthread_join(S.th, NULL); S.running = false; } }
bool chatgpt_signin_begin(char *url, int un, char *why, int wn) {
    join_old();
    if (access("/usr/bin/curl", X_OK)) { snprintf(why, (size_t)wn, "/usr/bin/curl is not available"); return false; }
    char sw[200]; if (!secstore_available(sw, sizeof sw)) { snprintf(why, (size_t)wn, "%s", sw); return false; }
    int fd = socket(AF_INET, SOCK_STREAM, 0);
    if (fd < 0) { snprintf(why, (size_t)wn, "socket: %s", strerror(errno)); return false; }
    int one = 1; setsockopt(fd, SOL_SOCKET, SO_REUSEADDR, &one, sizeof one);
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_port = htons((uint16_t)port()); a.sin_addr.s_addr = htonl(INADDR_LOOPBACK);
    if (bind(fd, (struct sockaddr *)&a, sizeof a) || listen(fd, 4)) {
        snprintf(why, (size_t)wn, "Port %d on this machine is in use (another sign-in, or Codex, may be waiting there): close it and try again", port());
        close(fd); return false;
    }
    S.lfd = fd; S.cancel = 0;
    /* PKCE (S256), state and nonce */
    rand_token(S.verifier, 48); rand_token(S.st, 24);
    char nonce[48]; rand_token(nonce, 24);
    char hex[65]; sha256_hex(S.verifier, strlen(S.verifier), hex);
    unsigned char dig[32]; for (int i = 0; i < 32; i++) dig[i] = (unsigned char)(hexv(hex[2 * i]) * 16 + hexv(hex[2 * i + 1]));
    char chal[64]; b64url(dig, 32, chal);
    char client[128], host[128]; reg_load(client, sizeof client, host, sizeof host);
    if (!host[0]) host_id(host, sizeof host);
    snprintf(S.client_sent, sizeof S.client_sent, "%s", client[0] ? client : "dynamic_agent_client");
    char ru[128]; redirect_uri(ru, sizeof ru);
    size_t cap = 1024, w = 0; char *q = malloc(cap); q[0] = 0;
    param(&q, &w, &cap, "client_id", S.client_sent);
    param(&q, &w, &cap, "agent_name_hint", "Lume");
    param(&q, &w, &cap, "ext_agent_host_id", host);
    param(&q, &w, &cap, "response_type", "code");
    param(&q, &w, &cap, "redirect_uri", ru);
    param(&q, &w, &cap, "scope", SCOPES);
    param(&q, &w, &cap, "resource", RESOURCE);
    param(&q, &w, &cap, "state", S.st);
    param(&q, &w, &cap, "nonce", nonce);
    param(&q, &w, &cap, "code_challenge_method", "S256");
    param(&q, &w, &cap, "code_challenge", chal);
    snprintf(url, (size_t)un, "%s/api/accounts/authorize?%s", chatgpt_auth_base(), q);
    free(q);
    set_state(CS_WAITING, "Waiting for you to sign in in the browser…");
    S.running = pthread_create(&S.th, NULL, listen_th, NULL) == 0;
    if (!S.running) { close(S.lfd); S.lfd = -1; set_state(CS_FAILED, "Could not start the sign-in listener"); snprintf(why, (size_t)wn, "Could not start the sign-in listener"); return false; }
    return true;
}
int chatgpt_signin_status(char *msg, int mn) {
    pthread_mutex_lock(&S.mu);
    int st = S.state; if (msg && mn) snprintf(msg, (size_t)mn, "%s", S.msg);
    pthread_mutex_unlock(&S.mu);
    if ((st == CS_DONE || st == CS_FAILED) && S.running) { pthread_join(S.th, NULL); S.running = false; }
    return st;
}
void chatgpt_signin_cancel(void) { join_old(); pthread_mutex_lock(&S.mu); if (S.state == CS_WAITING || S.state == CS_EXCHANGING) { S.state = CS_IDLE; S.msg[0] = 0; } pthread_mutex_unlock(&S.mu); }

static void *reap(void *p) { pid_t pid = (pid_t)(intptr_t)p; int st; waitpid(pid, &st, 0); return NULL; }
static void open_with(const char *url, bool test);
void chatgpt_open_browser(const char *url) { const char *tb = getenv("LUME_TEST_BROWSER"); open_with(url, tb && !strcmp(tb, "curl")); }
void chatgpt_open_url(const char *url) { if (getenv("LUME_SCRIPT")) return; open_with(url, false); }   /* tests never open a browser */
static void open_with(const char *url, bool test) {
    char *argv[10]; int n = 0;
    if (test) {   /* tests: follow the mock's redirect to the loopback callback */
        argv[n++] = "/usr/bin/curl"; argv[n++] = "-sS"; argv[n++] = "-L"; argv[n++] = "--max-time"; argv[n++] = "20"; argv[n++] = "-o"; argv[n++] = "/dev/null"; argv[n++] = (char *)url;
    }
#if defined(__APPLE__)
    else { argv[n++] = "/usr/bin/open"; argv[n++] = (char *)url; }
#else
    else { argv[n++] = "xdg-open"; argv[n++] = (char *)url; }
#endif
    argv[n] = NULL;
    extern char **environ;
    pid_t pid;
    if (posix_spawnp(&pid, argv[0], NULL, NULL, argv, environ) == 0) {
        pthread_t t; if (pthread_create(&t, NULL, reap, (void *)(intptr_t)pid) == 0) pthread_detach(t);
    }
}
