#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# OpenAI for Lume Intelligence through a loopback mock (tests/tools/openai_mock.py):
#   - an OpenAI API key pasted in Settings: models listed (chat models only), tested, kept in the OS
#     secret store, Responses API (store:false, non-streamed);
#   - "Continue with ChatGPT": OAuth code + PKCE S256 over the 127.0.0.1 loopback redirect, the issued
#     client id kept, the session in the secret store (never a file), plan models, SSE answers with
#     stream:true and none of the refused fields, token refresh with rotation, usage limit, a revoked
#     session signing the user out.
# The secret store uses a test service name, so the user's real entries are never read or touched.
cd "$(dirname "$0")/.."
export LUME_TOOLBAR=hidden LUME_SECSTORE_SERVICE="Lume Data Test"
cmake --build build >/dev/null || { echo "FAIL  build"; exit 1; }
fails=0
expect() { if eval "$2"; then echo "ok    $1"; else echo "FAIL  $1"; fails=$((fails + 1)); fi; }
st() { printf '%s\n' "$out" | grep '^rptsetup' | sed -n "$1p"; }

pf=build/openai_mock.port; rm -f $pf
python3 -I tests/tools/openai_mock.py > $pf & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $pf ] && break; sleep 0.2; done
port=$(head -1 $pf); base=http://127.0.0.1:$port
trap 'kill $mock 2>/dev/null' EXIT
export LUME_OPENAI_API_BASE=$base/v1 LUME_CHATGPT_AUTH_BASE=$base LUME_TEST_BROWSER=curl LUME_CHATGPT_PORT=$((20000 + $$ % 20000))
stats() { curl -s $base/stats; }
security delete-generic-password -s "Lume Data Test" -a chatgpt-refresh >/dev/null 2>&1
security delete-generic-password -s "Lume Data Test" -a chatgpt-access >/dev/null 2>&1

# ---- 1. API key ----
K=build/openai-key; rm -rf $K; mkdir -p $K; printf 'provider=none\n' > $K/ai.txt
out=$(LUME_PREF_DIR=$K LUME_SCRIPT=tests/scripts/openai_key.txt env -u OPENAI_API_KEY ./build/lume 2>&1)
expect "OpenAI key: with no key nothing is offered, the provider is OpenAI (API key)" 'st 1 | grep -q "provider=openai .*keysource=0"'
expect "OpenAI key: Load models lists the account's chat models only (no embeddings / realtime)" 'st 2 | grep -q "models=2 first=gpt-5.5"'
expect "OpenAI key: the pasted key is tested with the chosen model before anything is saved" 'st 3 | grep -q "test=2 .*cfg=none/.*keysource=0 keyfield=1"'
expect "OpenAI key: Enable saves the key in the OS secret store, ai.txt says provider=openai" 'st 4 | grep -q "open=0 .*cfg=openai/gpt-5.5 keysource=2 keyfield=0" && grep -q "^provider=openai" $K/ai.txt && ! grep -q "^endpoint=" $K/ai.txt'
expect "OpenAI key: the next request reads the key back from the store (Responses API, store:false)" 'st 5 | grep -q "test=2 .*keysource=2"'
expect "OpenAI key: Remove deletes it from the store" 'st 6 | grep -q "keysource=0" && ! security find-generic-password -s "Lume Data Test" -a openai-api-key >/dev/null 2>&1'
expect "OpenAI key: the key is in no file of the prefs folder; no request file is left" '! grep -rq "sk-oa-test" $K && ! ls $K/lume-ai-* >/dev/null 2>&1'
expect "OpenAI key: the mock refused nothing Lume sent" 'stats | grep -q "\"rejected\": 0"'

# ---- 2. Continue with ChatGPT (tokens expire in 1 s, so every use refreshes) ----
curl -s "$base/ctl?expires=1" >/dev/null
C=build/openai-plan; rm -rf $C; mkdir -p $C; printf 'provider=none\n' > $C/ai.txt
out=$(LUME_PREF_DIR=$C LUME_SCRIPT=tests/scripts/openai_plan.txt ./build/lume 2>&1)
expect "ChatGPT: not signed in at first" 'st 1 | grep -q "provider=chatgpt .*signedin=0"'
expect "ChatGPT: the browser sign-in (PKCE S256, loopback 127.0.0.1) signs in and loads the plan's listed models" 'st 2 | grep -q "signedin=1 email=isa@example.com signing=0 models=2 first=gpt-5.5 "'
expect "ChatGPT: the issued client id is kept for the next sign-in (no secrets in that file)" 'grep -q "^client_id=oaiapp_lume_test" $C/chatgpt.txt && grep -q "^host_id=urn:uuid:" $C/chatgpt.txt && ! grep -q "tok-" $C/chatgpt.txt'
expect "ChatGPT: the test runs on the plan (SSE, stream:true, no refused fields)" 'st 3 | grep -q "test=2 .*signedin=1"'
expect "ChatGPT: Enable saves provider=chatgpt and shows the plan note once" 'st 4 | grep -q "open=0 .*cfg=chatgpt/gpt-5.5 .*welcome=1"'
expect "ChatGPT: a later request after the access token expired refreshes it (rotation) and works" 'st 5 | grep -q "test=2 .*welcome=0" && stats | grep -q "\"refresh\": [1-9]" && stats | grep -q "\"reused\": 0"'
expect "ChatGPT: no token in any file of the prefs folder; no request file is left" '! grep -rq "tok-access\|tok-refresh\|code-[0-9]" $C && ! ls $C/lume-ai-* >/dev/null 2>&1'
expect "ChatGPT: the session is in the OS secret store" 'security find-generic-password -s "Lume Data Test" -a chatgpt-refresh >/dev/null 2>&1'
expect "ChatGPT: the mock refused nothing Lume sent (PKCE, client id, stream, store, fields)" 'stats | grep -q "\"rejected\": 0" && stats | grep -q "\"code\": 1"'

# ---- 3. usage limit, then a revoked session ----
curl -s "$base/ctl?usage=1" >/dev/null
out=$(LUME_PREF_DIR=$C LUME_SCRIPT=tests/scripts/openai_plan2.txt ./build/lume 2>&1)
expect "ChatGPT: usage limit reached is said in words, with Manage usage" 'st 1 | grep -q "test=3 msg=\"Usage limit reached for Lume on your ChatGPT plan. Manage usage: https://chatgpt.com/settings/usage\""'
curl -s "$base/ctl?usage=0&revoke=1" >/dev/null
out=$(LUME_PREF_DIR=$C LUME_SCRIPT=tests/scripts/openai_plan2.txt ./build/lume 2>&1)
expect "ChatGPT: a refused refresh ends the session: signed out, asked to sign in again" 'st 1 | grep -q "test=3 msg=\"The ChatGPT session ended.*sign in again\".*signedin=0" && ! security find-generic-password -s "Lume Data Test" -a chatgpt-refresh >/dev/null 2>&1'
expect "the audit records sign-in and tests without tokens" 'grep -q "chatgpt-signin.*signed-in" $C/audit/report.log && ! grep -q "tok-" $C/audit/report.log'

if [ $fails = 0 ]; then echo "OPENAI UI: all passed"; else echo "OPENAI UI: FAILED ($fails failures)"; fi
exit $fails
