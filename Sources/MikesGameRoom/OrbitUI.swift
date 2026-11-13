import SwiftUI
import AppKit
import CoreText
import GameController

// Gizzy's Gaming Hub: the "Night Orbit" look. A midnight sky; the selected game is the planet in the middle
// and the rest of the library orbits it, slowly. The game's details and the Launch button sit beside it, and a
// flight log (the activity log) runs along the bottom. Type: Unbounded and Manrope (SIL Open Font License),
// bundled in Resources/Fonts.

enum Orbit {
    static let bg = Color(rgb: 0x0A0C22)
    static let panel = Color(rgb: 0x1B1D45)
    static let card = Color(rgb: 0x12143A)
    static let line = Color(rgb: 0x2E3066)
    static let ring = Color(rgb: 0x262860)
    static let text = Color(rgb: 0xECEAFB)
    static let dim = Color(rgb: 0xB7B4DA)
    static let mute = Color(rgb: 0x8C88B8)
    static let lavender = Color(rgb: 0xB9A6FF)
    static let mint = Color(rgb: 0x7CF0C5)
    static let peach = Color(rgb: 0xFF9E7A)
    static let ink = Color(rgb: 0x12103A)
    static let planetColors: [UInt32] = [0x7CF0C5, 0xFF9E7A, 0x6F8BFF, 0x9BD36A, 0xE58BD8, 0xFFD36E, 0x6FD6FF]

    /// Unbounded at 400, 600 or 800.
    static func display(_ size: CGFloat, _ weight: Int = 600) -> Font { .custom("Unbounded-W\(weight)", size: size) }
    /// Manrope at 400, 500 or 700.
    static func body(_ size: CGFloat, _ weight: Int = 500) -> Font { .custom("Manrope-W\(weight)", size: size) }

    /// Makes the bundled fonts available to the app (Contents/Resources/Fonts).
    static func registerFonts() {
        guard let dir = Bundle.main.resourceURL?.appendingPathComponent("Fonts"),
              let files = try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) else { return }
        for f in files where f.pathExtension.lowercased() == "ttf" {
            CTFontManagerRegisterFontsForURL(f as CFURL, .process, nil)
        }
    }

    static func planetColor(_ name: String) -> Color {
        let h = name.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0x7FFFFFFF }
        return Color(rgb: planetColors[h % planetColors.count])
    }
}

/// Small uppercase label with wide tracking.
struct OrbitLabel: View {
    let text: String
    var color: Color = Orbit.mute
    init(_ text: String, color: Color = Orbit.mute) { self.text = text; self.color = color }
    var body: some View {
        Text(text.uppercased()).font(Orbit.body(12, 700)).tracking(2.4).foregroundStyle(color)
    }
}

/// Lavender pill (primary) or outlined pill (secondary).
struct OrbitButtonStyle: ButtonStyle {
    var primary = true
    var height: CGFloat = 52
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(primary ? Orbit.display(18, 800) : Orbit.body(15, 700))
            .tracking(primary ? 1.2 : 0)
            .foregroundStyle(primary ? Orbit.ink : Orbit.text)
            .frame(maxWidth: .infinity, minHeight: height)
            .background(Capsule().fill(primary ? Orbit.lavender : Color.clear))
            .overlay(Capsule().strokeBorder(primary ? Color.clear : Orbit.line, lineWidth: 1))
            .shadow(color: primary ? Orbit.lavender.opacity(configuration.isPressed ? 0.2 : 0.45) : .clear, radius: 20)
            .scaleEffect(configuration.isPressed ? 0.98 : 1)
            .contentShape(Capsule())
    }
}

enum OrbitScreen: Equatable { case orbit, controllers, settings }

@MainActor
final class OrbitState: ObservableObject {
    @Published var screen: OrbitScreen = .orbit
    @Published var node: ShelfNode = .all
    @Published var selectedID: String?
    @Published var optionsFor: LibraryEntry?
    @Published var showImporter = false
    @Published var dropTargeted = false
    @Published var memoryCheckFor: LibraryEntry?
    @Published var doctorFor: LibraryEntry?
    @Published var savesFor: LibraryEntry?
}

// MARK: the window

