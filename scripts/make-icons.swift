// Generates every Ridge Racer Marathon app icon from one vector design.
//
//   swift scripts/make-icons.swift [repo-root]      (normally run via scripts/make-icons.sh)
//
// The emblem is original artwork: a waving yellow/black checkered flag, an italic "RR"
// monogram and a "MARATHON" plate, drawn with CoreGraphics and the bundled Rajdhani Bold
// font (SIL OFL, assets/fonts). No third-party logos or game artwork are used.
//
// Outputs (paths relative to the repo root):
//   platform/icons/marathon-1024.png                       master, full bleed, opaque
//   platform/icons/iconset/*.png                           macOS iconset (iconutil -> .icns)
//   platform/windows/marathon.ico                          16..256 px
//   platform/linux/io.github.ridgeracermarathon.png        256 px
//   platform/ios/Assets.xcassets/AppIcon.appiconset/*      single 1024 px entry
//   platform/android/app/src/main/res/mipmap-*/ic_launcher_{foreground,background,monochrome}.png

import CoreGraphics
import CoreText
import Foundation
import ImageIO
import UniformTypeIdentifiers

// MARK: - setup

let root = URL(fileURLWithPath: CommandLine.arguments.count > 1 ? CommandLine.arguments[1]
                                                                 : FileManager.default.currentDirectoryPath)
let fontURL = root.appendingPathComponent("assets/fonts/Rajdhani-Bold.ttf")
guard let descs = CTFontManagerCreateFontDescriptorsFromURL(fontURL as CFURL) as? [CTFontDescriptor],
      let fontDesc = descs.first else {
    FileHandle.standardError.write("error: cannot load \(fontURL.path)\n".data(using: .utf8)!)
    exit(1)
}
func font(_ size: CGFloat) -> CTFont { CTFontCreateWithFontDescriptor(fontDesc, size, nil) }

let srgb = CGColorSpace(name: CGColorSpace.sRGB)!
func rgb(_ hex: UInt32, _ a: CGFloat = 1) -> CGColor {
    CGColor(colorSpace: srgb, components: [CGFloat((hex >> 16) & 0xFF) / 255, CGFloat((hex >> 8) & 0xFF) / 255,
                                           CGFloat(hex & 0xFF) / 255, a])!
}

let yellow = rgb(0xFFC400)
let yellowHi = rgb(0xFFE45C)
let yellowLo = rgb(0xF2A900)
let ink = rgb(0x0B0B0D)
let extrude = rgb(0x6B4300)

func makeContext(_ w: Int, _ h: Int, opaque: Bool = false) -> CGContext {
    let info = opaque ? CGImageAlphaInfo.noneSkipLast.rawValue : CGImageAlphaInfo.premultipliedLast.rawValue
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: 0, space: srgb,
                        bitmapInfo: info)!
    ctx.setShouldAntialias(true)
    ctx.interpolationQuality = .high
    return ctx
}

// The design is authored on a 1024x1024 canvas with a top-left origin.
// D(ctx, rect) maps that canvas into `rect` (CoreGraphics coordinates).
func withDesignSpace(_ ctx: CGContext, _ rect: CGRect, _ body: () -> Void) {
    ctx.saveGState()
    ctx.translateBy(x: rect.minX, y: rect.maxY)
    ctx.scaleBy(x: rect.width / 1024, y: -rect.height / 1024)
    body()
    ctx.restoreGState()
}

// Text -> CGPath in design space (top-left origin, y down), baseline at y = 0.
func textPath(_ text: String, size: CGFloat, tracking: CGFloat = 0) -> CGPath {
    let f = font(size)
    let attr = NSAttributedString(string: text, attributes: [kCTFontAttributeName as NSAttributedString.Key: f,
                                                              kCTKernAttributeName as NSAttributedString.Key: tracking])
    let line = CTLineCreateWithAttributedString(attr)
    let path = CGMutablePath()
    for run in CTLineGetGlyphRuns(line) as! [CTRun] {
        let n = CTRunGetGlyphCount(run)
        var glyphs = [CGGlyph](repeating: 0, count: n)
        var pos = [CGPoint](repeating: .zero, count: n)
        CTRunGetGlyphs(run, CFRange(location: 0, length: n), &glyphs)
        CTRunGetPositions(run, CFRange(location: 0, length: n), &pos)
        for i in 0..<n {
            guard let g = CTFontCreatePathForGlyph(f, glyphs[i], nil) else { continue }
            // Flip glyph (y up) into design space (y down).
            let t = CGAffineTransform(a: 1, b: 0, c: 0, d: -1, tx: pos[i].x, ty: -pos[i].y)
            path.addPath(g, transform: t)
        }
    }
    return path
}

