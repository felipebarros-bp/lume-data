/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * Sandboxed custom extractors: see extract.h for the user-facing contract.
 *
 * Files (all in the preferences folder passed as `dir`, which ends with a slash):
 *   extractors.txt        user-edited entries, "name<TAB>command[<TAB>timeout=N]"
 *   extractors-trust.txt  one "hash<TAB>name" line per approved entry
 *   extractors-log.txt    append-only audit log of runs
 *
 * A run: extract_begin makes a scratch folder (mkdtemp) where the caller writes rows.tsv;
 * extract_run splits the command into argv (no shell; {file} and {out} substituted), wraps it
 * in sandbox-exec with a generated deny-by-default profile (macOS) or in bubblewrap (Linux),
 * forks, applies rlimits in the child, execs with a minimal environment and polls the child
 * until it exits, times out, is cancelled through pump() or exceeds the memory cap; on
 * success the output file is moved to the destination. extract_end deletes the scratch
 * folder.
 *
 * Threading: no shared state; extract_run blocks its caller (the UI keeps painting through
 * pump()).
 */
#define _DARWIN_C_SOURCE
#define _GNU_SOURCE
#ifdef __APPLE__
#include <libproc.h>
#endif
#include "extract.h"
#include <dirent.h>
#include <errno.h>
#include <fcntl.h>
#include <limits.h>
#include <signal.h>
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <sys/resource.h>
#include <sys/stat.h>
#include <sys/types.h>
#include <sys/wait.h>
#include <stdint.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

#if defined(__APPLE__)
#define SANDBOX_EXEC "/usr/bin/sandbox-exec"
#else
#define BWRAP_PATHS { "/usr/bin/bwrap", "/usr/local/bin/bwrap", NULL }
#endif

/* Physical memory of the extractor's process group, in bytes (0 when it cannot be read).
 * macOS ignores RLIMIT_AS, so the memory limit is enforced by sampling this from the wait loop
 * and killing the group when it goes over (US-045). phys_footprint is what Activity Monitor
 * shows as "Memory". Linux keeps the kernel's RLIMIT_AS. */
static unsigned long long group_memory(pid_t pgid) {
#ifdef __APPLE__
    pid_t pids[256];
    int n = proc_listpgrppids(pgid, pids, sizeof pids);
    if (n <= 0) { pids[0] = pgid; n = 1; }
    unsigned long long total = 0;
    for (int i = 0; i < n && i < 256; i++) {
        struct rusage_info_v2 ri;
        if (proc_pid_rusage(pids[i], RUSAGE_INFO_V2, (rusage_info_t *)&ri) == 0) total += ri.ri_phys_footprint;
    }
    return total;
#else
    (void)pgid;
    return 0;
#endif
}

/* Monotonic clock in seconds. */
static double now_s(void) { struct timespec ts; clock_gettime(CLOCK_MONOTONIC, &ts); return ts.tv_sec + ts.tv_nsec / 1e9; }

/* FNV-1a over name, command and timeout (each followed by a separator byte) as 8 hex digits:
 * the identity an approval is bound to. Not a security hash; it detects edits. */
static void fnv_hex(const Extractor *e, char *out, size_t n) {
    uint32_t h = 2166136261u;
    const char *parts[3] = { e->name, e->cmd, NULL };
    char t[16]; snprintf(t, sizeof t, "%d", e->timeout_secs); parts[2] = t;
    for (int i = 0; i < 3; i++) { for (const char *p = parts[i]; *p; p++) { h ^= (unsigned char)*p; h *= 16777619u; } h ^= 9; h *= 16777619u; }
    snprintf(out, n, "%08x", h);
}

/* See extract.h. Indexed by the EXTRACT_* enum order. */
const char *extract_status_name(int status) {
    static const char *const names[] = { "ok", "not approved", "no sandbox", "bad command", "could not start",
                                         "failed", "timeout", "cancelled", "no output" };
    return status >= 0 && status < (int)(sizeof names / sizeof *names) ? names[status] : "?";
}

