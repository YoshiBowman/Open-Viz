// Draws Openviz's icon: the app's amber lamp, its beam through haze, and the pool of light it
// throws on a dark stage.
//
//   swiftc -O -o /tmp/make_icon launcher/mac/make_icon.swift && /tmp/make_icon out.png 1024
//
// launcher/build_icon.sh runs this and packs the sizes macOS wants into Openviz.icns.
import CoreGraphics
import Foundation
import ImageIO
import UniformTypeIdentifiers

let arguments = CommandLine.arguments
guard arguments.count >= 2 else {
    FileHandle.standardError.write("usage: make_icon <out.png> [size]\n".data(using: .utf8)!)
    exit(2)
}
let size = arguments.count > 2 ? Int(arguments[2]) ?? 1024 : 1024
let space = CGColorSpace(name: CGColorSpace.sRGB)!
let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0, space: space,
                        bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
context.scaleBy(x: CGFloat(size) / 1024, y: CGFloat(size) / 1024)    // everything below is on a 1024 grid, y upward

func color(_ hex: UInt32, _ alpha: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: space, components: [CGFloat((hex >> 16) & 0xFF) / 255, CGFloat((hex >> 8) & 0xFF) / 255, CGFloat(hex & 0xFF) / 255, alpha])!
}
func gradient(_ stops: [(CGFloat, CGColor)]) -> CGGradient {
    CGGradient(colorsSpace: space, colors: stops.map { $0.1 } as CFArray, locations: stops.map { $0.0 })!
}

let amber: UInt32 = 0xF2B34A      // the lamp in the app's sidebar
let warm: UInt32 = 0xFFE3A8

// The tile: macOS's rounded square, 824 across on a 1024 canvas, with the soft shadow icons carry.
let tile = CGPath(roundedRect: CGRect(x: 100, y: 100, width: 824, height: 824), cornerWidth: 186, cornerHeight: 186, transform: nil)
context.saveGState()
context.setShadow(offset: CGSize(width: 0, height: -12), blur: 28, color: color(0x000000, 0.45))
context.addPath(tile)
context.setFillColor(color(0x15161A))
context.fillPath()
context.restoreGState()

context.saveGState()
context.addPath(tile)
context.clip()

// A dark stage: nearly black at the floor, a little lighter up in the rig.
context.drawLinearGradient(gradient([(0, color(0x0B0C0E)), (0.55, color(0x15171B)), (1, color(0x262A32))]),
                           start: CGPoint(x: 512, y: 100), end: CGPoint(x: 512, y: 924), options: [])

let lamp = CGPoint(x: 512, y: 742)
let floorY: CGFloat = 300
let poolRadius = CGSize(width: 262, height: 66)

// Haze around the beam: a wide, faint glow so the beam does not look cut out.
context.saveGState()
context.translateBy(x: 512, y: 520)
context.scaleBy(x: 1, y: 1.25)
context.drawRadialGradient(gradient([(0, color(amber, 0.20)), (1, color(amber, 0))]), startCenter: .zero, startRadius: 0,
                           endCenter: .zero, endRadius: 330, options: [])
context.restoreGState()

// The beam: narrow at the lamp, as wide as the pool at the floor, thinning as it falls.
func beam(spread: CGFloat) -> CGPath {
    let path = CGMutablePath()
    path.move(to: CGPoint(x: lamp.x - 16 * spread, y: lamp.y))
    path.addLine(to: CGPoint(x: lamp.x + 16 * spread, y: lamp.y))
    path.addLine(to: CGPoint(x: 512 + poolRadius.width * spread, y: floorY))
    path.addLine(to: CGPoint(x: 512 - poolRadius.width * spread, y: floorY))
    path.closeSubpath()
    return path
}
let beamLayers: [(CGFloat, CGFloat)] = [(1.0, 0.34), (0.72, 0.30), (0.40, 0.34)]    // a brighter core inside a softer cone
for (spread, strength) in beamLayers {
    context.saveGState()
    context.addPath(beam(spread: spread))
    context.clip()
    context.drawLinearGradient(gradient([(0, color(warm, strength * 1.6)), (0.5, color(amber, strength)), (1, color(amber, strength * 0.35))]),
                               start: lamp, end: CGPoint(x: 512, y: floorY), options: [])
    context.restoreGState()
}

// The pool of light on the floor, and its hot centre.
let poolLayers: [(CGFloat, [(CGFloat, CGColor)])] = [
    (1.0, [(0, color(warm, 0.95)), (0.55, color(amber, 0.75)), (1, color(amber, 0))]),
    (0.5, [(0, color(0xFFFFFF, 0.85)), (1, color(warm, 0))]),
]
for (scale, stops) in poolLayers {
    context.saveGState()
    context.translateBy(x: 512, y: floorY)
    context.scaleBy(x: 1, y: poolRadius.height / poolRadius.width)
    context.drawRadialGradient(gradient(stops), startCenter: .zero, startRadius: 0, endCenter: .zero,
                               endRadius: poolRadius.width * 1.18 * scale, options: [])
    context.restoreGState()
}

// The lamp: the amber dot, glowing.
context.saveGState()
context.drawRadialGradient(gradient([(0, color(amber, 0.55)), (1, color(amber, 0))]), startCenter: lamp, startRadius: 0,
                           endCenter: lamp, endRadius: 150, options: [])
context.setShadow(offset: .zero, blur: 46, color: color(amber, 0.95))
context.setFillColor(color(amber))
context.fillEllipse(in: CGRect(x: lamp.x - 44, y: lamp.y - 44, width: 88, height: 88))
context.restoreGState()
context.drawRadialGradient(gradient([(0, color(0xFFF4DC)), (0.6, color(0xFFD27A)), (1, color(amber))]),
                           startCenter: CGPoint(x: lamp.x - 8, y: lamp.y + 10), startRadius: 0, endCenter: lamp, endRadius: 44, options: [])
context.restoreGState()    // the end of what is clipped to the tile

// A hairline of light along the tile's top edge, as the system's own dark icons have.
context.saveGState()
context.addPath(tile)
context.clip()
context.addPath(CGPath(roundedRect: CGRect(x: 101.5, y: 101.5, width: 821, height: 821), cornerWidth: 184.5, cornerHeight: 184.5, transform: nil))
context.setStrokeColor(color(0xFFFFFF, 0.07))
context.setLineWidth(3)
context.strokePath()
context.restoreGState()

let image = context.makeImage()!
let destination = CGImageDestinationCreateWithURL(URL(fileURLWithPath: arguments[1]) as CFURL, UTType.png.identifier as CFString, 1, nil)!
CGImageDestinationAddImage(destination, image, nil)
guard CGImageDestinationFinalize(destination) else {
    FileHandle.standardError.write("could not write \(arguments[1])\n".data(using: .utf8)!)
    exit(1)
}
