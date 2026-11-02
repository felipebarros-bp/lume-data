#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Execution plans + Lume Intelligence, headless (ASan): real fixtures of 6 engines, capture SQL, diagnostics, AI request / response, loopback mock.
cd "$(dirname "$0")/.."
mkdir -p build
SYSLIBS="-ldl"; [ "$(uname -s)" = Darwin ] && SYSLIBS="-framework CoreFoundation -framework Security"   # secstore.c: Keychain / libsecret via dlopen
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -Itests/shim -o build/plan_check tests/plan_check.c src/plan.c src/ai.c src/secstore.c src/chatgpt.c src/jw.c src/sha256.c src/json.c src/rs.c tests/rs_stub.c -lpthread $SYSLIBS || exit 1
fifo=build/ai_mock.port; rm -f $fifo
python3 -I tests/tools/ai_mock.py > $fifo & mock=$!
for i in 1 2 3 4 5 6 7 8 9 10; do [ -s $fifo ] && break; sleep 0.2; done
LUME_SECSTORE_SERVICE="Lume Data Test" LUME_AI_MOCK_PORT=$(head -1 $fifo) build/plan_check; rc=$?
kill $mock 2>/dev/null; wait $mock 2>/dev/null; rm -f $fifo
exit $rc
