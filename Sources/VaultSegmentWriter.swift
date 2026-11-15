import AppKit
import AVFoundation
import CoreGraphics
import CoreMedia
import CoreText
import CoreVideo
import Foundation
import UniformTypeIdentifiers

// MARK: - Unambiguous International Computer-Time Formatter

enum ClipTimestampFormatter {
    private static let lock = NSLock()
    private static var cachedFullFormatter: DateFormatter?
    private static var cachedCompactFormatter: DateFormatter?
    private static var cachedTimeZoneID: String = ""
    private static var cachedUses12Hour: Bool = false

    static func systemUses12HourClock(locale: Locale = .autoupdatingCurrent) -> Bool {
        let j = DateFormatter.dateFormat(fromTemplate: "j", options: 0, locale: locale) ?? ""
        return j.contains("a")
    }

    private static func ensureFormattersLocked() -> (full: DateFormatter, compact: DateFormatter) {
        let tz = TimeZone.autoupdatingCurrent
        let is12h = systemUses12HourClock()
        if let full = cachedFullFormatter,
           let compact = cachedCompactFormatter,
           cachedTimeZoneID == tz.identifier,
           cachedUses12Hour == is12h {
            return (full, compact)
        }

        // Always spell out the month word ("Nov") + 4-digit year ("2026") so dates are 100%
        // unambiguous worldwide (never numeric 11/12/26 or 12/11/26), in the user's computer timezone.
        let full = DateFormatter()
        full.locale = Locale(identifier: "en_US_POSIX")
        full.calendar = Calendar(identifier: .gregorian)
        full.timeZone = tz
        full.dateFormat = is12h ? "d MMM yyyy • h:mm:ss a zzz" : "d MMM yyyy • HH:mm:ss zzz"

        let compact = DateFormatter()
        compact.locale = Locale(identifier: "en_US_POSIX")
        compact.calendar = Calendar(identifier: .gregorian)
        compact.timeZone = tz
        compact.dateFormat = is12h ? "d MMM yyyy, h:mm:ss a" : "d MMM yyyy, HH:mm:ss"

        cachedFullFormatter = full
        cachedCompactFormatter = compact
        cachedTimeZoneID = tz.identifier
        cachedUses12Hour = is12h
        return (full, compact)
    }

    static func format(_ date: Date = Date()) -> String {
        lock.lock()
        let (full, _) = ensureFormattersLocked()
        let text = full.string(from: date)
        lock.unlock()
        return text
    }

    static func formatCompact(_ date: Date = Date()) -> String {
        lock.lock()
        let (_, compact) = ensureFormattersLocked()
        let text = compact.string(from: date)
        lock.unlock()
        return text
    }
}

// MARK: - 1-Hz Cached NV12 Bottom-Left Timestamp Stamper

final class FrameTimestampStamper {
    private var cachedSecond: Int64 = Int64.min
    private var cachedFrameW: Int = 0
    private var cachedFrameH: Int = 0
    private var stampW: Int = 0
    private var stampH: Int = 0
    // Packed 4 bytes per pixel: [Y, Cb, Cr, Alpha] in BT.709 Video Range
    private var stampYCbCrA: [UInt8] = []

    func stampBottomLeftLocked(pixelBuffer: CVPixelBuffer, date: Date) {
        guard CVPixelBufferGetPlaneCount(pixelBuffer) >= 2 else { return }
        let dstW = CVPixelBufferGetWidthOfPlane(pixelBuffer, 0)
        let dstH = CVPixelBufferGetHeightOfPlane(pixelBuffer, 0)
        guard dstW >= 160, dstH >= 120 else { return }

        let sec = Int64(floor(date.timeIntervalSince1970))
        if sec != cachedSecond || dstW != cachedFrameW || dstH != cachedFrameH {
            rebuildStamp(text: ClipTimestampFormatter.format(date), frameW: dstW, frameH: dstH)
            cachedSecond = sec
            cachedFrameW = dstW
            cachedFrameH = dstH
        }

        guard stampW > 0, stampH > 0, stampW + 8 < dstW, stampH + 8 < dstH,
              let baseY = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 0),
              let baseUV = CVPixelBufferGetBaseAddressOfPlane(pixelBuffer, 1) else {
            return
        }

