#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Report files where the user wants them: the first Save asks for a name and a folder (Save As), later
# saves go to the same file, autosave and versions stay in the prefs folder (nothing next to the file),
# the file is in the recent list; exports as PDF, PNG, card PNG, card CSV and page XLSX (one sheet per
# card) are valid files. LUME_TEST_SAVE_DIR stands in for the system's save dialog.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
P=build/rpt-export; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
OUT=build/rpt-export-out; rm -rf "$OUT"; mkdir -p "$OUT"
out=$(LUME_TEST_SAVE_DIR=$PWD/$OUT LUME_PREF_DIR=$P LUME_SCRIPT=tests/scripts/rpt_export.txt ./build/lume 2>&1)
f="$OUT/Painel Vendas- Q4-2026"
expect "first Save asks where: the report is saved under its title in the chosen folder (unsafe characters replaced)" '[ -f "$f.lrpt" ] && printf "%s\n" "$out" | grep -q "^rptpath action=7 ok=1"'
expect "later saves go to the same file (no second dialog)" '[ "$(printf "%s\n" "$out" | grep -c "^rptpath action=7")" = 1 ] && grep -q "Painel alterado" "$f.lrpt"'
expect "autosave and versions stay in the prefs folder, nothing next to the file" '! ls "$OUT" | grep -q "versions\|autosave" && ls -d $P/reports/*.lrpt.versions >/dev/null 2>&1'
expect "the file is in the recent list" 'grep -q "$OUT/Painel Vendas- Q4-2026.lrpt" $P/reports/recent.txt'
expect "PDF: a valid PDF with the page image" 'head -c 8 "$f.pdf" | grep -q "%PDF-1.4" && tail -c 8 "$f.pdf" | grep -q "EOF" && grep -aq "/Subtype /Image" "$f.pdf" && python3 -I -c "
import sys,re,zlib
b=open(sys.argv[1],\"rb\").read()
m=re.search(rb\"/Width (\d+) /Height (\d+).*?/Length (\d+) >>\nstream\n\", b, re.S)
w,h,l=map(int,m.groups()); s=m.end()
raw=zlib.decompress(b[s:s+l]); assert len(raw)==w*h*3, (len(raw), w*h*3)
xref=int(re.search(rb\"startxref\n(\d+)\", b).group(1)); assert b[xref:xref+4]==b\"xref\"
" "$f.pdf"'
expect "HTML: one self-contained page: values as copyable text, charts as SVG, no script, nothing loaded from outside" 'h="$f.html"; [ -f "$h" ] && grep -q "<meta http-equiv=\"Content-Security-Policy\" content=\"default-src .none.;" "$h" && ! grep -qi "<script" "$h" && ! grep -qi "src=\"http\|href=\"http" "$h" && grep -q "<div class=\"kpi\">R\$ [0-9]" "$h" && [ "$(grep -o "<svg class=\"chart\"" "$h" | wc -l | tr -d " ")" = 2 ] && grep -q "<td class=\"num\">" "$h" && grep -q "<title>Painel Vendas: Q4/2026</title>" "$h" && grep -q "<li>só texto</li>" "$h" && grep -q "@media print" "$h"'
expect "exporting never opens the About (the dialog wake-up event used to collide with it)" 'printf "%s\n" "$out" | grep "^aboutstate" | head -1 | grep -q "open=0"'
expect "PNG of the page and of the selected card (separate files)" 'file "$f.png" | grep -q "PNG image data, 1600" && [ "$(ls "$OUT"/*.png | wc -l | tr -d " ")" = 2 ]'
expect "CSV of the card (named after the report and the card): business column names, rows, UTF-8 with BOM" 'c=$(ls "$OUT"/*.csv | head -1); head -c 3 "$c" | od -An -tx1 | tr -s " " | grep -q "ef bb bf" && sed -n 1p "$c" | grep -q "Região,Receita líquida" && [ "$(wc -l < "$c" | tr -d " ")" -ge 3 ]'
expect "XLSX: a valid workbook with one sheet per data card (text card left out), numbers as numbers" 'python3 -I -c "
import sys,zipfile,re
z=zipfile.ZipFile(sys.argv[1]); assert z.testzip() is None
wb=z.read(\"xl/workbook.xml\").decode(); n=len(re.findall(\"<sheet \",wb)); assert n==4, n
s1=z.read(\"xl/worksheets/sheet1.xml\").decode(); assert \"<v>\" in s1
" "$f.xlsx"'
expect "exports and saves are audited without content" 'grep -q "tool=export_report.*decision=pdf" $P/audit/report.log && grep -q "decision=xlsx" $P/audit/report.log && ! grep -qi "select\|net_total" $P/audit/report.log'
# reopen the saved file: it is a user file (Save keeps writing there)
sed "s|__PATH__|$PWD/$f.lrpt|" tests/scripts/rpt_export_reopen.txt > build/rpt_export_reopen.txt
out2=$(LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_export_reopen.txt ./build/lume 2>&1)
expect "the saved file opens again as the user's file" 'printf "%s\n" "$out2" | grep -q "^rptopen ok=1 user=1"'
if [ $fails = 0 ]; then echo "RPT EXPORT UI: all passed"; else echo "RPT EXPORT UI: FAILED ($fails failures)"; fi
exit $fails
