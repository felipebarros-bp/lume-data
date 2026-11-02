#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Lume Intelligence evaluation (roadmap item 22): for each question in tests/evals/sql_questions.txt,
# a report is asked on the demo database and the outcome is classified from the audit log:
#   first-try   the assistant's SQL passed the local check (WHERE 1=0) at once
#   corrected   it passed after the assistant corrected it (<sql_check> round)
#   failed      no plan (refused, cannot answer, error)
# Default: the loopback mock (deterministic, part of the pipeline). A real provider is used ONLY when
# you pass --real AND set LUME_ALLOW_REAL_AI=1 on the command line yourself; it spends your plan / key.
cd "$(dirname "$0")/../.."
export LUME_TOOLBAR=hidden
real=0; [ "$1" = "--real" ] && real=1
if [ $real = 1 ] && [ "$LUME_ALLOW_REAL_AI" != 1 ]; then echo "--real needs LUME_ALLOW_REAL_AI=1 on the command line (it spends your AI usage)"; exit 2; fi
if [ $real = 0 ] && [ -n "$LUME_ALLOW_REAL_AI" ]; then echo "LUME_ALLOW_REAL_AI is set: use --real for a real run, or unset it"; exit 2; fi
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
P=build/eval-sql; rm -rf $P; sh tests/rpt_setup_sqlite.sh $P >/dev/null || { echo "FAIL  setup"; exit 1; }
rm -f $P/semantic.json   # no certified metrics: every number comes from the assistant's SQL
if [ $real = 1 ]; then
  src="${LUME_EVAL_PREFS:-$HOME/Library/Application Support/lume/Lume}"; cp "$src/ai.txt" $P/ai.txt 2>/dev/null || { echo "no ai.txt in $src"; exit 2; }
else
  pf=build/eval_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py /dev/null > $pf & mock=$!
  for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
  printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $P/ai.txt
  export LUME_AI_KEY=sk-test-not-a-real-key
fi
first=0; corrected=0; failed=0; n=0
grep -v '^#' tests/evals/sql_questions.txt | grep -v '^$' > build/eval_q.txt
while IFS= read -r q; do
  n=$((n + 1)); rm -rf $P/audit
  printf 'wait 300\ndnode "Shop"\nawaitconn "Shop" 20000\nrpt new Eval %d\nwait 100\ninvoke report.assistant\nwait 200\nrpt ai ask %s\nrpt ai schema conn Shop\nrpt ai schema allow\nrpt await 120000\nrptstate\nquit\n' $n "$q" > build/eval_script.txt
  out=$(LUME_PREF_DIR=$P LUME_SCRIPT=build/eval_script.txt ./build/lume 2>&1 </dev/null)
  st=$(printf '%s\n' "$out" | grep '^rptstate' | tail -1 | sed 's/.*ast=\([a-z-]*\).*/\1/')
  retries=$(grep -c "sql-check-retry" $P/audit/report.log 2>/dev/null)
  if [ "$st" = "awaiting-approval" ] && [ "${retries:-0}" = 0 ]; then first=$((first + 1)); r=first-try
  elif [ "$st" = "awaiting-approval" ]; then corrected=$((corrected + 1)); r="corrected ($retries round(s))"
  else failed=$((failed + 1)); r="failed ($st)"; fi
  echo "eval  $r  $q"
done < build/eval_q.txt
[ $real = 0 ] && { kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf; }
echo "EVAL SQL: $n question(s): $first first-try, $corrected corrected, $failed failed ($(( n ? first * 100 / n : 0 ))% first-try)"
if [ $real = 0 ]; then   # the mock's expected outcome (a regression check of the measuring itself)
  [ $first = 2 ] && [ $corrected = 1 ] && [ $failed = 0 ] && { echo "EVAL SQL (mock): all passed"; exit 0; }
  echo "EVAL SQL (mock): FAILED"; exit 1
fi
