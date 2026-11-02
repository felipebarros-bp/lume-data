/* SPDX-License-Identifier: AGPL-3.0-only */
/* SSH tunnels through the system's OpenSSH client: see sshtun.h. */
#include "sshtun.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>

#ifdef _WIN32
struct SshTunnel { int unused; };
SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en) {
    (void)s; (void)dst_host; (void)dst_port; (void)timeout_s; (void)local_port;
    snprintf(err, en, "SSH tunnels are not available on Windows yet");
    return NULL;
}
void sshtun_close(SshTunnel *t) { (void)t; }
#else
#include <arpa/inet.h>
#include <errno.h>
#include <fcntl.h>
#include <netinet/in.h>
#include <signal.h>
#include <spawn.h>
#include <sys/socket.h>
#include <sys/stat.h>
#include <sys/wait.h>
#include <time.h>
#include <unistd.h>

extern char **environ;

struct SshTunnel { pid_t pid; int port; };

/* Host, user and key path go to ssh as arguments: no option look-alikes, no whitespace or control
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
/* The local end accepts a connection: ssh is authenticated and forwarding. */
static bool port_ready(int port) {
    int fd = socket(AF_INET, SOCK_STREAM, 0); if (fd < 0) return false;
    struct sockaddr_in a; memset(&a, 0, sizeof a); a.sin_family = AF_INET; a.sin_addr.s_addr = htonl(INADDR_LOOPBACK); a.sin_port = htons((uint16_t)port);
    bool ok = connect(fd, (struct sockaddr *)&a, sizeof a) == 0;
    close(fd);
    return ok;
}
static void nap_ms(int ms) { struct timespec ts = { ms / 1000, (long)(ms % 1000) * 1000000L }; nanosleep(&ts, NULL); }

/* ssh's last meaningful line from its stderr file, as the error ("ssh: " + message). */
static void last_message(int fd, char *err, size_t en, const char *fallback) {
    char buf[4096]; ssize_t n = pread(fd, buf, sizeof buf - 1, 0); if (n < 0) n = 0; buf[n] = 0;
    char *best = NULL;
    for (char *line = strtok(buf, "\r\n"); line; line = strtok(NULL, "\r\n")) {
        while (*line == ' ') line++;
        if (!*line || !strncmp(line, "Warning: Permanently added", 26)) continue;   /* accept-new's note, not the cause */
        best = line;
    }
    snprintf(err, en, "SSH tunnel: %s", best ? best : fallback);
}

static void stop(pid_t pid) {
    kill(-pid, SIGTERM); kill(pid, SIGTERM);
    for (int i = 0; i < 40; i++) { if (waitpid(pid, NULL, WNOHANG) != 0) return; nap_ms(25); }
    kill(-pid, SIGKILL); kill(pid, SIGKILL);
    waitpid(pid, NULL, 0);
}

SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en) {
    *local_port = 0;
    if (!s || !safe_arg(s->host, false)) { snprintf(err, en, "SSH tunnel: the SSH host is missing or not valid"); return NULL; }
    if (!safe_arg(s->user, true)) { snprintf(err, en, "SSH tunnel: the SSH user is not valid"); return NULL; }
    if (!safe_arg(dst_host, false) || dst_port <= 0 || dst_port > 65535) { snprintf(err, en, "SSH tunnel: the database host or port is not valid"); return NULL; }
    if (s->port < 0 || s->port > 65535) { snprintf(err, en, "SSH tunnel: the SSH port is not valid"); return NULL; }
    /* the key: "~/" is the home folder; it must exist (ssh would only warn and try the others) */
    char key[1100] = "";
    if (s->key && *s->key) {
        const char *home = getenv("HOME");
        if (!strncmp(s->key, "~/", 2) && home) snprintf(key, sizeof key, "%s/%s", home, s->key + 2); else snprintf(key, sizeof key, "%s", s->key);
        if (!safe_arg(key, false)) { snprintf(err, en, "SSH tunnel: the key file path is not valid"); return NULL; }
        if (access(key, R_OK) != 0) { snprintf(err, en, "SSH tunnel: cannot read the key file %s", key); return NULL; }
    }
    const char *ssh = access("/usr/bin/ssh", X_OK) == 0 ? "/usr/bin/ssh" : "ssh";
    int port = free_port();
    if (!port) { snprintf(err, en, "SSH tunnel: no free local port"); return NULL; }

    /* ssh's stderr goes to an unlinked private file: read on failure, never shown otherwise */
    const char *tmp = getenv("TMPDIR"); char tpl[1100];
    snprintf(tpl, sizeof tpl, "%s/lume-ssh-XXXXXX", tmp && *tmp ? tmp : "/tmp");
    int efd = mkstemp(tpl);
    if (efd < 0) { snprintf(err, en, "SSH tunnel: %s", strerror(errno)); return NULL; }
    unlink(tpl);

    char fwd[400], sport[16];
    bool v6 = strchr(dst_host, ':') != NULL;
    snprintf(fwd, sizeof fwd, v6 ? "127.0.0.1:%d:[%s]:%d" : "127.0.0.1:%d:%s:%d", port, dst_host, dst_port);
    snprintf(sport, sizeof sport, "%d", s->port);
    const char *av[40]; int n = 0;
    av[n++] = ssh; av[n++] = "-N"; av[n++] = "-T";
    av[n++] = "-o"; av[n++] = "BatchMode=yes";
    av[n++] = "-o"; av[n++] = "ExitOnForwardFailure=yes";
    av[n++] = "-o"; av[n++] = "ConnectTimeout=10";
    av[n++] = "-o"; av[n++] = "ServerAliveInterval=30";
    av[n++] = "-o"; av[n++] = "ServerAliveCountMax=3";
    av[n++] = "-o"; av[n++] = "StrictHostKeyChecking=accept-new";
    av[n++] = "-o"; av[n++] = "ClearAllForwardings=no";
    av[n++] = "-o"; av[n++] = "ControlMaster=no";   /* its own connection: a shared master would hold the forward after Lume closes it */
    av[n++] = "-o"; av[n++] = "ControlPath=none";
    av[n++] = "-L"; av[n++] = fwd;
    if (s->port > 0) { av[n++] = "-p"; av[n++] = sport; }
    if (s->user && *s->user) { av[n++] = "-l"; av[n++] = s->user; }
    if (key[0]) { av[n++] = "-i"; av[n++] = key; av[n++] = "-o"; av[n++] = "IdentitiesOnly=yes"; }
    av[n++] = "--"; av[n++] = s->host; av[n] = NULL;

    posix_spawn_file_actions_t fa; posix_spawn_file_actions_init(&fa);
    posix_spawn_file_actions_addopen(&fa, 0, "/dev/null", O_RDONLY, 0);
    posix_spawn_file_actions_addopen(&fa, 1, "/dev/null", O_WRONLY, 0);
    posix_spawn_file_actions_adddup2(&fa, efd, 2);
    posix_spawnattr_t at; posix_spawnattr_init(&at);
    posix_spawnattr_setflags(&at, POSIX_SPAWN_SETPGROUP); posix_spawnattr_setpgroup(&at, 0);   /* its own group: closed as a whole */
    pid_t pid = 0;
    int rc = ssh[0] == '/' ? posix_spawn(&pid, ssh, &fa, &at, (char *const *)av, environ) : posix_spawnp(&pid, ssh, &fa, &at, (char *const *)av, environ);
    posix_spawn_file_actions_destroy(&fa); posix_spawnattr_destroy(&at);
    if (rc != 0) { close(efd); snprintf(err, en, "SSH tunnel: could not start ssh (%s)", strerror(rc)); return NULL; }

    int limit_ms = (timeout_s > 0 ? timeout_s : 20) * 1000;
    for (int waited = 0;; waited += 50) {
        int st = 0;
        if (waitpid(pid, &st, WNOHANG) == pid) {   /* ssh gave up: its message says why */
            last_message(efd, err, en, "ssh exited before the tunnel was ready");
            close(efd); return NULL;
        }
        if (port_ready(port)) break;
        if (waited >= limit_ms) {
            stop(pid);
            last_message(efd, err, en, "not ready in time (is the SSH host reachable?)");
            close(efd); return NULL;
        }
        nap_ms(50);
    }
    close(efd);   /* ssh keeps writing to its own copy of the file, which is already unlinked */
    SshTunnel *t = calloc(1, sizeof *t);
    if (!t) { stop(pid); snprintf(err, en, "SSH tunnel: out of memory"); return NULL; }
    t->pid = pid; t->port = port;
    *local_port = port;
    return t;
}

void sshtun_close(SshTunnel *t) {
    if (!t) return;
    if (t->pid > 0) stop(t->pid);
    free(t);
}
#endif
