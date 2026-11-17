import AppKit
import Foundation

// MARK: - Sound Sensitivity Checkpoints & Acoustic Wake Engine

struct SoundSensitivityCheckpoint {
    let percent: Double
    let title: String
    let examples: String

    var thresholdDB: Double {
        SoundDetector.thresholdDB(forPercent: percent)
    }

    static let all: [SoundSensitivityCheckpoint] = [
        SoundSensitivityCheckpoint(
            percent: 15.0,
            title: "Door Slam / Shout / Alarm",
            examples: "Only loud impacts, shouting, glass break, or smoke alarms"
        ),
        SoundSensitivityCheckpoint(
            percent: 35.0,
            title: "Loud Voice / Knock / Bark",
            examples: "Raised voice, firm door knock, dog bark, dropped object"
        ),
        SoundSensitivityCheckpoint(
            percent: 55.0,
            title: "Normal Talking Voice",
            examples: "Everyday conversation in room, phone/TV audio, door opening"
        ),
        SoundSensitivityCheckpoint(
            percent: 78.0,
            title: "Quiet Voice / Footsteps",
            examples: "Soft talking across room, footsteps, keys, doorknob turn"
        ),
        SoundSensitivityCheckpoint(
            percent: 95.0,
            title: "Whisper / Soft Rustle",
            examples: "Faint whisper, tiptoeing, fabric or paper rustle (quiet rooms)"
        )
    ]

    static func nearest(toPercent pct: Double) -> SoundSensitivityCheckpoint {
        let clamped = max(10.0, min(100.0, pct))
        var best = all[2]
        var bestDist = Double.greatestFiniteMagnitude
        for cp in all {
            let d = abs(cp.percent - clamped)
            if d < bestDist {
                bestDist = d
                best = cp
            }
        }
        return best
    }

    static func snapIfClose(_ rawPercent: Double, tolerance: Double = 1.6) -> Double {
        let clamped = max(10.0, min(100.0, rawPercent))
        for cp in all where abs(cp.percent - clamped) <= tolerance {
            return cp.percent
        }
        return clamped
    }
}

struct SoundEvaluation {
    let filteredDB: Float
    let thresholdDB: Float
    let triggered: Bool
}

final class SoundDetector {
    static let defaultSensitivityPercent: Double = 55.0 // -30.0 dB ("Normal Talking Voice")

    private var sensitivityPct: Double = SoundDetector.defaultSensitivityPercent
    private var threshDB: Double = SoundDetector.thresholdDB(forPercent: SoundDetector.defaultSensitivityPercent)
    private var enabled: Bool = false
    private var warmupUntil: TimeInterval = 0.0

    // 1st-order high-pass filter (~85 Hz @ 48 kHz: alpha = 1 / (1 + 2*pi*85/48000) ~= 0.9890)
    // Rejects sub-bass HVAC, fan rumble, and 50/60 Hz mains hum while preserving voices, knocks, and footsteps.
    private let hpfAlpha: Double = 0.9890
    private var prevIn: Double = 0.0
    private var prevOut: Double = 0.0

    // Fixed 50 ms acoustic analysis window (2400 samples @ 48 kHz)
    private var windowSqSum: Double = 0.0
    private var windowSampleCount: Int = 0
    private var recentWindowHits: [Bool] = [false, false, false]
    private var latestFilteredDB: Float = -60.0

    static func thresholdDB(forPercent percent: Double) -> Double {
        let clamped = max(10.0, min(100.0, percent))
        // 10% -> -10.0 dB, 55% -> -30.0 dB ("Normal Talking Voice"), 100% -> -50.0 dB
        return -10.0 - ((clamped - 10.0) / 90.0) * 40.0
    }

    static func meterFraction(forDB db: Double) -> Double {
        return max(0.0, min(1.0, (db + 60.0) / 60.0))
    }

    var sensitivityPercent: Double { sensitivityPct }
    var thresholdDB: Double { threshDB }
    var isEnabled: Bool { enabled }

    func setEnabled(_ active: Bool) {
        enabled = active
        if !active {
            recentWindowHits = [false, false, false]
        }
    }

    func setSensitivityPercent(_ percent: Double) {
        sensitivityPct = max(10.0, min(100.0, percent))
        threshDB = Self.thresholdDB(forPercent: sensitivityPct)
    }

