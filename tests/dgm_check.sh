#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# ER diagram model, headless under ASan (docs/diagrams.md): catalog from the 14k fixture, rules, layout,
# find, grid, file, exports; budgets measured on an optimised build (ASan would distort them).
cd "$(dirname "$0")/.."
mkdir -p build
[ -s build/dgm14k.db ] || python3 -I tests/tools/dgm_fixture.py build/dgm14k.db >/dev/null || { echo "FAIL  fixture"; exit 1; }
SRC="tests/dgm_check.c src/dgm.c src/dgm_layout.c src/dgm_io.c src/schidx.c src/json.c src/jw.c"
fails=0
cc -std=gnu11 -O2 -Wall -Wextra -Wno-unused-parameter -Isrc -o build/dgm_check $SRC -lsqlite3 -lm && build/dgm_check build/dgm14k.db || fails=1
# memory safety: the same checks under ASan / UBSan (timings not judged there)
if cc -std=gnu11 -O1 -g -fsanitize=address,undefined -fno-omit-frame-pointer -Wall -Isrc -DDGM_NO_BUDGETS -o build/dgm_check_asan $SRC -lsqlite3 -lm 2>/dev/null; then
  ASAN_OPTIONS=detect_leaks=0 build/dgm_check_asan build/dgm14k.db > build/dgm_check_asan.log 2>&1
  if grep -q "ERROR: AddressSanitizer\|runtime error" build/dgm_check_asan.log; then echo "FAIL  ASan / UBSan: see build/dgm_check_asan.log"; fails=1; else echo "ok    ASan / UBSan clean"; fi
fi
exit $fails
