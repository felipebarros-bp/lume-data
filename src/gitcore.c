/* SPDX-License-Identifier: AGPL-3.0-only */
/* Lume Git Core: see gitcore.h. */
#include "gitcore.h"
#include <ctype.h>
#include <errno.h>
#include <fcntl.h>
#include <poll.h>
#include <signal.h>
#include <spawn.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <strings.h>
#include <sys/stat.h>
#include <sys/time.h>
#include <sys/wait.h>
#include <unistd.h>

extern char **environ;

static double now_ms(void) { struct timeval tv; gettimeofday(&tv, NULL); return tv.tv_sec * 1000.0 + tv.tv_usec / 1000.0; }

/* ---- secrets ---- */
void gitc_redact(const char *in, char *out, size_t n) {
    if (!n) return;
    size_t w = 0;
    for (size_t i = 0; in && in[i] && w + 1 < n;) {
        /* scheme://user[:password]@host becomes scheme://(masked)@host */
        if (!strncmp(in + i, "://", 3)) {
            size_t j = i + 3; while (in[j] && in[j] != '@' && in[j] != '/' && !isspace((unsigned char)in[j])) j++;
            if (in[j] == '@') {
                const char *m = "://***@"; for (const char *p = m; *p && w + 1 < n; p++) out[w++] = *p;
                i = j + 1; continue;
            }
        }
        /* well-known token shapes */
        static const char *const pre[] = { "ghp_", "gho_", "ghu_", "ghs_", "ghr_", "github_pat_", "glpat-", "xoxb-", "xoxp-", "AKIA", NULL };
        bool tok = false;
        for (int k = 0; pre[k]; k++) {
            size_t pl = strlen(pre[k]);
            if (!strncmp(in + i, pre[k], pl) && (i == 0 || !isalnum((unsigned char)in[i - 1]))) {
                size_t j = i + pl; while (isalnum((unsigned char)in[j]) || in[j] == '_' || in[j] == '-') j++;
                if (j - i >= pl + 8) { const char *m = "***"; for (const char *p = m; *p && w + 1 < n; p++) out[w++] = *p; i = j; tok = true; }
                break;
            }
        }
        if (tok) continue;
        /* an Authorization header: everything after it on the line is secret (scheme and token) */
        if (!strncasecmp(in + i, "Authorization:", 14)) {
            const char *m = "Authorization: ***"; for (const char *p = m; *p && w + 1 < n; p++) out[w++] = *p;
            while (in[i] && in[i] != '\n') i++;
            continue;
        }
        static const char *const keys[] = { "Bearer ", "bearer ", "password=", "PASSWORD=", "token=", "access_token=", NULL };
        for (int k = 0; keys[k]; k++) {
            size_t kl = strlen(keys[k]);
            if (!strncmp(in + i, keys[k], kl)) {
                for (size_t q = 0; q < kl && w + 1 < n; q++) out[w++] = in[i + q];
                size_t j = i + kl; while (in[j] && !isspace((unsigned char)in[j]) && in[j] != '&' && in[j] != '"') j++;
                const char *m = "***"; for (const char *p = m; *p && w + 1 < n; p++) out[w++] = *p;
                i = j; tok = true; break;
            }
        }
        if (tok) continue;
        out[w++] = in[i++];
    }
    out[w] = 0;
}

