#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Drives the result grid through the event harness: server-side sort/filter, layout (freeze,
# wrap), value viewer with the JSON tree, formats, export dialog, history, columns panel and
# the memory budget. Screenshots land in build/ui/*.png for a visual check.
# Note: every screenshot inflates RSS by ~16 MB (Metal read-back staging); the number in the
# status bar of a run with several shots is not the app's idle footprint.
set -e
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
cmake --build build >/dev/null
rm -rf build/ui build/pref && mkdir -p build/ui build/pref && : > build/ui/stats.txt
# a registered (not yet approved) extractor so the export dialog shows the approval flow
printf 'copy\t/bin/cp {file} {out}\n' > build/pref/extractors.txt
for s in grid_server grid_layout grid_viewer grid_budget grid_edit grid_v3 grid_output grid_edit_tx output_prefs1 output_prefs2 grid_local grid_menus grid_evidence; do
  # every script starts from the Default layout (layout.json persists between launches);
  # output_prefs2 deliberately reuses what output_prefs1 saved
  [ "$s" = output_prefs2 ] || rm -rf build/pref/layout.json build/pref/layouts build/pref/output-prefs.txt
  LUME_PREF_DIR=build/pref LUME_SCRIPT=tests/scripts/$s.txt ./build/lume | tee -a build/ui/stats.txt | grep '^stat' || true
done
# bar for the on-screen grid (docs/benchmarks.md): the frame after a 200-notch scroll over
# 800,000 rows under 16 ms, the app under 150 MB with that tab open. One run's RSS varies by
# several MB on this machine (BL-047: 145..152 MB for the same build), so the scenario runs three
# times and the bars apply to the median; the bars themselves are unchanged.
grep '^stat grid-scrolled-800k' build/ui/stats.txt > build/ui/stats_800k.txt || true
for i in 2 3; do
  rm -rf build/pref/layout.json build/pref/layouts build/pref/output-prefs.txt
  LUME_PREF_DIR=build/pref LUME_SCRIPT=tests/scripts/grid_budget.txt ./build/lume | grep '^stat grid-scrolled-800k' >> build/ui/stats_800k.txt || true
done
awk '{ ms[NR] = $4 + 0; rss[NR] = $7 + 0; all = all " " $7 }
  END { if (NR == 0) { print "FAIL  grid_budget printed no stat line"; exit 1 }
    n = asort_ms(ms, NR); r = asort_ms(rss, NR)
    print "info  800k rows rss samples (MB):" all
    if (n <= 16) print "ok    grid scroll over 800k rows: median frame " n " ms (bar 16 ms)"; else { print "FAIL  grid scroll over 800k rows: median frame " n " ms (bar 16 ms)"; bad = 1 }
    if (r <= 150) print "ok    grid with 800k rows: median rss " r " MB of " NR " runs (bar 150 MB)"; else { print "FAIL  grid with 800k rows: median rss " r " MB of " NR " runs (bar 150 MB)"; bad = 1 }
    exit bad }
  function asort_ms(a, n,   i, j, t) { for (i = 1; i <= n; i++) for (j = i + 1; j <= n; j++) if (a[j] < a[i]) { t = a[i]; a[i] = a[j]; a[j] = t } return a[int((n + 1) / 2)] }' build/ui/stats_800k.txt
[ ! -e build/ui/pending_export.csv ] || { echo "FAIL: export produced a file while rows were unsaved"; exit 1; }
echo "ok    export refused while rows are unsaved (no build/ui/pending_export.csv)"
for f in build/ui/*.bmp; do
  command -v sips >/dev/null && sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"
done
ls build/ui
