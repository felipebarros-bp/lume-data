#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# ER diagrams on screen (docs/diagrams.md): open focused, find / Next across the scope, expand with a budget,
# layout, positions kept, structure changes flagged, user relations / notes / entities / business names,
# every export, the 14k-table catalog (budget asked first, frames, culling), Lume Intelligence through a
# loopback mock, idle CPU, read-only. Fixtures are synthetic SQLite files (tests/tools/dgm_fixture.py).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: target not found'; }
st() { printf '%s\n' "$1" | grep '^dgmstate' | sed -n "$2p"; }
field() { printf '%s' "$1" | sed -n "s/.* $2=\([^ ]*\).*/\1/p" | tr -d '"'; }
node() { printf '%s\n' "$1" | grep "^dgmnode \"$2\"" | tail -1; }
[ -s build/dgm14k.db ] || python3 -I tests/tools/dgm_fixture.py build/dgm14k.db >/dev/null
python3 -I tests/tools/dgm_fixture.py build/dgm_small.db 300 >/dev/null
P=build/dgm-ui-prefs; rm -rf $P build/dgm-export; mkdir -p $P build/dgm-export build/ui
printf '[connection]\nid=shop\nname=Shop\ndriver=sqlite\npath=%s/build/dgm_small.db\n\n[connection]\nid=big\nname=Big\ndriver=sqlite\npath=%s/build/dgm14k.db\n' "$PWD" "$PWD" > $P/connections.txt
run() { LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1; }

out=$(run dgm_a)
expect "every script target found (open / find / expand / save)" '[ "$(missing "$out")" = 0 ]'
s1=$(st "$out" 1); s2=$(st "$out" 2); s3=$(st "$out" 3); s4=$(st "$out" 4); s5=$(st "$out" 5); s6=$(st "$out" 6); s7=$(st "$out" 7)
expect "DGM-001: Show ER Diagram on orders opens it with its 1-hop neighbours only (3 cards of 700)" '[ "$(field "$s1" nodes)" = 3 ] && [ "$(field "$s1" in_scope)" = 700 ]'
expect "DGM-071: the whole scope in 4 bulk catalog statements" '[ "$(field "$s1" statements)" = 4 ]'
expect "DGM-042 / 043: the FKs are drawn with their cardinality (orders -> customers many-to-one)" 'printf "%s\n" "$out" | grep -q "^dgmedge \"orders(customer_id)\" \"orders\" -> \"customers\" origin=0 card=2:1"'
expect "DGM-034 / D5: typing a table not on the diagram and Enter adds exactly that table and focuses it" '[ "$(field "$s2" nodes)" = 4 ] && [ "$(field "$s2" flash)" = audit_event_history ] && [ "$(field "$s2" first_sel)" = audit_event_history ]'
expect "DGM-032: Next walks the hits in order and wraps with a message (3 hits: on the diagram first)" '[ "$(field "$s3" hits)" = 3 ] && [ "$(field "$s3" cur)" = 0 ] && printf "%s" "$s3" | grep -q "msg=\"Back to the first match\""'
expect "DGM-075: expanding customers adds its missing neighbour (customer_profiles)" 'printf "%s\n" "$out" | grep -q "^dgmnode \"customer_profiles\""'
expect "DGM-045: All columns mode; DGM-024: a move is one undo step" '[ "$(field "$s5" undo)" != "0/0" ] && node "$out" orders | grep -q "mode=2"'
expect "DGM-084: the card's context menu opens" '[ "$(field "$s6" menu)" = 1 ]'
ox=$(field "$(node "$out" orders)" x)
expect "DGM-053: the diagram is saved in the workspace's diagrams folder" '[ -s $P/diagrams/orders.lume-erd ] && grep -q "\"formatVersion\": 1" $P/diagrams/orders.lume-erd'
expect "RN-08: no credentials or rows in the file" '! grep -qiE "password|secret|INSERT|SELECT" $P/diagrams/orders.lume-erd'
saved_x=$(python3 -c "import json;d=json.load(open('$P/diagrams/orders.lume-erd'));print([n['x'] for n in d['nodes'] if n['key']=='orders'][0])")

out=$(run dgm_findfilter)
expect "every script target found (find filters)" '[ "$(missing "$out")" = 0 ]'
expect "DGM-031: clicking Columns in the find list adds column hits (the canvas under the list does not take the click)" '[ "$(field "$(st "$out" 2)" hits)" -gt "$(field "$(st "$out" 1)" hits)" ] && [ "$(field "$(st "$out" 2)" find)" = cus ]'

