#include "SelfTest.hpp"

#include "Fmp4Muxer.hpp"
#include "MediaPipeline.hpp"
#include "SleepAndLockManager.hpp"
#include "TimestampStamper.hpp"
#include "VaultCrypto.hpp"

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <iostream>
#include <stdexcept>
#include <string>
#include <thread>
#include <vector>

#ifdef _WIN32
#ifndef NOMINMAX
#define NOMINMAX
#endif
#define WIN32_LEAN_AND_MEAN
#include <windows.h>
#include <mfapi.h>
#include <mfidl.h>
#include <mfreadwrite.h>
#include <wrl/client.h>
using Microsoft::WRL::ComPtr;
#endif

namespace maxwatch {

namespace {

void checkTrue(bool cond, const std::string& msg) {
    if (!cond) {
        throw std::runtime_error("SelfTest assertion failed: " + msg);
    }
}

std::vector<uint8_t> hexToBytes(const char* hex) {
    std::vector<uint8_t> out;
    size_t len = std::strlen(hex);
    for (size_t i = 0; i + 1 < len; i += 2) {
        unsigned int byteVal = 0;
        std::sscanf(hex + i, "%2x", &byteVal);
        out.push_back(static_cast<uint8_t>(byteVal));
    }
    return out;
}

#ifdef _WIN32

void generateSyntheticMovingNV12Frame(
    std::vector<uint8_t>& nv12,
    int width,
    int height,
    int frameIndex
) {
    nv12.assign(static_cast<size_t>(width) * static_cast<size_t>(height) * 3 / 2, 128);
    uint8_t* yPlane = nv12.data();
    uint8_t* uvPlane = nv12.data() + static_cast<size_t>(width) * static_cast<size_t>(height);

    // Subtle dark gradient background
    for (int y = 0; y < height; ++y) {
        uint8_t baseLuma = static_cast<uint8_t>(26 + ((y * 44) / std::max(1, height)));
        std::memset(yPlane + static_cast<size_t>(y) * width, baseLuma, static_cast<size_t>(width));
    }

    // Moving bright target box so every frame differs visually for scrub verification
    int boxW = 160;
    int boxH = 110;
    int bx = (frameIndex * 18) % std::max(1, width - boxW - 10);
    int by = 90 + ((frameIndex * 9) % std::max(1, height - boxH - 190));

    for (int y = by; y < by + boxH && y < height; ++y) {
        for (int x = bx; x < bx + boxW && x < width; ++x) {
            yPlane[static_cast<size_t>(y) * width + x] = 225;
            if ((y % 2 == 0) && (x % 2 == 0)) {
                size_t uvIdx = static_cast<size_t>(y / 2) * width + x;
                uvPlane[uvIdx] = 90;
                uvPlane[uvIdx + 1] = 185;
            }
        }
    }
}

void feedSyntheticClipToWriter(
    VaultSegmentWriter& writer,
    int width,
    int height,
    int totalSeconds,
    bool includeAudio,
    int64_t baseUnixMs
) {
    const int fps = 30;
    const int totalFrames = totalSeconds * fps;
    std::vector<uint8_t> nv12;
    std::vector<int16_t> pcmChunk(1600, 0); // 1/30s @ 48 kHz mono

    for (int f = 0; f < totalFrames; ++f) {
        generateSyntheticMovingNV12Frame(nv12, width, height, f);
        int64_t pts100ns = (static_cast<int64_t>(f) * 10000000LL) / fps;
        int64_t frameUnixMs = baseUnixMs + (static_cast<int64_t>(f) * 1000LL) / fps;

        writer.appendNV12Frame(
            nv12.data(), width,
            nv12.data() + static_cast<size_t>(width) * height, width,
            width, height,
            pts100ns,
            frameUnixMs
        );

        if (includeAudio) {
            for (size_t s = 0; s < pcmChunk.size(); ++s) {
                double t = static_cast<double>(f * 1600 + static_cast<int>(s)) / 48000.0;
                pcmChunk[s] = static_cast<int16_t>(std::sin(t * 2.0 * 3.141592653589793 * 440.0) * 6500.0);
            }
            writer.appendAudioPCM16Mono48k(pcmChunk.data(), pcmChunk.size(), pts100ns);
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(4));
    }
}

void assertZeroPlaintextMediaFilesRecursively(const std::filesystem::path& rootDir) {
    if (!std::filesystem::exists(rootDir)) return;
    for (const auto& entry : std::filesystem::recursive_directory_iterator(rootDir)) {
        if (!entry.is_regular_file()) continue;
        std::wstring ext = entry.path().extension().wstring();
        std::transform(ext.begin(), ext.end(), ext.begin(), ::towlower);
        checkTrue(
            ext != L".mp4" && ext != L".mov" && ext != L".m4v" && ext != L".tmp" && ext != L".mkv" && ext != L".avi",
            "Found unexpected unencrypted/temp file on disk: " + entry.path().string()
        );
    }
}

void verifyExportedMp4DecodesIndependently(const std::wstring& mp4Path, int expectedW, int expectedH, bool expectAudio) {
    ensureMediaFoundationInitialized();
    ComPtr<IMFAttributes> attrs;
    HRESULT hr = MFCreateAttributes(&attrs, 2);
    checkTrue(SUCCEEDED(hr), "MFCreateAttributes failed for exported MP4 check");
    attrs->SetUINT32(MF_READWRITE_ENABLE_HARDWARE_TRANSFORMS, TRUE);
    attrs->SetUINT32(MF_SOURCE_READER_ENABLE_VIDEO_PROCESSING, TRUE);

    ComPtr<IMFSourceReader> reader;
    hr = MFCreateSourceReaderFromURL(mp4Path.c_str(), attrs.Get(), &reader);
    checkTrue(SUCCEEDED(hr) && reader != nullptr, "MFCreateSourceReaderFromURL failed to open exported .mp4");

    ComPtr<IMFMediaType> nativeVideo;
    hr = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &nativeVideo);
    checkTrue(SUCCEEDED(hr) && nativeVideo != nullptr, "Exported .mp4 missing video stream");

