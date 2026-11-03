#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Update check: at launch (then daily) a newer GitHub release is offered (Download opens this platform's
# installer, Skip This Version is remembered); up to date / no release / errors stay silent unless asked;
# Help > Check for Updates always answers; the Settings switch turns the automatic check off. Loopback mock only.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=visible
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
REQ=build/update_requests.jsonl; P=build/update-prefs
# the version being built (the release workflow sets it in CMakeLists.txt before building)
V=$(sed -n "s/^project(lume VERSION \([0-9.]*\).*/\1/p" CMakeLists.txt)
mock() {   # mock <tag | none | error>: (re)starts the fake GitHub, exports LUME_UPDATE_URL
  [ -n "${mpid:-}" ] && { kill $mpid 2>/dev/null; wait $mpid 2>/dev/null; }
  pf=build/update_mock.port; rm -f $pf; python3 -I tests/tools/update_mock.py "$1" "$REQ" "v$V" > $pf & mpid=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
  export LUME_UPDATE_URL="http://127.0.0.1:$(head -1 $pf)/repos/tessarotto21/lume-data/releases?per_page=20"
}
fresh() { rm -rf $P; mkdir -p $P; rm -f $REQ; }
run() { LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1; }
reqs() { [ -f $REQ ] && wc -l < $REQ | tr -d ' ' || echo 0; }
case "$(uname -s)-$(uname -m)" in
  Darwin-*) INST="Lume-Data-9.0.0-macos-universal.dmg" ;;
  Linux-aarch64|Linux-arm64) INST="lume-data-9.0.0-linux-aarch64.tar.gz" ;;
  Linux-*) INST="lume-data-9.0.0-linux-x86_64.tar.gz" ;;
  *) INST="lume-data-9.0.0-windows-x64.zip" ;;
esac

