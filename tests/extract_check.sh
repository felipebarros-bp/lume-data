#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Builds and runs the sandboxed-extractor checks (US-045) in a throwaway folder.
set -e
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -o build/extract_check tests/extract_check.c src/extract.c
rm -rf build/extract_check_dir
build/extract_check "$PWD/build/extract_check_dir"