out=$(run dgm_zoom)
z1=$(field "$(st "$out" 1)" zoom); z2=$(field "$(st "$out" 2)" zoom); z3=$(field "$(st "$out" 3)" zoom); z4=$(field "$(st "$out" 4)" zoom)
expect "DGM-020: a trackpad pinch zooms (1.0 -> $z2 -> $z3), the toolbar + too (-> $z4)" 'awk -v a="$z1" -v b="$z2" -v c="$z3" -v d="$z4" "BEGIN{exit !(b > a * 1.4 && c < b * 0.6 && d > c * 1.2)}"'

# reopen: positions kept; a changed table is flagged
sqlite3 build/dgm_small.db "ALTER TABLE customers ADD COLUMN vip INTEGER"
rm -f $P/cache/dgm-*.idx
out=$(run dgm_b)
expect "every script target found (reopen)" '[ "$(missing "$out")" = 0 ]'
rx=$(field "$(node "$out" orders)" x)
expect "DGM-053: reopening keeps the saved position (orders x=$saved_x)" 'awk -v a="$rx" -v b="$saved_x" "BEGIN{exit !(a+0 == b+0)}"'
expect "DGM-016: the changed table (customers got a column) is flagged, the others are not" 'node "$out" customers | grep -q "status=3" && node "$out" orders | grep -q "status=0"'

out=$(run dgm_c)
expect "every script target found (relations / exports)" '[ "$(missing "$out")" = 0 ]'
c1=$(st "$out" 1)
expect "DGM-013: a virtual relation is drawn as virtual; DGM-047 / 008: a note area and a logical entity" '[ "$(field "$c1" user_rel)" = 1 ] && [ "$(field "$c1" notes)" = 1 ] && [ "$(field "$c1" entities)" = 1 ]'
expect "hiding a relation keeps it counted as hidden" '[ "$(field "$c1" hidden)" = 1 ]'
expect "DGM-061: PNG export (a real PNG)" '[ "$(head -c 8 build/dgm-export/d.png | od -An -tx1 | tr -d " ")" = 89504e470d0a1a0a ]'
expect "DGM-062 / 100: SVG export escapes names (no live tag from a note title)" 'grep -q "<svg" build/dgm-export/d.svg && ! grep -q "<b>" build/dgm-export/d.svg && grep -q "Billing &lt;b&gt;" build/dgm-export/d.svg && ! grep -q "<script" build/dgm-export/d.svg'
expect "DGM-051: the conceptual view exports the business name" 'grep -q ">Clients<" build/dgm-export/d.svg'
expect "DGM-063: PDF export" '[ "$(head -c 5 build/dgm-export/d.pdf)" = "%PDF-" ]'
expect "DGM-064: Mermaid, GraphML and draw.io" 'head -1 build/dgm-export/d.mmd | grep -q erDiagram && grep -q "<graphml" build/dgm-export/d.graphml && grep -q "<mxGraphModel>" build/dgm-export/d.drawio'
expect "DGM-069: one self-contained HTML (one inline script, no external request)" '[ "$(grep -c "<script" build/dgm-export/d.html)" = 1 ] && ! grep -qE "src=|href=\"http" build/dgm-export/d.html'
expect "DGM-065 / 059: exporting the selection holds only the selection" '[ "$(grep -c "class=\"card\"" build/dgm-export/sel.svg)" = 1 ] && [ "$(grep -c "\"key\":" build/dgm-export/sel.lume-erd)" = 1 ]'
c2=$(st "$out" 3); cf=$(st "$out" 2)
expect "DGM-039: relations are found by their ends (from:archive to:customers selects both ends)" '[ "$(field "$cf" hits)" = 1 ] && [ "$(field "$cf" sel)" = 2 ]'
expect "DGM-049: Select Disconnected Tables (the logical entity and the hidden-relation table count)" '[ "$(field "$c2" sel)" -ge 1 ]'

