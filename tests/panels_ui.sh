#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Panels and docking on screen (LUME-PANEL-001): drives the shell through the harness with two
# connections, then checks the layout file across restarts, including a corrupt one.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/panels-pref; rm -rf "$P"; mkdir -p "$P" build/ui
sqlite3 "$P/second.db" "CREATE TABLE t1(id INTEGER PRIMARY KEY, v TEXT); INSERT INTO t1 VALUES (1, 'from second');"
printf '[connection]\nid=demo1\nname=Demo (in-memory)\ndriver=sqlite\ndemo=1\npath=:memory:\n\n[connection]\nid=second1\nname=Second\ndriver=sqlite\npath=%s/%s/second.db\n' "$PWD" "$P" > "$P/connections.txt"
fails=0
run() {   # script, keep-layout
  [ "$2" = keep ] || rm -f "$P/layout.json"
  out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1)
  missing=$(printf '%s\n' "$out" | grep '^script: target not found' || true)
  if [ -n "$missing" ]; then echo "FAIL  $1: $missing"; fails=$((fails + 1)); else echo "ok    $1: every target found"; fi
}
run panels_a; run panels_b; run panels_c; run panels_e
grep -q '"name"\|Reviewing' "$P/layouts/Reviewing.json" 2>/dev/null && echo "ok    named layout saved as layouts/Reviewing.json" || { echo "FAIL  named layout file"; fails=$((fails + 1)); }
grep -q "Reviewing" "$P/layouts/bindings.txt" 2>/dev/null && echo "ok    layout bound to a connection (layouts/bindings.txt)" || { echo "FAIL  binding file"; fails=$((fails + 1)); }
grep -q '"schemaVersion": 1' build/ui/exported-layout.json && ! grep -qiE 'SELECT|password' build/ui/exported-layout.json && echo "ok    exported layout: schema 1, no SQL or secrets" || { echo "FAIL  exported layout"; fails=$((fails + 1)); }
printf 'focus_mode=Cmd+Shift+K\n' > "$P/keymap.txt"; run panels_f; rm -f "$P/keymap.txt"
grep -q '"focusMode": true' "$P/layout.json" && echo "ok    keymap.txt override: Cmd+Shift+K turned focus mode on" || { echo "FAIL  keymap override"; fails=$((fails + 1)); }
rm -rf "$P/layouts"; run panels_g
n_open=$(printf '%s\n' "$out" | grep -c '^floatpos open')
back=$(printf '%s\n' "$out" | grep '^floatpos open' | sed -n 2p)
[ "$n_open" -ge 3 ] && echo "ok    separate and floating windows opened (native windows, same panel instances)" || { echo "FAIL  panel windows did not open ($n_open)"; fails=$((fails + 1)); }
case "$back" in *-30000*) echo "FAIL  window left off-screen after the display change: $back"; fails=$((fails + 1));; *) echo "ok    window moved off every display came back on screen ($back)";; esac
grep -q 'docked back into the main window' "$P/logs/lume.log" && echo "ok    closing a panel window docks the panel back" || { echo "FAIL  close did not dock back"; fails=$((fails + 1)); }
# regression (crash 2026-10-31): hiding the last panel of a group from the bar, mid-frame
rm -f "$P/layout.json"
printf '{"schemaVersion": 1, "regions": {"left": [{"tabs": ["lume.data.explorer"], "active": "lume.data.explorer", "weight": 1.0, "collapsed": false, "lane": 0}], "right": [{"tabs": ["lume.data.inspector"], "active": "lume.data.inspector", "weight": 1.0, "collapsed": false, "lane": 0}], "bottom": [{"tabs": ["lume.data.results"], "active": "lume.data.results", "weight": 1.0, "collapsed": false, "lane": 0}, {"tabs": ["lume.data.history"], "active": "lume.data.history", "weight": 1.0, "collapsed": false, "lane": 0}], "top": []}}' > "$P/layout.json"
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_hide_last.txt ./build/lume >"$P/hide_last.out" 2>&1; rc=$?
if [ $rc -eq 0 ] && ! grep -q 'target not found' "$P/hide_last.out" && grep -q 'Query History hidden' "$P/logs/lume.log"; then echo "ok    hiding the only panel of a group from the bar and its menu: no crash, group drawn empty"
else echo "FAIL  hide last panel of a group (exit $rc)"; fails=$((fails + 1)); fi
run panels_h
grep -q '"tabs": \["lume.data.output", "lume.data.results"\]' "$P/layout.json" && echo "ok    a tab dropped on another tab lands at that position (PNL-010)" || { echo "FAIL  tab reorder by drag"; fails=$((fails + 1)); }
run panels_d1
grep -q '"lume.data.output"' "$P/layout.json" && grep -q '"collapsed": true' "$P/layout.json" \
  && echo "ok    layout.json written after the changes (Output on the right, Explorer collapsed)" || { echo "FAIL  layout.json content"; fails=$((fails + 1)); }