struct OrbitRootView: View {
    @EnvironmentObject var library: LibraryStore
    @StateObject private var state = OrbitState()
    @ObservedObject private var sessions = NativeSessions.shared
    @State private var showIntro = IntroPrefs.enabled
    @State private var showSetup = false
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        ZStack {
            Orbit.bg
            Starfield()
            SkyTint()
            HStack(spacing: 0) {
                OrbitNav().frame(width: 252)
                VStack(spacing: 0) {
                    Group {
                        switch state.screen {
                        case .orbit: OrbitMain(play: play)
                        case .controllers: OrbitControllers()
                        case .settings: OrbitSettings()
                        }
                    }
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
                    FlightLogBar()
                }
            }
            .overlay(alignment: .bottomLeading) {
                if state.screen == .orbit { MiniTV()
                    .shadow(color: .black.opacity(0.5), radius: 24, y: 8)
                    .padding(.leading, 22)
                    .padding(.bottom, 58 + 16) }
            }
            if showIntro {
                GizzyIntro { showIntro = false; selectionChanged() }
                    .transition(.opacity)
                    .zIndex(10)
            }
            if state.screen == .orbit {
                VStack { PadHints().padding(.top, 16); Spacer() }
                    .allowsHitTesting(false)
            }
            // full screen hides the window's buttons: this takes you back to the desktop (click the Dock icon to return)
            VStack {
                HStack {
                    Spacer()
                    Button { NSApp.hide(nil) } label: {
                        Image(systemName: "minus").font(.system(size: 13, weight: .bold))
                            .frame(width: 30, height: 30)
                            .background(Circle().fill(Orbit.panel.opacity(0.85)))
                            .overlay(Circle().strokeBorder(Orbit.line, lineWidth: 1))
                    }
                    .buttonStyle(.plain)
                    .foregroundStyle(Orbit.dim)
                    .help("Back to the desktop")
                    .accessibilityLabel("Minimize to the desktop")
                    .padding(.top, 14).padding(.trailing, 16)
                }
                Spacer()
            }
            .zIndex(5)
            if state.dropTargeted {
                RoundedRectangle(cornerRadius: 28).strokeBorder(Orbit.lavender, style: StrokeStyle(lineWidth: 3, dash: [12, 10]))
                    .padding(20)
                    .overlay(Text("Drop games to add them").font(Orbit.display(26, 600)).foregroundStyle(Orbit.lavender))
                    .allowsHitTesting(false)
            }
        }
        .foregroundStyle(Orbit.text)
        .ignoresSafeArea()
        .background(OrbitWindowConfig())
        .environmentObject(state)
        .dropDestination(for: URL.self) { urls, _ in
            library.add(urls: urls); ActivityLog.shared.log("Adding \(urls.count) item(s) to the library")
            return true
        } isTargeted: { state.dropTargeted = $0 }
        .fileImporter(isPresented: $state.showImporter, allowedContentTypes: [.item, .folder], allowsMultipleSelection: true) { r in
            if case .success(let urls) = r { library.add(urls: urls); ActivityLog.shared.log("Adding \(urls.count) item(s) to the library") }
        }
        .sheet(item: $state.memoryCheckFor) { entry in
            MemoryCheckSheet(gameName: entry.info.name, neededMB: NativeEngine.shared.nativeApp(entry.info).map { MemoryCheck.neededMB($0) } ?? 10_000) {
                launch(entry)
            }
        }
        .sheet(item: $state.doctorFor) { entry in
            if let r = PerformanceDoctor.shared.reports[entry.info.titleIDString] {
                DoctorReportView(report: r, app: NativeEngine.shared.nativeApp(entry.info))
            }
        }
        .sheet(item: $state.savesFor) { entry in
            if let app = NativeEngine.shared.nativeApp(entry.info) { SaveTimelineView(app: app, name: entry.info.name) }
        }
        .onChange(of: state.selectedID) { _ in selectionChanged() }
        .onReceive(NotificationCenter.default.publisher(for: .GCControllerDidConnect)) { _ in selectionChanged() }
        .sheet(item: $state.optionsFor) { entry in
            GameDetailView(entry: entry) { state.optionsFor = nil; play(entry) }
                .environmentObject(library)
                .preferredColorScheme(.dark)
        }
        .sheet(isPresented: $showSetup) { EngineSetupView { showSetup = false }.preferredColorScheme(.dark) }
        .onChange(of: showIntro) { on in if !on && !EngineSetup.shared.ready { showSetup = true } }   // first run: offer the setup once the intro is over
        .onReceive(NotificationCenter.default.publisher(for: .mgrAddGames)) { _ in state.showImporter = true }
        .onReceive(NotificationCenter.default.publisher(for: .gizzyPlayIntro)) { _ in showIntro = true }
        .onReceive(NotificationCenter.default.publisher(for: .mgrRetroTab)) { n in
            guard let t = n.object as? RetroTab else { return }
            switch t {
            case .games: state.screen = .orbit; state.node = .all
            case .favorites: state.screen = .orbit; state.node = .favorites
            case .recent: state.screen = .orbit; state.node = .recent
            case .controllers: state.screen = .controllers
            case .settings: state.screen = .settings
            }
        }
        .onAppear {
            if !showIntro { DispatchQueue.main.asyncAfter(deadline: .now() + 0.3) { IntroDriver.enterFullScreen() } }
            if !showIntro && !EngineSetup.shared.ready { DispatchQueue.main.asyncAfter(deadline: .now() + 1.0) { showSetup = true } }
            _ = ControllerWatch.shared
            _ = TwitchAccount.shared
            HubPad.shared.handler = { a in padAction(a) }
            HubPad.shared.start()
            for e in library.entries { NativeEngine.shared.adoptIfRunning(e.info) }   // a preparation still running from before a restart
            if ActivityLog.shared.lines.isEmpty {
                ActivityLog.shared.log("Welcome to Gizzy's Gaming Hub")
                ActivityLog.shared.log("Library loaded: \(library.entries.count) game\(library.entries.count == 1 ? "" : "s")")
            }
            if state.selectedID == nil { state.selectedID = library.entries(for: .recent).first?.id ?? library.entries(for: .all).first?.id }
            ReplayRecorder.shared.installHotKey()
            StorageCenter.shared.startWatching()
            DiscordPresence.shared.refresh()
            PrepQueue.shared.start()
            PerformanceDoctor.shared.catchUp(library.entries)
            for e in library.entries {   // a first save snapshot for games played before the time machine existed
                if let app = NativeEngine.shared.nativeApp(e.info), !NativeSessions.shared.isRunning(app), SaveVault.shared.list(app.titleID).isEmpty {
                    SaveVault.shared.snapshot(app, label: "First snapshot", minutes: nil)
                }
            }
            selectionChanged()
        }
    }

    /// Launch the prepared game; a game that is not prepared opens its options, where it can be prepared.
    func play(_ entry: LibraryEntry) {
        if let app = NativeEngine.shared.nativeApp(entry.info), !NativeSessions.shared.isRunning(app),
           PerformanceDoctor.shared.memoryCheckOn, MemoryCheck.availableMB() < MemoryCheck.neededMB(app),
           MemoryCheck.heavyApps().reduce(0, { $0 + $1.mb }) >= 800 {
            state.memoryCheckFor = entry
            return
        }
        launch(entry)
    }

    // MARK: controller navigation (HubPad)

    func padAction(_ a: PadAction) {
        if showIntro { return }
        let tv = TwitchTV.shared
        // a sheet is open: B closes it; A launches from a game's options (or past the memory check)
        if state.optionsFor != nil || state.memoryCheckFor != nil || state.doctorFor != nil || state.savesFor != nil {
            switch a {
            case .back:
                state.optionsFor = nil; state.memoryCheckFor = nil; state.doctorFor = nil; state.savesFor = nil
            case .accept:
                if let e = state.optionsFor { state.optionsFor = nil; play(e) }
                else if let e = state.memoryCheckFor { state.memoryCheckFor = nil; launch(e) }
            default: break
            }
            return
        }
        switch a {
        case .settings: state.screen = state.screen == .settings ? .orbit : .settings; return
        case .controllers: state.screen = state.screen == .controllers ? .orbit : .controllers; return
        case .back: state.screen = .orbit; return
        case .channelPrev, .channelNext: padChannel(a == .channelNext ? 1 : -1); return
        case .mute: tv.muted.toggle(); return
        case .bigTV:
            if tv.bigOpen { tv.bringBack() } else { tv.on = true; tv.bigOpen = true; openWindow(id: "tv") }
            return
        default: break
        }
        guard state.screen == .orbit else { return }
        let list = library.entries(for: state.node)
        let current = list.firstIndex { $0.id == state.selectedID } ?? 0
        switch a {
        case .left, .right:
            guard !list.isEmpty else { return }
            state.selectedID = list[(current + (a == .right ? 1 : -1) + list.count) % list.count].id
        case .up, .down:
            let systems = padSystems
            let i = systems.firstIndex(of: state.node) ?? 0
            state.node = systems[(i + (a == .down ? 1 : -1) + systems.count) % systems.count]
            let now = library.entries(for: state.node)
            if !now.contains(where: { $0.id == state.selectedID }) { state.selectedID = now.first?.id }
        case .accept:
            if list.indices.contains(current) { play(list[current]) }
        case .options:
            if list.indices.contains(current) { state.optionsFor = list[current] }
        case .favorite:
            if list.indices.contains(current) { library.toggleFavorite(list[current].info.path) }
        default: break
        }
    }

    private var padSystems: [ShelfNode] {
        var s: [ShelfNode] = [.all, .favorites, .recent]
        let platforms = Platform.allCases.filter { p in library.entries.contains { $0.info.platform == p } }
        if platforms.count > 1 { s += platforms.map { .platform($0) } }
        return s
    }

    /// Flips through the TV's recent channels (the channel menu adds to them).
    private func padChannel(_ step: Int) {
        let tv = TwitchTV.shared
        var list = tv.recent
        if !list.contains(tv.channel) { list.insert(tv.channel, at: 0) }
        guard list.count > 1, let i = list.firstIndex(of: tv.channel) else {
            ActivityLog.shared.log("TV: watch a few channels from the TV's channel menu to flip through them")
            return
        }
        tv.on = true
        tv.channel = list[(i + step + list.count) % list.count]
        ActivityLog.shared.log("TV: \(tv.channel)")
    }

    /// The selected game sets the controller lights and, when the TV follows games, the channel.
    func selectionChanged() {
        guard let e = library.entries.first(where: { $0.id == state.selectedID }) else { return }
        ControllerLights.set(GameColor.of(e))
        TwitchTV.shared.currentGame = e.info.name
        TwitchTV.shared.follow(e)
    }

    func launch(_ entry: LibraryEntry) {
        if NativeEngine.shared.play(entry.info) {
            library.markPlayed(entry.info.path)
            ActivityLog.shared.log("Launching \(entry.info.name)")
        } else {
            ActivityLog.shared.log("\(entry.info.name) needs preparing first: see Game options")
            state.optionsFor = entry
        }
    }
}

