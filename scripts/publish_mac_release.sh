#!/usr/bin/env bash
set -euo pipefail

export PATH="/opt/homebrew/bin:/usr/local/bin:$PATH"

ROOT_DIR="$(cd "$(dirname "$0")/.." && pwd)"
cd "$ROOT_DIR"

VERSION="1.4.0"
TAG="v${VERSION}"
REPO="MaxFreedomPollard/Max-Watch"
DEV_DIR="$ROOT_DIR/build/DeveloperID"
DMG_PATH="$DEV_DIR/Max-Watch-${VERSION}.dmg"
PKG_PATH="$DEV_DIR/Max-Watch-${VERSION}.pkg"
SUMS_PATH="$DEV_DIR/SHA256SUMS.txt"

if ! command -v gh >/dev/null 2>&1; then
  echo "ERROR: GitHub CLI (gh) is not installed. Run: brew install gh && gh auth login" >&2
  exit 1
fi

if ! gh auth status >/dev/null 2>&1; then
  echo "ERROR: GitHub CLI is not logged in. Run: gh auth login" >&2
  exit 1
fi

echo "=== [Step 1/4] Building Universal (arm64 + x86_64) Max Watch ${VERSION} & Running 12-Stage Self-Test ==="
chmod +x "$ROOT_DIR/scripts/build_universal.sh" "$ROOT_DIR/scripts/package_developer_id.sh"
"$ROOT_DIR/scripts/build_universal.sh"

echo "=== [Step 2/4] Signing & Notarizing Max Watch ${VERSION} (.app, .dmg, .pkg) ==="
"$ROOT_DIR/scripts/package_developer_id.sh"

echo "=== [Step 3/4] Uploading Signed & Notarized macOS .dmg and .pkg to ${TAG} ==="
gh release upload "$TAG" \
  -R "$REPO" \
  "$DMG_PATH" \
  "$PKG_PATH" \
  --clobber

DMG_SHA="$(awk '/Max-Watch-1\.4\.0\.dmg/ {print $1}' "$SUMS_PATH" | head -n 1)"
PKG_SHA="$(awk '/Max-Watch-1\.4\.0\.pkg/ {print $1}' "$SUMS_PATH" | head -n 1)"

echo "=== [Step 4/4] Updating SHA-256 Checksums & Publishing ${TAG} Live ==="
NOTES_TMP="$(mktemp /tmp/mw_release_notes.XXXXXX)"
python3 - "$REPO" "$TAG" "$DMG_SHA" "$PKG_SHA" "$NOTES_TMP" <<'PY'
import subprocess, sys

repo, tag, dmg_sha, pkg_sha, out_path = sys.argv[1:6]
body = subprocess.check_output(
    ["gh", "release", "view", tag, "-R", repo, "--json", "body", "--jq", ".body"],
    text=True,
).replace("\r\n", "\n")

if dmg_sha:
    body = body.replace(
        "<pending-notarized-mac-upload>  Max-Watch-1.4.0.dmg",
        f"{dmg_sha}  Max-Watch-1.4.0.dmg",
    )
if pkg_sha:
    body = body.replace(
        "<pending-notarized-mac-upload>  Max-Watch-1.4.0.pkg",
        f"{pkg_sha}  Max-Watch-1.4.0.pkg",
    )

with open(out_path, "w", encoding="utf-8") as f:
    f.write(body)
PY

gh release edit "$TAG" \
  -R "$REPO" \
  --target main \
  --notes-file "$NOTES_TMP" \
  --draft=false \
  --latest
rm -f "$NOTES_TMP"

# Best-effort update of Homebrew tap cask if accessible
if [ -n "$DMG_SHA" ]; then
  python3 - "$VERSION" "$DMG_SHA" <<'PY' || true
import base64, json, subprocess, sys

version, dmg_sha = sys.argv[1:3]
tap_repo = "MaxFreedomPollard/homebrew-tap"
cask_path = "Casks/max-watch.rb"

meta_raw = subprocess.check_output(
    ["gh", "api", f"repos/{tap_repo}/contents/{cask_path}"],
    text=True,
)
file_sha = json.loads(meta_raw)["sha"]

new_cask = f'''cask "max-watch" do
  version "{version}"
  sha256 "{dmg_sha}"

  url "https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v#{{version}}/Max-Watch-#{{version}}.dmg"
  name "Max Watch"
  desc "100% offline encrypted Mac & Windows video recorder with motion detection"
  homepage "https://github.com/MaxFreedomPollard/Max-Watch"

  depends_on macos: ">= :ventura"

  app "Max Watch.app"

  zap trash: [
    "~/Library/Containers/app.c60.maxwatch",
  ]
end
'''
b64 = base64.b64encode(new_cask.encode("utf-8")).decode("ascii")
subprocess.check_call([
    "gh", "api", "-X", "PUT", f"repos/{tap_repo}/contents/{cask_path}",
    "-f", f"message=Update max-watch to {version}",
    "-f", f"sha={file_sha}",
    "-f", f"content={b64}",
], stdout=subprocess.DEVNULL)
print(f"Updated Homebrew tap ({tap_repo}) to v{version}!")
PY
fi

echo ""
echo "================================================================"
echo "SUCCESS! Max Watch ${TAG} (Mac + Windows) is now LIVE:"
echo "https://github.com/${REPO}/releases/tag/${TAG}"
echo "================================================================"
