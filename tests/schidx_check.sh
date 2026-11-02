#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Schema index for Lume Intelligence on large databases, headless (ASan / UBSan): a synthetic 15,000-table
# catalog; picking tables for a question (accents, plural, abbreviations, descriptions, FK neighbours,
# tables in use, audit copies last), search_schema, the <schema> byte budget, untrusted catalog text,
# the disk cache and the bulk catalog SQL per engine.
cd "$(dirname "$0")/.."
mkdir -p build
cc -std=gnu11 -O1 -g -fsanitize=address,undefined -Wall -Wextra -Isrc -o build/schidx_check tests/schidx_check.c src/schidx.c -lm || exit 1
build/schidx_check