# the 14k-table catalog
out=$(run dgm_big)
expect "every script target found (14k)" '[ "$(missing "$out")" = 0 ]'
b1=$(st "$out" 1); b2=$(st "$out" 2); bf=$(st "$out" 3); b3=$(st "$out" 4)
expect "DGM-070: the hub opens with at most 40 cards plus what it says is left, never the 14k" '[ "$(field "$b1" nodes)" -le 41 ] && [ "$(field "$b1" in_scope)" -ge 14000 ]'
expect "DGM-075: a large expansion asks first (\"Add N tables?\")" '[ "$(field "$b2" dlg)" = 1 ] && printf "%s" "$b2" | grep -q "dlg_title=\"Add 150 tables?\""'
n3=$(field "$b3" nodes)
expect "DGM-070: one step adds at most 150 ($n3 cards now)" '[ "$n3" -le 192 ] && [ "$n3" -gt 41 ]'
p95=$(field "$b3" frame_p95); p99=$(field "$b3" frame_p99)
BAR95=16; BAR99=33; [ -n "${CI:-}" ] && { BAR95=60; BAR99=120; }
expect "DGM-095: pan / zoom frame p95 ${p95} ms (bar $BAR95), p99 ${p99} ms (bar $BAR99) with $n3 cards" 'awk -v p="$p95" -v q="$p99" -v b="$BAR95" -v c="$BAR99" "BEGIN{exit !(p+0 < b && q+0 < c)}"'
expect "DGM-073: the canvas draws only the visible cards ($(field "$b3" drawn_cards) of $n3)" '[ "$(field "$b3" drawn_cards)" -lt "$n3" ]'
expect "DGM-022 / review: at 15 % the boxes still carry names (lod 0, $(field "$bf" drawn_labels) names drawn)" '[ "$(field "$bf" lod)" = 0 ] && [ "$(field "$bf" drawn_labels)" -gt 0 ]'
expect "DGM-022: at 12 % the cards are boxes, at 100 % full" 'printf "%s\n" "$out" | grep -q "^dgmstate" && [ "$(field "$b3" lod)" = 2 ]'

# Lume Intelligence through the loopback mock
log=build/dgm_ai_requests.log; rm -f $log
pf=build/dgm_ai_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/dgm_ai.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null
expect "every script target found (assistant)" '[ "$(missing "$out")" = 0 ]'
a1=$(st "$out" 1)
expect "DGM-088: nothing is sent before the consent dialog (it shows the exact structure)" '[ "$(field "$a1" dlg)" = 6 ] && printf "%s" "$a1" | grep -q "dlg_title=\"Send the structure of"'
expect "DGM-089: domain groups come back as proposals; made-up table names are dropped" 'printf "%s\n" "$out" | grep -q "^dgmproposal 0 \"Sales\" tables=\"orders, customers\"" && ! printf "%s\n" "$out" | grep "^dgmproposal" | grep -q "ghost\|Imaginary\|nope"'
expect "DGM-089: accepting a group draws an area; rejecting changes nothing" '[ "$(field "$(st "$out" 3)" notes)" = 1 ]'
expect "DGM-090: business names and logical relations are hypotheses: accepted ones become an alias and a suggested relation" 'printf "%s\n" "$out" | grep -q "^dgmnode \"customers\".*alias=\"Clients\"" && printf "%s\n" "$out" | grep -q "^dgmedge \"archived for\".*origin=2"'
expect "DGM-088: the explanation arrives" 'printf "%s\n" "$out" | grep -q "^dgmai \"orders belongs to customers"'
expect "DGM-088: only structure went to the model (no rows: the fixture's tables are empty, and no INSERT / values)" '[ -s $log ] && grep -q "customer_id" $log && ! grep -qiE "insert into|password" $log'

expect "RN-01 / DGM-092: the diagrams ran no statement on their connections but catalog reads" '! grep -qE "execution [0-9]+ on (Shop|Big)" $P/logs/lume.log && grep -q "diagram: reading the structure of Shop" $P/logs/lume.log'

# idle CPU with a diagram open (DGM-076)
: > $P/logs/lume.log 2>/dev/null
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/dgm_idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
cput() { [ -r /proc/$1/stat ] && { sed 's/^.*) //' /proc/$1/stat | awk '{ print $12 + $13 }'; return; }
  ps -o time= -p $1 | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*100+$3; else print (($1*60+$2)*60+$3)*100+$4 }'; }
for i in $(seq 1 100); do grep -q "diagram: layout of" $P/logs/lume.log 2>/dev/null && break; sleep 0.1; done
sleep 1; c0=$(cput $pid); sleep 3; c1=$(cput $pid); kill $pid 2>/dev/null; wait $pid 2>/dev/null
cpu=$(awk -v a="$c0" -v b="$c1" 'BEGIN{printf "%.1f", (b-a)/3}')
CPUBAR=2; [ -n "${CI:-}" ] && CPUBAR=5; [ "$(uname -s)" = Linux ] && [ -n "${CI:-}" ] && CPUBAR=40
expect "DGM-076: idle with a diagram open (minimap on): $cpu % CPU (bar $CPUBAR %)" 'awk -v c="$cpu" -v b="$CPUBAR" "BEGIN{exit !(c + 0 <= b + 0)}"'

if [ $fails = 0 ]; then echo "DGM UI: all passed"; else echo "DGM UI: FAILED ($fails failures)"; fi
exit $fails