/* ---- environment ---- */
static const char *const EXTRA[] = {
    "GIT_TERMINAL_PROMPT=0",          /* never wait for a username / password on a terminal */
    "GIT_ASKPASS=/usr/bin/false",     /* nor in a GUI askpass: credential helpers still work */
    "SSH_ASKPASS=/usr/bin/false",
    "GCM_INTERACTIVE=never",
    "LC_ALL=C", "LANG=C",             /* messages Lume can recognise */
    "GIT_PAGER=cat", "PAGER=cat",
    "GIT_EDITOR=true", "GIT_MERGE_AUTOEDIT=no",
    NULL
};
int gitc_env_extra(const char **out, int max) { int n = 0; for (int i = 0; EXTRA[i] && n < max; i++) out[n++] = EXTRA[i]; return n; }
static char **build_env(bool no_locks) {
    int ne = 0; while (environ && environ[ne]) ne++;
    char **env = calloc((size_t)ne + 16, sizeof *env); int w = 0;
    bool has_ssh = getenv("GIT_SSH_COMMAND") != NULL;
    for (int i = 0; i < ne; i++) {
        bool over = false;
        for (int k = 0; EXTRA[k]; k++) { const char *eq = strchr(EXTRA[k], '='); if (!strncmp(environ[i], EXTRA[k], (size_t)(eq - EXTRA[k] + 1))) over = true; }
        if (!strncmp(environ[i], "GIT_OPTIONAL_LOCKS=", 19)) over = true;
        if (!over) env[w++] = environ[i];
    }
    for (int k = 0; EXTRA[k]; k++) env[w++] = (char *)EXTRA[k];
    if (!has_ssh) env[w++] = "GIT_SSH_COMMAND=ssh -o BatchMode=yes";   /* SSH uses the agent / keys, never prompts */
    if (no_locks) env[w++] = "GIT_OPTIONAL_LOCKS=0";
    env[w] = NULL;
    return env;
}

/* ---- running ---- */
void gitc_out_free(GitOut *o) { free(o->out); o->out = NULL; o->out_len = 0; }
bool gitc_run(const char *git, const char *cwd, const char *const *argv, const GitOpts *o, GitOut *out) {
    memset(out, 0, sizeof *out); out->exit_code = -1;
    GitOpts def = { 0 }; if (!o) o = &def;
    int timeout = o->timeout_ms > 0 ? o->timeout_ms : 60000;
    size_t cap = o->max_out ? o->max_out : (size_t)64 << 20;
    int pin[2], pout[2], perr[2];
    if (pipe(pin) || pipe(pout) || pipe(perr)) { out->spawn_failed = true; snprintf(out->err, sizeof out->err, "pipe: %s", strerror(errno)); return false; }
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_adddup2(&fa, pin[0], 0); posix_spawn_file_actions_adddup2(&fa, pout[1], 1); posix_spawn_file_actions_adddup2(&fa, perr[1], 2);
    posix_spawn_file_actions_addclose(&fa, pin[1]); posix_spawn_file_actions_addclose(&fa, pout[0]); posix_spawn_file_actions_addclose(&fa, perr[0]);
    if (cwd && *cwd) posix_spawn_file_actions_addchdir_np(&fa, cwd);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&at, 0);   /* cancel reaches helpers (ssh, remote-https) */
    int na = 0; while (argv[na]) na++;
    char **av = calloc((size_t)na + 2, sizeof *av); av[0] = (char *)git; for (int i = 0; i < na; i++) av[i + 1] = (char *)argv[i];
    char **env = build_env(o->no_optional_locks);
    pid_t pid = 0;
    double t0 = now_ms();
    int rc = posix_spawn(&pid, git, &fa, &at, av, env);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at); free(av); free(env);
    close(pin[0]); close(pout[1]); close(perr[1]);
    if (rc != 0) { close(pin[1]); close(pout[0]); close(perr[0]); out->spawn_failed = true; snprintf(out->err, sizeof out->err, "cannot start %s: %s", git, strerror(rc)); return false; }
    /* stdin: written while reading so a large patch cannot deadlock */
    const char *in = o->stdin_data; size_t inl = in ? strlen(in) : 0, inw = 0;
    if (!inl) { close(pin[1]); pin[1] = -1; } else fcntl(pin[1], F_SETFL, O_NONBLOCK);
    size_t ocap = 65536; out->out = malloc(ocap); out->out[0] = 0;
    char ebuf[16384]; size_t elen = 0;
    bool oopen = true, eopen = true; int status = 0; bool exited = false;
    while (oopen || eopen || !exited) {
        struct pollfd pf[3]; int np = 0, io = -1, ie = -1, ii = -1;
        if (oopen) { io = np; pf[np].fd = pout[0]; pf[np].events = POLLIN; np++; }
        if (eopen) { ie = np; pf[np].fd = perr[0]; pf[np].events = POLLIN; np++; }
        if (pin[1] >= 0) { ii = np; pf[np].fd = pin[1]; pf[np].events = POLLOUT; np++; }
        if (np) poll(pf, (nfds_t)np, 50); else usleep(10000);
        if (ii >= 0 && (pf[ii].revents & (POLLOUT | POLLERR | POLLHUP))) {
            ssize_t k = write(pin[1], in + inw, inl - inw);
            if (k > 0) inw += (size_t)k;
            if (k < 0 && errno != EAGAIN) inw = inl;
            if (inw >= inl) { close(pin[1]); pin[1] = -1; }
        }
        if (io >= 0 && (pf[io].revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[65536]; ssize_t k = read(pout[0], b, sizeof b);
            if (k <= 0) { oopen = false; close(pout[0]); }
            else if (out->out_len + (size_t)k < cap) {
                if (out->out_len + (size_t)k + 1 > ocap) { while (out->out_len + (size_t)k + 1 > ocap) ocap *= 2; out->out = realloc(out->out, ocap); }
                memcpy(out->out + out->out_len, b, (size_t)k); out->out_len += (size_t)k; out->out[out->out_len] = 0;
            } else out->truncated = true;
        }
        if (ie >= 0 && (pf[ie].revents & (POLLIN | POLLHUP | POLLERR))) {
            char b[4096]; ssize_t k = read(perr[0], b, sizeof b);
            if (k <= 0) { eopen = false; close(perr[0]); }
            else for (ssize_t q = 0; q < k; q++) { if (elen == sizeof ebuf - 1) { memmove(ebuf, ebuf + 4096, elen - 4096); elen -= 4096; } ebuf[elen++] = b[q]; }
        }
        if (!exited) { pid_t r = waitpid(pid, &status, WNOHANG); if (r == pid) exited = true; }
        double el = now_ms() - t0;
        bool cancel = o->cancel && *o->cancel;
        if (!exited && (el > timeout || cancel)) {
            if (cancel) out->cancelled = true; else out->timed_out = true;
            kill(-pid, SIGTERM); usleep(200000); kill(-pid, SIGKILL);
            waitpid(pid, &status, 0); exited = true;
        }
        if (exited && (out->timed_out || out->cancelled)) { if (oopen) { close(pout[0]); oopen = false; } if (eopen) { close(perr[0]); eopen = false; } }
    }
    if (pin[1] >= 0) close(pin[1]);
    out->ms = now_ms() - t0;
    if (WIFEXITED(status) && !out->timed_out && !out->cancelled) out->exit_code = WEXITSTATUS(status);
    ebuf[elen] = 0;
    const char *tail = elen > sizeof out->err - 1 ? ebuf + elen - (sizeof out->err - 1) : ebuf;
    gitc_redact(tail, out->err, sizeof out->err);
    /* trailing whitespace off */
    size_t L = strlen(out->err); while (L && isspace((unsigned char)out->err[L - 1])) out->err[--L] = 0;
    return out->exit_code == 0;
}