/// Transparent title bar over the sky; the window moves by its background.
struct OrbitWindowConfig: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { V() }
    func updateNSView(_ v: NSView, context: Context) {}
    final class V: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let w = window else { return }
            for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] { w.standardWindowButton(b)?.isHidden = false }
            w.titlebarAppearsTransparent = true
            w.titleVisibility = .hidden
            w.isMovableByWindowBackground = true
            w.backgroundColor = NSColor(red: 0.04, green: 0.047, blue: 0.133, alpha: 1)
            w.appearance = NSAppearance(named: .darkAqua)
        }
    }
}

/// The night sky: fixed stars that twinkle very slowly.
struct Starfield: View {
    @ObservedObject private var sessions = NativeSessions.shared
    private struct Star { let x, y, r, phase, speed: Double }
    private let stars: [Star] = {
        var s: UInt64 = 0x9E3779B97F4A7C15
        func rnd() -> Double { s = s &* 6364136223846793005 &+ 1442695040888963407; return Double(s >> 11) / Double(1 << 53) }
        return (0..<170).map { _ in Star(x: rnd(), y: rnd(), r: rnd() < 0.15 ? 1.6 : 0.9, phase: rnd() * 6.28, speed: 0.2 + rnd() * 0.5) }
    }()

    var body: some View {
        // paused while a game runs, so the game gets the whole GPU
        TimelineView(.animation(minimumInterval: 1 / 15, paused: !sessions.running.isEmpty)) { tl in
            let t = tl.date.timeIntervalSinceReferenceDate
            Canvas { ctx, size in
                for st in stars {
                    let a = 0.25 + 0.45 * (0.5 + 0.5 * sin(t * st.speed + st.phase))
                    let rect = CGRect(x: st.x * size.width, y: st.y * size.height, width: st.r * 2, height: st.r * 2)
                    ctx.fill(Path(ellipseIn: rect), with: .color(Orbit.text.opacity(a * starBrightness(tl.date))))
                }
            }
        }
        .allowsHitTesting(false)
    }
}

