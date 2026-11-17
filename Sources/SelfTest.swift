import AVFoundation
import CoreMedia
import CoreVideo
import Foundation

// MARK: - Automated Cryptographic, Power-Cut & Fast-Scrub Self-Test (`--selftest <dir>`)

enum MaxWatchSelfTest {
    private static func check(_ condition: @autoclosure () -> Bool, _ message: String) throws {
        if !condition() {
            throw VaultError.ioFailure("SelfTest Assertion Failed: \(message)")
        }
    }

    static func run(in rootDir: URL) -> Int32 {
        do {
            try FileManager.default.createDirectory(at: rootDir, withIntermediateDirectories: true)
            let vaultDir = rootDir.appendingPathComponent("TestVault", isDirectory: true)
            try? FileManager.default.removeItem(at: vaultDir)

            print("[1/12] Initializing AES-256-GCM + Curve25519 Vault (verifying starts LOCKED)...")
            let vault = VaultManager(customVaultDirectory: vaultDir)
            try vault.initializeVault(password: "chill-max-watch-2026")
            try check(vault.isInitialized && !vault.isUnlocked, "Vault must remain LOCKED immediately after initialization")

            // Verify wrong password fails cleanly
            var rejectedWrongPassword = false
            do {
                _ = try vault.unlock(password: "wrong-password")
            } catch VaultError.invalidPassword {
                rejectedWrongPassword = true
            }
            try check(rejectedWrongPassword, "Wrong password must be rejected")
            try check(!vault.isUnlocked, "Vault must still be locked after wrong password")

            print("[2/12] Recording Clip #1 while Vault is LOCKED, then simulating HARD POWER CUT...")
            let powerCutURL = try recordSyntheticClip(
                vault: vault,
                durationSeconds: 3.8,
                simulateHardPowerCut: true
            )
            try check(!vault.isUnlocked, "Vault must stay LOCKED while saving Clip #1")

            print("[3/12] Recording Clip #2 while Vault is LOCKED, then flushing cleanly (Exit-Safe)...")
            let cleanExitURL = try recordSyntheticClip(
                vault: vault,
                durationSeconds: 3.4,
                simulateHardPowerCut: false
            )
            try check(!vault.isUnlocked, "Vault must stay LOCKED while saving Clip #2")

            // Verify zero plaintext mp4/mov files exist on disk
            let allFiles = try FileManager.default.subpathsOfDirectory(atPath: vaultDir.path)
            for rel in allFiles {
                let lower = rel.lowercased()
                try check(!lower.hasSuffix(".mp4") && !lower.hasSuffix(".mov") && !lower.hasSuffix(".m4v"),
                          "Plaintext media file leaked to disk: \(rel)")
            }

            // Verify reading clip while locked fails
            var blockedWhileLocked = false
            do {
                _ = try vault.openClipReader(url: cleanExitURL)
            } catch VaultError.vaultLocked {
                blockedWhileLocked = true
            }
            try check(blockedWhileLocked, "Opening clip reader must fail while vault is locked")

            print("[4/12] Unlocking Vault and verifying Power-Cut Clip recovery + Clean-Exit Clip...")
            try vault.unlock(password: "chill-max-watch-2026")
            try check(vault.isUnlocked, "Vault should unlock with valid password")

            let summaries = vault.listSummaries()
            try check(summaries.count == 2, "Expected 2 playable encrypted clips, found \(summaries.count)")

            let powerCutReader = try vault.openClipReader(url: powerCutURL)
            try check(!powerCutReader.header.cleanlyFinalized, "Power-cut clip should report unfinalized header")
            try check(powerCutReader.mediaFragmentCount >= 2, "Power-cut clip must preserve >= 2 completed 1s segments (got \(powerCutReader.mediaFragmentCount))")

            let cleanReader = try vault.openClipReader(url: cleanExitURL)
            try check(cleanReader.header.cleanlyFinalized, "Clean-exit clip should be marked cleanly finalized")
            try check(cleanReader.mediaFragmentCount >= 3, "Clean-exit clip must have >= 3 media segments (got \(cleanReader.mediaFragmentCount))")

            print("[5/12] Verifying zero-disk-plaintext RAM playback at 1x, 4x, and 12x speeds + fast ball seeking...")
            try verifyPlaybackAndSpeeds(reader: powerCutReader, label: "PowerCutRecoveredClip")
            try verifyPlaybackAndSpeeds(reader: cleanReader, label: "CleanExitClip")

            print("[6/12] Verifying Save & Next MP4 export to external chosen folder...")
            let externalSaveDir = rootDir.appendingPathComponent("ExternalUserChosenFolder", isDirectory: true)
            try FileManager.default.createDirectory(at: externalSaveDir, withIntermediateDirectories: true)
            let exportedMP4 = try vault.exportClipToDirectory(clipURL: cleanExitURL, directoryURL: externalSaveDir)
            let exportedAttrs = try FileManager.default.attributesOfItem(atPath: exportedMP4.path)
            let exportedSize = (exportedAttrs[.size] as? NSNumber)?.int64Value ?? 0
            try check(exportedSize > 10_000, "Exported MP4 should be non-empty (got \(exportedSize) bytes)")

            print("[7/12] Verifying O(1) Vault Password Change & re-unlock...")
            try vault.changePassword(currentPassword: "chill-max-watch-2026", newPassword: "new-vault-password-99")
            vault.lock()
            var oldPassRejected = false
            do {
                _ = try vault.unlock(password: "chill-max-watch-2026")
            } catch VaultError.invalidPassword {
                oldPassRejected = true
            }
            try check(oldPassRejected, "Old password must be rejected after password change")
            try vault.unlock(password: "new-vault-password-99")
            let postRotateReader = try vault.openClipReader(url: cleanExitURL)
            try check(postRotateReader.totalPlaintextLength == cleanReader.totalPlaintextLength,
                      "Existing encrypted clips must remain readable after password change")

            print("[8/12] Verifying instant Delete & Next secure wipe...")
            try vault.secureDeleteClip(url: powerCutURL)
            try check(!FileManager.default.fileExists(atPath: powerCutURL.path), "Deleted clip must be removed")
            try check(vault.listSummaries().count == 1, "Remaining clip count should be 1")

            print("[9/12] Verifying unambiguous international bottom-left timestamp & pixel burn-in...")
            let testDate = Date(timeIntervalSince1970: 1_794_720_934) // Nov 2026
            let tsFull = ClipTimestampFormatter.format(testDate)
            let tsCompact = ClipTimestampFormatter.formatCompact(testDate)
            try check(!tsFull.contains("/") && !tsCompact.contains("/"),
                      "Timestamp must never use ambiguous numeric slashes (got '\(tsFull)')")
            try check(tsFull.contains("2026") && tsCompact.contains("2026"),
                      "Timestamp must always include full 4-digit year (got '\(tsFull)')")
            let monthWords = ["Jan", "Feb", "Mar", "Apr", "May", "Jun", "Jul", "Aug", "Sep", "Oct", "Nov", "Dec"]
            try check(monthWords.contains(where: { tsFull.contains($0) }),
                      "Timestamp must spell out month word unambiguously (got '\(tsFull)')")
            try verifyBottomLeftPixelStamp(date: testDate)

            print("[10/12] Verifying Audio OFF (Video-Only, hasAudio=false) recording & 1x/4x/12x playback...")
            vault.lock()
            let videoOnlyURL = try recordSyntheticClip(
                vault: vault,
                durationSeconds: 2.6,
                simulateHardPowerCut: false,
                hasAudio: false
            )
            try vault.unlock(password: "new-vault-password-99")
            let updatedSummaries = vault.listSummaries()
            guard let voSummary = updatedSummaries.first(where: { $0.url.lastPathComponent == videoOnlyURL.lastPathComponent }) else {
                throw VaultError.ioFailure("Missing video-only clip summary")
            }
            try check(!voSummary.hasAudio, "Audio-OFF clip must have hasAudio == false")
            let voReader = try vault.openClipReader(url: videoOnlyURL)
            try verifyPlaybackAndSpeeds(reader: voReader, label: "AudioOffVideoOnlyClip")

            print("[11/12] Verifying SoundDetector checkpoints ('Normal Talking Voice') & 85Hz HPF acoustic wake...")
            try verifySoundDetectorAndCheckpoints()

            print("[12/12] Verifying Default-ON Max Brightness When Locked boost/restore & Night Motion Floor...")
            try verifyLockedBrightnessAndNightVision()

            vault.lock()
            try? FileManager.default.removeItem(at: rootDir)
            print("ALL 12 MAX WATCH SELF-TESTS PASSED 100%.")
            return 0
        } catch {
            fputs("SELF-TEST FAILED: \(error.localizedDescription)\n", stderr)
            return 1
        }
    }