/* ---- executable ---- */
bool gitc_find(const char *preferred, char *path, int pn, char *version, int vn, char *why, int wn) {
    const char *cand[8]; int nc = 0;
    if (preferred && *preferred) cand[nc++] = preferred;
    const char *env = getenv("LUME_GIT"); if (env && *env) cand[nc++] = env;
    cand[nc++] = "/usr/bin/git"; cand[nc++] = "/opt/homebrew/bin/git"; cand[nc++] = "/usr/local/bin/git";
    for (int i = 0; i < nc; i++) {
        if (access(cand[i], X_OK)) continue;
        const char *av[] = { "--version", NULL };
        GitOpts o = { .timeout_ms = 15000 }; GitOut r;
        bool ok = gitc_run(cand[i], NULL, av, &o, &r);
        if (ok && r.out && !strncmp(r.out, "git version ", 12)) {
            snprintf(path, (size_t)pn, "%s", cand[i]);
            char v[64]; snprintf(v, sizeof v, "%s", r.out + 12); v[strcspn(v, " \n")] = 0;
            snprintf(version, (size_t)vn, "%s", v);
            gitc_out_free(&r);
            return true;
        }
        gitc_out_free(&r);
    }
    snprintf(why, (size_t)wn, "%s", preferred && *preferred ? "the configured Git does not run; install Git (https://git-scm.com) or fix git= in git.txt" : "Git was not found: install it (Xcode Command Line Tools: xcode-select --install, or https://git-scm.com) or set git=<path> in git.txt");
    return false;
}

