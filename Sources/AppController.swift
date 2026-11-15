import AppKit
import AVFoundation
import AVKit
import CoreGraphics
import CoreMedia
import CoreVideo
import Darwin
import UniformTypeIdentifiers

// MARK: - Custom Studio Views

final class CameraPreviewView: NSView {
    let previewLayer = AVCaptureVideoPreviewLayer()

    override init(frame frameRect: NSRect) {
        super.init(frame: frameRect)
        wantsLayer = true
        layer?.backgroundColor = NSColor(calibratedRed: 0.03, green: 0.04, blue: 0.06, alpha: 1.0).cgColor
        layer?.cornerRadius = 12
        layer?.masksToBounds = true
        layer?.borderWidth = 1.0
        layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.10).cgColor

        previewLayer.videoGravity = .resizeAspect
        previewLayer.backgroundColor = NSColor.black.cgColor
        layer?.addSublayer(previewLayer)
    }

    required init?(coder: NSCoder) {
        fatalError("init(coder:) has not been implemented")
    }

    override func layout() {
        super.layout()
        CATransaction.begin()
        CATransaction.setDisableActions(true)
        previewLayer.frame = bounds
        CATransaction.commit()
    }
}

final class StudioMeterBarView: NSView {
    var fraction: Double = 0.0 {
        didSet {
            fraction = max(0.0, min(1.0, fraction))
            needsDisplay = true
        }
    }
    var thresholdFraction: Double? = nil {
        didSet { needsDisplay = true }
    }
    var barColor: NSColor = .systemGreen {
        didSet { needsDisplay = true }
    }

    override func draw(_ dirtyRect: NSRect) {
        super.draw(dirtyRect)
        let b = bounds
        let r: CGFloat = b.height * 0.5

        NSColor(calibratedRed: 0.11, green: 0.14, blue: 0.19, alpha: 1.0).setFill()
        let bg = NSBezierPath(roundedRect: b, xRadius: r, yRadius: r)
        bg.fill()

        let fillW = CGFloat(fraction) * b.width
        if fillW > 1.0 {
            let fillRect = NSRect(x: 0, y: 0, width: min(b.width, max(b.height, fillW)), height: b.height)
            barColor.setFill()
            NSBezierPath(roundedRect: fillRect, xRadius: r, yRadius: r).fill()
        }

        if let thresh = thresholdFraction {
            let xPos = max(2.0, min(b.width - 2.0, CGFloat(thresh) * b.width))
            NSColor.systemYellow.setStroke()
            let marker = NSBezierPath()
            marker.lineWidth = 2.5
            marker.move(to: NSPoint(x: xPos, y: 0))
            marker.line(to: NSPoint(x: xPos, y: b.height))
            marker.stroke()
        }

        NSColor(calibratedWhite: 1.0, alpha: 0.12).setStroke()
        bg.lineWidth = 1.0
        bg.stroke()
    }
}

enum MFLogoRenderer {
    /// Renders a crisp vector "MF" icon for the UI header badge and macOS Menu Bar status item.
    static func makeStatusImage(pointSize: CGFloat = 18, stateDotColor: NSColor? = nil) -> NSImage {
        let width = pointSize * 1.48
        let height = pointSize
        let img = NSImage(size: NSSize(width: width, height: height), flipped: false) { rect in
            guard let ctx = NSGraphicsContext.current?.cgContext else { return false }
            ctx.setAllowsAntialiasing(true)
            ctx.setShouldAntialias(true)

            let pillRect = rect.insetBy(dx: 0.5, dy: 1.0)
            let pillPath = CGPath(roundedRect: pillRect, cornerWidth: 4.0, cornerHeight: 4.0, transform: nil)
            ctx.addPath(pillPath)
            ctx.setFillColor(NSColor(calibratedRed: 0.08, green: 0.11, blue: 0.17, alpha: 0.96).cgColor)
            ctx.fillPath()

            ctx.addPath(pillPath)
            ctx.setStrokeColor(NSColor(calibratedWhite: 1.0, alpha: 0.28).cgColor)
            ctx.setLineWidth(0.9)
            ctx.strokePath()

            let padX = pillRect.width * 0.14
            let padY = pillRect.height * 0.21
            let leftX = pillRect.minX + padX
            let rightX = pillRect.maxX - padX - (stateDotColor != nil ? 4.5 : 0.0)
            let botY = pillRect.minY + padY
            let topY = pillRect.maxY - padY
            let capH = topY - botY
            let totalW = rightX - leftX
            let gap = totalW * 0.08
            let mW = totalW * 0.53
            let fW = totalW - mW - gap
            let stroke = capH * 0.20

            let p = CGMutablePath()
            let mMid = leftX + mW * 0.5
            p.move(to: CGPoint(x: leftX, y: botY))
            p.addLine(to: CGPoint(x: leftX, y: topY))
            p.addLine(to: CGPoint(x: leftX + stroke, y: topY))
            p.addLine(to: CGPoint(x: mMid, y: botY + capH * 0.44))
            p.addLine(to: CGPoint(x: leftX + mW - stroke, y: topY))
            p.addLine(to: CGPoint(x: leftX + mW, y: topY))
            p.addLine(to: CGPoint(x: leftX + mW, y: botY))
            p.addLine(to: CGPoint(x: leftX + mW - stroke * 0.94, y: botY))
            p.addLine(to: CGPoint(x: leftX + mW - stroke * 0.94, y: topY - stroke * 1.4))
            p.addLine(to: CGPoint(x: mMid + stroke * 0.2, y: botY + capH * 0.24))
            p.addLine(to: CGPoint(x: mMid - stroke * 0.2, y: botY + capH * 0.24))
            p.addLine(to: CGPoint(x: leftX + stroke * 0.94, y: topY - stroke * 1.4))
            p.addLine(to: CGPoint(x: leftX + stroke * 0.94, y: botY))
            p.closeSubpath()

            let fL = leftX + mW + gap
            let fR = fL + fW
            let midTop = botY + capH * 0.56
            let midBot = midTop - stroke * 0.90
            p.move(to: CGPoint(x: fL, y: botY))
            p.addLine(to: CGPoint(x: fL, y: topY))
            p.addLine(to: CGPoint(x: fR, y: topY))
            p.addLine(to: CGPoint(x: fR, y: topY - stroke))
            p.addLine(to: CGPoint(x: fL + stroke, y: topY - stroke))
            p.addLine(to: CGPoint(x: fL + stroke, y: midTop))
            p.addLine(to: CGPoint(x: fL + fW * 0.88, y: midTop))
            p.addLine(to: CGPoint(x: fL + fW * 0.88, y: midBot))
            p.addLine(to: CGPoint(x: fL + stroke, y: midBot))
            p.addLine(to: CGPoint(x: fL + stroke, y: botY))
            p.closeSubpath()

            ctx.addPath(p)
            ctx.setFillColor(NSColor.white.cgColor)
            ctx.fillPath()

            if let dot = stateDotColor {
                let dR: CGFloat = 2.6
                let dRect = CGRect(x: pillRect.maxX - dR * 2 - 2.4, y: pillRect.maxY - dR * 2 - 2.4, width: dR * 2, height: dR * 2)
                ctx.setFillColor(dot.cgColor)
                ctx.fillEllipse(in: dRect)
            }
            return true
        }
        img.isTemplate = false
        return img
    }
}

// MARK: - Main Application Controller

