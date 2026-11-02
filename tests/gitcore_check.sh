#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Lume Git Core, headless (ASan), against real Git in build/gitc: runner, redaction, discovery, status,
# diff / hunk staging, log / refs / remotes, conflicts, worktrees, 1,000 files, credential-less push.
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Wno-unused-parameter -Isrc -o build/gitcore_check tests/gitcore_check.c src/gitcore.c || exit 1
build/gitcore_check
