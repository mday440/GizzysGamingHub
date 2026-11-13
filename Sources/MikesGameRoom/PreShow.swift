import SwiftUI
import AppKit
import GameController

// Gizzy's Starlight Cinema: the pre-show that covers a game while it loads (its pipeline archive, then "Preparing
// pipelines"), from the moment Launch is pressed until the game is ready. A full-screen panel above every space,
// including the game's own full-screen one. It follows the game's log (~/Library/Logs/nxnative/<title ID>.log):
//   "pipeline archive … opened in"                    the archive is open
//   "pipeline prebuild: X of Y pairs" / "… done"      the device's own prebuild (first launches, Metroid)
//   "pipeline prepare: X of Y lines" / "… before the first frame"   the prepare step; the game draws right after
// then counts down 3-2-1 and fades into the game. A click skips it; Settings › Launch intro turns it off.

@MainActor
final class PreShow: ObservableObject {
    static let shared = PreShow()
    static var enabled: Bool { UserDefaults.standard.object(forKey: "preshow.enabled") as? Bool ?? true }

    enum Phase: Equatable { case loading, countdown(Int), fading }

    @Published private(set) var phase: Phase = .loading
    @Published private(set) var progress = 0.0
    @Published private(set) var what = "Opening the film canister…"
    @Published private(set) var count = "LOADING PIPELINE ARCHIVE"
    @Published private(set) var etaSeconds: Int?
    @Published private(set) var title = ""
    @Published private(set) var cover: NSImage?
    @Published private(set) var seat = "PLAYER 1"

    private var panel: NSPanel?
    private var task: Task<Void, Never>?

