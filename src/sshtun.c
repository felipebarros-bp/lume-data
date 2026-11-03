/* SPDX-License-Identifier: AGPL-3.0-only */
/* SSH tunnels through the system's OpenSSH client: see sshtun.h. */
#include "sshtun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#define UNKNOWN_MARK "SSH tunnel: unknown host key "

/* See sshtun.h. Parses the message sshtun_open writes for an unknown host key:
 * "SSH tunnel: unknown host key <type> SHA256:<hash> for <host>". */
bool sshtun_unknown_key(const char *err, char *fp, size_t fpn, char *type, size_t tn) {
    if (!err || strncmp(err, UNKNOWN_MARK, sizeof UNKNOWN_MARK - 1) != 0) return false;
    const char *p = err + sizeof UNKNOWN_MARK - 1;
    const char *sp = strchr(p, ' ');
    if (!sp || strncmp(sp + 1, "SHA256:", 7) != 0) return false;
    const char *e = strchr(sp + 1, ' ');
    if (!e) e = sp + 1 + strlen(sp + 1);
    if (type) snprintf(type, tn, "%.*s", (int)(sp - p), p);
    if (fp) snprintf(fp, fpn, "%.*s", (int)(e - (sp + 1)), sp + 1);
    return true;
}

#ifdef _WIN32
struct SshTunnel { int unused; };
SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en) {
    (void)s; (void)dst_host; (void)dst_port; (void)timeout_s; (void)local_port;
    snprintf(err, en, "SSH tunnels are not available on Windows yet");
    return NULL;
}
void sshtun_close(SshTunnel *t) { (void)t; }
void sshtun_init(const char *dir) { (void)dir; }
void sshtun_kill_all(void) {}
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <pthread.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>
#ifdef __APPLE__
#include <libproc.h>
#endif

extern char **environ;

struct SshTunnel { pid_t pid; int port; };

/* ---- the running ssh processes ------------------------------------------------------------------
 * Each live tunnel's pid is kept here and in <prefs>/ssh-tunnels.pids. Lume stops them when it
 * quits (atexit) or crashes (sshtun_kill_all from the crash handler); after a kill -9 the next
 * launch finds the file and stops the ones left orphaned (still ssh, parent gone). */
static pthread_mutex_t reg_mu = PTHREAD_MUTEX_INITIALIZER;
static pid_t *volatile reg_pids;     /* read by the crash handler: only grown, never freed */
static volatile int reg_n;
static int reg_cap;
static char reg_file[1100];

static void reg_save_locked(void) {
    if (!reg_file[0]) return;
    if (reg_n == 0) { unlink(reg_file); return; }
    int fd = open(reg_file, O_WRONLY | O_CREAT | O_TRUNC | O_CLOEXEC, 0600);
    if (fd < 0) return;
    for (int i = 0; i < reg_n; i++) { char l[24]; int k = snprintf(l, sizeof l, "%d\n", (int)reg_pids[i]); if (write(fd, l, (size_t)k) < 0) break; }
    close(fd);
}
static void reg_add(pid_t pid) {
    pthread_mutex_lock(&reg_mu);
    if (reg_n == reg_cap) {   /* grown by copy: the old array stays valid for a handler reading it */
        int nc = reg_cap ? reg_cap * 2 : 8;
        pid_t *np = malloc(sizeof *np * (size_t)nc);
        if (!np) { pthread_mutex_unlock(&reg_mu); return; }
        for (int i = 0; i < reg_n; i++) np[i] = reg_pids[i];
        reg_pids = np; reg_cap = nc;   /* the old block is leaked on purpose (a few bytes, rare) */
    }
    reg_pids[reg_n] = pid; reg_n = reg_n + 1;
    reg_save_locked();
    pthread_mutex_unlock(&reg_mu);
}
static void reg_remove(pid_t pid) {
    pthread_mutex_lock(&reg_mu);
    for (int i = 0; i < reg_n; i++) if (reg_pids[i] == pid) { reg_pids[i] = reg_pids[reg_n - 1]; reg_n = reg_n - 1; break; }
    reg_save_locked();
    pthread_mutex_unlock(&reg_mu);
}

