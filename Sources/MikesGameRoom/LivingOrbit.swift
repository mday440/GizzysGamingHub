import Foundation
import SwiftUI
import AppKit
import GameController
import CoreImage

// The living orbit: planets grow with playtime and glow by how recently they were played, and the sky follows
// the time of day. Controller identity: a DualSense's light bar takes the selected game's colour.

// MARK: a game's colour

@MainActor
enum GameColor {
    private static var cache: [String: NSColor] = [:]

    /// The average colour of the game's art, brightened to read as a light; else its planet colour.
    static func of(_ e: LibraryEntry) -> NSColor {
        let key = e.id + "|" + (e.coverArt ?? "")
        if let c = cache[key] { return c }
        var c = NSColor(Orbit.planetColor(e.info.name))
        if let img = entryImage(e), let tiff = img.tiffRepresentation, let ci = CIImage(data: tiff) {
            let f = CIFilter(name: "CIAreaAverage", parameters: [kCIInputImageKey: ci, kCIInputExtentKey: CIVector(cgRect: ci.extent)])
            var px = [UInt8](repeating: 0, count: 4)
            if let out = f?.outputImage {
                CIContext(options: [.workingColorSpace: NSNull()]).render(out, toBitmap: &px, rowBytes: 4, bounds: CGRect(x: 0, y: 0, width: 1, height: 1), format: .RGBA8, colorSpace: nil)
                let avg = NSColor(red: CGFloat(px[0]) / 255, green: CGFloat(px[1]) / 255, blue: CGFloat(px[2]) / 255, alpha: 1)
                var h: CGFloat = 0, s: CGFloat = 0, b: CGFloat = 0, a: CGFloat = 0
                avg.usingColorSpace(.deviceRGB)?.getHue(&h, saturation: &s, brightness: &b, alpha: &a)
                c = NSColor(hue: h, saturation: max(s, 0.55), brightness: max(b, 0.85), alpha: 1)
            }
        }
        cache[key] = c
        return c
    }
}

// MARK: controller lights

@MainActor
enum ControllerLights {
    /// Sets every connected controller's light (DualSense / DualShock light bars) to the colour.
    static func set(_ color: NSColor) {
        guard let rgb = color.usingColorSpace(.deviceRGB) else { return }
        for c in GCController.controllers() {
            c.light?.color = GCColor(red: Float(rgb.redComponent), green: Float(rgb.greenComponent), blue: Float(rgb.blueComponent))
        }
    }

    /// Flashes one controller's light so the player can tell which it is.
    static func identify(_ c: GCController) {
        guard let light = c.light else { return }
        let original = light.color
        Task { @MainActor in
            for _ in 0..<3 {
                light.color = GCColor(red: 1, green: 1, blue: 1)
                try? await Task.sleep(nanoseconds: 180_000_000)
                light.color = GCColor(red: 0, green: 0, blue: 0)
                try? await Task.sleep(nanoseconds: 180_000_000)
            }
            light.color = original
        }
    }
}

// MARK: playtime and recency

extension LibraryEntry {
    var hoursPlayed: Double { (playSeconds ?? 0) / 3600 }
    /// 1 for a game played today, fading to 0 over about a month.
    var recency: Double {
        guard let d = lastPlayed else { return 0 }
        return exp(-Date().timeIntervalSince(d) / 86_400 / 10)
    }
    /// Planet size factor: 0.85 for an unplayed game up to 1.45 at about 40 hours.
    var growth: Double { 0.85 + 0.6 * min(1, log(1 + hoursPlayed) / log(41)) }
}

func formatHours(_ seconds: Double?) -> String {
    let s = seconds ?? 0
    if s < 60 { return "Not yet" }
    return formatMinutes(s / 60)
}

// MARK: the sky by time of day

/// A soft wash over the night sky: dawn glow in the morning, a lighter sky by day, dusk colours in the evening.
struct SkyTint: View {
    var body: some View {
        TimelineView(.periodic(from: .now, by: 300)) { tl in
            let h = Calendar.current.component(.hour, from: tl.date) + Calendar.current.component(.minute, from: tl.date) / 60
            ZStack {
                LinearGradient(colors: [Color.clear, glow(h)], startPoint: .top, endPoint: .bottom)
                LinearGradient(colors: [day(h), Color.clear], startPoint: .top, endPoint: .center)
            }
            .allowsHitTesting(false)
            .animation(.easeInOut(duration: 3), value: h)
        }
    }
    /// Horizon glow: peach at dawn, pink-lavender at dusk.
    private func glow(_ h: Int) -> Color {
        switch h {
        case 5..<8: return Color(rgb: 0xFF9E7A).opacity(0.16)
        case 17..<20: return Color(rgb: 0xE58BD8).opacity(0.14)
        default: return .clear
        }
    }
    /// Daytime lifts the top of the sky.
    private func day(_ h: Int) -> Color {
        switch h {
        case 8..<17: return Color(rgb: 0x3A4BB0).opacity(0.22)
        case 7, 17: return Color(rgb: 0x3A4BB0).opacity(0.1)
        default: return .clear
        }
    }
}

/// How bright the stars are: full at night, faint by day.
func starBrightness(_ date: Date = Date()) -> Double {
    switch Calendar.current.component(.hour, from: date) {
    case 9..<17: return 0.45
    case 7, 8, 17, 18: return 0.7
    default: return 1
    }
}