/* ---- repositories ---- */
bool gitc_find_root(const char *path, char *root, int rn, char *gitdir, int gn) {
    char cur[2048]; snprintf(cur, sizeof cur, "%s", path);
    struct stat sb;
    if (stat(cur, &sb) == 0 && !S_ISDIR(sb.st_mode)) { char *sl = strrchr(cur, '/'); if (sl) *sl = 0; }
    for (int guard = 0; guard < 256; guard++) {
        char g[2200]; snprintf(g, sizeof g, "%s/.git", cur[0] ? cur : "/");
        if (stat(g, &sb) == 0) {
            snprintf(root, (size_t)rn, "%s", cur[0] ? cur : "/");
            if (S_ISDIR(sb.st_mode)) { snprintf(gitdir, (size_t)gn, "%s", g); return true; }
            /* worktree / submodule: a file "gitdir: <path>" */
            FILE *f = fopen(g, "r"); char line[2048] = "";
            if (f) { if (!fgets(line, sizeof line, f)) line[0] = 0; fclose(f); }
            line[strcspn(line, "\r\n")] = 0;
            if (!strncmp(line, "gitdir: ", 8)) {
                const char *d = line + 8;
                if (d[0] == '/') snprintf(gitdir, (size_t)gn, "%s", d); else snprintf(gitdir, (size_t)gn, "%s/%s", cur, d);
                return true;
            }
        }
        char *sl = strrchr(cur, '/');
        if (!sl || sl == cur) { if (sl == cur && cur[1]) { cur[1] = 0; continue; } break; }
        *sl = 0;
    }
    return false;
}
static bool exists_in(const char *dir, const char *name) { char p[2300]; snprintf(p, sizeof p, "%s/%s", dir, name); struct stat sb; return stat(p, &sb) == 0; }
int gitc_repo_state(const char *gitdir) {
    if (exists_in(gitdir, "rebase-merge") || exists_in(gitdir, "rebase-apply")) return GS_REBASING;
    if (exists_in(gitdir, "MERGE_HEAD")) return GS_MERGING;
    if (exists_in(gitdir, "CHERRY_PICK_HEAD")) return GS_CHERRY_PICKING;
    if (exists_in(gitdir, "REVERT_HEAD")) return GS_REVERTING;
    if (exists_in(gitdir, "BISECT_LOG")) return GS_BISECTING;
    return GS_NONE;
}
const char *gitc_state_name(int st) { static const char *const n[] = { "", "merging", "rebasing", "cherry-picking", "reverting", "bisecting" }; return st >= 0 && st <= GS_BISECTING ? n[st] : ""; }

