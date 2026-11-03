#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Crash reports with consent: a crash leaves a report; the next launch asks (default), and View report
# shows the exact event, Don't send deletes it, Send report posts it (Sentry format, GlitchTip) with no
# path, user or host; "never" drops it silently, "always" sends without asking. Loopback mock only.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
REQ=build/crash_requests.jsonl; rm -f $REQ
pf=build/crash_mock.port; rm -f $pf; python3 -I tests/tools/telemetry_mock.py "$REQ" > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
export LUME_CRASH_URL="http://127.0.0.1:$(head -1 $pf)/api/29464/store/"
P=build/crash-prefs
run() { LUME_PREF_DIR=$P LUME_SCRIPT=$1 ./build/lume 2>&1; }
pending() { [ -s $P/logs/crash-pending.txt ]; }
# 1. ask (the default): view, then don't send
rm -rf $P; mkdir -p $P
run tests/scripts/crash_sim.txt >/dev/null
expect "a crash leaves its report for the next launch" 'pending && grep -q "^signal=11" $P/logs/crash-pending.txt && grep -q "^stack:" $P/logs/crash-pending.txt'
out=$(run tests/scripts/crash_ask_view_drop.txt)
expect "next launch asks (the default): Send report / View report / Don't send" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "Lume Data closed unexpectedly"'
expect "View report opens the exact event in a tab (Sentry format: exception, stack frames)" 'printf "%s\n" "$out" | grep "^edtext" | grep -q "\"exception\":{\"values\":\[{\"type\":\"SIGSEGV\"" && printf "%s\n" "$out" | grep "^edtext" | grep -q "\"frames\":\[{"'
expect "after viewing, the decision is still open" 'printf "%s\n" "$out" | grep "^tbmodal" | sed -n 2p | grep -q "closed unexpectedly"'
expect "Don't send: deleted, nothing sent" '! pending && [ ! -s $REQ ]'
# 2. ask: send
run tests/scripts/crash_sim.txt >/dev/null
out=$(run tests/scripts/crash_ask_send.txt); sleep 1
expect "Send report: posted to the store endpoint with the project's key, then deleted" 'grep -q "\"path\": \"/api/29464/store/\"" $REQ && grep -q "sentry_key=dc45" $REQ && ! pending'
expect "the event: release, OS, arch, the signal and named frames" 'grep -qF "lume-data@$(sed -n "s/^project(lume VERSION \([0-9.]*\).*/\1/p" CMakeLists.txt)" $REQ && grep -q "SIGSEGV" $REQ && grep -q "log_write_crash_report" $REQ'
expect "nothing private: no path, user name, host name or home folder" '! grep -qE "/Users/|/home/|$(whoami)|$(hostname -s)|$HOME|$PWD" $REQ'
# 3. never: dropped silently
n0=$(wc -l < $REQ); printf 'crash_reports=2\n' > $P/settings.txt
run tests/scripts/crash_sim.txt >/dev/null; out=$(run tests/scripts/crash_idle.txt); sleep 1
expect "never send: no dialog, the report is deleted, nothing sent" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -qv "closed unexpectedly" && ! pending && [ "$(wc -l < $REQ)" = "$n0" ]'
# 4. always: sent without asking
printf 'crash_reports=1\n' > $P/settings.txt
run tests/scripts/crash_sim.txt >/dev/null; out=$(run tests/scripts/crash_idle.txt); sleep 1
expect "always send: no dialog, sent, deleted" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -qv "closed unexpectedly" && ! pending && [ "$(wc -l < $REQ)" -gt "$n0" ]'
kill $mock 2>/dev/null; wait $mock 2>/dev/null
if [ $fails = 0 ]; then echo "CRASH REPORT UI: all passed"; else echo "CRASH REPORT UI: FAILED ($fails failures)"; fi
exit $fails
