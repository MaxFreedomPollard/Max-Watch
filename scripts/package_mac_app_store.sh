#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
ACTOOL_DIR="$BUILD_DIR/actool"
BIN_DIR="$BUILD_DIR/bin"
MAS_DIR="$BUILD_DIR/MacAppStore"
MAS_APP="$MAS_DIR/Max Watch.app"
MAS_PKG="$MAS_DIR/Max-Watch-1.4.0-MAS.pkg"
PROFILE_PATH="/Users/max/.appstoreconnect/profiles/app_c60_maxwatch_Mac_App_Store.provisionprofile"

MAS_APP_SIGN_ID="3rd Party Mac Developer Application: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"
MAS_INST_SIGN_ID="3rd Party Mac Developer Installer: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"

if [ ! -f "$BIN_DIR/MaxWatch" ]; then
  "$ROOT_DIR/scripts/build_universal.sh"
fi

if [ ! -f "$PROFILE_PATH" ]; then
  echo "Missing MAS provisioning profile at $PROFILE_PATH" >&2
  exit 1
fi

echo "=== [1/3] Assembling Mac App Store Bundle ==="
rm -rf "$MAS_APP" "$MAS_PKG"
mkdir -p "$MAS_APP/Contents/MacOS" "$MAS_APP/Contents/Resources"
cp "$BIN_DIR/MaxWatch" "$MAS_APP/Contents/MacOS/MaxWatch"
cp "$ROOT_DIR/Resources/Info.plist" "$MAS_APP/Contents/Info.plist"
cp "$ACTOOL_DIR/Assets.car" "$MAS_APP/Contents/Resources/Assets.car"
cp "$ROOT_DIR/Resources/AppIcon.icns" "$MAS_APP/Contents/Resources/AppIcon.icns"
cp "$PROFILE_PATH" "$MAS_APP/Contents/embedded.provisionprofile"
printf "APPL????" > "$MAS_APP/Contents/PkgInfo"
xattr -cr "$MAS_APP"

echo "=== [2/3] Signing Mac App Store App Bundle ==="
codesign --force --timestamp \
  --entitlements "$ROOT_DIR/Resources/MacAppStore.entitlements" \
  --sign "$MAS_APP_SIGN_ID" \
  "$MAS_APP"

codesign --verify --deep --strict --verbose=2 "$MAS_APP"
codesign -d --entitlements :- "$MAS_APP"

echo "=== [3/3] Building Signed Mac App Store Installer PKG ==="
productbuild \
  --component "$MAS_APP" "/Applications" \
  --sign "$MAS_INST_SIGN_ID" \
  "$MAS_PKG"

pkgutil --check-signature "$MAS_PKG"
ls -lh "$MAS_PKG"
