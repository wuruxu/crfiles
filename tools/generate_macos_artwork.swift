// Generate deterministic macOS packaging artwork from the project's SVG logo.
import AppKit

let project = URL(fileURLWithPath: CommandLine.arguments[1]).standardizedFileURL
func color(_ r: CGFloat, _ g: CGFloat, _ b: CGFloat, _ a: CGFloat = 1) -> NSColor {
    NSColor(srgbRed: r / 255, green: g / 255, blue: b / 255, alpha: a)
}
func png(_ width: Int, _ height: Int, _ url: URL, draw: () -> Void) throws {
    let bitmap = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: width,
        pixelsHigh: height, bitsPerSample: 8, samplesPerPixel: 4, hasAlpha: true,
        isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: 0, bitsPerPixel: 0)!
    NSGraphicsContext.saveGraphicsState()
    NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: bitmap)
    draw()
    NSGraphicsContext.restoreGraphicsState()
    try bitmap.representation(using: .png, properties: [:])!.write(to: url)
}
let fm = FileManager.default
let iconset = URL(fileURLWithPath: CommandLine.arguments[2])
try fm.createDirectory(at: iconset, withIntermediateDirectories: true)
guard let logo = NSImage(contentsOf: project.appendingPathComponent("crFiles.svg")) else {
    fatalError("Cannot read the application SVG")
}
for size in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let edge = size * scale
        let suffix = scale == 2 ? "@2x" : ""
        try png(edge, edge, iconset.appendingPathComponent("icon_\(size)x\(size)\(suffix).png")) {
            let e = CGFloat(edge)
            NSColor.white.setFill()
            NSBezierPath(roundedRect: NSRect(x: e * 0.035, y: e * 0.035,
                width: e * 0.93, height: e * 0.93), xRadius: e * 0.20, yRadius: e * 0.20).fill()
            logo.draw(in: NSRect(x: e * 0.10, y: e * 0.10, width: e * 0.80, height: e * 0.80))
        }
    }
}
// Finder uses a 720 x 440 point canvas. Keep both icon locations uncluttered.
try png(720, 440, project.appendingPathComponent("mac/dmg/background.png")) {
    NSGradient(starting: color(249, 251, 255), ending: color(229, 237, 251))!
        .draw(in: NSRect(x: 0, y: 0, width: 720, height: 440), angle: -30)
    color(66, 133, 244, 0.07).setStroke()
    for x in stride(from: 0, through: 720, by: 24) {
        let path = NSBezierPath(); path.move(to: NSPoint(x: x, y: 0)); path.line(to: NSPoint(x: x, y: 440)); path.lineWidth = 0.5; path.stroke()
    }
    for y in stride(from: 0, through: 440, by: 24) {
        let path = NSBezierPath(); path.move(to: NSPoint(x: 0, y: y)); path.line(to: NSPoint(x: 720, y: y)); path.lineWidth = 0.5; path.stroke()
    }
    let palette = [color(66,133,244), color(234,67,53), color(251,188,5), color(52,168,83)]
    for (i, c) in palette.enumerated() {
        c.withAlphaComponent(0.22).setStroke()
        let arc = NSBezierPath()
        arc.appendArc(withCenter: NSPoint(x: 680, y: 430), radius: CGFloat(150 + i * 22),
                      startAngle: 180, endAngle: 350)
        arc.lineWidth = 2; arc.stroke()
        c.setFill(); NSBezierPath(roundedRect: NSRect(x: 40 + i * 28, y: 390, width: 20, height: 4), xRadius: 2, yRadius: 2).fill()
    }
    color(66,133,244,0.65).setStroke()
    let arrow = NSBezierPath(); arrow.move(to: NSPoint(x: 329, y: 220)); arrow.line(to: NSPoint(x: 391, y: 220))
    arrow.move(to: NSPoint(x: 380, y: 230)); arrow.line(to: NSPoint(x: 391, y: 220)); arrow.line(to: NSPoint(x: 380, y: 210))
    arrow.lineWidth = 3; arrow.lineCapStyle = .round; arrow.lineJoinStyle = .round; arrow.stroke()
}