grep -qiE 'SELECT|password|0x[0-9a-f]{6}' "$P/layout.json" && { echo "FAIL  layout.json holds SQL, a secret or a pointer"; fails=$((fails + 1)); } || echo "ok    layout.json holds ids, modes and sizes only"
run panels_d2 keep
printf '{"schemaVersion": 1, "regionSize": {"left": NaN}}' > "$P/layout.json"
run panels_d3 keep
ls "$P"/layout.json.bad-* >/dev/null 2>&1 && echo "ok    corrupt layout.json kept aside as layout.json.bad-*" || { echo "FAIL  corrupt layout not kept"; fails=$((fails + 1)); }
grep -q 'layout file rejected' "$P/logs/lume.log" && echo "ok    the rejection is in the log with its reason" || { echo "FAIL  no log line"; fails=$((fails + 1)); }
# PNL-011: split across the region's axis (lanes): drag with preview, menu without drag, lane
# splitter, persistence, normalisation when a lane empties
rm -f "$P/layout.json"; : > "$P/logs/lume.log"
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_lane.txt ./build/lume >/dev/null 2>&1
lanes() { python3 -I -c "
import json, sys
d = json.load(open('$P/layout.json'))
left = d['regions']['left']
print(' '.join(t + ':' + str(g.get('lane', 0)) for g in left for t in g['tabs']), '|', ' '.join('%.3f' % w for w in d['laneWeight']['left']))"; }
st=$(lanes)
case "$st" in *"lume.data.explorer:0"*"lume.data.output:2"*"lume.data.inspector:1"*) echo "ok    drag onto a group's side and Split > Side by side: three lanes in the left region ($st)";;
  *) echo "FAIL  lanes after drag + menu: $st"; fails=$((fails + 1));; esac
w1=$(printf '%s' "$st" | sed 's/.*| //')
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_lane_drag.txt ./build/lume >/dev/null 2>&1
w2=$(lanes | sed 's/.*| //')
[ -n "$w2" ] && [ "$w1" != "$w2" ] && echo "ok    lane splitter dragged: weights $w1 -> $w2 (other lanes keep theirs)" || { echo "FAIL  lane splitter drag: $w1 -> $w2"; fails=$((fails + 1)); }
grep -q 'layout file rejected' "$P/logs/lume.log" && { echo "FAIL  a layout was rejected"; fails=$((fails + 1)); }
: > "$P/logs/lume.log"
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_lane_b.txt ./build/lume >/dev/null 2>&1
grep -q 'layout restored from' "$P/logs/lume.log" && ! grep -q 'layout file rejected' "$P/logs/lume.log" && echo "ok    restart: the lanes load back from layout.json" || { echo "FAIL  lanes restore"; fails=$((fails + 1)); }
python3 -I -c "
import json, sys
d = json.load(open('$P/layout.json'))
left = d['regions']['left']
tabs = {t: g.get('lane', 0) for g in left for t in g['tabs']}
ok = 'lume.data.output' not in tabs and sorted(set(tabs.values())) == [0, 1] and len(d['laneWeight']['left']) == 2
print(tabs, d['laneWeight']['left']); sys.exit(0 if ok else 1)" > build/ui/lanes.txt 2>&1 \
  && echo "ok    Output moved out: its lane is gone and the others are renumbered ($(cat build/ui/lanes.txt))" || { echo "FAIL  normalisation: $(cat build/ui/lanes.txt)"; fails=$((fails + 1)); }

