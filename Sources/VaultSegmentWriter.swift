import AVFoundation
import CoreMedia
import CoreVideo
import Foundation
import UniformTypeIdentifiers

// MARK: - Real-Time 1-Second fMP4 -> AES-256-GCM Segment Writer

final class VaultSegmentWriter: NSObject, AVAssetWriterDelegate {
    let clipWriter: VaultClipFileWriter
    let width: Int
    let height: Int
    let hasAudio: Bool

    private let writerQueue = DispatchQueue(label: "app.c60.maxwatch.segmentWriterQueue", qos: .userInitiated)
    private var assetWriter: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var audioInput: AVAssetWriterInput?

    private var sessionStarted = false
    private var isFinishing = false
    private var firstVideoPTS: CMTime = .invalid
    private var latestVideoRelativePTS: CMTime = .zero
    private var latestAudioRelativePTS: CMTime = .invalid
    private var emittedSegmentCount: Int = 0
    private var writerError: Error?

    init(clipWriter: VaultClipFileWriter, width: Int, height: Int, hasAudio: Bool) throws {
        self.clipWriter = clipWriter
        self.width = max(320, width)
        self.height = max(240, height)
        self.hasAudio = hasAudio
        super.init()

        let writer = AVAssetWriter(contentType: .mpeg4Movie)
        writer.outputFileTypeProfile = .mpeg4AppleHLS
        writer.preferredOutputSegmentInterval = CMTime(seconds: 1.0, preferredTimescale: 600)
        writer.initialSegmentStartTime = .zero
        writer.shouldOptimizeForNetworkUse = true
        writer.delegate = self

        // Scale bitrate cleanly to resolution (12 Mbps @ 1080p, 6 Mbps @ 720p)
        let pixels = Double(self.width * self.height)
        let scale = max(0.35, min(1.5, pixels / (1920.0 * 1080.0)))
        let bitrate = Int(12_000_000.0 * scale)

        let videoCompression: [String: Any] = [
            AVVideoAverageBitRateKey: bitrate,
            AVVideoProfileLevelKey: AVVideoProfileLevelH264HighAutoLevel,
            AVVideoMaxKeyFrameIntervalKey: 8,               // Keyframe every ~0.26s for instant ball scrubbing
            AVVideoMaxKeyFrameIntervalDurationKey: 0.30,
            AVVideoExpectedSourceFrameRateKey: 30,
            AVVideoAllowFrameReorderingKey: false           // Zero B-frame reorder lag during fast drag scrubbing
        ]
        let videoSettings: [String: Any] = [
            AVVideoCodecKey: AVVideoCodecType.h264,
            AVVideoWidthKey: self.width,
            AVVideoHeightKey: self.height,
            AVVideoCompressionPropertiesKey: videoCompression
        ]

        let vIn = AVAssetWriterInput(mediaType: .video, outputSettings: videoSettings)
        vIn.expectsMediaDataInRealTime = true
        guard writer.canAdd(vIn) else {
            throw VaultError.ioFailure("Cannot configure H.264 video writer input.")
        }
        writer.add(vIn)
        self.videoInput = vIn

        if hasAudio {
            let audioSettings: [String: Any] = [
                AVFormatIDKey: kAudioFormatMPEG4AAC,
                AVSampleRateKey: 48_000,
                AVNumberOfChannelsKey: 1,
                AVEncoderBitRateKey: 128_000
            ]
            let aIn = AVAssetWriterInput(mediaType: .audio, outputSettings: audioSettings)
            aIn.expectsMediaDataInRealTime = true
            if writer.canAdd(aIn) {
                writer.add(aIn)
                self.audioInput = aIn
            }
        }

        guard writer.startWriting() else {
            let reason = writer.error?.localizedDescription ?? "Unknown AVAssetWriter error"
            throw VaultError.ioFailure("Failed to start segment writer: \(reason)")
        }
        self.assetWriter = writer
    }

    var fileURL: URL { clipWriter.fileURL }

    var recordedDurationSeconds: Double {
        writerQueue.sync {
            guard sessionStarted, latestVideoRelativePTS.isValid else { return 0.0 }
            return max(0.0, CMTimeGetSeconds(latestVideoRelativePTS))
        }
    }

    var flushedSegmentCount: Int {
        clipWriter.writtenChunkCount
    }