// MARK: navigation

struct OrbitNav: View {
    @EnvironmentObject var library: LibraryStore
    @EnvironmentObject var state: OrbitState
    @ObservedObject private var controllers = ControllerWatch.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 4) {
            Group {
                if let url = Bundle.main.url(forResource: "GizzyLogo", withExtension: "png"), let logo = NSImage(contentsOf: url) {
                    Image(nsImage: logo).resizable().interpolation(.high).aspectRatio(contentMode: .fit)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .accessibilityLabel("Gizzy's Gaming Hub")
                } else {
                    Text("Gizzy's\nGaming Hub").font(Orbit.display(21, 800)).lineSpacing(2)
                }
            }
            .padding(.bottom, 14)
            HStack(spacing: 8) {
                pill("gamecontroller.fill", "Controllers", on: state.screen == .controllers) { state.screen = .controllers }
                pill("gearshape.fill", "Settings", on: state.screen == .settings) { state.screen = .settings }
            }
            .padding(.bottom, 24)
            HStack {
                OrbitLabel("Systems")
                Spacer()
                Button { state.showImporter = true } label: {
                    Text("Add Game").font(Orbit.body(13, 700))
                        .lineLimit(1)
                        .fixedSize()
                    .foregroundStyle(Orbit.lavender)
                    .padding(.horizontal, 11).frame(height: 28)
                    .background(Capsule().strokeBorder(Orbit.line, lineWidth: 1))
                    .contentShape(Capsule())
                }
                .buttonStyle(.plain)
                .help("Add games")
                .accessibilityLabel("Add games")
            }
            .padding(.bottom, 4)
            ForEach(systems, id: \.self) { n in
                row(n.title, count: library.entries(for: n).count, on: state.screen == .orbit && state.node == n) {
                    state.screen = .orbit; state.node = n
                    let list = library.entries(for: n)
                    if !list.contains(where: { $0.id == state.selectedID }), let f = list.first { state.selectedID = f.id }
                }
            }
            Spacer(minLength: 0)
        }
        .padding(.horizontal, 26)
        .padding(.top, 56)
        .padding(.bottom, 26)
        .frame(maxHeight: .infinity, alignment: .top)
    }

    private var systems: [ShelfNode] {
        var s: [ShelfNode] = [.all, .favorites, .recent]
        let platforms = Platform.allCases.filter { p in library.entries.contains { $0.info.platform == p } }
        if platforms.count > 1 { s += platforms.map { .platform($0) } }
        return s
    }

    private func row(_ title: String, count: Int?, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            HStack {
                Text(title).font(Orbit.body(16, on ? 700 : 500)).foregroundStyle(on ? Orbit.text : Orbit.dim)
                Spacer()
                if let count { Text("\(count)").font(Orbit.body(14, 700)).foregroundStyle(on ? Orbit.lavender : Orbit.mute) }
            }
            .padding(.horizontal, 14)
            .frame(height: 44)
            .background(RoundedRectangle(cornerRadius: 12).fill(on ? Orbit.panel : Color.clear))
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
    }

    private func pill(_ symbol: String, _ title: String, on: Bool, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Text(title).font(Orbit.body(13, 700)).lineLimit(1).fixedSize()
                .foregroundStyle(on ? Orbit.text : Orbit.dim)
                .padding(.horizontal, 12).frame(height: 32)
                .background(Capsule().fill(on ? Orbit.panel : Color.clear))
                .overlay(Capsule().strokeBorder(Orbit.line, lineWidth: on ? 0 : 1))
                .contentShape(Capsule())
        }
        .buttonStyle(.plain)
    }

    private var crew: some View {
        let c = controllers.controllers.first
        return VStack(alignment: .leading, spacing: 6) {
            OrbitLabel("Crew")
            HStack(spacing: 8) {
                Circle().fill(c != nil ? Orbit.mint : Orbit.peach).frame(width: 8, height: 8)
                Text("Player 1 · " + (c.map { ($0.vendorName ?? "Controller").replacingOccurrences(of: " Wireless Controller", with: "") } ?? "Keyboard"))
                    .font(Orbit.body(15, 700)).lineLimit(1)
            }
            if let b = c?.battery {
                Text("Battery \(Int(b.batteryLevel * 100))%" + (b.batteryState == .charging ? ", charging" : ""))
                    .font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute)
            } else if c == nil {
                Text("Pair a controller in Bluetooth settings").font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute)
            }
        }
        .padding(16)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 16).strokeBorder(Orbit.line, lineWidth: 1))
    }
}

// MARK: the orbit

struct OrbitMain: View {
    @EnvironmentObject var library: LibraryStore
    @EnvironmentObject var state: OrbitState
    let play: (LibraryEntry) -> Void

    var body: some View {
        let list = library.entries(for: state.node)
        let selected = list.first { $0.id == state.selectedID } ?? list.first
        HStack(spacing: 40) {
            OrbitSystem(entries: list, selected: selected)
                .frame(maxWidth: .infinity, maxHeight: .infinity)
            MissionPanel(entry: selected, play: play)
                .frame(width: 380)
        }
        .padding(.leading, 170).padding(.trailing, 48).padding(.vertical, 36)
    }
}

/// The selected game in the middle; the others orbit it on two rings.
struct OrbitSystem: View {
    let entries: [LibraryEntry]
    let selected: LibraryEntry?
    @EnvironmentObject var state: OrbitState
    @EnvironmentObject var library: LibraryStore
    @ObservedObject private var sessions = NativeSessions.shared

