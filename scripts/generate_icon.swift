import AppKit
import CoreGraphics
import CoreText
import Foundation

let args = CommandLine.arguments
guard args.count >= 2 else {
    fputs("Usage: swift generate_icon.swift <project_root>\n", stderr)
    exit(1)
}

let projectRoot = URL(fileURLWithPath: args[1], isDirectory: true)
let xcassetsDir = projectRoot.appendingPathComponent("Resources/Assets.xcassets/AppIcon.appiconset", isDirectory: true)
let iconsetDir = projectRoot.appendingPathComponent("build/AppIcon.iconset", isDirectory: true)
let resourcesDir = projectRoot.appendingPathComponent("Resources", isDirectory: true)

try FileManager.default.createDirectory(at: xcassetsDir, withIntermediateDirectories: true)
try FileManager.default.createDirectory(at: iconsetDir, withIntermediateDirectories: true)

func renderMFIcon(pixelSize: Int) -> Data {
    let s = CGFloat(pixelSize)
    let colorSpace = CGColorSpace(name: CGColorSpace.sRGB)!
    let ctx = CGContext(
        data: nil,
        width: pixelSize,
        height: pixelSize,
        bitsPerComponent: 8,
        bytesPerRow: pixelSize * 4,
        space: colorSpace,
        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue
    )!

    ctx.setAllowsAntialiasing(true)
    ctx.setShouldAntialias(true)

    // Full-bleed macOS squircle-friendly canvas (Apple's macOS icon grid inset ~ 9.5% or full bleed inside rounded rect)
    // Note: On macOS 11+, full-bleed 1024x1024 with macOS icon grid proportions (824/1024 = 80.5% squircle or full-canvas squircle):
    // Drawing a crisp macOS continuous-curve rounded square with zero outer clip issues, filling 92% of canvas so MF is HUGE in the Dock!
    ctx.clear(CGRect(x: 0, y: 0, width: s, height: s))

    let pad = s * 0.045
    let cardRect = CGRect(x: pad, y: pad, width: s - pad * 2, height: s - pad * 2)
    let corner = cardRect.width * 0.225

    let cardPath = CGPath(roundedRect: cardRect, cornerWidth: corner, cornerHeight: corner, transform: nil)

    // Subtle drop shadow for Dock depth
    ctx.saveGState()
    ctx.setShadow(
        offset: CGSize(width: 0, height: -s * 0.018),
        blur: s * 0.035,
        color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.48)
    )
    ctx.addPath(cardPath)
    ctx.setFillColor(CGColor(srgbRed: 0.05, green: 0.07, blue: 0.11, alpha: 1.0))
    ctx.fillPath()
    ctx.restoreGState()

    // Clip inside squircle
    ctx.saveGState()
    ctx.addPath(cardPath)
    ctx.clip()

    // Deep obsidian -> midnight slate gradient
    let bgColors = [
        CGColor(srgbRed: 0.11, green: 0.15, blue: 0.22, alpha: 1.0),
        CGColor(srgbRed: 0.04, green: 0.05, blue: 0.08, alpha: 1.0)
    ] as CFArray
    let bgGrad = CGGradient(colorsSpace: colorSpace, colors: bgColors, locations: [0.0, 1.0])!
    ctx.drawLinearGradient(
        bgGrad,
        start: CGPoint(x: s * 0.2, y: s * 0.95),
        end: CGPoint(x: s * 0.8, y: s * 0.05),
        options: []
    )

    // Subtle top-right emerald/cyan security glow
    let glowColors = [
        CGColor(srgbRed: 0.08, green: 0.72, blue: 0.65, alpha: 0.24),
        CGColor(srgbRed: 0.08, green: 0.72, blue: 0.65, alpha: 0.0)
    ] as CFArray
    let glowGrad = CGGradient(colorsSpace: colorSpace, colors: glowColors, locations: [0.0, 1.0])!
    ctx.drawRadialGradient(
        glowGrad,
        startCenter: CGPoint(x: s * 0.78, y: s * 0.80),
        startRadius: 0,
        endCenter: CGPoint(x: s * 0.78, y: s * 0.80),
        endRadius: s * 0.55,
        options: []
    )

    // Crisp inner rim border
    ctx.addPath(cardPath)
    ctx.setStrokeColor(CGColor(srgbRed: 0.38, green: 0.52, blue: 0.68, alpha: 0.38))
    ctx.setLineWidth(max(1.0, s * 0.016))
    ctx.strokePath()

    // Custom ultra-bold geometric "M" and "F" taking up the full logo space!
    // Drawn as crisp vector geometry so kerning, stroke weight, and alignment are pixel-perfect at 16px AND 1024px.
    let leftX = cardRect.minX + cardRect.width * 0.095
    let rightX = cardRect.maxX - cardRect.width * 0.085
    let topY = cardRect.maxY - cardRect.height * 0.155
    let botY = cardRect.minY + cardRect.height * 0.155
    let capH = topY - botY
    let totalW = rightX - leftX

    let gap = totalW * 0.070
    let mWidth = totalW * 0.535
    let fWidth = totalW - mWidth - gap
    let stroke = capH * 0.185

    let lettersPath = CGMutablePath()

    // --- BOLD GEOMETRIC "M" ---
    let mL = leftX
    let mR = leftX + mWidth
    let mMidX = (mL + mR) * 0.5
    let valleyBottomY = botY + capH * 0.25
    let valleyTopY = valleyBottomY + stroke * 1.18
    let innerLeftX = mL + stroke * 0.96
    let innerRightX = mR - stroke * 0.96

    lettersPath.move(to: CGPoint(x: mL, y: botY))
    lettersPath.addLine(to: CGPoint(x: mL, y: topY))
    lettersPath.addLine(to: CGPoint(x: mL + stroke * 1.02, y: topY))
    lettersPath.addLine(to: CGPoint(x: mMidX, y: valleyTopY))
    lettersPath.addLine(to: CGPoint(x: mR - stroke * 1.02, y: topY))
    lettersPath.addLine(to: CGPoint(x: mR, y: topY))
    lettersPath.addLine(to: CGPoint(x: mR, y: botY))
    lettersPath.addLine(to: CGPoint(x: innerRightX, y: botY))
    lettersPath.addLine(to: CGPoint(x: innerRightX, y: topY - stroke * 1.48))
    lettersPath.addLine(to: CGPoint(x: mMidX + stroke * 0.24, y: valleyBottomY))
    lettersPath.addLine(to: CGPoint(x: mMidX - stroke * 0.24, y: valleyBottomY))
    lettersPath.addLine(to: CGPoint(x: innerLeftX, y: topY - stroke * 1.48))
    lettersPath.addLine(to: CGPoint(x: innerLeftX, y: botY))
    lettersPath.closeSubpath()

    // --- BOLD GEOMETRIC "F" ---
    let fL = mR + gap
    let fR = fL + fWidth
    let midBarTopY = botY + capH * 0.56
    let midBarBotY = midBarTopY - stroke * 0.92
    let midBarRightX = fL + fWidth * 0.88

    lettersPath.move(to: CGPoint(x: fL, y: botY))
    lettersPath.addLine(to: CGPoint(x: fL, y: topY))
    lettersPath.addLine(to: CGPoint(x: fR, y: topY))
    lettersPath.addLine(to: CGPoint(x: fR, y: topY - stroke))
    lettersPath.addLine(to: CGPoint(x: fL + stroke, y: topY - stroke))
    lettersPath.addLine(to: CGPoint(x: fL + stroke, y: midBarTopY))
    lettersPath.addLine(to: CGPoint(x: midBarRightX, y: midBarTopY))
    lettersPath.addLine(to: CGPoint(x: midBarRightX, y: midBarBotY))
    lettersPath.addLine(to: CGPoint(x: fL + stroke, y: midBarBotY))
    lettersPath.addLine(to: CGPoint(x: fL + stroke, y: botY))
    lettersPath.closeSubpath()

    // Subtle crisp shadow behind MF
    ctx.saveGState()
    ctx.setShadow(
        offset: CGSize(width: 0, height: -s * 0.015),
        blur: s * 0.025,
        color: CGColor(srgbRed: 0, green: 0, blue: 0, alpha: 0.65)
    )
    ctx.addPath(lettersPath)
    ctx.setFillColor(CGColor(srgbRed: 1, green: 1, blue: 1, alpha: 1))
    ctx.fillPath()
    ctx.restoreGState()

    // Crisp luminous white-to-ice-cyan fill on MF
    ctx.saveGState()
    ctx.addPath(lettersPath)
    ctx.clip()
    let fgColors = [
        CGColor(srgbRed: 1.00, green: 1.00, blue: 1.00, alpha: 1.0),
        CGColor(srgbRed: 0.82, green: 0.95, blue: 0.99, alpha: 1.0)
    ] as CFArray
    let fgGrad = CGGradient(colorsSpace: colorSpace, colors: fgColors, locations: [0.0, 1.0])!
    ctx.drawLinearGradient(
        fgGrad,
        start: CGPoint(x: s * 0.5, y: topY),
        end: CGPoint(x: s * 0.5, y: botY),
        options: []
    )
    ctx.restoreGState()

    ctx.restoreGState()

    let cgImg = ctx.makeImage()!
    let rep = NSBitmapImageRep(cgImage: cgImg)
    return rep.representation(using: .png, properties: [:])!
}