/* ---- status ---- */
static const char *skip_fields(const char *s, int n) { for (int i = 0; i < n && s; i++) { s = strchr(s, ' '); if (s) s++; } return s; }
static void add_entry(GitStatus *st, char x, char y, int kind, const char *path, const char *orig, bool sub) {
    if (st->n == st->cap) { st->cap = st->cap ? st->cap * 2 : 64; st->e = realloc(st->e, sizeof *st->e * (size_t)st->cap); }
    GitEntry *e = &st->e[st->n++];
    e->x = x; e->y = y; e->kind = kind; e->path = strdup(path); e->orig = orig ? strdup(orig) : NULL; e->submodule = sub;
    if (kind == GE_UNMERGED) st->conflicts++;
    else if (kind == GE_UNTRACKED) st->untracked++;
    else { if (x != '.') st->staged++; if (y != '.') st->unstaged++; }
}
bool gitc_status_parse(const char *z, size_t len, GitStatus *st) {
    memset(st, 0, sizeof *st);
    size_t i = 0;
    while (i < len) {
        const char *rec = z + i; size_t rl = strnlen(rec, len - i);
        char line[4200]; snprintf(line, sizeof line, "%.*s", (int)(rl < sizeof line - 1 ? rl : sizeof line - 1), rec);
        i += rl + 1;
        if (!line[0]) continue;
        if (line[0] == '#') {
            if (!strncmp(line, "# branch.oid ", 13)) { snprintf(st->oid, sizeof st->oid, "%s", line + 13); st->initial = !strcmp(st->oid, "(initial)"); }
            else if (!strncmp(line, "# branch.head ", 14)) { snprintf(st->head, sizeof st->head, "%s", line + 14); st->detached = !strcmp(st->head, "(detached)"); }
            else if (!strncmp(line, "# branch.upstream ", 18)) { snprintf(st->upstream, sizeof st->upstream, "%s", line + 18); st->has_upstream = true; }
            else if (!strncmp(line, "# branch.ab ", 12)) sscanf(line + 12, "+%d -%d", &st->ahead, &st->behind);
            continue;
        }
        if (line[0] == '1' && line[1] == ' ') {
            const char *p = skip_fields(line, 8); if (!p) continue;
            add_entry(st, line[2], line[3], GE_ORDINARY, p, NULL, line[5] == 'S');
        } else if (line[0] == '2' && line[1] == ' ') {
            const char *p = skip_fields(line, 9); if (!p) continue;
            const char *orig = z + i; size_t ol = strnlen(orig, len - i); i += ol + 1;
            add_entry(st, line[2], line[3], GE_RENAME, p, orig, line[5] == 'S');
        } else if (line[0] == 'u' && line[1] == ' ') {
            const char *p = skip_fields(line, 10); if (!p) continue;
            add_entry(st, line[2], line[3], GE_UNMERGED, p, NULL, false);
        } else if (line[0] == '?' && line[1] == ' ') {
            add_entry(st, '?', '?', GE_UNTRACKED, line + 2, NULL, false);
        }
    }
    return true;
}
void gitc_status_free(GitStatus *st) { for (int i = 0; i < st->n; i++) { free(st->e[i].path); free(st->e[i].orig); } free(st->e); memset(st, 0, sizeof *st); }
bool gitc_entry_staged(const GitEntry *e) { return (e->kind == GE_ORDINARY || e->kind == GE_RENAME) && e->x != '.'; }
bool gitc_entry_unstaged(const GitEntry *e) { return e->kind == GE_UNTRACKED || ((e->kind == GE_ORDINARY || e->kind == GE_RENAME) && e->y != '.'); }
const char *gitc_entry_label(char c) {
    switch (c) { case 'M': return "modified"; case 'A': return "added"; case 'D': return "deleted"; case 'R': return "renamed"; case 'C': return "copied"; case 'T': return "type changed"; case '?': return "untracked"; case 'U': return "conflict"; default: return "unchanged"; }
}