    private let innerMax = 6, outerMax = 10

    var body: some View {
        GeometryReader { g in
            let side = min(g.size.width, g.size.height)
            let c = CGPoint(x: g.size.width / 2, y: g.size.height / 2)
            let others = entries.filter { $0.id != selected?.id }
            let inner = Array(others.prefix(innerMax))
            let outer = Array(others.dropFirst(innerMax).prefix(outerMax))
            let hidden = max(0, others.count - innerMax - outerMax)
            TimelineView(.animation(minimumInterval: 1 / 30, paused: !sessions.running.isEmpty)) { tl in
                let t = tl.date.timeIntervalSinceReferenceDate
                ZStack {
                    Circle().strokeBorder(Orbit.line, style: StrokeStyle(lineWidth: 1, dash: [3, 6]))
                        .frame(width: side, height: side).position(c)
                    Circle().strokeBorder(Orbit.ring, lineWidth: 1)
                        .frame(width: side * 0.72, height: side * 0.72).position(c)
                    Circle().strokeBorder(Orbit.lavender.opacity(0.25), lineWidth: 1)
                        .frame(width: side * 0.5, height: side * 0.5).position(c)

                    centre(side: side).position(c)

                    ForEach(Array(inner.enumerated()), id: \.element.id) { i, e in
                        let a = Double(i) / Double(max(inner.count, 1)) * 2 * .pi + t * 2 * .pi / 360 - .pi / 2
                        planet(e, size: side * (0.075 - Double(i % 3) * 0.008) * e.growth)
                            .position(x: c.x + cos(a) * side * 0.36, y: c.y + sin(a) * side * 0.36)
                    }
                    ForEach(Array(outer.enumerated()), id: \.element.id) { i, e in
                        let a = Double(i) / Double(max(outer.count, 1)) * 2 * .pi - t * 2 * .pi / 600 + 0.4
                        planet(e, size: side * (0.062 - Double(i % 3) * 0.007) * e.growth)
                            .position(x: c.x + cos(a) * side * 0.5, y: c.y + sin(a) * side * 0.5)
                    }
                    if hidden > 0 {
                        Text("+\(hidden) more in this system").font(Orbit.body(13, 700)).foregroundStyle(Orbit.mute)
                            .position(x: c.x, y: c.y + side * 0.5 + 24)
                    }
                }
                .animation(.spring(response: 0.6, dampingFraction: 0.8), value: selected?.id)
            }
        }
    }

    @ViewBuilder private func centre(side: CGFloat) -> some View {
        let d = side * 0.406
        if let e = selected {
            ZStack {
                if let img = entryImage(e) {
                    Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                        .frame(width: d, height: d).clipShape(Circle())
                } else {
                    Circle().fill(Orbit.planetColor(e.info.name)).frame(width: d, height: d)
                    Text(initials(e.info.name)).font(Orbit.display(d * 0.24, 800)).foregroundStyle(Orbit.ink)
                }
            }
            .overlay(Circle().strokeBorder(Orbit.lavender.opacity(0.14), lineWidth: 10).padding(-10))
            .shadow(color: Orbit.mint.opacity(0.28), radius: 50)
            .id(e.id)
            .transition(.scale(scale: 0.6).combined(with: .opacity))
            .onTapGesture(count: 2) { NotificationCenter.default.post(name: .gizzyPlaySelected, object: e.id) }
            .contextMenu { planetMenu(e) }
        } else {
            Button { state.showImporter = true } label: {
                VStack(spacing: 10) {
                    Image(systemName: "plus").font(.system(size: 34, weight: .light))
                    Text("Add your first game").font(Orbit.body(16, 700))
                }
                .foregroundStyle(Orbit.lavender)
                .frame(width: d, height: d)
                .background(Circle().strokeBorder(Orbit.lavender.opacity(0.5), style: StrokeStyle(lineWidth: 1.5, dash: [6, 6])))
                .contentShape(Circle())
            }
            .buttonStyle(.plain)
        }
    }

    private func planet(_ e: LibraryEntry, size: CGFloat) -> some View {
        PlanetView(entry: e, size: size)
            .onTapGesture { withAnimation(.spring(response: 0.6, dampingFraction: 0.8)) { state.selectedID = e.id } }
            .contextMenu { planetMenu(e) }
    }

    @ViewBuilder private func planetMenu(_ e: LibraryEntry) -> some View {
        Button(e.favorite == true ? "Remove from Favorites" : "Add to Favorites") { library.toggleFavorite(e.info.path) }
        Button("Game Options...") { state.optionsFor = e }
        Button("Set Cover Art...") { pickCoverArt(library, path: e.info.path) }
        if e.coverArt != nil { Button("Use the Game's Own Art") { library.setCoverArt(e.info.path, from: nil) } }
        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: e.info.path)]) }
        Divider()
        Button("Remove from Library", role: .destructive) { library.remove(e) }
    }
}

/// One orbiting game: its art in a circle (or a coloured planet with its initials) and its name.
struct PlanetView: View {
    let entry: LibraryEntry
    let size: CGFloat
    @State private var hover = false
    var body: some View {
        VStack(spacing: 6) {
            ZStack {
                if let img = entryImage(entry) {
                    Image(nsImage: img).resizable().aspectRatio(contentMode: .fill).frame(width: size, height: size).clipShape(Circle())
                } else {
                    Circle().fill(Orbit.planetColor(entry.info.name)).frame(width: size, height: size)
                    Text(initials(entry.info.name)).font(Orbit.display(size * 0.3, 800)).foregroundStyle(Orbit.ink)
                }
            }
            .shadow(color: Color(nsColor: GameColor.of(entry)).opacity(hover ? 0.7 : 0.15 + 0.55 * entry.recency), radius: hover ? 20 : 6 + 16 * entry.recency)
            Text(entry.info.name).font(Orbit.body(13, 700)).foregroundStyle(hover ? Orbit.text : Orbit.dim)
                .lineLimit(1).frame(maxWidth: 150)
        }
        .scaleEffect(hover ? 1.12 : 1)
        .animation(.spring(response: 0.3), value: hover)
        .onHover { hover = $0 }
        .contentShape(Rectangle())
        .help(entry.info.name)
    }
}