    func reset(warmupUntilTimestamp: TimeInterval = 0.0) {
        prevIn = 0.0
        prevOut = 0.0
        windowSqSum = 0.0
        windowSampleCount = 0
        recentWindowHits = [false, false, false]
        latestFilteredDB = -60.0
        warmupUntil = warmupUntilTimestamp
    }

    func applyAcousticGrace(untilTimestamp: TimeInterval) {
        warmupUntil = max(warmupUntil, untilTimestamp)
        recentWindowHits = [false, false, false]
    }

    func analyzePCM16Mono(
        samples: UnsafePointer<Int16>,
        count: Int,
        sampleRate: Int = 48_000,
        nowTimestamp: TimeInterval
    ) -> SoundEvaluation {
        guard count > 0 else {
            return SoundEvaluation(filteredDB: latestFilteredDB, thresholdDB: Float(threshDB), triggered: false)
        }

        let windowTarget = max(240, sampleRate / 20) // 50 ms window
        var didTrigger = false
        var chunkPeakDB: Float = -60.0
        var completedWindowInCall = false

        for i in 0..<count {
            let x = Double(samples[i]) / 32768.0
            let y = hpfAlpha * (prevOut + x - prevIn)
            prevIn = x
            prevOut = y

            windowSqSum += y * y
            windowSampleCount += 1

            if windowSampleCount >= windowTarget {
                let rms = sqrt(windowSqSum / Double(windowSampleCount))
                let winDB: Double = rms > 0.00001 ? max(-60.0, min(0.0, 20.0 * log10(rms))) : -60.0
                latestFilteredDB = Float(winDB)
                if !completedWindowInCall || Float(winDB) > chunkPeakDB {
                    chunkPeakDB = Float(winDB)
                }
                completedWindowInCall = true

                let isHit = enabled && (nowTimestamp >= warmupUntil) && (winDB >= threshDB)
                recentWindowHits.removeFirst()
                recentWindowHits.append(isHit)

                let hitCount = (recentWindowHits[0] ? 1 : 0) + (recentWindowHits[1] ? 1 : 0) + (recentWindowHits[2] ? 1 : 0)
                if enabled && nowTimestamp >= warmupUntil && hitCount >= 2 {
                    didTrigger = true
                }

                windowSqSum = 0.0
                windowSampleCount = 0
            }
        }

        let reportedDB = completedWindowInCall ? chunkPeakDB : latestFilteredDB
        return SoundEvaluation(
            filteredDB: reportedDB,
            thresholdDB: Float(threshDB),
            triggered: didTrigger
        )
    }
}

// MARK: - Interactive Sound Sensitivity Drag Slider with Pop-Up Example Checkpoints

final class SoundTriggerSliderView: NSView {
    var sensitivityPercent: Double = SoundDetector.defaultSensitivityPercent {
        didSet {
            sensitivityPercent = max(10.0, min(100.0, sensitivityPercent))
            needsDisplay = true
        }
    }

    var isWakeOnSoundActive: Bool = false {
        didSet {
            needsDisplay = true
        }
    }

    private(set) var isDraggingThumb: Bool = false
    private(set) var isHoveringTrack: Bool = false
    private var hoverPercent: Double? = nil
    private var trackingAreaRef: NSTrackingArea?

    /// `(newSensitivityPercent, isDragging, shouldAutoEnableWakeOnSound)`
    var onValueChanged: ((Double, Bool, Bool) -> Void)?
    /// Notifies parent overlay to show/update/hide the unclipped pop-up checkpoint card
    var onPopupStateChanged: ((Bool, Double, Bool) -> Void)?

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    static let horizontalInset: CGFloat = 11.0

    func thumbXInBounds(forPercent pct: Double) -> CGFloat {
        let clamped = max(10.0, min(100.0, pct))
        let frac = CGFloat((clamped - 10.0) / 90.0)
        let usable = max(1.0, bounds.width - Self.horizontalInset * 2.0)
        return Self.horizontalInset + frac * usable
    }

    override func updateTrackingAreas() {
        super.updateTrackingAreas()
        if let old = trackingAreaRef {
            removeTrackingArea(old)
        }
        let area = NSTrackingArea(
            rect: bounds,
            options: [.mouseEnteredAndExited, .mouseMoved, .activeAlways, .inVisibleRect],
            owner: self,
            userInfo: nil
        )
        addTrackingArea(area)
        trackingAreaRef = area
    }