    private static func verifyBottomLeftPixelStamp(date: Date) throws {
        var pbOpt: CVPixelBuffer?
        let attrs: [CFString: Any] = [kCVPixelBufferIOSurfacePropertiesKey: [:] as [CFString: Any]]
        guard CVPixelBufferCreate(
            kCFAllocatorDefault,
            1280,
            720,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            attrs as CFDictionary,
            &pbOpt
        ) == kCVReturnSuccess, let pb = pbOpt else {
            throw VaultError.ioFailure("Failed to allocate test CVPixelBuffer")
        }
        CVPixelBufferLockBaseAddress(pb, [])
        defer { CVPixelBufferUnlockBaseAddress(pb, []) }
        guard let baseY = CVPixelBufferGetBaseAddressOfPlane(pb, 0),
              let baseUV = CVPixelBufferGetBaseAddressOfPlane(pb, 1) else {
            throw VaultError.ioFailure("Missing NV12 planes")
        }
        let bprY = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
        let bprUV = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
        memset(baseY, 110, bprY * 720)
        memset(baseUV, 128, bprUV * 360)

        let stamper = FrameTimestampStamper()
        stamper.stampBottomLeftLocked(pixelBuffer: pb, date: date)

        let ptrY = baseY.assumingMemoryBound(to: UInt8.self)
        var darkenedPillPixels = 0
        var brightGlyphPixels = 0
        for r in 660..<718 {
            for c in 8..<360 {
                let v = ptrY[r * bprY + c]
                if v < 70 { darkenedPillPixels += 1 }
                if v > 175 { brightGlyphPixels += 1 }
            }
        }
        // Top-right should remain completely untouched (luma == 110)
        let topRightUntouched = (ptrY[40 * bprY + 1200] == 110)
        try check(darkenedPillPixels > 500 && brightGlyphPixels > 80 && topRightUntouched,
                  "Bottom-left timestamp must render dark pill (\(darkenedPillPixels)) + bright text (\(brightGlyphPixels)) exclusively at bottom-left")
    }

