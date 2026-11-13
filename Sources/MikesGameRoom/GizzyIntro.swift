import SwiftUI
import AppKit
import AVFoundation
import GameController

// Gizzy's Gaming Hub launch intro (5 s):
//   stardust gathers into the emblem → it hardens into layered glass → the logo's star
//   flies a comet around the orbit ring and ignites → the name reveals → the camera dives
//   through the controller face of the G and the app appears, full screen.
// Connected DualSense / DualShock light bars follow the intro's colours.

// MARK: preferences

enum IntroPrefs {
    private static func flag(_ k: String) -> Bool { UserDefaults.standard.object(forKey: k) as? Bool ?? true }
    static var enabled: Bool { flag("intro.enabled") }
    static var sound: Bool { flag("intro.sound") }
    static var fullScreen: Bool { flag("intro.fullscreen") }
    static var lights: Bool { flag("intro.lights") }
}

extension Notification.Name {
    static let gizzyPlayIntro = Notification.Name("gizzyPlayIntro")
}

struct IntroSettings: View {
    @AppStorage("intro.enabled") private var enabled = true
    @AppStorage("intro.sound") private var sound = true
    @AppStorage("intro.lights") private var lights = true
    @AppStorage("intro.fullscreen") private var fullScreen = true

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Play the intro when the app opens", isOn: $enabled)
            Toggle("Intro sound", isOn: $sound).disabled(!enabled)
            Toggle("Controller light follows the intro", isOn: $lights).disabled(!enabled)
            Toggle("Open the app in full screen", isOn: $fullScreen)
            PreShowSetting()
            HStack(spacing: 12) {
                Button("Play it now") { NotificationCenter.default.post(name: .gizzyPlayIntro, object: nil) }
                Text("Any key, click or controller button skips it.")
                    .font(Orbit.body(12)).foregroundStyle(Orbit.dim)
            }
        }
        .font(Orbit.body(13))
    }
}

// MARK: math

private func clamp01(_ v: Double) -> Double { min(1, max(0, v)) }
private func prog(_ t: Double, _ a: Double, _ b: Double) -> Double { clamp01((t - a) / (b - a)) }
private func eIO(_ x: Double) -> Double { x < 0.5 ? 4 * x * x * x : 1 - pow(-2 * x + 2, 3) / 2 }
private func eOut(_ x: Double) -> Double { 1 - pow(1 - x, 3) }
private func eIn(_ x: Double) -> Double { x * x * x }
private func eBack(_ x: Double) -> Double { let c = 1.6; return 1 + (c + 1) * pow(x - 1, 3) + c * pow(x - 1, 2) }
private func lerp(_ a: Double, _ b: Double, _ t: Double) -> Double { a + (b - a) * t }

private struct IntroRNG {
    var s: UInt64
    mutating func next() -> Double {
        s = s &* 6364136223846793005 &+ 1442695040888963407
        return Double(s >> 11) / Double(UInt64(1) << 53)
    }
}

// MARK: art

enum IntroArt {
    static let emblem: NSImage? = load("IntroEmblem")
    static let letters: NSImage? = load("IntroLetters")
    static let emblemAspect: Double = {
        guard let e = emblem, e.size.width > 0 else { return 650.0 / 757.0 }
        return Double(e.size.height / e.size.width)
    }()
    static let lettersAspect: Double = {
        guard let l = letters, l.size.width > 0 else { return 394.0 / 1163.0 }
        return Double(l.size.height / l.size.width)
    }()
    private static func load(_ name: String) -> NSImage? {
        Bundle.main.url(forResource: name, withExtension: "png").flatMap { NSImage(contentsOf: $0) }
    }

    struct Speck { let fx, fy: Double; let color: Color; let angle, radius, spin, delay, size: Double }

