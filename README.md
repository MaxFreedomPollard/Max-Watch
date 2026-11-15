# Max Watch | Free Mac & Windows Security Cam. 
## Encrypted. Offline. Works on Screen Lock. Motion Detect. Password Protected. 
## Actually free, no upgrade possible. Works on download, no setup. No internet, ever. 

**A 100% offline, free, encrypted Mac & Windows video recorder with motion detection.**

Max Watch gives every Mac and Windows PC a free motion-activated webcam and USB security camera recorder that keeps recording even while your screen is locked. Every clip is encrypted in memory with `AES-256-GCM` into a password-protected local video vault, with zero cloud servers, zero accounts, zero subscriptions, and zero network access. It's designed to save the clip it's recording even if someone shuts down the computer or closes the program, and nobody can watch the videos without your password, unless you save them to a different part of the computer. Once in "Play All" mode, it plays all the motion detected clips and you either press "delete and next" or "save and next" or just "next" if you're still deciding. No setup required besides putting in the initial password. The only free Mac & Windows security camera app that works when your screen is locked, better than any paid app for offline Mac and Windows security camera recording.

Leaving the office overnight? Set this when you leave, to see if anyone is entering your office and stealing stuff.

Have a child? This is a great nanny cam on the go, if you're traveling with a kid and a laptop.

Are you hot? Check you're not being raped, by leaving this on in your room or wherever you sleep/party.

In a hotel? Leave this on when you're out and have the do not disturb sign on, if nobody comes in it will just be the video of you leaving and coming back. Extra 4 seconds, super simple.

If you have a laptop, you now have a reliable security camera program that you can leave on whenever you want. Can't believe I'm the first person to make this, should have been made 10 years ago before AI even existed, but I'm the only good person on earth clearly.

Note: This does not disable the light that shows when the camera is recording. If you want to do that, black nail polish or something. I decided not to blank the light because what works on one computer might break another computer, and this program is truly compatible with all Macs and Windows PCs. Old, new, ARM, Intel, AMD, Silicone, whatever, if it's a Mac or a Windows PC it should work on your computer. If you have any technical issues, just message me on instagram or linked and I will do a new release, usually same day. If you message on Github it might be lost, because I get 10-20 messages per day on Github. You can turn audio on or off, so it either records just video on motion, or records video and audio on motion

---

## Download for macOS & Windows

### macOS (`macOS 13.0+` Universal: Apple Silicon & Intel)

