#!/usr/bin/env bash
set -euo pipefail

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
BUILD_DIR="$ROOT_DIR/build"
ACTOOL_DIR="$BUILD_DIR/actool"
BIN_DIR="$BUILD_DIR/bin"
DEV_APP="$BUILD_DIR/DeveloperID/Max Watch.app"

mkdir -p "$ACTOOL_DIR" "$BIN_DIR" "$BUILD_DIR/DeveloperID" "$BUILD_DIR/MacAppStore"

echo "=== [1/4] Generating MF Icons & Compiling Asset Catalog ==="
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

echo "=== [2/4] Compiling arm64 + x86_64 Universal Binary (macOS 13.0+) ==="
xcrun swiftc -O -swift-version 5 \
  -sdk "$SDK_PATH" \
  -target arm64-apple-macos13.0 \
  "${SOURCES[@]}" "${FRAMEWORKS[@]}" \
  -o "$BIN_DIR/MaxWatch-arm64"

xcrun swiftc -O -swift-version 5 \
  -sdk "$SDK_PATH" \
  -target x86_64-apple-macos13.0 \
  "${SOURCES[@]}" "${FRAMEWORKS[@]}" \
  -o "$BIN_DIR/MaxWatch-x86_64"

lipo -create -output "$BIN_DIR/MaxWatch" "$BIN_DIR/MaxWatch-arm64" "$BIN_DIR/MaxWatch-x86_64"
chmod 755 "$BIN_DIR/MaxWatch"
echo "Universal architectures: $(lipo -archs "$BIN_DIR/MaxWatch")"

echo "=== [3/4] Assembling Max Watch.app Bundle ==="
rm -rf "$DEV_APP"
mkdir -p "$DEV_APP/Contents/MacOS" "$DEV_APP/Contents/Resources"
cp "$BIN_DIR/MaxWatch" "$DEV_APP/Contents/MacOS/MaxWatch"
cp "$ROOT_DIR/Resources/Info.plist" "$DEV_APP/Contents/Info.plist"
cp "$ACTOOL_DIR/Assets.car" "$DEV_APP/Contents/Resources/Assets.car"
cp "$ROOT_DIR/Resources/AppIcon.icns" "$DEV_APP/Contents/Resources/AppIcon.icns"
printf "APPL????" > "$DEV_APP/Contents/PkgInfo"
xattr -cr "$DEV_APP"

echo "=== [4/4] Running 12-Stage Automated Cryptographic, Sound-Wake, Night-Brightness & Fast-Scrub Self-Test ==="
"$DEV_APP/Contents/MacOS/MaxWatch" --selftest "$BUILD_DIR/selftest_tmp"
echo "Build & Self-Test Complete: $DEV_APP"