    /// Stardust sampled from the emblem's own pixels, so every speck lands on its own colour.
    static let dust: [Speck] = {
        guard let cg = emblem?.cgImage(forProposedRect: nil, context: nil, hints: nil) else { return [] }
        let w = 190, h = max(1, Int((190 * emblemAspect).rounded()))
        var px = [UInt8](repeating: 0, count: w * h * 4)
        let ok: Bool = px.withUnsafeMutableBytes { buf in
            guard let ctx = CGContext(data: buf.baseAddress, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                      space: CGColorSpaceCreateDeviceRGB(),
                                      bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue) else { return false }
            ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
            return true
        }
        guard ok else { return [] }
        var pts: [(Double, Double, Double, Double, Double)] = []
        for y in 0..<h {
            for x in 0..<w {
                let i = (y * w + x) * 4
                let a = Double(px[i + 3])
                if a > 150 {
                    let un = 255.0 / a
                    pts.append((Double(x) / Double(w), Double(y) / Double(h),
                                Double(px[i]) * un, Double(px[i + 1]) * un, Double(px[i + 2]) * un))
                }
            }
        }
        guard !pts.isEmpty else { return [] }
        var r = IntroRNG(s: 9)
        return (0..<min(2200, pts.count)).map { _ in
            let p = pts[min(pts.count - 1, Int(r.next() * Double(pts.count)))]
            let color = Color(red: min(255, p.2 + 40) / 255, green: min(255, p.3 + 40) / 255, blue: min(255, p.4 + 30) / 255)
            return Speck(fx: p.0, fy: p.1, color: color,
                         angle: r.next() * .pi * 2, radius: 0.55 + r.next() * 0.75,
                         spin: (r.next() < 0.5 ? -1 : 1) * (1.6 + r.next() * 1.4),
                         delay: r.next() * 0.55, size: r.next() * 1.3 + 0.6)
        }
    }()

    struct Star { let x, y, z, phase, size: Double; let color: Color }
    static let stars: [Star] = {
        var r = IntroRNG(s: 42)
        let lilac = Color(red: 196 / 255, green: 181 / 255, blue: 253 / 255)
        let ice = Color(red: 165 / 255, green: 243 / 255, blue: 252 / 255)
        return (0..<520).map { _ in
            let c: Color = r.next() < 0.2 ? lilac : (r.next() < 0.3 ? ice : .white)
            let s = r.next() < 0.06 ? 1.8 : r.next() * 1.1 + 0.3
            return Star(x: r.next(), y: r.next(), z: r.next(), phase: r.next() * 6.28, size: s, color: c)
        }
    }()
}

// MARK: layout

private struct IntroLayout {
    static let slices = 16
    static let star = (x: 0.905, y: 0.085)     // star tip, fraction of the emblem
    static let face = (x: 0.52, y: 0.47)       // controller face inside the G (fly-through target)

    let size: CGSize
    let W, H, emW, emH, centerShift: Double

    init(size: CGSize) {
        self.size = size
        W = min(Double(size.width) * 0.86, Double(size.height) * 1.5)
        H = W * 0.3302
        emW = W * 0.385
        emH = emW * IntroArt.emblemAspect
        centerShift = W * 0.5 - emW * 0.5
    }
    var left: Double { Double(size.width) / 2 - W / 2 }
    func emblemRect(_ shift: Double) -> CGRect {
        CGRect(x: left + shift, y: Double(size.height) / 2 - H / 2 + (H - emH) / 2, width: emW, height: emH)
    }
}

// MARK: driver (clock, skip, lights, sound, full screen)

final class IntroDriver: ObservableObject {
    static let duration = 5.0
    private var startDate = Date.distantFuture
    private var onFinish: (() -> Void)?
    private var monitor: Any?
    private var loop: Task<Void, Never>?
    private var player: AVAudioPlayer?
    private var finished = false
    private var lastLight = -1

    func time(at date: Date) -> Double {
        max(0, min(Self.duration + 0.6, date.timeIntervalSince(startDate)))
    }