    func begin(_ app: NativeApp) {
        guard Self.enabled else { return }
        close()
        let entry = LibraryStore.shared?.entries.first { $0.info.titleIDString.uppercased() == app.titleID.uppercased() }
        title = entry?.info.name ?? app.url.deletingPathExtension().lastPathComponent
        cover = entry.flatMap { entryImage($0) }
        let pad = GCController.controllers().first { $0.extendedGamepad != nil }
        seat = "PLAYER 1 · " + (pad?.vendorName ?? "KEYBOARD").uppercased()
        phase = .loading; progress = 0; etaSeconds = nil
        what = "Opening the film canister…"; count = "LOADING THE GAME"
        NSCursor.setHiddenUntilMouseMoves(true)   // the pointer stays out of the picture; moving the mouse brings it back

        let screen = NSApp.windows.first(where: { $0.isVisible && $0.canBecomeMain })?.screen ?? NSScreen.main
        guard let screen else { return }
        let p = NSPanel(contentRect: screen.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.level = .screenSaver
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        p.isOpaque = true
        p.backgroundColor = .black
        p.hasShadow = false
        p.hidesOnDeactivate = false
        p.isReleasedWhenClosed = false
        p.contentView = NSHostingView(rootView: PreShowView())
        p.setFrame(screen.frame, display: true)
        p.orderFrontRegardless()
        panel = p

        let log = app.logFile, tid = app.titleID
        let before = try? FileManager.default.attributesOfItem(atPath: log.path)
        let oldInode = (before?[.systemFileNumber] as? NSNumber)?.intValue
        let oldSize = (before?[.size] as? NSNumber)?.intValue ?? 0
        task = Task { @MainActor [weak self] in
            await self?.watch(log: log, tid: tid, oldInode: oldInode, oldSize: oldSize)
        }
    }

    /// A click on the pre-show: straight into the game.
    func skip() {
        guard phase != .fading else { return }
        fadeOut()
    }

    func close() {
        task?.cancel(); task = nil
        panel?.orderOut(nil); panel?.close(); panel = nil
    }

    private func fadeOut() {
        task?.cancel(); task = nil
        withAnimation(.easeIn(duration: 0.6)) { phase = .fading }
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 650_000_000)
            self?.close()
        }
    }

    // MARK: following the log

    private func watch(log: URL, tid: String, oldInode: Int?, oldSize: Int) async {
        let start = Date()
        var fresh = oldInode == nil
        var archiveAt: Date?, prepareSeen = false, prepareDone = false, prebuildSeen = false, prebuildDone = false
        var prebuildDoneAt: Date?
        var phaseStart: (key: String, at: Date, n: Int)?
        while !Task.isCancelled {
            try? await Task.sleep(nanoseconds: 300_000_000)
            if Task.isCancelled { return }
            if NativeSessions.shared.running[tid] == nil { fadeOut(); return }   // the game ended (or never started)
            let attrs = try? FileManager.default.attributesOfItem(atPath: log.path)
            if !fresh {
                // the launcher starts a new log for this run; until then the file is the last run's
                let inode = (attrs?[.systemFileNumber] as? NSNumber)?.intValue
                let size = (attrs?[.size] as? NSNumber)?.intValue ?? 0
                if inode != oldInode || size < oldSize { fresh = true } else { continue }
            }
            let lines = Self.tail(log)
            for l in lines {
                if l.contains("pipeline archive") && l.contains("opened in") && archiveAt == nil { archiveAt = Date() }
                if l.contains("pipeline prebuild: ") { prebuildSeen = true }
                if l.contains("pipeline prebuild done") { prebuildDone = true; if prebuildDoneAt == nil { prebuildDoneAt = Date() } }
                if l.contains("pipeline prepare: ") { prepareSeen = true }
                if l.contains("pipeline prepare: ") && l.contains("before the first frame") { prepareDone = true }
                if l.contains("pipeline prewarm done") { prepareDone = true }
            }
            // progress: the busier of the two steps still running
            var shown = false
            if prebuildSeen && !prebuildDone, let (x, y) = Self.lastCount(lines, key: "pipeline prebuild: ", unit: "pairs") {
                show(x, y, key: "prebuild", phaseStart: &phaseStart); shown = true
            }
            if !shown, prepareSeen && !prepareDone, let (x, y) = Self.lastCount(lines, key: "pipeline prepare: ", unit: "lines") {
                show(x, y, key: "prepare", phaseStart: &phaseStart); shown = true
            }
            if !shown {
                // opening the archive: no count, so creep towards 15%
                let t = Date().timeIntervalSince(start)
                progress = max(progress, 0.15 * (1 - exp(-t / 12)))
                if archiveAt != nil { what = "Threading the projector…"; count = "PIPELINE ARCHIVE OPEN" }
                else { what = "Opening the film canister…"; count = "LOADING THE GAME" }
            }
            // ready?
            let now = Date()
            let ready = (prepareDone && (!prebuildSeen || prebuildDone))
                || (prebuildDone && !prepareSeen && now.timeIntervalSince(prebuildDoneAt ?? now) > 5)
                || (archiveAt != nil && !prepareSeen && !prebuildSeen && now.timeIntervalSince(archiveAt!) > 8)
                || (archiveAt == nil && !prepareSeen && !prebuildSeen && now.timeIntervalSince(start) > 150)
            if ready { await countdown(); return }
        }
    }

    private func show(_ x: Int, _ y: Int, key: String, phaseStart: inout (key: String, at: Date, n: Int)?) {
        guard y > 0 else { return }
        if phaseStart?.key != key { phaseStart = (key, Date(), x) }
        progress = max(progress, 0.15 + 0.85 * Double(x) / Double(y))
        what = key == "prebuild" ? "Building the pipelines for your Mac…" : "Threading the projector…"
        count = "\(x.formatted()) OF \(y.formatted()) PIPELINES READY"
        if let s = phaseStart {
            let t = Date().timeIntervalSince(s.at), done = x - s.n
            if t > 2, done > 0 { etaSeconds = Int((Double(y - x) / (Double(done) / t)).rounded()) }
        }
    }

    private func countdown() async {
        progress = 1
        what = "Lights down. Enjoy the show."
        count = "ALL PIPELINES READY"
        etaSeconds = 0
        for n in [3, 2, 1] {
            withAnimation(.easeOut(duration: 0.3)) { phase = .countdown(n) }
            try? await Task.sleep(nanoseconds: 1_000_000_000)
            if Task.isCancelled { return }
        }
        fadeOut()
    }

    // "… 12,000 of 97,598 pairs …" → (12000, 97598), from the last line with the key
    private static func lastCount(_ lines: [String], key: String, unit: String) -> (Int, Int)? {
        for l in lines.reversed() where l.contains(key) {
            guard let r = l.range(of: #"(\d[\d,]*) of (\d[\d,]*) "# + unit, options: .regularExpression) else { continue }
            let parts = l[r].components(separatedBy: " of ")
            let a = Int(parts[0].replacingOccurrences(of: ",", with: "")) ?? 0
            let b = Int(parts[1].components(separatedBy: " ")[0].replacingOccurrences(of: ",", with: "")) ?? 0
            return (a, b)
        }
        return nil
    }

    private static func tail(_ url: URL, bytes: UInt64 = 131_072) -> [String] {
        guard let h = try? FileHandle(forReadingFrom: url) else { return [] }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > bytes ? size - bytes : 0)
        let data = (try? h.readToEnd()) ?? Data()
        return String(decoding: data, as: UTF8.self).split(whereSeparator: { $0 == "\n" || $0 == "\r" }).map(String.init)
    }
}