// Italic shear around the vertical centre of `rect` (design space, y down: top leans right).
func italic(_ p: CGPath, k: CGFloat = 0.2, pivotY: CGFloat) -> CGPath {
    var t = CGAffineTransform(a: 1, b: 0, c: -k, d: 1, tx: k * pivotY, ty: 0)
    return p.copy(using: &t)!
}

func translated(_ p: CGPath, _ dx: CGFloat, _ dy: CGFloat) -> CGPath {
    var t = CGAffineTransform(translationX: dx, y: dy)
    return p.copy(using: &t)!
}

func fillGradient(_ ctx: CGContext, _ path: CGPath, _ colors: [CGColor], from: CGPoint, to: CGPoint) {
    ctx.saveGState()
    ctx.addPath(path)
    ctx.clip()
    let g = CGGradient(colorsSpace: srgb, colors: colors as CFArray, locations: nil)!
    ctx.drawLinearGradient(g, start: from, end: to, options: [.drawsBeforeStartLocation, .drawsAfterEndLocation])
    ctx.restoreGState()
}

// MARK: - design elements (all in design space)

func drawBackground(_ ctx: CGContext) {
    let full = CGRect(x: 0, y: 0, width: 1024, height: 1024)
    fillGradient(ctx, CGPath(rect: full, transform: nil), [rgb(0x23232A), rgb(0x08080A)],
                 from: CGPoint(x: 0, y: 0), to: CGPoint(x: 0, y: 1024))
    // Warm glow behind the monogram.
    let glow = CGGradient(colorsSpace: srgb, colors: [rgb(0xFFC400, 0.30), rgb(0xFFC400, 0.0)] as CFArray,
                          locations: [0, 1])!
    ctx.saveGState()
    ctx.addRect(full)
    ctx.clip()
    ctx.drawRadialGradient(glow, startCenter: CGPoint(x: 512, y: 470), startRadius: 0,
                           endCenter: CGPoint(x: 512, y: 470), endRadius: 560, options: [])
    ctx.restoreGState()
}

// A waving checkered flag band running diagonally behind the monogram.
func drawCheckerBand(_ ctx: CGContext) {
    let s: CGFloat = 64          // square size
    let rows = 4
    let cols = 30
    let angle: CGFloat = -0.30   // radians, rising to the right
    let centre = CGPoint(x: 512, y: 455)
    let amp: CGFloat = 26, wave: CGFloat = 520
    func warp(_ u: CGFloat, _ v: CGFloat) -> CGPoint {
        // u along the band, v across it; flag wave displaces v.
        let vv = v + amp * sin(2 * .pi * u / wave)
        let x = u * cos(angle) - vv * sin(angle)
        let y = u * sin(angle) + vv * cos(angle)
        return CGPoint(x: centre.x + x, y: centre.y + y)
    }
    let u0 = -CGFloat(cols) * s / 2
    let v0 = -CGFloat(rows) * s / 2
    ctx.saveGState()
    for r in 0..<rows {
        for c in 0..<cols where (r + c) % 2 == 0 {
            let u = u0 + CGFloat(c) * s, v = v0 + CGFloat(r) * s
            let p = CGMutablePath()
            let steps = 6
            p.move(to: warp(u, v))
            for i in 1...steps { p.addLine(to: warp(u + s * CGFloat(i) / CGFloat(steps), v)) }
            for i in 0...steps { p.addLine(to: warp(u + s - s * CGFloat(i) / CGFloat(steps), v + s)) }
            p.closeSubpath()
            // Shade by the slope of the wave so the flag looks like it ripples.
            let shade = 0.80 + 0.20 * cos(2 * .pi * (u + s / 2) / wave)
            ctx.addPath(p)
            ctx.setFillColor(rgb(0xFFC400, 0.92 * shade))
            ctx.fillPath()
        }
    }
    // Edge stripes above and below the band.
    for (v, w) in [(v0 - 22, CGFloat(10)), (v0 + CGFloat(rows) * s + 12, CGFloat(10))] {
        let p = CGMutablePath()
        let start = u0 - s, end = -u0 + s
        var u = start
        p.move(to: warp(u, v))
        while u < end { u += 16; p.addLine(to: warp(u, v)) }
        while u > start { p.addLine(to: warp(u, v + w)); u -= 16 }
        p.addLine(to: warp(start, v + w))
        p.closeSubpath()
        ctx.addPath(p)
        ctx.setFillColor(rgb(0xFFC400, 0.85))
        ctx.fillPath()
    }
    ctx.restoreGState()
}