/* See extract.h. */
ExtractLimits extract_default_limits(const Extractor *e) {
    ExtractLimits l;
    l.wall_secs = e && e->timeout_secs > 0 ? e->timeout_secs : EXTRACT_DEFAULT_SECS;
    if (l.wall_secs > EXTRACT_MAX_SECS) l.wall_secs = EXTRACT_MAX_SECS;
    l.cpu_secs = l.wall_secs;
    l.mem_mb = 1024;
    l.out_mb = 4096;
    return l;
}

/* See extract.h. */
bool extract_sandbox_available(void) {
#if defined(__APPLE__)
    return access(SANDBOX_EXEC, X_OK) == 0;
#else
    const char *paths[] = BWRAP_PATHS;
    for (int i = 0; paths[i]; i++) if (access(paths[i], X_OK) == 0) return true;
    return false;
#endif
}

/* Whole file as a NUL-terminated heap string (caller frees); NULL when it cannot be opened. */
static char *read_all(const char *path) {
    FILE *f = fopen(path, "rb");
    if (!f) return NULL;
    char *s = NULL; size_t len = 0, cap = 0, k; char chunk[4096];
    while ((k = fread(chunk, 1, sizeof chunk, f)) > 0) {
        if (len + k + 1 > cap) { cap = (len + k + 1) * 2; s = realloc(s, cap); }
        memcpy(s + len, chunk, k); len += k;
    }
    fclose(f);
    if (!s) s = calloc(1, 1); else s[len] = 0;
    return s;
}

/* Path of the trust file. */
static void trust_path(const char *dir, char *buf, size_t n) { snprintf(buf, n, "%sextractors-trust.txt", dir); }

/* True when a line of the trust file starts with the 8-digit hash. */
static bool trust_has(const char *dir, const char *hash) {
    char path[1200]; trust_path(dir, path, sizeof path);
    char *c = read_all(path);
    if (!c) return false;
    bool found = false;
    for (const char *p = c; *p;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        if (ll >= 8 && strncmp(p, hash, 8) == 0) { found = true; break; }
        p = nl ? nl + 1 : p + ll;
    }
    free(c);
    return found;
}

/* See extract.h. Lines of 500 bytes or more, or without a non-empty command, are ignored;
 * the timeout is clamped to 1..EXTRACT_MAX_SECS (default when missing). */
int extract_load(const char *dir, Extractor *out, int max) {
    char path[1200];
    snprintf(path, sizeof path, "%sextractors.txt", dir);
    char *content = read_all(path);
    if (!content) return 0;
    int n = 0;
    for (const char *p = content; *p && n < max;) {
        const char *nl = strchr(p, '\n');
        size_t ll = nl ? (size_t)(nl - p) : strlen(p);
        const char *tab = memchr(p, '\t', ll);
        if (*p != '#' && tab && tab > p && ll - (size_t)(tab - p) > 1 && ll < 500) {
            Extractor *e = &out[n];
            memset(e, 0, sizeof *e);
            snprintf(e->name, sizeof e->name, "%.*s", (int)(tab - p), p);
            const char *cmd = tab + 1;
            size_t cl = ll - (size_t)(tab - p) - 1;
            const char *tab2 = memchr(cmd, '\t', cl);
            if (tab2) {
                const char *opt = tab2 + 1;
                if (strncmp(opt, "timeout=", 8) == 0) e->timeout_secs = atoi(opt + 8);
                cl = (size_t)(tab2 - cmd);
            }
            while (cl && (cmd[cl - 1] == '\r' || cmd[cl - 1] == ' ')) cl--;
            snprintf(e->cmd, sizeof e->cmd, "%.*s", (int)cl, cmd);
            if (e->timeout_secs <= 0) e->timeout_secs = EXTRACT_DEFAULT_SECS;
            if (e->timeout_secs > EXTRACT_MAX_SECS) e->timeout_secs = EXTRACT_MAX_SECS;
            snprintf(e->origin, sizeof e->origin, "%s", path);
            fnv_hex(e, e->hash, sizeof e->hash);
            e->trusted = trust_has(dir, e->hash);
            n++;
        }
        p = nl ? nl + 1 : p + ll;
    }
    free(content);
    return n;
}