/* See sshtun.h. Only kill(): safe in a signal handler. */
void sshtun_kill_all(void) {
    pid_t *p = reg_pids; int n = reg_n;
    for (int i = 0; p && i < n; i++) { kill(-p[i], SIGTERM); kill(p[i], SIGTERM); }
}
static void kill_all_at_exit(void) {
    sshtun_kill_all();
    pthread_mutex_lock(&reg_mu); reg_n = 0; reg_save_locked(); pthread_mutex_unlock(&reg_mu);
}

/* A process left behind by an earlier run: still an ssh, and orphaned (its parent is gone). A pid
 * reused by anything else, or an ssh some other program runs, is never touched. */
static bool orphan_ssh(pid_t pid) {
#ifdef __APPLE__
    char name[64] = "";
    if (proc_name(pid, name, sizeof name) <= 0 || strcmp(name, "ssh") != 0) return false;
    struct proc_bsdinfo bi;
    if (proc_pidinfo(pid, PROC_PIDTBSDINFO, 0, &bi, sizeof bi) != (int)sizeof bi) return false;
    return bi.pbi_ppid == 1;
#else
    char path[64], buf[512]; snprintf(path, sizeof path, "/proc/%d/stat", (int)pid);
    FILE *f = fopen(path, "re"); if (!f) return false;
    size_t n = fread(buf, 1, sizeof buf - 1, f); fclose(f); buf[n] = 0;
    char *l = strchr(buf, '('), *r = strrchr(buf, ')');
    if (!l || !r || r - l - 1 != 3 || strncmp(l + 1, "ssh", 3) != 0) return false;
    char st; int ppid = 0;
    if (sscanf(r + 1, " %c %d", &st, &ppid) != 2) return false;
    return ppid == 1;
#endif
}

/* See sshtun.h. */
void sshtun_init(const char *dir) {
    static bool once;
    if (!once) { once = true; atexit(kill_all_at_exit); }
    pthread_mutex_lock(&reg_mu);
    snprintf(reg_file, sizeof reg_file, "%sssh-tunnels.pids", dir ? dir : "");
    FILE *f = fopen(reg_file, "re");
    if (f) {
        int pid;
        while (fscanf(f, "%d", &pid) == 1) if (pid > 1 && orphan_ssh(pid)) { kill(-pid, SIGTERM); kill(pid, SIGTERM); }
        fclose(f);
    }
    reg_save_locked();   /* this run's list (empty: removes the old file) */
    pthread_mutex_unlock(&reg_mu);
}

/* ---- starting ssh ---------------------------------------------------------------------------- */

/* Host, user and paths go to ssh as arguments: no option look-alikes, no whitespace or control
 * characters (ssh would read "-oProxyCommand=..." as an option). */
static bool safe_arg(const char *s, bool allow_empty) {
    if (!s || !*s) return allow_empty;
    if (s[0] == '-') return false;
    for (const unsigned char *p = (const unsigned char *)s; *p; p++) if (*p <= ' ' || *p == 0x7F) return false;
    return true;
}

/* A port nobody listens on right now (the kernel's pick); ssh binds it a moment later and
 * ExitOnForwardFailure turns the rare race into a clear error. */
static int free_port(void) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return 0;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = 0;
    socklen_t l = sizeof a; int port = 0;
    if (!bind(fd, (struct sockaddr *)&a, sizeof a) && !getsockname(fd, (struct sockaddr *)&a, &l)) port = ntohs(a.sin_port);
    close(fd);
    return port;
}
/* The readiness probe: the local end of the forward accepts a TCP connection. ssh opens its -L
 * listener only after the host key was checked and the login succeeded (ExitOnForwardFailure), so
 * an accepted connection means "authenticated and forwarding". The probe is a bare connect and
 * close: ssh opens a channel to the database for it, which the database sees as a client that
 * went away before speaking (no credentials, no query). */
