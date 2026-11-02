#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Assistant chat: an approval taller than the view opens at its title (once); scrolled to the end, it
# stays at the end (it used to bounce back to the title on every frame).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/chat-scroll; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
pf=build/chat_scroll_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py /dev/null > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/chat_scroll.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null
printf '%s\n' "$out" > build/chat_scroll.out
c() { printf '%s\n' "$out" | grep '^rptchat scroll=' | sed -n "$1p"; }
num() { echo "$1" | sed "s/.*$2=\([0-9]*\).*/\1/"; }
expect "the approval waits for the user and is taller than the chat (the case under test)" 'printf "%s\n" "$out" | grep -q "rptchat live \[awaiting-approval\]" && [ "$(num "$(c 1)" max)" -gt 0 ]'
expect "it opens at the approval's title, not at the bottom" '[ "$(num "$(c 1)" scroll)" -lt "$(num "$(c 1)" max)" ]'
expect "scrolled to the end, it is at the end" '[ "$(num "$(c 2)" scroll)" = "$(num "$(c 2)" max)" ]'
expect "and it stays there (no bounce back to the title)" '[ "$(num "$(c 3)" scroll)" = "$(num "$(c 3)" max)" ]'
if [ $fails = 0 ]; then echo "CHAT SCROLL UI: all passed"; else echo "CHAT SCROLL UI: FAILED ($fails failures)"; fi
exit $fails
