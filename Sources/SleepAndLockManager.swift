import AppKit
import CoreGraphics
import Darwin
import Foundation
import IOKit
import IOKit.graphics
import IOKit.pwr_mgt

// MARK: - Display Brightness Hardware Backend (Apple Silicon DisplayServices + Intel IOKit)

protocol DisplayBrightnessBackend: AnyObject {
    func snapshotBrightnessByDisplay() -> [UInt32: Float]
    func setAllDisplaysBrightness(_ level: Float)
    func restoreBrightnessByDisplay(_ saved: [UInt32: Float])
}

final class SystemDisplayBrightnessBackend: DisplayBrightnessBackend {
    #if !MAC_APP_STORE
    private typealias GetBrightnessFn = @convention(c) (CGDirectDisplayID, UnsafeMutablePointer<Float>) -> Int32
    private typealias SetBrightnessFn = @convention(c) (CGDirectDisplayID, Float) -> Int32

    private var dsHandle: UnsafeMutableRawPointer?
    private var dsGetBrightness: GetBrightnessFn?
    private var dsSetBrightness: SetBrightnessFn?

    init() {
        if let handle = dlopen("/System/Library/PrivateFrameworks/DisplayServices.framework/DisplayServices", RTLD_LAZY) {
            self.dsHandle = handle
            if let getSym = dlsym(handle, "DisplayServicesGetBrightness") {
                self.dsGetBrightness = unsafeBitCast(getSym, to: GetBrightnessFn.self)
            }
            if let setSym = dlsym(handle, "DisplayServicesSetBrightness") {
                self.dsSetBrightness = unsafeBitCast(setSym, to: SetBrightnessFn.self)
            }
        }
    }

    deinit {
        if let handle = dsHandle {
            dlclose(handle)
        }
    }

    private func onlineDisplayIDs() -> [CGDirectDisplayID] {
        var count: UInt32 = 0
        guard CGGetOnlineDisplayList(0, nil, &count) == .success, count > 0 else {
            return [CGMainDisplayID()]
        }
        var ids = [CGDirectDisplayID](repeating: 0, count: Int(count))
        guard CGGetOnlineDisplayList(count, &ids, &count) == .success else {
            return [CGMainDisplayID()]
        }
        return Array(ids.prefix(Int(count)))
    }
    #else
    init() {}
    #endif

    func snapshotBrightnessByDisplay() -> [UInt32: Float] {
        var snapshot: [UInt32: Float] = [:]

        #if !MAC_APP_STORE
        // 1. Apple Silicon & modern macOS DisplayServices path (Developer ID build only)
        if let getFn = dsGetBrightness {
            for id in onlineDisplayIDs() {
                var val: Float = -1.0
                if getFn(id, &val) == 0, val >= 0.0, val <= 1.0 {
                    snapshot[id] = val
                }
            }
        }
        #endif

        // 2. Public IOKit IODisplayConnect path
        if snapshot.isEmpty {
            var iterator: io_iterator_t = 0
            if IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &iterator) == kIOReturnSuccess {
                defer { IOObjectRelease(iterator) }
                var serviceIndex: UInt32 = 10_000
                while true {
                    let service = IOIteratorNext(iterator)
                    if service == 0 { break }
                    defer { IOObjectRelease(service) }
                    var val: Float = -1.0
                    if IODisplayGetFloatParameter(service, 0, kIODisplayBrightnessKey as CFString, &val) == kIOReturnSuccess,
                       val >= 0.0, val <= 1.0 {
                        snapshot[serviceIndex] = val
                    }
                    serviceIndex &+= 1
                }
            }
        }

