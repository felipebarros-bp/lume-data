#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Explorer folders: a connection's `folder` ("a/b", tidied: " a // b / " is "a/b") nests it under
# folder rows, folders first and closed until opened; the open ones are remembered in
# explorer-folders.txt. The folder menu renames a folder (its connections follow), removes one
# (its connections move up a level) and starts a new connection inside it.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/explorer-folders; rm -rf $P; mkdir -p $P
c() { printf '\n[connection]\nid=%s\nname=%s\ndriver=sqlite\npath=:memory:\nfolder=%s\n' "$1" "$2" "$3"; }
{ c fa "acme-prod" "Clients/Acme"; c fb "acme-hml" " Clients // Acme / "; c fc "globex" "Clients"; c fd "scratch" "Lab"; c fe "loose" ""; } > $P/connections.txt
out=$(LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/explorer_folders.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/explorer_folders.out
row() { printf '%s\n' "$out" | grep '^explorer ' | sed -n "$1p"; }
expect "folders first, closed, with how many connections they hold; then the loose connection" 'row 1 | grep -q "^explorer |+Clients(3) |+Lab(1) |loose"'
expect "an untidy path joins its folder; opening shows subfolders before connections" 'row 2 | grep -q "|-Clients(3) |.-Acme(2) |..acme-prod |..acme-hml |.globex |+Lab(1)"'
expect "renaming a folder moves its connections, and it stays open" 'row 3 | grep -q "|.-Acme Corp(2) |..acme-prod |..acme-hml"'
expect "renamed in connections.txt" '[ "$(grep -c "^folder=Clients/Acme Corp$" $P/connections.txt)" = 2 ]'
expect "removing a folder moves its connections up, nothing else changes" 'row 4 | grep -q "|scratch |loose" && ! row 4 | grep -q "Lab" && ! grep -A5 "id=fd" $P/connections.txt | grep -q "^folder="'
expect "open folders are remembered for the next launch" 'grep -qx "Clients" $P/explorer-folders.txt && grep -qx "Clients/Acme Corp" $P/explorer-folders.txt && ! grep -q "^Clients/Acme$" $P/explorer-folders.txt'
expect "New Connection Here starts the dialog inside that folder" 'printf "%s\n" "$out" | grep -q "^dialogfolder \"Clients/Acme Corp\""'
expect "no script target missing" '! printf "%s\n" "$out" | grep -q "target not found"'
if [ $fails = 0 ]; then echo "EXPLORER FOLDERS UI: all passed"; else echo "EXPLORER FOLDERS UI: FAILED ($fails failures)"; fi
exit $fails