// "RR" monogram: two overlapping italic letters with an extruded 3-D edge.
// mono = flat white silhouette (Android themed icons).
func drawMonogram(_ ctx: CGContext, scale: CGFloat = 1, centreY: CGFloat = 455, mono: Bool = false) {
    let capRatio = CTFontGetCapHeight(font(100)) / 100
    let size = 470 / capRatio * scale
    let baseline = centreY + 470 * scale / 2
    let letter = textPath("R", size: size)
    let lb = letter.boundingBoxOfPath
    let overlap = lb.width * 0.20
    let total = lb.width * 2 - overlap
    let x0 = 512 - total / 2 - lb.minX + 18 * scale
    let paths = [0, 1].map { i -> CGPath in
        let p = translated(letter, x0 + CGFloat(i) * (lb.width - overlap), baseline)
        return italic(p, k: 0.22, pivotY: centreY)
    }
    let depth = Int(22 * scale)
    for p in paths {
        if mono {
            ctx.addPath(p)
            ctx.setFillColor(rgb(0xFFFFFF))
            ctx.fillPath()
            continue
        }
        // Extrusion towards bottom-right.
        for d in stride(from: depth, through: 1, by: -1) {
            ctx.addPath(translated(p, CGFloat(d) * 0.55, CGFloat(d)))
            ctx.setFillColor(d == depth ? ink : extrude)
            ctx.fillPath()
        }
        // Thick ink outline (drawn at 2x width, the fill covers the inner half).
        ctx.addPath(p)
        ctx.setStrokeColor(ink)
        ctx.setLineWidth(30 * scale)
        ctx.setLineJoin(.round)
        ctx.strokePath()
        let b = p.boundingBoxOfPath
        fillGradient(ctx, p, [yellowHi, yellow, yellowLo], from: CGPoint(x: b.midX, y: b.minY),
                     to: CGPoint(x: b.midX, y: b.maxY))
        // Inner highlight line along the top edge of each letter.
        ctx.saveGState()
        ctx.addPath(p)
        ctx.clip()
        ctx.addPath(translated(p, 0, 7 * scale))
        ctx.setStrokeColor(rgb(0xFFFFFF, 0.35))
        ctx.setLineWidth(5 * scale)
        ctx.strokePath()
        ctx.restoreGState()
    }
}