Download the Apple-signed and notarized universal installer (`macOS 13.0+` Ventura, Sonoma, Sequoia, and newer on M1/M2/M3/M4 & Intel Macs) from [**GitHub Releases**](https://github.com/MaxFreedomPollard/Max-Watch/releases/latest):

- [**`Max-Watch-1.3.0.dmg`**](https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v1.3.0/Max-Watch-1.3.0.dmg): Open the disk image and drag **Max Watch.app** into **Applications**.
- [**`Max-Watch-1.3.0.pkg`**](https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v1.3.0/Max-Watch-1.3.0.pkg): Standard macOS installer package that installs directly to `/Applications/Max Watch.app`.

Or install on macOS via **Homebrew**:

```bash
brew tap maxfreedompollard/tap
brew install --cask max-watch
```

### Windows (`Windows 10 & Windows 11` Native: x64 & ARM64)

Download the Windows installer or zero-install portable `.exe` (`Windows 10` & `Windows 11` on Intel, AMD, and Snapdragon / Surface ARM64 PCs) from [**GitHub Releases**](https://github.com/MaxFreedomPollard/Max-Watch/releases/latest):

- [**`Max-Watch-1.3.0-Windows-Setup.exe`**](https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v1.3.0/Max-Watch-1.3.0-Windows-Setup.exe): Universal `x64 + ARM64` Windows installer (installs Start Menu & optional Desktop shortcut, no admin rights required).
- [**`Max-Watch-1.3.0-Windows-Portable-x64.exe`**](https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v1.3.0/Max-Watch-1.3.0-Windows-Portable-x64.exe): Standalone single-file portable app for Intel & AMD Windows PCs (runs immediately on download with zero installation).
- [**`Max-Watch-1.3.0-Windows-Portable-ARM64.exe`**](https://github.com/MaxFreedomPollard/Max-Watch/releases/download/v1.3.0/Max-Watch-1.3.0-Windows-Portable-ARM64.exe): Standalone single-file portable app for ARM64 / Snapdragon Windows laptops.

![Max Watch Live Motion Detection & Locked AES-256 Vault](./docs/assets/01_live_motion_studio.png)

---

## Features

### 1. Motion-Activated Video & Audio Recording (Adjustable Sensitivity Scale + Audio ON/OFF Switch)
- Smooth motion detection sensitivity slider from **`10%`** (large movement only) up to **`100%`** (subtle motion), paired with a live motion energy meter, visual trigger threshold marker, and real-time `48 kHz` AAC microphone `dB` meter.
- **One-Click Audio ON / OFF Switch**: Toggle microphone recording ON or OFF anytime (`Cmd+Shift+A` on Mac / `Ctrl+Shift+A` on Windows, or the studio deck switch) to record full `48 kHz` AAC sound or silent video-only clips, plus a quick playback mute toggle inside the Vault Player.
- **Unambiguous International Bottom-Left Timestamp**: Every recorded clip embeds a small, clear local computer-time timestamp in the bottom-left corner with the month spelled out and a 4-digit year (e.g., `14 Nov 2026 • 9:35:34 PM PST` or `21:35:34 CET`), avoiding ambiguous numeric `11/12/26` dates across countries.
- Works with built-in laptop / desktop webcams, Continuity Camera, and external USB webcams or capture devices (`1080p` hardware H.264 encoding via Apple VideoToolbox on Mac and Windows Media Foundation on Windows).
- Each motion event records for at least 60 seconds and extends automatically without file-size or duration limits for as long as motion continues.

### 2. Records While Your Mac or Windows Screen Is Locked
- Click **Start Motion Detection** and lock your screen (`Ctrl+Cmd+Q` on Mac or `Win+L` on Windows).
- Max Watch prevents idle system sleep (including Windows Modern Standby) while armed and continues monitoring your camera and encrypting motion clips in the background while your desktop stays locked.

### 3. Password-Protected AES-256 Video Vault (Stays Locked While Saving)
- Video and audio are encrypted in RAM before a single byte touches your drive using hybrid **`Curve25519`** key agreement + **`PBKDF2-HMAC-SHA256`** (`310,000` rounds) + **`AES-256-GCM`**.
- Because recording uses your vault public key, **the vault remains strictly locked while Max Watch arms, monitors, and saves clips** (and re-locks automatically when switching to Live Camera, closing/minimizing the window, locking your screen, or leaving a paused player unattended). Nobody entering a room can press **Play All** or delete footage without your password.

### 4. Power-Cut-Safe & Exit-Safe Recording
- **Survives Sudden Power Loss**: Footage is muxed in memory into 1-second fragmented MP4 (`fMP4`) segments, encrypted with `AES-256-GCM`, and flushed (`fsync` / `FlushFileBuffers`) to disk every second. Even if power is cut or a battery dies mid-recording, your latest clip is preserved and playable right up to the last completed second.
- **Saves Active Clip on Exit**: Quitting the program (`Cmd+Q` / `Ctrl+Q`, menu bar / system tray quit, `SIGTERM`, or OS logout/restart/shutdown) while recording flushes and encrypts the final sub-second frames cleanly before exiting.

### 5. Password-Locked `Play All`, Fast-Drag Timeline Scrubber Ball & `1x` / `4x` / `12x` Speeds
- Unlocking **Play All** streams decrypted segments directly from RAM (`AVAssetResourceLoader` on Mac / custom `IMFByteStream` on Windows) with **zero unencrypted temporary files ever written to disk**.
- **Fast-Drag Scrubber Ball**: Grab and slide the timeline scrubber ball across any recording to scrub through video frames instantaneously as you drag it.
- **Multi-Speed Shuttle**: Watch clips sequentially hands-free at **Normal (`1x`)**, **`4x`**, or **`12x`** speed.

### 6. One-Click `Save & Next` and `Delete & Next` Clip Triage
- **`Save & Next` (`Cmd+S` / `Ctrl+S`)**: Choose any folder on your computer outside the vault (such as `Desktop`, `Movies`/`Videos`, `Downloads`, or an external drive) to export a standard playable `.mp4` copy of the current clip and immediately jump to the next clip in a single click (`Cmd+Shift+S` / `Ctrl+Shift+S` changes your target folder anytime).
- **`Delete & Next` (`Cmd+Delete` / `Ctrl+Delete`)**: Securely zero-wipe unwanted clips from the encrypted vault and immediately advance to play the next recording.

### 7. 100% Offline Enforcement
- **macOS**: Compiled and signed with `com.apple.security.app-sandbox = true` and **without** `com.apple.security.network.client` or `com.apple.security.network.server`, blocking all network sockets at the kernel level.
- **Windows**: Compiled natively with zero network socket or HTTP DLL imports (`ws2_32.dll`, `wininet.dll`, `winhttp.dll`, `urlmon.dll`, `dnsapi.dll` completely absent from the executable import table, verified automatically on every build). Nothing can ever be uploaded or transmitted anywhere.

---

## Build From Source

### macOS (`macOS 13.0+` with Xcode Command Line Tools)

```bash
git clone https://github.com/MaxFreedomPollard/Max-Watch.git
cd Max-Watch
./scripts/build_universal.sh
```

`./scripts/build_universal.sh` generates the icon asset catalog, compiles native `arm64` and `x86_64` binaries, merges them into a universal `Max Watch.app`, and runs the automated cryptographic, locked-while-saving, power-cut recovery, external `.mp4` export, and `1x` / `4x` / `12x` playback self-test suite.

### Windows (`Windows 10 / 11` with Visual Studio 2022 C++ & CMake)

```powershell
git clone https://github.com/MaxFreedomPollard/Max-Watch.git
cd Max-Watch
./Windows/build_windows.ps1
```

`./Windows/build_windows.ps1` compiles standalone static (`x64` and `ARM64`) native Win32/Direct2D/Media Foundation/BCrypt executables, runs the 14-stage automated verification suite (including cross-OS vault compatibility and PE import zero-network audit), runs the Direct2D UI smoke test, and builds the universal `Setup.exe` installer.

---

## Security Architecture

1. **Zero Network Access**: macOS App Sandbox omits all network capabilities, and the Windows native binary links zero networking DLLs; only local camera, microphone, and user-selected file export are permitted.
2. **Asymmetric Capture / Symmetric Chunk Encryption**: `vault_manifest.json` stores your `Curve25519` public key alongside your `AES-256-GCM`-wrapped private key (derived via `310,000` rounds of `PBKDF2-HMAC-SHA256`). Each recorded clip generates a fresh ephemeral `Curve25519` keypair + 256-bit `HKDF-SHA256` clip key so new clips encrypt while the private key is completely absent from memory.
3. **Authenticated Chunk Framing (`MWV1`)**: Every 1-second `fMP4` segment uses a unique 96-bit nonce and 128-bit GCM authentication tag binding the clip UUID, 64-bit chunk index, and segment type in Additional Authenticated Data (`AAD`), preventing chunk truncation, injection, or reordering across both macOS and Windows.

---

## License

Released under the [MIT License](./LICENSE).
