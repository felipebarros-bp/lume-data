#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Source Control on screen (GIT-001..071 MVP): file tabs, stage / commit only staged (Gherkin), one-hunk staging,
# pull blocked by an unsaved buffer (Gherkin), rejected push without force (Gherkin), fetch / ff-only / merge
# pull, offline branch (Gherkin), clean buffer reloaded on checkout, conflict state and abort, discard with
# confirmation and audit, stash, masked credentials, init, AI commit message with consent (mock), and the
# database untouched by any Git operation (Gherkin "Commit de uma procedure não publica no banco").
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
sh tests/gitcore_check.sh >/dev/null 2>&1 || { echo "FAIL  gitcore_check (headless) must pass first"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
missing() { printf '%s\n' "$1" | grep -c '^script: \(target not found\|git \)' ; }
blk() { printf '%s\n' "$1" | awk -v n="$2" '/^gitstate /{k++} k==n'; }
W=build/git-ui; sh tests/git_setup.sh $W
before=$(sqlite3 $W/pref/shop.db "SELECT sql FROM sqlite_master WHERE name = 'v_revenue'" | md5)
sed -e "s|WORK|$PWD/$W/work repo|" -e "s|NEWPROJ|$PWD/$W/newproj|" tests/scripts/git_ui.txt > build/git_ui_run.txt
out=$(LUME_PREF_DIR=$W/pref LUME_SCRIPT=build/git_ui_run.txt ./build/lume 2>&1)
printf '%s\n' "$out" > build/git_ui.out
R="$W/work repo"
expect "every script target found" '[ "$(missing "$out")" = 0 ]'
expect "GIT-001 / 005 / 008: repository opened: branch, upstream, ahead / behind, identity" 'blk "$out" 1 | grep -q "git=1 .*loaded=1 head=main .*upstream=origin/main ahead=0 behind=0" && blk "$out" 1 | grep -q "user=\"Lume Test\""'
expect "GIT-017 / 064: a file tab shows unsaved edits (dirty) until saved" 'blk "$out" 2 | grep -q "dirtyfile=1" && blk "$out" 3 | grep -q "dirtyfile=0 " && grep -q "file: saved .*v_revenue.sql" $W/pref/logs/lume.log'
expect "GIT-009: the saved file appears as modified" 'blk "$out" 3 | grep -q "unstaged=1"'
expect "Gherkin 'Commit apenas dos arquivos staged' / GIT-014: only the staged file went into the commit" '[ "$(git -C "$R" log -1 --format= --name-only HEAD~2 2>/dev/null; git -C "$R" show --name-only --format= $(git -C "$R" log --format=%h --grep="review the revenue view" -1))" != "" ] && [ "$(git -C "$R" show --name-only --format= $(git -C "$R" log --format=%h --grep="review the revenue view" -1))" = "sql/v_revenue.sql" ] && blk "$out" 4 | grep -q "gitentry .M ordinary notes.md"'
expect "GIT-014: the commit message (summary + description) is the one typed" 'git -C "$R" log --format=%B --grep="review the revenue view" -1 | grep -q "Adds a header comment."'
expect "GIT-012: the diff tab shows the file's hunk" 'blk "$out" 5 | grep -q "gitdiff tab=[0-9]* kind=0 files=1 hunks=1"'
expect "GIT-011: one hunk staged from the diff" 'blk "$out" 6 | grep -q "staged=1 unstaged=0" && blk "$out" 6 | grep -q "Hunk staged"'
expect "Gherkin 'Pull com buffer não salvo' / GIT-064: Pull stops and offers to preserve the buffer" 'blk "$out" 8 | grep -q "confirm=1" && printf "%s\n" "$out" | grep -q "^gitconfirm \"1 file(s) of this repository have unsaved edits"'
expect "Gherkin: cancelling keeps the buffer dirty and the file untouched" 'blk "$out" 9 | grep -q "confirm=0 .*dirtyfile=1 msg=\"Cancelled: nothing changed\""'
expect "Gherkin 'Push rejeitado' / GIT-029: the reason is shown, local commits kept, no force push" 'blk "$out" 10 | grep -q "Push failed. The remote has commits you do not have" && blk "$out" 10 | grep -q "ahead=2" && ! grep -q -- "--force\|push -f" $W/pref/logs/lume.log'
expect "GIT-027: fetch recalculates behind" 'blk "$out" 11 | grep -q "ahead=2 behind=1"'
expect "GIT-028: fast-forward-only pull refuses diverged branches and says what to do" 'blk "$out" 12 | grep -q "The branches diverged"'
expect "GIT-028: the explicit merge strategy pulls; then push succeeds" 'blk "$out" 13 | grep -q "behind=0" && blk "$out" 14 | grep -q "ahead=0 behind=0 .*Push: done"'
expect "Gherkin 'Git sem conexão' / GIT-018: a branch is created locally" 'blk "$out" 15 | grep -q "head=feature/relatório"'
expect "GIT-017: a clean file tab follows a checkout (reloaded, not overwritten)" 'printf "%s\n" "$out" | grep -q "^edtext rev=[0-9]* text=\"feature x notes" && grep -q "file: reloaded .*notes.md" $W/pref/logs/lume.log'
expect "GIT-023 / 067: a conflicting merge: state merging, 1 conflict, steps explained" 'blk "$out" 17 | grep -q "state=merging .*conflicts=1" && blk "$out" 17 | grep -q "resolve the <<<<<<<"'
expect "GIT-067 / 066: abort after confirmation; audited" 'blk "$out" 18 | grep -q "state=- .*conflicts=0" && grep -q "op=git.merge.abort .*result=ok confirmation=confirmed" $W/pref/audit/git.log'
expect "GIT-013: discard asks first (file, scope, no undo)" 'blk "$out" 19 | grep -q "confirm=1" && printf "%s\n" "$out" | grep -q "^gitconfirm \"Discard the unstaged changes of \\\"notes.md\\\"?"'
expect "GIT-013 / 066: confirmed discard restores the file and is audited" 'blk "$out" 20 | grep -q "unstaged=0 .*Discard changes: done" && grep -q "op=discard target=\"notes.md\" result=ok confirmation=confirmed" $W/pref/audit/git.log'
expect "GIT-022: stash saves and pop restores the change" 'blk "$out" 21 | grep -q "unstaged=0 .*Stash: done" && printf "%s\n" "$out" | grep -q "^gitstash stash@{0}" && blk "$out" 22 | grep -q "unstaged=1 .*Pop stash: done"'
expect "GIT-026 / 065: a remote URL with a token is masked in the UI; the fetch failure is explained without it" 'printf "%s\n" "$out" | grep -q "^gitremote corp https://\*\*\*@git.example.invalid" && blk "$out" 24 | grep -q "The host could not be reached"'
expect "GIT-065 / 070: no token in the log, the Output or the audit" '! grep -rq "ghp_ABCDEFG" $W/pref/logs $W/pref/audit 2>/dev/null'
expect "GIT-020: history lists the commits" 'printf "%s\n" "$out" | grep -q "^gitlog [0-9a-f]* feat: review the revenue view"'
expect "GIT-002: init after confirmation creates the repository (branch main)" '[ -d "$W/newproj/.git" ] && [ "$(git -C "$W/newproj" symbolic-ref --short HEAD)" = main ] && blk "$out" 26 | grep -q "Init: done"'
expect "GIT-070: operations are in the log with exit codes" 'grep -q "git: git.commit done in [0-9]* ms (exit 0)" $W/pref/logs/lume.log && grep -q "git: git.push failed" $W/pref/logs/lume.log'
expect "Gherkin 'Commit de uma procedure não publica no banco' / GIT-048: the database view is unchanged after commits of .sql files (DROP TABLE included)" '[ "$before" = "$(sqlite3 $W/pref/shop.db "SELECT sql FROM sqlite_master WHERE name = '"'"'v_revenue'"'"'" | md5)" ] && [ "$(sqlite3 $W/pref/shop.db "SELECT count(*) FROM rpt_sales")" = 1 ]'

# AI commit message with consent (GIT-058 / 062), through the loopback mock
Q=build/git-ai; sh tests/git_setup.sh $Q
printf 'TOKEN=abc123secret\n' > "$Q/work repo/.env"; printf 'SELECT 42;\n' > "$Q/work repo/sql/new.sql"
git -C "$Q/work repo" add -f .env sql/new.sql
pf=build/git_mock.port; rm -f $pf; python3 -I tests/tools/ai_mock.py > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
printf 'provider=anthropic\nendpoint=http://127.0.0.1:%s/v1/messages\nmodel=claude-opus-5-5\n' "$(head -1 $pf)" > $Q/pref/ai.txt
commits=$(git -C "$Q/work repo" rev-list --count HEAD)
printf 'wait 300\ngit open %s\ngit show\ngit await\ngit aisuggest\ngit await\ngit aipayload\ngitstate\ngit aisend\nwait 1500\ngit await\ngitstate\ngit aireview\ngit await\ngit aisend\nwait 1500\ngit await\ngit aireviewtext\ngitstate\nquit\n' "$PWD/$Q/work repo" > build/git_ai_run.txt
out=$(LUME_AI_KEY=sk-test-not-a-real-key LUME_PREF_DIR=$Q/pref LUME_SCRIPT=build/git_ai_run.txt ./build/lume 2>&1)
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $pf
pay=$(printf '%s\n' "$out" | awk '/^gitpayload$/{on=1; next} /^gitpayload-end/{on=0} on')
expect "GIT-058 / 062: the payload shown first is the staged diff, .env excluded by policy, no secret" 'printf "%s\n" "$pay" | grep -q "SELECT 42" && printf "%s\n" "$pay" | grep -q "excluded by policy" && ! printf "%s\n" "$pay" | grep -q "abc123secret"'
expect "GIT-058: the suggestion fills the message only; no commit is created by it" 'blk "$out" 2 | grep -q "Suggested message filled in" && [ "$(git -C "$Q/work repo" rev-list --count HEAD)" = "$commits" ]'
expect "GIT-062 / 066: the AI request is audited with its size, without content or key" 'grep -q "op=ai-commit-message target=\"[0-9]* bytes\" result=sent confirmation=user-reviewed-payload" $Q/pref/audit/git.log && ! grep -rq "sk-test-not-a-real-key\|abc123secret\|SELECT 42" $Q/pref/audit $Q/pref/logs'
expect "roadmap 21: the staged change is reviewed for risks (payload shown first); the review is shown, nothing committed" 'printf "%s\n" "$out" | awk "/^gitreview\$/{on=1; next} /^gitreview-end/{on=0} on" | grep -q "^- \[high\] sql/v_revenue.sql" && [ "$(git -C "$Q/work repo" rev-list --count HEAD)" = "$commits" ] && grep -q "op=ai-change-review" $Q/pref/audit/git.log'
if [ $fails = 0 ]; then echo "GIT UI: all passed"; else echo "GIT UI: FAILED ($fails failures)"; fi
exit $fails