func initials(_ name: String) -> String {
    let skip: Set<String> = ["the", "a", "an", "of", "and"]
    let words = name.split { !$0.isLetter && !$0.isNumber }.filter { !skip.contains($0.lowercased()) }.prefix(2)
    let s = words.compactMap { $0.first.map(String.init) }.joined()
    return s.isEmpty ? "?" : s.uppercased()
}

// MARK: the selected game

struct MissionPanel: View {
    let entry: LibraryEntry?
    let play: (LibraryEntry) -> Void
    @EnvironmentObject var state: OrbitState
    @EnvironmentObject var library: LibraryStore
    @ObservedObject private var sessions = NativeSessions.shared
    @ObservedObject private var engine = NativeEngine.shared
    @ObservedObject private var doctor = PerformanceDoctor.shared
    @ObservedObject private var vault = SaveVault.shared
    @ObservedObject private var prep = PrepQueue.shared

    var body: some View {
        if let e = entry {
            let app = engine.nativeApp(e.info)
            let running = app.map { sessions.isRunning($0) } ?? false
            let progress = preparing(e)
            VStack(alignment: .leading, spacing: 16) {
                kicker(e, app: app, running: running, progress: progress)
                Text(e.info.name).font(Orbit.display(34, 600)).lineSpacing(4)
                    .lineLimit(4).minimumScaleFactor(0.6).fixedSize(horizontal: false, vertical: true)
                facts(e, app: app).padding(.top, 6)
                if let p = progress {
                    VStack(alignment: .leading, spacing: 6) {
                        Text(p.step).font(Orbit.body(14, 700)).lineLimit(1)
                        ProgressView(value: p.fraction ?? 0).tint(Orbit.lavender)
                    }
                    .padding(.top, 6)
                }
                VStack(spacing: 12) {
                    if running, let app {
                        Button("SHOW GAME") { sessions.start(app) }.buttonStyle(OrbitButtonStyle(height: 64))
                        Button("Quit game") { sessions.stop(app); ActivityLog.shared.log("Quitting \(e.info.name)") }
                            .buttonStyle(OrbitButtonStyle(primary: false, height: 50))
                    } else {
                        Button(app != nil ? "LAUNCH" : "PREPARE FOR LAUNCH") { play(e) }
                            .buttonStyle(OrbitButtonStyle(height: 64))
                            .disabled(progress != nil)
                            .opacity(progress != nil ? 0.5 : 1)
                        if app == nil, progress == nil {
                            Button(PrepQueue.shared.contains(e.info) ? "Queued for tonight ✓" : "Prepare tonight") {
                                if PrepQueue.shared.contains(e.info) { PrepQueue.shared.remove(e.info.path) } else { PrepQueue.shared.add(e.info) }
                            }
                            .buttonStyle(OrbitButtonStyle(primary: false, height: 44))
                        }
                        Button("Game options") { state.optionsFor = e }
                            .buttonStyle(OrbitButtonStyle(primary: false, height: 50))
                    }
                }
                .padding(.top, 14)
                HStack(spacing: 18) {
                    smallButton(e.favorite == true ? "star.fill" : "star", e.favorite == true ? "Favorite" : "Add to favorites") { library.toggleFavorite(e.info.path) }
                    smallButton("photo", "Cover art") { pickCoverArt(library, path: e.info.path) }
                    if app != nil { smallButton("clock.arrow.circlepath", "Saves") { state.savesFor = e } }
                    smallButton("folder", "Show file") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: e.info.path)]) }
                }
                .padding(.top, 4)
                if let r = PerformanceDoctor.shared.reports[e.info.titleIDString] {
                    Button { state.doctorFor = e } label: {
                        HStack(spacing: 10) {
                            Image(systemName: r.findings.contains { $0.kind != .smooth } ? "stethoscope" : "checkmark.seal.fill")
                                .foregroundStyle(r.findings.contains { $0.kind != .smooth } ? Orbit.peach : Orbit.mint)
                            VStack(alignment: .leading, spacing: 2) {
                                Text("Last session · \(formatMinutes(r.minutes))" + (r.smoothPercent.map { " · \($0)% smooth" } ?? "")).font(Orbit.body(13, 700))
                                Text(r.findings.contains { $0.kind != .smooth } ? "\(r.findings.filter { $0.kind != .smooth }.count) tips from Performance Doctor" : "Ran smoothly")
                                    .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
                            }
                            Spacer()
                            Image(systemName: "chevron.right").foregroundStyle(Orbit.mute)
                        }
                        .padding(12)
                        .background(RoundedRectangle(cornerRadius: 14).strokeBorder(Orbit.line, lineWidth: 1))
                        .contentShape(Rectangle())
                    }
                    .buttonStyle(.plain)
                    .padding(.top, 6)
                }
            }
            .frame(maxHeight: .infinity, alignment: .center)
        } else {
            VStack(alignment: .leading, spacing: 12) {
                OrbitLabel("Empty sky", color: Orbit.mint)
                Text(state.node == .favorites ? "No favorites yet" : state.node == .recent ? "Nothing played yet" : "No games yet")
                    .font(Orbit.display(30, 600))
                Text(state.node == .favorites ? "Right-click a planet and choose Add to Favorites."
                     : "Drag game files onto the window, or use Add games.")
                    .font(Orbit.body(16, 500)).foregroundStyle(Orbit.dim)
            }
            .frame(maxHeight: .infinity, alignment: .center)
        }
    }

    private func kicker(_ e: LibraryEntry, app: NativeApp?, running: Bool, progress: (step: String, fraction: Double?)?) -> some View {
        if running { return OrbitLabel("In flight · playing now", color: Orbit.mint) }
        if let p = progress { return OrbitLabel("Preparing" + (p.fraction.map { " · \(Int($0 * 100))%" } ?? ""), color: Orbit.peach) }
        return app != nil ? OrbitLabel("In orbit · prepared for Mac", color: Orbit.mint) : OrbitLabel("Not prepared yet", color: Orbit.peach)
    }

    private func facts(_ e: LibraryEntry, app: NativeApp?) -> some View {
        let s = app.map { NativeSettingsModel(app: $0) }
        let fps = s.flatMap { m in m.settings.first { $0.env == "DM_FPS_TARGET" }.map { m.value($0) + " fps" } } ?? "—"
        let mode = s.flatMap { m in m.settings.first { $0.env == "DM_OPMODE" }.map { m.value($0) == "0" ? "Handheld" : "Docked" } } ?? "—"
        return Grid(alignment: .leading, horizontalSpacing: 24, verticalSpacing: 16) {
            GridRow { fact("System", e.info.platform.rawValue); fact("Last flight", lastPlayedText(e.lastPlayed)) }
            GridRow { fact("Frame rate", fps); fact("Mode", mode) }
            GridRow { fact("Played", formatHours(e.playSeconds)); fact("Saves kept", "\(SaveVault.shared.list(e.info.titleIDString).count)") }
        }
    }

    private func fact(_ k: String, _ v: String) -> some View {
        VStack(alignment: .leading, spacing: 4) {
            OrbitLabel(k)
            Text(v).font(Orbit.body(17, 700)).lineLimit(1)
        }
        .frame(minWidth: 150, alignment: .leading)
    }

    private func smallButton(_ symbol: String, _ title: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Label(title, systemImage: symbol).font(Orbit.body(13, 700)).foregroundStyle(Orbit.dim)
        }
        .buttonStyle(.plain)
    }

    private func preparing(_ e: LibraryEntry) -> (step: String, fraction: Double?)? {
        if case .running(let step, _, let f)? = engine.builds[e.info.path]?.phase { return (step, f) }
        return nil
    }
}