    override func mouseEntered(with event: NSEvent) {
        isHoveringTrack = true
        NSCursor.pointingHand.push()
        let raw = rawPercent(for: event)
        hoverPercent = SoundSensitivityCheckpoint.snapIfClose(raw, tolerance: 2.2)
        notifyPopup()
        needsDisplay = true
    }

    override func mouseMoved(with event: NSEvent) {
        guard !isDraggingThumb else { return }
        let raw = rawPercent(for: event)
        hoverPercent = SoundSensitivityCheckpoint.snapIfClose(raw, tolerance: 2.2)
        notifyPopup()
        needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        isHoveringTrack = false
        hoverPercent = nil
        NSCursor.pop()
        if !isDraggingThumb {
            notifyPopup()
        }
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        isDraggingThumb = true
        let snapped = SoundSensitivityCheckpoint.snapIfClose(rawPercent(for: event))
        sensitivityPercent = snapped
        onValueChanged?(snapped, true, true)
        notifyPopup()
        needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard isDraggingThumb else { return }
        let snapped = SoundSensitivityCheckpoint.snapIfClose(rawPercent(for: event))
        sensitivityPercent = snapped
        onValueChanged?(snapped, true, true)
        notifyPopup()
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        guard isDraggingThumb else { return }
        let snapped = SoundSensitivityCheckpoint.snapIfClose(rawPercent(for: event))
        isDraggingThumb = false
        sensitivityPercent = snapped
        onValueChanged?(snapped, false, true)
        notifyPopup()
        needsDisplay = true
    }

    private func rawPercent(for event: NSEvent) -> Double {
        let pt = convert(event.locationInWindow, from: nil)
        let usable = max(1.0, bounds.width - Self.horizontalInset * 2.0)
        let frac = Double(max(0.0, min(1.0, (pt.x - Self.horizontalInset) / usable)))
        return 10.0 + frac * 90.0
    }

    private func notifyPopup() {
        let visible = isDraggingThumb || isHoveringTrack
        let displayPct = isDraggingThumb ? sensitivityPercent : (hoverPercent ?? sensitivityPercent)
        onPopupStateChanged?(visible, displayPct, isDraggingThumb)
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }

        let w = bounds.width
        let h = bounds.height
        let inset = Self.horizontalInset
        let usable = max(1.0, w - inset * 2.0)
        let trackH: CGFloat = isDraggingThumb ? 8.0 : 6.5
        let cy = h * 0.5
        let trackY = cy - trackH * 0.5

        let frac = CGFloat(max(0.0, min(1.0, (sensitivityPercent - 10.0) / 90.0)))
        let thumbX = inset + frac * usable
        let activeAlpha: CGFloat = isWakeOnSoundActive ? 1.0 : 0.42

        // 1. Rail background
        let railRect = CGRect(x: inset, y: trackY, width: usable, height: trackH)
        let railPath = CGPath(roundedRect: railRect, cornerWidth: trackH * 0.5, cornerHeight: trackH * 0.5, transform: nil)
        ctx.addPath(railPath)
        ctx.setFillColor(NSColor(calibratedRed: 0.12, green: 0.15, blue: 0.21, alpha: 1.0).cgColor)
        ctx.fillPath()

        ctx.addPath(railPath)
        ctx.setStrokeColor(NSColor(calibratedWhite: 1.0, alpha: 0.14).cgColor)
        ctx.setLineWidth(1.0)
        ctx.strokePath()