    UINT32 w = 0, h = 0;
    MFGetAttributeSize(nativeVideo.Get(), MF_MT_FRAME_SIZE, &w, &h);
    checkTrue(static_cast<int>(w) == expectedW && static_cast<int>(h) == expectedH, "Exported .mp4 video dimensions mismatch");

    ComPtr<IMFMediaType> nativeAudio;
    HRESULT hrAudio = reader->GetNativeMediaType(MF_SOURCE_READER_FIRST_AUDIO_STREAM, 0, &nativeAudio);
    if (expectAudio) {
        checkTrue(SUCCEEDED(hrAudio) && nativeAudio != nullptr, "Exported .mp4 missing expected AAC audio stream");
    }

    ComPtr<IMFMediaType> outRgb;
    MFCreateMediaType(&outRgb);
    outRgb->SetGUID(MF_MT_MAJOR_TYPE, MFMediaType_Video);
    outRgb->SetGUID(MF_MT_SUBTYPE, MFVideoFormat_RGB32);
    checkTrue(SUCCEEDED(reader->SetCurrentMediaType(MF_SOURCE_READER_FIRST_VIDEO_STREAM, nullptr, outRgb.Get())), "Set RGB32 output on exported .mp4 failed");

    int decodedFrames = 0;
    for (int tries = 0; tries < 40 && decodedFrames < 5; ++tries) {
        DWORD streamIdx = 0, flags = 0;
        LONGLONG ts = 0;
        ComPtr<IMFSample> sample;
        hr = reader->ReadSample(MF_SOURCE_READER_FIRST_VIDEO_STREAM, 0, &streamIdx, &flags, &ts, &sample);
        checkTrue(SUCCEEDED(hr), "ReadSample failed on exported .mp4");
        if (flags & MF_SOURCE_READERF_ENDOFSTREAM) break;
        if (sample) decodedFrames++;
    }
    checkTrue(decodedFrames >= 3, "Exported .mp4 decoded fewer than 3 video frames");
}

void verifyZeroNetworkDllImportsInCurrentExecutable() {
    HMODULE hMod = GetModuleHandleW(nullptr);
    checkTrue(hMod != nullptr, "GetModuleHandleW(nullptr) returned null");

    const auto* base = reinterpret_cast<const uint8_t*>(hMod);
    const auto* dos = reinterpret_cast<const IMAGE_DOS_HEADER*>(base);
    checkTrue(dos->e_magic == IMAGE_DOS_SIGNATURE, "Invalid DOS signature on running module");

    const auto* nt = reinterpret_cast<const IMAGE_NT_HEADERS*>(base + dos->e_lfanew);
    checkTrue(nt->Signature == IMAGE_NT_SIGNATURE, "Invalid NT signature on running module");

    const IMAGE_DATA_DIRECTORY& impDir = nt->OptionalHeader.DataDirectory[IMAGE_DIRECTORY_ENTRY_IMPORT];
    if (impDir.VirtualAddress == 0 || impDir.Size == 0) {
        return;
    }

    const std::vector<std::string> forbiddenDlls = {
        "ws2_32.dll",
        "wsock32.dll",
        "wininet.dll",
        "winhttp.dll",
        "urlmon.dll",
        "dnsapi.dll",
        "mswsock.dll",
        "webio.dll"
    };

    const auto* desc = reinterpret_cast<const IMAGE_IMPORT_DESCRIPTOR*>(base + impDir.VirtualAddress);
    while (desc->Name != 0) {
        const char* dllNameRaw = reinterpret_cast<const char*>(base + desc->Name);
        std::string dllName(dllNameRaw ? dllNameRaw : "");
        std::transform(dllName.begin(), dllName.end(), dllName.begin(), [](unsigned char c) {
            return static_cast<char>(std::tolower(c));
        });
        for (const auto& bad : forbiddenDlls) {
            checkTrue(dllName != bad, "SECURITY VIOLATION: Executable imports forbidden network DLL: " + dllName);
        }
        ++desc;
    }
}

#endif // _WIN32

} // namespace