// MARK: flight log

struct FlightLogBar: View {
    @ObservedObject private var log = ActivityLog.shared
    @ObservedObject private var sessions = NativeSessions.shared
    @EnvironmentObject var library: LibraryStore

    var body: some View {
        HStack(spacing: 24) {
            Text("FLIGHT LOG").font(Orbit.display(13, 600)).tracking(1.2)
            ForEach(Array(log.lines.suffix(3).reversed().enumerated()), id: \.offset) { i, l in
                Text(l).font(Orbit.body(14, 500)).foregroundStyle(i == 0 ? Orbit.dim : Orbit.mute).lineLimit(1)
            }
            Spacer(minLength: 12)
            if replay.recording {
                HStack(spacing: 5) { Circle().fill(Color(rgb: 0xFF5A5A)).frame(width: 7, height: 7); Text("Replay · ⌃⌘R").font(Orbit.body(13, 700)) }
            }
            if !prep.status.isEmpty { Text(prep.status).font(Orbit.body(13, 700)).foregroundStyle(Orbit.lavender).lineLimit(1) }
            playerOne
            if let s = sessions.running.values.first {
                HStack(spacing: 8) {
                    Circle().fill(Orbit.mint).frame(width: 8, height: 8)
                    Text("Playing").font(Orbit.body(14, 700))
                    Text(s.started, style: .timer).font(Orbit.body(14, 700)).monospacedDigit().foregroundStyle(Orbit.mint)
                }
            } else {
                Text(NativeEngine.shared.checkTools().allSatisfy(\.ok) ? "Native engine ready" : "Native engine needs setup")
                    .font(Orbit.body(14, 700)).foregroundStyle(Orbit.mint)
            }
        }
        .padding(.horizontal, 30)
        .frame(height: 58)
        .overlay(alignment: .top) { Rectangle().fill(Orbit.ring).frame(height: 1) }
    }

    @ObservedObject private var controllers = ControllerWatch.shared
    @ObservedObject private var replay = ReplayRecorder.shared
    @ObservedObject private var prep = PrepQueue.shared
    private var playerOne: some View {
        let c = controllers.controllers.first
        let name = c.map { ($0.vendorName ?? "Controller").replacingOccurrences(of: " Wireless Controller", with: "") } ?? "Keyboard"
        let battery = c?.battery.map { " \(Int($0.batteryLevel * 100))%" } ?? ""
        return HStack(spacing: 6) {
            Circle().fill(c != nil ? Orbit.mint : Orbit.peach).frame(width: 7, height: 7)
            Text("Player 1 · \(name)\(battery)").font(Orbit.body(14, 700)).foregroundStyle(Orbit.dim).lineLimit(1)
        }
    }
}

// MARK: station screens

struct OrbitControllers: View {
    @ObservedObject private var watch = ControllerWatch.shared
    private let keys: [(String, String)] = [
        ("Left stick", "W A S D"), ("Right stick (camera)", "Shift + arrows"), ("D-pad", "Arrow keys"),
        ("A / B / X / Y", "J / K / U / I"), ("L / R", "Q / E"), ("ZL / ZR", "Z / C"), ("Stick clicks", "F / G"),
        ("+ / −", "Return / Tab"), ("Shake", "Space"), ("Full screen", "Ctrl + Cmd + F"), ("Quit", "Cmd + Q"),
    ]

