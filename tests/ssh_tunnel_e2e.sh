#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# SSH tunnel end to end against a throwaway sshd on 127.0.0.1 (the system's /usr/sbin/sshd, run as
# this user with its own host keys; nothing of the user's ~/.ssh is read or written: the tests point
# Lume at their own known_hosts with LUME_SSH_KNOWN_HOSTS). Skipped when there is no sshd.
# With the dbv-postgres container up, a real query also goes through the tunnel.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
SSHD=/usr/sbin/sshd
[ -x $SSHD ] || { echo "SSH TUNNEL E2E: skipped (no $SSHD)"; exit 0; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
D=$PWD/build/ssh-e2e; rm -rf $D; mkdir -p $D/prefs
ssh-keygen -q -t ed25519 -N '' -f $D/hostkey1 -C lume-test-host1
ssh-keygen -q -t ed25519 -N '' -f $D/hostkey2 -C lume-test-host2
ssh-keygen -q -t ed25519 -N '' -f $D/userkey -C lume-test-user
cp $D/userkey.pub $D/authorized_keys; chmod 600 $D/authorized_keys
SPORT=$(python3 -c 'import socket; s=socket.socket(); s.bind(("127.0.0.1",0)); print(s.getsockname()[1])')
start_sshd() {   # $1 = host key
  [ -f $D/sshd.pid ] && kill "$(cat $D/sshd.pid)" 2>/dev/null; sleep 0.3
  cat > $D/sshd_config <<CFG
Port $SPORT
ListenAddress 127.0.0.1
HostKey $1
AuthorizedKeysFile $D/authorized_keys
PidFile $D/sshd.pid
UsePAM no
StrictModes no
PasswordAuthentication no
KbdInteractiveAuthentication no
AllowTcpForwarding yes
CFG
  $SSHD -f $D/sshd_config -E $D/sshd.log
  for i in 1 2 3 4 5 6 7 8 9 10; do nc -z 127.0.0.1 $SPORT 2>/dev/null && return 0; sleep 0.2; done
  return 1
}
stop_sshd() { [ -f $D/sshd.pid ] && kill "$(cat $D/sshd.pid)" 2>/dev/null; }
trap stop_sshd EXIT
start_sshd $D/hostkey1 || { echo "SSH TUNNEL E2E: skipped (sshd did not start: $(tail -1 $D/sshd.log))"; exit 0; }
FP1=$(ssh-keygen -l -E sha256 -f $D/hostkey1.pub | awk '{print $2}')
FP2=$(ssh-keygen -l -E sha256 -f $D/hostkey2.pub | awk '{print $2}')
export LUME_SSH_KNOWN_HOSTS=$D/known_hosts
: > $LUME_SSH_KNOWN_HOSTS

# the tunnel target: the test PostgreSQL when it runs, else the sshd itself (the driver then fails
# after the tunnel is up, which is enough for the host-key checks)
ENV_FILE="${LUME_PG_ENV:-$HOME/workspace/tessarotto/test-servers/pg/.env}"
PG=0; DPORT=$SPORT
if docker exec dbv-postgres true 2>/dev/null && [ -f "$ENV_FILE" ]; then PG=1; DPORT=15432; fi
P=$D/prefs
conn() { printf '[connection]\nid=%s\nname=%s\ndriver=postgres\nhost=127.0.0.1\nport=%s\ndatabase=postgres\nuser=postgres\nssh=1\nssh_host=127.0.0.1\nssh_port=%s\nssh_user=\nssh_key=%s\n\n' "$1" "$2" "$3" "$SPORT" "$D/userkey"; }
{ conn e2e "tunnel e2e" $DPORT; conn pg "tunnel pg" $DPORT; } > $P/connections.txt
if [ $PG = 1 ]; then
  PW=$(grep '^PG_PASSWORD=' "$ENV_FILE" | cut -d= -f2- | tr -d '"')
  ( umask 077; printf 'e2e\tpassword\t%s\npg\tpassword\t%s\n' "$PW" "$PW" > $P/secrets.txt )
fi
run() { LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1; }
tunnels_left() { pgrep -f "ssh -N -T .*-p $SPORT" >/dev/null; }

# ---- run 1: the trust question, from the connect failure ----
out=$(run ssh_e2e_modal); printf '%s\n' "$out" > $D/run1.out
m1=$(printf '%s\n' "$out" | grep '^tbmodal' | sed -n 1p)
m2=$(printf '%s\n' "$out" | grep '^tbmodal' | sed -n 2p)
expect "an unknown host key is not accepted: Lume asks, showing the server's real fingerprint" 'printf "%s\n" "$m1" | grep -q "Trust the SSH host 127.0.0.1?" && printf "%s\n" "$m1" | grep -qF "$FP1"'
expect "Trust and Connect: the key is added to known_hosts, exactly the approved one" '[ "$(ssh-keygen -l -E sha256 -f $LUME_SSH_KNOWN_HOSTS | awk "{print \$2}" | sort -u)" = "$FP1" ]'
if [ $PG = 1 ]; then
  expect "through the tunnel the connection opens (no failure dialog after trusting)" 'printf "%s\n" "$m2" | grep -q "^tbmodal kind=0"'
  expect "a second connection through the now-known host runs a query" 'printf "%s\n" "$out" | grep -q "^rawsql ok"'
else
  expect "after trusting, a failure is the database's, not the tunnel's" 'printf "%s\n" "$m2" | grep -q "Could not connect" && ! printf "%s\n" "$m2" | grep -q "SSH tunnel"'
fi
expect "Duplicate Connection keeps the SSH tunnel settings" 'printf "%s\n" "$out" | grep "^dialogstate" | sed -n 1p | grep -q "edit=0 .*ssh=1"'
expect "no ssh left running after quitting" '! tunnels_left'

# ---- run 2: the server's key changed ----
start_sshd $D/hostkey2 || echo "FAIL  sshd restart"
out=$(run ssh_e2e_changed); printf '%s\n' "$out" > $D/run2.out
m1=$(printf '%s\n' "$out" | grep '^tbmodal' | sed -n 1p)
expect "a changed host key is refused, with no trust question" 'printf "%s\n" "$m1" | grep -q "Could not connect to tunnel e2e.*SSH tunnel: Host key verification failed" && ! printf "%s\n" "$m1" | grep -q "Trust the SSH host"'
expect "known_hosts still holds only the old key" '[ "$(ssh-keygen -l -E sha256 -f $LUME_SSH_KNOWN_HOSTS | awk "{print \$2}" | sort -u)" = "$FP1" ]'

# ---- run 3: the trust question from Test connection in the dialog ----
: > $LUME_SSH_KNOWN_HOSTS
out=$(run ssh_e2e_dialog); printf '%s\n' "$out" > $D/run3.out
d1=$(printf '%s\n' "$out" | grep '^dialogstate' | sed -n 1p)
d2=$(printf '%s\n' "$out" | grep '^dialogstate' | sed -n 2p)
expect "Test connection names the unknown key and its fingerprint" 'printf "%s\n" "$d1" | grep -q "msg_ok=0 msg=\"Unknown SSH host key ssh-ed25519 $FP2"'
expect "Trust Host Key: the test goes through the tunnel and the key is remembered" '! printf "%s\n" "$d2" | grep -q "SSH" && [ "$(ssh-keygen -l -E sha256 -f $LUME_SSH_KNOWN_HOSTS | awk "{print \$2}" | sort -u)" = "$FP2" ]'
[ $PG = 1 ] && expect "the dialog test connects" 'printf "%s\n" "$d2" | grep -q "msg_ok=1 msg=\"✓ Connected"'

# (with PostgreSQL, run 1 connects after trusting, so its second "Close" has no dialog to close)
expect "every script target found" '! grep "target not found" $D/run1.out $D/run2.out $D/run3.out | grep -v "run1.out:.*button \"Close\"" | grep -q .'

# ---- run 4: an ssh orphaned by a killed Lume is stopped at the next launch; nothing else is ----
sshargs="-N -T -o BatchMode=yes -o UserKnownHostsFile=$LUME_SSH_KNOWN_HOSTS -o StrictHostKeyChecking=yes -i $D/userkey -p $SPORT 127.0.0.1"
sh -c "ssh $sshargs </dev/null >/dev/null 2>&1 & echo \$! > $D/orphan.pid"
ssh $sshargs </dev/null >/dev/null 2>&1 & child=$!
sleep 300 & other=$!
sleep 1; orphan=$(cat $D/orphan.pid)
printf '%s\n%s\n%s\n' "$orphan" "$child" "$other" > $P/ssh-tunnels.pids
if [ "$(ps -o ppid= -p $orphan | tr -d ' ')" = 1 ]; then
  run ssh_e2e_quit >/dev/null
  sleep 0.5
  expect "the orphaned ssh of an earlier run is stopped at launch" '! kill -0 $orphan 2>/dev/null'
  expect "an ssh with a live parent and a non-ssh pid in the list are left alone" 'kill -0 $child 2>/dev/null && kill -0 $other 2>/dev/null'
  expect "the list is cleared" '[ ! -f $P/ssh-tunnels.pids ]'
else echo "skip  orphan sweep (orphans are not re-parented to pid 1 here)"; fi
kill $orphan $child $other 2>/dev/null; wait $child $other 2>/dev/null

if [ $fails = 0 ]; then echo "SSH TUNNEL E2E: all passed"; else echo "SSH TUNNEL E2E: FAILED ($fails failures)"; fi
exit $fails