/* ---- diff ---- */
void gitc_diff_free(GitDiff *d) {
    for (int i = 0; i < d->nf; i++) { free(d->f[i].old_path); free(d->f[i].new_path); free(d->f[i].header); free(d->f[i].h); }
    free(d->f); free(d->l); free(d->text); memset(d, 0, sizeof *d);
}
static char *dupn(const char *s, int n) { char *r = malloc((size_t)n + 1); memcpy(r, s, (size_t)n); r[n] = 0; return r; }
bool gitc_diff_parse(const char *text, GitDiff *d) {
    memset(d, 0, sizeof *d);
    d->text = strdup(text ? text : "");
    const char *s = d->text;
    int capf = 0, capl = 0, caph = 0;
    GitDFile *f = NULL; GitHunk *h = NULL;
    const char *hdr_start = NULL;
    int old_no = 0, new_no = 0;
    while (*s) {
        const char *e = strchr(s, '\n'); int L = e ? (int)(e - s) : (int)strlen(s);
        if (!strncmp(s, "diff --git ", 11)) {
            if (f && hdr_start && !f->header) f->header = dupn(hdr_start, (int)(s - hdr_start));
            if (d->nf == capf) { capf = capf ? capf * 2 : 8; d->f = realloc(d->f, sizeof *d->f * (size_t)capf); }
            f = &d->f[d->nf++]; memset(f, 0, sizeof *f); caph = 0; h = NULL;
            hdr_start = s;
            /* a/x b/y: paths may hold spaces; take them from ---/+++ or rename lines when present */
            const char *b = strstr(s, " b/"); if (b && b < s + L) { f->old_path = dupn(s + 13, (int)(b - s - 13)); f->new_path = dupn(b + 3, (int)(s + L - b - 3)); }
        } else if (f && !h) {
            if (!strncmp(s, "new file mode", 13)) f->is_new = true;
            else if (!strncmp(s, "deleted file mode", 17)) f->is_deleted = true;
            else if (!strncmp(s, "rename from ", 12)) { free(f->old_path); f->old_path = dupn(s + 12, L - 12); f->renamed = true; }
            else if (!strncmp(s, "rename to ", 10)) { free(f->new_path); f->new_path = dupn(s + 10, L - 10); f->renamed = true; }
            else if (!strncmp(s, "Binary files ", 13) || !strncmp(s, "GIT binary patch", 16)) f->binary = true;
            else if (!strncmp(s, "--- ", 4) && strncmp(s + 4, "/dev/null", 9) && !f->renamed) { free(f->old_path); f->old_path = dupn(s + 6, L - 6 > 0 ? L - 6 : 0); }
            else if (!strncmp(s, "+++ ", 4) && strncmp(s + 4, "/dev/null", 9) && !f->renamed) { free(f->new_path); f->new_path = dupn(s + 6, L - 6 > 0 ? L - 6 : 0); }
        }
        if (f && !strncmp(s, "@@ ", 3)) {
            if (!f->header && hdr_start) f->header = dupn(hdr_start, (int)(s - hdr_start));
            if (f->nh == caph) { caph = caph ? caph * 2 : 4; f->h = realloc(f->h, sizeof *f->h * (size_t)caph); }
            h = &f->h[f->nh++]; memset(h, 0, sizeof *h);
            /* forms: -a,b +c,d | -a +c,d | -a,b +c | -a +c */
            { int a = 0, b2 = 1, c = 0, dd = 1; const char *q = s + 4; a = atoi(q); while (*q && *q != ' ' && *q != ',') q++; if (*q == ',') { b2 = atoi(q + 1); while (*q && *q != ' ') q++; } while (*q == ' ') q++; if (*q == '+') { q++; c = atoi(q); while (*q && *q != ' ' && *q != ',') q++; if (*q == ',') dd = atoi(q + 1); } h->old_start = a; h->old_n = b2; h->new_start = c; h->new_n = dd; }
            snprintf(h->header, sizeof h->header, "%.*s", L, s);
            h->first = d->nl; old_no = h->old_start; new_no = h->new_start;
        } else if (h && (s[0] == ' ' || s[0] == '+' || s[0] == '-' || s[0] == '\\')) {
            if (d->nl == capl) { capl = capl ? capl * 2 : 256; d->l = realloc(d->l, sizeof *d->l * (size_t)capl); }
            GitDLine *dl = &d->l[d->nl++];
            dl->t = s[0]; dl->s = s + 1; dl->len = L - 1;
            dl->old_no = s[0] == '+' || s[0] == '\\' ? 0 : old_no; dl->new_no = s[0] == '-' || s[0] == '\\' ? 0 : new_no;
            if (s[0] == ' ') { old_no++; new_no++; } else if (s[0] == '-') { old_no++; d->dels++; } else if (s[0] == '+') { new_no++; d->adds++; }
            h->nlines++;
        } else if (h && strncmp(s, "diff --git ", 11)) h = NULL;
        s += L + (e ? 1 : 0);
    }
    if (f && hdr_start && !f->header) f->header = dupn(hdr_start, (int)(strlen(hdr_start)));
    return true;
}
char *gitc_hunk_patch(const GitDiff *d, int fi, int hi) {
    if (fi < 0 || fi >= d->nf || hi < 0 || hi >= d->f[fi].nh) return NULL;
    const GitDFile *f = &d->f[fi]; const GitHunk *h = &f->h[hi];
    /* the file header up to (and including) the +++ line */
    const char *hd = f->header ? f->header : ""; const char *pp = strstr(hd, "\n+++ ");
    size_t hl = pp ? (size_t)(strchr(pp + 1, '\n') ? strchr(pp + 1, '\n') - hd + 1 : strlen(hd)) : strlen(hd);
    size_t cap = hl + strlen(h->header) + 64; for (int i = 0; i < h->nlines; i++) cap += (size_t)d->l[h->first + i].len + 2;
    char *p = malloc(cap); size_t w = 0;
    memcpy(p, hd, hl); w = hl; if (w && p[w - 1] != '\n') p[w++] = '\n';
    w += (size_t)snprintf(p + w, cap - w, "%s\n", h->header);
    for (int i = 0; i < h->nlines; i++) { const GitDLine *l = &d->l[h->first + i]; p[w++] = l->t; memcpy(p + w, l->s, (size_t)l->len); w += (size_t)l->len; p[w++] = '\n'; }
    p[w] = 0;
    return p;
}
int gitc_side_by_side(const GitDiff *d, int fi, int (*rows)[2], int max) {
    if (fi < 0 || fi >= d->nf) return 0;
    const GitDFile *f = &d->f[fi]; int n = 0;
    for (int hi = 0; hi < f->nh; hi++) {
        const GitHunk *h = &f->h[hi];
        int i = h->first, end = h->first + h->nlines;
        if (n < max) { rows[n][0] = -2 - hi; rows[n][1] = -2 - hi; n++; }   /* a hunk separator row */
        while (i < end && n < max) {
            char t = d->l[i].t;
            if (t == ' ') { rows[n][0] = i; rows[n][1] = i; n++; i++; continue; }
            if (t == '\\') { i++; continue; }
            int ds = i; while (i < end && d->l[i].t == '-') i++;
            int de = i; int as = i; while (i < end && (d->l[i].t == '+' || d->l[i].t == '\\')) i++;
            int ae = i;
            int nd = de - ds, nadd = 0; for (int k = as; k < ae; k++) if (d->l[k].t == '+') nadd++;
            int m = nd > nadd ? nd : nadd, ai = as;
            for (int k = 0; k < m && n < max; k++) {
                while (ai < ae && d->l[ai].t != '+') ai++;
                rows[n][0] = k < nd ? ds + k : -1;
                rows[n][1] = ai < ae ? ai : -1; if (ai < ae) ai++;
                n++;
            }
        }
    }
    return n;
}