        // 2. Active gradient fill
        if frac > 0.002 {
            let fillW = min(usable, max(trackH, frac * usable))
            let fillRect = CGRect(x: inset, y: trackY, width: fillW, height: trackH)
            let fillPath = CGPath(roundedRect: fillRect, cornerWidth: trackH * 0.5, cornerHeight: trackH * 0.5, transform: nil)
            ctx.saveGState()
            ctx.addPath(fillPath)
            ctx.clip()
            let colors = [
                NSColor(calibratedRed: 0.96, green: 0.65, blue: 0.16, alpha: activeAlpha).cgColor,
                NSColor(calibratedRed: 0.20, green: 0.80, blue: 0.96, alpha: activeAlpha).cgColor
            ] as CFArray
            if let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0.0, 1.0]) {
                ctx.drawLinearGradient(
                    grad,
                    start: CGPoint(x: inset, y: cy),
                    end: CGPoint(x: inset + usable, y: cy),
                    options: []
                )
            }
            ctx.restoreGState()
        }

        // 3. Checkpoint Marker Dots along the rail (5 acoustic example levels)
        let activeCP = SoundSensitivityCheckpoint.nearest(toPercent: isDraggingThumb ? sensitivityPercent : (hoverPercent ?? sensitivityPercent))
        for cp in SoundSensitivityCheckpoint.all {
            let cpFrac = CGFloat((cp.percent - 10.0) / 90.0)
            let cx = inset + cpFrac * usable
            let isNearest = abs(cp.percent - activeCP.percent) < 0.1
            let dotR: CGFloat = isNearest && (isDraggingThumb || isHoveringTrack) ? 4.4 : 3.2
            let dotRect = CGRect(x: cx - dotR, y: cy - dotR, width: dotR * 2, height: dotR * 2)

            if isNearest && (isDraggingThumb || isHoveringTrack) {
                ctx.setFillColor(NSColor(calibratedRed: 1.0, green: 0.84, blue: 0.28, alpha: 1.0).cgColor)
            } else if cp.percent <= sensitivityPercent && isWakeOnSoundActive {
                ctx.setFillColor(NSColor(calibratedWhite: 1.0, alpha: 0.88).cgColor)
            } else {
                ctx.setFillColor(NSColor(calibratedWhite: 0.65, alpha: 0.55).cgColor)
            }
            ctx.fillEllipse(in: dotRect)
        }

        // 4. Draggable Knob
        let knobR: CGFloat = isDraggingThumb ? 9.5 : (isHoveringTrack ? 8.8 : 8.0)
        if isDraggingThumb || isHoveringTrack {
            let haloR = knobR + 4.5
            ctx.setFillColor(NSColor(calibratedRed: 0.98, green: 0.76, blue: 0.24, alpha: isDraggingThumb ? 0.30 : 0.18).cgColor)
            ctx.fillEllipse(in: CGRect(x: thumbX - haloR, y: cy - haloR, width: haloR * 2, height: haloR * 2))
        }

        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -1.5), blur: 3.5, color: NSColor(calibratedWhite: 0, alpha: 0.6).cgColor)
        ctx.setFillColor(NSColor(calibratedWhite: 1.0, alpha: isWakeOnSoundActive ? 1.0 : 0.72).cgColor)
        ctx.fillEllipse(in: CGRect(x: thumbX - knobR, y: cy - knobR, width: knobR * 2, height: knobR * 2))
        ctx.restoreGState()

        let coreR: CGFloat = 3.3
        let coreColor = isWakeOnSoundActive
            ? NSColor(calibratedRed: 0.95, green: 0.60, blue: 0.12, alpha: 1.0)
            : NSColor(calibratedWhite: 0.45, alpha: 1.0)
        ctx.setFillColor(coreColor.cgColor)
        ctx.fillEllipse(in: CGRect(x: thumbX - coreR, y: cy - coreR, width: coreR * 2, height: coreR * 2))
    }
}

// MARK: - Unclipped Floating Checkpoint Pop-Up Callout Bubble

final class SoundCheckpointPopupView: NSView {
    var displayPercent: Double = SoundDetector.defaultSensitivityPercent {
        didSet { needsDisplay = true }
    }
    var isDragging: Bool = false {
        didSet { needsDisplay = true }
    }
    var isWakeOnSoundEnabled: Bool = true {
        didSet { needsDisplay = true }
    }
    /// X coordinate of the slider thumb mapped into this view's coordinate space
    var anchorX: CGFloat = 140.0 {
        didSet { needsDisplay = true }
    }

