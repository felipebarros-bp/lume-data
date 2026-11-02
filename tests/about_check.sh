#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# About metadata, headless: once with build metadata, once with none.
cd "$(dirname "$0")/.."
mkdir -p build
F=$(pkg-config --cflags --libs sdl3)
fails=0
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -DWITH_METADATA -DLUME_VERSION='"0.1.0"' -DLUME_CHANNEL='"Early Preview"' -DLUME_BUILD_ID='""' -DLUME_WEBSITE_URL='"https://docs.lume.test/start"' \
  -o build/about_check tests/about_check.c src/product.c $F && build/about_check || fails=1
cc -std=gnu11 -O1 -Wall -Wextra -Wno-unused-parameter -Isrc -o build/about_check_bare tests/about_check.c src/product.c $F && build/about_check_bare || fails=1
exit $fails