// MARK: the pre-show

struct PreShowView: View {
    @ObservedObject private var show = PreShow.shared

    var body: some View {
        GeometryReader { geo in
            let vh = geo.size.height / 100
            let width = min(geo.size.height * 1.18, geo.size.width * 0.86)
            ZStack {
                RadialGradient(colors: [Color(rgb: 0x17184A), Color(rgb: 0x0C0D2A), Color(rgb: 0x06061A)],
                               center: UnitPoint(x: 0.5, y: 0.4), startRadius: 0, endRadius: max(geo.size.width, geo.size.height) * 0.7)
                PreShowStars()
                beam(vh: vh)
                VStack(spacing: 0) {
                    marquee(vh: vh).padding(.top, 3.2 * vh)
                    Spacer(minLength: 2 * vh)
                    screen(vh: vh).frame(width: width, height: 56 * vh)
                    Spacer(minLength: 2 * vh)
                    ticket(vh: vh).frame(width: width, height: 12 * vh).padding(.bottom, 4.5 * vh)
                }
                if show.phase == .fading { Color.black.transition(.opacity) }
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .ignoresSafeArea()
        .contentShape(Rectangle())
        .onTapGesture { show.skip() }
        .foregroundStyle(Orbit.text)
    }

    private func beam(vh: CGFloat) -> some View {
        TimelineView(.animation(minimumInterval: 1 / 20)) { tl in
            let t = tl.date.timeIntervalSinceReferenceDate
            let flicker = 0.85 + 0.15 * abs(sin(t * 1.9) * sin(t * 0.7))
            AngularGradient(stops: [
                .init(color: .clear, location: 0.0), .init(color: .clear, location: 0.44),
                .init(color: Color(rgb: 0xB9A6FF).opacity(0.07), location: 0.478),
                .init(color: .white.opacity(0.10), location: 0.5),
                .init(color: Color(rgb: 0xB9A6FF).opacity(0.07), location: 0.522),
                .init(color: .clear, location: 0.56), .init(color: .clear, location: 1)],
                center: .top, startAngle: .degrees(0), endAngle: .degrees(360))
                .rotationEffect(.degrees(180))
                .frame(width: 120 * vh, height: 70 * vh)
                .blur(radius: 6)
                .opacity(flicker)
                .frame(maxHeight: .infinity, alignment: .top)
                .offset(y: -6 * vh)
                .allowsHitTesting(false)
        }
    }

    private func marquee(vh: CGFloat) -> some View {
        VStack(spacing: 0.4 * vh) {
            Text("GIZZY'S STARLIGHT CINEMA").font(Orbit.display(1.05 * vh, 600)).tracking(0.45 * vh).foregroundStyle(Orbit.mint)
            Text("NOW SHOWING · SCREEN P1").font(Orbit.display(2.6 * vh, 800)).tracking(0.15 * vh)
                .foregroundStyle(LinearGradient(colors: [Color(rgb: 0xC4B5FD), .white, Color(rgb: 0xA5F3FC)], startPoint: .leading, endPoint: .trailing))
        }
        .padding(.vertical, 1.4 * vh).padding(.horizontal, 3 * vh)
        .background(RoundedRectangle(cornerRadius: 16).fill(LinearGradient(colors: [Orbit.panel.opacity(0.9), Orbit.card.opacity(0.9)], startPoint: .top, endPoint: .bottom)))
        .overlay(RoundedRectangle(cornerRadius: 16).strokeBorder(Orbit.lavender.opacity(0.35), lineWidth: 1))
        .overlay(alignment: .top) { Bulbs().offset(y: -3) }
        .overlay(alignment: .bottom) { Bulbs().offset(y: 3) }
        .shadow(color: Color(rgb: 0x7C3AED).opacity(0.25), radius: 30)
    }

    private func screen(vh: CGFloat) -> some View {
        ZStack {
            RoundedRectangle(cornerRadius: 26)
                .fill(LinearGradient(colors: [Orbit.card.opacity(0.85), Color(rgb: 0x0C0D2A).opacity(0.92)], startPoint: .top, endPoint: .bottom))
            if case .countdown(let n) = show.phase {
                Leader(n: n, vh: vh)
            } else {
                VStack(spacing: 0) {
                    if let img = show.cover {
                        Image(nsImage: img).resizable().aspectRatio(contentMode: .fit)
                            .frame(maxHeight: 24 * vh)
                            .clipShape(RoundedRectangle(cornerRadius: 18))
                            .shadow(color: .black.opacity(0.6), radius: 30, y: 16)
                            .padding(.bottom, 3.4 * vh)
                    } else {
                        Image(systemName: "film.stack").font(.system(size: 9 * vh, weight: .light)).foregroundStyle(Orbit.lavender)
                            .padding(.bottom, 3 * vh)
                    }
                    Text("TONIGHT'S FEATURE").font(Orbit.display(1.2 * vh, 600)).tracking(0.4 * vh).foregroundStyle(Orbit.mint)
                    Text(show.title).font(Orbit.display(3.8 * vh, 800)).multilineTextAlignment(.center).lineLimit(2)
                        .minimumScaleFactor(0.6).padding(.top, 1.2 * vh).padding(.horizontal, 6 * vh)
                    Text("Grab the popcorn. We're polishing every pixel for your premiere.")
                        .font(Orbit.display(2.2 * vh, 600)).multilineTextAlignment(.center)
                        .foregroundStyle(LinearGradient(colors: [Color(rgb: 0xC4B5FD), .white, Color(rgb: 0xA5F3FC)], startPoint: .leading, endPoint: .trailing))
                        .padding(.top, 1.8 * vh).padding(.horizontal, 6 * vh)
                    Text(snackLine).font(Orbit.body(1.8 * vh, 500)).foregroundStyle(Orbit.dim).padding(.top, 1.2 * vh)
                }
                .padding(4 * vh)
            }
        }
        .clipShape(RoundedRectangle(cornerRadius: 26))
        .overlay(RoundedRectangle(cornerRadius: 26).strokeBorder(Orbit.line, lineWidth: 1))
        .shadow(color: .black.opacity(0.55), radius: 45, y: 30)
        .shadow(color: Color(rgb: 0x7C3AED).opacity(0.18), radius: 60)
    }

    private var snackLine: String {
        guard let s = show.etaSeconds, s > 0 else { return "🍿 Snack run? Perfect timing." }
        return "🍿 Snack run? You have about \(Self.clock(s))."
    }

    private func ticket(vh: CGFloat) -> some View {
        let torn: Bool = { if case .countdown = show.phase { return true }; return show.phase == .fading }()
        return HStack(spacing: 0) {
            VStack(alignment: .leading, spacing: 0.6 * vh) {
                Text("ADMIT ONE").font(Orbit.display(2 * vh, 800)).tracking(0.15 * vh).foregroundStyle(Color(rgb: 0xFFD36E))
                Text("SEAT P1 · ROW ORBIT").font(Orbit.display(1 * vh, 600)).tracking(0.25 * vh).foregroundStyle(Orbit.mute)
            }
            .padding(.horizontal, 2 * vh)
            .frame(width: 19 * vh, alignment: .leading)
            .frame(maxHeight: .infinity)
            .background(SideRounded(left: true).fill(ticketFill))
            .overlay(alignment: .trailing) {
                Rectangle().stroke(Orbit.lavender.opacity(0.35), style: StrokeStyle(lineWidth: 2, dash: [5, 4])).frame(width: 1)
            }
            .rotationEffect(.degrees(torn ? -9 : 0), anchor: .bottomTrailing)
            .offset(x: torn ? -2 * vh : 0, y: torn ? 1.5 * vh : 0)
            .opacity(torn ? 0.85 : 1)
            .animation(.spring(response: 0.7, dampingFraction: 0.55), value: torn)

            VStack(spacing: 1.1 * vh) {
                HStack {
                    Text(show.what).font(Orbit.body(1.8 * vh, 700))
                    Spacer()
                    Text(etaText).font(Orbit.body(1.6 * vh, 700)).foregroundStyle(Orbit.mint)
                }
                FilmStrip(progress: show.progress).frame(height: 3.2 * vh)
                HStack {
                    Text(show.count).font(Orbit.display(1.15 * vh, 600)).tracking(0.2 * vh).foregroundStyle(Orbit.mute)
                    Spacer()
                    Text(show.seat).font(Orbit.display(1.15 * vh, 600)).tracking(0.2 * vh).foregroundStyle(Orbit.lavender)
                }
            }
            .padding(.horizontal, 2.6 * vh)
            .frame(maxHeight: .infinity)
            .background(SideRounded(left: false).fill(ticketFill))
        }
        .shadow(color: .black.opacity(0.5), radius: 30, y: 18)
    }

    private var ticketFill: LinearGradient {
        LinearGradient(colors: [Color(rgb: 0x24265A), Orbit.panel], startPoint: .top, endPoint: .bottom)
    }

    private var etaText: String {
        switch show.phase {
        case .countdown, .fading: return "Now playing"
        case .loading:
            guard let s = show.etaSeconds else { return "Feature starts soon" }
            return "Feature in ~\(Self.clock(s))"
        }
    }

    static func clock(_ s: Int) -> String {
        s >= 60 ? "\(s / 60):" + String(format: "%02d", s % 60) : "\(s) s"
    }
}

/// The film leader's countdown ring.
private struct Leader: View {
    let n: Int
    let vh: CGFloat
    @State private var sweep: CGFloat = 0

    var body: some View {
        ZStack {
            RadialGradient(colors: [Color(rgb: 0x1D1F52), Color(rgb: 0x0B0C26)], center: .center, startRadius: 0, endRadius: 60 * vh)
            Rectangle().fill(Orbit.lavender.opacity(0.25)).frame(width: 1)
            Rectangle().fill(Orbit.lavender.opacity(0.25)).frame(height: 1)
            ZStack {
                Circle().stroke(Orbit.line, lineWidth: 0.9 * vh)
                Circle().trim(from: 0, to: sweep).stroke(Orbit.lavender, style: StrokeStyle(lineWidth: 0.9 * vh, lineCap: .round))
                    .rotationEffect(.degrees(-90))
                Text("\(n)").font(Orbit.display(16 * vh, 800)).shadow(color: Orbit.lavender.opacity(0.6), radius: 30)
            }
            .frame(width: 34 * vh, height: 34 * vh)
            Text("ENJOY THE SHOW").font(Orbit.display(1.4 * vh, 600)).tracking(0.7 * vh).foregroundStyle(Orbit.mint)
                .frame(maxHeight: .infinity, alignment: .bottom).padding(.bottom, 5 * vh)
        }
        .onAppear { run() }
        .onChange(of: n) { _ in run() }
    }

    private func run() {
        sweep = 0
        withAnimation(.linear(duration: 0.95)) { sweep = 1 }
    }
}

/// The progress bar as a strip of film rolling through.
private struct FilmStrip: View {
    let progress: Double

    var body: some View {
        GeometryReader { g in
            ZStack(alignment: .leading) {
                RoundedRectangle(cornerRadius: 8).fill(Color(rgb: 0x0B0C26))
                Rectangle()
                    .fill(LinearGradient(colors: [Color(rgb: 0x7C3AED), Orbit.lavender, Color(rgb: 0xA5F3FC)], startPoint: .leading, endPoint: .trailing))
                    .frame(width: g.size.width * min(1, max(0, progress)))
                    .animation(.easeOut(duration: 0.4), value: progress)
                TimelineView(.animation(minimumInterval: 1 / 30)) { tl in
                    Canvas { ctx, size in
                        let shift = CGFloat(tl.date.timeIntervalSinceReferenceDate.truncatingRemainder(dividingBy: 1)) * 14
                        var x = -14 + shift
                        while x < size.width + 14 {
                            for y in [size.height * 0.2, size.height * 0.8] {
                                ctx.fill(Path(ellipseIn: CGRect(x: x + 4.5, y: y - 2.5, width: 5, height: 5)), with: .color(Color(rgb: 0x0B0C26)))
                            }
                            x += 14
                        }
                    }
                }
            }
            .clipShape(RoundedRectangle(cornerRadius: 8))
            .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Orbit.line, lineWidth: 1))
        }
    }
}