    override func hitTest(_ point: NSPoint) -> NSView? {
        // Purely informational pop-up bubble; never intercepts mouse clicks!
        return nil
    }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        isHidden = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }

        let cp = SoundSensitivityCheckpoint.nearest(toPercent: displayPercent)
        let db = SoundDetector.thresholdDB(forPercent: displayPercent)
        let exactMatch = abs(cp.percent - displayPercent) <= 2.0

        let headerStr = String(
            format: "%@ • %.0f%% (%.0f dB)%@",
            isDragging ? "🔊 SOUND WAKE LEVEL" : "🔊 CHECKPOINT PREVIEW",
            displayPercent,
            db,
            (!isWakeOnSoundEnabled && !isDragging) ? " • Click/Drag to Enable" : ""
        )
        let titleStr = exactMatch ? "“\(cp.title)”" : "≈ “\(cp.title)”"
        let exampleStr = "Picks up: \(cp.examples)"

        let headerAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: 10.5, weight: .bold),
            .foregroundColor: NSColor(calibratedRed: 0.99, green: 0.78, blue: 0.30, alpha: 1.0)
        ]
        let titleAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 13.0, weight: .bold),
            .foregroundColor: NSColor.white
        ]
        let exAttrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.systemFont(ofSize: 11.2, weight: .medium),
            .foregroundColor: NSColor(calibratedWhite: 0.86, alpha: 1.0)
        ]

        let hAttr = NSAttributedString(string: headerStr, attributes: headerAttrs)
        let tAttr = NSAttributedString(string: titleStr, attributes: titleAttrs)
        let eAttr = NSAttributedString(string: exampleStr, attributes: exAttrs)

        let cardW = min(bounds.width - 16.0, max(296.0, max(hAttr.size().width, max(tAttr.size().width, eAttr.size().width)) + 28.0))
        let cardH: CGFloat = 68.0
        let arrowH: CGFloat = 8.0
        let cardX = max(8.0, min(bounds.width - cardW - 8.0, anchorX - cardW * 0.5))
        let cardY: CGFloat = arrowH + 2.0

        let cardRect = CGRect(x: cardX, y: cardY, width: cardW, height: cardH)
        let arrowTipX = max(cardX + 18.0, min(cardX + cardW - 18.0, anchorX))

        let bubble = CGMutablePath()
        let r: CGFloat = 9.0
        bubble.move(to: CGPoint(x: cardRect.minX + r, y: cardRect.minY))
        bubble.addLine(to: CGPoint(x: arrowTipX - 7.5, y: cardRect.minY))
        bubble.addLine(to: CGPoint(x: arrowTipX, y: cardRect.minY - arrowH))
        bubble.addLine(to: CGPoint(x: arrowTipX + 7.5, y: cardRect.minY))
        bubble.addLine(to: CGPoint(x: cardRect.maxX - r, y: cardRect.minY))
        bubble.addArc(tangent1End: CGPoint(x: cardRect.maxX, y: cardRect.minY), tangent2End: CGPoint(x: cardRect.maxX, y: cardRect.minY + r), radius: r)
        bubble.addLine(to: CGPoint(x: cardRect.maxX, y: cardRect.maxY - r))
        bubble.addArc(tangent1End: CGPoint(x: cardRect.maxX, y: cardRect.maxY), tangent2End: CGPoint(x: cardRect.maxX - r, y: cardRect.maxY), radius: r)
        bubble.addLine(to: CGPoint(x: cardRect.minX + r, y: cardRect.maxY))
        bubble.addArc(tangent1End: CGPoint(x: cardRect.minX, y: cardRect.maxY), tangent2End: CGPoint(x: cardRect.minX, y: cardRect.maxY - r), radius: r)
        bubble.addLine(to: CGPoint(x: cardRect.minX, y: cardRect.minY + r))
        bubble.addArc(tangent1End: CGPoint(x: cardRect.minX, y: cardRect.minY), tangent2End: CGPoint(x: cardRect.minX + r, y: cardRect.minY), radius: r)
        bubble.closeSubpath()

        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -3), blur: 10.0, color: NSColor(calibratedWhite: 0, alpha: 0.68).cgColor)
        ctx.addPath(bubble)
        ctx.setFillColor(NSColor(calibratedRed: 0.06, green: 0.09, blue: 0.14, alpha: 0.97).cgColor)
        ctx.fillPath()
        ctx.restoreGState()

        ctx.addPath(bubble)
        ctx.setStrokeColor(NSColor(calibratedRed: 0.98, green: 0.75, blue: 0.25, alpha: isDragging ? 0.90 : 0.55).cgColor)
        ctx.setLineWidth(1.3)
        ctx.strokePath()

        hAttr.draw(at: NSPoint(x: cardX + 13.0, y: cardRect.maxY - 19.0))
        tAttr.draw(at: NSPoint(x: cardX + 13.0, y: cardRect.maxY - 39.0))
        eAttr.draw(at: NSPoint(x: cardX + 13.0, y: cardRect.minY + 8.0))
    }
}
