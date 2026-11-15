import AppKit
import Foundation

// Entry point for Max Watch (Universal macOS 13.0+ Offline Motion Security & Encrypted Video Vault)
let args = CommandLine.arguments

if let selfTestIdx = args.firstIndex(of: "--selftest"), selfTestIdx + 1 < args.count {
    let dir = URL(fileURLWithPath: args[selfTestIdx + 1], isDirectory: true)
    let code = MaxWatchSelfTest.run(in: dir)
    exit(code)
}

var customVaultURL: URL? = nil
if let vaultIdx = args.firstIndex(of: "--vault-dir"), vaultIdx + 1 < args.count {
    customVaultURL = URL(fileURLWithPath: args[vaultIdx + 1], isDirectory: true)
}

let app = NSApplication.shared
let controller = AppController(customVaultDirectory: customVaultURL)
app.delegate = controller
app.run()
