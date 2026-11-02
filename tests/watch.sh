#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Watch the UI tests run: every scripted Lume window comes to the front while it runs, and
# each script step waits LUME_WATCH milliseconds so a person can follow it.
#   tests/watch.sh                         panels_ui at 400 ms per step
#   tests/watch.sh tests/textfield_ui.sh   any suite (or several)
#   LUME_WATCH=800 tests/watch.sh ...      slower
# macOS only (System Events); the suites still report ok / FAIL as usual.
cd "$(dirname "$0")/.."
export LUME_WATCH="${LUME_WATCH:-400}" LUME_FOREGROUND=1   # watching: windows come to the front
[ $# -eq 0 ] && set -- tests/panels_ui.sh
( while :; do   # keeps any running test window in front; it stops with this script
    osascript -e 'tell application "System Events" to if exists (first process whose name is "lume") then set frontmost of (first process whose name is "lume") to true' >/dev/null 2>&1
    sleep 0.5
  done ) & keeper=$!
trap 'kill $keeper 2>/dev/null' EXIT INT TERM
status=0
for suite in "$@"; do echo "== $suite (watch: $LUME_WATCH ms per step)"; sh "$suite" || status=1; done
exit $status
