#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds <dir>: "work repo" (main tracking origin), origin.git (bare), "other" (a second clone that pushes
# a commit, so work's first push is rejected), branches for the conflict / reload scenarios, a SQLite
# database with a view (its definition must never change through Git), and the prefs folder.
d=$1; rm -rf "$d"; mkdir -p "$d/pref"
g() { git -C "$1" -c user.name="Lume Test" -c user.email="test@example.invalid" "$@" 2>/dev/null; }
git init -q --bare -b main "$d/origin.git"
git clone -q "$d/origin.git" "$d/work repo" 2>/dev/null
w="$d/work repo"
git -C "$w" config user.name "Lume Test"; git -C "$w" config user.email "test@example.invalid"
mkdir -p "$w/sql"
printf 'CREATE VIEW v_revenue AS\nSELECT region, SUM(net_total) AS revenue\nFROM rpt_sales\nWHERE status = %spaid%s\nGROUP BY region;\n' "'" "'" > "$w/sql/v_revenue.sql"
printf 'DROP TABLE rpt_sales;\n' > "$w/sql/dangerous.sql"
printf 'line one\nline two\nline three\nline four\nline five\nline six\nline seven\nline eight\nline nine\nline ten\n' > "$w/notes.md"
printf '*.log\n' > "$w/.gitignore"
git -C "$w" add -A; git -C "$w" commit -q -m "initial"; git -C "$w" push -q -u origin main 2>/dev/null
git -C "$w" switch -q -c conflict-a; sed -i '' 's/^line one$/line one (conflict-a)/' "$w/notes.md"; git -C "$w" commit -q -am "conflict-a"
git -C "$w" switch -q -c feature-x main; printf 'feature x notes\n' > "$w/notes.md"; git -C "$w" commit -q -am "feature x"
git -C "$w" switch -q main
git clone -q "$d/origin.git" "$d/other" 2>/dev/null
git -C "$d/other" config user.name "Other"; git -C "$d/other" config user.email "o@example.invalid"
printf 'from the other clone\n' > "$d/other/other.txt"; git -C "$d/other" add other.txt; git -C "$d/other" commit -q -m "other side"; git -C "$d/other" push -q 2>/dev/null
sqlite3 "$d/pref/shop.db" "CREATE TABLE rpt_sales(id INTEGER PRIMARY KEY, region TEXT, status TEXT, net_total NUMERIC); INSERT INTO rpt_sales VALUES (1,'Sul','paid',10); CREATE VIEW v_revenue AS SELECT region, SUM(net_total) AS revenue FROM rpt_sales GROUP BY region;"
printf '[connection]\nid=shop1\nname=Shop\ndriver=sqlite\npath=%s/%s/pref/shop.db\n' "$PWD" "$d" > "$d/pref/connections.txt"
mkdir -p "$d/newproj"
