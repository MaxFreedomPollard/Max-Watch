#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
DEV_DIR="$BUILD_DIR/DeveloperID"
APP_PATH="$DEV_DIR/Max Watch.app"
VERSION="1.2.0"
DMG_PATH="$DEV_DIR/Max-Watch-$VERSION.dmg"
PKG_UNSIGNED="$DEV_DIR/Max-Watch-$VERSION-component.pkg"
PKG_PATH="$DEV_DIR/Max-Watch-$VERSION.pkg"

APP_SIGN_ID="Developer ID Application: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"
INST_SIGN_ID="Developer ID Installer: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"

KEY_FILE="/Users/max/.appstoreconnect/private_keys/AuthKey_2K68Y89NCH.p8"
KEY_ID="2K68Y89NCH"
ISSUER_ID="71def431-23db-4694-aaad-f179223eb493"

if [ ! -d "$APP_PATH" ]; then
  "$ROOT_DIR/scripts/build_universal.sh"
fi

echo "=== [1/5] Signing Max Watch.app with Developer ID Application + Hardened Runtime ==="
xattr -cr "$APP_PATH"
codesign --force --options runtime --timestamp \
  --entitlements "$ROOT_DIR/Resources/DeveloperID.entitlements" \
  --sign "$APP_SIGN_ID" \
  "$APP_PATH"

codesign --verify --deep --strict --verbose=2 "$APP_PATH"
echo "Entitlements check:"
codesign -d --entitlements :- "$APP_PATH"

echo "=== [2/5] Notarizing & Stapling Max Watch.app ==="
APP_ZIP="$DEV_DIR/Max-Watch-app.zip"
rm -f "$APP_ZIP"
ditto -c -k --sequesterRsrc --keepParent "$APP_PATH" "$APP_ZIP"
xcrun notarytool submit "$APP_ZIP" \
  --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER_ID" \
  --wait
rm -f "$APP_ZIP"

xcrun stapler staple "$APP_PATH"
xcrun stapler validate "$APP_PATH"

echo "=== [3/5] Building Signed DMG & Signed PKG from Stapled App ==="
DMG_STAGE="$DEV_DIR/dmg_stage"
rm -rf "$DMG_STAGE" "$DMG_PATH" "$PKG_UNSIGNED" "$PKG_PATH"
mkdir -p "$DMG_STAGE"
ditto "$APP_PATH" "$DMG_STAGE/Max Watch.app"
ln -s /Applications "$DMG_STAGE/Applications"

hdiutil create \
  -volname "Max Watch" \
  -srcfolder "$DMG_STAGE" \
  -ov -format UDZO \
  "$DMG_PATH"
rm -rf "$DMG_STAGE"

codesign --force --timestamp --sign "$APP_SIGN_ID" "$DMG_PATH"

pkgbuild \
  --component "$APP_PATH" \
  --install-location "/Applications" \
  --version "$VERSION" \
  "$PKG_UNSIGNED"

productbuild \
  --package "$PKG_UNSIGNED" \
  --sign "$INST_SIGN_ID" \
  --timestamp \
  "$PKG_PATH"
rm -f "$PKG_UNSIGNED"

echo "=== [4/5] Notarizing & Stapling DMG and PKG ==="
xcrun notarytool submit "$DMG_PATH" \
  --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER_ID" \
  --wait
xcrun stapler staple "$DMG_PATH"
xcrun stapler validate "$DMG_PATH"

xcrun notarytool submit "$PKG_PATH" \
  --key "$KEY_FILE" --key-id "$KEY_ID" --issuer "$ISSUER_ID" \
  --wait
xcrun stapler staple "$PKG_PATH"
xcrun stapler validate "$PKG_PATH"

echo "=== [5/5] Installing to /Applications & Running Gatekeeper Verification ==="
rm -rf "/Applications/Max Watch.app"
ditto "$APP_PATH" "/Applications/Max Watch.app"

spctl -a -vvv -t exec "/Applications/Max Watch.app"
spctl -a -vvv -t open --context context:primary-signature "$DMG_PATH"
spctl -a -vvv -t install "$PKG_PATH"

shasum -a 256 "$DMG_PATH" "$PKG_PATH" | tee "$DEV_DIR/SHA256SUMS.txt"
echo "Developer ID Release Artifacts Ready:"
ls -lh "$DMG_PATH" "$PKG_PATH"
