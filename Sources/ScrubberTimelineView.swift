import AppKit
import AVFoundation
import CoreMedia

// MARK: - Zero-Lag Frame-Accurate Scrub Chase Engine (Apple QA1820 Pattern)

final class ZeroLagScrubChaseEngine {
    private weak var player: AVPlayer?
    private var isSeekInProgress = false
    private var chaseTime: CMTime = .zero
    private var hasPendingChase = false

    init(player: AVPlayer) {
        self.player = player
    }

    func scrub(to seconds: Double) {
        guard let player = player, player.currentItem != nil else { return }
        let clamped = max(0.0, seconds)
        let target = CMTime(seconds: clamped, preferredTimescale: 600)
        chaseTime = target
        hasPendingChase = true

        if !isSeekInProgress {
            trySeekToChaseTime()
        }
    }

    func reset() {
        isSeekInProgress = false
        hasPendingChase = false
        chaseTime = .zero
    }

    private func trySeekToChaseTime() {
        guard let player = player,
              let item = player.currentItem,
              item.status == .readyToPlay,
              hasPendingChase else {
            isSeekInProgress = false
            return
        }

        isSeekInProgress = true
        hasPendingChase = false
        let target = chaseTime

        player.seek(to: target, toleranceBefore: .zero, toleranceAfter: .zero) { [weak self] _ in
            DispatchQueue.main.async {
                guard let self = self else { return }
                if self.hasPendingChase {
                    self.trySeekToChaseTime()
                } else {
                    self.isSeekInProgress = false
                }
            }
        }
    }
}

// MARK: - Prominent Fast-Drag Video Scrubber "Ball" View

final class ScrubberTimelineView: NSView {
    var durationSeconds: Double = 60.0 {
        didSet {
            durationSeconds = max(0.01, durationSeconds)
            needsDisplay = true
        }
    }

    var currentSeconds: Double = 0.0 {
        didSet {
            currentSeconds = max(0.0, min(durationSeconds, currentSeconds))
            needsDisplay = true
        }
    }

    private(set) var isDraggingBall: Bool = false
    private var isHovering: Bool = false
    private var trackingAreaRef: NSTrackingArea?

    /// Invoked continuously on mouseDown / mouseDragged / mouseUp: `(seconds, isDragging)`
    var onScrub: ((Double, Bool) -> Void)?