    func appendVideoSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        writerQueue.async { [weak self] in
            self?.appendVideoOnQueue(sampleBuffer)
        }
    }

    func appendAudioSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        guard hasAudio else { return }
        writerQueue.async { [weak self] in
            self?.appendAudioOnQueue(sampleBuffer)
        }
    }

    private func appendVideoOnQueue(_ sampleBuffer: CMSampleBuffer) {
        guard !isFinishing,
              let writer = assetWriter,
              writer.status == .writing,
              let vIn = videoInput else { return }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        guard pts.isValid else { return }

        if !sessionStarted {
            firstVideoPTS = pts
            writer.startSession(atSourceTime: .zero)
            sessionStarted = true
        }

        let relPTS = CMTimeSubtract(pts, firstVideoPTS)
        guard relPTS >= .zero else { return }

        if vIn.isReadyForMoreMediaData,
           let retimed = Self.retimeSampleBuffer(sampleBuffer, newPTS: relPTS) {
            if vIn.append(retimed) {
                latestVideoRelativePTS = relPTS
            }
        }

        // Audio gap watchdog: if an audio track was attached and the mic drops/stalls > 0.35s,
        // feed silent 48kHz PCM so fMP4 1-second segments continue flushing on schedule!
        if let aIn = audioInput, aIn.isReadyForMoreMediaData {
            let audioPos = latestAudioRelativePTS.isValid ? latestAudioRelativePTS : .zero
            let gap = CMTimeGetSeconds(CMTimeSubtract(relPTS, audioPos))
            if gap > 0.35 {
                let fillStart = audioPos
                let fillSeconds = min(0.25, gap - 0.10)
                if fillSeconds >= 0.02,
                   let silence = Self.makeSilentPCMSampleBuffer(startTime: fillStart, durationSeconds: fillSeconds) {
                    if aIn.append(silence) {
                        latestAudioRelativePTS = CMTimeAdd(fillStart, CMTime(seconds: fillSeconds, preferredTimescale: 48_000))
                    }
                }
            }
        }
    }

    private func appendAudioOnQueue(_ sampleBuffer: CMSampleBuffer) {
        guard !isFinishing,
              sessionStarted,
              firstVideoPTS.isValid,
              let writer = assetWriter,
              writer.status == .writing,
              let aIn = audioInput,
              aIn.isReadyForMoreMediaData else { return }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        guard pts.isValid else { return }

        let relPTS = CMTimeSubtract(pts, firstVideoPTS)
        guard relPTS >= .zero else { return }
        if latestAudioRelativePTS.isValid && CMTimeCompare(relPTS, latestAudioRelativePTS) < 0 {
            return
        }

        if let retimed = Self.retimeSampleBuffer(sampleBuffer, newPTS: relPTS) {
            if aIn.append(retimed) {
                let dur = CMSampleBufferGetDuration(retimed)
                latestAudioRelativePTS = dur.isValid ? CMTimeAdd(relPTS, dur) : relPTS
            }
        }
    }

    /// Flushes remaining sub-second frames, encrypts & fsyncs the tail fragment, and seals the clip header.
    func stopAndFinalize(completion: @escaping (URL, Double, Error?) -> Void) {
        writerQueue.async { [weak self] in
            guard let self = self else { return }
            if self.isFinishing {
                completion(self.clipWriter.fileURL, 0, nil)
                return
            }
            self.isFinishing = true

            guard let writer = self.assetWriter else {
                try? self.clipWriter.finalizeAndClose(durationSeconds: 0, cleanlyFinalized: false)
                completion(self.clipWriter.fileURL, 0, self.writerError)
                return
            }

            if !self.sessionStarted || writer.status != .writing {
                writer.cancelWriting()
                try? self.clipWriter.finalizeAndClose(durationSeconds: 0, cleanlyFinalized: false)
                completion(self.clipWriter.fileURL, 0, self.writerError)
                return
            }

            // Ensure audio catches up to final video PTS before finishing so final segment flushes cleanly
            if let aIn = self.audioInput {
                let audioPos = self.latestAudioRelativePTS.isValid ? self.latestAudioRelativePTS : .zero
                let tailGap = CMTimeGetSeconds(CMTimeSubtract(self.latestVideoRelativePTS, audioPos))
                if tailGap > 0.02, aIn.isReadyForMoreMediaData,
                   let silence = Self.makeSilentPCMSampleBuffer(startTime: audioPos, durationSeconds: min(1.0, tailGap)) {
                    _ = aIn.append(silence)
                }
                aIn.markAsFinished()
            }
            self.videoInput?.markAsFinished()

            let finalDur = max(0.1, CMTimeGetSeconds(self.latestVideoRelativePTS) + (1.0 / 30.0))
            writer.finishWriting { [weak self] in
                guard let self = self else { return }
                self.writerQueue.async {
                    let wErr = writer.error ?? self.writerError
                    do {
                        try self.clipWriter.finalizeAndClose(durationSeconds: finalDur, cleanlyFinalized: wErr == nil)
                        completion(self.clipWriter.fileURL, finalDur, wErr)
                    } catch {
                        completion(self.clipWriter.fileURL, finalDur, error)
                    }
                }
            }
        }
    }

    /// Synchronous flush used during `applicationShouldTerminate` / `SIGTERM` / power-off shutdown.
    func stopAndFinalizeSynchronously(timeoutSeconds: TimeInterval = 2.5) {
        let sem = DispatchSemaphore(value: 0)
        stopAndFinalize { _, _, _ in
            sem.signal()
        }
        let deadline = Date().addingTimeInterval(timeoutSeconds)
        while sem.wait(timeout: .now()) == .timedOut && Date() < deadline {
            RunLoop.current.run(mode: .default, before: Date().addingTimeInterval(0.01))
        }
    }

    // MARK: - AVAssetWriterDelegate (Called in Memory for Every 1.0s fMP4 Segment)

    func assetWriter(
        _ writer: AVAssetWriter,
        didOutputSegmentData segmentData: Data,
        segmentType: AVAssetSegmentType,
        segmentReport: AVAssetSegmentReport?
    ) {
        let kind: VaultSegmentKind = (segmentType == .initialization) ? .initialization : .mediaFragment
        do {
            try clipWriter.appendSegment(segmentData, kind: kind)
        } catch {
            writerQueue.async { [weak self] in
                self?.writerError = error
            }
        }
    }

    // MARK: - Sample Buffer Helpers

    static func retimeSampleBuffer(_ sampleBuffer: CMSampleBuffer, newPTS: CMTime) -> CMSampleBuffer? {
        var count: CMItemCount = 0
        guard CMSampleBufferGetSampleTimingInfoArray(sampleBuffer, entryCount: 0, arrayToFill: nil, entriesNeededOut: &count) == noErr,
              count > 0 else {
            return nil
        }
        var infos = [CMSampleTimingInfo](
            repeating: CMSampleTimingInfo(duration: .invalid, presentationTimeStamp: .invalid, decodeTimeStamp: .invalid),
            count: count
        )
        guard CMSampleBufferGetSampleTimingInfoArray(sampleBuffer, entryCount: count, arrayToFill: &infos, entriesNeededOut: nil) == noErr else {
            return nil
        }

        let basePTS = infos[0].presentationTimeStamp
        for i in 0..<count {
            let offset = infos[i].presentationTimeStamp.isValid && basePTS.isValid
                ? CMTimeSubtract(infos[i].presentationTimeStamp, basePTS)
                : .zero
            infos[i].presentationTimeStamp = CMTimeAdd(newPTS, offset)
            infos[i].decodeTimeStamp = .invalid
        }

        var copy: CMSampleBuffer?
        let status = CMSampleBufferCreateCopyWithNewTiming(
            allocator: kCFAllocatorDefault,
            sampleBuffer: sampleBuffer,
            sampleTimingEntryCount: count,
            sampleTimingArray: &infos,
            sampleBufferOut: &copy
        )
        return status == noErr ? copy : nil
    }

    static func makeSilentPCMSampleBuffer(startTime: CMTime, durationSeconds: Double, sampleRate: Int = 48_000) -> CMSampleBuffer? {
        let frameCount = max(1, Int((durationSeconds * Double(sampleRate)).rounded()))
        var asbd = AudioStreamBasicDescription(
            mSampleRate: Float64(sampleRate),
            mFormatID: kAudioFormatLinearPCM,
            mFormatFlags: kLinearPCMFormatFlagIsSignedInteger | kLinearPCMFormatFlagIsPacked,
            mBytesPerPacket: 2,
            mFramesPerPacket: 1,
            mBytesPerFrame: 2,
            mChannelsPerFrame: 1,
            mBitsPerChannel: 16,
            mReserved: 0
        )

        var formatDesc: CMAudioFormatDescription?
        guard CMAudioFormatDescriptionCreate(
            allocator: kCFAllocatorDefault,
            asbd: &asbd,
            layoutSize: 0,
            layout: nil,
            magicCookieSize: 0,
            magicCookie: nil,
            extensions: nil,
            formatDescriptionOut: &formatDesc
        ) == noErr, let fmt = formatDesc else {
            return nil
        }

        let byteLen = frameCount * 2
        var blockBuf: CMBlockBuffer?
        guard CMBlockBufferCreateWithMemoryBlock(
            allocator: kCFAllocatorDefault,
            memoryBlock: nil,
            blockLength: byteLen,
            blockAllocator: kCFAllocatorDefault,
            customBlockSource: nil,
            offsetToData: 0,
            dataLength: byteLen,
            flags: kCMBlockBufferAssureMemoryNowFlag,
            blockBufferOut: &blockBuf
        ) == noErr, let block = blockBuf else {
            return nil
        }
        CMBlockBufferFillDataBytes(with: 0, blockBuffer: block, offsetIntoDestination: 0, dataLength: byteLen)

        var sampleBuf: CMSampleBuffer?
        let status = CMAudioSampleBufferCreateReadyWithPacketDescriptions(
            allocator: kCFAllocatorDefault,
            dataBuffer: block,
            formatDescription: fmt,
            sampleCount: frameCount,
            presentationTimeStamp: startTime,
            packetDescriptions: nil,
            sampleBufferOut: &sampleBuf
        )
        return status == noErr ? sampleBuf : nil
    }
}
