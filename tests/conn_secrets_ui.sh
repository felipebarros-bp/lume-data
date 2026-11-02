#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Connection passwords live in the OS secret store: a password found in secrets.txt moves there once
# (and the file goes), it comes back on the next launch, and deleting the connection removes it.
# Fake password only, in the test service "Lume Data Test" (never the user's entries).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible LUME_SECSTORE_SERVICE="Lume Data Test" LUME_TEST_PW_STORE=1
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
[ "$(uname -s)" = Darwin ] || { echo "skip  the secret-store check reads the macOS Keychain with security(1); other systems: BL-058"; echo "CONN SECRETS UI: skipped"; exit 0; }
# a fresh id per run, and only the entry's attributes are read (never its secret): security(1) is not the
# app that made the entry, so reading the password itself would make macOS ask for the login password
ID="fk$(( ($$ + $(date +%s)) % 100000000 ))"   # at most 11 characters (the connection id field)
ACC="connection:$ID:password"
exists() { security find-generic-password -s "Lume Data Test" -a "$ACC" >/dev/null 2>&1; }
P=build/conn-secrets; rm -rf $P; mkdir -p $P
printf '[connection]\nid=%s\nname=Fake PG\ndriver=postgres\nhost=127.0.0.1\nport=1\ndatabase=x\nuser=x\n' "$ID" > $P/connections.txt
( umask 077; printf '%s\tpassword\tfake-pw-not-real-123\n' "$ID" > $P/secrets.txt )
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/conn_secrets_a.txt ./build/lume >/dev/null 2>&1
expect "first launch: the password moved to the secret store" 'exists'
expect "and secrets.txt is gone (nothing was left in it)" '[ ! -e $P/secrets.txt ]'
expect "connections.txt never holds the password" '! grep -q "fake-pw-not-real-123" $P/connections.txt'
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/conn_secrets_b.txt ./build/lume 2>&1)
expect "next launch: the password comes back from the store (all 5 fields filled in the editor)" 'printf "%s\n" "$out" | grep "^dialogstate" | head -1 | grep -q "edit=1 .*filled=5/5"'
expect "deleting the connection removes its password from the store" '! exists'
expect "no script target missing" '! printf "%s\n" "$out" | grep -q "target not found"'
if [ $fails = 0 ]; then echo "CONN SECRETS UI: all passed"; else echo "CONN SECRETS UI: FAILED ($fails failures)"; fi
exit $fails
