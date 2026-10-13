// make_icon: a macOS app icon (.icns) from a square picture, in Apple's icon shape.
//
// usage: make_icon <picture.png> <out.icns>
//
// Every size an .icns holds (16 to 1024 pixels, @1x and @2x) is drawn from the picture: Apple's
// app icon grid (macOS 11 and later; macOS 26 puts icons of another shape on a gray square): a
// 1024 canvas with an 824-pixel rounded square (continuous corners, radius 185.4) 100 pixels in
// from each side, the picture filling it (cropped to square if it isn't), a soft shadow below.
// tools/launcher/build.sh compiles and runs it when it is given a picture.
import AppKit
import SwiftUI

let args = CommandLine.arguments
guard args.count == 3 else {
    FileHandle.standardError.write("usage: make_icon <picture.png> <out.icns>\n".data(using: .utf8)!)
    exit(2)
}
guard let source = CGImageSourceCreateWithURL(URL(fileURLWithPath: args[1]) as CFURL, nil),
      let picture = CGImageSourceCreateImageAtIndex(source, 0, nil) else {
    FileHandle.standardError.write("make_icon: can't read \(args[1])\n".data(using: .utf8)!)
    exit(1)
}

func icon(size: Int) -> CGImage {
    let s = CGFloat(size)
    let context = CGContext(data: nil, width: size, height: size, bitsPerComponent: 8, bytesPerRow: 0,
                            space: CGColorSpace(name: CGColorSpace.sRGB)!,
                            bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue)!
    context.interpolationQuality = .high
    let body = CGRect(x: s * 100 / 1024, y: s * 100 / 1024, width: s * 824 / 1024, height: s * 824 / 1024)
    let shape = Path(roundedRect: body, cornerRadius: s * 185.4 / 1024, style: .continuous).cgPath
    // The shadow: the shape filled once with it, then covered by the picture.
    context.saveGState()
    context.setShadow(offset: CGSize(width: 0, height: -s * 10 / 1024), blur: s * 20 / 1024,
                      color: CGColor(gray: 0, alpha: 0.35))
    context.addPath(shape)
    context.setFillColor(CGColor(gray: 0, alpha: 1))
    context.fillPath()
    context.restoreGState()
    // The picture, cropped to its centre square, clipped to the shape.
    let side = min(picture.width, picture.height)
    let square = picture.cropping(to: CGRect(x: (picture.width - side) / 2, y: (picture.height - side) / 2,
                                             width: side, height: side))!
    context.addPath(shape)
    context.clip()
    context.draw(square, in: body)
    return context.makeImage()!
}

let iconset = URL(fileURLWithPath: NSTemporaryDirectory()).appendingPathComponent("make_icon-\(getpid()).iconset")
try? FileManager.default.removeItem(at: iconset)
try FileManager.default.createDirectory(at: iconset, withIntermediateDirectories: true)
for points in [16, 32, 128, 256, 512] {
    for scale in [1, 2] {
        let name = scale == 1 ? "icon_\(points)x\(points).png" : "icon_\(points)x\(points)@2x.png"
        let destination = CGImageDestinationCreateWithURL(iconset.appendingPathComponent(name) as CFURL,
                                                          "public.png" as CFString, 1, nil)!
        CGImageDestinationAddImage(destination, icon(size: points * scale), nil)
        CGImageDestinationFinalize(destination)
    }
}
let iconutil = Process()
iconutil.executableURL = URL(fileURLWithPath: "/usr/bin/iconutil")
iconutil.arguments = ["-c", "icns", iconset.path, "-o", args[2]]
try iconutil.run()
iconutil.waitUntilExit()
try? FileManager.default.removeItem(at: iconset)
exit(iconutil.terminationStatus)