    func start(onFinish: @escaping () -> Void) {
        self.onFinish = onFinish
        finished = false
        // Go full screen first, under a dark screen, then start the show once the window has settled.
        let wentFull = IntroDriver.enterFullScreen()
        let lead = wentFull ? 0.75 : 0.15
        startDate = Date().addingTimeInterval(lead)

        if IntroPrefs.sound, let url = Bundle.main.url(forResource: "IntroSound", withExtension: "m4a"),
           let p = try? AVAudioPlayer(contentsOf: url) {
            p.volume = 0.9
            p.prepareToPlay()
            p.play(atTime: p.deviceCurrentTime + lead)
            player = p
        }

        monitor = NSEvent.addLocalMonitorForEvents(matching: [.keyDown, .leftMouseDown, .rightMouseDown]) { [weak self] e in
            guard let self, !self.finished else { return e }
            self.skip()
            return nil
        }

        loop = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                self.tick()
                try? await Task.sleep(nanoseconds: 33_000_000)
            }
        }
    }

    func skip() {
        let now = Date()
        let t = time(at: now)
        guard t < 4.6 else { return }
        startDate = now.addingTimeInterval(-4.6)      // jump to the hand-off; the fade still plays
        player?.setVolume(0, fadeDuration: 0.35)
    }

    func stop() {
        loop?.cancel(); loop = nil
        if let m = monitor { NSEvent.removeMonitor(m); monitor = nil }
        player?.stop(); player = nil
    }

    @MainActor private func tick() {
        let t = time(at: Date())
        if !finished, buttonPressed(), t > 0.2 { skip() }
        if IntroPrefs.lights { updateLights(t) }
        if !finished, t >= Self.duration + 0.05 {
            finished = true
            stop()
            onFinish?()
        }
    }

    private var buttonsHeld = true
    @MainActor private func buttonPressed() -> Bool {
        var any = false
        for c in GCController.controllers() {
            guard let g = c.extendedGamepad else { continue }
            if g.buttonA.isPressed || g.buttonB.isPressed || g.buttonX.isPressed || g.buttonY.isPressed || g.buttonMenu.isPressed { any = true }
        }
        // ignore a button that was already held when the app opened
        defer { if !any { buttonsHeld = false } }
        return any && !buttonsHeld
    }

    @MainActor private func updateLights(_ t: Double) {
        var rgb: (Double, Double, Double)
        if t < 1.3 { let k = 0.4 + 0.6 * eOut(prog(t, 0, 0.8)); rgb = (42 * k, 32 * k, 96 * k) }
        else if t < 2.1 { rgb = (124, 58, 237) }
        else if t < 3.0 { rgb = (103, 232, 249) }
        else if t < 3.5 { let k = prog(t, 3.0, 3.5); rgb = (lerp(255, 167, k), lerp(255, 139, k), lerp(255, 250, k)) }
        else if t < 4.25 { rgb = (167, 139, 250) }
        else { rgb = (34, 211, 238) }
        let pulse = (t >= 3 && t < 3.5) ? 1.0 : 0.75 + 0.25 * sin(t * 6)
        let key = Int(rgb.0 * pulse) << 16 | Int(rgb.1 * pulse) << 8 | Int(rgb.2 * pulse)
        guard key != lastLight else { return }
        lastLight = key
        ControllerLights.set(NSColor(deviceRed: rgb.0 * pulse / 255, green: rgb.1 * pulse / 255, blue: rgb.2 * pulse / 255, alpha: 1))
    }

    /// Puts the main window into full screen. Returns true if it started a transition.
    @discardableResult
    static func enterFullScreen() -> Bool {
        guard IntroPrefs.fullScreen else { return false }
        let w = NSApp.mainWindow ?? NSApp.keyWindow ?? NSApp.windows.first { $0.isVisible && $0.canBecomeMain }
        guard let w, !w.styleMask.contains(.fullScreen) else { return false }
        w.collectionBehavior.insert(.fullScreenPrimary)
        w.toggleFullScreen(nil)
        return true
    }
}

// MARK: the intro view

struct GizzyIntro: View {
    let onFinish: () -> Void
    @StateObject private var driver = IntroDriver()

    var body: some View {
        GeometryReader { geo in
            TimelineView(.animation) { tl in
                IntroFrame(t: driver.time(at: tl.date), size: geo.size)
            }
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onAppear {
            // the window may not be on screen during the very first layout pass
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.05) { driver.start(onFinish: onFinish) }
        }
        .onDisappear { driver.stop() }
    }
}

private struct IntroFrame: View {
    let t: Double
    let size: CGSize

    var body: some View {
        let L = IntroLayout(size: size)
        let slide = eIO(prog(t, 2.95, 3.65))
        let R = L.emblemRect(lerp(L.centerShift, 0, slide))
        let R0 = L.emblemRect(L.centerShift)
        let fly = eIn(prog(t, 4.25, 4.95))
        let fade = 1 - eOut(prog(t, 4.7, 5.0))
        let focus = CGPoint(x: R.minX + R.width * IntroLayout.face.x, y: R.minY + R.height * IntroLayout.face.y)

        ZStack {
            RadialGradient(colors: [Color(rgb: 0x151137), Color(rgb: 0x0A0920), Color(rgb: 0x05040F)],
                           center: UnitPoint(x: 0.5, y: 0.55), startRadius: 0,
                           endRadius: max(size.width, size.height) * 0.75)

            Canvas { ctx, sz in
                IntroPainter.sky(&ctx, t: t, size: sz, focus: focus)
                IntroPainter.dust(&ctx, t: t, size: sz, rect: R0)
            }

            halo(R: R, W: L.W, fly: fly)
            GlassEmblem(t: t, rect: R, fly: fly)
            letters(L: L)

            Canvas { ctx, _ in
                IntroPainter.comet(&ctx, t: t, rect: R)
                IntroPainter.burst(&ctx, t: t, rect: R)
            }
        }
        .frame(width: size.width, height: size.height)
        .opacity(fade)
        .allowsHitTesting(fade > 0.05)
    }