# PNL-047: a non-SQL module (Markdown) through the extension API: factory, lifecycle, own menu
# items, move + persist, restore, unregister at run time, module disabled
rm -f "$P/layout.json" "$P/notes.md"; : > "$P/logs/lume.log"
run panels_ext_a
L="$P/logs/lume.log"
grep -q 'module lume.markdown: init' "$L" && grep -q 'panel lume.md.outline registered' "$L" && echo "ok    module registered its panels through panels_register (no change to the shell)" || { echo "FAIL  module registration"; fails=$((fails + 1)); }
grep -q 'markdown: outline view created' "$L" && grep -q 'markdown: outline shown' "$L" && grep -q 'markdown: stats hidden (polling stopped)' "$L" && echo "ok    view factory on first use; visibility hook on show and hide" || { echo "FAIL  factory / visibility hooks"; fails=$((fails + 1)); }
grep -q 'markdown: outline copied (5 headings)' "$L" && echo "ok    the panel's own context-menu item ran (Copy outline)" || { echo "FAIL  module menu item"; fails=$((fails + 1)); }
python3 -I -c "
import json, sys
d = json.load(open('$P/layout.json'))
left = [t for g in d['regions']['left'] for t in g['tabs']]
st = {p['id']: p['mode'] for p in d['panels']}
sys.exit(0 if 'lume.md.outline' in left and st.get('lume.md.stats') == 'hidden' else 1)" && echo "ok    moved to the left and hidden: persisted in layout.json like any panel" || { echo "FAIL  module panel persistence"; fails=$((fails + 1)); }
cp "$P/layout.json" "$P/layout-with-md.json"; : > "$L"
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_ext_b.txt ./build/lume 2>&1)
grep -q 'markdown: outline view created' "$L" && echo "ok    restart: the module's panel is restored where it was left (registered before the layout is read)" || { echo "FAIL  restore of a module panel"; fails=$((fails + 1)); }
printf '%s\n' "$out" | grep -q 'unregister lume.md.outline ok' && grep -q 'markdown: outline view destroyed' "$L" && grep -q 'unregistered (module disabled); layout valid' "$L" \
  && echo "ok    run-time unregister: hidden, destroyed, layout valid (no dangling reference)" || { echo "FAIL  run-time unregister"; fails=$((fails + 1)); }
printf '%s\n' "$out" | grep -q 'unregister lume.data.results refused' && echo "ok    a Lume Data panel cannot be unregistered" || { echo "FAIL  data panel unregister not refused"; fails=$((fails + 1)); }
grep -q 'markdown: stats view created' "$L" && echo "ok    the remaining module panel still opens after the indices were compacted" || { echo "FAIL  panel after compaction"; fails=$((fails + 1)); }
cp "$P/layout-with-md.json" "$P/layout.json"; : > "$L"
LUME_DISABLE_MODULES=lume.markdown LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_ext_c.txt ./build/lume >/dev/null 2>&1
grep -q 'module lume.markdown disabled' "$L" && grep -q 'layout restored from' "$L" && ! grep -q 'layout file rejected' "$L" \
  && echo "ok    module disabled: a layout naming its panels still loads, without them" || { echo "FAIL  disabled module"; fails=$((fails + 1)); }
printf 'lume.markdown=off\n' > "$P/modules.txt"; : > "$L"
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_ext_c.txt ./build/lume >/dev/null 2>&1
grep -q 'module lume.markdown disabled' "$L" && echo "ok    modules.txt turns a module off as well" || { echo "FAIL  modules.txt"; fails=$((fails + 1)); }
rm -f "$P/modules.txt" "$P/layout-with-md.json"

# PNL-041: frames on screen right after panel operations, and CPU while idle
rm -f "$P/layout.json"; rm -rf "$P/layouts"
frames=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_perf.txt ./build/lume 2>/dev/null | awk '/^stat op/ { print $4 + 0 }' | sort -n)
n=$(printf '%s\n' "$frames" | grep -c .); p95=$(printf '%s\n' "$frames" | awk -v n="$n" 'NR == int(n * 0.95 + 0.5) { print }'); mx=$(printf '%s\n' "$frames" | tail -1)
awk -v p="$p95" -v m="$mx" -v n="$n" 'BEGIN { ok = n >= 20 && p <= 16; printf "%s  panel operations on screen: %d frames, p95 %.2f ms, max %.2f ms (bar 16 ms)\n", ok ? "ok  " : "FAIL", n, p, m; exit !ok }' || fails=$((fails + 1))
printf 'wait 4000\nquit\n' > build/ui/idle.txt
LUME_PREF_DIR=$P LUME_SCRIPT=build/ui/idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
sleep 3; cpu=$(ps -o %cpu= -p $pid | tr -d ' '); wait $pid
awk -v c="$cpu" 'BEGIN { ok = c + 0 <= 2.0; printf "%s  idle CPU with the panels laid out: %s %% (bar 2 %%: no redraw loop)\n", ok ? "ok  " : "FAIL", c; exit !ok }' || fails=$((fails + 1))
LUME_REDUCE_MOTION=1 LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/panels_d2.txt ./build/lume >/dev/null 2>&1
grep -q 'reduce motion is on' "$P/logs/lume.log" && echo "ok    reduce motion honoured (LUME_REDUCE_MOTION=1; the macOS setting is read the same way)" || { echo "FAIL  reduce motion"; fails=$((fails + 1)); }
for f in build/ui/pnl_*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
echo "PANELS UI: $([ $fails -eq 0 ] && echo OK || echo FAIL) ($fails failures)"
exit $fails
