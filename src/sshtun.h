/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * SSH tunnels for connections.
 *
 * The tunnel is the system's OpenSSH client (`ssh -N -L 127.0.0.1:<free port>:<db host>:<db port>`),
 * so it honours ~/.ssh/config (ProxyJump, aliases, IdentityFile...), the ssh-agent and the keys the
 * user already has. It runs with BatchMode=yes: no password or passphrase prompt (use a key, the
 * agent, or a passphrase kept in the agent / Keychain). Host keys are checked strictly: an unknown
 * one fails with its fingerprint (sshtun_unknown_key), the user compares it and approves, and the
 * next open with SshSpec.trust_fp set accepts exactly that key and adds it to known_hosts; a changed
 * key is always refused.
 *
 * ssh inherits no descriptor of Lume's but stdin/stdout/stderr, and never outlives Lume for long:
 * the tunnels are stopped when Lume quits or crashes, and a launch after a kill -9 stops the ssh
 * processes the previous run left orphaned (sshtun_init).
 *
 * Nothing secret goes on the command line: the key is a path, the database password never touches
 * the tunnel. ssh's own messages are kept only to explain a failure.
 */
#ifndef LUME_SSHTUN_H
#define LUME_SSHTUN_H
#include <stdbool.h>
#include <stddef.h>

/* Where the tunnel goes through. port 0 and an empty user/key mean "ssh's default / ~/.ssh/config". */
/* trust_fp: a fingerprint ("SHA256:...") the user approved for an unknown host, NULL / "" otherwise. */
typedef struct SshSpec { const char *host, *user, *key; int port; const char *trust_fp; } SshSpec;
typedef struct SshTunnel SshTunnel;

/* Starts ssh and waits (up to timeout_s, <= 0: 20 s) until the local end of the forward accepts
 * connections, i.e. ssh is authenticated and forwarding. On success returns the tunnel and the local
 * port in *local_port (the driver then connects to 127.0.0.1:*local_port). On failure returns NULL
 * with ssh's last message in err. Blocking: call it off the UI thread. */
SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en);
/* Stops ssh (TERM, then KILL) and frees the tunnel. NULL is fine. */
void sshtun_close(SshTunnel *t);
/* Whether err is sshtun_open's "unknown host key" failure; then fp gets "SHA256:..." and type the key
 * type (both may be NULL). */
bool sshtun_unknown_key(const char *err, char *fp, size_t fpn, char *type, size_t tn);
/* At startup, with the prefs folder (ending in '/'): stops the tunnels a killed earlier run left
 * behind, and arranges for this run's to stop when Lume exits. */
void sshtun_init(const char *dir);
/* Stops every running tunnel's ssh. Async-signal-safe (the crash handler calls it). */
void sshtun_kill_all(void);

#endif
