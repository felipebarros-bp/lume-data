#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# US-045 on Linux: builds tests/extract_check.c in a throwaway Debian container with bubblewrap
# and runs it as an ordinary user, so the real bwrap sandbox (namespaces, no network, limits)
# is exercised. --privileged lets bwrap mount its own /proc inside the container, as it can on
# a Linux host; the container is removed afterwards (--rm) and only reads the source tree.
cd "$(dirname "$0")/.."
command -v docker >/dev/null || { echo "skip  Linux extractor checks: docker not available"; exit 0; }
docker run --rm --privileged -v "$PWD:/src:ro" debian:12-slim sh -c '
  apt-get update -qq >/dev/null && apt-get install -y -qq gcc libc6-dev bubblewrap python3 >/dev/null 2>&1 || exit 2
  useradd -m tester && mkdir -p /w && chown tester /w && cd /w
  gcc -std=gnu11 -O1 -I/src/src -o extract_check /src/tests/extract_check.c /src/src/extract.c || exit 2
  echo "kernel $(uname -r), $(bwrap --version)"
  su tester -c "./extract_check /w/dir"'