let specs: [(pt: Int, scale: Int, filename: String)] = [
    (16, 1, "icon_16x16.png"),
    (16, 2, "icon_16x16@2x.png"),
    (32, 1, "icon_32x32.png"),
    (32, 2, "icon_32x32@2x.png"),
    (128, 1, "icon_128x128.png"),
    (128, 2, "icon_128x128@2x.png"),
    (256, 1, "icon_256x256.png"),
    (256, 2, "icon_256x256@2x.png"),
    (512, 1, "icon_512x512.png"),
    (512, 2, "icon_512x512@2x.png")
]

var imagesJSON: [[String: String]] = []
for spec in specs {
    let px = spec.pt * spec.scale
    let png = renderMFIcon(pixelSize: px)
    try png.write(to: xcassetsDir.appendingPathComponent(spec.filename))
    try png.write(to: iconsetDir.appendingPathComponent(spec.filename))
    imagesJSON.append([
        "idiom": "mac",
        "size": "\(spec.pt)x\(spec.pt)",
        "scale": "\(spec.scale)x",
        "filename": spec.filename
    ])
}

let contentsObj: [String: Any] = [
    "images": imagesJSON,
    "info": ["author": "xcode", "version": 1]
]
let contentsData = try JSONSerialization.data(withJSONObject: contentsObj, options: [.prettyPrinted, .sortedKeys])
try contentsData.write(to: xcassetsDir.appendingPathComponent("Contents.json"))

let rootContents: [String: Any] = [
    "info": ["author": "xcode", "version": 1]
]
let rootData = try JSONSerialization.data(withJSONObject: rootContents, options: [.prettyPrinted, .sortedKeys])
try rootData.write(to: projectRoot.appendingPathComponent("Resources/Assets.xcassets/Contents.json"))

let proc = Process()
proc.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
proc.arguments = ["-c", "icns", iconsetDir.path, "-o", resourcesDir.appendingPathComponent("AppIcon.icns").path]
try proc.run()
proc.waitUntilExit()
guard proc.terminationStatus == 0 else {
    fputs("iconutil failed\n", stderr)
    exit(1)
}
print("Generated MF AppIcon.icns and Assets.xcassets successfully.")