/* See extract.h. Every line of the old file except this hash is copied, then the new
 * approval is appended when trusting. */
bool extract_trust(const char *dir, Extractor *e, bool trust) {
    char path[1200]; trust_path(dir, path, sizeof path);
    char *old = read_all(path);
    char tmp[1210]; snprintf(tmp, sizeof tmp, "%s.tmp", path);
    FILE *f = fopen(tmp, "wb");
    if (!f) { free(old); return false; }
    if (old) {
        for (const char *p = old; *p;) {
            const char *nl = strchr(p, '\n');
            size_t ll = nl ? (size_t)(nl - p) : strlen(p);
            if (ll >= 8 && strncmp(p, e->hash, 8) != 0) fprintf(f, "%.*s\n", (int)ll, p);
            p = nl ? nl + 1 : p + ll;
        }
        free(old);
    }
    if (trust) fprintf(f, "%s\t%s\n", e->hash, e->name);
    bool ok = fclose(f) == 0 && rename(tmp, path) == 0;
    if (ok) e->trusted = trust;
    return ok;
}

/* See extract.h. */
void extract_describe(const Extractor *e, char *buf, size_t n) {
    ExtractLimits l = extract_default_limits(e);
    snprintf(buf, n,
             "Runs as a separate process in a sandbox: it can read the TSV Lume writes and system programs, "
             "write only the output file, and has no network and no access to your home folder, keychain, "
             ".env files or other data. Stopped after %d s of wall clock or CPU, a %ld MB output, or %ld MB of memory (sampled and enforced by Lume on macOS, by the kernel on Linux). "
             "Program and arguments are used as written, no shell. Approval is tied to this exact line.",
             l.wall_secs, l.out_mb, l.mem_mb);
}

/* ---- argv ---------------------------------------------------------------- */

/* Splits a command line into argv the way a user would expect from a shell, without being
 * one: blanks separate tokens, single or double quotes group (no escapes), and {file} /
 * {out} anywhere in a token become the input / output paths. Tokens are heap copies (free
 * with free_argv); argv is NULL-terminated. Returns argc, or -1 with err filled. */
static int split_argv(const char *cmd, const char *infile, const char *outfile, char **argv, int max, char *err, size_t errn) {
    int argc = 0;
    const char *p = cmd;
    while (*p) {
        while (*p == ' ' || *p == '\t') p++;
        if (!*p) break;
        if (argc >= max - 1) { snprintf(err, errn, "too many arguments"); return -1; }
        char tok[2048]; size_t t = 0; char quote = 0;
        while (*p && (quote || (*p != ' ' && *p != '\t'))) {
            if (quote) { if (*p == quote) { quote = 0; p++; continue; } }
            else if (*p == '"' || *p == '\'') { quote = *p++; continue; }
            /* a path that does not fit is refused, never cut: a truncated path names another file */
            if (strncmp(p, "{file}", 6) == 0) { if (strlen(infile) >= sizeof tok - t) { snprintf(err, errn, "argument too long"); return -1; } t += (size_t)snprintf(tok + t, sizeof tok - t, "%s", infile); p += 6; continue; }
            if (strncmp(p, "{out}", 5) == 0) { if (strlen(outfile) >= sizeof tok - t) { snprintf(err, errn, "argument too long"); return -1; } t += (size_t)snprintf(tok + t, sizeof tok - t, "%s", outfile); p += 5; continue; }
            if (t < sizeof tok - 1) tok[t++] = *p;
            p++;
        }
        if (quote) { snprintf(err, errn, "unbalanced quote"); return -1; }
        tok[t] = 0;
        argv[argc++] = strdup(tok);
    }
    argv[argc] = NULL;
    return argc;
}

/* Frees the tokens of a NULL-terminated argv (not the array itself). */
static void free_argv(char **argv) { for (int i = 0; argv[i]; i++) free(argv[i]); }

