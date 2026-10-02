// Render an SVG to an N×N 8-bit alpha map and print it as a C array.  usage: swift svg_alpha.swift file.svg NAME N
import AppKit
let a = CommandLine.arguments
var svg = try! String(contentsOfFile: a[1], encoding: .utf8).replacingOccurrences(of: "currentColor", with: "#000")
svg = svg.replacingOccurrences(of: "height=\"1em\"", with: "height=\"24\"").replacingOccurrences(of: "width=\"1em\"", with: "width=\"24\"")
let n = Int(a[3])!
let img = NSImage(data: svg.data(using: .utf8)!)!
let rep = NSBitmapImageRep(bitmapDataPlanes: nil, pixelsWide: n, pixelsHigh: n, bitsPerSample: 8, samplesPerPixel: 4,
                           hasAlpha: true, isPlanar: false, colorSpaceName: .deviceRGB, bytesPerRow: n * 4, bitsPerPixel: 32)!
NSGraphicsContext.saveGraphicsState()
NSGraphicsContext.current = NSGraphicsContext(bitmapImageRep: rep)
NSGraphicsContext.current!.imageInterpolation = .high
img.draw(in: NSRect(x: 0, y: 0, width: n, height: n))
NSGraphicsContext.restoreGraphicsState()
var out = "static const uint8_t \(a[2])[\(n * n)] = {"
for y in 0..<n { out += "\n  "; for x in 0..<n { out += "\(Int((rep.colorAt(x: x, y: y)!.alphaComponent * 255).rounded())),"  } }
print(out + "\n};")