        return snapshot
    }

    func setAllDisplaysBrightness(_ level: Float) {
        let clamped = max(0.0, min(1.0, level))
        var appliedAny = false

        #if !MAC_APP_STORE
        if let setFn = dsSetBrightness {
            for id in onlineDisplayIDs() {
                if setFn(id, clamped) == 0 {
                    appliedAny = true
                }
            }
        }
        #endif

        // Public IOKit IODisplaySetFloatParameter path
        if !appliedAny {
            var iterator: io_iterator_t = 0
            if IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &iterator) == kIOReturnSuccess {
                defer { IOObjectRelease(iterator) }
                while true {
                    let service = IOIteratorNext(iterator)
                    if service == 0 { break }
                    defer { IOObjectRelease(service) }
                    _ = IODisplaySetFloatParameter(service, 0, kIODisplayBrightnessKey as CFString, clamped)
                }
            }
        }
    }

    func restoreBrightnessByDisplay(_ saved: [UInt32: Float]) {
        guard !saved.isEmpty else { return }

        #if !MAC_APP_STORE
        if let setFn = dsSetBrightness {
            for (id, val) in saved where id < 10_000 {
                _ = setFn(id, max(0.0, min(1.0, val)))
            }
        }
        #endif

        let ioEntries = saved.filter { $0.key >= 10_000 }
        if !ioEntries.isEmpty {
            var iterator: io_iterator_t = 0
            if IOServiceGetMatchingServices(kIOMainPortDefault, IOServiceMatching("IODisplayConnect"), &iterator) == kIOReturnSuccess {
                defer { IOObjectRelease(iterator) }
                var serviceIndex: UInt32 = 10_000
                while true {
                    let service = IOIteratorNext(iterator)
                    if service == 0 { break }
                    defer { IOObjectRelease(service) }
                    if let target = ioEntries[serviceIndex] {
                        _ = IODisplaySetFloatParameter(service, 0, kIODisplayBrightnessKey as CFString, max(0.0, min(1.0, target)))
                    }
                    serviceIndex &+= 1
                }
            }
        }
    }
}

// MARK: - Screen-Lock Continuation, Night Illumination & Anti-Sleep Power Manager

final class SleepAndLockManager {
    private var sleepAssertionID: IOPMAssertionID = IOPMAssertionID(0)
    private var hasSleepAssertion = false

    private var displaySleepAssertionID: IOPMAssertionID = IOPMAssertionID(0)
    private var hasDisplaySleepAssertion = false

    private var userActivityAssertionID: IOPMAssertionID = IOPMAssertionID(0)
    private var hasUserActivityAssertion = false

    private var processActivityToken: NSObjectProtocol?
    private var processActivityIncludesDisplay: Bool = false

    private let brightnessBackend: DisplayBrightnessBackend
    private let enableSystemPowerAssertions: Bool
    private var savedBrightnessByDisplay: [UInt32: Float] = [:]
    private(set) var isBrightnessBoostActive: Bool = false
    private var lastBoostReinforcedAt: Date = .distantPast

    private(set) var isArmed: Bool = false
    private(set) var isRecordingActive: Bool = false
    private(set) var isScreenCurrentlyLocked: Bool = false

    /// Checked by default (`true`) so arming + locking the screen keeps the lock screen / screen saver
    /// at 100% brightness for pitch-dark night visibility. Can be unchecked anytime by the user.
    var maxBrightnessWhenLockedEnabled: Bool = true {
        didSet {
            guard oldValue != maxBrightnessWhenLockedEnabled else { return }
            reconcilePowerAndBrightnessState(forceWakePulse: maxBrightnessWhenLockedEnabled)
        }
    }

    var onScreenLocked: (() -> Void)?
    var onScreenUnlocked: (() -> Void)?
    var onSystemWillPowerOff: (() -> Void)?

    init(
        brightnessBackend: DisplayBrightnessBackend = SystemDisplayBrightnessBackend(),
        registerSystemNotifications: Bool = true,
        enableSystemPowerAssertions: Bool = true
    ) {
        self.brightnessBackend = brightnessBackend
        self.enableSystemPowerAssertions = enableSystemPowerAssertions

        guard registerSystemNotifications else { return }

        let dnc = DistributedNotificationCenter.default()
        dnc.addObserver(
            self,
            selector: #selector(handleScreenLocked(_:)),
            name: NSNotification.Name("com.apple.screenIsLocked"),
            object: nil
        )
        dnc.addObserver(
            self,
            selector: #selector(handleScreenUnlocked(_:)),
            name: NSNotification.Name("com.apple.screenIsUnlocked"),
            object: nil
        )

        NSWorkspace.shared.notificationCenter.addObserver(
            self,
            selector: #selector(handleWillPowerOff(_:)),
            name: NSWorkspace.willPowerOffNotification,
            object: nil
        )
    }

