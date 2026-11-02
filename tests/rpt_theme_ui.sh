#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Shared report themes (.lrtheme): a theme file with its logo, referenced by several reports (relative
# path) and by the workspace (Brand Kit); editing the file restyles every report that uses it; local
# overrides stay in one report and a locked theme ignores them; a report away from its theme file uses
# the copy it keeps. The header band and the logo are drawn on screen and in the exported PNG.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
th() { printf '%s\n' "$1" | grep '^rpttheme' | sed -n "$2p"; }
P=build/rpt-theme; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P || { echo "FAIL  setup"; exit 1; }
OUT=$PWD/build/rpt-theme-out; rm -rf "$OUT"; mkdir -p "$OUT/themes"
FIX=$PWD/tests/fixtures/themes
for s in a b c; do sed "s|__OUT__|$OUT|g; s|__FIX__|$FIX|g" tests/scripts/rpt_theme_$s.txt > build/rpt_theme_$s.txt; done
out=$(LUME_TEST_SAVE_DIR=$OUT LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_theme_a.txt ./build/lume 2>&1)
expect "a theme saved as a file is referenced by the report (relative path) with its logo next to it" 'th "$out" 1 | grep -q "ref=file:themes/brand.lrtheme .*header=1 logo=yes" && [ -f "$OUT/themes/logo.png" ] && grep -q "\"logo\":\"logo.png\"" "$OUT/themes/brand.lrtheme"'
expect "a second report opens the same theme file" 'th "$out" 2 | grep -q "ref=file:themes/brand.lrtheme .*logo=yes"'
expect "each saved report keeps the reference and a copy of the theme" 'grep -q "\"themeRef\":\"file:themes/brand.lrtheme\"" "$OUT/Relatorio A.lrpt" && grep -q "\"themeSnapshot\":{" "$OUT/Relatorio B.lrpt"'
expect "the header band shows the logo on screen" 'python3 -I -c "
import sys,struct
b=open(sys.argv[1],\"rb\").read(); off=struct.unpack(\"<I\",b[10:14])[0]; w,h=struct.unpack(\"<ii\",b[18:26]); bpp=struct.unpack(\"<H\",b[28:30])[0]//8
px=b[off:]; n=sum(1 for i in range(0,len(px)-bpp,bpp*7) if px[i]>200 and px[i+1]<60 and px[i+2]>200)
sys.exit(0 if n>20 else 1)
" "$OUT/b.bmp"'
expect "the exported PNG has the logo too" 'python3 -I -c "
import sys,zlib,struct
b=open(sys.argv[1],\"rb\").read(); i=8; idat=b\"\"; w=h=0
while i<len(b):
    L=struct.unpack(\">I\",b[i:i+4])[0]; t=b[i+4:i+8]; d=b[i+8:i+8+L]; i+=12+L
    if t==b\"IHDR\": w,h=struct.unpack(\">II\",d[:8])
    if t==b\"IDAT\": idat+=d
raw=zlib.decompress(idat); stride=w*4+1; prev=bytearray(w*4); n=0
for y in range(h):
    f=raw[y*stride]; row=bytearray(raw[y*stride+1:(y+1)*stride])
    for x in range(len(row)):
        a=row[x-4] if x>=4 else 0
        if f==1: row[x]=(row[x]+a)&255
        elif f==2: row[x]=(row[x]+prev[x])&255
    for x in range(0,len(row),4):
        if row[x]>200 and row[x+1]<60 and row[x+2]>200: n+=1
    prev=row
sys.exit(0 if n>50 else 1)
" "$OUT/Relatorio B.png"'
expect "the workspace theme (Brand Kit) is written and a new report starts with it" '[ -f "$P/workspaces/Lume Data/theme.lrtheme" ] || ls $P/workspaces/*/theme.lrtheme >/dev/null 2>&1; th "$out" 3 | grep -q "ref=workspace"'
# ---- outside edit of the theme file: both reports follow ----
python3 -I -c "
import json,sys; p=sys.argv[1]; t=json.load(open(p)); t['colors']['accent']='#0B7A75'; json.dump(t,open(p,'w'))
" "$OUT/themes/brand.lrtheme"
out=$(LUME_TEST_SAVE_DIR=$OUT LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_theme_b.txt ./build/lume 2>&1)
expect "editing the theme file restyles the reports that use it (accent from the file)" 'th "$out" 1 | grep -q "accent=#0B7A75 .*overrides=0" && th "$out" 3 | grep -q "accent=#0B7A75"'
expect "a local change stays in that report only (kept as an override, the file untouched)" 'th "$out" 2 | grep -q "accent=#B42318 .*overrides=1" && grep -q "#0B7A75" "$OUT/themes/brand.lrtheme"'
# ---- lock the theme; move report B away from the theme file ----
python3 -I -c "
import json,sys; p=sys.argv[1]; t=json.load(open(p)); t['locked']=True; json.dump(t,open(p,'w'))
" "$OUT/themes/brand.lrtheme"
mkdir -p "$OUT/moved"; mv "$OUT/Relatorio B.lrpt" "$OUT/moved/"
out=$(LUME_TEST_SAVE_DIR=$OUT LUME_PREF_DIR=$P LUME_SCRIPT=build/rpt_theme_c.txt ./build/lume 2>&1)
expect "a locked theme ignores the report override and refuses new ones" 'th "$out" 1 | grep -q "locked=1 accent=#0B7A75" && th "$out" 2 | grep -q "accent=#0B7A75 .*msg=\"Tema travado"'
expect "away from its theme file, a report renders with the copy it kept when last saved, and says so" 'th "$out" 3 | grep -q "accent=#1F6FEB .*warn=\"Arquivo de tema não encontrado"'
if [ $fails = 0 ]; then echo "RPT THEME UI: all passed"; else echo "RPT THEME UI: FAILED ($fails failures)"; fi
exit $fails
