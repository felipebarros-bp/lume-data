#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# About Lume Data on screen (ABOUT-001..032, scenarios 1..8).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/about-pref; rm -rf "$P"; mkdir -p "$P" build/ui
fails=0
CHANNEL=$(sed -n 's/^LUME_CHANNEL:STRING=//p' build/CMakeCache.txt); CHANNEL=${CHANNEL:-Development}   # the channel this build was configured with
CPUBAR=2; [ -n "${CI:-}" ] && CPUBAR=5   # shared CI machines are noisy; the 2 % bar holds on a real Mac
# Linux CI draws on Xvfb with software OpenGL (llvmpipe): the caret's two redraws a second already
# cost ~10-20 % there, while a redraw loop takes a whole core or more, so the bar still tells them apart
[ "$(uname -s)" = Linux ] && [ -n "${CI:-}" ] && CPUBAR=40
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: target not found'; }
case "$(uname -s)" in Darwin) PLATFORM=macOS ;; *) PLATFORM=$(uname -s) ;; esac   # SDL_GetPlatform()
case "$(uname -m)" in arm64|aarch64) PLATFORM="$PLATFORM arm64" ;; *) PLATFORM="$PLATFORM $(uname -m)" ;; esac
nth() { printf '%s\n' "$1" | grep "^$2" | sed -n "$3p"; }
run() { LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/$1.txt ./build/lume 2>&1; }
# connections holding a user, a password and a host (never connected) and a file under the home folder
printf '[connection]\nid=demo1\nname=Demo (in-memory)\ndriver=sqlite\ndemo=1\npath=:memory:\n\n[connection]\nid=pgx\nname=Payroll\ndriver=postgres\nhost=db.payroll.internal\nport=5432\ndatabase=payroll\nuser=alice.admin\n\n[connection]\nid=sqf\nname=Private file\ndriver=sqlite\npath=%s/private-notes.db\n' "$HOME" > $P/connections.txt
( umask 077; printf 'pgx\tpassword\tSup3rS3cret!\n' > $P/secrets.txt )

out=$(run about_a)
expect "every script target found (About main / licenses)" '[ "$(missing "$out")" = 0 ]'
expect "ABOUT-001 / 002 / 003: the brand in the menu bar opens About Lume Data with the official symbol (no fallback)" 'nth "$out" aboutstate 1 | grep -q "open=1 view=main" && nth "$out" aboutstate 1 | grep -q "fallback=0"'
expect "ABOUT-007..012: real version, build, channel, platform and available drivers" 'nth "$out" aboutstate 1 | grep -qE "version=\"0\.1\.0\" build=\"(Development build|[0-9a-f]{7,})\" channel=\"$CHANNEL\" platform=\"$PLATFORM\" drivers=\"SQLite"'
expect "ABOUT-020: Licenses shows the third-party view; Esc goes back, then closes" 'nth "$out" aboutstate 3 | grep -q "open=1 view=main focus=3" && nth "$out" aboutstate 4 | grep -q "open=0"'

out=$(run about_b)
expect "every script target found (keyboard / reopen)" '[ "$(missing "$out")" = 0 ]'
expect "scenario 1 / ABOUT-001: opening it again (brand, then the command) keeps one instance" 'nth "$out" aboutstate 1 | grep -q "open=1.*opens=2"'
expect "ABOUT-027: Tab skips the links that are not configured (Close -> Licenses)" 'nth "$out" aboutstate 2 | grep -q "focus=3"'
expect "ABOUT-027: Enter activates the focused control (Technical Details)" 'nth "$out" aboutstate 3 | grep -q "focus=5 details=1"'
expect "scenario 2 / ABOUT-017: an unconfigured link explains itself in words" 'nth "$out" aboutstate 4 | grep -q "note=\"Release Notes: Not configured for this build yet\""'
expect "scenario 5 / ABOUT-005: Esc closes only the About; focus back on the editor" 'nth "$out" aboutstate 5 | grep -q "open=0.*app_focus=2"'
e1=$(nth "$out" edtext 1); e2=$(nth "$out" edtext 2)
expect "scenario 5: the SQL tab is untouched by opening and closing (same text, same revision)" '[ "$e1" = "$e2" ] && printf "%s" "$e1" | grep -q "SELECT 42"'
expect "ABOUT-005: typing goes to the SQL tab again" 'nth "$out" edtext 3 | grep -q "SELECT 421"'
expect "ABOUT-001 / spec 5.2: \"About Lume Data\" is a registered command (Help menu, palette); app.about.* listed" 'printf "%s\n" "$out" | grep -q "About Lume Data" && printf "%s\n" "$out" | grep -q "About: Copy Info"'

