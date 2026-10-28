import AppKit

// A vector-drawn app icon: two overlapping spaces with a small forward arrow.
let destination = CommandLine.arguments[1]
try FileManager.default.createDirectory(atPath: destination, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let side = points * scale
        let image = NSImage(size: NSSize(width: side, height: side))
        image.lockFocus()
        let context = NSGraphicsContext.current!.cgContext
        context.scaleBy(x: CGFloat(side) / 1024, y: CGFloat(side) / 1024)
        let background = NSBezierPath(roundedRect: NSRect(x: 50, y: 50, width: 924, height: 924), xRadius: 215, yRadius: 215)
        NSGradient(starting: NSColor(srgbRed: 0.40, green: 0.49, blue: 0.96, alpha: 1),
                   ending: NSColor(srgbRed: 0.24, green: 0.31, blue: 0.78, alpha: 1))!.draw(in: background, angle: -70)
        NSColor.white.withAlphaComponent(0.32).setFill()
        NSBezierPath(roundedRect: NSRect(x: 250, y: 405, width: 570, height: 380), xRadius: 55, yRadius: 55).fill()
        NSColor.white.setFill()
        NSBezierPath(roundedRect: NSRect(x: 170, y: 260, width: 570, height: 380), xRadius: 55, yRadius: 55).fill()
        NSColor(srgbRed: 0.31, green: 0.40, blue: 0.87, alpha: 1).setStroke()
        let arrow = NSBezierPath(); arrow.lineWidth = 40; arrow.lineCapStyle = .round; arrow.lineJoinStyle = .round
        arrow.move(to: NSPoint(x: 310, y: 450)); arrow.line(to: NSPoint(x: 600, y: 450))
        arrow.move(to: NSPoint(x: 505, y: 545)); arrow.line(to: NSPoint(x: 600, y: 450)); arrow.line(to: NSPoint(x: 505, y: 355)); arrow.stroke()
        image.unlockFocus()
        let bitmap = NSBitmapImageRep(data: image.tiffRepresentation!)!
        let suffix = scale == 2 ? "@2x" : ""
        try bitmap.representation(using: .png, properties: [:])!.write(to: URL(fileURLWithPath: "\(destination)/icon_\(points)x\(points)\(suffix).png"))
    }
}