final class AppController: NSObject,
    NSApplicationDelegate,
    NSWindowDelegate,
    AVCaptureVideoDataOutputSampleBufferDelegate,
    AVCaptureAudioDataOutputSampleBufferDelegate,
    NSTableViewDataSource,
    NSTableViewDelegate {

    // Core Managers
    let vault: VaultManager
    let sleepAndLock = SleepAndLockManager()

    // Capture Pipeline
    let captureSession = AVCaptureSession()
    private let sessionQueue = DispatchQueue(label: "app.c60.maxwatch.sessionQueue", qos: .userInitiated)
    private let mediaSampleQueue = DispatchQueue(label: "app.c60.maxwatch.mediaSampleQueue", qos: .userInitiated)
    private let videoDataOutput = AVCaptureVideoDataOutput()
    private let audioDataOutput = AVCaptureAudioDataOutput()

    private static let recordAudioPrefsKey = "MaxWatch.RecordAudioEnabled"

    private var videoDevices: [AVCaptureDevice] = []
    private var audioDevices: [AVCaptureDevice] = []
    private var activeVideoInput: AVCaptureDeviceInput?
    private var activeAudioInput: AVCaptureDeviceInput?
    private var activeWidth: Int = 1920
    private var activeHeight: Int = 1080
    private var activeAudioActive: Bool = false
    private var isAudioRecordingEnabled: Bool = true
    private var sampleQueueAudioEnabled: Bool = true
    private var needsSessionRefreshAfterClip: Bool = false
    private var isPlayerAudioMuted: Bool = false
    private var activeCameraSummary: String = "1080p HD"

    // Motion Detection State (accessed on mediaSampleQueue)
    private var prevLumaGrid: [UInt8] = []
    private var prevMeanLuma: Double = 0.0
    private var frameCounter: UInt64 = 0
    private var consecutiveMotionHits: Int = 0
    private var armedWarmupUntil: Date = .distantPast
    private var activeSegmentWriter: VaultSegmentWriter?

    // Shared Recorder State (Main Thread)
    private var isArmed: Bool = false
    private var isRecordingClip: Bool = false
    private var recordingStartedAt: Date?
    private var recordingStopDeadline: Date?
    private let minimumClipDuration: TimeInterval = 60.0

    // Sensitivity Slider (10% = Heavy Motion 10.0% delta, 100% = Ultra Sensitive 0.25% delta; default 78% -> ~1.5% delta)
    private var sensitivityPercent: Double = 78.0
    private var motionThresholdPercent: Double = 1.5
    private var latestMotionPercent: Double = 0.0
    private var latestAudioDB: Float = -60.0

    // Vault Player State
    private let player = AVPlayer()
    private lazy var scrubEngine = ZeroLagScrubChaseEngine(player: player)
    private var activeClipReader: VaultClipReader?
    private var activeResourceLoader: VaultAssetResourceLoader?
    private var timeObserverToken: Any?
    private var clipEndObserver: NSObjectProtocol?
    private var unlockedClips: [EncryptedClipSummary] = []
    private var currentClipIndex: Int = -1
    private var currentClipDuration: Double = 1.0
    private var selectedPlaybackSpeed: Float = 1.0
    private var wasPlayingBeforeDrag: Bool = false
    private var isInPlayerMode: Bool = false
    private var isPlayAllSequentialActive: Bool = false

    // Signal Sources for Exit-Safe Recording
    private var sigtermSource: DispatchSourceSignal?
    private var sigintSource: DispatchSourceSignal?
    private var isTerminatingApp = false

    // Window, Menu Bar & UI Elements
    var window: NSWindow!
    private var statusItem: NSStatusItem?
    private var menuArmItem: NSMenuItem?
    private var menuRecordAudioItem: NSMenuItem?
    private var statusMenuRecordAudioItem: NSMenuItem?
    private var uiTimer: Timer?

    // Header UI
    private var armButton: NSButton!
    private var playAllTopButton: NSButton!
    private var vaultLockButton: NSButton!
    private var modeSwitchButton: NSButton!
    private var statusPillLabel: NSTextField!

    // Stage Containers
    private var stageContainer: NSView!
    private var recorderStageView: NSView!
    private var playerStageView: NSView!

    // Recorder Stage UI
    private var previewView: CameraPreviewView!
    private var hudPrimaryLabel: NSTextField!
    private var hudRecordingBadge: NSTextField!
    private var hudBottomLeftTimestampLabel: NSTextField!
    private var permissionRecoveryButton: NSButton!
    private var motionMeterView: StudioMeterBarView!
    private var motionReadoutLabel: NSTextField!
    private var sensitivitySlider: NSSlider!
    private var sensitivityReadoutLabel: NSTextField!
    private var audioMeterView: StudioMeterBarView!
    private var audioReadoutLabel: NSTextField!
    private var cameraPopup: NSPopUpButton!
    private var micPopup: NSPopUpButton!
    private var recordAudioSegmentedControl: NSSegmentedControl!

    // Player Stage UI
    private var lockedOverlayCard: NSView!
    private var unlockedPlayerContent: NSView!
    private var nowPlayingTitleLabel: NSTextField!
    private var nowPlayingMetaLabel: NSTextField!
    private var playerView: AVPlayerView!
    private var scrubberBallView: ScrubberTimelineView!
    private var playerTimeReadoutLabel: NSTextField!
    private var speedSegmentedControl: NSSegmentedControl!
    private var playAllPlayerButton: NSButton!
    private var prevClipButton: NSButton!
    private var playPauseButton: NSButton!
    private var nextClipButton: NSButton!
    private var saveAndNextButton: NSButton!
    private var deleteAndNextButton: NSButton!
    private var playerAudioToggleButton: NSButton!
    private var chooseSaveFolderButton: NSButton!
    private var exportClipButton: NSButton!

    // User-chosen external folder on Mac (outside the Vault) for one-click "Save & Next"
    private var chosenSaveDirectoryURL: URL?
    private var hasSecurityScopedSaveDir: Bool = false
    private var lastVaultInteractionAt: Date = Date()

    // Sidebar UI
    private var sidebarHeaderLabel: NSTextField!
    private var sidebarVaultBadgeLabel: NSTextField!
    private var clipsScrollView: NSScrollView!
    private var clipsTableView: NSTableView!
    private var lockedSidebarPlaceholder: NSView!
    private var lockedSidebarCountLabel: NSTextField!
    private var changePasswordButton: NSButton!

    init(customVaultDirectory: URL? = nil) {
        self.vault = VaultManager(customVaultDirectory: customVaultDirectory)
        let savedAudioPref = UserDefaults.standard.object(forKey: Self.recordAudioPrefsKey) as? Bool ?? true
        self.isAudioRecordingEnabled = savedAudioPref
        self.sampleQueueAudioEnabled = savedAudioPref
        super.init()
        recomputeThresholdFromSensitivity()
    }

    // MARK: - App Lifecycle & Signal / Power-Off Safety

    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.appearance = NSAppearance(named: .darkAqua)

        installSignalAndLockHandlers()
        buildMainMenu()
        buildStatusBarItem()
        buildMainWindow()
        setupPlayerObservers()
        refreshVaultUIState()
        requestPermissionsAndStartCapture()

        let timer = Timer.scheduledTimer(
            timeInterval: 0.10,
            target: self,
            selector: #selector(onUITimerTick),
            userInfo: nil,
            repeats: true
        )
        RunLoop.main.add(timer, forMode: .common)
        self.uiTimer = timer

        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        if !flag {
            window.makeKeyAndOrderFront(nil)
        }
        return true
    }

    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool {
        // Keep running in Dock & MF menu bar if armed/recording; closing window never loses a recording.
        return false
    }

    func applicationDidHide(_ notification: Notification) {
        if vault.isUnlocked {
            lockVaultImmediately(keepCurrentStageIfCamera: false)
        }
    }

    func windowWillClose(_ notification: Notification) {
        if vault.isUnlocked {
            lockVaultImmediately(keepCurrentStageIfCamera: false)
        }
    }

    func windowDidMiniaturize(_ notification: Notification) {
        if vault.isUnlocked {
            lockVaultImmediately(keepCurrentStageIfCamera: false)
        }
    }

    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        isTerminatingApp = true
        sleepAndLock.setArmedPreventingIdleSleep(false)
        releaseChosenSaveDirectoryScope()

        if let writer = detachActiveWriterSynchronously() {
            // Flush sub-second tail frames, encrypt & F_FULLFSYNC before quitting!
            DispatchQueue.global(qos: .userInitiated).async { [weak self] in
                writer.stopAndFinalizeSynchronously(timeoutSeconds: 2.5)
                DispatchQueue.main.async {
                    self?.stopCaptureSessionCleanly()
                    self?.vault.lock()
                    NSApp.reply(toApplicationShouldTerminate: true)
                }
            }
            return .terminateLater
        }

        stopCaptureSessionCleanly()
        vault.lock()
        return .terminateNow
    }

    private func installSignalAndLockHandlers() {
        // Auto-lock the encrypted vault whenever the Mac screen locks, while keeping motion capture active!
        sleepAndLock.onScreenLocked = { [weak self] in
            guard let self = self else { return }
            if self.vault.isUnlocked {
                self.lockVaultImmediately(keepCurrentStageIfCamera: true)
            }
            self.updateHeaderStatusTexts()
        }
        sleepAndLock.onScreenUnlocked = { [weak self] in
            self?.updateHeaderStatusTexts()
        }
        sleepAndLock.onSystemWillPowerOff = { [weak self] in
            guard let self = self else { return }
            if let writer = self.detachActiveWriterSynchronously() {
                writer.stopAndFinalizeSynchronously(timeoutSeconds: 2.0)
            }
            self.vault.lock()
        }

        // Catch SIGTERM / SIGINT so terminal kill or logout flushes active recording cleanly
        Darwin.signal(SIGTERM, SIG_IGN)
        Darwin.signal(SIGINT, SIG_IGN)

        let termSrc = DispatchSource.makeSignalSource(signal: SIGTERM, queue: .main)
        termSrc.setEventHandler {
            NSApp.terminate(nil)
        }
        termSrc.resume()
        self.sigtermSource = termSrc

        let intSrc = DispatchSource.makeSignalSource(signal: SIGINT, queue: .main)
        intSrc.setEventHandler {
            NSApp.terminate(nil)
        }
        intSrc.resume()
        self.sigintSource = intSrc
    }

    private func stopCaptureSessionCleanly() {
        sessionQueue.sync {
            if captureSession.isRunning {
                captureSession.stopRunning()
            }
        }
    }

    // MARK: - macOS Main Menu & "MF" Menu Bar Item

    private func buildMainMenu() {
        let mainMenu = NSMenu()

        // 1. App Menu
        let appItem = NSMenuItem()
        mainMenu.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "About Max Watch", action: #selector(showAboutPanel), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Set or Change Vault Password...", action: #selector(changePasswordClicked), keyEquivalent: ",")
        appMenu.addItem(withTitle: "Lock Video Vault", action: #selector(lockVaultAction), keyEquivalent: "l")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Hide Max Watch", action: #selector(NSApplication.hide(_:)), keyEquivalent: "h")
        let hideOthers = appMenu.addItem(withTitle: "Hide Others", action: #selector(NSApplication.hideOtherApplications(_:)), keyEquivalent: "h")
        hideOthers.keyEquivalentModifierMask = [.command, .option]
        appMenu.addItem(withTitle: "Show All", action: #selector(NSApplication.unhideAllApplications(_:)), keyEquivalent: "")
        appMenu.addItem(.separator())
        appMenu.addItem(withTitle: "Quit Max Watch", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu

        // 2. File Menu
        let fileItem = NSMenuItem()
        mainMenu.addItem(fileItem)
        let fileMenu = NSMenu(title: "File")
        fileMenu.addItem(withTitle: "Show Main Window", action: #selector(showMainWindowAction), keyEquivalent: "0")
        fileMenu.addItem(withTitle: "Start or Stop Motion Detection", action: #selector(toggleArmedClicked), keyEquivalent: "r")
        let recAudioMenuItem = fileMenu.addItem(withTitle: "Record Microphone Audio", action: #selector(toggleRecordAudioMenuAction), keyEquivalent: "A")
        recAudioMenuItem.state = isAudioRecordingEnabled ? .on : .off
        self.menuRecordAudioItem = recAudioMenuItem
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Save & Next (to Chosen Folder)", action: #selector(saveAndNextClicked), keyEquivalent: "s")
        fileMenu.addItem(withTitle: "Choose Save & Next Folder...", action: #selector(chooseSaveFolderClicked), keyEquivalent: "S")
        fileMenu.addItem(withTitle: "Save Current Clip As MP4...", action: #selector(exportCurrentClipClicked), keyEquivalent: "e")
        fileMenu.addItem(.separator())
        fileMenu.addItem(withTitle: "Close Window", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        fileItem.submenu = fileMenu

        // 3. Edit Menu (Enables Cmd+X/C/V/A in Password Sheets)
        let editItem = NSMenuItem()
        mainMenu.addItem(editItem)
        let editMenu = NSMenu(title: "Edit")
        editMenu.addItem(withTitle: "Undo", action: Selector(("undo:")), keyEquivalent: "z")
        editMenu.addItem(withTitle: "Redo", action: Selector(("redo:")), keyEquivalent: "Z")
        editMenu.addItem(.separator())
        editMenu.addItem(withTitle: "Cut", action: #selector(NSText.cut(_:)), keyEquivalent: "x")
        editMenu.addItem(withTitle: "Copy", action: #selector(NSText.copy(_:)), keyEquivalent: "c")
        editMenu.addItem(withTitle: "Paste", action: #selector(NSText.paste(_:)), keyEquivalent: "v")
        editMenu.addItem(withTitle: "Select All", action: #selector(NSText.selectAll(_:)), keyEquivalent: "a")
        editItem.submenu = editMenu

        // 4. Playback Menu
        let playItem = NSMenuItem()
        mainMenu.addItem(playItem)
        let playMenu = NSMenu(title: "Playback")
        playMenu.addItem(withTitle: "Play All Vault Clips", action: #selector(playAllClicked), keyEquivalent: "p")
        let pauseItem = playMenu.addItem(withTitle: "Play / Pause", action: #selector(togglePlayPauseClicked), keyEquivalent: " ")
        pauseItem.keyEquivalentModifierMask = []
        playMenu.addItem(.separator())
        playMenu.addItem(withTitle: "Normal Speed (1x)", action: #selector(selectSpeed1x), keyEquivalent: "1")
        playMenu.addItem(withTitle: "Fast Speed (4x)", action: #selector(selectSpeed4x), keyEquivalent: "2")
        playMenu.addItem(withTitle: "Super Fast Speed (12x)", action: #selector(selectSpeed12x), keyEquivalent: "3")
        playMenu.addItem(withTitle: "Toggle Playback Audio", action: #selector(togglePlayerMuteClicked), keyEquivalent: "M")
        playMenu.addItem(.separator())
        playMenu.addItem(withTitle: "Previous Clip", action: #selector(prevClipClicked), keyEquivalent: "[")
        playMenu.addItem(withTitle: "Next Clip", action: #selector(nextClipClicked), keyEquivalent: "]")
        playMenu.addItem(withTitle: "Save & Next", action: #selector(saveAndNextClicked), keyEquivalent: "")
        let delItem = playMenu.addItem(withTitle: "Delete & Next", action: #selector(deleteAndNextClicked), keyEquivalent: String(UnicodeScalar(NSBackspaceCharacter)!))
        delItem.keyEquivalentModifierMask = [.command]
        playItem.submenu = playMenu

        // 5. Window Menu
        let winItem = NSMenuItem()
        mainMenu.addItem(winItem)
        let winMenu = NSMenu(title: "Window")
        winMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        winMenu.addItem(withTitle: "Zoom", action: #selector(NSWindow.performZoom(_:)), keyEquivalent: "")
        winMenu.addItem(.separator())
        winMenu.addItem(withTitle: "Bring All to Front", action: #selector(NSApplication.arrangeInFront(_:)), keyEquivalent: "")
        winItem.submenu = winMenu
        NSApp.windowsMenu = winMenu

        NSApp.mainMenu = mainMenu
    }

    private func buildStatusBarItem() {
        let item = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        item.button?.image = MFLogoRenderer.makeStatusImage(pointSize: 18, stateDotColor: nil)
        item.button?.toolTip = "Max Watch - Free Offline Motion Security"

        let menu = NSMenu()
        menu.addItem(withTitle: "Open Max Watch", action: #selector(showMainWindowAction), keyEquivalent: "")
        menu.addItem(.separator())
        let armItem = menu.addItem(withTitle: "Start Motion Detection", action: #selector(toggleArmedClicked), keyEquivalent: "")
        self.menuArmItem = armItem
        let statusAudioItem = menu.addItem(withTitle: "Record Microphone Audio", action: #selector(toggleRecordAudioMenuAction), keyEquivalent: "")
        statusAudioItem.state = isAudioRecordingEnabled ? .on : .off
        self.statusMenuRecordAudioItem = statusAudioItem
        menu.addItem(withTitle: "Play All (Password Protected)", action: #selector(playAllClicked), keyEquivalent: "")
        menu.addItem(withTitle: "Lock Video Vault", action: #selector(lockVaultAction), keyEquivalent: "")
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit Max Watch", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        for m in menu.items where m.action != #selector(NSApplication.terminate(_:)) {
            m.target = self
        }
        item.menu = menu
        self.statusItem = item
    }

    private func refreshStatusBarIcon() {
        let dot: NSColor? = isRecordingClip ? .systemRed : (isArmed ? .systemGreen : nil)
        statusItem?.button?.image = MFLogoRenderer.makeStatusImage(pointSize: 18, stateDotColor: dot)
        menuArmItem?.title = isArmed ? "Stop Motion Detection" : "Start Motion Detection"
    }

    // MARK: - Window & Studio Layout Construction

    private func makeLabel(_ text: String, size: CGFloat = 13, weight: NSFont.Weight = .regular, color: NSColor = .labelColor, monoDigits: Bool = false) -> NSTextField {
        let label = NSTextField(labelWithString: text)
        label.font = monoDigits
            ? NSFont.monospacedDigitSystemFont(ofSize: size, weight: weight)
            : NSFont.systemFont(ofSize: size, weight: weight)
        label.textColor = color
        label.lineBreakMode = .byTruncatingTail
        label.translatesAutoresizingMaskIntoConstraints = false
        return label
    }

    private func makeButton(title: String, action: Selector, tint: NSColor? = nil) -> NSButton {
        let btn = NSButton(title: title, target: self, action: action)
        btn.bezelStyle = .rounded
        btn.controlSize = .regular
        btn.font = NSFont.systemFont(ofSize: 12.5, weight: .semibold)
        if let t = tint {
            btn.contentTintColor = t
        }
        btn.translatesAutoresizingMaskIntoConstraints = false
        return btn
    }

    private func buildMainWindow() {
        let rect = NSRect(x: 0, y: 0, width: 1300, height: 800)
        window = NSWindow(
            contentRect: rect,
            styleMask: [.titled, .closable, .miniaturizable, .resizable],
            backing: .buffered,
            defer: false
        )
        window.title = "Max Watch - Offline Motion Security & Encrypted Video Vault"
        window.minSize = NSSize(width: 1150, height: 680)
        window.backgroundColor = NSColor(calibratedRed: 0.05, green: 0.06, blue: 0.09, alpha: 1.0)
        window.center()
        window.delegate = self

        let root = NSView(frame: rect)
        root.wantsLayer = true
        root.layer?.backgroundColor = NSColor(calibratedRed: 0.05, green: 0.06, blue: 0.09, alpha: 1.0).cgColor
        window.contentView = root

        // 1. Top Studio Header Bar
        let topBar = NSView()
        topBar.wantsLayer = true
        topBar.layer?.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.10, blue: 0.15, alpha: 1.0).cgColor
        topBar.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(topBar)

        let topDivider = NSView()
        topDivider.wantsLayer = true
        topDivider.layer?.backgroundColor = NSColor(calibratedWhite: 1.0, alpha: 0.09).cgColor
        topDivider.translatesAutoresizingMaskIntoConstraints = false
        topBar.addSubview(topDivider)

        let logoImageView = NSImageView(image: MFLogoRenderer.makeStatusImage(pointSize: 28, stateDotColor: nil))
        logoImageView.translatesAutoresizingMaskIntoConstraints = false
        topBar.addSubview(logoImageView)

        let brandTitle = makeLabel("Max Watch", size: 16, weight: .bold, color: .white)
        topBar.addSubview(brandTitle)

        let brandSub = makeLabel("Free 100% offline security • Gift from Max Freedom Pollard", size: 11, weight: .medium, color: NSColor(calibratedWhite: 0.68, alpha: 1.0))
        topBar.addSubview(brandSub)

        armButton = makeButton(title: "● Start Motion Detection", action: #selector(toggleArmedClicked), tint: .systemGreen)
        topBar.addSubview(armButton)

        playAllTopButton = makeButton(title: "🔒 Play All (0)", action: #selector(playAllClicked), tint: .systemCyan)
        topBar.addSubview(playAllTopButton)

        modeSwitchButton = makeButton(title: "Vault Player", action: #selector(toggleStageModeClicked))
        topBar.addSubview(modeSwitchButton)

        vaultLockButton = makeButton(title: "🔑 Set Vault Password", action: #selector(vaultLockButtonClicked))
        topBar.addSubview(vaultLockButton)

        statusPillLabel = makeLabel("100% OFFLINE • READY", size: 11.5, weight: .bold, color: .systemTeal, monoDigits: true)
        statusPillLabel.alignment = .right
        topBar.addSubview(statusPillLabel)

        NSLayoutConstraint.activate([
            topBar.topAnchor.constraint(equalTo: root.topAnchor),
            topBar.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            topBar.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            topBar.heightAnchor.constraint(equalToConstant: 62),

            topDivider.leadingAnchor.constraint(equalTo: topBar.leadingAnchor),
            topDivider.trailingAnchor.constraint(equalTo: topBar.trailingAnchor),
            topDivider.bottomAnchor.constraint(equalTo: topBar.bottomAnchor),
            topDivider.heightAnchor.constraint(equalToConstant: 1),

            logoImageView.leadingAnchor.constraint(equalTo: topBar.leadingAnchor, constant: 16),
            logoImageView.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),

            brandTitle.leadingAnchor.constraint(equalTo: logoImageView.trailingAnchor, constant: 10),
            brandTitle.topAnchor.constraint(equalTo: topBar.topAnchor, constant: 12),

            brandSub.leadingAnchor.constraint(equalTo: brandTitle.leadingAnchor),
            brandSub.topAnchor.constraint(equalTo: brandTitle.bottomAnchor, constant: 1),

            armButton.leadingAnchor.constraint(equalTo: brandSub.trailingAnchor, constant: 20),
            armButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            armButton.heightAnchor.constraint(equalToConstant: 34),

            playAllTopButton.leadingAnchor.constraint(equalTo: armButton.trailingAnchor, constant: 10),
            playAllTopButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            playAllTopButton.heightAnchor.constraint(equalToConstant: 34),

            modeSwitchButton.leadingAnchor.constraint(equalTo: playAllTopButton.trailingAnchor, constant: 10),
            modeSwitchButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            modeSwitchButton.heightAnchor.constraint(equalToConstant: 34),

            vaultLockButton.leadingAnchor.constraint(equalTo: modeSwitchButton.trailingAnchor, constant: 10),
            vaultLockButton.centerYAnchor.constraint(equalTo: topBar.centerYAnchor),
            vaultLockButton.heightAnchor.constraint(equalToConstant: 34),

            statusPillLabel.leadingAnchor.constraint(greaterThanOrEqualTo: vaultLockButton.trailingAnchor, constant: 12),
            statusPillLabel.trailingAnchor.constraint(equalTo: topBar.trailingAnchor, constant: -18),
            statusPillLabel.centerYAnchor.constraint(equalTo: topBar.centerYAnchor)
        ])

        // 2. Right Encrypted Vault Sidebar
        let sidebar = NSView()
        sidebar.wantsLayer = true
        sidebar.layer?.backgroundColor = NSColor(calibratedRed: 0.07, green: 0.08, blue: 0.12, alpha: 1.0).cgColor
        sidebar.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(sidebar)

        let sideBorder = NSView()
        sideBorder.wantsLayer = true
        sideBorder.layer?.backgroundColor = NSColor(calibratedWhite: 1.0, alpha: 0.08).cgColor
        sideBorder.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addSubview(sideBorder)

        sidebarHeaderLabel = makeLabel("Encrypted Video Vault", size: 14, weight: .bold, color: .white)
        sidebar.addSubview(sidebarHeaderLabel)

        sidebarVaultBadgeLabel = makeLabel("AES-256-GCM • Zero Plaintext on Disk", size: 11, weight: .medium, color: NSColor(calibratedRed: 0.35, green: 0.85, blue: 0.78, alpha: 1.0))
        sidebar.addSubview(sidebarVaultBadgeLabel)

        clipsScrollView = NSScrollView()
        clipsScrollView.hasVerticalScroller = true
        clipsScrollView.drawsBackground = false
        clipsScrollView.borderType = .noBorder
        clipsScrollView.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addSubview(clipsScrollView)

        clipsTableView = NSTableView()
        clipsTableView.headerView = nil
        clipsTableView.rowHeight = 52
        clipsTableView.backgroundColor = NSColor(calibratedRed: 0.09, green: 0.11, blue: 0.16, alpha: 1.0)
        clipsTableView.selectionHighlightStyle = .regular
        clipsTableView.delegate = self
        clipsTableView.dataSource = self
        clipsTableView.target = self
        clipsTableView.action = #selector(onVaultClipRowClicked)

        let clipCol = NSTableColumn(identifier: NSUserInterfaceItemIdentifier("VaultClipCol"))
        clipCol.width = 276
        clipsTableView.addTableColumn(clipCol)
        clipsScrollView.documentView = clipsTableView

        // Locked placeholder card inside sidebar
        lockedSidebarPlaceholder = NSView()
        lockedSidebarPlaceholder.wantsLayer = true
        lockedSidebarPlaceholder.layer?.backgroundColor = NSColor(calibratedRed: 0.10, green: 0.12, blue: 0.18, alpha: 1.0).cgColor
        lockedSidebarPlaceholder.layer?.cornerRadius = 10
        lockedSidebarPlaceholder.layer?.borderWidth = 1
        lockedSidebarPlaceholder.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.10).cgColor
        lockedSidebarPlaceholder.translatesAutoresizingMaskIntoConstraints = false
        sidebar.addSubview(lockedSidebarPlaceholder)

        let lockTitle = makeLabel("🔒 Vault Locked", size: 14, weight: .bold, color: .white)
        lockTitle.alignment = .center
        lockedSidebarPlaceholder.addSubview(lockTitle)

        lockedSidebarCountLabel = makeLabel("0 encrypted clips stored safely offline.", size: 12, weight: .medium, color: NSColor(calibratedWhite: 0.78, alpha: 1.0))
        lockedSidebarCountLabel.alignment = .center
        lockedSidebarCountLabel.maximumNumberOfLines = 3
        lockedSidebarCountLabel.lineBreakMode = .byWordWrapping
        lockedSidebarPlaceholder.addSubview(lockedSidebarCountLabel)

        let sidebarUnlockBtn = makeButton(title: "Unlock Vault / Play All", action: #selector(playAllClicked), tint: .systemCyan)
        lockedSidebarPlaceholder.addSubview(sidebarUnlockBtn)

        let lockNote = makeLabel("New motion clips encrypt automatically even while locked or when your Mac screen is locked.", size: 11, weight: .regular, color: NSColor(calibratedWhite: 0.62, alpha: 1.0))
        lockNote.alignment = .center
        lockNote.maximumNumberOfLines = 4
        lockNote.lineBreakMode = .byWordWrapping
        lockedSidebarPlaceholder.addSubview(lockNote)

        changePasswordButton = makeButton(title: "Vault Password Settings...", action: #selector(changePasswordClicked))
        sidebar.addSubview(changePasswordButton)

        NSLayoutConstraint.activate([
            sidebar.topAnchor.constraint(equalTo: topBar.bottomAnchor),
            sidebar.trailingAnchor.constraint(equalTo: root.trailingAnchor),
            sidebar.bottomAnchor.constraint(equalTo: root.bottomAnchor),
            sidebar.widthAnchor.constraint(equalToConstant: 306),

            sideBorder.topAnchor.constraint(equalTo: sidebar.topAnchor),
            sideBorder.bottomAnchor.constraint(equalTo: sidebar.bottomAnchor),
            sideBorder.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor),
            sideBorder.widthAnchor.constraint(equalToConstant: 1),

            sidebarHeaderLabel.topAnchor.constraint(equalTo: sidebar.topAnchor, constant: 14),
            sidebarHeaderLabel.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor, constant: 14),
            sidebarHeaderLabel.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: -14),

            sidebarVaultBadgeLabel.topAnchor.constraint(equalTo: sidebarHeaderLabel.bottomAnchor, constant: 3),
            sidebarVaultBadgeLabel.leadingAnchor.constraint(equalTo: sidebarHeaderLabel.leadingAnchor),
            sidebarVaultBadgeLabel.trailingAnchor.constraint(equalTo: sidebarHeaderLabel.trailingAnchor),

            clipsScrollView.topAnchor.constraint(equalTo: sidebarVaultBadgeLabel.bottomAnchor, constant: 12),
            clipsScrollView.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor, constant: 12),
            clipsScrollView.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: -12),
            clipsScrollView.bottomAnchor.constraint(equalTo: changePasswordButton.topAnchor, constant: -12),

            lockedSidebarPlaceholder.topAnchor.constraint(equalTo: sidebarVaultBadgeLabel.bottomAnchor, constant: 14),
            lockedSidebarPlaceholder.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor, constant: 14),
            lockedSidebarPlaceholder.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: -14),
            lockedSidebarPlaceholder.heightAnchor.constraint(equalToConstant: 210),

            lockTitle.topAnchor.constraint(equalTo: lockedSidebarPlaceholder.topAnchor, constant: 22),
            lockTitle.centerXAnchor.constraint(equalTo: lockedSidebarPlaceholder.centerXAnchor),

            lockedSidebarCountLabel.topAnchor.constraint(equalTo: lockTitle.bottomAnchor, constant: 10),
            lockedSidebarCountLabel.leadingAnchor.constraint(equalTo: lockedSidebarPlaceholder.leadingAnchor, constant: 14),
            lockedSidebarCountLabel.trailingAnchor.constraint(equalTo: lockedSidebarPlaceholder.trailingAnchor, constant: -14),

            sidebarUnlockBtn.topAnchor.constraint(equalTo: lockedSidebarCountLabel.bottomAnchor, constant: 14),
            sidebarUnlockBtn.centerXAnchor.constraint(equalTo: lockedSidebarPlaceholder.centerXAnchor),
            sidebarUnlockBtn.heightAnchor.constraint(equalToConstant: 32),

            lockNote.topAnchor.constraint(equalTo: sidebarUnlockBtn.bottomAnchor, constant: 14),
            lockNote.leadingAnchor.constraint(equalTo: lockedSidebarPlaceholder.leadingAnchor, constant: 14),
            lockNote.trailingAnchor.constraint(equalTo: lockedSidebarPlaceholder.trailingAnchor, constant: -14),

            changePasswordButton.leadingAnchor.constraint(equalTo: sidebar.leadingAnchor, constant: 14),
            changePasswordButton.trailingAnchor.constraint(equalTo: sidebar.trailingAnchor, constant: -14),
            changePasswordButton.bottomAnchor.constraint(equalTo: sidebar.bottomAnchor, constant: -14),
            changePasswordButton.heightAnchor.constraint(equalToConstant: 30)
        ])

        // 3. Main Stage Container
        stageContainer = NSView()
        stageContainer.translatesAutoresizingMaskIntoConstraints = false
        root.addSubview(stageContainer)

        NSLayoutConstraint.activate([
            stageContainer.topAnchor.constraint(equalTo: topBar.bottomAnchor),
            stageContainer.leadingAnchor.constraint(equalTo: root.leadingAnchor),
            stageContainer.trailingAnchor.constraint(equalTo: sidebar.leadingAnchor),
            stageContainer.bottomAnchor.constraint(equalTo: root.bottomAnchor)
        ])

        buildRecorderStage()
        buildPlayerStage()
        setStageMode(playerMode: false)
    }

    private func buildRecorderStage() {
        recorderStageView = NSView()
        recorderStageView.translatesAutoresizingMaskIntoConstraints = false
        stageContainer.addSubview(recorderStageView)

        previewView = CameraPreviewView()
        previewView.previewLayer.session = captureSession
        previewView.translatesAutoresizingMaskIntoConstraints = false
        recorderStageView.addSubview(previewView)

        // HUD Top-Left Banner
        let hudBox = NSView()
        hudBox.wantsLayer = true
        hudBox.layer?.backgroundColor = NSColor(calibratedRed: 0.04, green: 0.06, blue: 0.10, alpha: 0.80).cgColor
        hudBox.layer?.cornerRadius = 8
        hudBox.layer?.borderWidth = 1
        hudBox.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.14).cgColor
        hudBox.translatesAutoresizingMaskIntoConstraints = false
        recorderStageView.addSubview(hudBox)

        hudPrimaryLabel = makeLabel("LIVE MONITOR • Initializing camera & microphone...", size: 12.5, weight: .semibold, color: .white)
        hudBox.addSubview(hudPrimaryLabel)

        // HUD Top-Right Recording Badge
        let recBox = NSView()
        recBox.wantsLayer = true
        recBox.layer?.backgroundColor = NSColor(calibratedRed: 0.72, green: 0.08, blue: 0.12, alpha: 0.92).cgColor
        recBox.layer?.cornerRadius = 8
        recBox.translatesAutoresizingMaskIntoConstraints = false
        recorderStageView.addSubview(recBox)

        hudRecordingBadge = makeLabel("● REC 00:00 • 1s Power-Cut-Safe Vault Sync", size: 12.5, weight: .bold, color: .white, monoDigits: true)
        recBox.addSubview(hudRecordingBadge)
        recBox.isHidden = true

        // HUD Bottom-Left Live Unambiguous Computer-Time Stamp (matches burned-in clip bottom-left timestamp)
        let tsBox = NSView()
        tsBox.wantsLayer = true
        tsBox.layer?.backgroundColor = NSColor(calibratedRed: 0.04, green: 0.06, blue: 0.10, alpha: 0.74).cgColor
        tsBox.layer?.cornerRadius = 6
        tsBox.layer?.borderWidth = 1
        tsBox.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.16).cgColor
        tsBox.translatesAutoresizingMaskIntoConstraints = false
        recorderStageView.addSubview(tsBox)

        hudBottomLeftTimestampLabel = makeLabel(ClipTimestampFormatter.format(), size: 11.5, weight: .semibold, color: NSColor(calibratedWhite: 0.96, alpha: 1.0), monoDigits: true)
        tsBox.addSubview(hudBottomLeftTimestampLabel)

        permissionRecoveryButton = makeButton(title: "Open System Camera & Mic Settings", action: #selector(openPrivacySettingsClicked), tint: .systemYellow)
        permissionRecoveryButton.isHidden = true
        recorderStageView.addSubview(permissionRecoveryButton)

        // Bottom Studio Deck
        let deck = NSView()
        deck.wantsLayer = true
        deck.layer?.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.10, blue: 0.15, alpha: 1.0).cgColor
        deck.layer?.cornerRadius = 12
        deck.layer?.borderWidth = 1
        deck.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.09).cgColor
        deck.translatesAutoresizingMaskIntoConstraints = false
        recorderStageView.addSubview(deck)

        let motionTitle = makeLabel("Live Motion:", size: 12.5, weight: .semibold, color: .white)
        deck.addSubview(motionTitle)

        motionMeterView = StudioMeterBarView()
        motionMeterView.barColor = .systemGreen
        motionMeterView.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(motionMeterView)

        motionReadoutLabel = makeLabel("0.0% (Trigger: 1.5%)", size: 12, weight: .medium, color: NSColor(calibratedWhite: 0.82, alpha: 1.0), monoDigits: true)
        deck.addSubview(motionReadoutLabel)

        let sensTitle = makeLabel("Motion Detect Scale:", size: 12.5, weight: .semibold, color: .white)
        deck.addSubview(sensTitle)

        sensitivitySlider = NSSlider(value: sensitivityPercent, minValue: 10.0, maxValue: 100.0, target: self, action: #selector(onSensitivitySliderChanged(_:)))
        sensitivitySlider.isContinuous = true
        sensitivitySlider.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(sensitivitySlider)

        sensitivityReadoutLabel = makeLabel("Sensitivity: 78% (Balanced - 1.5% delta)", size: 12, weight: .medium, color: .systemCyan, monoDigits: true)
        deck.addSubview(sensitivityReadoutLabel)

        let micTitle = makeLabel("Mic Audio:", size: 12.5, weight: .semibold, color: .white)
        deck.addSubview(micTitle)

        audioMeterView = StudioMeterBarView()
        audioMeterView.barColor = .systemTeal
        audioMeterView.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(audioMeterView)

        audioReadoutLabel = makeLabel("48 kHz AAC Ready", size: 12, weight: .medium, color: NSColor(calibratedWhite: 0.80, alpha: 1.0), monoDigits: true)
        deck.addSubview(audioReadoutLabel)

        cameraPopup = NSPopUpButton(frame: .zero, pullsDown: false)
        cameraPopup.font = NSFont.systemFont(ofSize: 11.5, weight: .medium)
        cameraPopup.target = self
        cameraPopup.action = #selector(onCameraPopupChanged(_:))
        cameraPopup.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(cameraPopup)

        micPopup = NSPopUpButton(frame: .zero, pullsDown: false)
        micPopup.font = NSFont.systemFont(ofSize: 11.5, weight: .medium)
        micPopup.target = self
        micPopup.action = #selector(onMicPopupChanged(_:))
        micPopup.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(micPopup)

        recordAudioSegmentedControl = NSSegmentedControl(
            labels: ["🔊 Audio ON", "🔇 Audio OFF"],
            trackingMode: .selectOne,
            target: self,
            action: #selector(onRecordAudioSegmentChanged(_:))
        )
        recordAudioSegmentedControl.selectedSegment = isAudioRecordingEnabled ? 0 : 1
        recordAudioSegmentedControl.segmentStyle = .rounded
        recordAudioSegmentedControl.font = NSFont.systemFont(ofSize: 11.5, weight: .semibold)
        recordAudioSegmentedControl.translatesAutoresizingMaskIntoConstraints = false
        deck.addSubview(recordAudioSegmentedControl)

        let footerInfo = makeLabel(
            "Locked-Screen Ready: Press Ctrl+Cmd+Q to lock your Mac while Max Watch keeps recording. Saves every 1s against power loss & flushes cleanly on exit.",
            size: 11.5,
            weight: .medium,
            color: NSColor(calibratedRed: 0.96, green: 0.82, blue: 0.38, alpha: 0.95)
        )
        deck.addSubview(footerInfo)

        NSLayoutConstraint.activate([
            recorderStageView.topAnchor.constraint(equalTo: stageContainer.topAnchor),
            recorderStageView.leadingAnchor.constraint(equalTo: stageContainer.leadingAnchor),
            recorderStageView.trailingAnchor.constraint(equalTo: stageContainer.trailingAnchor),
            recorderStageView.bottomAnchor.constraint(equalTo: stageContainer.bottomAnchor),

            previewView.topAnchor.constraint(equalTo: recorderStageView.topAnchor, constant: 14),
            previewView.leadingAnchor.constraint(equalTo: recorderStageView.leadingAnchor, constant: 14),
            previewView.trailingAnchor.constraint(equalTo: recorderStageView.trailingAnchor, constant: -14),
            previewView.bottomAnchor.constraint(equalTo: deck.topAnchor, constant: -12),

            hudBox.topAnchor.constraint(equalTo: previewView.topAnchor, constant: 12),
            hudBox.leadingAnchor.constraint(equalTo: previewView.leadingAnchor, constant: 12),
            hudBox.trailingAnchor.constraint(lessThanOrEqualTo: previewView.trailingAnchor, constant: -240),

            hudPrimaryLabel.topAnchor.constraint(equalTo: hudBox.topAnchor, constant: 7),
            hudPrimaryLabel.bottomAnchor.constraint(equalTo: hudBox.bottomAnchor, constant: -7),
            hudPrimaryLabel.leadingAnchor.constraint(equalTo: hudBox.leadingAnchor, constant: 12),
            hudPrimaryLabel.trailingAnchor.constraint(equalTo: hudBox.trailingAnchor, constant: -12),

            recBox.topAnchor.constraint(equalTo: previewView.topAnchor, constant: 12),
            recBox.trailingAnchor.constraint(equalTo: previewView.trailingAnchor, constant: -12),

            hudRecordingBadge.topAnchor.constraint(equalTo: recBox.topAnchor, constant: 7),
            hudRecordingBadge.bottomAnchor.constraint(equalTo: recBox.bottomAnchor, constant: -7),
            hudRecordingBadge.leadingAnchor.constraint(equalTo: recBox.leadingAnchor, constant: 12),
            hudRecordingBadge.trailingAnchor.constraint(equalTo: recBox.trailingAnchor, constant: -12),

            tsBox.leadingAnchor.constraint(equalTo: previewView.leadingAnchor, constant: 12),
            tsBox.bottomAnchor.constraint(equalTo: previewView.bottomAnchor, constant: -10),
            tsBox.trailingAnchor.constraint(lessThanOrEqualTo: previewView.trailingAnchor, constant: -12),

            hudBottomLeftTimestampLabel.topAnchor.constraint(equalTo: tsBox.topAnchor, constant: 4),
            hudBottomLeftTimestampLabel.bottomAnchor.constraint(equalTo: tsBox.bottomAnchor, constant: -4),
            hudBottomLeftTimestampLabel.leadingAnchor.constraint(equalTo: tsBox.leadingAnchor, constant: 9),
            hudBottomLeftTimestampLabel.trailingAnchor.constraint(equalTo: tsBox.trailingAnchor, constant: -9),

            permissionRecoveryButton.centerXAnchor.constraint(equalTo: previewView.centerXAnchor),
            permissionRecoveryButton.centerYAnchor.constraint(equalTo: previewView.centerYAnchor),
            permissionRecoveryButton.heightAnchor.constraint(equalToConstant: 36),

            deck.leadingAnchor.constraint(equalTo: recorderStageView.leadingAnchor, constant: 14),
            deck.trailingAnchor.constraint(equalTo: recorderStageView.trailingAnchor, constant: -14),
            deck.bottomAnchor.constraint(equalTo: recorderStageView.bottomAnchor, constant: -14),
            deck.heightAnchor.constraint(equalToConstant: 148),

            motionTitle.topAnchor.constraint(equalTo: deck.topAnchor, constant: 14),
            motionTitle.leadingAnchor.constraint(equalTo: deck.leadingAnchor, constant: 16),
            motionTitle.widthAnchor.constraint(equalToConstant: 140),

            motionMeterView.centerYAnchor.constraint(equalTo: motionTitle.centerYAnchor),
            motionMeterView.leadingAnchor.constraint(equalTo: motionTitle.trailingAnchor, constant: 8),
            motionMeterView.widthAnchor.constraint(equalToConstant: 260),
            motionMeterView.heightAnchor.constraint(equalToConstant: 14),

            motionReadoutLabel.centerYAnchor.constraint(equalTo: motionTitle.centerYAnchor),
            motionReadoutLabel.leadingAnchor.constraint(equalTo: motionMeterView.trailingAnchor, constant: 12),
            motionReadoutLabel.trailingAnchor.constraint(lessThanOrEqualTo: cameraPopup.leadingAnchor, constant: -10),

            cameraPopup.centerYAnchor.constraint(equalTo: motionTitle.centerYAnchor),
            cameraPopup.trailingAnchor.constraint(equalTo: deck.trailingAnchor, constant: -16),
            cameraPopup.widthAnchor.constraint(equalToConstant: 210),

            sensTitle.topAnchor.constraint(equalTo: motionTitle.bottomAnchor, constant: 14),
            sensTitle.leadingAnchor.constraint(equalTo: deck.leadingAnchor, constant: 16),
            sensTitle.widthAnchor.constraint(equalToConstant: 140),

            sensitivitySlider.centerYAnchor.constraint(equalTo: sensTitle.centerYAnchor),
            sensitivitySlider.leadingAnchor.constraint(equalTo: sensTitle.trailingAnchor, constant: 8),
            sensitivitySlider.widthAnchor.constraint(equalToConstant: 260),

            sensitivityReadoutLabel.centerYAnchor.constraint(equalTo: sensTitle.centerYAnchor),
            sensitivityReadoutLabel.leadingAnchor.constraint(equalTo: sensitivitySlider.trailingAnchor, constant: 12),
            sensitivityReadoutLabel.trailingAnchor.constraint(lessThanOrEqualTo: micPopup.leadingAnchor, constant: -10),

            micPopup.centerYAnchor.constraint(equalTo: sensTitle.centerYAnchor),
            micPopup.trailingAnchor.constraint(equalTo: deck.trailingAnchor, constant: -16),
            micPopup.widthAnchor.constraint(equalToConstant: 210),

            micTitle.topAnchor.constraint(equalTo: sensTitle.bottomAnchor, constant: 14),
            micTitle.leadingAnchor.constraint(equalTo: deck.leadingAnchor, constant: 16),
            micTitle.widthAnchor.constraint(equalToConstant: 140),

            audioMeterView.centerYAnchor.constraint(equalTo: micTitle.centerYAnchor),
            audioMeterView.leadingAnchor.constraint(equalTo: micTitle.trailingAnchor, constant: 8),
            audioMeterView.widthAnchor.constraint(equalToConstant: 260),
            audioMeterView.heightAnchor.constraint(equalToConstant: 14),

            audioReadoutLabel.centerYAnchor.constraint(equalTo: micTitle.centerYAnchor),
            audioReadoutLabel.leadingAnchor.constraint(equalTo: audioMeterView.trailingAnchor, constant: 12),
            audioReadoutLabel.trailingAnchor.constraint(lessThanOrEqualTo: recordAudioSegmentedControl.leadingAnchor, constant: -10),

            recordAudioSegmentedControl.centerYAnchor.constraint(equalTo: micTitle.centerYAnchor),
            recordAudioSegmentedControl.trailingAnchor.constraint(equalTo: deck.trailingAnchor, constant: -16),
            recordAudioSegmentedControl.widthAnchor.constraint(equalToConstant: 210),

            footerInfo.topAnchor.constraint(equalTo: micTitle.bottomAnchor, constant: 13),
            footerInfo.leadingAnchor.constraint(equalTo: deck.leadingAnchor, constant: 16),
            footerInfo.trailingAnchor.constraint(equalTo: deck.trailingAnchor, constant: -16)
        ])

        recomputeThresholdFromSensitivity()
    }

    private func buildPlayerStage() {
        playerStageView = NSView()
        playerStageView.translatesAutoresizingMaskIntoConstraints = false
        stageContainer.addSubview(playerStageView)

        // A. Unlocked Vault Player Container
        unlockedPlayerContent = NSView()
        unlockedPlayerContent.translatesAutoresizingMaskIntoConstraints = false
        playerStageView.addSubview(unlockedPlayerContent)

        let playerHeader = NSView()
        playerHeader.wantsLayer = true
        playerHeader.layer?.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.11, blue: 0.16, alpha: 1.0).cgColor
        playerHeader.layer?.cornerRadius = 10
        playerHeader.translatesAutoresizingMaskIntoConstraints = false
        unlockedPlayerContent.addSubview(playerHeader)

        nowPlayingTitleLabel = makeLabel("Encrypted Vault Player - Select a clip or click Play All", size: 13.5, weight: .bold, color: .white)
        playerHeader.addSubview(nowPlayingTitleLabel)

        nowPlayingMetaLabel = makeLabel("Decrypted on-the-fly in RAM • Zero temporary files written to disk", size: 11.5, weight: .regular, color: NSColor(calibratedWhite: 0.70, alpha: 1.0))
        playerHeader.addSubview(nowPlayingMetaLabel)

        playerAudioToggleButton = makeButton(title: "🔊 Audio: ON", action: #selector(togglePlayerMuteClicked), tint: .systemTeal)
        playerHeader.addSubview(playerAudioToggleButton)

        chooseSaveFolderButton = makeButton(title: "📁 Save Folder: Choose...", action: #selector(chooseSaveFolderClicked))
        playerHeader.addSubview(chooseSaveFolderButton)

        exportClipButton = makeButton(title: "Save As...", action: #selector(exportCurrentClipClicked))
        playerHeader.addSubview(exportClipButton)

        let backBtn = makeButton(title: "🔒 Lock & Live Camera", action: #selector(switchToLiveCameraClicked), tint: .systemGreen)
        playerHeader.addSubview(backBtn)

        let videoWrap = NSView()
        videoWrap.wantsLayer = true
        videoWrap.layer?.backgroundColor = NSColor.black.cgColor
        videoWrap.layer?.cornerRadius = 12
        videoWrap.layer?.masksToBounds = true
        videoWrap.layer?.borderWidth = 1
        videoWrap.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.10).cgColor
        videoWrap.translatesAutoresizingMaskIntoConstraints = false
        unlockedPlayerContent.addSubview(videoWrap)

        playerView = AVPlayerView()
        playerView.player = player
        playerView.controlsStyle = .none
        playerView.videoGravity = .resizeAspect
        playerView.translatesAutoresizingMaskIntoConstraints = false
        videoWrap.addSubview(playerView)

        let controlsCard = NSView()
        controlsCard.wantsLayer = true
        controlsCard.layer?.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.10, blue: 0.15, alpha: 1.0).cgColor
        controlsCard.layer?.cornerRadius = 12
        controlsCard.layer?.borderWidth = 1
        controlsCard.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.09).cgColor
        controlsCard.translatesAutoresizingMaskIntoConstraints = false
        unlockedPlayerContent.addSubview(controlsCard)

        let scrubHint = makeLabel("Drag Ball to Scrub Fast:", size: 12, weight: .semibold, color: NSColor(calibratedWhite: 0.88, alpha: 1.0))
        controlsCard.addSubview(scrubHint)

        scrubberBallView = ScrubberTimelineView()
        scrubberBallView.translatesAutoresizingMaskIntoConstraints = false
        scrubberBallView.onScrub = { [weak self] seconds, dragging in
            self?.handleUserBallScrub(seconds: seconds, isDragging: dragging)
        }
        controlsCard.addSubview(scrubberBallView)

        playerTimeReadoutLabel = makeLabel("00:00.0 / 00:00.0", size: 12.5, weight: .bold, color: .systemCyan, monoDigits: true)
        playerTimeReadoutLabel.alignment = .right
        controlsCard.addSubview(playerTimeReadoutLabel)

        // Transport + Save & Next + Delete & Next + Speed Row
        playAllPlayerButton = makeButton(title: "▶ Play All", action: #selector(playAllClicked), tint: .systemCyan)
        controlsCard.addSubview(playAllPlayerButton)

        prevClipButton = makeButton(title: "⏮ Prev", action: #selector(prevClipClicked))
        controlsCard.addSubview(prevClipButton)

        playPauseButton = makeButton(title: "⏯ Play/Pause", action: #selector(togglePlayPauseClicked))
        controlsCard.addSubview(playPauseButton)

        nextClipButton = makeButton(title: "Next ⏭", action: #selector(nextClipClicked))
        controlsCard.addSubview(nextClipButton)

        saveAndNextButton = makeButton(title: "💾 Save & Next", action: #selector(saveAndNextClicked), tint: .systemGreen)
        controlsCard.addSubview(saveAndNextButton)

        deleteAndNextButton = makeButton(title: "🗑 Delete & Next", action: #selector(deleteAndNextClicked), tint: .systemRed)
        controlsCard.addSubview(deleteAndNextButton)

        let speedLabel = makeLabel("Speed:", size: 12, weight: .semibold, color: NSColor(calibratedWhite: 0.82, alpha: 1.0))
        controlsCard.addSubview(speedLabel)

        speedSegmentedControl = NSSegmentedControl(labels: ["Normal (1x)", "4x", "12x"], trackingMode: .selectOne, target: self, action: #selector(onSpeedSegmentChanged(_:)))
        speedSegmentedControl.selectedSegment = 0
        speedSegmentedControl.segmentStyle = .rounded
        speedSegmentedControl.translatesAutoresizingMaskIntoConstraints = false
        controlsCard.addSubview(speedSegmentedControl)

        // B. Locked Shield Overlay Card
        lockedOverlayCard = NSView()
        lockedOverlayCard.wantsLayer = true
        lockedOverlayCard.layer?.backgroundColor = NSColor(calibratedRed: 0.08, green: 0.10, blue: 0.15, alpha: 1.0).cgColor
        lockedOverlayCard.layer?.cornerRadius = 14
        lockedOverlayCard.layer?.borderWidth = 1
        lockedOverlayCard.layer?.borderColor = NSColor(calibratedWhite: 1.0, alpha: 0.12).cgColor
        lockedOverlayCard.translatesAutoresizingMaskIntoConstraints = false
        playerStageView.addSubview(lockedOverlayCard)

        let shieldTitle = makeLabel("🔒 Password-Protected Video Vault", size: 20, weight: .bold, color: .white)
        lockedOverlayCard.addSubview(shieldTitle)

        let shieldBody = makeLabel(
            "All recordings are encrypted on the fly with AES-256-GCM. Unlock with your password to browse clips, drag the fast-scrub ball, or run Play All at 1x, 4x, or 12x speed.",
            size: 13,
            weight: .regular,
            color: NSColor(calibratedWhite: 0.78, alpha: 1.0)
        )
        shieldBody.alignment = .center
        shieldBody.maximumNumberOfLines = 4
        shieldBody.lineBreakMode = .byWordWrapping
        lockedOverlayCard.addSubview(shieldBody)

        let unlockBigBtn = makeButton(title: "🔑 Enter Password & Play All", action: #selector(playAllClicked), tint: .systemCyan)
        lockedOverlayCard.addSubview(unlockBigBtn)

        let returnCamBtn = makeButton(title: "Back to Live Camera", action: #selector(switchToLiveCameraClicked))
        lockedOverlayCard.addSubview(returnCamBtn)

        NSLayoutConstraint.activate([
            playerStageView.topAnchor.constraint(equalTo: stageContainer.topAnchor),
            playerStageView.leadingAnchor.constraint(equalTo: stageContainer.leadingAnchor),
            playerStageView.trailingAnchor.constraint(equalTo: stageContainer.trailingAnchor),
            playerStageView.bottomAnchor.constraint(equalTo: stageContainer.bottomAnchor),

            unlockedPlayerContent.topAnchor.constraint(equalTo: playerStageView.topAnchor),
            unlockedPlayerContent.leadingAnchor.constraint(equalTo: playerStageView.leadingAnchor),
            unlockedPlayerContent.trailingAnchor.constraint(equalTo: playerStageView.trailingAnchor),
            unlockedPlayerContent.bottomAnchor.constraint(equalTo: playerStageView.bottomAnchor),

            playerHeader.topAnchor.constraint(equalTo: unlockedPlayerContent.topAnchor, constant: 14),
            playerHeader.leadingAnchor.constraint(equalTo: unlockedPlayerContent.leadingAnchor, constant: 14),
            playerHeader.trailingAnchor.constraint(equalTo: unlockedPlayerContent.trailingAnchor, constant: -14),
            playerHeader.heightAnchor.constraint(equalToConstant: 52),

            nowPlayingTitleLabel.topAnchor.constraint(equalTo: playerHeader.topAnchor, constant: 8),
            nowPlayingTitleLabel.leadingAnchor.constraint(equalTo: playerHeader.leadingAnchor, constant: 14),
            nowPlayingTitleLabel.trailingAnchor.constraint(lessThanOrEqualTo: playerAudioToggleButton.leadingAnchor, constant: -10),

            nowPlayingMetaLabel.topAnchor.constraint(equalTo: nowPlayingTitleLabel.bottomAnchor, constant: 2),
            nowPlayingMetaLabel.leadingAnchor.constraint(equalTo: nowPlayingTitleLabel.leadingAnchor),
            nowPlayingMetaLabel.trailingAnchor.constraint(lessThanOrEqualTo: playerAudioToggleButton.leadingAnchor, constant: -10),

            backBtn.trailingAnchor.constraint(equalTo: playerHeader.trailingAnchor, constant: -14),
            backBtn.centerYAnchor.constraint(equalTo: playerHeader.centerYAnchor),
            backBtn.heightAnchor.constraint(equalToConstant: 32),

            exportClipButton.trailingAnchor.constraint(equalTo: backBtn.leadingAnchor, constant: -8),
            exportClipButton.centerYAnchor.constraint(equalTo: playerHeader.centerYAnchor),
            exportClipButton.heightAnchor.constraint(equalToConstant: 32),

            chooseSaveFolderButton.trailingAnchor.constraint(equalTo: exportClipButton.leadingAnchor, constant: -8),
            chooseSaveFolderButton.centerYAnchor.constraint(equalTo: playerHeader.centerYAnchor),
            chooseSaveFolderButton.heightAnchor.constraint(equalToConstant: 32),

            playerAudioToggleButton.trailingAnchor.constraint(equalTo: chooseSaveFolderButton.leadingAnchor, constant: -8),
            playerAudioToggleButton.centerYAnchor.constraint(equalTo: playerHeader.centerYAnchor),
            playerAudioToggleButton.heightAnchor.constraint(equalToConstant: 32),

            videoWrap.topAnchor.constraint(equalTo: playerHeader.bottomAnchor, constant: 10),
            videoWrap.leadingAnchor.constraint(equalTo: unlockedPlayerContent.leadingAnchor, constant: 14),
            videoWrap.trailingAnchor.constraint(equalTo: unlockedPlayerContent.trailingAnchor, constant: -14),
            videoWrap.bottomAnchor.constraint(equalTo: controlsCard.topAnchor, constant: -10),

            playerView.topAnchor.constraint(equalTo: videoWrap.topAnchor),
            playerView.leadingAnchor.constraint(equalTo: videoWrap.leadingAnchor),
            playerView.trailingAnchor.constraint(equalTo: videoWrap.trailingAnchor),
            playerView.bottomAnchor.constraint(equalTo: videoWrap.bottomAnchor),

            controlsCard.leadingAnchor.constraint(equalTo: unlockedPlayerContent.leadingAnchor, constant: 14),
            controlsCard.trailingAnchor.constraint(equalTo: unlockedPlayerContent.trailingAnchor, constant: -14),
            controlsCard.bottomAnchor.constraint(equalTo: unlockedPlayerContent.bottomAnchor, constant: -14),
            controlsCard.heightAnchor.constraint(equalToConstant: 126),

            scrubHint.leadingAnchor.constraint(equalTo: controlsCard.leadingAnchor, constant: 16),
            scrubHint.topAnchor.constraint(equalTo: controlsCard.topAnchor, constant: 28),

            scrubberBallView.leadingAnchor.constraint(equalTo: scrubHint.trailingAnchor, constant: 8),
            scrubberBallView.trailingAnchor.constraint(equalTo: playerTimeReadoutLabel.leadingAnchor, constant: -10),
            scrubberBallView.topAnchor.constraint(equalTo: controlsCard.topAnchor, constant: 6),
            scrubberBallView.heightAnchor.constraint(equalToConstant: 56),

            playerTimeReadoutLabel.trailingAnchor.constraint(equalTo: controlsCard.trailingAnchor, constant: -16),
            playerTimeReadoutLabel.centerYAnchor.constraint(equalTo: scrubHint.centerYAnchor),
            playerTimeReadoutLabel.widthAnchor.constraint(equalToConstant: 142),

            playAllPlayerButton.leadingAnchor.constraint(equalTo: controlsCard.leadingAnchor, constant: 14),
            playAllPlayerButton.bottomAnchor.constraint(equalTo: controlsCard.bottomAnchor, constant: -14),
            playAllPlayerButton.heightAnchor.constraint(equalToConstant: 34),

            prevClipButton.leadingAnchor.constraint(equalTo: playAllPlayerButton.trailingAnchor, constant: 6),
            prevClipButton.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),
            prevClipButton.heightAnchor.constraint(equalToConstant: 34),

            playPauseButton.leadingAnchor.constraint(equalTo: prevClipButton.trailingAnchor, constant: 6),
            playPauseButton.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),
            playPauseButton.heightAnchor.constraint(equalToConstant: 34),

            nextClipButton.leadingAnchor.constraint(equalTo: playPauseButton.trailingAnchor, constant: 6),
            nextClipButton.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),
            nextClipButton.heightAnchor.constraint(equalToConstant: 34),

            saveAndNextButton.leadingAnchor.constraint(equalTo: nextClipButton.trailingAnchor, constant: 8),
            saveAndNextButton.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),
            saveAndNextButton.heightAnchor.constraint(equalToConstant: 34),

            deleteAndNextButton.leadingAnchor.constraint(equalTo: saveAndNextButton.trailingAnchor, constant: 8),
            deleteAndNextButton.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),
            deleteAndNextButton.heightAnchor.constraint(equalToConstant: 34),

            speedSegmentedControl.trailingAnchor.constraint(equalTo: controlsCard.trailingAnchor, constant: -14),
            speedSegmentedControl.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),

            speedLabel.trailingAnchor.constraint(equalTo: speedSegmentedControl.leadingAnchor, constant: -6),
            speedLabel.leadingAnchor.constraint(greaterThanOrEqualTo: deleteAndNextButton.trailingAnchor, constant: 10),
            speedLabel.centerYAnchor.constraint(equalTo: playAllPlayerButton.centerYAnchor),

            lockedOverlayCard.centerXAnchor.constraint(equalTo: playerStageView.centerXAnchor),
            lockedOverlayCard.centerYAnchor.constraint(equalTo: playerStageView.centerYAnchor),
            lockedOverlayCard.widthAnchor.constraint(equalToConstant: 500),
            lockedOverlayCard.heightAnchor.constraint(equalToConstant: 240),

            shieldTitle.topAnchor.constraint(equalTo: lockedOverlayCard.topAnchor, constant: 28),
            shieldTitle.centerXAnchor.constraint(equalTo: lockedOverlayCard.centerXAnchor),

            shieldBody.topAnchor.constraint(equalTo: shieldTitle.bottomAnchor, constant: 14),
            shieldBody.leadingAnchor.constraint(equalTo: lockedOverlayCard.leadingAnchor, constant: 28),
            shieldBody.trailingAnchor.constraint(equalTo: lockedOverlayCard.trailingAnchor, constant: -28),

            unlockBigBtn.topAnchor.constraint(equalTo: shieldBody.bottomAnchor, constant: 20),
            unlockBigBtn.centerXAnchor.constraint(equalTo: lockedOverlayCard.centerXAnchor, constant: -76),
            unlockBigBtn.heightAnchor.constraint(equalToConstant: 36),

            returnCamBtn.centerYAnchor.constraint(equalTo: unlockBigBtn.centerYAnchor),
            returnCamBtn.leadingAnchor.constraint(equalTo: unlockBigBtn.trailingAnchor, constant: 12),
            returnCamBtn.heightAnchor.constraint(equalToConstant: 36)
        ])
    }

    private func setStageMode(playerMode: Bool) {
        isInPlayerMode = playerMode
        recorderStageView.isHidden = playerMode
        playerStageView.isHidden = !playerMode
        modeSwitchButton.title = playerMode ? "Live Camera" : "Vault Player"
        if !playerMode {
            // Security invariant: whenever Live Camera / Motion Monitoring is active on screen,
            // keep the vault strictly LOCKED so nobody entering the room can press Play All or Delete!
            unloadActivePlayerClip()
            vault.lock()
        } else {
            lastVaultInteractionAt = Date()
        }
        refreshVaultUIState()
    }

    // MARK: - Camera & Microphone Capture Configuration

    private func requestPermissionsAndStartCapture() {
        let group = DispatchGroup()
        group.enter()
        AVCaptureDevice.requestAccess(for: .video) { _ in group.leave() }
        group.enter()
        AVCaptureDevice.requestAccess(for: .audio) { _ in group.leave() }

        group.notify(queue: .main) { [weak self] in
            guard let self = self else { return }
            let vStatus = AVCaptureDevice.authorizationStatus(for: .video)
            let aStatus = AVCaptureDevice.authorizationStatus(for: .audio)

            self.permissionRecoveryButton.isHidden = (vStatus == .authorized)
            if vStatus != .authorized {
                self.hudPrimaryLabel.stringValue = "Camera permission needed: click 'Open System Camera & Mic Settings' to allow access."
                return
            }
            if aStatus != .authorized {
                self.audioReadoutLabel.stringValue = "Mic off (Video-only)"
            }
            self.refreshDeviceListsAndConfigureSession(preferredVideoID: nil, preferredAudioID: nil)
        }
    }

    private func refreshDeviceListsAndConfigureSession(preferredVideoID: String?, preferredAudioID: String?) {
        let videoTypes: [AVCaptureDevice.DeviceType]
        let audioTypes: [AVCaptureDevice.DeviceType]
        if #available(macOS 14.0, *) {
            videoTypes = [.builtInWideAngleCamera, .external]
            audioTypes = [.microphone, .external]
        } else {
            videoTypes = [.builtInWideAngleCamera, .externalUnknown]
            audioTypes = [.builtInMicrophone, .externalUnknown]
        }
        let vDiscovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: videoTypes,
            mediaType: .video,
            position: .unspecified
        )
        let aDiscovery = AVCaptureDevice.DiscoverySession(
            deviceTypes: audioTypes,
            mediaType: .audio,
            position: .unspecified
        )

        var vList = vDiscovery.devices
        if let defV = AVCaptureDevice.default(for: .video), !vList.contains(where: { $0.uniqueID == defV.uniqueID }) {
            vList.insert(defV, at: 0)
        }
        var aList = aDiscovery.devices
        if let defA = AVCaptureDevice.default(for: .audio), !aList.contains(where: { $0.uniqueID == defA.uniqueID }) {
            aList.insert(defA, at: 0)
        }

        self.videoDevices = vList
        self.audioDevices = aList

        cameraPopup.removeAllItems()
        if vList.isEmpty {
            cameraPopup.addItem(withTitle: "No Camera Detected")
        } else {
            for d in vList { cameraPopup.addItem(withTitle: "Cam: \(d.localizedName)") }
        }

        micPopup.removeAllItems()
        let audioAuthorized = (AVCaptureDevice.authorizationStatus(for: .audio) == .authorized)
        if !audioAuthorized || aList.isEmpty {
            micPopup.addItem(withTitle: audioAuthorized ? "No Mic Detected" : "Mic Permission Off")
        } else {
            for d in aList { micPopup.addItem(withTitle: "Mic: \(d.localizedName)") }
        }
        micPopup.isEnabled = isAudioRecordingEnabled && audioAuthorized && !aList.isEmpty

        let chosenVideo = vList.first(where: { $0.uniqueID == preferredVideoID }) ?? AVCaptureDevice.default(for: .video) ?? vList.first
        let selectedMicDevice = audioAuthorized
            ? (aList.first(where: { $0.uniqueID == preferredAudioID }) ?? AVCaptureDevice.default(for: .audio) ?? aList.first)
            : nil
        let chosenAudio = isAudioRecordingEnabled ? selectedMicDevice : nil

        if let cv = chosenVideo, let idx = vList.firstIndex(where: { $0.uniqueID == cv.uniqueID }) {
            cameraPopup.selectItem(at: idx)
        }
        if let sm = selectedMicDevice, let idx = aList.firstIndex(where: { $0.uniqueID == sm.uniqueID }) {
            micPopup.selectItem(at: idx)
        }

        let audioEnabledSnapshot = isAudioRecordingEnabled
        sessionQueue.async { [weak self] in
            self?.configureCaptureSessionOnQueue(
                videoDevice: chosenVideo,
                audioDevice: chosenAudio,
                audioRecordingEnabled: audioEnabledSnapshot
            )
        }
    }

    private func configureCaptureSessionOnQueue(
        videoDevice: AVCaptureDevice?,
        audioDevice: AVCaptureDevice?,
        audioRecordingEnabled: Bool
    ) {
        captureSession.beginConfiguration()

        if let oldV = activeVideoInput {
            captureSession.removeInput(oldV)
            activeVideoInput = nil
        }
        if let oldA = activeAudioInput {
            captureSession.removeInput(oldA)
            activeAudioInput = nil
        }

        if captureSession.canSetSessionPreset(.high) {
            captureSession.sessionPreset = .high
        }

        var selectedW = 1280
        var selectedH = 720

        if let vDev = videoDevice,
           let vIn = try? AVCaptureDeviceInput(device: vDev),
           captureSession.canAddInput(vIn) {
            captureSession.addInput(vIn)
            activeVideoInput = vIn

            // Prefer landscape up to 1920x1080 @ 30fps so hardware H.264 & scrubbing stay ultra-fast on all Macs
            var bestFormat: AVCaptureDevice.Format?
            var bestPixels: Int32 = 0
            var bestRange: AVFrameRateRange?

            for fmt in vDev.formats {
                let dims = CMVideoFormatDescriptionGetDimensions(fmt.formatDescription)
                guard dims.width >= dims.height, dims.width <= 1920, dims.height <= 1080 else { continue }
                for r in fmt.videoSupportedFrameRateRanges where r.maxFrameRate >= 29.9 {
                    let px = dims.width * dims.height
                    if px >= bestPixels {
                        bestPixels = px
                        bestFormat = fmt
                        bestRange = r
                    }
                }
            }

            if let fmt = bestFormat {
                do {
                    try vDev.lockForConfiguration()
                    vDev.activeFormat = fmt
                    if let range = bestRange {
                        let fps = min(30.0, range.maxFrameRate)
                        let dur = CMTime(value: 1, timescale: CMTimeScale(fps.rounded()))
                        vDev.activeVideoMinFrameDuration = dur
                        vDev.activeVideoMaxFrameDuration = dur
                    }
                    vDev.unlockForConfiguration()
                } catch {}
            }

            let activeDims = CMVideoFormatDescriptionGetDimensions(vDev.activeFormat.formatDescription)
            if activeDims.width > 0 && activeDims.height > 0 {
                selectedW = Int(activeDims.width)
                selectedH = Int(activeDims.height)
            }
        }

        var micAttached = false
        if let aDev = audioDevice,
           let aIn = try? AVCaptureDeviceInput(device: aDev),
           captureSession.canAddInput(aIn) {
            captureSession.addInput(aIn)
            activeAudioInput = aIn
            micAttached = true
        }

        // Video Data Output (Hardware NV12 -> feeds both Luma Motion Detector and VaultSegmentWriter)
        videoDataOutput.videoSettings = [
            kCVPixelBufferPixelFormatTypeKey as String: kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        ]
        videoDataOutput.alwaysDiscardsLateVideoFrames = false
        videoDataOutput.setSampleBufferDelegate(self, queue: mediaSampleQueue)
        if !captureSession.outputs.contains(videoDataOutput), captureSession.canAddOutput(videoDataOutput) {
            captureSession.addOutput(videoDataOutput)
        }

        // Audio Data Output (Normalized 48 kHz 16-bit Mono Linear PCM when Audio is ON; detached when Audio is OFF)
        if micAttached {
            audioDataOutput.audioSettings = [
                AVFormatIDKey: kAudioFormatLinearPCM,
                AVSampleRateKey: 48_000,
                AVNumberOfChannelsKey: 1,
                AVLinearPCMBitDepthKey: 16,
                AVLinearPCMIsFloatKey: false,
                AVLinearPCMIsBigEndianKey: false,
                AVLinearPCMIsNonInterleaved: false
            ]
            audioDataOutput.setSampleBufferDelegate(self, queue: mediaSampleQueue)
            if !captureSession.outputs.contains(audioDataOutput), captureSession.canAddOutput(audioDataOutput) {
                captureSession.addOutput(audioDataOutput)
            }
        } else {
            if captureSession.outputs.contains(audioDataOutput) {
                captureSession.removeOutput(audioDataOutput)
            }
        }

        captureSession.commitConfiguration()
        if !captureSession.isRunning {
            captureSession.startRunning()
        }

        let camName = videoDevice?.localizedName ?? "Camera"
        let micName = micAttached
            ? (audioDevice?.localizedName ?? "Mic")
            : (audioRecordingEnabled ? "Video Only" : "Audio Off")
        let dimsStr = "\(selectedW)x\(selectedH)"

        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.activeWidth = selectedW
            self.activeHeight = selectedH
            self.activeAudioActive = micAttached
            if !micAttached {
                self.latestAudioDB = -60.0
            }
            self.activeCameraSummary = "\(camName) (\(dimsStr)) • \(micName)"
            self.updateHeaderStatusTexts()
        }
    }

    private func reapplyCurrentDeviceSelection() {
        let camIdx = cameraPopup.indexOfSelectedItem
        let camID = (camIdx >= 0 && camIdx < videoDevices.count) ? videoDevices[camIdx].uniqueID : nil
        let micIdx = micPopup.indexOfSelectedItem
        let micID = (micIdx >= 0 && micIdx < audioDevices.count) ? audioDevices[micIdx].uniqueID : nil
        refreshDeviceListsAndConfigureSession(preferredVideoID: camID, preferredAudioID: micID)
    }

    private func syncRecordAudioControlsUI() {
        recordAudioSegmentedControl?.selectedSegment = isAudioRecordingEnabled ? 0 : 1
        menuRecordAudioItem?.state = isAudioRecordingEnabled ? .on : .off
        statusMenuRecordAudioItem?.state = isAudioRecordingEnabled ? .on : .off
        let audioAuthorized = (AVCaptureDevice.authorizationStatus(for: .audio) == .authorized)
        micPopup?.isEnabled = isAudioRecordingEnabled && audioAuthorized && !audioDevices.isEmpty
        if !isAudioRecordingEnabled {
            audioMeterView?.fraction = 0.0
            audioReadoutLabel?.stringValue = "Audio Off (Video-Only)"
        }
    }

    private func setRecordAudioEnabled(_ enabled: Bool) {
        guard isAudioRecordingEnabled != enabled else {
            syncRecordAudioControlsUI()
            return
        }
        isAudioRecordingEnabled = enabled
        UserDefaults.standard.set(enabled, forKey: Self.recordAudioPrefsKey)
        syncRecordAudioControlsUI()

        mediaSampleQueue.async { [weak self] in
            self?.sampleQueueAudioEnabled = enabled
        }

        if isRecordingClip {
            // Do not interrupt an active motion recording mid-clip; immediately mute/unmute incoming samples
            // and apply hardware mic attach/detach cleanly the moment this clip finishes.
            needsSessionRefreshAfterClip = true
        } else {
            needsSessionRefreshAfterClip = false
            reapplyCurrentDeviceSelection()
        }
        updateHeaderStatusTexts()
    }

    @objc private func onRecordAudioSegmentChanged(_ sender: NSSegmentedControl) {
        setRecordAudioEnabled(sender.selectedSegment == 0)
    }

    @objc private func toggleRecordAudioMenuAction() {
        setRecordAudioEnabled(!isAudioRecordingEnabled)
    }

    @objc private func onCameraPopupChanged(_ sender: NSPopUpButton) {
        let idx = sender.indexOfSelectedItem
        guard idx >= 0, idx < videoDevices.count else { return }
        let camID = videoDevices[idx].uniqueID
        let micIdx = micPopup.indexOfSelectedItem
        let micID = (micIdx >= 0 && micIdx < audioDevices.count) ? audioDevices[micIdx].uniqueID : nil
        refreshDeviceListsAndConfigureSession(preferredVideoID: camID, preferredAudioID: micID)
    }

    @objc private func onMicPopupChanged(_ sender: NSPopUpButton) {
        let micIdx = sender.indexOfSelectedItem
        guard micIdx >= 0, micIdx < audioDevices.count else { return }
        let micID = audioDevices[micIdx].uniqueID
        let camIdx = cameraPopup.indexOfSelectedItem
        let camID = (camIdx >= 0 && camIdx < videoDevices.count) ? videoDevices[camIdx].uniqueID : nil
        refreshDeviceListsAndConfigureSession(preferredVideoID: camID, preferredAudioID: micID)
    }

    // MARK: - Real-Time Motion Detection & Encrypted Vault Recording

    private func recomputeThresholdFromSensitivity() {
        // Non-linear studio taper:
        // 100% -> 0.25% frame delta (ultra sensitive)
        // 78%  -> 1.50% frame delta (balanced default)
        // 50%  -> 4.20% frame delta
        // 10%  -> 10.5% frame delta (heavy movement only)
        let norm = max(0.0, min(1.0, (100.0 - sensitivityPercent) / 90.0))
        let threshold = 0.25 + pow(norm, 1.65) * 10.25
        motionThresholdPercent = threshold

        let desc: String
        if sensitivityPercent >= 88 {
            desc = "Ultra Sensitive"
        } else if sensitivityPercent >= 68 {
            desc = "Balanced"
        } else if sensitivityPercent >= 40 {
            desc = "Moderate"
        } else {
            desc = "Heavy Motion Only"
        }

        sensitivityReadoutLabel?.stringValue = String(
            format: "Sensitivity: %.0f%% (%@ • %.2f%% trigger)",
            sensitivityPercent,
            desc,
            threshold
        )
        motionMeterView?.thresholdFraction = min(1.0, threshold / 12.0)
    }

    @objc private func onSensitivitySliderChanged(_ sender: NSSlider) {
        sensitivityPercent = sender.doubleValue
        recomputeThresholdFromSensitivity()
    }

    func captureOutput(_ output: AVCaptureOutput, didOutput sampleBuffer: CMSampleBuffer, from connection: AVCaptureConnection) {
        if output === videoDataOutput {
            handleVideoSampleBuffer(sampleBuffer)
        } else if output === audioDataOutput {
            handleAudioSampleBuffer(sampleBuffer, connection: connection)
        }
    }

    private func handleVideoSampleBuffer(_ sampleBuffer: CMSampleBuffer) {
        if let writer = activeSegmentWriter {
            writer.appendVideoSampleBuffer(sampleBuffer)
        }

        frameCounter &+= 1
        // Analyze every 3rd frame (~10 Hz) so CPU stays < 0.5% while catching motion in <= 200ms
        guard frameCounter % 3 == 0,
              let pb = CMSampleBufferGetImageBuffer(sampleBuffer) else { return }

        CVPixelBufferLockBaseAddress(pb, .readOnly)
        let width = CVPixelBufferGetWidthOfPlane(pb, 0)
        let height = CVPixelBufferGetHeightOfPlane(pb, 0)
        let bpr = CVPixelBufferGetBytesPerRowOfPlane(pb, 0)
        let basePtr = CVPixelBufferGetBaseAddressOfPlane(pb, 0)

        var motionScorePercent = 0.0
        if width >= 64, height >= 64, let raw = basePtr {
            let luma = raw.assumingMemoryBound(to: UInt8.self)
            let cols = 80
            let rows = 45
            let count = cols * rows
            var grid = [UInt8](repeating: 0, count: count)
            var sum = 0

            for r in 0..<rows {
                let y = min(height - 2, (r * (height - 2)) / rows)
                let row0 = y * bpr
                let row1 = (y + 1) * bpr
                for c in 0..<cols {
                    let x = min(width - 2, (c * (width - 2)) / cols)
                    let avg = (Int(luma[row0 + x]) + Int(luma[row0 + x + 1]) + Int(luma[row1 + x]) + Int(luma[row1 + x + 1])) >> 2
                    grid[r * cols + c] = UInt8(avg)
                    sum += avg
                }
            }

            let mean = Double(sum) / Double(count)
            if prevLumaGrid.count == count {
                let globalShift = Int(((mean - prevMeanLuma) * 0.65).rounded())
                var changed = 0
                for i in 0..<count {
                    let adjusted = Int(grid[i]) - globalShift
                    let diff = abs(adjusted - Int(prevLumaGrid[i]))
                    if diff > 14 {
                        changed += 1
                    }
                }
                motionScorePercent = (Double(changed) / Double(count)) * 100.0
            }
            prevLumaGrid = grid
            prevMeanLuma = mean
        }
        CVPixelBufferUnlockBaseAddress(pb, .readOnly)

        let now = Date()
        let thresholdSnapshot = motionThresholdPercent
        let motionHit: Bool
        if now >= armedWarmupUntil && motionScorePercent >= thresholdSnapshot {
            consecutiveMotionHits += 1
            motionHit = (consecutiveMotionHits >= 2)
        } else {
            consecutiveMotionHits = 0
            motionHit = false
        }

        DispatchQueue.main.async { [weak self] in
            guard let self = self, !self.isTerminatingApp else { return }
            self.latestMotionPercent = motionScorePercent
            if self.isArmed && motionHit {
                self.handleMotionTriggeredOnMain(now: now)
            }
        }
    }

    private func handleAudioSampleBuffer(_ sampleBuffer: CMSampleBuffer, connection: AVCaptureConnection) {
        guard sampleQueueAudioEnabled else { return }
        if let writer = activeSegmentWriter {
            writer.appendAudioSampleBuffer(sampleBuffer)
        }

        var db: Float = -60.0
        if let chan = connection.audioChannels.first {
            db = chan.averagePowerLevel
        } else if let block = CMSampleBufferGetDataBuffer(sampleBuffer) {
            var len = 0
            var ptr: UnsafeMutablePointer<Int8>?
            if CMBlockBufferGetDataPointer(block, atOffset: 0, lengthAtOffsetOut: nil, totalLengthOut: &len, dataPointerOut: &ptr) == noErr,
               let p = ptr, len >= 2 {
                let samples = min(512, len / 2)
                p.withMemoryRebound(to: Int16.self, capacity: samples) { sPtr in
                    var sqSum: Double = 0
                    for i in 0..<samples {
                        let v = Double(sPtr[i]) / 32768.0
                        sqSum += v * v
                    }
                    let rms = sqrt(sqSum / Double(max(1, samples)))
                    db = rms > 0.00001 ? Float(20.0 * log10(rms)) : -60.0
                }
            }
        }

        DispatchQueue.main.async { [weak self] in
            self?.latestAudioDB = max(-60.0, min(0.0, db))
        }
    }

    private func handleMotionTriggeredOnMain(now: Date) {
        let newDeadline = now.addingTimeInterval(minimumClipDuration)
        if isRecordingClip {
            if let existing = recordingStopDeadline {
                recordingStopDeadline = max(existing, newDeadline)
            } else {
                recordingStopDeadline = newDeadline
            }
            return
        }

        // Start a brand-new AES-256-GCM 1s-chunked vault recording!
        // Enforce strict locked-while-saving invariant: always locked in Live Camera mode,
        // and also immediately lock an unattended paused Vault Player (>20s idle) if someone enters the room!
        if vault.isUnlocked {
            let unattendedPlayer = isInPlayerMode && player.rate == 0 && !scrubberBallView.isDraggingBall && now.timeIntervalSince(lastVaultInteractionAt) > 20.0
            if !isInPlayerMode || unattendedPlayer {
                lockVaultImmediately(keepCurrentStageIfCamera: false)
            }
        }
        do {
            let recordWithAudio = isAudioRecordingEnabled && activeAudioActive
            let clipWriter = try vault.createClipWriter(
                width: activeWidth,
                height: activeHeight,
                hasAudio: recordWithAudio
            )
            let segWriter = try VaultSegmentWriter(
                clipWriter: clipWriter,
                width: activeWidth,
                height: activeHeight,
                hasAudio: recordWithAudio
            )
            isRecordingClip = true
            recordingStartedAt = now
            recordingStopDeadline = newDeadline
            mediaSampleQueue.async { [weak self] in
                self?.activeSegmentWriter = segWriter
            }
            refreshStatusBarIcon()
            updateHeaderStatusTexts()
        } catch {
            hudPrimaryLabel.stringValue = "Vault recording error: \(error.localizedDescription)"
        }
    }

    private func stopCurrentRecordingClip(completion: (() -> Void)? = nil) {
        guard isRecordingClip else {
            completion?()
            return
        }
        isRecordingClip = false
        recordingStartedAt = nil
        recordingStopDeadline = nil
        if !isInPlayerMode && vault.isUnlocked {
            vault.lock()
        }
        refreshStatusBarIcon()
        updateHeaderStatusTexts()

        mediaSampleQueue.async { [weak self] in
            guard let self = self else { return }
            let writer = self.activeSegmentWriter
            self.activeSegmentWriter = nil
            guard let active = writer else {
                DispatchQueue.main.async { completion?() }
                return
            }
            active.stopAndFinalize { [weak self] _, _, _ in
                DispatchQueue.main.async {
                    guard let self = self else { return }
                    if !self.isInPlayerMode && self.vault.isUnlocked {
                        self.vault.lock()
                    }
                    if self.needsSessionRefreshAfterClip {
                        self.needsSessionRefreshAfterClip = false
                        self.reapplyCurrentDeviceSelection()
                    }
                    self.refreshVaultUIState()
                    completion?()
                }
            }
        }
    }

    private func detachActiveWriterSynchronously() -> VaultSegmentWriter? {
        var captured: VaultSegmentWriter?
        mediaSampleQueue.sync {
            captured = activeSegmentWriter
            activeSegmentWriter = nil
        }
        return captured
    }

    // MARK: - Periodic UI Refresh (10 Hz)

    @objc private func onUITimerTick() {
        let now = Date()
        hudBottomLeftTimestampLabel?.stringValue = ClipTimestampFormatter.format(now)

        // Motion meter & readout
        let motionFrac = min(1.0, latestMotionPercent / 12.0)
        motionMeterView.fraction = motionFrac
        let triggered = latestMotionPercent >= motionThresholdPercent
        motionMeterView.barColor = triggered ? .systemRed : .systemGreen
        motionReadoutLabel.stringValue = String(
            format: "%.2f%% (Trigger: %.2f%%)",
            latestMotionPercent,
            motionThresholdPercent
        )

        // Audio meter & readout
        if !isAudioRecordingEnabled {
            audioMeterView.fraction = 0.0
            audioReadoutLabel.stringValue = "Audio Off (Video-Only)"
        } else if activeAudioActive {
            let normDB = Double(max(0.0, min(1.0, (latestAudioDB + 55.0) / 55.0)))
            audioMeterView.fraction = normDB
            audioReadoutLabel.stringValue = String(format: "%.0f dB (48 kHz AAC)", latestAudioDB)
        } else {
            audioMeterView.fraction = 0.0
            let auth = AVCaptureDevice.authorizationStatus(for: .audio)
            audioReadoutLabel.stringValue = (auth == .authorized) ? "No Mic (Video-Only)" : "Mic Off (Video-Only)"
        }

        // Recording countdown check
        if isRecordingClip {
            if let deadline = recordingStopDeadline, now >= deadline {
                stopCurrentRecordingClip()
            } else if let started = recordingStartedAt {
                let elapsed = max(0, Int(now.timeIntervalSince(started)))
                let rem = max(0, Int((recordingStopDeadline ?? now).timeIntervalSince(now).rounded(.up)))
                hudRecordingBadge.superview?.isHidden = false
                hudRecordingBadge.stringValue = String(
                    format: "● REC %02d:%02d • Vault Locked • Holds %ds",
                    elapsed / 60,
                    elapsed % 60,
                    rem
                )
            }
        } else {
            hudRecordingBadge.superview?.isHidden = true
        }

        // Auto-lock idle unlocked Vault Player after 90s of inactivity so an unattended Mac never stays unlocked
        if vault.isUnlocked {
            if !isInPlayerMode {
                vault.lock()
                refreshVaultUIState()
            } else if player.rate == 0 && !scrubberBallView.isDraggingBall && now.timeIntervalSince(lastVaultInteractionAt) > 90.0 {
                lockVaultImmediately(keepCurrentStageIfCamera: false)
            }
        }
    }

    private func updateHeaderStatusTexts() {
        let lockBadge = vault.isUnlocked ? "VAULT UNLOCKED" : "VAULT LOCKED"
        if isRecordingClip {
            statusPillLabel.stringValue = "● RECORDING • \(lockBadge)"
            statusPillLabel.textColor = .systemRed
            hudPrimaryLabel.stringValue = "RECORDING TO LOCKED VAULT • \(activeCameraSummary) • Safe on Exit & Power Loss"
        } else if isArmed {
            statusPillLabel.stringValue = "● ARMED • \(lockBadge) (WORKS LOCKED)"
            statusPillLabel.textColor = .systemGreen
            hudPrimaryLabel.stringValue = "ARMED & WATCHING (VAULT LOCKED) • \(activeCameraSummary) • Lock screen anytime (Ctrl+Cmd+Q)"
        } else {
            statusPillLabel.stringValue = "OFFLINE • \(lockBadge)"
            statusPillLabel.textColor = .systemTeal
            if AVCaptureDevice.authorizationStatus(for: .video) == .authorized {
                hudPrimaryLabel.stringValue = "LIVE PREVIEW • \(activeCameraSummary) • Press 'Start Motion Detection' to arm"
            }
        }

        armButton.title = isArmed ? "■ Stop Motion Detection" : "● Start Motion Detection"
        armButton.contentTintColor = isArmed ? .systemRed : .systemGreen
    }

    // MARK: - Arming, Password Prompts & Vault State

    @objc private func toggleArmedClicked() {
        if isArmed {
            isArmed = false
            sleepAndLock.setArmedPreventingIdleSleep(false)
            stopCurrentRecordingClip()
            refreshStatusBarIcon()
            updateHeaderStatusTexts()
            return
        }

        // Ensure vault has a password before arming so every recorded clip is encrypted under user's password!
        // Importantly, unlockAfterCreate is FALSE so the vault stays strictly LOCKED while monitoring & saving!
        guard vault.isInitialized else {
            presentCreatePasswordSheet(
                reason: "Create a Vault Password first so all motion recordings are encrypted and locked against tampering while Max Watch saves.",
                unlockAfterCreate: false
            ) { [weak self] success in
                if success {
                    self?.armNow()
                }
            }
            return
        }
        armNow()
    }

    private func armNow() {
        // Always lock the vault and switch to Live Camera when arming Motion Detection
        // so nobody entering the room can press Play All or Delete without the password!
        lockVaultImmediately(keepCurrentStageIfCamera: false)
        isArmed = true
        let warmup = Date().addingTimeInterval(1.1)
        mediaSampleQueue.async { [weak self] in
            self?.prevLumaGrid.removeAll()
            self?.consecutiveMotionHits = 0
            self?.armedWarmupUntil = warmup
        }
        sleepAndLock.setArmedPreventingIdleSleep(true)
        refreshStatusBarIcon()
        updateHeaderStatusTexts()
    }

    private func refreshVaultUIState() {
        let totalCount = vault.countVaultClips()
        if !vault.isInitialized {
            vaultLockButton.title = "🔑 Set Vault Password"
            playAllTopButton.title = "🔒 Play All (\(totalCount))"
            lockedSidebarPlaceholder.isHidden = false
            clipsScrollView.isHidden = true
            lockedSidebarCountLabel.stringValue = "Set a password to initialize your encrypted offline video vault."
            unlockedPlayerContent.isHidden = true
            lockedOverlayCard.isHidden = false
            unlockedClips = []
            clipsTableView.reloadData()
        } else if !vault.isUnlocked {
            vaultLockButton.title = "🔒 Unlock Vault"
            playAllTopButton.title = "🔒 Play All (\(totalCount))"
            lockedSidebarPlaceholder.isHidden = false
            clipsScrollView.isHidden = true
            lockedSidebarCountLabel.stringValue = "\(totalCount) encrypted clip\(totalCount == 1 ? "" : "s") protected by AES-256-GCM."
            unlockedPlayerContent.isHidden = true
            lockedOverlayCard.isHidden = false
            unlockedClips = []
            clipsTableView.reloadData()
        } else {
            let activeURL = activeSegmentWriter?.fileURL
            unlockedClips = vault.listSummaries(excludeActiveURL: activeURL)
            let count = unlockedClips.count
            vaultLockButton.title = "🔓 Lock Vault"
            playAllTopButton.title = "▶ Play All (\(count))"
            lockedSidebarPlaceholder.isHidden = true
            clipsScrollView.isHidden = false
            unlockedPlayerContent.isHidden = false
            lockedOverlayCard.isHidden = true
            sidebarHeaderLabel.stringValue = "Encrypted Vault (\(count))"
            clipsTableView.reloadData()
            if currentClipIndex >= 0 && currentClipIndex < count {
                clipsTableView.selectRowIndexes(IndexSet(integer: currentClipIndex), byExtendingSelection: false)
            }
        }
        updateHeaderStatusTexts()
    }

    @objc private func vaultLockButtonClicked() {
        if !vault.isInitialized {
            presentCreatePasswordSheet(
                reason: "Choose a password to protect your offline video vault and Play All button.",
                unlockAfterCreate: false
            ) { [weak self] _ in
                self?.refreshVaultUIState()
            }
        } else if vault.isUnlocked {
            lockVaultImmediately(keepCurrentStageIfCamera: false)
        } else {
            presentUnlockPasswordSheet(prompt: "Enter your Vault Password to review saved clips.") { [weak self] unlocked in
                guard let self = self, unlocked else { return }
                self.setStageMode(playerMode: true)
                self.refreshVaultUIState()
            }
        }
    }

    @objc private func lockVaultAction() {
        lockVaultImmediately(keepCurrentStageIfCamera: false)
    }

    private func lockVaultImmediately(keepCurrentStageIfCamera: Bool) {
        unloadActivePlayerClip()
        vault.lock()
        refreshVaultUIState()
        if !keepCurrentStageIfCamera && isInPlayerMode {
            setStageMode(playerMode: false)
        }
    }

    // MARK: - Password Sheets (Create / Unlock / Change / Reset)

    private func presentCreatePasswordSheet(reason: String, unlockAfterCreate: Bool, completion: @escaping (Bool) -> Void) {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Set Your Max Watch Vault Password"
        alert.informativeText = "\(reason)\n\n100% Offline: Your password never leaves this Mac. Minimum 4 characters."
        alert.addButton(withTitle: "Create Encrypted Vault")
        alert.addButton(withTitle: "Cancel")

        let stack = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 68))
        let pass1 = NSSecureTextField(frame: NSRect(x: 0, y: 38, width: 320, height: 26))
        pass1.placeholderString = "New vault password"
        let pass2 = NSSecureTextField(frame: NSRect(x: 0, y: 4, width: 320, height: 26))
        pass2.placeholderString = "Confirm vault password"
        stack.addSubview(pass1)
        stack.addSubview(pass2)
        alert.accessoryView = stack
        alert.window.initialFirstResponder = pass1

        alert.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self, resp == .alertFirstButtonReturn else {
                completion(false)
                return
            }
            let p1 = pass1.stringValue
            let p2 = pass2.stringValue
            guard p1.count >= 4 else {
                self.showSimpleError("Password must be at least 4 characters.") {
                    self.presentCreatePasswordSheet(reason: reason, unlockAfterCreate: unlockAfterCreate, completion: completion)
                }
                return
            }
            guard p1 == p2 else {
                self.showSimpleError("Passwords did not match. Please try again.") {
                    self.presentCreatePasswordSheet(reason: reason, unlockAfterCreate: unlockAfterCreate, completion: completion)
                }
                return
            }
            do {
                try self.vault.initializeVault(password: p1)
                if unlockAfterCreate {
                    try self.vault.unlock(password: p1)
                }
                self.refreshVaultUIState()
                completion(true)
            } catch {
                self.showSimpleError(error.localizedDescription) {
                    completion(false)
                }
            }
        }
    }

    private func presentUnlockPasswordSheet(prompt: String, completion: @escaping (Bool) -> Void) {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Unlock Encrypted Video Vault"
        alert.informativeText = prompt
        alert.addButton(withTitle: "Unlock")
        alert.addButton(withTitle: "Cancel")

        let passField = NSSecureTextField(frame: NSRect(x: 0, y: 0, width: 300, height: 26))
        passField.placeholderString = "Enter vault password"
        alert.accessoryView = passField
        alert.window.initialFirstResponder = passField

        alert.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self, resp == .alertFirstButtonReturn else {
                completion(false)
                return
            }
            do {
                try self.vault.unlock(password: passField.stringValue)
                self.lastVaultInteractionAt = Date()
                self.refreshVaultUIState()
                completion(true)
            } catch {
                self.showSimpleError("Incorrect password. Vault remains locked.") {
                    completion(false)
                }
            }
        }
    }

    @objc private func changePasswordClicked() {
        if !vault.isInitialized {
            presentCreatePasswordSheet(
                reason: "Create a Vault Password to protect your saved clips and Play All.",
                unlockAfterCreate: false
            ) { _ in }
            return
        }

        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.messageText = "Change Vault Password"
        alert.informativeText = "Enter your current password and a new password (at least 4 characters). Existing clips stay encrypted without needing re-encoding."
        alert.addButton(withTitle: "Update Password")
        alert.addButton(withTitle: "Cancel")
        alert.addButton(withTitle: "Reset Forgotten Vault...")

        let box = NSView(frame: NSRect(x: 0, y: 0, width: 320, height: 100))
        let cur = NSSecureTextField(frame: NSRect(x: 0, y: 70, width: 320, height: 26))
        cur.placeholderString = "Current password"
        let np1 = NSSecureTextField(frame: NSRect(x: 0, y: 36, width: 320, height: 26))
        np1.placeholderString = "New password"
        let np2 = NSSecureTextField(frame: NSRect(x: 0, y: 2, width: 320, height: 26))
        np2.placeholderString = "Confirm new password"
        box.addSubview(cur)
        box.addSubview(np1)
        box.addSubview(np2)
        alert.accessoryView = box
        alert.window.initialFirstResponder = cur

        alert.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self else { return }
            if resp == .alertThirdButtonReturn {
                self.confirmDestructiveVaultReset()
                return
            }
            guard resp == .alertFirstButtonReturn else { return }
            guard np1.stringValue == np2.stringValue else {
                self.showSimpleError("New passwords did not match.") {}
                return
            }
            do {
                try self.vault.changePassword(currentPassword: cur.stringValue, newPassword: np1.stringValue)
                self.showSimpleInfo("Vault password updated.", detail: "Your encrypted clips are now protected with your new password.")
            } catch {
                self.showSimpleError(error.localizedDescription) {}
            }
        }
    }

    private func confirmDestructiveVaultReset() {
        let warn = NSAlert()
        warn.alertStyle = .critical
        warn.messageText = "Erase Encrypted Vault & Start Fresh?"
        warn.informativeText = "Because Max Watch uses real zero-backdoor AES-256 encryption, forgotten passwords cannot be recovered. Resetting permanently wipes all existing encrypted clips."
        warn.addButton(withTitle: "Permanently Erase & Reset")
        warn.addButton(withTitle: "Cancel")
        warn.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self, resp == .alertFirstButtonReturn else { return }
            self.unloadActivePlayerClip()
            try? self.vault.resetVaultDeletingAllClips()
            self.refreshVaultUIState()
        }
    }

    private func showSimpleError(_ msg: String, after: @escaping () -> Void) {
        let a = NSAlert()
        a.alertStyle = .warning
        a.messageText = msg
        a.addButton(withTitle: "OK")
        a.beginSheetModal(for: window) { _ in after() }
    }

    private func showSimpleInfo(_ title: String, detail: String) {
        let a = NSAlert()
        a.alertStyle = .informational
        a.messageText = title
        a.informativeText = detail
        a.addButton(withTitle: "OK")
        a.beginSheetModal(for: window, completionHandler: nil)
    }

    // MARK: - Password-Locked Play All, Fast Scrubber Ball & 1x / 4x / 12x Playback

    private func setupPlayerObservers() {
        let interval = CMTime(seconds: 0.04, preferredTimescale: 600)
        timeObserverToken = player.addPeriodicTimeObserver(forInterval: interval, queue: .main) { [weak self] time in
            guard let self = self, !self.scrubberBallView.isDraggingBall else { return }
            let sec = max(0.0, CMTimeGetSeconds(time))
            guard sec.isFinite else { return }
            self.scrubberBallView.currentSeconds = sec
            self.updatePlayerTimeReadout(current: sec, total: self.currentClipDuration)
        }

        clipEndObserver = NotificationCenter.default.addObserver(
            forName: .AVPlayerItemDidPlayToEndTime,
            object: nil,
            queue: .main
        ) { [weak self] note in
            guard let self = self,
                  let item = note.object as? AVPlayerItem,
                  item === self.player.currentItem else { return }
            self.handleCurrentClipFinished()
        }
    }

    @objc private func playAllClicked() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)

        let proceedUnlocked = { [weak self] in
            guard let self = self else { return }
            // If currently recording a clip and user presses Play All, seal current clip first so it's included!
            if self.isRecordingClip {
                self.stopCurrentRecordingClip { [weak self] in
                    self?.startPlayAllUnlockedSequence()
                }
            } else {
                self.startPlayAllUnlockedSequence()
            }
        }

        if !vault.isInitialized {
            presentCreatePasswordSheet(
                reason: "Create a Vault Password to unlock Play All and encrypt recordings.",
                unlockAfterCreate: true
            ) { ok in
                if ok { proceedUnlocked() }
            }
        } else if !vault.isUnlocked {
            presentUnlockPasswordSheet(prompt: "Enter your Vault Password to run Play All.") { ok in
                if ok { proceedUnlocked() }
            }
        } else {
            proceedUnlocked()
        }
    }

    private func startPlayAllUnlockedSequence() {
        lastVaultInteractionAt = Date()
        setStageMode(playerMode: true)
        refreshVaultUIState()
        guard !unlockedClips.isEmpty else {
            nowPlayingTitleLabel.stringValue = "Vault is unlocked (0 clips recorded yet)"
            nowPlayingMetaLabel.stringValue = "Click 'Lock & Live Camera' and 'Start Motion Detection' to capture motion clips while locked."
            return
        }
        isPlayAllSequentialActive = true
        loadAndPlayClip(at: 0, autoPlay: true)
    }

    private func loadAndPlayClip(at index: Int, autoPlay: Bool) {
        guard vault.isUnlocked, index >= 0, index < unlockedClips.count else { return }
        let summary = unlockedClips[index]

        do {
            unloadActivePlayerClip()
            let reader = try vault.openClipReader(url: summary.url)
            let (asset, loader) = VaultAssetResourceLoader.makePlayableAsset(reader: reader)
            let item = AVPlayerItem(asset: asset)
            item.audioTimePitchAlgorithm = .varispeed

            self.activeClipReader = reader
            self.activeResourceLoader = loader
            self.currentClipIndex = index
            self.currentClipDuration = max(0.2, summary.estimatedDurationSeconds)
            self.scrubEngine.reset()

            scrubberBallView.durationSeconds = currentClipDuration
            scrubberBallView.currentSeconds = 0.0
            updatePlayerTimeReadout(current: 0.0, total: currentClipDuration)

            player.replaceCurrentItem(with: item)
            clipsTableView.selectRowIndexes(IndexSet(integer: index), byExtendingSelection: false)
            clipsTableView.scrollRowToVisible(index)

            let stamp = ClipTimestampFormatter.format(summary.createdAt)
            let mb = Double(summary.fileSizeBytes) / 1_048_576.0
            let safetyTag = summary.cleanlyFinalized ? "Clean Seal" : "Recovered Power-Cut Clip"
            let audioTag = summary.hasAudio ? "AAC Audio" : "Video-Only"

            nowPlayingTitleLabel.stringValue = String(
                format: "Clip %d of %d • %@",
                index + 1,
                unlockedClips.count,
                stamp
            )
            nowPlayingMetaLabel.stringValue = String(
                format: "%dx%d • %@ • %.1f MB AES-256-GCM • %@ • Speed %.0fx",
                summary.width,
                summary.height,
                audioTag,
                mb,
                safetyTag,
                selectedPlaybackSpeed
            )

            // Update exact duration once AVPlayerItem parses fMP4 segments
            Task { @MainActor [weak self] in
                guard let self = self else { return }
                if let dur = try? await asset.load(.duration) {
                    let secs = CMTimeGetSeconds(dur)
                    if secs.isFinite && secs > 0.1 && self.currentClipIndex == index {
                        self.currentClipDuration = secs
                        self.scrubberBallView.durationSeconds = secs
                        self.updatePlayerTimeReadout(current: CMTimeGetSeconds(self.player.currentTime()), total: secs)
                    }
                }
                if autoPlay && self.currentClipIndex == index {
                    self.applyPlaybackSpeedAndPlay()
                }
            }
        } catch {
            nowPlayingTitleLabel.stringValue = "Unable to open clip: \(error.localizedDescription)"
        }
    }

    private func applyPlaybackSpeedAndPlay() {
        guard let item = player.currentItem else { return }
        item.audioTimePitchAlgorithm = .varispeed
        player.isMuted = isPlayerAudioMuted
        // Keep crisp audio at 1x and 4x; mute audio track at 12x so CoreAudio never clamps video rate!
        for track in item.tracks where track.assetTrack?.mediaType == .audio {
            track.isEnabled = (selectedPlaybackSpeed <= 4.0) && !isPlayerAudioMuted
        }
        player.playImmediately(atRate: selectedPlaybackSpeed)
    }

    @objc private func togglePlayerMuteClicked() {
        lastVaultInteractionAt = Date()
        isPlayerAudioMuted.toggle()
        player.isMuted = isPlayerAudioMuted
        if let item = player.currentItem {
            for track in item.tracks where track.assetTrack?.mediaType == .audio {
                track.isEnabled = (selectedPlaybackSpeed <= 4.0) && !isPlayerAudioMuted
            }
        }
        playerAudioToggleButton?.title = isPlayerAudioMuted ? "🔇 Audio: OFF" : "🔊 Audio: ON"
        playerAudioToggleButton?.contentTintColor = isPlayerAudioMuted ? .systemOrange : .systemTeal
    }

    private func handleUserBallScrub(seconds: Double, isDragging: Bool) {
        lastVaultInteractionAt = Date()
        guard player.currentItem != nil else { return }
        if isDragging {
            if player.rate != 0 {
                wasPlayingBeforeDrag = true
                player.pause()
            }
            scrubEngine.scrub(to: seconds)
            updatePlayerTimeReadout(current: seconds, total: currentClipDuration)
        } else {
            scrubEngine.scrub(to: seconds)
            updatePlayerTimeReadout(current: seconds, total: currentClipDuration)
            if wasPlayingBeforeDrag {
                wasPlayingBeforeDrag = false
                applyPlaybackSpeedAndPlay()
            }
        }
    }

    private func updatePlayerTimeReadout(current: Double, total: Double) {
        func fmt(_ s: Double) -> String {
            let clamped = max(0.0, s)
            let mins = Int(clamped) / 60
            let secs = clamped - Double(mins * 60)
            return String(format: "%02d:%04.1f", mins, secs)
        }
        playerTimeReadoutLabel.stringValue = "\(fmt(current)) / \(fmt(total))"
    }

    private func handleCurrentClipFinished() {
        guard isPlayAllSequentialActive else { return }
        if currentClipIndex + 1 < unlockedClips.count {
            loadAndPlayClip(at: currentClipIndex + 1, autoPlay: true)
        } else {
            isPlayAllSequentialActive = false
            nowPlayingMetaLabel.stringValue = "Finished playing all \(unlockedClips.count) vault clips."
        }
    }

    private func unloadActivePlayerClip() {
        player.pause()
        player.replaceCurrentItem(with: nil)
        scrubEngine.reset()
        activeResourceLoader = nil
        activeClipReader = nil
        currentClipIndex = -1
    }

    // MARK: - Playback Controls & Speed Actions

    @objc private func onSpeedSegmentChanged(_ sender: NSSegmentedControl) {
        switch sender.selectedSegment {
        case 1: setPlaybackSpeed(4.0)
        case 2: setPlaybackSpeed(12.0)
        default: setPlaybackSpeed(1.0)
        }
    }

    @objc private func selectSpeed1x() {
        speedSegmentedControl.selectedSegment = 0
        setPlaybackSpeed(1.0)
    }

    @objc private func selectSpeed4x() {
        speedSegmentedControl.selectedSegment = 1
        setPlaybackSpeed(4.0)
    }

    @objc private func selectSpeed12x() {
        speedSegmentedControl.selectedSegment = 2
        setPlaybackSpeed(12.0)
    }

    private func setPlaybackSpeed(_ speed: Float) {
        lastVaultInteractionAt = Date()
        selectedPlaybackSpeed = speed
        if player.rate > 0 {
            applyPlaybackSpeedAndPlay()
        }
        if currentClipIndex >= 0 && currentClipIndex < unlockedClips.count {
            let s = unlockedClips[currentClipIndex]
            let mb = Double(s.fileSizeBytes) / 1_048_576.0
            nowPlayingMetaLabel.stringValue = String(
                format: "%dx%d • %.1f MB AES-256-GCM • Speed %.0fx",
                s.width,
                s.height,
                mb,
                speed
            )
        }
    }

    @objc private func togglePlayPauseClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked else {
            playAllClicked()
            return
        }
        if player.currentItem == nil {
            if !unlockedClips.isEmpty {
                loadAndPlayClip(at: max(0, currentClipIndex), autoPlay: true)
            }
            return
        }
        if player.rate != 0 {
            player.pause()
        } else {
            if CMTimeGetSeconds(player.currentTime()) >= currentClipDuration - 0.15 {
                player.seek(to: .zero, toleranceBefore: .zero, toleranceAfter: .zero)
            }
            applyPlaybackSpeedAndPlay()
        }
    }

    @objc private func prevClipClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked, !unlockedClips.isEmpty else { return }
        let target = max(0, (currentClipIndex <= 0 ? 0 : currentClipIndex - 1))
        loadAndPlayClip(at: target, autoPlay: true)
    }

    @objc private func nextClipClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked, !unlockedClips.isEmpty else { return }
        if currentClipIndex + 1 < unlockedClips.count {
            loadAndPlayClip(at: currentClipIndex + 1, autoPlay: true)
        }
    }

    // MARK: - Save & Next (to User-Chosen Mac Folder Outside Vault) + Delete & Next

    private func releaseChosenSaveDirectoryScope() {
        if hasSecurityScopedSaveDir, let dir = chosenSaveDirectoryURL {
            dir.stopAccessingSecurityScopedResource()
            hasSecurityScopedSaveDir = false
        }
    }

    private func promptForSaveFolder(completion: @escaping (URL?) -> Void) {
        let panel = NSOpenPanel()
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        panel.allowsMultipleSelection = false
        panel.canCreateDirectories = true
        panel.title = "Choose Folder on Your Mac for 'Save & Next'"
        panel.message = "Select any folder on your Mac (outside the encrypted vault) where 'Save & Next' should save standard MP4 video copies."
        panel.prompt = "Choose Save Folder"

        panel.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self, resp == .OK, let picked = panel.url else {
                completion(nil)
                return
            }
            let stdPicked = picked.standardizedFileURL.path
            let stdVault = self.vault.vaultDirectoryURL.standardizedFileURL.path
            if stdPicked == stdVault || stdPicked.hasPrefix(stdVault + "/") {
                self.showSimpleError("Please choose a folder on your Mac outside the internal Encrypted Vault (for example Desktop, Movies, or Downloads).") {
                    completion(nil)
                }
                return
            }

            self.releaseChosenSaveDirectoryScope()
            let scoped = picked.startAccessingSecurityScopedResource()
            self.chosenSaveDirectoryURL = picked
            self.hasSecurityScopedSaveDir = scoped
            self.chooseSaveFolderButton.title = "📁 Save Folder: \(picked.lastPathComponent)"
            completion(picked)
        }
    }

    @objc private func chooseSaveFolderClicked() {
        lastVaultInteractionAt = Date()
        promptForSaveFolder { [weak self] dir in
            guard let self = self, let d = dir else { return }
            self.nowPlayingMetaLabel.stringValue = "Save & Next destination set to '\(d.lastPathComponent)' on your Mac."
        }
    }

    @objc private func saveAndNextClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked else {
            playAllClicked()
            return
        }
        guard !unlockedClips.isEmpty else {
            showSimpleInfo("No Clips to Save", detail: "Your encrypted vault does not have any recorded clips yet.")
            return
        }
        let targetIndex = (currentClipIndex >= 0 && currentClipIndex < unlockedClips.count) ? currentClipIndex : 0
        if currentClipIndex != targetIndex {
            loadAndPlayClip(at: targetIndex, autoPlay: false)
        }

        let performSaveAndAdvance: (URL) -> Void = { [weak self] dirURL in
            guard let self = self,
                  self.vault.isUnlocked,
                  targetIndex >= 0,
                  targetIndex < self.unlockedClips.count else { return }
            let summary = self.unlockedClips[targetIndex]
            do {
                let savedURL = try self.vault.exportClipToDirectory(clipURL: summary.url, directoryURL: dirURL)
                let savedNote = "Saved \(savedURL.lastPathComponent) to '\(dirURL.lastPathComponent)'"
                if targetIndex + 1 < self.unlockedClips.count {
                    self.loadAndPlayClip(at: targetIndex + 1, autoPlay: true)
                    self.nowPlayingMetaLabel.stringValue = "\(savedNote) • Playing next clip"
                } else {
                    self.player.pause()
                    self.isPlayAllSequentialActive = false
                    self.nowPlayingMetaLabel.stringValue = "\(savedNote) • Reached final clip (\(self.unlockedClips.count) of \(self.unlockedClips.count))"
                }
            } catch {
                self.showSimpleError("Save & Next failed: \(error.localizedDescription)") {}
            }
        }

        if let existingDir = chosenSaveDirectoryURL {
            performSaveAndAdvance(existingDir)
        } else {
            let wasPlaying = (player.rate != 0)
            if wasPlaying {
                player.pause()
            }
            promptForSaveFolder { [weak self] pickedDir in
                guard let self = self else { return }
                if let dir = pickedDir {
                    performSaveAndAdvance(dir)
                } else if wasPlaying {
                    self.applyPlaybackSpeedAndPlay()
                }
            }
        }
    }

    @objc private func deleteAndNextClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked, currentClipIndex >= 0, currentClipIndex < unlockedClips.count else { return }
        let doomed = unlockedClips[currentClipIndex]
        let resumeIdx = currentClipIndex

        unloadActivePlayerClip()
        try? vault.secureDeleteClip(url: doomed.url)
        refreshVaultUIState()

        if unlockedClips.isEmpty {
            isPlayAllSequentialActive = false
            nowPlayingTitleLabel.stringValue = "All clips deleted - Vault is empty"
            nowPlayingMetaLabel.stringValue = "Securely zero-wiped and removed from disk."
            scrubberBallView.currentSeconds = 0
            updatePlayerTimeReadout(current: 0, total: 0)
        } else if resumeIdx < unlockedClips.count {
            loadAndPlayClip(at: resumeIdx, autoPlay: true)
            nowPlayingMetaLabel.stringValue = "Deleted previous clip • Playing next clip (\(resumeIdx + 1) of \(unlockedClips.count))"
        } else {
            isPlayAllSequentialActive = false
            let lastIdx = unlockedClips.count - 1
            loadAndPlayClip(at: lastIdx, autoPlay: false)
            nowPlayingMetaLabel.stringValue = "Deleted final clip • Reached end of vault (\(unlockedClips.count) remaining)"
        }
    }

    @objc private func exportCurrentClipClicked() {
        lastVaultInteractionAt = Date()
        guard vault.isUnlocked, currentClipIndex >= 0, currentClipIndex < unlockedClips.count else {
            showSimpleInfo("Select an Unlocked Clip First", detail: "Unlock the vault and select a clip to export a decrypted MP4 copy.")
            return
        }
        let summary = unlockedClips[currentClipIndex]
        let savePanel = NSSavePanel()
        savePanel.allowedContentTypes = [.mpeg4Movie]
        savePanel.nameFieldStringValue = summary.url.deletingPathExtension().lastPathComponent + ".mp4"
        savePanel.title = "Save Decrypted MP4 Clip As..."
        savePanel.message = "Choose anywhere on your Mac to save a standard MP4 copy outside the encrypted vault."
        savePanel.beginSheetModal(for: window) { [weak self] resp in
            guard let self = self, resp == .OK, let dest = savePanel.url else { return }
            do {
                try self.vault.exportClipAsMP4(clipURL: summary.url, destinationURL: dest)
                self.nowPlayingMetaLabel.stringValue = "Saved \(dest.lastPathComponent) to '\(dest.deletingLastPathComponent().lastPathComponent)'"
            } catch {
                self.showSimpleError("Export failed: \(error.localizedDescription)") {}
            }
        }
    }

    // MARK: - Sidebar TableView Delegate & DataSource

    func numberOfRows(in tableView: NSTableView) -> Int {
        return unlockedClips.count
    }

    func tableView(_ tableView: NSTableView, viewFor tableColumn: NSTableColumn?, row: Int) -> NSView? {
        guard row >= 0, row < unlockedClips.count else { return nil }
        let id = NSUserInterfaceItemIdentifier("VaultClipCell")
        let cell: NSTableCellView
        if let reused = tableView.makeView(withIdentifier: id, owner: self) as? NSTableCellView {
            cell = reused
        } else {
            cell = NSTableCellView()
            cell.identifier = id
            let title = makeLabel("", size: 12.5, weight: .semibold, color: .white, monoDigits: true)
            title.tag = 101
            let sub = makeLabel("", size: 11, weight: .regular, color: NSColor(calibratedWhite: 0.68, alpha: 1.0), monoDigits: true)
            sub.tag = 102
            cell.addSubview(title)
            cell.addSubview(sub)
            NSLayoutConstraint.activate([
                title.topAnchor.constraint(equalTo: cell.topAnchor, constant: 7),
                title.leadingAnchor.constraint(equalTo: cell.leadingAnchor, constant: 8),
                title.trailingAnchor.constraint(equalTo: cell.trailingAnchor, constant: -8),
                sub.topAnchor.constraint(equalTo: title.bottomAnchor, constant: 2),
                sub.leadingAnchor.constraint(equalTo: title.leadingAnchor),
                sub.trailingAnchor.constraint(equalTo: title.trailingAnchor)
            ])
        }

        let item = unlockedClips[row]
        let stamp = ClipTimestampFormatter.formatCompact(item.createdAt)
        let titleText = "#\(row + 1)  \(stamp)"
        let mb = Double(item.fileSizeBytes) / 1_048_576.0
        let audioNote = item.hasAudio ? "" : " • No Audio"
        let subText = String(format: "%.0fs • %.1f MB • %@%@%@",
                             item.estimatedDurationSeconds,
                             mb,
                             "\(item.width)x\(item.height)",
                             audioNote,
                             item.cleanlyFinalized ? "" : " • Recovered")

        (cell.viewWithTag(101) as? NSTextField)?.stringValue = titleText
        (cell.viewWithTag(102) as? NSTextField)?.stringValue = subText
        return cell
    }

    @objc private func onVaultClipRowClicked() {
        lastVaultInteractionAt = Date()
        let row = clipsTableView.clickedRow
        guard vault.isUnlocked, row >= 0, row < unlockedClips.count else { return }
        isPlayAllSequentialActive = false
        setStageMode(playerMode: true)
        loadAndPlayClip(at: row, autoPlay: true)
    }

    // MARK: - Window & Menu Utility Actions

    @objc private func toggleStageModeClicked() {
        setStageMode(playerMode: !isInPlayerMode)
    }

    @objc private func switchToLiveCameraClicked() {
        setStageMode(playerMode: false)
    }

    @objc private func showMainWindowAction() {
        window.makeKeyAndOrderFront(nil)
        NSApp.activate(ignoringOtherApps: true)
    }

    @objc private func openPrivacySettingsClicked() {
        if let url = URL(string: "x-apple.systempreferences:com.apple.preference.security?Privacy_Camera") {
            NSWorkspace.shared.open(url)
        }
    }

    @objc private func showAboutPanel() {
        let alert = NSAlert()
        alert.alertStyle = .informational
        alert.icon = NSApp.applicationIconImage
        alert.messageText = "Max Watch (v1.2.0)"
        alert.informativeText = """
        A free, 100% offline, encrypted motion-detection security camera & video vault for Mac - gifted to the community by Max Freedom Pollard.

        • Zero network access (sandboxed offline at the macOS kernel level)
        • Vault stays strictly locked while arming & saving clips against room tampering
        • Works continuously even while your Mac screen is locked
        • Unambiguous international computer-time stamp on every clip
        • One-click Audio ON / OFF switch for video-with-audio or silent video-only capture
        • AES-256-GCM encrypted vault + password-locked Play All
        • 1-second power-cut-safe recording + clean exit save
        • Fast-drag timeline scrubber ball + 1x / 4x / 12x playback
        • One-click Save & Next (to any Mac folder) + Delete & Next

        Stay safe, stay peaceful, and take good care of each other.
        """
        alert.addButton(withTitle: "Peace & Thanks")
        alert.runModal()
    }
}