/* ---- log / refs / remotes ---- */
static int split(char *rec, char sep, char **f, int max) { int n = 0; f[n++] = rec; for (char *p = rec; *p && n < max; p++) if (*p == sep) { *p = 0; f[n++] = p + 1; } return n; }
#define CPY(d, s) snprintf(d, sizeof(d), "%s", s)
int gitc_log_parse(const char *text, GitCommit *out, int max) {
    char *t = strdup(text ? text : ""); int n = 0;
    char *save = t;
    for (char *rec = t; rec && *rec && n < max;) {
        char *end = strchr(rec, '\x1e'); if (end) *end = 0;
        while (*rec == '\n' || *rec == '\r') rec++;
        char *f[8]; int k = split(rec, '\x1f', f, 8);
        if (k >= 6) {
            GitCommit *c = &out[n++]; memset(c, 0, sizeof *c);
            CPY(c->hash, f[0]); CPY(c->shorth, f[1]); CPY(c->author, f[2]); CPY(c->email, f[3]); CPY(c->date, f[4]); CPY(c->subject, f[5]); if (k >= 7) CPY(c->refs, f[6]);
        }
        rec = end ? end + 1 : NULL;
    }
    free(save);
    return n;
}
int gitc_refs_parse(const char *text, GitRef *out, int max) {
    char *t = strdup(text ? text : ""); int n = 0;
    for (char *rec = t; rec && *rec && n < max;) {
        char *end = strchr(rec, '\x1e'); if (end) *end = 0;
        while (*rec == '\n') rec++;
        char *f[8]; int k = split(rec, '\x1f', f, 8);
        if (k >= 5 && f[0][0]) {
            GitRef *r = &out[n]; memset(r, 0, sizeof *r);
            CPY(r->name, f[0]); CPY(r->shortn, f[1]); CPY(r->oid, f[2]); CPY(r->upstream, f[3]); r->head = f[4][0] == '*'; if (k >= 6) CPY(r->track, f[5]);
            r->remote = !strncmp(r->name, "refs/remotes/", 13);
            if (!(r->remote && strstr(r->name, "/HEAD"))) n++;   /* origin/HEAD is an alias, not a branch */
        }
        rec = end ? end + 1 : NULL;
    }
    free(t);
    return n;
}
int gitc_remotes_parse(const char *text, GitRemote *out, int max) {
    int n = 0; const char *s = text ? text : "";
    while (*s) {
        const char *e = strchr(s, '\n'); int L = e ? (int)(e - s) : (int)strlen(s);
        char line[1200]; snprintf(line, sizeof line, "%.*s", L < 1199 ? L : 1199, s);
        char name[64] = "", url[1100] = "", kind[16] = "";
        if (sscanf(line, "%63s %1099s %15s", name, url, kind) >= 2) {
            int k; for (k = 0; k < n; k++) if (!strcmp(out[k].name, name)) break;
            if (k == n && n < max) { memset(&out[n], 0, sizeof out[n]); CPY(out[n].name, name); n++; }
            if (k < max) { char red[600]; gitc_redact(url, red, sizeof red); if (!strcmp(kind, "(push)")) CPY(out[k].push, red); else CPY(out[k].fetch, red); }
        }
        s += L + (e ? 1 : 0);
    }
    return n;
}
const char *gitc_provider(const char *url) {
    if (!url || !*url) return "none";
    if (strcasestr(url, "github.com")) return "GitHub";
    if (strcasestr(url, "gitlab")) return "GitLab";
    if (strcasestr(url, "dev.azure.com") || strcasestr(url, "visualstudio.com")) return "Azure DevOps";
    if (strcasestr(url, "bitbucket")) return "Bitbucket";
    if (url[0] == '/' || !strncmp(url, "file://", 7)) return "local";
    return "self-hosted";
}

