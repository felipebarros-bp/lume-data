#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# The Settings screen (Cmd+,) with its Lume Intelligence section (roadmap item 25): opens over the
# workspace, saves the fast model to ai.txt (no key ever written), forgets what was remembered for a
# connection (consents, scope, its structure index), Esc closes it.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/settings-ui; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
OUT=$PWD/build/settings-out; rm -rf "$OUT"; mkdir -p "$OUT"
printf 'provider=anthropic\nendpoint=http://127.0.0.1:9/v1/messages\nmodel=claude-opus-5-5\n' > $P/ai.txt
( umask 077; printf 'shop1\tconsent\tschema\nshop1\tschemas\tmain\nother\tconsent\tschema,catvalues\n' > $P/ai-scope.txt )
sed "s|__OUT__|$OUT|g" tests/scripts/settings.txt > build/settings.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=build/settings.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/settings_ui.out
expect "Cmd+, opens Settings on its first section" 'printf "%s\n" "$out" | grep "^settingsstate" | sed -n 1p | grep -q "open=1 section=General"'
expect "a section that fits does not scroll" 'printf "%s\n" "$out" | grep "^settingsstate" | sed -n 2p | grep -q "scroll=0 "'
expect "Query Execution: rows per fetch saved to settings.txt (kept for the next launch)" 'grep -q "^fetch_size=2000$" $P/settings.txt'
expect "Results & Format: the thousands separator saved" 'grep -q "^fmt_group=1$" $P/settings.txt'
expect "Editor: the formatter rule saved in sqlstyle.txt" 'grep -q "^\*.tabs=yes$" $P/sqlstyle.txt' 
expect "the fast model is saved to ai.txt (and nothing else changes there; no key)" 'grep -q "^model_fast=claude-haiku-5-5$" $P/ai.txt && grep -q "^model=claude-opus-5-5$" $P/ai.txt && ! grep -qi "sk-" $P/ai.txt'
expect "Forget removes only that connection: its consents and scope" '! grep -q "^shop1	" $P/ai-scope.txt && grep -q "^other	consent	schema,catvalues" $P/ai-scope.txt'
expect "Esc closes it" 'printf "%s\n" "$out" | grep "^settingsstate" | sed -n 3p | grep -q "open=0"'
if [ "$(uname -s)" = Darwin ]; then expect "the macOS app menu Settings... item is enabled and opens Lume Settings" 'printf "%s\n" "$out" | grep -q "^nativesettings sent" && printf "%s\n" "$out" | grep "^settingsstate" | sed -n 4p | grep -q "open=1"'
else echo "skip  the native app menu Settings... item is macOS only"; fi
if [ $fails = 0 ]; then echo "SETTINGS UI: all passed"; else echo "SETTINGS UI: FAILED ($fails failures)"; fi
exit $fails
