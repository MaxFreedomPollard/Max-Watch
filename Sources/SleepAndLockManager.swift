import AppKit
import Foundation
import IOKit.pwr_mgt

// MARK: - Screen-Lock Continuation & Anti-Sleep Power Manager

final class SleepAndLockManager {
    private var sleepAssertionID: IOPMAssertionID = IOPMAssertionID(0)
    private var hasSleepAssertion = false
    private var processActivityToken: NSObjectProtocol?

    private(set) var isScreenCurrentlyLocked: Bool = false

    var onScreenLocked: (() -> Void)?
    var onScreenUnlocked: (() -> Void)?
    var onSystemWillPowerOff: (() -> Void)?

    init() {
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
        setArmedPreventingIdleSleep(false)
        DistributedNotificationCenter.default().removeObserver(self)
        NSWorkspace.shared.notificationCenter.removeObserver(self)
    }

    /// Prevents macOS idle system sleep and App Nap so camera capture + motion encryption
    /// continue uninterrupted even when the display sleeps or the user locks their screen (`Ctrl+Cmd+Q`).
    func setArmedPreventingIdleSleep(_ active: Bool) {
        if active {
            if !hasSleepAssertion {
                var id = IOPMAssertionID(0)
                let kr = IOPMAssertionCreateWithName(
                    "PreventUserIdleSystemSleep" as CFString,
                    IOPMAssertionLevel(kIOPMAssertionLevelOn),
                    "Max Watch Armed Offline Motion Security Recording" as CFString,
                    &id
                )
                if kr == kIOReturnSuccess {
                    sleepAssertionID = id
                    hasSleepAssertion = true
                }
            }
            if processActivityToken == nil {
                processActivityToken = ProcessInfo.processInfo.beginActivity(
                    options: [
                        .userInitiated,
                        .idleSystemSleepDisabled,
                        .suddenTerminationDisabled,
                        .automaticTerminationDisabled
                    ],
                    reason: "Max Watch Continuous Locked-Screen Motion Security"
                )
            }
        } else {
            if hasSleepAssertion {
                IOPMAssertionRelease(sleepAssertionID)
                sleepAssertionID = IOPMAssertionID(0)
                hasSleepAssertion = false
            }
            if let token = processActivityToken {
                ProcessInfo.processInfo.endActivity(token)
                processActivityToken = nil
            }
        }
    }

    @objc private func handleScreenLocked(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isScreenCurrentlyLocked = true
            self.onScreenLocked?()
        }
    }

    @objc private func handleScreenUnlocked(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            guard let self = self else { return }
            self.isScreenCurrentlyLocked = false
            self.onScreenUnlocked?()
        }
    }

    @objc private func handleWillPowerOff(_ note: Notification) {
        DispatchQueue.main.async { [weak self] in
            self?.onSystemWillPowerOff?()
        }
    }
}