/* ---- scratch folder ------------------------------------------------------- */

/* See extract.h. The scratch path is resolved with realpath so the sandbox profile names
 * the real location (/var -> /private/var on macOS). */
bool extract_begin(const char *dir, ExtractRun *run, char *err, size_t errn) {
    memset(run, 0, sizeof *run);
    snprintf(run->scratch, sizeof run->scratch, "%sextract-XXXXXX", dir);
    if (!mkdtemp(run->scratch)) { snprintf(err, errn, "cannot create a scratch folder in %s: %s", dir, strerror(errno)); return false; }
    char real[PATH_MAX];
    if (realpath(run->scratch, real)) snprintf(run->scratch, sizeof run->scratch, "%s", real);
    snprintf(run->infile, sizeof run->infile, "%s/rows.tsv", run->scratch);
    run->active = true;
    return true;
}

/* Recursive delete (at most 8 levels deep); symlinks are unlinked, never followed. */
static void remove_tree(const char *path, int depth) {
    if (depth > 8) return;
    DIR *d = opendir(path);
    if (!d) { remove(path); return; }
    struct dirent *de;
    while ((de = readdir(d)) != NULL) {
        if (strcmp(de->d_name, ".") == 0 || strcmp(de->d_name, "..") == 0) continue;
        char sub[PATH_MAX]; snprintf(sub, sizeof sub, "%s/%s", path, de->d_name);
        struct stat st;
        if (lstat(sub, &st) == 0 && S_ISDIR(st.st_mode)) remove_tree(sub, depth + 1); else unlink(sub);
    }
    closedir(d);
    rmdir(path);
}

/* See extract.h. */
void extract_end(ExtractRun *run) {
    if (!run->active) return;
    remove_tree(run->scratch, 0);
    run->active = false;
}

/* ---- run ------------------------------------------------------------------ */

/* Moves a file: rename when on the same file system, otherwise copy then leave the source
 * for extract_end to delete. A partial copy is removed. */
static bool move_file(const char *from, const char *to, char *err, size_t errn) {
    if (rename(from, to) == 0) return true;
    FILE *in = fopen(from, "rb"), *out = in ? fopen(to, "wb") : NULL;
    if (!in || !out) { if (in) fclose(in); snprintf(err, errn, "cannot write %s: %s", to, strerror(errno)); return false; }
    char buf[65536]; size_t k; bool ok = true;
    while ((k = fread(buf, 1, sizeof buf, in)) > 0) if (fwrite(buf, 1, k, out) != k) { ok = false; break; }
    fclose(in);
    if (fclose(out) != 0) ok = false;
    if (!ok) { snprintf(err, errn, "write error on %s", to); remove(to); }
    return ok;
}

/* See extract.h. Steps: refuse unapproved entries or a missing sandbox; build argv and check
 * the program; wrap it in the sandbox; fork and exec with limits; poll; classify the exit;
 * move the output. */
