#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# New Connection dialog: the database picker (search + Enter) before the form, PROD suggests
# read-only, Test reports beside its button, editing changes the type through the picker.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/conn-dialog; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/conn_dialog.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/conn_dialog.out
s() { printf '%s\n' "$out" | grep '^dialogstate' | sed -n "$1p"; }
expect "a new connection starts with the database picker" 's 1 | grep -q "open=1 step=0 edit=0"'
expect "typing pg and Enter opens the PostgreSQL form, its defaults as placeholders (empty fields)" 's 2 | grep -q "step=1 edit=0 focus=1 driver=postgres filled=0/5"'
expect "Enter in Host does not connect yet: it moves on to the empty Password (defaults count as filled)" 's 3 | grep -q "open=1 step=1 edit=0 focus=5 driver=postgres"'
expect "choosing PROD turns Read-only in Lume on, and says so" 's 4 | grep -q "env=3 ro=1 .*msg=\"PROD: Read-only"'
expect "Test reports its outcome beside the button (here: the local server refuses or accepts)" 's 6 | grep -q "testing=0 " && ! s 6 | grep -q "msg=\"\""'
expect "Escape closes the dialog" 's 7 | grep -q "open=0"'
expect "editing opens the form directly, the type as it is" 's 8 | grep -q "open=1 step=1 edit=1 focus=[0-9]* driver=sqlite"'
expect "Change... goes through the picker: SQL Server found by mssql" 's 9 | grep -q "step=1 edit=1 focus=[0-9]* driver=sqlserver"'
expect "no script target missing" '! printf "%s\n" "$out" | grep -q "target not found"'
if [ $fails = 0 ]; then echo "CONN DIALOG UI: all passed"; else echo "CONN DIALOG UI: FAILED ($fails failures)"; fi
exit $fails