    var body: some View {
        HStack(alignment: .top, spacing: 28) {
            VStack(alignment: .leading, spacing: 16) {
                OrbitLabel("Station", color: Orbit.mint)
                Text("Controllers").font(Orbit.display(34, 600))
                if watch.controllers.isEmpty {
                    Text("No controller connected. Pair a PS5, Xbox or Switch Pro controller in System Settings › Bluetooth, or fly with the keyboard.")
                        .font(Orbit.body(16, 500)).foregroundStyle(Orbit.dim).fixedSize(horizontal: false, vertical: true)
                }
                ForEach(Array(watch.controllers.enumerated()), id: \.offset) { i, c in
                    HStack(spacing: 16) {
                        Circle().fill(Orbit.planetColors.indices.contains(i) ? Color(rgb: Orbit.planetColors[i]) : Orbit.mint).frame(width: 44, height: 44)
                            .overlay(Text("P\(i + 1)").font(Orbit.display(14, 800)).foregroundStyle(Orbit.ink))
                        VStack(alignment: .leading, spacing: 3) {
                            Text(c.vendorName ?? "Controller").font(Orbit.body(17, 700))
                            if let b = c.battery {
                                Text("Battery \(Int(b.batteryLevel * 100))%" + (b.batteryState == .charging ? ", charging" : ""))
                                    .font(Orbit.body(14, 500)).foregroundStyle(Orbit.mute)
                            }
                        }
                        Spacer()
                        if let l = c.light {
                            Circle().fill(Color(red: Double(l.color.red), green: Double(l.color.green), blue: Double(l.color.blue))).frame(width: 14, height: 14)
                                .help("Light bar colour: the selected game's colour")
                        }
                        Button("Identify") { ControllerLights.identify(c) }.disabled(c.light == nil)
                    }
                    .padding(16)
                    .background(RoundedRectangle(cornerRadius: 18).fill(Orbit.card))
                    .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(Orbit.line, lineWidth: 1))
                }
                Text("Buttons are matched by position: on a PS5 controller, Circle is A, Cross is B, Triangle is X and Square is Y.")
                    .font(Orbit.body(14, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
                Spacer()
            }
            .frame(maxWidth: .infinity, alignment: .leading)

            VStack(alignment: .leading, spacing: 14) {
                OrbitLabel("Keyboard")
                VStack(spacing: 0) {
                    ForEach(Array(keys.enumerated()), id: \.offset) { i, k in
                        HStack {
                            Text(k.0).font(Orbit.body(15, 700))
                            Spacer()
                            Text(k.1).font(.system(size: 14, weight: .semibold, design: .monospaced)).foregroundStyle(Orbit.lavender)
                        }
                        .padding(.horizontal, 18).frame(height: 42)
                        .background(i % 2 == 0 ? Orbit.card : Color.clear)
                    }
                }
                .clipShape(RoundedRectangle(cornerRadius: 18))
                .overlay(RoundedRectangle(cornerRadius: 18).strokeBorder(Orbit.line, lineWidth: 1))
                Spacer()
            }
            .frame(maxWidth: 460)
        }
        .padding(.horizontal, 40).padding(.top, 56).padding(.bottom, 24)
    }
}

struct OrbitSettings: View {
    @EnvironmentObject var library: LibraryStore
    @ObservedObject private var doctor = PerformanceDoctor.shared

    private func featureCard<C: View>(_ title: String, _ symbol: String, @ViewBuilder _ content: () -> C) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            Label(title, systemImage: symbol).font(Orbit.body(16, 700))
            content()
        }
        .padding(18)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(RoundedRectangle(cornerRadius: 20).fill(Orbit.card))
        .overlay(RoundedRectangle(cornerRadius: 20).strokeBorder(Orbit.line, lineWidth: 1))
        .preferredColorScheme(.dark)
    }
    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            OrbitLabel("Station", color: Orbit.mint)
            Text("Settings").font(Orbit.display(34, 600))
            ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                featureCard("Launch intro", "sparkles") { IntroSettings() }
                featureCard("Performance Doctor", "stethoscope") {
                    Toggle("Check free memory before launching a game", isOn: $doctor.memoryCheckOn).toggleStyle(.switch)
                    Text("After every session the Doctor reads the game's log and explains any slowdowns, with one-click fixes: see the card under the Launch button.").font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
                }
                featureCard("Storage", "externaldrive.fill") { StorageSettings() }
                featureCard("Quick menu", "gamecontroller") { QuickMenuSettings() }
                featureCard("Instant replay", "record.circle") { ReplaySettings() }
                featureCard("Discord status", "person.2.wave.2.fill") { DiscordSettings() }
                featureCard("Smart TV", "tv") { SmartTVSettings() }
                featureCard("Overnight preparation", "moon.stars.fill") { PrepQueueSettings().environmentObject(library) }
            SettingsView()
                .environmentObject(library)
                .padding(10)
                .background(RoundedRectangle(cornerRadius: 20).fill(Orbit.card))
                .overlay(RoundedRectangle(cornerRadius: 20).strokeBorder(Orbit.line, lineWidth: 1))
                .preferredColorScheme(.dark)
                .frame(height: 460)
            }
            }
        }
        .padding(.horizontal, 40).padding(.top, 56).padding(.bottom, 24)
        .frame(maxWidth: 900, maxHeight: .infinity, alignment: .topLeading)
        .frame(maxWidth: .infinity, alignment: .leading)
    }
}

extension Notification.Name {
    static let gizzyPlaySelected = Notification.Name("gizzyPlaySelected")
}