// "MARATHON" plate: a slanted yellow bar with black lettering.
func drawPlate(_ ctx: CGContext, scale: CGFloat = 1, centreY: CGFloat = 820, mono: Bool = false) {
    let h = 118 * scale, w = 780 * scale
    let top = centreY - h / 2
    let slant = h * 0.30
    let plate = CGMutablePath()
    plate.move(to: CGPoint(x: 512 - w / 2 + slant, y: top))
    plate.addLine(to: CGPoint(x: 512 + w / 2 + slant, y: top))
    plate.addLine(to: CGPoint(x: 512 + w / 2 - slant, y: top + h))
    plate.addLine(to: CGPoint(x: 512 - w / 2 - slant, y: top + h))
    plate.closeSubpath()

    let capRatio = CTFontGetCapHeight(font(100)) / 100
    let textSize = 74 * scale / capRatio
    var text = textPath("MARATHON", size: textSize, tracking: 14 * scale)
    let tb = text.boundingBoxOfPath
    text = translated(text, 512 - tb.midX, centreY + 74 * scale / 2)
    text = italic(text, k: 0.22, pivotY: centreY)

    if mono {
        ctx.saveGState()
        ctx.addPath(plate)
        ctx.setFillColor(rgb(0xFFFFFF))
        ctx.fillPath()
        ctx.setBlendMode(.clear)
        ctx.addPath(text)
        ctx.fillPath()
        ctx.restoreGState()
        return
    }
    ctx.addPath(translated(plate, 8 * scale, 10 * scale))
    ctx.setFillColor(rgb(0x000000, 0.55))
    ctx.fillPath()
    ctx.addPath(plate)
    ctx.setStrokeColor(ink)
    ctx.setLineWidth(16 * scale)
    ctx.setLineJoin(.miter)
    ctx.strokePath()
    fillGradient(ctx, plate, [yellowHi, yellow, yellowLo], from: CGPoint(x: 0, y: top), to: CGPoint(x: 0, y: top + h))
    ctx.addPath(text)
    ctx.setFillColor(ink)
    ctx.fillPath()
}

// Small speed streaks to the left of the monogram.
func drawStreaks(_ ctx: CGContext) {
    let lines: [(CGFloat, CGFloat, CGFloat)] = [(330, 70, 150), (380, 40, 210), (430, 85, 130)]
    for (y, x, len) in lines {
        let p = CGMutablePath()
        p.move(to: CGPoint(x: x, y: y))
        p.addLine(to: CGPoint(x: x + len, y: y))
        ctx.addPath(italic(p, k: 0.22, pivotY: 455))
        ctx.setStrokeColor(rgb(0xFFC400, 0.65))
        ctx.setLineWidth(12)
        ctx.setLineCap(.round)
        ctx.strokePath()
    }
}

enum Detail { case full, simple }

// The complete full-bleed emblem.
func drawEmblem(_ ctx: CGContext, _ rect: CGRect, detail: Detail) {
    withDesignSpace(ctx, rect) {
        drawBackground(ctx)
        switch detail {
        case .full:
            drawCheckerBand(ctx)
            drawStreaks(ctx)
            drawMonogram(ctx)
            drawPlate(ctx)
        case .simple:
            // Small sizes: a bigger monogram and a plain bar, no fine detail.
            drawMonogram(ctx, scale: 1.25, centreY: 470)
            let bar = CGRect(x: 120, y: 840, width: 784, height: 70)
            ctx.addPath(CGPath(rect: bar, transform: nil))
            ctx.setFillColor(yellow)
            ctx.fillPath()
        }
    }
}

// MARK: - output helpers

func writePNG(_ image: CGImage, _ url: URL) {
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    let dest = CGImageDestinationCreateWithURL(url as CFURL, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, image, nil)
    if !CGImageDestinationFinalize(dest) { fatalError("cannot write \(url.path)") }
}

func pngData(_ image: CGImage) -> Data {
    let data = NSMutableData()
    let dest = CGImageDestinationCreateWithData(data as CFMutableData, UTType.png.identifier as CFString, 1, nil)!
    CGImageDestinationAddImage(dest, image, nil)
    CGImageDestinationFinalize(dest)
    return data as Data
}

func continuousRoundedRect(_ r: CGRect, radius: CGFloat) -> CGPath {
    CGPath(roundedRect: r, cornerWidth: radius, cornerHeight: radius, transform: nil)
}

// Full-bleed square (iOS, master).
func renderSquare(_ px: Int, detail: Detail) -> CGImage {
    let ctx = makeContext(px, px, opaque: true)
    drawEmblem(ctx, CGRect(x: 0, y: 0, width: px, height: px), detail: detail)
    return ctx.makeImage()!
}