        let bprY = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 0)
        let bprUV = CVPixelBufferGetBytesPerRowOfPlane(pixelBuffer, 1)
        let ptrY = baseY.assumingMemoryBound(to: UInt8.self)
        let ptrUV = baseUV.assumingMemoryBound(to: UInt8.self)

        let marginX = (max(10, Int((Double(dstW) * 0.010).rounded())) & ~1)
        let marginBottom = (max(10, Int((Double(dstH) * 0.013).rounded())) & ~1)
        let originX = min(marginX, (dstW - stampW) & ~1)
        let originY = max(0, (dstH - marginBottom - stampH) & ~1)

        stampYCbCrA.withUnsafeBufferPointer { srcBuf in
            guard let src = srcBuf.baseAddress else { return }

            // 1. Blend Luma (Plane 0)
            for r in 0..<stampH {
                let yRowOff = (originY + r) * bprY + originX
                let sRowOff = r * stampW * 4
                for c in 0..<stampW {
                    let sIdx = sRowOff + c * 4
                    let sA = Int(src[sIdx + 3])
                    if sA == 0 { continue }
                    let sY = Int(src[sIdx])
                    let dY = Int(ptrY[yRowOff + c])
                    ptrY[yRowOff + c] = UInt8((sY * sA + dY * (255 - sA) + 127) / 255)
                }
            }

            // 2. Blend 2x2 Subsampled Chroma (Plane 1: interleaved Cb, Cr)
            let halfH = stampH >> 1
            let halfW = stampW >> 1
            let uvOriginY = originY >> 1
            for r2 in 0..<halfH {
                let uvRowOff = (uvOriginY + r2) * bprUV + originX
                let sRowOff = (r2 * 2) * stampW * 4
                for c2 in 0..<halfW {
                    let sIdx = sRowOff + (c2 * 2) * 4
                    let sA = Int(src[sIdx + 3])
                    if sA == 0 { continue }
                    let sCb = Int(src[sIdx + 1])
                    let sCr = Int(src[sIdx + 2])
                    let uvIdx = uvRowOff + c2 * 2
                    let dCb = Int(ptrUV[uvIdx])
                    let dCr = Int(ptrUV[uvIdx + 1])
                    ptrUV[uvIdx] = UInt8((sCb * sA + dCb * (255 - sA) + 127) / 255)
                    ptrUV[uvIdx + 1] = UInt8((sCr * sA + dCr * (255 - sA) + 127) / 255)
                }
            }
        }
    }

    private func rebuildStamp(text: String, frameW: Int, frameH: Int) {
        let fontSize = max(11.0, min(26.0, (CGFloat(frameH) * 0.0165).rounded()))
        let nsFont = NSFont.monospacedDigitSystemFont(ofSize: fontSize, weight: .semibold)
        let ctFont = nsFont as CTFont

        let attrs: [CFString: Any] = [
            kCTFontAttributeName: ctFont,
            kCTForegroundColorAttributeName: CGColor(srgbRed: 1.0, green: 1.0, blue: 1.0, alpha: 0.96)
        ]
        guard let attrStr = CFAttributedStringCreate(kCFAllocatorDefault, text as CFString, attrs as CFDictionary) else {
            return
        }
        let line = CTLineCreateWithAttributedString(attrStr)
        var ascent: CGFloat = 0
        var descent: CGFloat = 0
        var leading: CGFloat = 0
        let textWidth = CGFloat(CTLineGetTypographicBounds(line, &ascent, &descent, &leading))

        let padX = max(7, Int((fontSize * 0.55).rounded()))
        let padY = max(4, Int((fontSize * 0.30).rounded()))
        let rawW = min(frameW - 16, Int(ceil(textWidth)) + padX * 2)
        let rawH = min(frameH - 16, Int(ceil(ascent + descent)) + padY * 2)
        let w = max(4, (rawW + 1) & ~1)
        let h = max(4, (rawH + 1) & ~1)

        var rgba = [UInt8](repeating: 0, count: w * h * 4)
        let colorSpace = CGColorSpace(name: CGColorSpace.sRGB) ?? CGColorSpaceCreateDeviceRGB()
        let bmpInfo = CGImageAlphaInfo.premultipliedLast.rawValue | CGBitmapInfo.byteOrder32Big.rawValue

        rgba.withUnsafeMutableBytes { rawPtr in
            guard let base = rawPtr.baseAddress,
                  let ctx = CGContext(
                      data: base,
                      width: w,
                      height: h,
                      bitsPerComponent: 8,
                      bytesPerRow: w * 4,
                      space: colorSpace,
                      bitmapInfo: bmpInfo
                  ) else { return }

            ctx.setAllowsAntialiasing(true)
            ctx.setShouldAntialias(true)
            ctx.setAllowsFontSmoothing(true)
            ctx.setShouldSmoothFonts(true)

            let rect = CGRect(x: 0.5, y: 0.5, width: CGFloat(w) - 1.0, height: CGFloat(h) - 1.0)
            let radius = max(4.0, (fontSize * 0.34).rounded())
            let pill = CGPath(roundedRect: rect, cornerWidth: radius, cornerHeight: radius, transform: nil)
            ctx.addPath(pill)
            ctx.setFillColor(CGColor(srgbRed: 0.04, green: 0.06, blue: 0.10, alpha: 0.72))
            ctx.fillPath()

            ctx.addPath(pill)
            ctx.setStrokeColor(CGColor(srgbRed: 1.0, green: 1.0, blue: 1.0, alpha: 0.16))
            ctx.setLineWidth(1.0)
            ctx.strokePath()

            ctx.textMatrix = .identity
            ctx.textPosition = CGPoint(x: CGFloat(padX), y: CGFloat(padY) + descent)
            CTLineDraw(line, ctx)
        }

        // Pre-convert premultiplied RGBA to BT.709 Video-Range [Y, Cb, Cr, A]
        var out = [UInt8](repeating: 0, count: w * h * 4)
        let pixelCount = w * h
        for i in 0..<pixelCount {
            let idx = i * 4
            let a = Int(rgba[idx + 3])
            if a == 0 {
                out[idx] = 16
                out[idx + 1] = 128
                out[idx + 2] = 128
                out[idx + 3] = 0
                continue
            }
            let r = min(255, (Int(rgba[idx]) * 255 + (a >> 1)) / a)
            let g = min(255, (Int(rgba[idx + 1]) * 255 + (a >> 1)) / a)
            let b = min(255, (Int(rgba[idx + 2]) * 255 + (a >> 1)) / a)

            let yVal = max(16, min(235, 16 + ((47 * r + 157 * g + 16 * b + 128) >> 8)))
            let cbVal = max(16, min(240, 128 + ((-26 * r - 87 * g + 112 * b + 128) >> 8)))
            let crVal = max(16, min(240, 128 + ((112 * r - 102 * g - 10 * b + 128) >> 8)))

            out[idx] = UInt8(yVal)
            out[idx + 1] = UInt8(cbVal)
            out[idx + 2] = UInt8(crVal)
            out[idx + 3] = UInt8(a)
        }

        self.stampW = w
        self.stampH = h
        self.stampYCbCrA = out
    }
}

