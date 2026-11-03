/* SPDX-License-Identifier: AGPL-3.0-only */
/*
 * SSH tunnels for connections.
 *
 * The tunnel is the system's OpenSSH client (`ssh -N -L 127.0.0.1:<free port>:<db host>:<db port>`),
 * so it honours ~/.ssh/config (ProxyJump, aliases, IdentityFile...), the ssh-agent and the keys the
 * user already has. It runs with BatchMode=yes: no password or passphrase prompt (use a key, the
 * agent, or a passphrase kept in the agent / Keychain). An unknown host key is accepted once and
 * remembered (StrictHostKeyChecking=accept-new); a changed one is refused.
 *
 * Nothing secret goes on the command line: the key is a path, the database password never touches
 * the tunnel. ssh's own messages are kept only to explain a failure.
 */
#ifndef LUME_SSHTUN_H
#define LUME_SSHTUN_H
#include <stdbool.h>
#include <stddef.h>

/* Where the tunnel goes through. port 0 and an empty user/key mean "ssh's default / ~/.ssh/config". */
typedef struct SshSpec { const char *host, *user, *key; int port; } SshSpec;
typedef struct SshTunnel SshTunnel;

/* Starts ssh and waits (up to timeout_s, <= 0: 20 s) until the local end of the forward accepts
 * connections, i.e. ssh is authenticated and forwarding. On success returns the tunnel and the local
 * port in *local_port (the driver then connects to 127.0.0.1:*local_port). On failure returns NULL
 * with ssh's last message in err. Blocking: call it off the UI thread. */
SshTunnel *sshtun_open(const SshSpec *s, const char *dst_host, int dst_port, int timeout_s, int *local_port, char *err, size_t en);
/* Stops ssh (TERM, then KILL) and frees the tunnel. NULL is fine. */
void sshtun_close(SshTunnel *t);

#endif
