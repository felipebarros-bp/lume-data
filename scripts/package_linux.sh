#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Packages the Linux build as a portable folder in a .tar.gz:
#   lume-data-<version>-linux-<arch>/
#     lume              the binary (rpath $ORIGIN/lib)
#     lib/libSDL3.so.0  SDL3, next to it
#     assets/           fonts, icons, demo data (found next to the binary, like on macOS / Windows)
#     lume-data.desktop + lume-data.png   for a menu entry (see README in the archive)
# SQLite and the database client libraries come from the system (libsqlite3; libpq, Oracle Instant
# Client, ... are loaded at run time when present). Needs patchelf.
# Usage: sh scripts/package_linux.sh [build dir] [SDL3 prefix]
set -eu
cd "$(dirname "$0")/.."
B=${1:-build}
SDLP=${2:-/usr/local}
[ -x "$B/lume" ] || { echo "no $B/lume: build first"; exit 1; }
VERSION=$(sed -n 's/^project(lume VERSION \([0-9.]*\).*/\1/p' CMakeLists.txt)
ARCH=$(uname -m)
NAME="lume-data-$VERSION-linux-$ARCH"
OUT="$B/$NAME"
rm -rf "$OUT"; mkdir -p "$OUT/lib"
cp "$B/lume" "$OUT/lume"
SDL_SO=$(ldd "$B/lume" | awk '/libSDL3/ { print $3; exit }')
[ -f "$SDL_SO" ] || SDL_SO=$(find "$SDLP" -name 'libSDL3.so.0' | head -1)
[ -f "$SDL_SO" ] || { echo "libSDL3.so.0 not found (SDL3 prefix: $SDLP)"; exit 1; }
cp -L "$SDL_SO" "$OUT/lib/libSDL3.so.0"
patchelf --set-rpath '$ORIGIN/lib' "$OUT/lume"
cp -R assets "$OUT/assets"
cp packaging/linux/lume-data.desktop "$OUT/"
cp assets/brand/lume-data-app-dark-256.png "$OUT/lume-data.png"
cp LICENSE "$OUT/"
cat > "$OUT/README.txt" <<EOF
Lume Data $VERSION for Linux ($ARCH), portable.

Run: ./lume
Needs: libsqlite3 (most distributions have it), an X11 or Wayland desktop.
Optional, for those databases: libpq (PostgreSQL), Oracle Instant Client, libmysqlclient / libmariadb,
FreeTDS (SQL Server). Passwords and AI keys go to the Secret Service (libsecret) when it is present.
Menu entry: edit Exec= and Icon= in lume-data.desktop to this folder, then copy it to
~/.local/share/applications/.
License: GNU AGPL-3.0-only (LICENSE).
EOF
tar -C "$B" -czf "$B/$NAME.tar.gz" "$NAME"
sha256sum "$B/$NAME.tar.gz"
echo "$B/$NAME.tar.gz"