    @ViewBuilder private func halo(R: CGRect, W: Double, fly: Double) -> some View {
        let form = eOut(prog(t, 1.25, 1.8))
        let ignite = prog(t, 2.95, 3.6)
        Circle()
            .fill(RadialGradient(colors: [Color(rgb: 0x8B5CF6).opacity(0.30), Color(rgb: 0x67E8F9).opacity(0.08), .clear],
                                 center: .center, startRadius: 0, endRadius: W * 0.3))
            .frame(width: W * 0.6, height: W * 0.6)
            .blur(radius: 6)
            .opacity((form * 0.55 + sin(.pi * ignite) * 0.6) * (1 - fly))
            .position(x: R.midX, y: R.midY)
    }

    @ViewBuilder private func letters(L: IntroLayout) -> some View {
        if let img = IntroArt.letters {
            let lw = L.W * 0.6
            let lh = lw * IntroArt.lettersAspect
            let lt = prog(t, 3.15, 3.95)
            Image(nsImage: img)
                .resizable()
                .frame(width: lw, height: lh)
                .mask(alignment: .leading) { Rectangle().frame(width: max(0, lw * eOut(lt))) }
                .blur(radius: (1 - eOut(lt)) * 14)
                .shadow(color: Color(rgb: 0xA78BFA).opacity(0.55), radius: 18 * (1 - lt) + 6)
                .opacity(eOut(lt) * (1 - prog(t, 4.25, 4.55)))
                .position(x: L.left + L.W * 0.4 + lw / 2 - (1 - eBack(lt)) * 30, y: Double(size.height) / 2)
        }
    }
}

/// The emblem as translucent glass: stacked slices give it real thickness when it tilts.
private struct GlassEmblem: View {
    let t: Double
    let rect: CGRect
    let fly: Double

    var body: some View {
        if let img = IntroArt.emblem {
            let form = eOut(prog(t, 1.25, 1.8))
            let depth = eOut(prog(t, 1.3, 2.1))
            let tiltIn = prog(t, 1.55, 2.35), tiltOut = prog(t, 2.35, 3.25)
            let settle = 1 - eIO(tiltOut)
            let ry = -34 * sin(.pi * tiltIn * 0.5) * settle + sin(t * 1.3) * 2.5 * prog(t, 3, 3.6)
            let rx = 10 * sin(.pi * tiltIn * 0.5) * settle + cos(t * 1.1) * 1.5 * prog(t, 3, 3.6)
            let step = Double(rect.width) * 0.0055 * depth
            let sw1 = prog(t, 1.6, 2.3), sw2 = prog(t, 2.9, 3.5)
            let swp = sw2 > 0 ? sw2 : sw1
            let sweepOn = (sw1 > 0 && sw1 < 1) || (sw2 > 0 && sw2 < 1)
            let c = lerp(1.3, -0.3, swp)
            let zoom = 1 + fly * 22

            ZStack {
                ForEach(0..<IntroLayout.slices, id: \.self) { j in
                    let i = IntroLayout.slices - 1 - j            // back slices first
                    let k = Double(i) / Double(IntroLayout.slices - 1)
                    Image(nsImage: img)
                        .resizable()
                        .brightness(i == 0 ? 0 : -0.55 * k)
                        .saturation(i == 0 ? 1.15 : 1.3)
                        .hueRotation(.degrees(i == 0 ? 0 : -8 - 20 * k))
                        .opacity(i == 0 ? 0.86 * form : form * (0.22 - 0.12 * k))
                        .shadow(color: Color(rgb: 0x8B5CF6).opacity(i == 0 ? 0.55 * form : 0), radius: 18)
                        .offset(x: -Double(i) * step * sin(ry * .pi / 180) * 2.2,
                                y: Double(i) * step * sin(rx * .pi / 180) * 2.2)
                }
                Rectangle()
                    .fill(RadialGradient(colors: [.white.opacity(0.35), .clear],
                                         center: UnitPoint(x: 0.3, y: 0.25), startRadius: 0, endRadius: rect.width * 0.6))
                    .mask(Image(nsImage: img).resizable())
                    .blendMode(.screen)
                    .opacity(0.9 * form)
                Rectangle()
                    .fill(LinearGradient(stops: [
                        .init(color: .clear, location: 0.38),
                        .init(color: .white.opacity(0.85), location: 0.48),
                        .init(color: Color(rgb: 0xBEE6FF).opacity(0.55), location: 0.52),
                        .init(color: .clear, location: 0.62)],
                        startPoint: UnitPoint(x: c - 0.9, y: 0.2), endPoint: UnitPoint(x: c + 0.9, y: 0.8)))
                    .mask(Image(nsImage: img).resizable())
                    .blendMode(.screen)
                    .opacity(sweepOn ? sin(.pi * swp) * 0.9 : 0)
            }
            .frame(width: rect.width, height: rect.height)
            .rotation3DEffect(.degrees(ry), axis: (x: 0, y: 1, z: 0), perspective: 0.45)
            .rotation3DEffect(.degrees(rx), axis: (x: 1, y: 0, z: 0), perspective: 0.45)
            .scaleEffect(zoom, anchor: UnitPoint(x: IntroLayout.face.x, y: IntroLayout.face.y))
            .opacity(1 - prog(t, 4.6, 4.92))
            .position(x: rect.midX, y: rect.midY)
        }
    }
}

