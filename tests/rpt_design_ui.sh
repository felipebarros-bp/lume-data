#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# The assistant as a designer (loopback mock): set_theme (accent, header, card / KPI styles), a card's own
# style and a decorative shape, approved as one undoable step and kept as the report's theme overrides;
# a design that would make text unreadable is refused (WCAG 4.5:1); the model is told the current theme.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
block() { printf '%s\n' "$1" | awk -v n="$2" '/^rptstate /{k++} k==n'; }
th() { printf '%s\n' "$1" | grep '^rpttheme' | sed -n "$2p"; }
P=build/rpt-design; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
OUT=$PWD/build/rpt-design-out; rm -rf "$OUT"; mkdir -p "$OUT"
sed "s|__OUT__|$OUT|g" tests/scripts/rpt_design.txt > build/rpt_design.txt
pf=build/rpt_design_mock.port; log=build/rpt_design_mock.log; rm -f $pf $log
python3 -I tests/tools/ai_mock.py $log > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_design.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
printf "%s\n" "$out" > build/rpt_design.out
expect "the design proposal is a plan to approve, its theme checked for contrast" 'block "$out" 1 | grep -q "ast=awaiting-approval" && printf "%s\n" "$out" | grep -q "^rptstep set_theme \[done\] accent #0B7A75, header on (#0B1F3A), cards accent-top, KPI accent-bar.*contrast checked"'
expect "approved: the theme changes are the report's overrides (header, accent, card and KPI styles)" 'th "$out" 1 | grep -q "accent=#0B7A75 header=1 .*card=accent-top kpi=accent-bar overrides=1"'
expect "the card got its own style and a decorative shape was added" 'block "$out" 2 | grep -q "^rptvis v2 kind=shape" && block "$out" 2 | grep "^rptstyle v1 " | grep -q "0B7A75" && block "$out" 2 | grep "^rptstyle v2 " | grep -q "line"'
expect "a design with unreadable text is refused before anything changes" 'block "$out" 2 | grep -q "ast=error amsg=\"The design was refused: text on the page background is hard to read"'
expect "Undo takes the whole design back" 'th "$out" 2 | grep -q "overrides=0"'
expect "the model was told the current theme (no image, no data)" 'grep -q "<design untrusted=\"true\">" $log && grep -q "\"cardStyle\":" $log'
rv=$(awk 'BEGIN{RS="=====\n"} /<report_html placeholders/' $log | head -c 400000)
expect "Review design: its size is said before sending; the page goes as HTML with placeholder data (no real value, no image) and comes back as a layout plan" 'printf "%s\n" "$out" | grep -q "^rptreview estimate=[1-9]" && printf "%s" "$rv" | grep -q "PLACEHOLDER data" && printf "%s" "$rv" | grep -q "<div class=\"kpi\">" && ! printf "%s" "$rv" | grep -q "293.044" && ! printf "%s" "$rv" | grep -q "data:image" && block "$out" 3 | grep -q "ast=awaiting-approval"'
expect "the review answer shows its tokens in the chat" 'printf "%s\n" "$out" | grep -q "^rptchat tokens: .* in .* out tokens"'
# run 2: a locked team theme
printf '{"formatVersion":1,"id":"brand","name":"Marca","locked":true,"colors":{"accent":"#7A1F5C"}}\n' > "$OUT/brand.lrtheme"
sed "s|__OUT__|$OUT|g" tests/scripts/rpt_design_b.txt > build/rpt_design_b.txt
python3 -I tests/tools/ai_mock.py /dev/null > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out2=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_design_b.txt ./build/lume 2>&1)
printf "%s\n" "$out2" > build/rpt_design_b.out
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
expect "a locked team theme: only the restyle is left out, the cards' changes stay in the plan, the brand stays" 'block "$out2" 1 | grep -q "ast=awaiting-approval" && printf "%s\n" "$out2" | grep -q "^rptstep [0-9] skipped set_theme .left out: the theme is locked by the brand" && th "$out2" 2 | grep -q "locked=1 accent=#7A1F5C .*overrides=0"'
# run 3: an image dropped on the chat (the user's logo) becomes the theme's logo; Cmd+C with the box focused
FIX=$PWD/tests/fixtures/themes
sed -e "s|__OUT__|$OUT|g" -e "s|__FIX__|$FIX|g" tests/scripts/rpt_design_c.txt > build/rpt_design_c.txt
log3=build/rpt_design_mock3.log; rm -f $log3
python3 -I tests/tools/ai_mock.py $log3 > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
mkdir -p "$OUT/saved"
out3=$(LUME_TEST_SAVE_DIR="$OUT/saved" LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_design_c.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
printf "%s\n" "$out3" > build/rpt_design_c.out
expect "an image dropped on the chat is attached (not opened); the model gets its name and colours, never the image" 'printf "%s\n" "$out3" | grep "^rptattach" | sed -n 1p | grep -q "name=\"logo-magenta.png\"" && grep -q "\"attachment\":{\"name\":\"logo-magenta.png\",\"kind\":\"image\",\"colors\":\[\"#" $log3 && ! grep -q "iVBOR" $log3'
expect "the plan says the attached image becomes the logo, in a theme file of its own" 'printf "%s\n" "$out3" | grep -q "^rptstep set_logo \[done\] logo-magenta.png (attached) becomes the logo; this report.s theme is saved as its own file"'
expect "approved: the report uses its own theme file with the logo and the header on; the attachment is used up" 'th "$out3" 1 | grep -q "ref=file:.*\.lrtheme .*accent=#C2185B header=1 logo=yes" && printf "%s\n" "$out3" | grep "^rptattach" | sed -n 2p | grep -q "name=\"\""'
expect "Save As to another folder: the theme Lume created moves with the report, logo included; the reference stays relative" 'th "$out3" 2 | grep -q "ref=file:[^/ ]*\.lrtheme .*logo=yes" && ls "$OUT"/saved/*.lrtheme >/dev/null 2>&1 && [ -f "$OUT/saved/logo.png" ]'
expect "Cmd+C copies the selected conversation while the question box has the focus" 'printf "%s\n" "$out3" | sed -n "/^clipboard-begin/,/^clipboard-end/p" | grep -q "coloca o nosso logo"'
if [ $fails = 0 ]; then echo "RPT DESIGN UI: all passed"; else echo "RPT DESIGN UI: FAILED ($fails failures)"; fi
exit $fails