    override var acceptsFirstResponder: Bool { true }
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
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
        isHovering = true
        NSCursor.pointingHand.push()
        needsDisplay = true
    }

    override func mouseExited(with event: NSEvent) {
        isHovering = false
        NSCursor.pop()
        needsDisplay = true
    }

    override func mouseDown(with event: NSEvent) {
        isDraggingBall = true
        let sec = secondsForEvent(event)
        currentSeconds = sec
        onScrub?(sec, true)
        needsDisplay = true
    }

    override func mouseDragged(with event: NSEvent) {
        guard isDraggingBall else { return }
        let sec = secondsForEvent(event)
        currentSeconds = sec
        onScrub?(sec, true)
        needsDisplay = true
    }

    override func mouseUp(with event: NSEvent) {
        guard isDraggingBall else { return }
        let sec = secondsForEvent(event)
        isDraggingBall = false
        currentSeconds = sec
        onScrub?(sec, false)
        needsDisplay = true
    }

    private func trackHorizontalInsets() -> CGFloat {
        return 22.0
    }

    private func secondsForEvent(_ event: NSEvent) -> Double {
        let pt = convert(event.locationInWindow, from: nil)
        let inset = trackHorizontalInsets()
        let usableWidth = max(1.0, bounds.width - inset * 2.0)
        let frac = Double(max(0.0, min(1.0, (pt.x - inset) / usableWidth)))
        return frac * durationSeconds
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        guard let ctx = NSGraphicsContext.current?.cgContext else { return }

        let w = bounds.width
        let h = bounds.height
        let inset = trackHorizontalInsets()
        let usableWidth = max(1.0, w - inset * 2.0)
        let trackY = 16.0
        let trackH: CGFloat = isDraggingBall ? 10.0 : 8.0

        let frac = CGFloat(max(0.0, min(1.0, durationSeconds > 0 ? currentSeconds / durationSeconds : 0)))
        let ballX = inset + frac * usableWidth
        let ballY = trackY + trackH * 0.5

        // 1. Track Background Rail
        let railRect = CGRect(x: inset, y: trackY, width: usableWidth, height: trackH)
        let railPath = CGPath(roundedRect: railRect, cornerWidth: trackH * 0.5, cornerHeight: trackH * 0.5, transform: nil)
        ctx.addPath(railPath)
        ctx.setFillColor(NSColor(calibratedRed: 0.14, green: 0.17, blue: 0.23, alpha: 1.0).cgColor)
        ctx.fillPath()

        ctx.addPath(railPath)
        ctx.setStrokeColor(NSColor(calibratedWhite: 1.0, alpha: 0.12).cgColor)
        ctx.setLineWidth(1.0)
        ctx.strokePath()

        // Subtle second tick marks along the rail for visual speed reference while dragging
        let tickCount = min(30, max(4, Int(durationSeconds.rounded())))
        if tickCount >= 2 {
            ctx.setStrokeColor(NSColor(calibratedWhite: 1.0, alpha: 0.10).cgColor)
            ctx.setLineWidth(1.0)
            for i in 1..<tickCount {
                let tx = inset + usableWidth * (CGFloat(i) / CGFloat(tickCount))
                ctx.move(to: CGPoint(x: tx, y: trackY + 1.5))
                ctx.addLine(to: CGPoint(x: tx, y: trackY + trackH - 1.5))
            }
            ctx.strokePath()
        }

        // 2. Active Progress Gradient Rail
        let fillW = max(trackH, frac * usableWidth)
        if frac > 0.001 {
            let progRect = CGRect(x: inset, y: trackY, width: min(usableWidth, fillW), height: trackH)
            let progPath = CGPath(roundedRect: progRect, cornerWidth: trackH * 0.5, cornerHeight: trackH * 0.5, transform: nil)
            ctx.saveGState()
            ctx.addPath(progPath)
            ctx.clip()
            let colors = [
                NSColor(calibratedRed: 0.06, green: 0.78, blue: 0.66, alpha: 1.0).cgColor,
                NSColor(calibratedRed: 0.18, green: 0.68, blue: 0.98, alpha: 1.0).cgColor
            ] as CFArray
            if let grad = CGGradient(colorsSpace: CGColorSpaceCreateDeviceRGB(), colors: colors, locations: [0.0, 1.0]) {
                ctx.drawLinearGradient(
                    grad,
                    start: CGPoint(x: inset, y: ballY),
                    end: CGPoint(x: inset + usableWidth, y: ballY),
                    options: []
                )
            }
            ctx.restoreGState()
        }

        // 3. Prominent Glowing "Second Ball" Orb
        let ballRadius: CGFloat = isDraggingBall ? 13.5 : (isHovering ? 12.0 : 11.0)

        // Outer halo when hovering or dragging
        if isDraggingBall || isHovering {
            let haloR = ballRadius + (isDraggingBall ? 7.0 : 4.5)
            let haloRect = CGRect(x: ballX - haloR, y: ballY - haloR, width: haloR * 2, height: haloR * 2)
            ctx.setFillColor(NSColor(calibratedRed: 0.16, green: 0.78, blue: 0.98, alpha: isDraggingBall ? 0.30 : 0.18).cgColor)
            ctx.fillEllipse(in: haloRect)
        }

        // Crisp drop shadow + luminous white/cyan orb
        let ballRect = CGRect(x: ballX - ballRadius, y: ballY - ballRadius, width: ballRadius * 2, height: ballRadius * 2)
        ctx.saveGState()
        ctx.setShadow(offset: CGSize(width: 0, height: -2), blur: 5.0, color: NSColor(calibratedWhite: 0, alpha: 0.65).cgColor)
        ctx.setFillColor(NSColor.white.cgColor)
        ctx.fillEllipse(in: ballRect)
        ctx.restoreGState()

        // Inner cyan core inside the ball
        let coreR: CGFloat = isDraggingBall ? 5.2 : 4.2
        let coreRect = CGRect(x: ballX - coreR, y: ballY - coreR, width: coreR * 2, height: coreR * 2)
        ctx.setFillColor(NSColor(calibratedRed: 0.06, green: 0.62, blue: 0.92, alpha: 1.0).cgColor)
        ctx.fillEllipse(in: coreRect)

        // 4. Floating Live Seconds Pill right above the Ball!
        let badgeText = String(format: "%.1fs", currentSeconds)
        let attrs: [NSAttributedString.Key: Any] = [
            .font: NSFont.monospacedDigitSystemFont(ofSize: isDraggingBall ? 11.5 : 10.5, weight: .bold),
            .foregroundColor: isDraggingBall
                ? NSColor(calibratedRed: 0.04, green: 0.08, blue: 0.14, alpha: 1.0)
                : NSColor(calibratedWhite: 0.95, alpha: 1.0)
        ]
        let attrStr = NSAttributedString(string: badgeText, attributes: attrs)
        let textSize = attrStr.size()
        let pillW = textSize.width + 12.0
        let pillH: CGFloat = 18.0
        let clampedPillX = max(4.0, min(w - pillW - 4.0, ballX - pillW * 0.5))
        let pillY = min(h - pillH - 1.0, ballY + ballRadius + 5.0)

        let pillRect = CGRect(x: clampedPillX, y: pillY, width: pillW, height: pillH)
        let pillPath = CGPath(roundedRect: pillRect, cornerWidth: 6.0, cornerHeight: 6.0, transform: nil)
        ctx.addPath(pillPath)
        if isDraggingBall {
            ctx.setFillColor(NSColor(calibratedRed: 0.32, green: 0.88, blue: 0.99, alpha: 0.98).cgColor)
        } else {
            ctx.setFillColor(NSColor(calibratedRed: 0.14, green: 0.20, blue: 0.29, alpha: 0.94).cgColor)
        }
        ctx.fillPath()

        attrStr.draw(at: NSPoint(x: clampedPillX + 6.0, y: pillY + (pillH - textSize.height) * 0.5))
    }
}
