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
MAS_KEYCHAIN="/Users/max/.appstoreconnect/certificates/mas_signing.keychain-db"
MAS_PW_FILE="/Users/max/.appstoreconnect/certificates/p12_password.txt"

MAS_APP_SIGN_ID="3rd Party Mac Developer Application: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"
MAS_INST_SIGN_ID="3rd Party Mac Developer Installer: MAX FREEDOMMOUSSA POLLARD (9M5WZ2C3V6)"

if [ ! -f "$PROFILE_PATH" ]; then
  echo "Missing MAS provisioning profile at $PROFILE_PATH" >&2
  exit 1
fi

mkdir -p "$ACTOOL_DIR" "$BIN_DIR" "$MAS_DIR"

echo "=== [1/5] Generating Icons & Compiling Asset Catalog ==="
swift "$ROOT_DIR/scripts/generate_icon.swift" "$ROOT_DIR"
xcrun actool \
  --compile "$ACTOOL_DIR" \
  --platform macosx \
  --minimum-deployment-target 13.0 \
  --app-icon AppIcon \
  --output-partial-info-plist "$ACTOOL_DIR/partial-info.plist" \
  "$ROOT_DIR/Resources/Assets.xcassets" >/dev/null

SDK_PATH="$(xcrun --sdk macosx --show-sdk-path)"
SOURCES=(
  "$ROOT_DIR/Sources/VaultCrypto.swift"
  "$ROOT_DIR/Sources/VaultSegmentWriter.swift"
  "$ROOT_DIR/Sources/VaultResourceLoader.swift"
  "$ROOT_DIR/Sources/ScrubberTimelineView.swift"
  "$ROOT_DIR/Sources/SoundDetection.swift"
  "$ROOT_DIR/Sources/SleepAndLockManager.swift"
  "$ROOT_DIR/Sources/SelfTest.swift"
  "$ROOT_DIR/Sources/AppController.swift"
  "$ROOT_DIR/Sources/main.swift"
)
FRAMEWORKS=(
  -framework AppKit
  -framework AVFoundation
  -framework AVKit
  -framework CoreGraphics
  -framework CoreMedia
  -framework CoreText
  -framework CoreVideo
  -framework CryptoKit
  -framework Foundation
  -framework IOKit
  -framework Security
  -framework UniformTypeIdentifiers
)

echo "=== [2/5] Compiling Mac App Store Universal Binary (-DMAC_APP_STORE, zero private APIs) ==="
xcrun swiftc -O -swift-version 5 -DMAC_APP_STORE \
  -sdk "$SDK_PATH" \
  -target arm64-apple-macos13.0 \
  "${SOURCES[@]}" "${FRAMEWORKS[@]}" \
  -o "$MAS_DIR/MaxWatch-mas-arm64"

xcrun swiftc -O -swift-version 5 -DMAC_APP_STORE \
  -sdk "$SDK_PATH" \
  -target x86_64-apple-macos13.0 \
  "${SOURCES[@]}" "${FRAMEWORKS[@]}" \
  -o "$MAS_DIR/MaxWatch-mas-x86_64"

lipo -create -output "$MAS_DIR/MaxWatch-mas" "$MAS_DIR/MaxWatch-mas-arm64" "$MAS_DIR/MaxWatch-mas-x86_64"
chmod 755 "$MAS_DIR/MaxWatch-mas"
echo "Universal MAS architectures: $(lipo -archs "$MAS_DIR/MaxWatch-mas")"

echo "=== [3/5] Fail-Closed Private API Binary Audit ==="
if strings "$MAS_DIR/MaxWatch-mas" | grep -E "PrivateFrameworks|DisplayServices" >/dev/null; then
  echo "FAIL: Private API symbol/string found in MAS binary!" >&2
  strings "$MAS_DIR/MaxWatch-mas" | grep -E "PrivateFrameworks|DisplayServices" >&2
  exit 1
fi
echo "PASS: Zero PrivateFrameworks / DisplayServices references in MAS binary."

echo "=== [4/5] Assembling & Signing Mac App Store Bundle ==="
rm -rf "$MAS_APP" "$MAS_PKG"
mkdir -p "$MAS_APP/Contents/MacOS" "$MAS_APP/Contents/Resources"
cp "$MAS_DIR/MaxWatch-mas" "$MAS_APP/Contents/MacOS/MaxWatch"
cp "$ROOT_DIR/Resources/Info.plist" "$MAS_APP/Contents/Info.plist"
/usr/libexec/PlistBuddy -c "Set :NSHumanReadableCopyright 'Copyright © 2026 C60 LLC'" "$MAS_APP/Contents/Info.plist"
cp "$ACTOOL_DIR/Assets.car" "$MAS_APP/Contents/Resources/Assets.car"
cp "$ROOT_DIR/Resources/AppIcon.icns" "$MAS_APP/Contents/Resources/AppIcon.icns"
cp "$PROFILE_PATH" "$MAS_APP/Contents/embedded.provisionprofile"
printf "APPL????" > "$MAS_APP/Contents/PkgInfo"
xattr -cr "$MAS_APP"

if [ -f "$MAS_KEYCHAIN" ] && [ -f "$MAS_PW_FILE" ]; then
  security unlock-keychain -p "$(cat "$MAS_PW_FILE")" "$MAS_KEYCHAIN"
  EXISTING_KCS="$(security list-keychains -d user | tr -d '\"' | xargs)"
  if [[ "$EXISTING_KCS" != *"$MAS_KEYCHAIN"* ]]; then
    security list-keychains -d user -s $EXISTING_KCS "$MAS_KEYCHAIN"
  fi
fi

codesign --force --timestamp \
  --entitlements "$ROOT_DIR/Resources/MacAppStore.entitlements" \
  --sign "$MAS_APP_SIGN_ID" \
  "$MAS_APP"

codesign --verify --deep --strict --verbose=2 "$MAS_APP"
codesign -d --entitlements :- "$MAS_APP"

echo "=== [5/5] Running 12-Stage Self-Test on MAS Binary & Building Signed PKG ==="
"$MAS_DIR/MaxWatch-mas" --selftest "$BUILD_DIR/selftest_mas_tmp"
rm -rf "$BUILD_DIR/selftest_mas_tmp"

productbuild \
  --component "$MAS_APP" "/Applications" \
  --sign "$MAS_INST_SIGN_ID" \
  "$MAS_PKG"

pkgutil --check-signature "$MAS_PKG"
ls -lh "$MAS_PKG"
