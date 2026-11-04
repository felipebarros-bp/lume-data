#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Quitting with sessions whose network died (a VPN dropped) must not freeze the window, on every
# server driver: idle sessions (the logoff), a statement running (its worker is left behind) and
# an open cursor (the driver's release). The dead network is simulated by pausing the container:
# the sockets stay open, nothing answers and nothing resets them, which is what a dropped VPN does.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0; ran=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
TS="$HOME/workspace/tessarotto/test-servers"
pw() { grep "^$2=" "$1" 2>/dev/null | cut -d= -f2- | tr -d '"'; }
O=build/deadnet; rm -rf $O; mkdir -p $O
OUT=$O/out.txt
cur=""
trap '[ -n "$cur" ] && docker unpause $cur >/dev/null 2>&1' EXIT INT TERM

# one scenario: $1 driver label, $2 container, $3 scenario name, $4 statement ("" = idle)
scenario() {
  drv=$1; ctr=$2; name=$3; sql=$4
  S=$O/$drv-$name.txt
  { printf 'wait 500\ndnode "%s"\nawaitconn "%s" 60000\nobjcatawait 180000\nobjcatstate\n' "$drv" "$drv"
    [ -n "$sql" ] && printf 'click 700 300\nkey cmd+a\ntype %s\nkey cmd+shift+Return\nwait 2500\nresstate\n' "$sql"
    printf 'stat pausepoint\nwait 3000\nquit\n'; } > $S
  : > $OUT
  LUME_PREF_DIR=$O LUME_SCRIPT=$S ./build/lume > $OUT 2>&1 &
  pid=$!
  i=0; while [ $i -lt 480 ] && ! grep -q '^stat pausepoint' $OUT && kill -0 $pid 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
  if ! grep -q '^stat pausepoint' $OUT; then kill -9 $pid 2>/dev/null; wait $pid 2>/dev/null; echo "FAIL  $drv/$name: the scenario did not reach the pause point"; fails=$((fails + 1)); return; fi
  cur=$ctr; docker pause $ctr >/dev/null
  t0=$(date +%s)
  i=0; while [ $i -lt 120 ] && kill -0 $pid 2>/dev/null; do sleep 0.5; i=$((i + 1)); done
  alive=0; kill -0 $pid 2>/dev/null && { alive=1; kill -9 $pid; }
  wait $pid 2>/dev/null; rc=$?
  elapsed=$(( $(date +%s) - t0 ))
  docker unpause $ctr >/dev/null; cur=""
  cp $OUT $O/$drv-$name.out
  echo "      $drv/$name: exited $elapsed s after the network died (the script quits ~3 s after it; alive=$alive rc=$rc)"
  expect "$drv/$name: the app quits within 25 s of the network dying, exit 0" '[ $alive = 0 ] && [ $elapsed -le 25 ] && [ $rc = 0 ]'
  ran=$((ran + 1))
}

# $1 driver label (= connection name), $2 container, $3 connection block, $4 password, $5 sleep statement, $6 big select
driver() {
  drv=$1; ctr=$2
  if ! docker ps --format '{{.Names}}' | grep -qx "$ctr" || [ -z "$4" ]; then echo "skip  $drv: $ctr not running or no password"; return; fi
  printf '[connection]\nid=%s\nname=%s\n%s\n' "$drv" "$drv" "$3" > $O/connections.txt
  ( umask 077; printf '%s\tpassword\t%s\n' "$drv" "$4" > $O/secrets.txt )
  rm -f $O/objcat-*.tsv
  scenario "$drv" "$ctr" idle ""
  scenario "$drv" "$ctr" running "$5"
  expect "$drv/running: the stuck statement's worker was left behind (logged) rather than joined" 'grep -q "left behind" $O/$drv-running.out'
  scenario "$drv" "$ctr" cursor "$6"
  expect "$drv/cursor: the first rows were on screen before the network died" 'grep -q "^resstate.*total=[1-9]" $O/$drv-cursor.out'
}

[ -z "$LUME_SKIP_ORACLE" ] && [ -e "$HOME/oracle/instantclient/libclntsh.dylib" ] && \
  driver oracle dbv-oracle 'driver=oracle
host=localhost
port=15210
service=FREEPDB1
user=system
env=DEV' "$(pw $TS/.env ORACLE_PASSWORD)" 'begin dbms_session.sleep(120); end;' 'select a.object_name from all_objects a, all_objects b'
driver postgres dbv-postgres 'driver=postgres
host=localhost
port=15432
database=syn
user=postgres
env=DEV' "$(pw $TS/pg/.env PG_PASSWORD)" 'select pg_sleep(120)' 'select * from generate_series(1, 5000000)'
driver mysql lume-mysql 'driver=mysql
host=127.0.0.1
port=13306
database=shop
user=root
env=DEV' "$(pw $TS/mysql/.env MYSQL_PW)" 'select sleep(120)' 'select a.table_name from information_schema.columns a, information_schema.columns b'
driver mariadb lume-mariadb 'driver=mariadb
host=127.0.0.1
port=13307
database=shop
user=root
env=DEV' "$(pw $TS/mysql/.env MYSQL_PW)" 'select sleep(120)' 'select a.table_name from information_schema.columns a, information_schema.columns b'
driver sqlserver lume-mssql 'driver=sqlserver
host=127.0.0.1
port=14330
database=master
user=sa
env=DEV' "$(pw $TS/mssql/.env MSSQL_PASSWORD)" "waitfor delay '00:02:00'" 'select a.name from sys.all_columns a, sys.all_columns b'

[ $ran = 0 ] && echo "skip  no test server available"
[ $fails = 0 ] && echo "DEADNET UI: all passed ($ran scenarios)" || echo "DEADNET UI: $fails failed"
exit $fails