// MARK: - Real-Time 1-Second fMP4 -> AES-256-GCM Segment Writer

final class VaultSegmentWriter: NSObject, AVAssetWriterDelegate {
    let clipWriter: VaultClipFileWriter
    let width: Int
    let height: Int
    let hasAudio: Bool

    private let writerQueue = DispatchQueue(label: "app.c60.maxwatch.segmentWriterQueue", qos: .userInitiated)
    private var assetWriter: AVAssetWriter?
    private var videoInput: AVAssetWriterInput?
    private var pixelBufferAdaptor: AVAssetWriterInputPixelBufferAdaptor?
    private var audioInput: AVAssetWriterInput?
    private let timestampStamper = FrameTimestampStamper()

    private var sessionStarted = false
    private var hasAppendedVideoFrame = false
    private var isFinishing = false
    private var firstVideoPTS: CMTime = .invalid
    private var latestVideoRelativePTS: CMTime = .zero
    private var latestAudioRelativePTS: CMTime = .invalid
    private var emittedSegmentCount: Int = 0
    private var writerError: Error?

    init(clipWriter: VaultClipFileWriter, width: Int, height: Int, hasAudio: Bool) throws {
        self.clipWriter = clipWriter
        self.width = max(320, width) & ~1
        self.height = max(240, height) & ~1
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

        let sourcePBAttrs: [String: Any] = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
            kCVPixelBufferWidthKey as String: self.width,
            kCVPixelBufferHeightKey as String: self.height,
            kCVPixelBufferIOSurfacePropertiesKey as String: [:]
        ]
        self.pixelBufferAdaptor = AVAssetWriterInputPixelBufferAdaptor(
            assetWriterInput: vIn,
            sourcePixelBufferAttributes: sourcePBAttrs
        )

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

    func appendVideoSampleBuffer(_ sampleBuffer: CMSampleBuffer, captureDate: Date = Date()) {
        writerQueue.async { [weak self] in
            self?.appendVideoOnQueue(sampleBuffer, captureDate: captureDate)
        }
    }