static bool port_ready(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return false;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    bool ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}
static void nap_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* A private temporary file (close-on-exec, 0600), unlinked unless the path is wanted back. */
static int temp_file(char *path, size_t pn, bool keep_path) {
    const char *tmp = getenv("TMPDIR"); char tpl[1100];
    snprintf(tpl, sizeof tpl, "%s/lume-ssh-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int fd = mkstemp(tpl);
    if (fd < 0) return -1;
    fcntl(fd, F_SETFD, FD_CLOEXEC);
    if (keep_path && path) snprintf(path, pn, "%s", tpl); else unlink(tpl);
    return fd;
}
static void read_all(int fd, char *buf, size_t n) {
    ssize_t k = pread(fd, buf, n - 1, 0); if (k < 0) k = 0; buf[k] = 0;
}

/* ssh's last meaningful line from its stderr file, as the error ("SSH tunnel: " + message).
 * strtok_r: this runs on the connect worker. */
static void last_message(int fd, char *err, size_t en, const char *fallback) {
    char buf[4096]; read_all(fd, buf, sizeof buf);
    char *best = NULL, *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        while (*line == ' ') line++;
        if (!*line || !strncmp(line, "debug", 5) || !strncmp(line, "Warning: Permanently added", 26)) continue;
        best = line;
    }
    snprintf(err, en, "SSH tunnel: %s", best ? best : fallback);
}

/* Starts ssh with stdin / stdout on /dev/null and stderr into efd (plus out_fd as stdout when >= 0).
 * The child inherits no other descriptor of Lume's, and runs in its own process group. */
static int spawn(const char *const *av, int efd, int out_fd, pid_t *pid) {
    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    if (out_fd >= 0) posix_spawn_file_actions_adddup2(&fa, out_fd, 1);
    else posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, efd, 2);
#if !defined(__APPLE__) && defined(__GLIBC__) && (__GLIBC__ > 2 || (__GLIBC__ == 2 && __GLIBC_MINOR__ >= 34))
    posix_spawn_file_actions_addclosefrom_np(&fa, 3);   /* elsewhere Lume opens its own descriptors close-on-exec */
#endif
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    short fl = POSIX_SPAWN_SETPGROUP;   /* its own group: closed as a whole */
#ifdef __APPLE__
    fl |= POSIX_SPAWN_CLOEXEC_DEFAULT;  /* only 0, 1 and 2 reach ssh: no socket, database file or pipe of Lume's */
#endif
    posix_spawnattr_setflags(&at, fl); posix_spawnattr_setpgroup(&at, 0);
    const char *ssh = av[0];
    int rc = ssh[0] == '/' ? posix_spawn(pid, ssh, &fa, &at, (char *const *)av, environ) : posix_spawnp(pid, ssh, &fa, &at, (char *const *)av, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    return rc;
}

static void stop(pid_t pid) {
    kill(-pid, SIGTERM); kill(pid, SIGTERM);
    for (int i = 0; i < 40; i++) { if (waitpid(pid, NULL, WNOHANG) != 0) return; nap_ms(25); }
    kill(-pid, SIGKILL); kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}
/* Waits for a short-lived child (up to ms), stopping it when it takes longer. */
static void wait_done(pid_t pid, int ms) {
    for (int w = 0; w < ms; w += 25) { if (waitpid(pid, NULL, WNOHANG) != 0) return; nap_ms(25); }
    stop(pid);
}

/* The known_hosts file a trusted key is added to: LUME_SSH_KNOWN_HOSTS (the tests' own file),
 * else ~/.ssh/known_hosts. */
static bool known_hosts_path(char *o, size_t n) {
    const char *env = getenv("LUME_SSH_KNOWN_HOSTS");
    if (env && *env) { snprintf(o, n, "%s", env); return true; }
    const char *home = getenv("HOME");
    if (!home || !*home) return false;
    char dir[1100]; snprintf(dir, sizeof dir, "%s/.ssh", home);
    if (mkdir(dir, 0700) != 0 && errno != EEXIST) return false;
    snprintf(o, n, "%s/known_hosts", dir);
    return true;
}