// MARK: canvas painting

private enum IntroPainter {
    static func sky(_ c: inout GraphicsContext, t: Double, size: CGSize, focus: CGPoint) {
        let warp = eIn(prog(t, 4.25, 4.95)) * (1 - eOut(prog(t, 4.95, 5.55)))
        let skyIn = eOut(prog(t, 0, 0.8))
        let w = Double(size.width), h = Double(size.height)
        for s in IntroArt.stars {
            let px = s.x * w, py = s.y * h
            let tw = 0.55 + 0.45 * sin(t * 1.7 + s.phase)
            let a = skyIn * tw * (0.35 + 0.65 * s.z)
            if warp > 0 {
                let dx = px - Double(focus.x), dy = py - Double(focus.y)
                let len = warp * (0.15 + s.z * 0.9) * 0.9
                var p = Path()
                p.move(to: CGPoint(x: px + dx * len * 0.2, y: py + dy * len * 0.2))
                p.addLine(to: CGPoint(x: px + dx * len * 1.2, y: py + dy * len * 1.2))
                c.stroke(p, with: .color(s.color.opacity(a)), lineWidth: s.size * (1 + warp * 1.5))
            } else {
                c.fill(Path(CGRect(x: px, y: py, width: s.size, height: s.size)), with: .color(s.color.opacity(a)))
            }
        }
    }

    static func dust(_ c: inout GraphicsContext, t: Double, size: CGSize, rect R0: CGRect) {
        let fade = 1 - prog(t, 1.3, 1.85)
        guard fade > 0, t > 0 else { return }
        c.blendMode = .plusLighter
        let ecx = Double(R0.midX), ecy = Double(R0.midY)
        let span = Double(max(size.width, size.height)) * 0.62
        for p in IntroArt.dust {
            let u = eOut(prog(t, 0.05 + p.delay, 0.95 + p.delay * 0.75))
            let ang = p.angle + p.spin * (1 - u) * 2.4
            let sx = ecx + cos(ang) * p.radius * span, sy = ecy + sin(ang) * p.radius * span * 0.62
            let tx = Double(R0.minX) + p.fx * Double(R0.width), ty = Double(R0.minY) + p.fy * Double(R0.height)
            let x = lerp(sx, tx, u), y = lerp(sy, ty, u)
            let a = (0.25 + 0.75 * u) * fade * min(1, t * 3)
            let sz = p.size * (1 + (1 - u) * 1.2)
            c.fill(Path(CGRect(x: x - sz / 2, y: y - sz / 2, width: sz, height: sz)), with: .color(p.color.opacity(a)))
        }
        c.blendMode = .normal
    }