    deinit {
        restoreBrightnessIfNeeded()
        releaseAllAssertions()
        DistributedNotificationCenter.default().removeObserver(self)
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }

    /// Prevents macOS idle system sleep and App Nap so camera capture + motion/sound encryption
    /// continue uninterrupted even when the user locks their screen (`Ctrl+Cmd+Q`).
    /// When `maxBrightnessWhenLockedEnabled` is ON and the screen is locked, also keeps the display
    /// awake at 100% brightness to illuminate dark rooms at night.
    func setArmedPreventingIdleSleep(_ active: Bool) {
        isArmed = active
        if !active {
            isRecordingActive = false
        }
        reconcilePowerAndBrightnessState(forceWakePulse: active && isScreenCurrentlyLocked)
    }

    /// Notifies the manager when a motion/sound clip starts or finishes so locked-screen display wake
    /// and 100% brightness are immediately reinforced if the Mac screen is locked.
    func setRecordingActive(_ recording: Bool) {
        let startedRecording = (!isRecordingActive && recording)
        isRecordingActive = recording
        reconcilePowerAndBrightnessState(forceWakePulse: startedRecording && isScreenCurrentlyLocked)
    }

    /// Called periodically (e.g. from the 10 Hz UI timer) to defeat ambient-light auto-dimming in a dark room
    /// while armed and screen-locked with Max Brightness When Locked enabled.
    func periodicMaintenanceTick(now: Date = Date()) {
        guard shouldBoostLockedBrightness else { return }
        if now.timeIntervalSince(lastBoostReinforcedAt) >= 2.0 {
            lastBoostReinforcedAt = now
            brightnessBackend.setAllDisplaysBrightness(1.0)
            declareDisplayWakeUserActivityIfNeeded()
        }
    }

    /// Deterministic lock-state hook used by `--selftest` to verify brightness save, 100% boost, and exact restore.
    func simulateScreenLockStateForTesting(_ locked: Bool) {
        isScreenCurrentlyLocked = locked
        reconcilePowerAndBrightnessState(forceWakePulse: locked)
        if locked {
            onScreenLocked?()
        } else {
            onScreenUnlocked?()
        }
    }

    var shouldBoostLockedBrightness: Bool {
        return isArmed && isScreenCurrentlyLocked && maxBrightnessWhenLockedEnabled
    }

    private func reconcilePowerAndBrightnessState(forceWakePulse: Bool) {
        let wantSystemAwake = isArmed
        let wantDisplayAwakeAndBright = shouldBoostLockedBrightness

        if enableSystemPowerAssertions {
            // 1. System idle sleep assertion
            if wantSystemAwake {
                if !hasSleepAssertion {
                    var id = IOPMAssertionID(0)
                    let kr = IOPMAssertionCreateWithName(
                        "PreventUserIdleSystemSleep" as CFString,
                        IOPMAssertionLevel(kIOPMAssertionLevelOn),
                        "Max Watch Armed Offline Motion & Sound Security Recording" as CFString,
                        &id
                    )
                    if kr == kIOReturnSuccess {
                        sleepAssertionID = id
                        hasSleepAssertion = true
                    }
                }
            } else if hasSleepAssertion {
                IOPMAssertionRelease(sleepAssertionID)
                sleepAssertionID = IOPMAssertionID(0)
                hasSleepAssertion = false
            }

            // 2. Display sleep assertion when locked + Night Light option is ON
            if wantDisplayAwakeAndBright {
                if !hasDisplaySleepAssertion {
                    var dispID = IOPMAssertionID(0)
                    let kr = IOPMAssertionCreateWithName(
                        "PreventUserIdleDisplaySleep" as CFString,
                        IOPMAssertionLevel(kIOPMAssertionLevelOn),
                        "Max Watch Locked-Screen 100% Brightness Night Illumination" as CFString,
                        &dispID
                    )
                    if kr == kIOReturnSuccess {
                        displaySleepAssertionID = dispID
                        hasDisplaySleepAssertion = true
                    }
                }
                if forceWakePulse {
                    declareDisplayWakeUserActivityIfNeeded()
                }
            } else {
                if hasDisplaySleepAssertion {
                    IOPMAssertionRelease(displaySleepAssertionID)
                    displaySleepAssertionID = IOPMAssertionID(0)
                    hasDisplaySleepAssertion = false
                }
                releaseUserActivityAssertion()
            }

            // 3. ProcessInfo activity token (prevent App Nap + system/display idle sleep)
            if wantSystemAwake {
                if processActivityToken == nil || processActivityIncludesDisplay != wantDisplayAwakeAndBright {
                    if let old = processActivityToken {
                        ProcessInfo.processInfo.endActivity(old)
                        processActivityToken = nil
                    }
                    var opts: ProcessInfo.ActivityOptions = [
                        .userInitiated,
                        .idleSystemSleepDisabled,
                        .suddenTerminationDisabled,
                        .automaticTerminationDisabled
                    ]
                    if wantDisplayAwakeAndBright {
                        opts.insert(.idleDisplaySleepDisabled)
                    }
                    processActivityToken = ProcessInfo.processInfo.beginActivity(
                        options: opts,
                        reason: wantDisplayAwakeAndBright
                            ? "Max Watch Locked-Screen Night Illumination & Security Recording"
                            : "Max Watch Continuous Locked-Screen Motion & Sound Security"
                    )
                    processActivityIncludesDisplay = wantDisplayAwakeAndBright
                }
            } else if let token = processActivityToken {
                ProcessInfo.processInfo.endActivity(token)
                processActivityToken = nil
                processActivityIncludesDisplay = false
            }
        }

        // 4. Hardware brightness save -> 100% boost -> exact restore
        if wantDisplayAwakeAndBright {
            if !isBrightnessBoostActive {
                savedBrightnessByDisplay = brightnessBackend.snapshotBrightnessByDisplay()
                isBrightnessBoostActive = true
            }
            brightnessBackend.setAllDisplaysBrightness(1.0)
            lastBoostReinforcedAt = Date()
        } else {
            restoreBrightnessIfNeeded()
        }
    }