int extract_run(ExtractRun *run, const Extractor *e, const char *dest, const ExtractLimits *lim,
                bool (*pump)(void *ud), void *ud, ExtractResult *res) {
    memset(res, 0, sizeof *res);
    res->exit_code = -1;
    if (!e->trusted) { res->status = EXTRACT_DENIED; snprintf(res->message, sizeof res->message, "extractor \"%s\" has not been approved", e->name); return res->status; }
    if (!extract_sandbox_available()) { res->status = EXTRACT_NO_SANDBOX; snprintf(res->message, sizeof res->message, "no sandbox is available on this system; extractors do not run without one"); return res->status; }
    ExtractLimits l = lim ? *lim : extract_default_limits(e);
    /* the program writes inside the scratch folder; the file is moved to `dest` on success */
    const char *ext = strrchr(dest, '.');
    if (ext && strchr(ext, '/')) ext = NULL;
    snprintf(run->outfile, sizeof run->outfile, "%s/out%s", run->scratch, ext ? ext : "");
    char *argv[64];
    int argc = split_argv(e->cmd, run->infile, run->outfile, argv, 40, res->message, sizeof res->message);
    if (argc <= 0) { res->status = EXTRACT_BAD_COMMAND; if (!res->message[0]) snprintf(res->message, sizeof res->message, "empty command"); return res->status; }
    if (argv[0][0] != '/') { res->status = EXTRACT_BAD_COMMAND; snprintf(res->message, sizeof res->message, "the program must be an absolute path (got \"%s\")", argv[0]); free_argv(argv); return res->status; }
    if (access(argv[0], X_OK) != 0) { res->status = EXTRACT_BAD_COMMAND; snprintf(res->message, sizeof res->message, "%s is not executable: %s", argv[0], strerror(errno)); free_argv(argv); return res->status; }

    /* sandbox wrapper argv: sandbox-exec -p <profile>, or bwrap with read-only system binds,
     * the scratch folder writable and every namespace unshared (no network) */
    char *wrapped[80]; int w = 0;
#if defined(__APPLE__)
    char profile[4096];
    snprintf(profile, sizeof profile,
             "(version 1)\n(deny default)\n(allow process-fork)\n(allow process-exec*)\n(allow signal (target same-sandbox))\n"
             "(allow sysctl-read)\n(allow mach-lookup (global-name \"com.apple.system.opendirectoryd.libinfo\") (global-name \"com.apple.system.logger\"))\n"
             "(allow file-read* (subpath \"/usr\") (subpath \"/bin\") (subpath \"/sbin\") (subpath \"/System\") (subpath \"/Library/Frameworks\") "
             "(subpath \"/Library/Developer\") (subpath \"/Library/Apple\") (subpath \"/private/var/db/dyld\") (subpath \"/private/var/db/timezone\") (subpath \"/private/var/select\") "
             "(subpath \"/private/etc\") (subpath \"/dev\") (subpath \"/opt\") (subpath \"/Applications/Xcode.app\") "
             "(literal \"/\") (literal \"/private\") (literal \"/Library\") (literal \"/etc\") (literal \"/var\") (literal \"/tmp\") (literal \"/private/tmp\") (literal \"/Users\"))\n"
             "(allow file-write* (literal \"/dev/null\") (literal \"/dev/dtracehelper\"))\n(allow file-ioctl (literal \"/dev/dtracehelper\") (subpath \"%s\"))\n"
             "(allow file* (subpath \"%s\"))\n(deny network*)\n",
             run->scratch, run->scratch);
    wrapped[w++] = (char *)SANDBOX_EXEC; wrapped[w++] = "-p"; wrapped[w++] = profile;
#else
    const char *bw_paths[] = BWRAP_PATHS; const char *bw = NULL;
    for (int i = 0; bw_paths[i]; i++) if (access(bw_paths[i], X_OK) == 0) { bw = bw_paths[i]; break; }
    wrapped[w++] = (char *)bw;
    const char *ro[] = { "/usr", "/bin", "/sbin", "/lib", "/lib64", "/etc/ld.so.cache", "/etc/alternatives", "/opt", NULL };
    for (int i = 0; ro[i]; i++) if (access(ro[i], F_OK) == 0) { wrapped[w++] = "--ro-bind"; wrapped[w++] = (char *)ro[i]; wrapped[w++] = (char *)ro[i]; }
    wrapped[w++] = "--bind"; wrapped[w++] = run->scratch; wrapped[w++] = run->scratch;
    wrapped[w++] = "--dev"; wrapped[w++] = "/dev"; wrapped[w++] = "--proc"; wrapped[w++] = "/proc";
    wrapped[w++] = "--unshare-all"; wrapped[w++] = "--die-with-parent"; wrapped[w++] = "--new-session";
    wrapped[w++] = "--chdir"; wrapped[w++] = run->scratch; wrapped[w++] = "--";
#endif
    for (int i = 0; i < argc && w < 78; i++) wrapped[w++] = argv[i];
    wrapped[w] = NULL;

    /* minimal environment: nothing from Lume's own environment leaks to the program */
    char home[1100], tmpdir[1100], path_env[] = "PATH=/usr/bin:/bin:/usr/sbin:/sbin:/opt/homebrew/bin:/usr/local/bin";
    snprintf(home, sizeof home, "HOME=%s", run->scratch);
    snprintf(tmpdir, sizeof tmpdir, "TMPDIR=%s", run->scratch);
    char *envp[] = { path_env, home, tmpdir, "LANG=C.UTF-8", "LC_ALL=C.UTF-8", "LUME_EXTRACTOR=1", NULL };
    char errlog[1100]; snprintf(errlog, sizeof errlog, "%s/stderr.txt", run->scratch);

    double t0 = now_s();
    pid_t pid = fork();
    if (pid < 0) { res->status = EXTRACT_SPAWN_FAILED; snprintf(res->message, sizeof res->message, "fork: %s", strerror(errno)); free_argv(argv); return res->status; }
    /* child: limits, stdin from /dev/null, stdout + stderr to stderr.txt, no inherited fds,
     * own session (so the whole process group can be killed), then exec */
    if (pid == 0) {
        struct rlimit rl;
        /* soft limit -> SIGXCPU; the hard limit (2 s later) -> SIGKILL. Equal limits made Linux
         * skip SIGXCPU, so the reason ("CPU limit") was lost */
        rl.rlim_cur = (rlim_t)l.cpu_secs; rl.rlim_max = (rlim_t)l.cpu_secs + 2; setrlimit(RLIMIT_CPU, &rl);
        rl.rlim_cur = rl.rlim_max = (rlim_t)l.out_mb << 20; setrlimit(RLIMIT_FSIZE, &rl);
        rl.rlim_cur = rl.rlim_max = (rlim_t)l.mem_mb << 20; setrlimit(RLIMIT_AS, &rl);   /* enforced where the kernel supports it */
#if !defined(__APPLE__)
        rl.rlim_cur = rl.rlim_max = 64; setrlimit(RLIMIT_NPROC, &rl);   /* per-user on macOS: would break fork for everyone */
#endif
        int in = open("/dev/null", O_RDONLY), out = open(errlog, O_WRONLY | O_CREAT | O_TRUNC, 0600);
        if (in >= 0) dup2(in, 0);
        if (out >= 0) { dup2(out, 1); dup2(out, 2); }
        for (int fd = 3; fd < 256; fd++) close(fd);
        if (chdir(run->scratch) != 0) _exit(126);
        setsid();
        execve(wrapped[0], wrapped, envp);
        _exit(127);
    }
    /* parent: poll every 15 ms for exit, wall-clock timeout, cancel via pump and memory cap */
    int status = 0; bool done = false, killed = false, cancelled = false, mem_killed = false;
    unsigned long long mem_peak = 0, mem_cap = (unsigned long long)l.mem_mb << 20;
    for (;;) {
        pid_t r = waitpid(pid, &status, WNOHANG);
        if (r == pid) { done = true; break; }
        if (r < 0 && errno != EINTR) break;
        double el = now_s() - t0;
        if (!killed && el > l.wall_secs) { kill(-pid, SIGKILL); kill(pid, SIGKILL); killed = true; }
        if (!killed && pump && !pump(ud)) { kill(-pid, SIGKILL); kill(pid, SIGKILL); killed = true; cancelled = true; }
        if (!killed && mem_cap) {
            unsigned long long m = group_memory(pid);
            if (m > mem_peak) mem_peak = m;
            if (m > mem_cap) { kill(-pid, SIGKILL); kill(pid, SIGKILL); killed = true; mem_killed = true; }
        }
        struct timespec ts = { 0, 15000000 };
        nanosleep(&ts, NULL);
    }
    /* classify the outcome, most specific reason first */
    res->secs = now_s() - t0;
    free_argv(argv);
    if (!done) { res->status = EXTRACT_SPAWN_FAILED; snprintf(res->message, sizeof res->message, "lost track of the process"); return res->status; }
    if (mem_killed) {
        res->status = EXTRACT_FAILED;
        snprintf(res->message, sizeof res->message, "stopped: memory use reached %llu MB, over the %ld MB limit; the process was killed (memory limit)", mem_peak >> 20, l.mem_mb);
        return res->status;
    }
    if (killed) {
        res->status = cancelled ? EXTRACT_CANCELLED : EXTRACT_TIMEOUT;
        snprintf(res->message, sizeof res->message, cancelled ? "cancelled after %.1f s; the process was killed" : "stopped after %.1f s (limit %d s); the process was killed", res->secs, l.wall_secs);
        return res->status;
    }
    if (WIFSIGNALED(status)) {
        res->status = EXTRACT_FAILED;
        snprintf(res->message, sizeof res->message, "killed by signal %d%s", WTERMSIG(status), WTERMSIG(status) == SIGXCPU ? " (CPU limit)" : WTERMSIG(status) == SIGXFSZ ? " (output size limit)" : "");
        return res->status;
    }
    res->exit_code = WEXITSTATUS(status);
#if !defined(__APPLE__)
    /* under bubblewrap the program is a grandchild: bwrap reports its death by signal N as
     * exit status 128 + N, so name the signal (and the limit behind it) like a direct child */
    if (res->exit_code > 128 && res->exit_code < 160) {
        int sig = res->exit_code - 128;
        res->status = EXTRACT_FAILED;
        snprintf(res->message, sizeof res->message, "killed by signal %d%s (exit status %d from the sandbox)", sig,
                 sig == SIGXCPU ? " (CPU limit)" : sig == SIGXFSZ ? " (output size limit)" : sig == SIGKILL ? " (killed: CPU or memory hard limit)" : "", res->exit_code);
        return res->status;
    }
#endif
    if (res->exit_code != 0) {
        res->status = EXTRACT_FAILED;
        char *log = read_all(errlog);
        char tail[160] = "";
        if (log) { size_t n = 0; for (const char *p = log; *p && n < sizeof tail - 1; p++) tail[n++] = (*p == '\n' || *p == '\r') ? ' ' : *p; tail[n] = 0; free(log); }
        snprintf(res->message, sizeof res->message, "exit status %d%s%s", res->exit_code, tail[0] ? ": " : "", tail);
        return res->status;
    }
    struct stat st;
    if (stat(run->outfile, &st) != 0 || !S_ISREG(st.st_mode)) {
        res->status = EXTRACT_NO_OUTPUT;
        snprintf(res->message, sizeof res->message, "the program exited 0 but wrote nothing to {out}");
        return res->status;
    }
    char err[200];
    if (!move_file(run->outfile, dest, err, sizeof err)) { res->status = EXTRACT_FAILED; snprintf(res->message, sizeof res->message, "%s", err); return res->status; }
    res->status = EXTRACT_OK;
    snprintf(res->message, sizeof res->message, "%lld bytes in %.1f s", (long long)st.st_size, res->secs);
    return res->status;
}

/* See extract.h. One tab-separated line appended per run; failures to open the log are
 * ignored. */
void extract_log(const char *dir, const Extractor *e, const ExtractResult *res, long rows, const char *dest) {
    char path[1200]; snprintf(path, sizeof path, "%sextractors-log.txt", dir);
    FILE *f = fopen(path, "a");
    if (!f) return;
    time_t now = time(NULL); struct tm tm; localtime_r(&now, &tm);
    char ts[40]; strftime(ts, sizeof ts, "%Y-%m-%dT%H:%M:%S", &tm);
    const char *base = dest ? strrchr(dest, '/') : NULL;
    base = base ? base + 1 : dest;
    fprintf(f, "%s\tname=%s\thash=%s\tstatus=%s\texit=%d\tsecs=%.2f\trows=%ld\tout=%s\n", ts, e->name, e->hash, extract_status_name(res->status), res->exit_code, res->secs, rows, base ? base : "-");
    fclose(f);
}