# 1. a newer release at launch: offered; Download opens this platform's installer
fresh; mock v9.0.0
out=$(run update_auto)
expect "launch: the automatic check asks GitHub's releases API (User-Agent Lume Data, GitHub's JSON)" '[ "$(reqs)" = 1 ] && grep -q "\"path\": \"/repos/tessarotto21/lume-data/releases?per_page=20\"" $REQ && grep -q "\"ua\": \"Lume Data\"" $REQ && grep -q "application/vnd.github+json" $REQ'
expect "a newer version is offered: Download / Release Notes / Later / Skip This Version" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "title=\"Lume Data 9.0.0 is available\"" && printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -qF "You have version $V"'
m1=$(printf "%s\n" "$out" | grep "^tbmodal" | head -1)
expect "what's new: every version newer than this one, newest first, Markdown as plain text" 'printf "%s" "$m1" | grep -qF "WHAT'"'"'S NEW|9.0.0|What'"'"'s new||• Faster result grid|• Fixed the export dialog||8.5.0|Fixes|• Saved queries reopen at the right line"'
expect "with what's new, the dialog is wider and as tall as its notes (more than the plain 230 points)" 'h=$(printf "%s" "$m1" | sed -n "s/.*box=\([0-9]*\)x\([0-9]*\).*/\2/p"); w=$(printf "%s" "$m1" | sed -n "s/.*box=\([0-9]*\)x.*/\1/p"); [ "$w" -gt 550 ] && [ "$h" -gt 230 ] && [ "$h" -le 560 ]'
expect "what's new leaves out the current version, pre-releases, images, HTML and link targets" '! printf "%s" "$m1" | grep -qE "First preview|Beta|Not for everyone|example\.invalid|<!--|\*\*|\]\("'
expect "Download opens the installer for this platform ($INST), never a real browser here" 'printf "%s\n" "$out" | grep -qx "url-open https://github.com/tessarotto21/lume-data/releases/download/v9.0.0/$INST"'
expect "the dialog closes after the answer" 'printf "%s\n" "$out" | grep "^tbmodal" | sed -n 2p | grep -q "kind=0"'
# 2. checked less than a day ago: no request at the next launch
out=$(run update_idle)
expect "the next launch within a day: no request, no dialog" '[ "$(reqs)" = 1 ] && printf "%s\n" "$out" | grep "^tbmodal" | grep -q "kind=0"'
# 3. a day later: checked again; Skip This Version is remembered
sed -i.bak "s/^last=.*/last=$(( $(date +%s) - 90000 ))/" $P/update.txt
out=$(run update_skip)
expect "a day later: checked again; Skip This Version is remembered" '[ "$(reqs)" = 2 ] && grep -qx "skip=v9.0.0" $P/update.txt && printf "%s\n" "$out" | grep "^updatestate" | grep -q "skip=\"v9.0.0\""'
sed -i.bak "s/^last=.*/last=0/" $P/update.txt
out=$(run update_idle)
expect "the skipped version is not offered again at launch" '[ "$(reqs)" = 3 ] && printf "%s\n" "$out" | grep "^tbmodal" | grep -q "kind=0"'
# 4. Help > Check for Updates offers it anyway (asked for), Release Notes opens the release page
printf 'update_check=0\n' > $P/settings.txt
out=$(run update_manual_offer)
expect "Help > Check for Updates offers even a skipped version; Release Notes opens its page" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "9.0.0 is available" && printf "%s\n" "$out" | grep -qx "url-open https://github.com/tessarotto21/lume-data/releases/tag/v9.0.0"'
# 5. the Settings switch off: no automatic request at all
n=$(reqs); printf 'update_check=0\n' > $P/settings.txt; sed -i.bak "s/^last=.*/last=0/" $P/update.txt
out=$(run update_idle)
expect "Settings > Privacy off: no automatic request" '[ "$(reqs)" = "$n" ] && printf "%s\n" "$out" | grep "^updatestate" | grep -q "checks=0"'
# 6. same version / no release published / an error: silent at launch, said when asked
fresh; mock current
out=$(run update_idle)
expect "launch, same version: no dialog" '[ "$(reqs)" = 1 ] && printf "%s\n" "$out" | grep "^tbmodal" | grep -q "kind=0"'
printf 'update_check=0\n' > $P/settings.txt
out=$(run update_manual)
expect "asked, same version: \"Lume Data is up to date\"; OK closes it" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "title=\"Lume Data is up to date\"" && printf "%s\n" "$out" | grep "^tbmodal" | sed -n 2p | grep -q "kind=0"'
fresh; mock none; printf 'update_check=0\n' > $P/settings.txt
out=$(run update_manual)
expect "asked, no release published yet (404): up to date" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "up to date"'
fresh; mock error
out=$(run update_idle)
expect "launch, GitHub fails: silent, retried in an hour (not every frame)" 'printf "%s\n" "$out" | grep "^tbmodal" | grep -q "kind=0" && [ "$(reqs)" = 1 ] && l=$(sed -n "s/^last=//p" $P/update.txt) && [ $(( $(date +%s) - l )) -ge 82000 ]'
printf 'update_check=0\n' > $P/settings.txt
out=$(run update_manual)
expect "asked, GitHub fails: \"Could not check for updates\" with the reason" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "title=\"Could not check for updates\" body=\"GitHub answered with status 500\""'
fresh; mock empty; printf 'update_check=0\n' > $P/settings.txt
out=$(run update_manual)
expect "asked, no release in the list yet: up to date" 'printf "%s\n" "$out" | grep "^tbmodal" | head -1 | grep -q "up to date"'
# 7. Help menu and palette
expect "Help > Check for Updates is a command (palette, Help group)" 'printf "%s\n" "$out" | grep -q "Check for Updates"'
kill $mpid 2>/dev/null; wait $mpid 2>/dev/null
# 8. without a mock under the harness: never a request
fresh; unset LUME_UPDATE_URL
out=$(run update_idle)
expect "the harness without LUME_UPDATE_URL never reaches the internet" 'printf "%s\n" "$out" | grep "^updatestate" | grep -q "checks=0"'
rm -f $P/*.bak
if [ $fails = 0 ]; then echo "UPDATE UI: all passed"; else echo "UPDATE UI: FAILED ($fails failures)"; fi
exit $fails