    private static func recordSyntheticClip(
        vault: VaultManager,
        durationSeconds: Double,
        simulateHardPowerCut: Bool,
        hasAudio: Bool = true
    ) throws -> URL {
        let width = 1280
        let height = 720
        let fps = 30
        let totalFrames = Int((durationSeconds * Double(fps)).rounded())

        let clipFileWriter = try vault.createClipWriter(width: width, height: height, hasAudio: hasAudio)
        let segWriter = try VaultSegmentWriter(clipWriter: clipFileWriter, width: width, height: height, hasAudio: hasAudio)
        let url = segWriter.fileURL

        for frameIdx in 0..<totalFrames {
            let t = Double(frameIdx) / Double(fps)
            let pts = CMTime(value: CMTimeValue(frameIdx), timescale: CMTimeScale(fps))
            if let vBuf = makeSyntheticVideoBuffer(width: width, height: height, frameIndex: frameIdx, pts: pts) {
                segWriter.appendVideoSampleBuffer(vBuf)
            }
            if hasAudio && frameIdx % 3 == 0 {
                let audioStart = CMTime(seconds: t, preferredTimescale: 48_000)
                if let aBuf = VaultSegmentWriter.makeSilentPCMSampleBuffer(startTime: audioStart, durationSeconds: 0.10) {
                    segWriter.appendAudioSampleBuffer(aBuf)
                }
            }
            // Pump runloop briefly so hardware VideoToolbox emits 1s fMP4 segments in real time
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.008))
        }

        // Give VideoToolbox a brief moment to emit completed 1s segments before simulated power loss
        let waitDeadline = Date().addingTimeInterval(0.8)
        while segWriter.flushedSegmentCount < 3 && Date() < waitDeadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02))
        }

        if simulateHardPowerCut {
            try check(segWriter.flushedSegmentCount >= 3,
                      "Expected >= 3 flushed segments (init + 2x 1s media) BEFORE finishWriting, got \(segWriter.flushedSegmentCount)")
            // Append half-written garbage bytes to the end of the file on disk to simulate mid-write power loss!
            let fd = url.path.withCString { Darwin.open($0, O_RDWR) }
            if fd >= 0 {
                let endPos = Darwin.lseek(fd, 0, SEEK_END)
                var partialCorruptTail: [UInt8] = [0x00, 0x01, 0x80, 0x00, 0x02, 0xDE, 0xAD, 0xBE, 0xEF, 0x11, 0x22]
                _ = Darwin.pwrite(fd, &partialCorruptTail, partialCorruptTail.count, endPos)
                _ = Darwin.fsync(fd)
                Darwin.close(fd)
            }
            return url
        } else {
            segWriter.stopAndFinalizeSynchronously(timeoutSeconds: 3.0)
            return url
        }
    }

    private static func verifyPlaybackAndSpeeds(reader: VaultClipReader, label: String) throws {
        let (asset, loader) = VaultAssetResourceLoader.makePlayableAsset(reader: reader)
        let item = AVPlayerItem(asset: asset)
        item.audioTimePitchAlgorithm = .varispeed
        let player = AVPlayer(playerItem: item)
        player.isMuted = true

        let readyDeadline = Date().addingTimeInterval(5.0)
        while item.status == .unknown && Date() < readyDeadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02))
        }
        guard item.status == .readyToPlay else {
            let msg = item.error?.localizedDescription ?? "status=\(item.status.rawValue)"
            throw VaultError.ioFailure("\(label) failed to become readyToPlay: \(msg)")
        }

        let dur = CMTimeGetSeconds(item.duration)
        try check(dur.isFinite && dur >= 1.8, "\(label) duration should be >= 1.8s, got \(dur)")

        // Verify 1x, 4x, and 12x speeds advance playback time cleanly
        for speed: Float in [1.0, 4.0, 12.0] {
            let seekDone = DispatchSemaphore(value: 0)
            player.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero) { _ in seekDone.signal() }
            while seekDone.wait(timeout: .now()) == .timedOut {
                RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
            }

            // Disable audio tracks above 4x so CoreAudio never clamps 12x visual rate
            for track in item.tracks where track.assetTrack?.mediaType == .audio {
                track.isEnabled = (speed <= 4.0)
            }
            player.playImmediately(atRate: speed)
            let playDeadline = Date().addingTimeInterval(1.2)
            var pos = 0.0
            while Date() < playDeadline {
                RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.03))
                pos = CMTimeGetSeconds(player.currentTime())
                if pos.isFinite && pos >= 0.08 {
                    break
                }
            }
            player.pause()
            try check(pos >= 0.08, "\(label) did not advance at \(speed)x speed (pos=\(pos))")
        }

        // Test fast zero-lag ball seeking across timestamps
        let scrub = ZeroLagScrubChaseEngine(player: player)
        let finalTarget = min(dur - 0.15, 1.6)
        for targetSec in [0.4, 1.3, 0.8, finalTarget] {
            scrub.scrub(to: min(dur - 0.15, targetSec))
            let stepEnd = Date().addingTimeInterval(0.02)
            while Date() < stepEnd {
                RunLoop.current.run(mode: .default, before: stepEnd)
            }
        }
        let scrubWait = Date().addingTimeInterval(1.0)
        var scrubbedPos = CMTimeGetSeconds(player.currentTime())
        while Date() < scrubWait && abs(scrubbedPos - finalTarget) >= 0.25 {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.02))
            scrubbedPos = CMTimeGetSeconds(player.currentTime())
        }
        try check(abs(scrubbedPos - finalTarget) < 0.25,
                  "\(label) fast ball scrub did not reach target \(finalTarget) (got \(scrubbedPos))")

        // Keep strong reference to loader until playback verification finishes
        _ = loader
    }

    private static func makeSyntheticVideoBuffer(width: Int, height: Int, frameIndex: Int, pts: CMTime) -> CMSampleBuffer? {
        var pixelBuffer: CVPixelBuffer?
        let attrs: [CFString: Any] = [
            kCVPixelBufferIOSurfacePropertiesKey: [:] as [CFString: Any]
        ]
        guard CVPixelBufferCreate(
            kCFAllocatorDefault,
            width,
            height,
            kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            attrs as CFDictionary,
            &pixelBuffer
        ) == kCVReturnSuccess, let pb = pixelBuffer else {
            return nil
        }

        CVPixelBufferLockBaseAddress(pb, [])
        if let baseY = CVPixelBufferGetBaseAddressOfPlane(pb, 0) {
            let bprY = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
            memset(baseY, Int32(32 + (frameIndex * 3) % 140), bprY * height)
            // Moving bright bar so every frame is visually distinct during scrubbing
            let barX = (frameIndex * 18) % max(1, width - 80)
            let ptr = baseY.assumingMemoryBound(to: UInt8.self)
            for r in 100..<min(height - 100, 420) {
                let rowOff = r * bprY
                for c in barX..<min(width, barX + 80) {
                    ptr[rowOff + c] = 235
                }
            }
        }
        if let baseUV = CVPixelBufferGetBaseAddressOfPlane(pb, 1) {
            let bprUV = CVPixelBufferGetBytesPerRowOfPlane(pb, 1)
            memset(baseUV, 128, bprUV * (height / 2))
        }
        CVPixelBufferUnlockBaseAddress(pb, [])

        var fmtDesc: CMVideoFormatDescription?
        guard CMVideoFormatDescriptionCreateForImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pb,
            formatDescriptionOut: &fmtDesc
        ) == noErr, let desc = fmtDesc else {
            return nil
        }

        var timing = CMSampleTimingInfo(
            duration: CMTime(value: 1, timescale: 30),
            presentationTimeStamp: pts,
            decodeTimeStamp: .invalid
        )
        var sampleBuffer: CMSampleBuffer?
        guard CMSampleBufferCreateReadyWithImageBuffer(
            allocator: kCFAllocatorDefault,
            imageBuffer: pb,
            formatDescription: desc,
            sampleTiming: &timing,
            sampleBufferOut: &sampleBuffer
        ) == noErr else {
            return nil
        }
        return sampleBuffer
    }

    private static func verifySoundDetectorAndCheckpoints() throws {
        try check(SoundSensitivityCheckpoint.all.count == 5, "Expected 5 sound sensitivity checkpoints")
        let defCP = SoundSensitivityCheckpoint.nearest(toPercent: SoundDetector.defaultSensitivityPercent)
        try check(defCP.title == "Normal Talking Voice",
                  "Default 55% checkpoint must be 'Normal Talking Voice', got '\(defCP.title)'")
        try check(abs(SoundDetector.thresholdDB(forPercent: 55.0) - (-30.0)) < 0.01,
                  "55% sound sensitivity must map to -30.0 dB")
        try check(SoundSensitivityCheckpoint.snapIfClose(56.1) == 55.0,
                  "Checkpoint magnetic snap should snap 56.1% to 55.0%")

        let detector = SoundDetector()
        detector.setSensitivityPercent(55.0) // -30.0 dB
        detector.setEnabled(true)
        detector.reset(warmupUntilTimestamp: 10.0)

        // 1. Sub-bass 20 Hz rumble at -25 dBFS (attenuated well below -30 dB by 85 Hz HPF -> must NOT trigger)
        var lowHum = [Int16](repeating: 0, count: 4800 * 2) // 200 ms @ 48 kHz
        for i in 0..<lowHum.count {
            let amp = 0.056 * 32767.0 * sin(2.0 * Double.pi * 20.0 * Double(i) / 48_000.0)
            lowHum[i] = Int16(amp)
        }
        let humEval = lowHum.withUnsafeBufferPointer { buf in
            detector.analyzePCM16Mono(samples: buf.baseAddress!, count: buf.count, sampleRate: 48_000, nowTimestamp: 12.0)
        }
        try check(!humEval.triggered, "20 Hz sub-bass rumble must be rejected by 85 Hz HPF (got dB=\(humEval.filteredDB))")

        // 2. Single 50 ms brief click at 1 kHz (-18 dBFS) -> 1 window only -> must NOT trigger (requires 2-of-3 windows)
        detector.reset(warmupUntilTimestamp: 10.0)
        var oneWindowTone = [Int16](repeating: 0, count: 2400) // 50 ms
        for i in 0..<oneWindowTone.count {
            oneWindowTone[i] = Int16(0.18 * 32767.0 * sin(2.0 * Double.pi * 1000.0 * Double(i) / 48_000.0))
        }
        let singleClickEval = oneWindowTone.withUnsafeBufferPointer { buf in
            detector.analyzePCM16Mono(samples: buf.baseAddress!, count: buf.count, sampleRate: 48_000, nowTimestamp: 12.0)
        }
        try check(!singleClickEval.triggered, "Single 50ms impulse must be debounced (requires >=2 of 3 windows)")

        // 3. Second 50 ms window of 1 kHz voice-band energy (-18 dBFS) -> 2 of 3 windows -> MUST trigger!
        let voiceTriggerEval = oneWindowTone.withUnsafeBufferPointer { buf in
            detector.analyzePCM16Mono(samples: buf.baseAddress!, count: buf.count, sampleRate: 48_000, nowTimestamp: 12.06)
        }
        try check(voiceTriggerEval.triggered, "Sustained 100ms 1kHz voice-level audio must trigger SoundDetector")

        // 4. Disabled detector must never trigger even on loud audio
        detector.setEnabled(false)
        let disabledEval = oneWindowTone.withUnsafeBufferPointer { buf in
            detector.analyzePCM16Mono(samples: buf.baseAddress!, count: buf.count, sampleRate: 48_000, nowTimestamp: 12.20)
        }
        try check(!disabledEval.triggered, "Disabled SoundDetector must never trigger")
    }

    private static func verifyLockedBrightnessAndNightVision() throws {
        // Verify adaptive night-contrast cell delta floor
        try check(AppController.adaptiveCellDeltaFloor(meanLuma: 120.0) == 14, "Daylight luma floor must be 14")
        try check(AppController.adaptiveCellDeltaFloor(meanLuma: 12.0) == 9, "Pitch-dark luma floor must adapt to 9")
        let twilightFloor = AppController.adaptiveCellDeltaFloor(meanLuma: 34.0)
        try check(twilightFloor >= 10 && twilightFloor <= 13, "Dim luma floor should interpolate smoothly (got \(twilightFloor))")

        // Verify SleepAndLockManager default-ON locked brightness boost & exact restoration
        let mockBackend = MockDisplayBrightnessBackend(initialBrightness: 0.34)
        let mgr = SleepAndLockManager(
            brightnessBackend: mockBackend,
            registerSystemNotifications: false,
            enableSystemPowerAssertions: false
        )
        try check(mgr.maxBrightnessWhenLockedEnabled == true,
                  "Max Brightness When Locked MUST be checked (true) by default")

        // Arming while screen is UNLOCKED must NOT change user's screen brightness
        mgr.setArmedPreventingIdleSleep(true)
        try check(abs(mockBackend.currentBrightness - 0.34) < 0.001,
                  "Arming while unlocked must keep user brightness at 0.34 (got \(mockBackend.currentBrightness))")

        // Locking screen while armed (with default ON) MUST boost brightness to 100% (1.0)
        mgr.simulateScreenLockStateForTesting(true)
        try check(mgr.isBrightnessBoostActive && abs(mockBackend.currentBrightness - 1.0) < 0.001,
                  "Locking screen while armed must boost brightness to 1.0 (got \(mockBackend.currentBrightness))")

        // Simulate ambient-light auto-dimming to 0.50 while locked, then periodic maintenance tick -> re-boosts to 1.0 WITHOUT losing saved 0.34!
        mockBackend.currentBrightness = 0.50
        mgr.periodicMaintenanceTick(now: Date().addingTimeInterval(5.0))
        try check(abs(mockBackend.currentBrightness - 1.0) < 0.001,
                  "Periodic maintenance must keep locked display at 1.0 against auto-dimming")

        // Unchecking option while locked restores original 0.34 immediately
        mgr.maxBrightnessWhenLockedEnabled = false
        try check(!mgr.isBrightnessBoostActive && abs(mockBackend.currentBrightness - 0.34) < 0.001,
                  "Unchecking Max Brightness When Locked must restore original 0.34 (got \(mockBackend.currentBrightness))")

        // Re-checking while still armed & locked boosts back to 1.0
        mgr.maxBrightnessWhenLockedEnabled = true
        try check(mgr.isBrightnessBoostActive && abs(mockBackend.currentBrightness - 1.0) < 0.001,
                  "Re-checking option while armed & locked must boost to 1.0")

        // Unlocking screen restores exact original 0.34
        mgr.simulateScreenLockStateForTesting(false)
        try check(!mgr.isBrightnessBoostActive && abs(mockBackend.currentBrightness - 0.34) < 0.001,
                  "Unlocking screen must restore exact original brightness 0.34 (got \(mockBackend.currentBrightness))")

        // Lock again -> 1.0, then disarm -> restores 0.34
        mgr.simulateScreenLockStateForTesting(true)
        try check(abs(mockBackend.currentBrightness - 1.0) < 0.001, "Expected 1.0 on second lock")
        mgr.setArmedPreventingIdleSleep(false)
        try check(abs(mockBackend.currentBrightness - 0.34) < 0.001,
                  "Disarming must restore original brightness 0.34")
    }
}

private final class MockDisplayBrightnessBackend: DisplayBrightnessBackend {
    var currentBrightness: Float

    init(initialBrightness: Float) {
        self.currentBrightness = initialBrightness
    }

    func snapshotBrightnessByDisplay() -> [UInt32: Float] {
        return [1: currentBrightness]
    }

    func setAllDisplaysBrightness(_ level: Float) {
        currentBrightness = max(0.0, min(1.0, level))
    }

    func restoreBrightnessByDisplay(_ saved: [UInt32: Float]) {
        if let v = saved[1] {
            currentBrightness = v
        }
    }
}
