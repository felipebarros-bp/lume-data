#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Crash regressions under AddressSanitizer: each scenario is a harness script that once crashed
# the app. A run passes only when ASan reports nothing and the process exits normally.
#   explorer_two_conns: two connections, the second active, double-click a table of the first
#   (switching connection rebuilt the tree and preview_table read the freed node; 31/10/2026).
#   objects_quit: quit with the object list loaded and an object tab edited (double free at exit, 31/10/2026).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
[ -f build-asan/CMakeCache.txt ] || cmake -S . -B build-asan -DCMAKE_BUILD_TYPE=Debug -DCMAKE_C_FLAGS="-fsanitize=address -fno-omit-frame-pointer" >/dev/null
cmake --build build-asan >/dev/null || { echo "FAIL  ASan build"; exit 1; }
fails=0
# two SQLite connections in a private preferences folder
P=build/crash-pref; rm -rf "$P"; mkdir -p "$P" build/ui
sqlite3 "$P/second.db" "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t1 VALUES (1, 'a'); CREATE VIEW v1 AS SELECT * FROM t1; CREATE TRIGGER tr1 AFTER INSERT ON t1 BEGIN SELECT 1; END;"
printf '[connection]\nid=demo1\nname=Demo (in-memory)\ndriver=sqlite\ndemo=1\npath=:memory:\n\n[connection]\nid=second1\nname=Second\ndriver=sqlite\npath=%s/%s/second.db\n' "$PWD" "$P" > "$P/connections.txt"
for s in explorer_two_conns objects_quit; do
  log=build/test-logs/crash_$s.log; mkdir -p build/test-logs
  ASAN_OPTIONS=abort_on_error=0:detect_leaks=0 LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$s.txt ./build-asan/lume > "$log" 2>&1
  rc=$?
  if [ $rc -eq 0 ] && ! grep -q 'ERROR: AddressSanitizer' "$log"; then echo "ok    $s: no crash, ASan clean"
  else echo "FAIL  $s (exit $rc): $(grep -m1 'ERROR: AddressSanitizer' "$log")"; grep -m4 '    #[0-9] ' "$log"; fails=$((fails + 1)); fi
done
echo "CRASH REGRESSIONS: $([ $fails -eq 0 ] && echo OK || echo FAIL) ($fails failures)"
exit $fails