/* ---- names and messages ---- */
bool gitc_branch_name_ok(const char *s, char *why, int n) {
    #define BAD(m) do { snprintf(why, (size_t)n, "%s", m); return false; } while (0)
    if (!s || !*s) BAD("a branch needs a name");
    if (strlen(s) > 200) BAD("name too long");
    if (s[0] == '-' || s[0] == '/' || s[0] == '.') BAD("cannot start with '-', '/' or '.'");
    size_t L = strlen(s);
    if (s[L - 1] == '/' || s[L - 1] == '.') BAD("cannot end with '/' or '.'");
    if (L >= 5 && !strcmp(s + L - 5, ".lock")) BAD("cannot end with .lock");
    if (strstr(s, "..") || strstr(s, "@{") || strstr(s, "//") || strstr(s, "/.")) BAD("cannot contain '..', '@{', '//' or '/.'");
    if (!strcmp(s, "@") || !strcmp(s, "HEAD")) BAD("reserved name");
    for (const char *p = s; *p; p++) if ((unsigned char)*p < 0x20 || *p == 0x7f || strchr(" ~^:?*[\\", *p)) BAD("cannot contain spaces or any of ~ ^ : ? * [ \\");
    return true;
    #undef BAD
}
bool gitc_message_ok(const char *summary, char *why, int n, char *warn, int wn) {
    if (wn) warn[0] = 0;
    const char *p = summary ? summary : ""; while (isspace((unsigned char)*p)) p++;
    if (!*p) { snprintf(why, (size_t)n, "write a commit message (the summary line)"); return false; }
    size_t L = strcspn(p, "\n");
    if (L > 72 && wn) snprintf(warn, (size_t)wn, "the summary has %zu characters: 72 or fewer read best in logs", L);
    return true;
}

/* ---- AI policy ---- */
bool gitc_path_sensitive(const char *path) {
    const char *b = strrchr(path, '/'); b = b ? b + 1 : path;
    static const char *const names[] = { ".env", ".netrc", ".npmrc", ".pypirc", ".git-credentials", "id_rsa", "id_ed25519", "id_ecdsa", "id_dsa", "secrets.txt", "connections.txt", "credentials", "credentials.json", "ai.txt", NULL };
    for (int i = 0; names[i]; i++) if (!strcasecmp(b, names[i])) return true;
    if (!strncasecmp(b, ".env.", 5)) return true;
    static const char *const exts[] = { ".pem", ".key", ".p12", ".pfx", ".keystore", ".jks", ".kdbx", ".asc", NULL };
    size_t L = strlen(b);
    for (int i = 0; exts[i]; i++) { size_t el = strlen(exts[i]); if (L > el && !strcasecmp(b + L - el, exts[i])) return true; }
    if (strcasestr(b, "secret") || strcasestr(b, "password") || strcasestr(b, "token")) return true;
    return false;
}