// macOS Big Sur style: 824/1024 rounded body with a soft shadow on a transparent canvas.
func renderMac(_ px: Int) -> CGImage {
    let ctx = makeContext(px, px)
    let s = CGFloat(px) / 1024
    let body = CGRect(x: 100 * s, y: 100 * s + 12 * s, width: 824 * s, height: 824 * s)
    let shape = continuousRoundedRect(body, radius: 185 * s)
    ctx.saveGState()
    ctx.setShadow(offset: CGSize(width: 0, height: -10 * s), blur: 24 * s, color: rgb(0x000000, 0.45))
    ctx.addPath(shape)
    ctx.setFillColor(ink)
    ctx.fillPath()
    ctx.restoreGState()
    ctx.saveGState()
    ctx.addPath(shape)
    ctx.clip()
    drawEmblem(ctx, body, detail: px >= 64 ? .full : .simple)
    ctx.restoreGState()
    return ctx.makeImage()!
}

// Windows / Linux: rounded square with a small transparent margin.
func renderRounded(_ px: Int) -> CGImage {
    let ctx = makeContext(px, px)
    let s = CGFloat(px) / 1024
    let inset: CGFloat = px <= 32 ? 0 : 24 * s
    let body = CGRect(x: inset, y: inset, width: CGFloat(px) - 2 * inset, height: CGFloat(px) - 2 * inset)
    ctx.addPath(continuousRoundedRect(body, radius: (px <= 32 ? 120 : 190) * s))
    ctx.clip()
    drawEmblem(ctx, body, detail: px >= 64 ? .full : .simple)
    return ctx.makeImage()!
}

// Android adaptive icon layers (108dp canvas; the launcher masks to ~66-72dp).
func renderAndroidBackground(_ px: Int) -> CGImage {
    let ctx = makeContext(px, px, opaque: true)
    withDesignSpace(ctx, CGRect(x: 0, y: 0, width: px, height: px)) {
        drawBackground(ctx)
        drawCheckerBand(ctx)
    }
    return ctx.makeImage()!
}

func renderAndroidForeground(_ px: Int, mono: Bool) -> CGImage {
    let ctx = makeContext(px, px)
    withDesignSpace(ctx, CGRect(x: 0, y: 0, width: px, height: px)) {
        // Keep the artwork inside the 66dp safe circle: scale the 1024 design about the centre.
        let k: CGFloat = 0.56
        ctx.translateBy(x: 512 * (1 - k), y: 512 * (1 - k) - 18)
        ctx.scaleBy(x: k, y: k)
        drawMonogram(ctx, centreY: 430, mono: mono)
        drawPlate(ctx, centreY: 830, mono: mono)
    }
    return ctx.makeImage()!
}

// MARK: - ICO writer (BMP/DIB entries up to 128 px, PNG for 256 px)

func dibEntry(_ image: CGImage) -> Data {
    let w = image.width, h = image.height
    // Render to straight (non-premultiplied would be ideal; premultiplied is fine for icons) BGRA.
    let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4, space: srgb,
                        bitmapInfo: CGImageAlphaInfo.premultipliedFirst.rawValue | CGBitmapInfo.byteOrder32Little.rawValue)!
    ctx.draw(image, in: CGRect(x: 0, y: 0, width: w, height: h))
    let src = ctx.data!.bindMemory(to: UInt8.self, capacity: w * h * 4)
    var d = Data()
    func u32(_ v: UInt32) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 4)) }
    func u16(_ v: UInt16) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 2)) }
    u32(40); u32(UInt32(w)); u32(UInt32(h * 2)); u16(1); u16(32); u32(0)
    let maskStride = ((w + 31) / 32) * 4
    u32(UInt32(w * h * 4 + maskStride * h)); u32(0); u32(0); u32(0); u32(0)
    // Pixel rows bottom-up. CGContext memory is top-down. Un-premultiply for the icon format.
    for y in (0..<h).reversed() {
        for x in 0..<w {
            let i = (y * w + x) * 4
            var b = src[i], g = src[i + 1], r = src[i + 2]
            let a = src[i + 3]
            if a > 0 && a < 255 {
                b = UInt8(min(255, Int(b) * 255 / Int(a)))
                g = UInt8(min(255, Int(g) * 255 / Int(a)))
                r = UInt8(min(255, Int(r) * 255 / Int(a)))
            }
            d.append(contentsOf: [b, g, r, a])
        }
    }
    // AND mask: 1 = transparent.
    for y in (0..<h).reversed() {
        var row = [UInt8](repeating: 0, count: maskStride)
        for x in 0..<w where src[(y * w + x) * 4 + 3] == 0 { row[x / 8] |= UInt8(0x80 >> (x % 8)) }
        d.append(contentsOf: row)
    }
    return d
}

