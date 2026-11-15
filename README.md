# Max Watch

**A free, 100% secure, 100% offline motion-detection security camera and encrypted video vault for Mac - gifted to the community by Max Freedom Pollard.**

Max Watch turns any Apple Silicon or Intel Mac (`macOS 13.0+`) into a private, local-only motion security recorder that keeps working even while your Mac screen is locked. No cloud accounts, no subscriptions, no tracking, and zero network access enforced by the macOS App Sandbox.

---

## Why Max Watch Exists

Sometimes you just want peace of mind knowing your room, studio, office, or gear is watched while you step away and lock your screen - without sending camera feeds to someone else's cloud server or paying a monthly subscription. Max Watch is completely free and open source for anyone who needs it.

---

## Key Features

- **Motion Detection Sliding Scale**: Smooth continuous sensitivity slider (`10%` heavy motion only up to `100%` ultra-sensitive) paired with a real-time motion level meter, visual trigger line, and live `48 kHz` AAC microphone `dB` meter. Every motion trigger records for at least 60 seconds and extends automatically without size or duration caps as long as motion continues.
- **Works While Your Mac Screen Is Locked**: Arm Max Watch and lock your screen (`Ctrl+Cmd+Q`). Max Watch prevents idle system sleep while armed, auto-locks your video vault immediately when the screen locks, and continues capturing and encrypting new motion clips in the background.
- **Password-Locked Secure Vault & Password-Locked Play All**: Every clip is encrypted in RAM before touching disk using `Curve25519` hybrid key agreement + `PBKDF2-HMAC-SHA256` (`310,000` iterations) + `AES-256-GCM`. Pressing **Play All** or opening the vault requires your password, and playback streams decrypted chunks straight from RAM via `AVAssetResourceLoader` with zero plaintext temp files ever written to disk.
- **Saves Current Recording on Exit**: Quitting the app (`Cmd+Q`, menu quit, `SIGTERM`, or macOS logout/restart) while recording automatically flushes and encrypts the final sub-second frames before exiting.
- **Saves Latest Recording on Sudden Power Loss**: Recordings are muxed in memory into 1-second fragmented MP4 (`fMP4`) chunks, encrypted with `AES-256-GCM`, and `fsync`ed to disk every single second. Even if power is cut mid-recording, your clip is preserved and playable right up to the last completed second.
- **Fast-Drag Scrubber "Ball" + `1x` / `4x` / `12x` Speeds**: Slide the glowing second ball along the timeline to scrub frames instantaneously in real time as you drag it, or review clips hands-free at **Normal (`1x`)**, **`4x`**, or **`12x`** speed, with instant **Next & Delete** zero-wipe cleanup.
- **Universal Mac Support & Bold `MF` Dock/Menu Bar Icon**: Native universal Mach-O binary (`arm64` + `x86_64`) signed and notarized with Apple Developer ID, featuring a crisp `MF` icon in your Dock and macOS menu bar.

---

## Easy Download & Install

Grab the Apple-signed & notarized universal installer from [**GitHub Releases**](https://github.com/MaxFreedomPollard/Max-Watch/releases/latest):

- **`Max-Watch-1.0.0.dmg`**: Open and drag **Max Watch.app** into **Applications**.
- **`Max-Watch-1.0.0.pkg`**: Standard signed macOS installer package that installs directly into `/Applications/Max Watch.app`.

---

## Build From Source

Requires macOS 13.0+ and Xcode Command Line Tools (`swiftc`, `actool`, `iconutil`, `lipo`):

```bash
git clone https://github.com/MaxFreedomPollard/Max-Watch.git
cd Max-Watch
./scripts/build_universal.sh
```

`./scripts/build_universal.sh` generates the `MF` asset catalog, compiles both `arm64` and `x86_64` slices, merges them into a universal `Max Watch.app`, and runs the 7-stage automated cryptographic, power-cut recovery, and `1x`/`4x`/`12x` playback self-test.

---

## Security Architecture

1. **Zero Network Entitlements**: Compiled and signed with `com.apple.security.app-sandbox = true` and **without** `com.apple.security.network.client` or `com.apple.security.network.server`. The macOS kernel denies all socket creation.
2. **Hybrid Public-Key Capture**: `vault_manifest.json` stores your `Curve25519` public key alongside your `AES-256-GCM`-wrapped private key (derived via `310,000` rounds of `PBKDF2-HMAC-SHA256`). New motion clips derive a unique ephemeral `ECDH` + `HKDF-SHA256` 256-bit symmetric key so Max Watch can record and encrypt clips while the vault is locked and your private key is wiped from RAM.
3. **Authenticated Chunk Framing (`MWV1`)**: Each 1-second fMP4 fragment carries a fresh 96-bit nonce and 128-bit GCM authentication tag binding the clip UUID, 64-bit chunk index, and segment type in Additional Authenticated Data (`AAD`), preventing chunk reordering or tampering.

---

## License

Released under the [MIT License](./LICENSE) as a free gift to the community by **Max Freedom Pollard**. Stay safe and take care of each other.
