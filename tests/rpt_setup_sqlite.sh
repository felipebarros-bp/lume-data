#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Creates <dir>/shop.db with the golden sales dataset of rpt_check (3000 deterministic orders) and the
# semantic catalog + connection files for the Report Studio UI tests.
dir=$1; mkdir -p "$dir"
build/rpt_check --emit build/rpt-golden > /dev/null || exit 1
rm -f "$dir/shop.db"
sqlite3 "$dir/shop.db" "CREATE TABLE rpt_sales(id INTEGER PRIMARY KEY, ordered_at TEXT, region TEXT, status TEXT, gross_total NUMERIC, net_total NUMERIC, customer_id INT);" ".import --csv build/rpt-golden/data.csv rpt_sales" "ALTER TABLE rpt_sales ADD COLUMN customer_email TEXT;" "UPDATE rpt_sales SET customer_email = 'c' || customer_id || '@example.com';" || exit 1
cp tests/fixtures/reports/semantic.json "$dir/semantic.json"
printf '[connection]\nid=shop1\nname=Shop\ndriver=sqlite\npath=%s/%s/shop.db\n' "$PWD" "$dir" > "$dir/connections.txt"