/* The fingerprint ssh saw for an unknown host: a separate, short ssh -v that stops at the host key
 * check (strict checking: nothing is accepted, no login is tried). With a ProxyJump the jump host's
 * key is logged first, so the last "Server host key" line is the target's. */
static bool probe_fingerprint(const char *const *base, int nbase, char *type, size_t tn, char *fp, size_t fpn) {
    const char *av[64]; int n = 0;
    av[n++] = base[0]; av[n++] = "-v";
    for (int i = 1; i < nbase && n < 63; i++) av[n++] = base[i];
    av[n] = NULL;
    int efd = temp_file(NULL, 0, false); if (efd < 0) return false;
    pid_t pid = 0;
    if (spawn(av, efd, -1, &pid) != 0) { close(efd); return false; }
    wait_done(pid, 15000);
    char buf[16384]; read_all(efd, buf, sizeof buf); close(efd);
    bool found = false; char *save = NULL;
    for (char *line = strtok_r(buf, "\r\n", &save); line; line = strtok_r(NULL, "\r\n", &save)) {
        char *k = strstr(line, "Server host key: "); if (!k) continue;
        char t[64], f[128];
        if (sscanf(k + 17, "%63s %127s", t, f) == 2 && !strncmp(f, "SHA256:", 7)) { snprintf(type, tn, "%s", t); snprintf(fp, fpn, "%s", f); found = true; }
    }
    return found;
}

/* Whether the key ssh stored in kh (a one-host known_hosts) has the approved fingerprint. */
static bool key_matches(const char *kh, const char *fp) {
    const char *kg = access("/usr/bin/ssh-keygen", X_OK) == 0 ? "/usr/bin/ssh-keygen" : "ssh-keygen";
    const char *av[] = { kg, "-l", "-E", "sha256", "-f", kh, NULL };
    int ofd = temp_file(NULL, 0, false); if (ofd < 0) return false;
    int efd = temp_file(NULL, 0, false); if (efd < 0) { close(ofd); return false; }
    pid_t pid = 0; bool ok = false;
    if (spawn(av, efd, ofd, &pid) == 0) {
        wait_done(pid, 10000);
        char buf[4096]; read_all(ofd, buf, sizeof buf);
        size_t fl = strlen(fp); bool any = false, all = true; char *save = NULL;
        for (char *line = strtok_r(buf, "\n", &save); line; line = strtok_r(NULL, "\n", &save)) {   /* "256 SHA256:xxx host (ED25519)" */
            char *s = strchr(line, ' '); if (!s) continue;
            any = true;
            if (strncmp(s + 1, fp, fl) != 0 || (s[1 + fl] != ' ' && s[1 + fl] != 0)) all = false;
        }
        ok = any && all;
    }
    close(ofd); close(efd);
    return ok;
}

/* Appends the trusted entry ssh wrote to the temporary file onto the user's known_hosts. */
static bool remember_key(const char *tmp_kh) {
    char dst[1100]; if (!known_hosts_path(dst, sizeof dst)) return false;
    int in = open(tmp_kh, O_RDONLY | O_CLOEXEC); if (in < 0) return false;
    char buf[8192]; ssize_t n = read(in, buf, sizeof buf); close(in);
    if (n <= 0) return false;
    int out = open(dst, O_WRONLY | O_CREAT | O_APPEND | O_CLOEXEC, 0600);
    if (out < 0) return false;
    bool ok = write(out, buf, (size_t)n) == n;
    close(out);
    return ok;
}

SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en) {
    *local_port = 0;
    if (!s || !safe_arg(s->host, false)) { snprintf(err, en, "SSH tunnel: the SSH host is missing or not valid"); return NULL; }
    if (!safe_arg(s->user, true)) { snprintf(err, en, "SSH tunnel: the SSH user is not valid"); return NULL; }
    if (!safe_arg(dst_host, false) || dst_port <= 0 || dst_port > 65535) { snprintf(err, en, "SSH tunnel: the database host or port is not valid"); return NULL; }
    if (s->port < 0 || s->port > 65535) { snprintf(err, en, "SSH tunnel: the SSH port is not valid"); return NULL; }
    bool trusting = s->trust_fp && *s->trust_fp;
    if (trusting && (strncmp(s->trust_fp, "SHA256:", 7) != 0 || !safe_arg(s->trust_fp, false))) { snprintf(err, en, "SSH tunnel: the approved fingerprint is not valid"); return NULL; }
    /* the key: "~/" is the home folder; it must exist (ssh would only warn and try the others) */
    char key[1100] = "";
    if (s->key && *s->key) {
        const char *home = getenv("HOME");
        if (!strncmp(s->key, "~/", 2) && home) snprintf(key, sizeof key, "%s/%s", home, s->key + 2); else snprintf(key, sizeof key, "%s", s->key);
        if (!safe_arg(key, false)) { snprintf(err, en, "SSH tunnel: the key file path is not valid"); return NULL; }
        if (access(key, R_OK) != 0) { snprintf(err, en, "SSH tunnel: cannot read the key file %s", key); return NULL; }
    }
    char khopt[1200] = "";
    const char *khenv = getenv("LUME_SSH_KNOWN_HOSTS");
    if (khenv && *khenv) {
        if (!safe_arg(khenv, false)) { snprintf(err, en, "SSH tunnel: LUME_SSH_KNOWN_HOSTS is not a usable path"); return NULL; }
        snprintf(khopt, sizeof khopt, "UserKnownHostsFile=%s", khenv);
    }
    /* trusting a key the user approved: ssh records it in a private temporary known_hosts, checked
     * against the approved fingerprint before anything uses the tunnel */
    char tmp_kh[1100] = "", tkopt[1200] = "";
    if (trusting) {
        int kfd = temp_file(tmp_kh, sizeof tmp_kh, true);
        if (kfd < 0) { snprintf(err, en, "SSH tunnel: %s", strerror(errno)); return NULL; }
        close(kfd);
        if (!safe_arg(tmp_kh, false)) { unlink(tmp_kh); snprintf(err, en, "SSH tunnel: the temporary folder path is not usable"); return NULL; }
        snprintf(tkopt, sizeof tkopt, "UserKnownHostsFile=%s", tmp_kh);
    }
    const char *ssh = access("/usr/bin/ssh", X_OK) == 0 ? "/usr/bin/ssh" : "ssh";
    int port = free_port();
    if (!port) { if (tmp_kh[0]) unlink(tmp_kh); snprintf(err, en, "SSH tunnel: no free local port"); return NULL; }

    /* ssh's stderr goes to an unlinked private file: read on failure, never shown otherwise */
    int efd = temp_file(NULL, 0, false);
    if (efd < 0) { if (tmp_kh[0]) unlink(tmp_kh); snprintf(err, en, "SSH tunnel: %s", strerror(errno)); return NULL; }

    char fwd[400], sport[16];
    bool v6 = strchr(dst_host, ':') != NULL;
    snprintf(fwd, sizeof fwd, v6 ? "127.0.0.1:%d:[%s]:%d" : "127.0.0.1:%d:%s:%d", port, dst_host, dst_port);
    snprintf(sport, sizeof sport, "%d", s->port);
    const char *av[48]; int n = 0;
    av[n++] = ssh; av[n++] = "-N"; av[n++] = "-T";
    av[n++] = "-o"; av[n++] = "BatchMode=yes";
    av[n++] = "-o"; av[n++] = "ExitOnForwardFailure=yes";
    av[n++] = "-o"; av[n++] = "ConnectTimeout=10";
    av[n++] = "-o"; av[n++] = "ServerAliveInterval=30";
    av[n++] = "-o"; av[n++] = "ServerAliveCountMax=3";
    /* never accept an unknown host key silently: strict checking, and a key the user approved
     * (by its fingerprint) goes through the temporary file above */
    av[n++] = "-o"; av[n++] = trusting ? "StrictHostKeyChecking=accept-new" : "StrictHostKeyChecking=yes";
    if (trusting) { av[n++] = "-o"; av[n++] = tkopt; }
    else if (khopt[0]) { av[n++] = "-o"; av[n++] = khopt; }
    av[n++] = "-o"; av[n++] = "ClearAllForwardings=no";
    av[n++] = "-o"; av[n++] = "ControlMaster=no";   /* its own connection: a shared master would hold the forward after Lume closes it */
    av[n++] = "-o"; av[n++] = "ControlPath=none";
    int fwd_at = n;
    av[n++] = "-L"; av[n++] = fwd;
    if (s->port > 0) { av[n++] = "-p"; av[n++] = sport; }
    if (s->user && *s->user) { av[n++] = "-l"; av[n++] = s->user; }
    if (key[0]) { av[n++] = "-i"; av[n++] = key; av[n++] = "-o"; av[n++] = "IdentitiesOnly=yes"; }
    av[n++] = "--"; av[n++] = s->host; av[n] = NULL;

    pid_t pid = 0;
    int rc = spawn(av, efd, -1, &pid);
    if (rc != 0) { close(efd); if (tmp_kh[0]) unlink(tmp_kh); snprintf(err, en, "SSH tunnel: could not start ssh (%s)", strerror(rc)); return NULL; }
    reg_add(pid);

    int limit_ms = (timeout_s > 0 ? timeout_s : 20) * 1000;
    for (int waited = 0;; waited += 50) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {   /* ssh gave up: its message says why */
            reg_remove(pid);
            char buf[4096]; read_all(efd, buf, sizeof buf);
            if (!trusting && strstr(buf, "Host key verification failed") && strstr(buf, "host key is known")) {
                /* unknown host: fetch its fingerprint for the user to compare (the same arguments
                 * without the forward: the probe never logs in) */
                const char *pv[48]; int pn = 0;
                for (int i = 0; i < n; i++) { if (i == fwd_at) { i++; continue; } pv[pn++] = av[i]; }
                char type[64], fp[128];
                if (probe_fingerprint(pv, pn, type, sizeof type, fp, sizeof fp)) snprintf(err, en, UNKNOWN_MARK "%s %s for %s", type, fp, s->host);
                else snprintf(err, en, "SSH tunnel: the host key of %s is not known and its fingerprint could not be read", s->host);
            } else last_message(efd, err, en, "ssh exited before the tunnel was ready");
            close(efd); if (tmp_kh[0]) unlink(tmp_kh); return NULL;
        }
        if (port_ready(port)) break;
        if (waited >= limit_ms) {
            stop(pid); reg_remove(pid);
            last_message(efd, err, en, "not ready in time (is the SSH host reachable?)");
            close(efd); if (tmp_kh[0]) unlink(tmp_kh); return NULL;
        }
        nap_ms(50);
    }
    close(efd);   /* ssh keeps writing to its own copy of the file, which is already unlinked */
    if (trusting) {   /* the key ssh met must be the one the user approved; only then is it remembered */
        bool same = key_matches(tmp_kh, s->trust_fp);
        bool kept = same && remember_key(tmp_kh);
        unlink(tmp_kh);
        if (!same) { stop(pid); reg_remove(pid); snprintf(err, en, "SSH tunnel: the host key of %s is not the one you approved; nothing was sent through it", s->host); return NULL; }
        if (!kept) { stop(pid); reg_remove(pid); snprintf(err, en, "SSH tunnel: could not add the host key to known_hosts"); return NULL; }
    }
    SshTunnel *t = calloc(1, sizeof *t);
    if (!t) { stop(pid); reg_remove(pid); snprintf(err, en, "SSH tunnel: out of memory"); return NULL; }
    t->pid = pid; t->port = port;
    *local_port = port;
    return t;
}

void sshtun_close(SshTunnel *t) {
    if (!t) return;
    if (t->pid > 0) { stop(t->pid); reg_remove(t->pid); }
    free(t);
}
#endif
