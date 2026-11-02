#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Drives the Output panel with 100,000 synthetic events through the harness and prints frame
# times and RSS (OUT-024). Numbers go to build/test-logs/output_perf.log via run_all.sh.
set -e
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
cmake --build build >/dev/null
mkdir -p build/ui build/pref_perf
LUME_PREF_DIR=build/pref_perf LUME_SCRIPT=tests/scripts/output_perf.txt ./build/lume 2>/dev/null | grep '^stat' | tee build/output_perf.tmp
# bars from docs/benchmarks.md: frames under 16 ms (filled, scrolled), the one-off search
# rebuild under 250 ms (a single action), RSS under 150 MB. "after-shot" is reported only:
# each screenshot adds ~16 MB of Metal read-back and a slow frame of its own.
awk '
  /^stat / { step = $2; sub(":", "", step); ms = $4 + 0; rss = $7 + 0
    if (step == "after-shot") { print "info  " $0; next }
    lim = (step == "searched") ? 250 : 16
    if (ms <= lim) print "ok    " step ": frame " ms " ms (bar " lim " ms)"; else { print "FAIL  " step ": frame " ms " ms (bar " lim " ms)"; bad = 1 }
    if (rss <= 150) print "ok    " step ": rss " rss " MB (bar 150 MB)"; else { print "FAIL  " step ": rss " rss " MB (bar 150 MB)"; bad = 1 } }
  END { exit bad }' build/output_perf.tmp
command -v sips >/dev/null && sips -s format png build/ui/output_100k.bmp --out build/ui/output_100k.png >/dev/null && rm -f build/ui/output_100k.bmp
