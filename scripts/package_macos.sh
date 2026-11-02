#!/bin/sh
# SPDX-License-Identifier: AGPL-3.0-only
# Packages build/Lume Data.app into a DMG that runs on other Macs:
#   1. SDL3 goes inside the bundle (Contents/Frameworks), the binary points at it (@rpath);
#   2. signing: with $LUME_SIGN_IDENTITY (a "Developer ID Application" identity) the dylib and the app are
#      signed with the hardened runtime and cmake/lume.entitlements; without it, an ad-hoc signature
#      (runs on this Mac; other Macs need right-click > Open);
#   3. the DMG (the app + a link to /Applications), signed with the same identity;
#   4. notarization, only when either
#      - $LUME_NOTARY_PROFILE names a `xcrun notarytool store-credentials` profile in the Keychain (local), or
#      - $LUME_NOTARY_KEY (path of an App Store Connect API key .p8), $LUME_NOTARY_KEY_ID and
#        $LUME_NOTARY_ISSUER are set (CI: the key file is written from a secret, then removed);
#      then the ticket is stapled to the DMG. No password ever goes on a command line.
# Usage: sh scripts/package_macos.sh [build dir]   ->   <build dir>/Lume-Data-<version>-macos-<arch>.dmg
# Never prints a secret.
set -eu
cd "$(dirname "$0")/.."
B=${1:-build}
APP="$B/Lume Data.app"
[ -d "$APP" ] || { echo "no $APP: build first (cmake --build $B)"; exit 1; }
VERSION=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleShortVersionString' "$APP/Contents/Info.plist")
BIN="$APP/Contents/MacOS/lume"
ARCHS=$(lipo -archs "$BIN")
case "$ARCHS" in *arm64*x86_64*|*x86_64*arm64*) ARCH=universal ;; *) ARCH=$(echo "$ARCHS" | tr ' ' '-') ;; esac
echo "Lume Data $VERSION ($ARCHS)"

# 1. SDL3 inside the bundle
SDL=$(otool -L "$BIN" | awk '/libSDL3/ { print $1; exit }')
[ -n "$SDL" ] || { echo "the binary does not link SDL3?"; exit 1; }
case "$SDL" in
  @rpath/*) SDL_FILE=$(find "${SDL3_PREFIX:-/opt/homebrew}" -name "$(basename "$SDL")" -path '*lib*' 2>/dev/null | head -1) ;;
  *) SDL_FILE=$SDL ;;
esac
[ -f "$SDL_FILE" ] || { echo "SDL3 dylib not found ($SDL): set SDL3_PREFIX"; exit 1; }
mkdir -p "$APP/Contents/Frameworks"
cp -fL "$SDL_FILE" "$APP/Contents/Frameworks/libSDL3.0.dylib"
chmod u+w "$APP/Contents/Frameworks/libSDL3.0.dylib"
install_name_tool -id @rpath/libSDL3.0.dylib "$APP/Contents/Frameworks/libSDL3.0.dylib"
install_name_tool -change "$SDL" @rpath/libSDL3.0.dylib "$BIN"
# only the bundle's own rpath stays (the build machine's library folders say nothing on another Mac)
for rp in $(otool -l "$BIN" | awk '/LC_RPATH/ { getline; getline; print $2 }' | sort -u); do   # once per path: a universal binary lists each arch
  [ "$rp" = "@executable_path/../Frameworks" ] || install_name_tool -delete_rpath "$rp" "$BIN"
done
otool -l "$BIN" | grep -q '@executable_path/../Frameworks' || install_name_tool -add_rpath @executable_path/../Frameworks "$BIN"
if otool -L "$BIN" | grep -E '/opt/homebrew|/usr/local' >/dev/null; then echo "the binary still points at a local library:"; otool -L "$BIN"; exit 1; fi

# 2. signing (inside out: the dylib, then the app)
ENT=cmake/lume.entitlements
if [ -n "${LUME_SIGN_IDENTITY:-}" ]; then
  codesign --force --timestamp --options runtime --sign "$LUME_SIGN_IDENTITY" "$APP/Contents/Frameworks/libSDL3.0.dylib"
  codesign --force --timestamp --options runtime --entitlements "$ENT" --sign "$LUME_SIGN_IDENTITY" "$APP"
else
  echo "no LUME_SIGN_IDENTITY: ad-hoc signature (not for distribution)"
  codesign --force --sign - "$APP/Contents/Frameworks/libSDL3.0.dylib"
  codesign --force --sign - "$APP"
fi
codesign --verify --strict --deep "$APP"

# 3. the DMG
DMG="$B/Lume-Data-$VERSION-macos-$ARCH.dmg"
STAGE=$(mktemp -d)
cp -R "$APP" "$STAGE/"
ln -s /Applications "$STAGE/Applications"
rm -f "$DMG"
hdiutil create -volname "Lume Data $VERSION" -srcfolder "$STAGE" -fs HFS+ -format UDZO -ov "$DMG" >/dev/null
rm -rf "$STAGE"
[ -n "${LUME_SIGN_IDENTITY:-}" ] && codesign --force --timestamp --sign "$LUME_SIGN_IDENTITY" "$DMG"

# 4. notarization (optional)
if [ -n "${LUME_SIGN_IDENTITY:-}" ] && { [ -n "${LUME_NOTARY_PROFILE:-}" ] || [ -n "${LUME_NOTARY_KEY:-}" ]; }; then
  echo "notarizing (this waits for Apple)"
  if [ -n "${LUME_NOTARY_PROFILE:-}" ]; then
    xcrun notarytool submit "$DMG" --keychain-profile "$LUME_NOTARY_PROFILE" --wait
  else
    xcrun notarytool submit "$DMG" --key "$LUME_NOTARY_KEY" --key-id "$LUME_NOTARY_KEY_ID" --issuer "$LUME_NOTARY_ISSUER" --wait
  fi
  xcrun stapler staple "$DMG"
  spctl --assess --type open --context context:primary-signature -v "$DMG" || true
fi
shasum -a 256 "$DMG"
echo "$DMG"