    /// A tilted ellipse around the emblem that ends exactly on the star tip.
    static func cometAt(_ u: Double, _ R: CGRect) -> (point: CGPoint, front: Bool) {
        let cx = Double(R.midX), cy = Double(R.minY) + Double(R.height) * 0.52
        let rx = Double(R.width) * 0.78, ry = Double(R.height) * 0.26, tilt = -0.42
        let sx = Double(R.minX) + Double(R.width) * IntroLayout.star.x
        let sy = Double(R.minY) + Double(R.height) * IntroLayout.star.y
        let a = lerp(.pi * 0.85, .pi * 0.85 + .pi * 2.15, eIO(u))
        var x = cx + cos(a) * rx * cos(tilt) - sin(a) * ry * sin(tilt)
        var y = cy + cos(a) * rx * sin(tilt) + sin(a) * ry * cos(tilt)
        let b = eIO(prog(u, 0.72, 1))
        x = lerp(x, sx, b); y = lerp(y, sy, b)
        return (CGPoint(x: x, y: y), sin(a) > 0)
    }

    static func comet(_ c: inout GraphicsContext, t: Double, rect R: CGRect) {
        let cu = prog(t, 2.1, 3.0)
        guard cu > 0, t < 3.9 else { return }
        c.blendMode = .plusLighter
        // the ring it draws, which lingers
        let ringA = sin(.pi * prog(t, 2.2, 3.7)) * 0.35
        var ring = Path()
        var s = 0.0
        ring.move(to: cometAt(0, R).point)
        while s <= cu { ring.addLine(to: cometAt(s, R).point); s += 0.01 }
        c.stroke(ring, with: .color(Color(rgb: 0xA78BFA).opacity(ringA)), lineWidth: 1.2)
        // trail
        let fade = 1 - prog(t, 3.05, 3.5)
        let trail = 26
        for i in stride(from: trail, through: 0, by: -1) {
            let uu = clamp01(cu - Double(i) * 0.012)
            guard uu > 0 else { continue }
            let p = cometAt(uu, R)
            let k = 1 - Double(i) / Double(trail)
            let r = (1.2 + k * 3.6) * (p.front ? 1.15 : 0.75)
            let col: Color = i < 3 ? .white : Color(rgb: 0xA78BFA)
            c.fill(Path(ellipseIn: CGRect(x: p.point.x - r, y: p.point.y - r, width: r * 2, height: r * 2)),
                   with: .color(col.opacity(k * 0.8 * fade)))
        }
        // head glow
        let head = cometAt(cu, R).point
        let hf = 1 - prog(t, 3.0, 3.35)
        c.fill(Path(ellipseIn: CGRect(x: head.x - 26, y: head.y - 26, width: 52, height: 52)),
               with: .radialGradient(Gradient(stops: [
                    .init(color: .white.opacity(0.95 * hf), location: 0),
                    .init(color: Color(rgb: 0x67E8F9).opacity(0.45 * hf), location: 0.3),
                    .init(color: .clear, location: 1)]),
                    center: head, startRadius: 0, endRadius: 26))
        c.blendMode = .normal
    }

    static func burst(_ c: inout GraphicsContext, t: Double, rect R: CGRect) {
        let b = prog(t, 2.98, 3.6)
        guard b > 0, b < 1 else { return }
        let sx = Double(R.minX) + Double(R.width) * IntroLayout.star.x
        let sy = Double(R.minY) + Double(R.height) * IntroLayout.star.y
        let rr = 12 + b * Double(R.width) * 0.55, a = 1 - b
        let center = CGPoint(x: sx, y: sy)
        c.blendMode = .plusLighter
        c.fill(Path(ellipseIn: CGRect(x: sx - rr, y: sy - rr, width: rr * 2, height: rr * 2)),
               with: .radialGradient(Gradient(stops: [
                    .init(color: .white.opacity(a), location: 0),
                    .init(color: Color(rgb: 0xC4B5FD).opacity(a * 0.6), location: 0.15),
                    .init(color: .clear, location: 1)]),
                    center: center, startRadius: 0, endRadius: rr))
        let l = Double(R.width) * 0.5 * eOut(b)
        var flare = Path()
        flare.move(to: CGPoint(x: sx - l, y: sy)); flare.addLine(to: CGPoint(x: sx + l, y: sy))
        flare.move(to: CGPoint(x: sx, y: sy - l * 0.6)); flare.addLine(to: CGPoint(x: sx, y: sy + l * 0.6))
        c.stroke(flare, with: .color(.white.opacity(a * 0.8)), lineWidth: 1.5)
        c.blendMode = .normal
    }
}
