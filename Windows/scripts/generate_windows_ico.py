#!/usr/bin/env python3
"""
Generates a multi-resolution Windows .ico (16, 24, 32, 48, 64, 128, 256 px PNG frames)
directly from the canonical Max Watch MF app icon assets.
"""
import os
import shutil
import struct
import subprocess
import sys
import tempfile

def main() -> int:
    repo_root = os.path.abspath(os.path.join(os.path.dirname(__file__), "..", ".."))
    src_512 = os.path.join(
        repo_root, "Resources", "Assets.xcassets", "AppIcon.appiconset", "icon_256x256@2x.png"
    )
    if not os.path.isfile(src_512):
        src_512 = os.path.join(repo_root, "docs", "assets", "icon.png")
    if not os.path.isfile(src_512):
        raise FileNotFoundError(f"Could not find source icon PNG at {src_512}")

    out_ico = os.path.join(repo_root, "Windows", "resources", "MaxWatch.ico")
    os.makedirs(os.path.dirname(out_ico), exist_ok=True)

    sizes = [16, 24, 32, 48, 64, 128, 256]
    png_blobs = []

    sips_bin = shutil.which("sips")
    if not sips_bin:
        raise RuntimeError("macOS 'sips' is required to regenerate MaxWatch.ico from PNG")

    with tempfile.TemporaryDirectory() as tmpdir:
        for sz in sizes:
            resized = os.path.join(tmpdir, f"icon_{sz}.png")
            subprocess.run(
                [sips_bin, "-z", str(sz), str(sz), src_512, "--out", resized],
                check=True,
                stdout=subprocess.DEVNULL,
                stderr=subprocess.DEVNULL,
            )
            with open(resized, "rb") as f:
                data = f.read()
            if data[:8] != b"\x89PNG\r\n\x1a\n":
                raise RuntimeError(f"Resized {sz}x{sz} output is not a valid PNG")
            png_blobs.append((sz, data))

    # ICONDIR header: Reserved=0 (2B), Type=1 (2B), Count=len(sizes) (2B)
    header = struct.pack("<HHH", 0, 1, len(png_blobs))
    entries = bytearray()
    payload = bytearray()
    current_offset = 6 + 16 * len(png_blobs)

    for sz, blob in png_blobs:
        w_byte = 0 if sz >= 256 else sz
        h_byte = 0 if sz >= 256 else sz
        # ICONDIRENTRY: bWidth, bHeight, bColorCount, bReserved, wPlanes, wBitCount, dwBytesInRes, dwImageOffset
        entries.extend(
            struct.pack("<BBBBHHII", w_byte, h_byte, 0, 0, 1, 32, len(blob), current_offset)
        )
        payload.extend(blob)
        current_offset += len(blob)

    with open(out_ico, "wb") as out:
        out.write(header)
        out.write(entries)
        out.write(payload)

    print(f"Generated {out_ico} ({os.path.getsize(out_ico)} bytes, {len(png_blobs)} resolutions)")
    return 0

if __name__ == "__main__":
    sys.exit(main())
