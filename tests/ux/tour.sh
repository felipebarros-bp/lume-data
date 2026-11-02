#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# UX tour: drives Lume through its main screens and flows and saves one PNG per scene in build/ux-tour/
# (for a UX / UI review). SQLite test data and the loopback AI mock only: no real database, no real AI.
cd "$(dirname "$0")/../.."
export LUME_TOOLBAR=visible
OUT=$PWD/build/ux-tour; rm -rf "$OUT"; mkdir -p "$OUT"
P=build/ux-prefs; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || exit 1
printf '\n[connection]\nid=nowhere\nname=Server down\ndriver=postgres\nhost=127.0.0.1\nport=1\ndatabase=x\nuser=x\nenv=DEV\n' >> $P/connections.txt
pf=build/ux_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py /dev/null > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
run() { sed "s|__OUT__|$OUT|g" "$1" > build/ux_scene.txt; LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$P LUME_SCRIPT=build/ux_scene.txt ./build/lume >/dev/null 2>&1; }
cat > build/ux1.txt <<'S'
wait 300
shot __OUT__/01-start.bmp
dnode "Shop"
awaitconn "Shop" 20000
rnode "Shop"
wait 60
shot __OUT__/02-connection-menu.bmp
button "New Query"
wait 200
type SELECT region, status, COUNT(*) AS pedidos, SUM(net_total) AS receita FROM rpt_sales GROUP BY region, status ORDER BY receita DESC;
key cmd+Return
wait 400
shot __OUT__/03-results.bmp
type \nSELECT net_totl FROM rpt_sales;
key cmd+shift+Return
wait 400
shot __OUT__/04-error-output.bmp
key cmd+shift+p
wait 60
type report
wait 30
shot __OUT__/05-palette.bmp
key Escape
key cmd+Up
wait 30
plan explain
wait 300
shot __OUT__/06-plan-graph.bmp
plan view findings
wait 60
shot __OUT__/07-plan-findings.bmp
plan view ai
wait 60
shot __OUT__/08-plan-ai.bmp
key cmd+Down
wait 30
invoke report.assistant
wait 100
rpt ai quick Corrija o erro desta query.
rpt await 20000
wait 30
shot __OUT__/09-assistant-proposal.bmp
rpt ai reject
dnode "Server down"
awaitconn "Server down" 15000
wait 60
shot __OUT__/10-connect-failed.bmp
button "Close"
quit
S
cat > build/ux2.txt <<'S'
wait 300
key cmd+,
wait 60
shot __OUT__/11-settings-general.bmp
clickon "settings:sec Appearance"
wait 30
shot __OUT__/12-settings-appearance.bmp
clickon "settings:sec Editor"
wait 30
shot __OUT__/13-settings-editor.bmp
clickon "settings:sec Query Execution"
wait 30
shot __OUT__/14-settings-query.bmp
clickon "settings:sec Results & Format"
wait 30
shot __OUT__/15-settings-results.bmp
clickon "settings:sec Reports"
wait 30
shot __OUT__/16-settings-reports.bmp
clickon "settings:sec Source Control"
wait 30
shot __OUT__/17-settings-git.bmp
clickon "settings:sec Lume Intelligence"
wait 30
shot __OUT__/18-settings-intelligence.bmp
key Escape
invoke app.about.open
wait 60
shot __OUT__/19-about.bmp
key Escape
quit
S
cat > build/ux3.txt <<'S'
wait 300
rpt new Painel de Vendas
wait 100
rpt asof 2026-10-31
rpt add kpi net_revenue
rpt add kpi orders
rpt add line net_revenue month
rpt add bar net_revenue region
rpt add table orders region
rpt add text Notas|- Receita líquida exclui pedidos cancelados
rpt await
wait 300
shot __OUT__/20-report.bmp
rpt select v3
wait 60
shot __OUT__/21-report-card-selected.bmp
invoke report.assistant
wait 100
rpt select
rpt ai ask DESIGN deixe com cara de dashboard executivo
rpt await 20000
wait 30
shot __OUT__/22-report-assistant-plan.bmp
rpt ai approve
rpt await 20000
wait 200
shot __OUT__/23-report-after-design.bmp
quit
S
cat > build/ux4.txt <<'S'
wait 300
dnode "Shop"
awaitconn "Shop" 20000
td new "Shop" main
wait 200
shot __OUT__/24-table-designer.bmp
key cmd+shift+n
wait 100
shot __OUT__/25-new-connection-picker.bmp
type pg
key Return
wait 80
shot __OUT__/26-new-connection-form.bmp
key Escape
wait 30
rnode "Shop"
wait 60
button "Edit Connection…"
wait 100
shot __OUT__/27-edit-connection.bmp
quit
S
for n in 1 2 3 4; do run build/ux$n.txt; done
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
for f in "$OUT"/*.bmp; do sips -s format png "$f" --out "${f%.bmp}.png" >/dev/null 2>&1 && rm -f "$f"; done
ls "$OUT" | wc -l | tr -d ' '
