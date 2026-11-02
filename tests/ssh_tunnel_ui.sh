#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# SSH tunnel (sshtun.c): offline checks only, no SSH server needed. A closed SSH port fails with ssh's
# own message in the connect dialog; a host that looks like an ssh option is refused before ssh runs;
# the dialog shows the tunnel and turning it off keeps the SSH settings saved; no ssh is left running.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/ssh-tunnel; rm -rf $P; mkdir -p $P
MARK=$PWD/build/ssh-tunnel-injected; rm -f "$MARK"
printf '[connection]\nid=sshrefused\nname=tunnel refused\ndriver=postgres\nhost=db.internal\nport=5432\ndatabase=x\nuser=x\nssh=1\nssh_host=127.0.0.1\nssh_port=1\nssh_user=\nssh_key=\n' > $P/connections.txt
printf '\n[connection]\nid=sshinject\nname=tunnel injection\ndriver=postgres\nhost=db.internal\nport=5432\ndatabase=x\nuser=x\nssh=1\nssh_host=-oProxyCommand=touch %s\nssh_port=\nssh_user=\nssh_key=\n' "$MARK" >> $P/connections.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/ssh_tunnel.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/ssh_tunnel.out
expect "a closed SSH port fails with ssh's message, said to be the tunnel" 'printf "%s\n" "$out" | grep "rawsql failed" | sed -n 1p | grep -q "SSH tunnel: .*refused"'
expect "a host that looks like an ssh option is refused, and nothing ran" 'printf "%s\n" "$out" | grep "rawsql failed" | sed -n 2p | grep -q "SSH host is missing or not valid" && [ ! -e "$MARK" ]'
expect "the connect dialog names the tunnel failure" 'printf "%s\n" "$out" | grep "^tbmodal" | sed -n 1p | grep -q "Could not connect to tunnel refused.*SSH tunnel"'
expect "editing shows the tunnel on, then the switch turns it off" 'printf "%s\n" "$out" | grep "^dialogstate" | sed -n 1p | grep -q "ssh=1" && printf "%s\n" "$out" | grep "^dialogstate" | sed -n 2p | grep -q "ssh=0"'
expect "saved off, the SSH settings are kept for later" '! grep -A12 "id=sshrefused" $P/connections.txt | grep -q "^ssh=1" && grep -A12 "id=sshrefused" $P/connections.txt | grep -q "^ssh_host=127.0.0.1"'
expect "no ssh process is left behind" '! pgrep -f "ssh -N -T .*127.0.0.1:[0-9]*:db.internal" >/dev/null'
expect "no script target missing" '! printf "%s\n" "$out" | grep -q "target not found"'
if [ $fails = 0 ]; then echo "SSH TUNNEL UI: all passed"; else echo "SSH TUNNEL UI: FAILED ($fails failures)"; fi
exit $fails