    private func declareDisplayWakeUserActivityIfNeeded() {
        guard enableSystemPowerAssertions else { return }
        releaseUserActivityAssertion()
        var actID = IOPMAssertionID(0)
        let kr = IOPMAssertionDeclareUserActivity(
            "Max Watch Night Illumination Keep-Display-On" as CFString,
            kIOPMUserActiveLocal,
            &actID
        )
        if kr == kIOReturnSuccess && actID != 0 {
            userActivityAssertionID = actID
            hasUserActivityAssertion = true
        }
    }

    private func releaseUserActivityAssertion() {
        if hasUserActivityAssertion {
            IOPMAssertionRelease(userActivityAssertionID)
            userActivityAssertionID = IOPMAssertionID(0)
            hasUserActivityAssertion = false
        }
    }

    private func restoreBrightnessIfNeeded() {
        guard isBrightnessBoostActive else { return }
        let toRestore = savedBrightnessByDisplay
        savedBrightnessByDisplay.removeAll()
        isBrightnessBoostActive = false
        brightnessBackend.restoreBrightnessByDisplay(toRestore)
    }

    private func releaseAllAssertions() {
        guard enableSystemPowerAssertions else { return }
        if hasSleepAssertion {
            IOPMAssertionRelease(sleepAssertionID)
            sleepAssertionID = IOPMAssertionID(0)
            hasSleepAssertion = false
        }
        if hasDisplaySleepAssertion {
            IOPMAssertionRelease(displaySleepAssertionID)
            displaySleepAssertionID = IOPMAssertionID(0)
            hasDisplaySleepAssertion = false
        }
        releaseUserActivityAssertion()
        if let token = processActivityToken {
            ProcessInfo.processInfo.endActivity(token)
            processActivityToken = nil
            processActivityIncludesDisplay = false
        }
    }

    @objc private func handleScreenLocked(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isScreenCurrentlyLocked = true
            self.reconcilePowerAndBrightnessState(forceWakePulse: true)
            self.onScreenLocked?()
        }
    }

    @objc private func handleScreenUnlocked(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isScreenCurrentlyLocked = false
            self.reconcilePowerAndBrightnessState(forceWakePulse: false)
            self.onScreenUnlocked?()
        }
    }

    @objc private func handleWillPowerOff(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.restoreBrightnessIfNeeded()
            self.onSystemWillPowerOff?()
        }
    }
}