out=$(run about_c)
clip=$(printf '%s\n' "$out" | awk '/^clipboard-begin/{f=1;next}/^clipboard-end/{f=0;exit}f')
expect "ABOUT-022: Copy Info (command app.about.copy_info) copies the allowlisted lines" 'printf "%s\n" "$clip" | head -1 | grep -qx "Lume Data" && printf "%s\n" "$clip" | grep -q "^Database drivers: SQLite"'
expect "scenario 3 / ABOUT-023: no password, user, host, database name, home path or machine name copied" '! printf "%s" "$clip" | grep -qiE "Sup3rS3cret|alice|payroll|db\.payroll|private-notes|$(whoami)|$(hostname -s)|$HOME"'
clip2=$(printf '%s\n' "$out" | awk '/^clipboard-begin/{c++} c==2&&!/^clipboard/{print} /^clipboard-end/&&c==2{exit}')
expect "ABOUT-026: Copy Version copies \"0.1.0 (build <commit>)\": the git commit, or Development build outside a repository" 'printf "%s" "$clip2" | grep -qE "^0\.1\.0 \((Development build|build [0-9a-f]{7,})\)$"'

# ABOUT-002: the symbol asset missing -> monochrome fallback, logged, the About still opens
T=build/about-noassets; rm -rf $T; mkdir -p $T; cp build/lume $T/; cp -R assets $T/assets; rm -f $T/assets/brand/lume-data-symbol-transparent-*.png
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/about_d.txt $T/lume 2>&1)
expect "ABOUT-002 / 024: without the symbol asset the About opens with the monochrome fallback, and the log says so" 'nth "$out" aboutstate 1 | grep -q "open=1.*fallback=1" && grep -q "symbol asset is missing" $P/logs/lume.log'

# BL-022: the native application menu's About opens this About; the .app bundle is self-contained
if [ "$(uname -s)" = Darwin ]; then
  out=$(run about_native)
  expect "BL-022: the native app menu item reads \"About Lume Data\" and opens Lume's About (same instance on a second click)" 'nth "$out" nativeabout 1 | grep -q "sent title=\"About Lume Data\"" && nth "$out" aboutstate 1 | grep -q "open=1" && nth "$out" aboutstate 2 | grep -q "open=1.*opens=2"'
  APP="build/Lume Data.app"
  expect "BL-022: bundle Info.plist valid, version from project(VERSION), name Lume Data, the brand icon" 'plutil -lint "$APP/Contents/Info.plist" >/dev/null && [ "$(plutil -extract CFBundleShortVersionString raw "$APP/Contents/Info.plist")" = 0.1.0 ] && [ "$(plutil -extract CFBundleName raw "$APP/Contents/Info.plist")" = "Lume Data" ] && [ -s "$APP/Contents/Resources/lume-data.icns" ]'
  B=build/about-bundle; rm -rf $B; mkdir -p $B; cp -R "$APP" $B/
  out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/about_native.txt "$B/Lume Data.app/Contents/MacOS/lume" 2>&1)
  expect "BL-022: a copied bundle runs on its own Resources (official symbol from inside the .app)" 'nth "$out" aboutstate 1 | grep -q "open=1.*fallback=0"'
  rm -f "$B/Lume Data.app/Contents/Resources/assets/brand/"lume-data-symbol-transparent-*.png
  out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/about_native.txt "$B/Lume Data.app/Contents/MacOS/lume" 2>&1)
  expect "BL-022: ... and really from there: without them inside the bundle it falls back (the source tree is not used)" 'nth "$out" aboutstate 1 | grep -q "fallback=1"'
else
  echo "skip  BL-022: the native application menu and the .app bundle are macOS only"
fi

# scenario 6 / ABOUT-030: open and idle, no redraw loop (its own log)
: > $P/logs/lume.log
LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/about_idle.txt ./build/lume >/dev/null 2>&1 & pid=$!
# CPU time spent in a 3 s window after the About is open and settled (ps %cpu on macOS is a
# decaying average that still carries the start-up cost, so measure the delta instead)
# CPU time in 1/100 s: /proc on Linux (ps there only shows whole seconds), ps elsewhere
cput() { [ -r /proc/$1/stat ] && { sed 's/^.*) //' /proc/$1/stat | awk '{ print $12 + $13 }'; return; }
  ps -o time= -p $1 | awk -F'[:.]' '{ if (NF==3) print ($1*60+$2)*100+$3; else print (($1*60+$2)*60+$3)*100+$4 }'; }
for i in $(seq 1 100); do grep -q 'About opened' $P/logs/lume.log 2>/dev/null && break; sleep 0.1; done
sleep 0.5; c0=$(cput $pid); sleep 3; c1=$(cput $pid); wait $pid
cpu=$(awk -v a="$c0" -v b="$c1" 'BEGIN{printf "%.1f", (b-a)/3}')
expect "scenario 6 / ABOUT-030: idle with the About open: $cpu % CPU (bar $CPUBAR %: no polling, no animation)" 'awk -v c="$cpu" -v b="$CPUBAR" "BEGIN{exit !(c + 0 <= b + 0)}"'
expect "ABOUT-030: opening the About ran no database query and no network request" '! sed -n "/About opened/,\$p" $P/logs/lume.log | grep -qE "execution [0-9]+ on|catalog reloaded|connected:"'

echo "ABOUT UI: $( [ $fails = 0 ] && echo OK || echo FAILED ) ($fails failures)"
[ $fails = 0 ]
