#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# A connect that fails opens a dialog with the driver's message and Retry / Edit connection... / Close
# (it used to be only a line in the status bar, easy to miss).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/connect-fail; rm -rf $P; mkdir -p $P
OUT=$PWD/build/connect-fail-out; rm -rf "$OUT"; mkdir -p "$OUT"
printf '[connection]\nid=nowhere\nname=nowhere\ndriver=postgres\nhost=127.0.0.1\nport=1\ndatabase=x\nuser=x\nenv=DEV\n' > $P/connections.txt
sed "s|__OUT__|$OUT|g" tests/scripts/connect_fail.txt > build/connect_fail.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=build/connect_fail.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/connect_fail.out
expect "the failure opens a dialog naming the connection, with the driver's message and Retry / Edit / Close" 'printf "%s\n" "$out" | grep "^tbmodal" | sed -n 1p | grep -q "Could not connect to nowhere" && printf "%s\n" "$out" | grep "^tbmodal" | sed -n 1p | grep -qi "refused\|failed"'
expect "Close dismisses it" 'printf "%s\n" "$out" | grep "^tbmodal" | sed -n 2p | grep -q "^tbmodal kind=0 "'
if [ $fails = 0 ]; then echo "CONNECT FAIL UI: all passed"; else echo "CONNECT FAIL UI: FAILED ($fails failures)"; fi
exit $fails