/// The marquee's chasing light bulbs.
private struct Bulbs: View {
    var body: some View {
        TimelineView(.periodic(from: .now, by: 0.22)) { tl in
            let step = Int(tl.date.timeIntervalSinceReferenceDate / 0.22)
            HStack {
                ForEach(0..<26, id: \.self) { i in
                    let on = (i + step) % 3 == 0
                    Circle().fill(on ? Color(rgb: 0xFFD36E) : Color(rgb: 0x5B4FA8))
                        .frame(width: 6, height: 6)
                        .shadow(color: on ? Color(rgb: 0xFFD36E) : .clear, radius: 4)
                    if i < 25 { Spacer(minLength: 0) }
                }
            }
            .padding(.horizontal, 10)
        }
    }
}

/// Drifting, twinkling stars (the hub's night sky).
private struct PreShowStars: View {
    private struct Star { let x, y, r, phase, speed: Double }
    private let stars: [Star] = {
        var s: UInt64 = 0x51A7C1E
        func rnd() -> Double { s = s &* 6364136223846793005 &+ 1442695040888963407; return Double(s >> 11) / Double(UInt64(1) << 53) }
        return (0..<260).map { _ in Star(x: rnd(), y: rnd(), r: rnd() * 1.4 + 0.4, phase: rnd() * 6.28, speed: 0.2 + rnd() * 0.6) }
    }()