    func appendAudioSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        guard hasAudio else { return }
        writerQueue.async { [weak self] in
            self?.appendAudioOnQueue(sampleBuffer)
        }
    }

    private func appendVideoOnQueue(_ sampleBuffer: CMSampleBuffer, captureDate: Date) {
        guard !isFinishing,
              let writer = assetWriter,
              writer.status == .writing,
              let vIn = videoInput,
              let adaptor = pixelBufferAdaptor,
              let srcPB = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        let pts = CMSampleBufferGetPresentationTimeStamp(sampleBuffer)
        guard pts.isValid else { return }

        if !sessionStarted {
            firstVideoPTS = pts
            writer.startSession(atSourceTime: .zero)
            sessionStarted = true
        }

        let relPTS = CMTimeSubtract(pts, firstVideoPTS)
        guard relPTS >= .zero else { return }
        if hasAppendedVideoFrame && CMTimeCompare(relPTS, latestVideoRelativePTS) <= 0 {
            return
        }

        if vIn.isReadyForMoreMediaData,
           let stampedPB = copyAndStampNV12Buffer(from: srcPB, adaptor: adaptor, date: captureDate) {
            if adaptor.append(stampedPB, withPresentationTime: relPTS) {
                hasAppendedVideoFrame = true
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

    private func copyAndStampNV12Buffer(
        from srcPB: CVPixelBuffer,
        adaptor: AVAssetWriterInputPixelBufferAdaptor,
        date: Date
    ) -> CVPixelBuffer? {
        guard CVPixelBufferGetPlaneCount(srcPB) >= 2 else { return nil }
        var pooledOut: CVPixelBuffer?
        if let pool = adaptor.pixelBufferPool {
            CVPixelBufferPoolCreatePixelBuffer(kCFAllocatorDefault, pool, &pooledOut)
        }
        if pooledOut == nil {
            let attrs: [CFString: Any] = [
                kCVPixelBufferIOSurfacePropertiesKey: [:] as [CFString: Any]
            ]
            CVPixelBufferCreate(
                kCFAllocatorDefault,
                self.width,
                self.height,
                kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange,
                attrs as CFDictionary,
                &pooledOut
            )
        }
        guard let dstPB = pooledOut else { return nil }

        CVPixelBufferLockBaseAddress(srcPB, .readOnly)
        CVPixelBufferLockBaseAddress(dstPB, [])
        defer {
            CVPixelBufferUnlockBaseAddress(dstPB, [])
            CVPixelBufferUnlockBaseAddress(srcPB, .readOnly)
        }

        let srcW = CVPixelBufferGetWidthOfPlane(srcPB, 0)
        let srcH = CVPixelBufferGetHeightOfPlane(srcPB, 0)
        let dstW = CVPixelBufferGetWidthOfPlane(dstPB, 0)
        let dstH = CVPixelBufferGetHeightOfPlane(dstPB, 0)
        let copyW = min(srcW, dstW)
        let copyH = min(srcH, dstH)

        if let sY = CVPixelBufferGetBaseAddressOfPlane(srcPB, 0),
           let dY = CVPixelBufferGetBaseAddressOfPlane(dstPB, 0) {
            let sBPR = CVPixelBufferGetBytesPerRowOfPlane(srcPB, 0)
            let dBPR = CVPixelBufferGetBytesPerRowOfPlane(dstPB, 0)
            if dstW > copyW || dstH > copyH {
                memset(dY, 16, dBPR * dstH)
            }
            for r in 0..<copyH {
                memcpy(dY.advanced(by: r * dBPR), sY.advanced(by: r * sBPR), copyW)
            }
        }

        if let sUV = CVPixelBufferGetBaseAddressOfPlane(srcPB, 1),
           let dUV = CVPixelBufferGetBaseAddressOfPlane(dstPB, 1) {
            let sBPR = CVPixelBufferGetBytesPerRowOfPlane(srcPB, 1)
            let dBPR = CVPixelBufferGetBytesPerRowOfPlane(dstPB, 1)
            let uvCopyH = copyH >> 1
            let uvDstH = dstH >> 1
            let uvCopyBytes = copyW & ~1
            if dstW > copyW || uvDstH > uvCopyH {
                memset(dUV, 128, dBPR * uvDstH)
            }
            for r in 0..<uvCopyH {
                memcpy(dUV.advanced(by: r * dBPR), sUV.advanced(by: r * sBPR), uvCopyBytes)
            }
        }

        timestampStamper.stampBottomLeftLocked(pixelBuffer: dstPB, date: date)
        return dstPB
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