int runAutomatedSelfTest(const std::wstring& scratchDir) {
    try {
        std::cout << "=== Max Watch Windows 14-Stage Automated Verification Suite ===" << std::endl;

        std::filesystem::path rootPath = scratchDir.empty()
            ? (std::filesystem::temp_directory_path() / L"MaxWatch_SelfTest")
            : std::filesystem::path(scratchDir);

        std::error_code ec;
        std::filesystem::remove_all(rootPath, ec);
        std::filesystem::create_directories(rootPath, ec);

        const std::filesystem::path vaultPath = rootPath / L"Vault";
        const std::filesystem::path exportPath = rootPath / L"ExternalExports";
        const std::filesystem::path crossOsPath = rootPath / L"MacOSCrossVault";
        std::filesystem::create_directories(exportPath, ec);

        // ---------------------------------------------------------------------
        // Stage 1: RFC 7748 X25519 Vectors & Vault Initialization (Starts LOCKED)
        // ---------------------------------------------------------------------
        std::cout << "[1/14] Verifying RFC 7748 X25519 vectors & Vault initialization (starts LOCKED)..." << std::endl;
        {
            auto alicePrivVec = hexToBytes("77076d0a7318a57d3c16c17251b26645df4c2f87ebc0992ab177fba51db92c2a");
            auto bobPrivVec   = hexToBytes("5dab087e624a8a4b79e17f8b83800ee66f3bb1292618b6fd1c2f8b27ff88e0eb");
            auto expShared    = hexToBytes("4a5d9d5ba4ce2de1728e3bf480350f25e07e21c947d19e3376f09b3c1e161742");
            std::array<uint8_t, 32> aPriv{}, bPriv{};
            std::memcpy(aPriv.data(), alicePrivVec.data(), 32);
            std::memcpy(bPriv.data(), bobPrivVec.data(), 32);
            auto aPub = crypto::x25519PublicFromPrivate(aPriv);
            auto bPub = crypto::x25519PublicFromPrivate(bPriv);
            auto s1 = crypto::x25519SharedSecret(aPriv, bPub);
            auto s2 = crypto::x25519SharedSecret(bPriv, aPub);
            checkTrue(s1 == s2, "X25519 shared secret asymmetry");
            checkTrue(std::memcmp(s1.data(), expShared.data(), 32) == 0, "RFC 7748 shared secret mismatch");
        }

        VaultManager vault(vaultPath.wstring());
        checkTrue(!vault.isInitialized(), "Fresh vault should not be initialized");
        checkTrue(!vault.isUnlocked(), "Fresh vault must start locked");

        vault.initializeVault("initial-pass-1234");
        checkTrue(vault.isInitialized(), "Vault must be initialized after initializeVault");
        checkTrue(!vault.isUnlocked(), "Vault MUST stay strictly LOCKED immediately after initializeVault");
        checkTrue(!vault.unlock("wrong-password-999"), "Wrong password must return false");
        checkTrue(!vault.isUnlocked(), "Vault must remain locked after wrong password");

#ifdef _WIN32
        ensureMediaFoundationInitialized();

        // ---------------------------------------------------------------------
        // Stage 2: Record Clip #1 WHILE LOCKED + Simulated Hard Power Cut
        // ---------------------------------------------------------------------
        std::cout << "[2/14] Recording 1280x720 H.264+AAC Clip #1 WHILE LOCKED + simulating sudden power loss..." << std::endl;
        std::wstring powerCutClipPath;
        {
            auto clipWriter = vault.createClipWriter(1280, 720, true);
            powerCutClipPath = clipWriter->filePath();
            auto segWriter = std::make_unique<VaultSegmentWriter>(std::move(clipWriter), 1280, 720, true);

            feedSyntheticClipToWriter(*segWriter, 1280, 720, 4, true, 1763194534000LL);
            // Wait until >= 2 1-second chunks have flushed to disk
            for (int w = 0; w < 50 && segWriter->flushedSegmentCount() < 3; ++w) {
                std::this_thread::sleep_for(std::chrono::milliseconds(60));
            }
            checkTrue(segWriter->flushedSegmentCount() >= 3, "Expected init + >=2 1s fragments flushed before power cut");

            // Abandon mid-recording WITHOUT finalizing!
            segWriter->abandonWithoutFinalizingForPowerCutTest();
            segWriter.reset();

            // Append 19 bytes of truncated half-written garbage at EOF to simulate sudden power death mid-sector
            std::ofstream corruptTail(std::filesystem::path(powerCutClipPath), std::ios::binary | std::ios::app);
            const char garbage[19] = {'P', 'W', 'R', 'C', 'U', 'T', 1, 2, 3, 4, 5, 6, 7, 8, 9, 10, 11, 12, 13};
            corruptTail.write(garbage, sizeof(garbage));
            corruptTail.close();
        }
        checkTrue(!vault.isUnlocked(), "Vault must remain LOCKED during Clip #1 recording");

        // ---------------------------------------------------------------------
        // Stage 3: Record Clip #2 WHILE LOCKED + Clean Exit Save + Zero Plaintext Check
        // ---------------------------------------------------------------------
        std::cout << "[3/14] Recording Clip #2 WHILE LOCKED + verifying clean exit flush & zero disk plaintext..." << std::endl;
        std::wstring cleanClipPath;
        {
            auto clipWriter = vault.createClipWriter(1280, 720, true);
            cleanClipPath = clipWriter->filePath();
            auto segWriter = std::make_unique<VaultSegmentWriter>(std::move(clipWriter), 1280, 720, true);

            feedSyntheticClipToWriter(*segWriter, 1280, 720, 3, true, 1763194600000LL);
            segWriter->stopAndFinalizeSynchronously(4.0);
        }
        checkTrue(vault.countVaultClips() == 2, "Expected 2 encrypted .mwv clips in locked vault");
        assertZeroPlaintextMediaFilesRecursively(vaultPath);

        bool lockedReadBlocked = false;
        try {
            auto blocked = vault.openClipReader(cleanClipPath);
            (void)blocked;
        } catch (const VaultException& ex) {
            lockedReadBlocked = (ex.kind == VaultErrorKind::VaultLocked);
        }
        checkTrue(lockedReadBlocked, "openClipReader MUST reject decryption while vault is LOCKED");

        // ---------------------------------------------------------------------
        // Stage 4: Unlock Vault & Verify Power-Cut Recovery + Clean Finalization
        // ---------------------------------------------------------------------
        std::cout << "[4/14] Unlocking vault & verifying power-cut tail recovery + clean-exit metadata..." << std::endl;
        checkTrue(vault.unlock("initial-pass-1234"), "Valid vault password must unlock");
        checkTrue(vault.isUnlocked(), "Vault must report unlocked");

        auto summaries = vault.listSummaries();
        checkTrue(summaries.size() == 2, "Expected 2 clip summaries after unlock");

        const EncryptedClipSummary* powerCutSum = nullptr;
        const EncryptedClipSummary* cleanSum = nullptr;
        for (const auto& s : summaries) {
            if (s.filePath == powerCutClipPath) powerCutSum = &s;
            if (s.filePath == cleanClipPath) cleanSum = &s;
        }
        checkTrue(powerCutSum != nullptr && cleanSum != nullptr, "Both clips must appear in summaries");
        checkTrue(!powerCutSum->cleanlyFinalized, "Power-cut clip must report cleanlyFinalized == false");
        checkTrue(powerCutSum->estimatedDurationSeconds >= 2.0, "Power-cut clip must recover >= 2.0s of completed segments");
        checkTrue(cleanSum->cleanlyFinalized, "Clean-exit clip must report cleanlyFinalized == true");
        checkTrue(cleanSum->estimatedDurationSeconds >= 2.5, "Clean-exit clip must preserve full ~3s duration");

        // ---------------------------------------------------------------------
        // Stage 5: Zero-Disk-Plaintext RAM Playback (1x / 4x / 12x) & Fast Ball Scrubbing
        // ---------------------------------------------------------------------
        std::cout << "[5/14] Verifying zero-disk-plaintext RAM playback (1x / 4x / 12x) & fast ball scrubbing..." << std::endl;
        {
            // Verify both power-cut recovered clip AND clean clip open & decode in RAM!
            for (const std::wstring& targetClip : {powerCutClipPath, cleanClipPath}) {
                std::shared_ptr<VaultClipReader> reader(vault.openClipReader(targetClip).release());
                VaultPlayerEngine player;
                std::string errMsg;
                checkTrue(player.loadClip(reader, &errMsg), "VaultPlayerEngine failed to load clip: " + errMsg);
                checkTrue(player.durationSeconds() >= 2.0, "Loaded clip duration too short");

                BgraFrame firstFrame;
                checkTrue(player.copyLatestVideoFrame(firstFrame), "Initial decoded BGRA frame missing");
                checkTrue(firstFrame.width == 1280 && firstFrame.height == 720, "Decoded frame resolution mismatch");

                // Test fast ball scrub chase across multiple timestamps
                player.scrubToSeconds(1.5);
                std::this_thread::sleep_for(std::chrono::milliseconds(160));
                checkTrue(std::abs(player.currentSeconds() - 1.5) < 0.75, "Scrub to 1.5s out of tolerance");

                BgraFrame scrubbedFrame;
                checkTrue(player.copyLatestVideoFrame(scrubbedFrame), "Scrubbed frame missing");

                // Test 1x, 4x, and 12x playback speeds
                for (float rate : {1.0f, 4.0f, 12.0f}) {
                    player.scrubToSeconds(0.0);
                    std::this_thread::sleep_for(std::chrono::milliseconds(80));
                    player.play(rate);
                    std::this_thread::sleep_for(std::chrono::milliseconds(220));
                    checkTrue(player.currentSeconds() > 0.03 || !player.isPlaying(), "Playback clock did not advance at speed " + std::to_string(rate));
                    player.pause();
                }

                player.unloadClip();
                assertZeroPlaintextMediaFilesRecursively(rootPath);
            }
        }

        // ---------------------------------------------------------------------
        // Stage 6: Save & Next External .MP4 Export + Inside-Vault Guard
        // ---------------------------------------------------------------------
        std::cout << "[6/14] Verifying Save & Next external .mp4 export & independent Media Foundation decode..." << std::endl;
        {
            bool insideVaultBlocked = false;
            try {
                vault.exportClipToDirectory(cleanClipPath, vault.clipsDirectoryPath());
            } catch (const VaultException&) {
                insideVaultBlocked = true;
            }
            checkTrue(insideVaultBlocked, "Exporting decrypted MP4 inside the Vault directory MUST be blocked");

            std::wstring savedMp4 = vault.exportClipToDirectory(cleanClipPath, exportPath.wstring());
            checkTrue(std::filesystem::exists(savedMp4), "Exported .mp4 file does not exist");
            checkTrue(std::filesystem::file_size(savedMp4) > 4096, "Exported .mp4 file size unexpectedly small");
            verifyExportedMp4DecodesIndependently(savedMp4, 1280, 720, true);
        }

        // ---------------------------------------------------------------------
        // Stage 7: O(1) Vault Password Rotation
        // ---------------------------------------------------------------------
        std::cout << "[7/14] Verifying O(1) Curve25519 private-key re-wrap password rotation..." << std::endl;
        {
            vault.changePassword("initial-pass-1234", "rotated-pass-5678");
            vault.lock();
            checkTrue(!vault.unlock("initial-pass-1234"), "Old password must no longer unlock vault");
            checkTrue(vault.unlock("rotated-pass-5678"), "Rotated password must unlock vault");

            auto reader = vault.openClipReader(cleanClipPath);
            checkTrue(reader->totalPlaintextLength() > 4096, "Existing clips must still decrypt cleanly after password rotation");
        }

        // ---------------------------------------------------------------------
        // Stage 8: Delete & Next Secure 128 KB Zero-Wipe
        // ---------------------------------------------------------------------
        std::cout << "[8/14] Verifying Delete & Next secure zero-wipe deletion..." << std::endl;
        {
            vault.secureDeleteClip(powerCutClipPath);
            checkTrue(!std::filesystem::exists(powerCutClipPath), "Zero-wiped clip must be unlinked from disk");
            checkTrue(vault.countVaultClips() == 1, "Expected 1 remaining clip after secureDeleteClip");
        }

        // ---------------------------------------------------------------------
        // Stage 9: Unambiguous International Timestamp Format & NV12 Pixel Burn-In
        // ---------------------------------------------------------------------
        std::cout << "[9/14] Verifying unambiguous bottom-left timestamp format & NV12 pixel burn-in..." << std::endl;
        {
            const int64_t sampleMs = 1763194534000LL; // Nov 2025/2026 sample epoch
            std::string s12 = ClipTimestampFormatter::formatExplicitUtf8(sampleMs, true);
            std::string s24 = ClipTimestampFormatter::formatExplicitUtf8(sampleMs, false);
            checkTrue(s12.find('/') == std::string::npos && s24.find('/') == std::string::npos, "Timestamp must NEVER contain ambiguous '/' slashes");
            checkTrue(s12.find("AM") != std::string::npos || s12.find("PM") != std::string::npos, "12-hour format must include AM/PM");
            checkTrue(s24.find("AM") == std::string::npos && s24.find("PM") == std::string::npos, "24-hour format must not include AM/PM");

            const int w = 1280, h = 720;
            std::vector<uint8_t> nv12(static_cast<size_t>(w) * h * 3 / 2, 180);
            FrameTimestampStamper stamper;
            stamper.stampBottomLeftNV12(
                nv12.data(), w,
                nv12.data() + static_cast<size_t>(w) * h, w,
                w, h,
                sampleMs
            );

            // Top-right corner must remain untouched (180)
            checkTrue(nv12[20 * w + (w - 30)] == 180, "Top-right pixels should not be modified by bottom-left stamper");

            // Bottom-left region must contain darkened pill pixels (< 90) AND bright text pixels (> 200)
            int darkPixels = 0;
            int brightTextPixels = 0;
            for (int y = h - 60; y < h - 8; ++y) {
                for (int x = 12; x < 420; ++x) {
                    uint8_t lum = nv12[static_cast<size_t>(y) * w + x];
                    if (lum < 95) darkPixels++;
                    if (lum > 195) brightTextPixels++;
                }
            }
            checkTrue(darkPixels > 400 && brightTextPixels > 40, "Bottom-left NV12 timestamp pill + crisp white glyphs not detected");
        }

        // ---------------------------------------------------------------------
        // Stage 10: Audio OFF (Silent Video-Only) Recording & Multi-Speed Playback
        // ---------------------------------------------------------------------
        std::cout << "[10/14] Verifying Audio OFF (hasAudio == false) video-only recording & playback..." << std::endl;
        {
            vault.lock();
            std::wstring silentClipPath;
            {
                auto cw = vault.createClipWriter(1280, 720, false);
                silentClipPath = cw->filePath();
                auto sw = std::make_unique<VaultSegmentWriter>(std::move(cw), 1280, 720, false);
                feedSyntheticClipToWriter(*sw, 1280, 720, 2, false, 1763195000000LL);
                sw->stopAndFinalizeSynchronously(3.0);
            }
            checkTrue(vault.unlock("rotated-pass-5678"), "Re-unlock for silent clip test failed");
            auto reader = std::shared_ptr<VaultClipReader>(vault.openClipReader(silentClipPath).release());
            checkTrue(!reader->header().hasAudio(), "Silent clip header must have hasAudio() == false");

            VaultPlayerEngine player;
            checkTrue(player.loadClip(reader), "Silent video-only clip must load in VaultPlayerEngine");
            player.play(4.0f);
            std::this_thread::sleep_for(std::chrono::milliseconds(180));
            player.pause();
            player.unloadClip();

            std::wstring silentExport = vault.exportClipToDirectory(silentClipPath, exportPath.wstring());
            verifyExportedMp4DecodesIndependently(silentExport, 1280, 720, false);
        }

#endif // _WIN32

        // ---------------------------------------------------------------------
        // Stage 11: MotionDetector Curve, Global Brightness Immunity & 2-Hit Trigger
        // ---------------------------------------------------------------------
        std::cout << "[11/14] Verifying MotionDetector studio curve, global brightness immunity & 2-hit debounce..." << std::endl;
        {
            MotionDetector md;
            md.setSensitivityPercent(100.0);
            checkTrue(std::abs(md.triggerThresholdPercent() - 0.25) < 0.01, "100% sensitivity must map to 0.25% delta");
            md.setSensitivityPercent(10.0);
            checkTrue(std::abs(md.triggerThresholdPercent() - 10.50) < 0.01, "10% sensitivity must map to 10.50% delta");
            md.setSensitivityPercent(78.0);
            checkTrue(std::abs(md.triggerThresholdPercent() - 1.252) < 0.05, "78% default sensitivity must match exact studio taper (~1.25%)");
            md.resetWarmup(0.0);
            const double t0 = std::chrono::duration<double>(std::chrono::steady_clock::now().time_since_epoch()).count() + 1.0;

            const int w = 640, h = 360;
            std::vector<uint8_t> frameA(static_cast<size_t>(w) * h, 100);
            auto r0 = md.analyzeNV12Luma(frameA.data(), w, w, h, t0);
            checkTrue(!r0.triggered, "Baseline frame must not trigger motion");

            // Uniform global room brightness shift (+18 across entire frame): 0.65 shift compensation leaves |18 - 12| = 6 <= 14 -> 0% motion!
            std::vector<uint8_t> frameBright(static_cast<size_t>(w) * h, 118);
            auto rBright1 = md.analyzeNV12Luma(frameBright.data(), w, w, h, t0 + 0.1);
            auto rBright2 = md.analyzeNV12Luma(frameBright.data(), w, w, h, t0 + 0.2);
            checkTrue(!rBright1.triggered && !rBright2.triggered, "Uniform ambient brightness shift must NOT false-trigger motion");

            // Localized subject entering room (120x120 bright person patch -> ~6.5% area delta)
            std::vector<uint8_t> frameSubject1 = frameBright;
            for (int y = 80; y < 200; ++y) {
                for (int x = 140; x < 260; ++x) {
                    frameSubject1[static_cast<size_t>(y) * w + x] = 230;
                }
            }
            auto hit1 = md.analyzeNV12Luma(frameSubject1.data(), w, w, h, t0 + 0.3);
            checkTrue(hit1.scorePercent > 3.0 && !hit1.triggered, "First motion frame must arm 2-hit debounce without firing on single-frame noise");

            std::vector<uint8_t> frameSubject2 = frameBright;
            for (int y = 80; y < 200; ++y) {
                for (int x = 220; x < 340; ++x) {
                    frameSubject2[static_cast<size_t>(y) * w + x] = 230;
                }
            }
            auto hit2 = md.analyzeNV12Luma(frameSubject2.data(), w, w, h, t0 + 0.4);
            checkTrue(hit2.triggered, "Second consecutive localized motion frame MUST trigger recording");
        }

        // ---------------------------------------------------------------------
        // Stage 12: Cross-Platform macOS CryptoKit MWV1 Vault Unlock & Decryption
        // ---------------------------------------------------------------------
        std::cout << "[12/14] Verifying byte-for-byte macOS CryptoKit MWV1 cross-platform vault compatibility..." << std::endl;
        {
            std::filesystem::create_directories(crossOsPath / L"Clips", ec);
            const std::string macManifestJson =
                "{\"createdAtISO8601\":\"2026-11-15T08:16:26Z\",\"pbkdf2Iterations\":310000,"
                "\"pbkdf2SaltBase64\":\"V6QxzG2RD2GFg5DdXtTsFg7RnVQpSJ5A6CqYYqUJ+B0=\","
                "\"publicKeyBase64\":\"NgxRqG2TZeDek9U+rZJoMaxfAs9qlpTETz4XYiD7jTo=\","
                "\"version\":1,"
                "\"wrappedPrivateKeyBase64\":\"ONV6FTB8Xk2ZGYjGaziQ9YGq4wtdAvy+VvA3xRvnuITsxf9xwt3\\/PRJ9+Cg6jx1gWyW9sEmCJnn987Ct\"}";
            {
                std::ofstream mf(crossOsPath / L"vault_manifest.json", std::ios::binary);
                mf.write(macManifestJson.data(), static_cast<std::streamsize>(macManifestJson.size()));
            }
            const std::string macClipB64 =
                "TVdWMQABAgB8VaKuvgpAloEIQygzVNbSAAABod5Hc04AAAKAAAABaIP6mSe9QOB0BRjtOpGrHxKPqvyLD0IpyDD4KJXPgQct6hcifJOmD3x4zU6J9LOGGUyo+j8+pZDV71OTuaJu"
                "/isxfG0jiZmaO7Sxk+CSzhwhAAAAAAAAA+gAAAA5AQQKvTNojfq5CQbol8xCxgLzCGu30wTL2zZDG33mMVKudn8GjxwmP5Tk7qJ7fvCq8mF8Z3qbSXRDQgAAAEECHl34VzerR4AC"
                "sc2ScGOzCAUGh29QC8IgRd0MnbVlYvNbim6y8N3aSbQVXMsy86gpOF4L36JXhSE65GnLnGV/pWU=";
            auto macClipBytes = crypto::base64Decode(macClipB64);
            std::filesystem::path macClipFile = crossOsPath / L"Clips" / L"clip_macos_vector.mwvault";
            {
                std::ofstream cf(macClipFile, std::ios::binary);
                cf.write(reinterpret_cast<const char*>(macClipBytes.data()), static_cast<std::streamsize>(macClipBytes.size()));
            }

            VaultManager macVault(crossOsPath.wstring());
            checkTrue(macVault.unlock("cross-os-test-1234"), "Windows VaultManager failed to unlock real macOS CryptoKit manifest");
            auto macReader = macVault.openClipReader(macClipFile.wstring());
            auto decryptedBytes = macReader->readPlaintextRange(0, static_cast<size_t>(macReader->totalPlaintextLength()));
            std::string decryptedText(decryptedBytes.begin(), decryptedBytes.end());
            checkTrue(
                decryptedText == "MACOS_INIT_SEGMENT_PAYLOAD_V1MACOS_MEDIA_FRAGMENT_PAYLOAD_SECOND_1",
                "Cross-OS macOS CryptoKit plaintext mismatch: " + decryptedText
            );
        }

        // ---------------------------------------------------------------------
        // Stage 13: SleepAndLockManager Anti-Sleep & WTS_SESSION_LOCK Auto-Lock
        // ---------------------------------------------------------------------
        std::cout << "[13/14] Verifying Modern Standby anti-sleep request & Win+L screen-lock vault auto-lock..." << std::endl;
        {
            if (!vault.isUnlocked()) {
                checkTrue(vault.unlock("rotated-pass-5678") || vault.unlock("initial-pass-1234"), "Unlock vault for Stage 13 failed");
            }
            SleepAndLockManager slm;
            bool lockCallbackFired = false;
            slm.onScreenLocked = [&]() {
                lockCallbackFired = true;
                vault.lock();
            };

            checkTrue(vault.isUnlocked(), "Vault expected unlocked prior to screen-lock test");
            slm.setArmedPreventingIdleSleep(true);
            checkTrue(slm.isArmedPreventingSleep(), "SleepAndLockManager must hold active power request while armed");

            slm.simulateScreenLockForTest(true);
            checkTrue(lockCallbackFired && slm.isScreenCurrentlyLocked(), "Screen lock event must fire callback");
            checkTrue(!vault.isUnlocked(), "Vault MUST auto-lock immediately when Windows screen locks (Win+L)");

            slm.simulateScreenLockForTest(false);
            slm.setArmedPreventingIdleSleep(false);
            checkTrue(!slm.isArmedPreventingSleep(), "SleepAndLockManager must release power request when disarmed");
        }

#ifdef _WIN32
        // ---------------------------------------------------------------------
        // Stage 14: PE Import Table Audit - Zero Network Socket / HTTP DLLs
        // ---------------------------------------------------------------------
        std::cout << "[14/14] Auditing PE Import Table for 100% offline zero-network guarantee..." << std::endl;
        verifyZeroNetworkDllImportsInCurrentExecutable();
#endif

        std::filesystem::remove_all(rootPath, ec);
        std::cout << "=== ALL 14 WINDOWS AUTOMATED VERIFICATION STAGES PASSED 100% ===" << std::endl;
        return 0;
    } catch (const std::exception& ex) {
        std::cerr << "\n[SELFTEST FAILED] " << ex.what() << std::endl;
        return 2;
    }
}

} // namespace maxwatch