    var body: some View {
        TimelineView(.animation(minimumInterval: 1 / 30)) { tl in
            let t = tl.date.timeIntervalSinceReferenceDate
            Canvas { ctx, size in
                for st in stars {
                    var x = (st.x * size.width - t * st.speed * 6).truncatingRemainder(dividingBy: size.width)
                    if x < 0 { x += size.width }
                    let a = 0.35 + 0.45 * pow(sin(t * st.speed * 1.1 + st.phase), 2)
                    ctx.fill(Path(CGRect(x: x, y: st.y * size.height, width: st.r, height: st.r)), with: .color(.white.opacity(a)))
                }
            }
        }
        .allowsHitTesting(false)
    }
}

/// Settings row (in the Launch intro card).
struct PreShowSetting: View {
    @AppStorage("preshow.enabled") private var on = true
    var body: some View {
        Toggle("Starlight Cinema pre-show while games load", isOn: $on)
    }
}

/// A rectangle rounded on one side only (the ticket's stub and body; macOS 13 has no UnevenRoundedRectangle).
private struct SideRounded: Shape {
    let left: Bool
    var radius: CGFloat = 16
    func path(in r: CGRect) -> Path {
        var p = Path()
        let rr = min(radius, r.height / 2)
        if left {
            p.move(to: CGPoint(x: r.maxX, y: r.minY))
            p.addLine(to: CGPoint(x: r.minX + rr, y: r.minY))
            p.addArc(center: CGPoint(x: r.minX + rr, y: r.minY + rr), radius: rr, startAngle: .degrees(-90), endAngle: .degrees(180), clockwise: true)
            p.addLine(to: CGPoint(x: r.minX, y: r.maxY - rr))
            p.addArc(center: CGPoint(x: r.minX + rr, y: r.maxY - rr), radius: rr, startAngle: .degrees(180), endAngle: .degrees(90), clockwise: true)
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY))
        } else {
            p.move(to: CGPoint(x: r.minX, y: r.minY))
            p.addLine(to: CGPoint(x: r.maxX - rr, y: r.minY))
            p.addArc(center: CGPoint(x: r.maxX - rr, y: r.minY + rr), radius: rr, startAngle: .degrees(-90), endAngle: .degrees(0), clockwise: false)
            p.addLine(to: CGPoint(x: r.maxX, y: r.maxY - rr))
            p.addArc(center: CGPoint(x: r.maxX - rr, y: r.maxY - rr), radius: rr, startAngle: .degrees(0), endAngle: .degrees(90), clockwise: false)
            p.addLine(to: CGPoint(x: r.minX, y: r.maxY))
        }
        p.closeSubpath()
        return p
    }
}