func writeICO(_ sizes: [Int], _ url: URL) {
    var images: [(Int, Data)] = []
    for px in sizes {
        let img = renderRounded(px)
        images.append((px, px >= 256 ? pngData(img) : dibEntry(img)))
    }
    var d = Data()
    func u32(_ v: UInt32) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 4)) }
    func u16(_ v: UInt16) { var x = v.littleEndian; d.append(Data(bytes: &x, count: 2)) }
    u16(0); u16(1); u16(UInt16(images.count))
    var offset = 6 + 16 * images.count
    for (px, data) in images {
        d.append(UInt8(px >= 256 ? 0 : px)); d.append(UInt8(px >= 256 ? 0 : px))
        d.append(0); d.append(0)
        u16(1); u16(32)
        u32(UInt32(data.count)); u32(UInt32(offset))
        offset += data.count
    }
    for (_, data) in images { d.append(data) }
    try? FileManager.default.createDirectory(at: url.deletingLastPathComponent(), withIntermediateDirectories: true)
    try! d.write(to: url)
}

// MARK: - main

func out(_ rel: String) -> URL { root.appendingPathComponent(rel) }

// Master + iOS
let master = renderSquare(1024, detail: .full)
writePNG(master, out("platform/icons/marathon-1024.png"))
writePNG(master, out("platform/ios/Assets.xcassets/AppIcon.appiconset/AppIcon-1024.png"))
try! """
{
  "images" : [
    {
      "filename" : "AppIcon-1024.png",
      "idiom" : "universal",
      "platform" : "ios",
      "size" : "1024x1024"
    }
  ],
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}

""".write(to: out("platform/ios/Assets.xcassets/AppIcon.appiconset/Contents.json"), atomically: true, encoding: .utf8)
try! """
{
  "info" : {
    "author" : "xcode",
    "version" : 1
  }
}

""".write(to: out("platform/ios/Assets.xcassets/Contents.json"), atomically: true, encoding: .utf8)

// macOS iconset (iconutil turns it into Marathon.icns)
for (name, px) in [("icon_16x16", 16), ("icon_16x16@2x", 32), ("icon_32x32", 32), ("icon_32x32@2x", 64),
                   ("icon_128x128", 128), ("icon_128x128@2x", 256), ("icon_256x256", 256),
                   ("icon_256x256@2x", 512), ("icon_512x512", 512), ("icon_512x512@2x", 1024)] {
    writePNG(renderMac(px), out("platform/icons/Marathon.iconset/\(name).png"))
}

// Windows + Linux
writeICO([16, 20, 24, 32, 40, 48, 64, 96, 128, 256], out("platform/windows/marathon.ico"))
writePNG(renderRounded(256), out("platform/linux/io.github.ridgeracermarathon.png"))

// Android adaptive icon layers
for (dpi, scale) in [("mdpi", 1.0), ("hdpi", 1.5), ("xhdpi", 2.0), ("xxhdpi", 3.0), ("xxxhdpi", 4.0)] {
    let px = Int(108 * scale)
    let dir = "platform/android/app/src/main/res/mipmap-\(dpi)"
    writePNG(renderAndroidBackground(px), out("\(dir)/ic_launcher_background.png"))
    writePNG(renderAndroidForeground(px, mono: false), out("\(dir)/ic_launcher_foreground.png"))
    writePNG(renderAndroidForeground(px, mono: true), out("\(dir)/ic_launcher_monochrome.png"))
}
// Store listing / README preview
writePNG(renderRounded(512), out("platform/icons/marathon-512-rounded.png"))
print("icons written under \(root.path)/platform")
