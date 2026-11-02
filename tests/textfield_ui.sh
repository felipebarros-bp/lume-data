#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Text field editing model (ui.c) in the real app, through the command palette's field:
# word moves, Shift selection, replace, undo / redo, clipboard, word / line delete, right-click menu, horizontal
# scroll, double / triple click, and clicks reaching the palette's own rows (modal mouse).
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden   # scripts written before the main toolbar use window coordinates
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/tf-pref; rm -rf "$P"; mkdir -p "$P" build/ui
fails=0
out=$( (LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/textfield.txt ./build/lume; LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/textfield_b.txt ./build/lume; LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/textfield_c.txt ./build/lume; LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/textfield_d.txt ./build/lume) 2>&1)
printf '%s\n' "$out" | grep -q '^script: target not found' && { echo "FAIL  a script target was not found"; fails=$((fails + 1)); }
LONG="the quick brown fox jumps over the lazy dog while the slow green turtle watches from the river bank and counts every single leaf falling END"
n=0
expect() {   # description, expected "field ..." line
  n=$((n + 1)); got=$(printf '%s\n' "$out" | grep '^field ' | sed -n "${n}p")
  if [ "$got" = "$2" ]; then echo "ok    $1"; else echo "FAIL  $1: got [$got], want [$2]"; fails=$((fails + 1)); fi
}
expect "Option+Left twice, Shift+Option+Right selects a word, typing replaces it" 'field "hello there foo" sel 11 11'
expect "Cmd+Z restores the word and its selection" 'field "hello world foo" sel 6 11'
expect "Cmd+Shift+Z, Shift+Cmd+Right, Cmd+C, Right, Cmd+V" 'field "hello there foo hello there foo" sel 31 31'
expect "Option+Backspace twice deletes the last two words" 'field "hello there foo hello " sel 22 22'
expect "Cmd+Backspace clears; punctuation splits words; Shift+Home selects to the start" 'field "a.b_c-d" sel 0 6'
expect "long text: caret at the end" "field \"$LONG\" sel 140 140"
expect "Cmd+Left goes to the start" "field \"$LONG\" sel 0 0"
expect "double-click selects the word under the pointer" "field \"$LONG\" sel 4 9"
expect "triple-click selects everything" "field \"$LONG\" sel 0 140"
n=$((n + 1))   # textfield_b prints one more line after the row click (the palette has closed)
expect "dialog field (drawn in a second pass): Shift+Right three times selects 3 characters" 'field "minha conexao local" sel 0 3'
expect "dialog field: Shift+Option+Left twice selects the last two words" 'field "minha conexao local" sel 6 19'
expect "dialog field: a mouse drag selects (release delivered with the last motion)" 'field "minha conexao local" sel 6 19'
expect "typing over a selection replaces it" 'field "minha rede" sel 10 10'
expect "Backspace over a selection deletes only the selection" 'field "minha " sel 6 6'
expect "Cmd+V over a selection replaces it with the clipboard" 'field "def abc def" sel 3 3'
expect "Delete (forward) over a selection deletes only the selection" 'field "def" sel 3 3'
expect "Cmd+X cuts the selection" 'field "" sel 0 0'
expect "Cmd+V puts the cut text back" 'field "def" sel 3 3'
expect "right-click menu: Select All" 'field "minha conexao local" sel 0 19'
expect "right-click menu: Copy, then Paste at the end" 'field "minha conexao local minha conexao local" sel 39 39'
expect "right-click menu: Undo reverts the paste" 'field "minha conexao local " sel 20 20'
expect "right-click menu: Delete removes the selection" 'field "" sel 0 0'
grep -q 'Query Focus' "$P/logs/lume.log" && echo "ok    a click on a palette row runs the command (the modal's widgets get the click)" || { echo "FAIL  palette row click"; fails=$((fails + 1)); }
# the selection must be visible, not only stored: count pixels of the selection colour (T_SELECTED
# 0x2A3A5C) inside the dialog's search field in both screenshots
if [ -n "${CI:-}" ]; then echo "skip  selection pixels: CI renders with another colour profile (the selection itself is checked above)"; fi
for shot in tf_dialog_keys tf_dialog_drag; do
  [ -n "${CI:-}" ] && break
  sips -s format png "build/ui/$shot.bmp" --out "build/ui/$shot.png" >/dev/null 2>&1
  px=$(python3 -I -c "
from PIL import Image
im = Image.open('build/ui/$shot.png').convert('RGB'); w, h = im.size; s = w / 1280
box = im.crop((int(344 * s), int(248 * s), int(936 * s), int(276 * s)))   # the dialog's database search field
print(sum(1 for p in box.getdata() if abs(p[0] - 0x2A) < 6 and abs(p[1] - 0x3A) < 6 and abs(p[2] - 0x5C) < 6))" 2>/dev/null)
  [ "${px:-0}" -gt 500 ] && echo "ok    $shot: the selection is drawn ($px px of the selection colour)" || { echo "FAIL  $shot: no visible selection (${px:-0} px)"; fails=$((fails + 1)); }
done
for f in build/ui/tf_*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null && rm "$f"; done
echo "TEXT FIELD UI: $([ $fails -eq 0 ] && echo OK || echo FAIL) ($fails failures)"
exit $fails
