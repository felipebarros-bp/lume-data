#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Anonymous usage statistics: nothing leaves when the user has not turned them on; with consent, the
# start event and the previous session's summary go out, carrying counts and ranges only (no SQL,
# names, hosts, users or paths). A loopback mock stands in for the endpoint.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
REQ=build/telemetry_requests.jsonl; rm -f $REQ
pf=build/telemetry_mock.port; rm -f $pf; python3 -I tests/tools/telemetry_mock.py "$REQ" > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
export LUME_TELEMETRY_URL="http://127.0.0.1:$(head -1 $pf)/api/v0/events"
run() { LUME_PREF_DIR=$1 LUME_SCRIPT=$2 ./build/lume 2>&1; }
# 1. default (never asked): nothing is sent, nothing is kept
P=build/telemetry-off; rm -rf $P; mkdir -p $P
out=$(run $P tests/scripts/telemetry_a.txt); out=$(run $P tests/scripts/telemetry_b.txt); sleep 1
expect "off by default: nothing reached the endpoint" '[ ! -s $REQ ]'
expect "off by default: no pending summary kept on disk" '[ ! -e $P/telemetry-pending.json ]'
# 2. with consent
P=build/telemetry-on; rm -rf $P; mkdir -p $P; printf 'telemetry=1\n' > $P/settings.txt
a=$(run $P tests/scripts/telemetry_a.txt)
expect "with consent: the session counts the query and the plan" 'printf "%s\n" "$a" | grep -q "^telemetry on=1 queries=1 plans=1"'
expect "the summary waits on disk for the next launch (quitting never waits on the network)" '[ -s $P/telemetry-pending.json ]'
b=$(run $P tests/scripts/telemetry_b.txt); sleep 1
expect "next launch: the start event and the previous session's summary went out" 'grep -q "session_summary" $REQ && grep -q "app_started" $REQ'
expect "Aptabase's request shape: /api/v0/events, App-Key, JSON array" 'grep -q "\"path\": \"/api/v0/events\"" $REQ && grep -q "\"app_key\": \"A-EU-" $REQ && grep -q "\"body\": \"\\[{" $REQ'
expect "counts are sent in ranges, never exact" 'grep -q "queries.*1-9" $REQ && grep -q "plans.*1-9" $REQ'
expect "nothing private: no SQL, table, city, connection name, path, user or host" '! grep -qiE "SELECT|customers|Salvador|Demo|in-memory|$(whoami)|$(hostname -s)|$PWD|$HOME" $REQ'
# 3. turned off again: nothing more goes out
printf 'telemetry=0\n' > $P/settings.txt; n0=$(wc -l < $REQ)
c=$(run $P tests/scripts/telemetry_a.txt); c=$(run $P tests/scripts/telemetry_b.txt); sleep 1
expect "turned off again: no further request" '[ "$(wc -l < $REQ)" = "$n0" ]'
kill $mock 2>/dev/null; wait $mock 2>/dev/null
if [ $fails = 0 ]; then echo "TELEMETRY UI: all passed"; else echo "TELEMETRY UI: FAILED ($fails failures)"; fi
exit $fails
