import SwiftUI
import AppKit
import GameController

// Mike's Game Room, 90s edition: a desktop-app look of the late 90s (bevelled silver buttons, navy panels,
// gold tabs, a pixel-art banner, a green-on-black activity log). All the art here is original and drawn in
// code; the launch panel shows the selected game's own icon, read from the game file.

// MARK: palette and building blocks

enum Retro {
    static let desk = Color(rgb: 0x0A1A5C)          // window background
    static let panel = Color(rgb: 0x0E2272)         // panel body
    static let panelDeep = Color(rgb: 0x081650)
    static let headerTop = Color(rgb: 0x3557E0)
    static let headerBottom = Color(rgb: 0x132C99)
    static let frame = Color(rgb: 0x6F86E8)
    static let silverTop = Color(rgb: 0xF2F2F2)
    static let silverBottom = Color(rgb: 0xBDBDBD)
    static let goldTop = Color(rgb: 0xFFD84A)
    static let goldBottom = Color(rgb: 0xFF9500)
    static let select = Color(rgb: 0x1E46E6)
    static let rowA = Color(rgb: 0xF0F0F0)
    static let rowB = Color(rgb: 0xE2E2E6)
    static let ink = Color(rgb: 0x111111)
    static let terminal = Color(rgb: 0x060B1E)
    static let led = Color(rgb: 0x3BE04A)

    static func font(_ size: CGFloat, _ weight: Font.Weight = .heavy) -> Font { .system(size: size, weight: weight, design: .default) }
}

extension Color {
    init(rgb: UInt32) {
        self.init(red: Double((rgb >> 16) & 0xFF) / 255, green: Double((rgb >> 8) & 0xFF) / 255, blue: Double(rgb & 0xFF) / 255)
    }
}

/// The two-tone edge of a 90s control: light on the top and left, dark on the bottom and right (swapped when pressed).
struct BevelEdge: Shape {
    var top: Bool
    func path(in r: CGRect) -> Path {
        var p = Path(); let i: CGFloat = 1
        if top {
            p.move(to: CGPoint(x: r.minX + i, y: r.maxY - i)); p.addLine(to: CGPoint(x: r.minX + i, y: r.minY + i)); p.addLine(to: CGPoint(x: r.maxX - i, y: r.minY + i))
        } else {
            p.move(to: CGPoint(x: r.maxX - i, y: r.minY + i)); p.addLine(to: CGPoint(x: r.maxX - i, y: r.maxY - i)); p.addLine(to: CGPoint(x: r.minX + i, y: r.maxY - i))
        }
        return p
    }
}

extension View {
    func bevel(raised: Bool = true, width: CGFloat = 2) -> some View {
        overlay(ZStack {
            BevelEdge(top: true).stroke(raised ? Color.white : Color.black.opacity(0.65), lineWidth: width)
            BevelEdge(top: false).stroke(raised ? Color.black.opacity(0.65) : Color.white, lineWidth: width)
        })
    }
}

/// Silver push button.
struct RetroButtonStyle: ButtonStyle {
    var size: CGFloat = 16
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .font(Retro.font(size, .bold))
            .foregroundStyle(Retro.ink)
            .padding(.horizontal, 14).padding(.vertical, 7)
            .frame(minHeight: 34)
            .background(LinearGradient(colors: configuration.isPressed ? [Retro.silverBottom, Retro.silverTop] : [Retro.silverTop, Retro.silverBottom],
                                       startPoint: .top, endPoint: .bottom))
            .bevel(raised: !configuration.isPressed)
            .overlay(Rectangle().strokeBorder(Color.black.opacity(0.55), lineWidth: 1))
            .offset(y: configuration.isPressed ? 1 : 0)
    }
}

/// The big orange launch button.
struct LaunchButtonStyle: ButtonStyle {
    func makeBody(configuration: Configuration) -> some View {
        configuration.label
            .frame(maxWidth: .infinity, minHeight: 74)
            .background(
                RoundedRectangle(cornerRadius: 12)
                    .fill(LinearGradient(colors: configuration.isPressed ? [Retro.goldBottom, Retro.goldTop] : [Color(rgb: 0xFFB82E), Color(rgb: 0xFF6A00)],
                                         startPoint: .top, endPoint: .bottom))
            )
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color(rgb: 0xFFE9A8), lineWidth: 3).padding(3))
            .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color(rgb: 0x8A3300), lineWidth: 3))
            .shadow(color: .black.opacity(0.5), radius: 0, x: 0, y: configuration.isPressed ? 1 : 4)
            .offset(y: configuration.isPressed ? 3 : 0)
    }
}

/// A navy panel with a blue title strip.
struct RetroPanel<Content: View>: View {
    let title: String
    let symbol: String
    var symbolColor: Color = .yellow
    @ViewBuilder var content: Content

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 10) {
                Image(systemName: symbol).font(.system(size: 18, weight: .bold)).foregroundStyle(symbolColor)
                    .shadow(color: .black.opacity(0.6), radius: 0, x: 1, y: 1)
                Text(title.uppercased()).font(Retro.font(19)).foregroundStyle(.white)
                    .shadow(color: .black.opacity(0.6), radius: 0, x: 1, y: 1)
                Spacer()
            }
            .padding(.horizontal, 12)
            .frame(height: 40)
            .background(LinearGradient(colors: [Retro.headerTop, Retro.headerBottom], startPoint: .top, endPoint: .bottom))
            .overlay(alignment: .bottom) { Rectangle().fill(Color.black.opacity(0.5)).frame(height: 2) }
            content.frame(maxWidth: .infinity, maxHeight: .infinity)
        }
        .background(Retro.panel)
        .clipShape(RoundedRectangle(cornerRadius: 7))
        .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(Retro.frame, lineWidth: 3))
        .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(Color.black.opacity(0.6), lineWidth: 1).padding(3))
    }
}

/// Draws a small picture from rows of characters (one colour per character, "." is clear).
struct PixelSprite: View {
    let rows: [String]
    let palette: [Character: Color]
    var body: some View {
        Canvas { ctx, size in
            let h = CGFloat(rows.count), w = CGFloat(rows.map(\.count).max() ?? 1)
            let px = min(size.width / w, size.height / h)
            let ox = (size.width - px * w) / 2, oy = (size.height - px * h) / 2
            for (y, row) in rows.enumerated() {
                for (x, ch) in row.enumerated() {
                    guard let c = palette[ch] else { continue }
                    ctx.fill(Path(CGRect(x: ox + CGFloat(x) * px, y: oy + CGFloat(y) * px, width: px + 0.5, height: px + 0.5)), with: .color(c))
                }
            }
        }
    }

    // An original player avatar: a kid in a white space helmet.
    static let avatar = PixelSprite(rows: [
        "....WWWWWW....",
        "..WWWWWWWWWW..",
        ".WWBBBBBBBBWW.",
        ".WBHHHHHHHHBW.",
        "WWBHSSSSSSHBWW",
        "WWBSSKSSKSSBWW",
        "WWBSSSSSSSSBWW",
        "WWBSSMMMMSSBWW",
        ".WBBSSSSSSBBW.",
        "..WWBBBBBBWW..",
        ".RRWWWWWWWWRR.",
        "RRRWWOWWOWWRRR",
        "RRRWWWWWWWWRRR",
        ".RRWWWWWWWWRR.",
    ], palette: ["W": Color(rgb: 0xF4F6FF), "B": Color(rgb: 0x2B3A8C), "H": Color(rgb: 0x6B3A1E), "S": Color(rgb: 0xF2C29B),
                 "K": Color(rgb: 0x1A1A1A), "M": Color(rgb: 0xC0392B), "R": Color(rgb: 0xE23B3B), "O": Color(rgb: 0x39C0FF)])

    // An original little rocket for the launch button.
    static let rocket = PixelSprite(rows: [
        ".......WR",
        "......WWR",
        ".....WBWW",
        "....WWWW.",
        ".R.WWWW..",
        "RRWWWW...",
        ".RWWW....",
        ".YOR.R...",
        "YOY..R...",
        "YY.......",
    ], palette: ["W": Color(rgb: 0xF4F6FF), "R": Color(rgb: 0xE03030), "B": Color(rgb: 0x3AA0FF), "Y": Color(rgb: 0xFFE14A), "O": Color(rgb: 0xFF7A00)])
}

// MARK: shared state

enum ShelfNode: Hashable {
    case all, favorites, recent, platform(Platform), attention

    var title: String {
        switch self {
        case .all: return "All Games"
        case .favorites: return "Favorites"
        case .recent: return "Recently Played"
        case .platform(let p): return p.rawValue
        case .attention: return "Needs Attention"
        }
    }
    var symbol: String {
        switch self {
        case .all: return "square.grid.2x2.fill"
        case .favorites: return "star.fill"
        case .recent: return "clock.fill"
        case .platform(let p): return p == .switch2 ? "2.circle.fill" : p == .switch1 ? "1.circle.fill" : "hammer.fill"
        case .attention: return "exclamationmark.triangle.fill"
        }
    }
    var badgeColor: Color {
        switch self {
        case .favorites: return .red
        case .recent: return Color(rgb: 0x333333)
        case .platform(let p): return p == .switch2 ? Brand.pink : p == .switch1 ? .red : .green
        case .attention: return .orange
        default: return .blue
        }
    }
}

enum RetroTab: String, CaseIterable, Identifiable {
    case games = "My Games", favorites = "Favorites", recent = "Recent", controllers = "Controllers", settings = "Settings"
    var id: String { rawValue }
}

/// What is shown: the tab, the shelf node, the selected game, list or grid.
@MainActor
final class RetroState: ObservableObject {
    @Published var tab: RetroTab = .games
    @Published var shelf: ShelfNode = .all
    @Published var selectedID: String?
    @Published var grid = false
    @Published var optionsFor: LibraryEntry?
    @Published var showImporter = false
}

/// The room's activity log, shown at the bottom of the window.
@MainActor
final class ActivityLog: ObservableObject {
    static let shared = ActivityLog()
    @Published private(set) var lines: [String] = []
    func log(_ s: String) {
        lines.append(s)
        if lines.count > 200 { lines.removeFirst(lines.count - 200) }
    }
}

/// Connected game controllers (GameController framework).
@MainActor
final class ControllerWatch: ObservableObject {
    static let shared = ControllerWatch()
    @Published private(set) var controllers: [GCController] = []
    private init() {
        controllers = GCController.controllers()
        let nc = NotificationCenter.default
        nc.addObserver(forName: .GCControllerDidConnect, object: nil, queue: .main) { n in
            Task { @MainActor in
                let c = n.object as? GCController
                ControllerWatch.shared.controllers = GCController.controllers()
                ActivityLog.shared.log("Controller connected: \(c?.vendorName ?? "controller").")
            }
        }
        nc.addObserver(forName: .GCControllerDidDisconnect, object: nil, queue: .main) { n in
            Task { @MainActor in
                let c = n.object as? GCController
                ControllerWatch.shared.controllers = GCController.controllers()
                ActivityLog.shared.log("Controller disconnected: \(c?.vendorName ?? "controller").")
            }
        }
        GCController.startWirelessControllerDiscovery {}
    }
}

extension LibraryStore {
    func entries(for node: ShelfNode) -> [LibraryEntry] {
        let byName = entries.sorted { $0.info.name.localizedCaseInsensitiveCompare($1.info.name) == .orderedAscending }
        switch node {
        case .all: return byName
        case .favorites: return byName.filter { $0.favorite == true }
        case .recent: return entries.filter { $0.lastPlayed != nil }.sorted { ($0.lastPlayed ?? .distantPast) > ($1.lastPlayed ?? .distantPast) }
        case .platform(let p): return byName.filter { $0.info.platform == p }
        case .attention: return byName.filter { !$0.info.bootable }
        }
    }
}

func lastPlayedText(_ d: Date?) -> String {
    guard let d else { return "Never" }
    if Date().timeIntervalSince(d) < 60 { return "Just now" }
    let f = RelativeDateTimeFormatter(); f.dateTimeStyle = .named; f.unitsStyle = .full
    return f.localizedString(for: d, relativeTo: Date()).capitalizedFirst
}

extension String {
    var capitalizedFirst: String { prefix(1).uppercased() + dropFirst() }
}

// MARK: the window

struct RetroRootView: View {
    @EnvironmentObject var library: LibraryStore
    @StateObject private var state = RetroState()
    @ObservedObject private var sessions = NativeSessions.shared
    @ObservedObject private var engine = NativeEngine.shared
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(spacing: 0) {
            RetroTitleBar()
            RetroMenuStrip(state: state)
            VStack(spacing: 10) {
                RetroBanner(playing: playingName)
                RetroTabBar(state: state)
                content
                    .frame(maxHeight: .infinity)
                RetroActivityBar(playing: playingName, started: sessions.running.values.first?.started)
                    .frame(height: 128)
            }
            .padding(12)
            RetroStatusBar(text: statusText, count: library.entries.count, right: engineText)
        }
        .background(Retro.desk)
        .overlay(Rectangle().strokeBorder(Color(rgb: 0x2A3FA8), lineWidth: 3))
        .background(WindowConfigurator())
        .ignoresSafeArea()
        .environmentObject(state)
        .fileImporter(isPresented: $state.showImporter, allowedContentTypes: [.item, .folder], allowsMultipleSelection: true) { r in
            if case .success(let urls) = r { library.add(urls: urls); ActivityLog.shared.log("Adding \(urls.count) item(s) to the library...") }
        }
        .onReceive(NotificationCenter.default.publisher(for: .mgrAddGames)) { _ in state.showImporter = true }
        .onReceive(NotificationCenter.default.publisher(for: .mgrRetroTab)) { n in
            if let t = n.object as? RetroTab { select(tab: t) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .mgrToggleGrid)) { _ in state.grid.toggle() }
        .sheet(item: $state.optionsFor) { entry in
            GameDetailView(entry: entry) { state.optionsFor = nil; play(entry) }
                .environmentObject(library)
        }
        .onAppear {
            _ = ControllerWatch.shared
            if ActivityLog.shared.lines.isEmpty {
                ActivityLog.shared.log("Welcome to Mike's Game Room.")
                ActivityLog.shared.log("Library loaded: \(library.entries.count) game\(library.entries.count == 1 ? "" : "s"). Keyboard ready.")
            }
            if state.selectedID == nil { state.selectedID = library.entries(for: .all).first?.id }
        }
    }

    @ViewBuilder private var content: some View {
        switch state.tab {
        case .games, .favorites, .recent:
            HStack(spacing: 10) {
                RetroShelf().frame(width: 300)
                RetroLibraryPanel(play: play)
                RetroLaunchPanel(play: play).frame(width: 470)
            }
        case .controllers:
            RetroControllersPanel()
        case .settings:
            RetroPanel(title: "Settings", symbol: "gearshape.fill") {
                SettingsView()
                    .environmentObject(library)
                    .padding(8)
                    .background(Retro.rowA)
                    .environment(\.colorScheme, .light)
                    .padding(12)
            }
        }
    }

    private func select(tab: RetroTab) {
        state.tab = tab
        switch tab {
        case .games: state.shelf = .all
        case .favorites: state.shelf = .favorites
        case .recent: state.shelf = .recent
        default: break
        }
    }

    private var playingName: String? {
        guard let tid = sessions.running.keys.first else { return nil }
        return library.entries.first { $0.info.titleIDString.caseInsensitiveCompare(tid) == .orderedSame }?.info.name ?? "a game"
    }

    private var statusText: String {
        if let n = playingName { return "Playing \(n)" }
        for (path, b) in engine.builds {
            if case .running(let step, _, let f) = b.phase {
                let name = library.entries.first { $0.info.path == path }?.info.name ?? "game"
                return "Preparing \(name): \(step)" + (f.map { " \(Int($0 * 100))%" } ?? "")
            }
        }
        if library.scanning { return "Scanning for games..." }
        return "Ready"
    }
    private var engineText: String {
        engine.checkTools().allSatisfy(\.ok) ? "Native engine ready" : "Local mode"
    }

    /// Play: the prepared game; otherwise its options, where it can be prepared.
    func play(_ entry: LibraryEntry) {
        if NativeEngine.shared.play(entry.info) {
            library.markPlayed(entry.info.path)
            ActivityLog.shared.log("Launching \(entry.info.name)...")
        } else {
            ActivityLog.shared.log("\(entry.info.name) is not prepared for the Mac yet: open Game Options to prepare it.")
            state.optionsFor = entry
        }
    }
}

// MARK: title bar and menus

/// A 90s title bar: drag to move, double-click to zoom; minimise, maximise and close buttons.
struct RetroTitleBar: View {
    var body: some View {
        HStack(spacing: 10) {
            Image(nsImage: Brand.appIcon).resizable().frame(width: 26, height: 26)
            Text("Mike's Game Room").font(Retro.font(20, .bold)).foregroundStyle(.white)
                .shadow(color: .black.opacity(0.5), radius: 0, x: 1, y: 1)
            Spacer()
            HStack(spacing: 4) {
                chrome("minus") { NSApp.keyWindow?.miniaturize(nil) }
                chrome("square") { NSApp.keyWindow?.zoom(nil) }
                chrome("xmark", close: true) { NSApp.keyWindow?.performClose(nil) }
            }
        }
        .padding(.horizontal, 10)
        .frame(height: 40)
        .background(LinearGradient(colors: [Color(rgb: 0x0B2AA8), Color(rgb: 0x1F4FE0), Color(rgb: 0x0B2AA8)], startPoint: .leading, endPoint: .trailing))
        .background(WindowDragArea())
    }

    private func chrome(_ symbol: String, close: Bool = false, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 14, weight: .black))
                .foregroundStyle(close ? Color.white : Retro.ink)
                .frame(width: 32, height: 26)
                .background(close ? LinearGradient(colors: [Color(rgb: 0xF06A5A), Color(rgb: 0xC81E10)], startPoint: .top, endPoint: .bottom)
                                  : LinearGradient(colors: [Retro.silverTop, Retro.silverBottom], startPoint: .top, endPoint: .bottom))
                .bevel()
                .overlay(Rectangle().strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
        }
        .buttonStyle(.plain)
    }
}

/// The menu strip under the title bar (the same commands are in the Mac's menu bar).
struct RetroMenuStrip: View {
    @ObservedObject var state: RetroState
    @EnvironmentObject var library: LibraryStore

    private func menuLabel(_ t: String) -> some View {
        Text(t).font(Retro.font(17, .medium)).foregroundStyle(Retro.ink).padding(.horizontal, 10)
    }

    var body: some View {
        HStack(spacing: 4) {
            Menu {
                Button("Add Games...") { state.showImporter = true }
                Divider()
                Button("Quit Mike's Game Room") { NSApp.terminate(nil) }
            } label: { menuLabel("File") }.fixedSize()
            Menu {
                Button("Refresh Library") { library.refreshAll(); ActivityLog.shared.log("Refreshing the library...") }
                Button("Rescan Game Folders") { library.rescanFolders() }
                Divider()
                ForEach([ShelfNode.all, .favorites, .recent], id: \.self) { n in
                    Button(n.title) { state.tab = .games; state.shelf = n }
                }
            } label: { menuLabel("Library") }.fixedSize()
            Menu {
                Button(state.grid ? "List View" : "Icon View") { state.grid.toggle() }
                Divider()
                ForEach(RetroTab.allCases) { t in Button(t.rawValue) { NotificationCenter.default.post(name: .mgrRetroTab, object: t) } }
            } label: { menuLabel("View") }.fixedSize()
            Menu {
                Button("Game Options...") {
                    if let id = state.selectedID, let e = library.entries.first(where: { $0.id == id }) { state.optionsFor = e }
                }
                Button("Set Cover Art...") {
                    if let id = state.selectedID { pickCoverArt(library, path: id) }
                }
                Button("Settings") { NotificationCenter.default.post(name: .mgrRetroTab, object: RetroTab.settings) }
            } label: { menuLabel("Options") }.fixedSize()
            Menu {
                Button("Controls Guide") { NotificationCenter.default.post(name: .mgrRetroTab, object: RetroTab.controllers) }
            } label: { menuLabel("Help") }.fixedSize()
            Spacer()
        }
        .menuStyle(.button)
        .buttonStyle(.plain)
        .menuIndicator(.hidden)
        .padding(.horizontal, 14)
        .frame(height: 32)
        .background(Color(rgb: 0xE4E4E4))
        .overlay(alignment: .bottom) { Rectangle().fill(Color(rgb: 0x8A8A8A)).frame(height: 1) }
    }
}

// MARK: banner

/// Pixel-art scenery with the logo on the left and the player's welcome box on the right.
struct RetroBanner: View {
    let playing: String?

    var body: some View {
        ZStack {
            PixelLandscape()
            HStack(spacing: 0) {
                RetroLogo().padding(.leading, 24)
                Spacer()
                welcome.padding(.trailing, 16)
            }
        }
        .frame(height: 150)
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Retro.frame, lineWidth: 3))
    }

    private var welcome: some View {
        HStack(spacing: 14) {
            PixelSprite.avatar
                .frame(width: 76, height: 76)
                .padding(6)
                .background(Color(rgb: 0x1A2A7A))
                .overlay(Rectangle().strokeBorder(Color.white.opacity(0.8), lineWidth: 2))
            VStack(alignment: .leading, spacing: 6) {
                Text("Welcome back, \(firstName)!").font(Retro.font(24, .bold)).foregroundStyle(.white)
                HStack(spacing: 8) {
                    Circle().fill(Retro.led).frame(width: 16, height: 16)
                        .overlay(Circle().strokeBorder(.black.opacity(0.4), lineWidth: 1))
                        .shadow(color: Retro.led, radius: 4)
                    Text(playing.map { "Playing \($0)" } ?? "Ready to play")
                        .font(Retro.font(19, .medium)).foregroundStyle(.white).lineLimit(1)
                }
            }
            Rectangle().fill(.white.opacity(0.6)).frame(width: 2, height: 70)
            Image(systemName: "gamecontroller.fill").font(.system(size: 40)).foregroundStyle(Retro.goldTop)
                .shadow(color: .black, radius: 0, x: 2, y: 2)
        }
        .padding(12)
        .frame(maxWidth: 560)
        .background(Color(rgb: 0x0B1A62).opacity(0.94))
        .clipShape(RoundedRectangle(cornerRadius: 8))
        .overlay(RoundedRectangle(cornerRadius: 8).strokeBorder(Color(rgb: 0x9DB0FF), lineWidth: 3))
    }

    private var firstName: String {
        let n = NSFullUserName().split(separator: " ").first.map(String.init) ?? "Mike"
        return n.isEmpty ? "Mike" : n
    }
}

/// The logo: the app's controller icon with "MIKE'S GAME ROOM" in chunky outlined letters.
struct RetroLogo: View {
    var body: some View {
        HStack(spacing: -6) {
            Image(nsImage: Brand.appIcon).resizable().frame(width: 112, height: 112)
                .shadow(color: .black.opacity(0.6), radius: 0, x: 3, y: 3)
            VStack(alignment: .leading, spacing: -10) {
                outlined("MIKE'S", size: 54, colors: [Color(rgb: 0xFFF16A), Color(rgb: 0xFF8A00)])
                outlined("GAME ROOM", size: 42, colors: [Color(rgb: 0x9FF3FF), Color(rgb: 0x1E90FF)])
                    .padding(.leading, 18)
            }
            .rotationEffect(.degrees(-4))
        }
    }

    private func outlined(_ s: String, size: CGFloat, colors: [Color]) -> some View {
        let f = Font.system(size: size, weight: .black, design: .rounded).italic()
        return ZStack {
            // a thick dark outline from offset copies, then the gradient face
            ForEach(0..<16, id: \.self) { i in
                let a = Double(i) / 16 * 2 * .pi
                Text(s).font(f).foregroundStyle(Color(rgb: 0x0B1550)).offset(x: cos(a) * 4, y: sin(a) * 4)
            }
            Text(s).font(f).foregroundStyle(Color(rgb: 0x0B1550)).offset(x: 3, y: 6)
            Text(s).font(f).foregroundStyle(LinearGradient(colors: colors, startPoint: .top, endPoint: .bottom))
        }
        .fixedSize()
    }
}

/// Pixel scenery: sky, clouds, purple mountains with snow, green hills and a castle.
struct PixelLandscape: View {
    var body: some View {
        Canvas { ctx, size in
            let px: CGFloat = 5
            let w = size.width, h = size.height
            // sky in bands
            let sky: [UInt32] = [0x2D5FD9, 0x3A6EE3, 0x4C7FEA, 0x6293F0, 0x7BA8F5, 0x98BEF9]
            for (i, c) in sky.enumerated() {
                let y = h * CGFloat(i) / CGFloat(sky.count)
                ctx.fill(Path(CGRect(x: 0, y: y, width: w, height: h / CGFloat(sky.count) + 1)), with: .color(Color(rgb: c)))
            }
            // clouds
            func cloud(_ cx: CGFloat, _ cy: CGFloat, _ s: CGFloat) {
                let rows: [(CGFloat, CGFloat)] = [(2, 4), (0, 8), (-1, 10), (0, 9)]
                for (j, r) in rows.enumerated() {
                    ctx.fill(Path(CGRect(x: cx + r.0 * px * s, y: cy + CGFloat(j) * px * s, width: r.1 * px * s, height: px * s)), with: .color(.white))
                }
                ctx.fill(Path(CGRect(x: cx, y: cy + 3 * px * s, width: 9 * px * s, height: px * s)), with: .color(Color(rgb: 0xCFE0FF)))
            }
            cloud(w * 0.05, 18, 1); cloud(w * 0.42, 10, 1.2); cloud(w * 0.58, 30, 0.8); cloud(w * 0.92, 14, 1)
            // mountains: two ranges of stepped peaks
            func range(_ base: CGFloat, _ amp: CGFloat, _ freq: Double, _ phase: Double, _ color: UInt32, _ snow: Bool) {
                var x: CGFloat = 0
                while x < w {
                    let t = Double(x / w)
                    var m = 0.5 + 0.5 * sin(t * freq * .pi * 2 + phase)
                    m = pow(m, 2.2) + 0.25 * (0.5 + 0.5 * sin(t * freq * 5.3 + phase * 2))
                    let top = ((base - amp * CGFloat(m)) / px).rounded() * px
                    ctx.fill(Path(CGRect(x: x, y: top, width: px, height: h - top)), with: .color(Color(rgb: color)))
                    if snow && m > 0.85 {
                        ctx.fill(Path(CGRect(x: x, y: top, width: px, height: px * 3)), with: .color(Color(rgb: 0xF2F4FF)))
                    }
                    // a lighter left face for a little relief
                    ctx.fill(Path(CGRect(x: x, y: top + px * 3, width: px * 0.5, height: px * 4)), with: .color(.white.opacity(0.08)))
                    x += px
                }
            }
            range(h * 0.85, h * 0.55, 3.0, 0.7, 0x5A4C9E, true)
            range(h * 0.95, h * 0.42, 4.4, 2.1, 0x6E3F7E, false)
            // hills
            var x: CGFloat = 0
            while x < w {
                let t = Double(x / w)
                let m = 0.5 + 0.5 * sin(t * 9 + 1.3)
                let top = ((h * 0.80 - CGFloat(m) * h * 0.12) / px).rounded() * px
                ctx.fill(Path(CGRect(x: x, y: top, width: px, height: h - top)), with: .color(Color(rgb: 0x2E9A3A)))
                ctx.fill(Path(CGRect(x: x, y: top, width: px, height: px)), with: .color(Color(rgb: 0x6FD35A)))
                if Int(x / px) % 7 == 0 {
                    ctx.fill(Path(CGRect(x: x, y: top - px * 2, width: px * 2, height: px * 2)), with: .color(Color(rgb: 0x1F7A2C)))
                }
                x += px
            }
            // castle, placed between the logo and the welcome box
            let cx = (((440 + max(460, w - 600)) / 2 - 8 * px) / px).rounded() * px, gy = (h * 0.72 / px).rounded() * px
            let stone = Color(rgb: 0xC9B48A), shade = Color(rgb: 0x8C7650), dark = Color(rgb: 0x3B2A1A)
            func block(_ bx: CGFloat, _ by: CGFloat, _ bw: CGFloat, _ bh: CGFloat, _ c: Color) {
                ctx.fill(Path(CGRect(x: cx + bx * px, y: gy - by * px, width: bw * px, height: bh * px)), with: .color(c))
            }
            block(0, 10, 16, 12, stone); block(13, 10, 3, 12, shade)                  // keep
            block(-4, 16, 5, 18, stone); block(15, 16, 5, 18, stone)                   // towers
            block(5, 20, 6, 10, stone); block(9, 20, 2, 10, shade)                     // central tower
            for i in stride(from: -4, to: 20, by: 2) { block(CGFloat(i), i < 1 || i > 14 ? 18 : 12, 1, 2, stone) }
            block(6, 22, 1, 2, stone); block(8, 22, 1, 2, stone); block(10, 22, 1, 2, stone)
            block(6, 6, 4, 6, dark)                                                    // gate
            block(-3, 12, 1, 2, dark); block(17, 12, 1, 2, dark); block(7, 16, 2, 2, dark)
            block(8, 27, 1, 5, Color(rgb: 0x444444)); block(9, 27, 3, 2, Color(rgb: 0xE02A2A)) // flag
        }
    }
}

// MARK: tabs

struct RetroTabBar: View {
    @ObservedObject var state: RetroState
    var body: some View {
        HStack(alignment: .bottom, spacing: 6) {
            ForEach(RetroTab.allCases) { t in
                let on = state.tab == t
                Button { NotificationCenter.default.post(name: .mgrRetroTab, object: t) } label: {
                    Text(t.rawValue.uppercased())
                        .font(Retro.font(21))
                        .foregroundStyle(Retro.ink)
                        .frame(maxWidth: .infinity)
                        .frame(height: on ? 50 : 44)
                        .background(
                            UnevenRoundedRectangle(topLeadingRadius: 10, topTrailingRadius: 10)
                                .fill(LinearGradient(colors: on ? [Retro.goldTop, Retro.goldBottom] : [Retro.silverTop, Retro.silverBottom],
                                                     startPoint: .top, endPoint: .bottom))
                        )
                        .overlay(UnevenRoundedRectangle(topLeadingRadius: 10, topTrailingRadius: 10)
                            .strokeBorder(on ? Color(rgb: 0xFFF4C0) : .white, lineWidth: 2))
                        .overlay(UnevenRoundedRectangle(topLeadingRadius: 10, topTrailingRadius: 10)
                            .strokeBorder(Color.black.opacity(0.55), lineWidth: 1))
                }
                .buttonStyle(.plain)
            }
        }
        .padding(.horizontal, 30)
        .frame(height: 52, alignment: .bottom)
        .overlay(alignment: .bottom) {
            Rectangle().fill(LinearGradient(colors: [Retro.goldBottom, Color(rgb: 0xB35A00)], startPoint: .top, endPoint: .bottom)).frame(height: 4)
        }
    }
}

// MARK: game shelf

struct RetroShelf: View {
    @EnvironmentObject var library: LibraryStore
    @EnvironmentObject var state: RetroState
    @ObservedObject private var controllers = ControllerWatch.shared
    @State private var open: Set<ShelfNode> = []

    var body: some View {
        VStack(spacing: 10) {
            RetroPanel(title: "Game Shelf", symbol: "books.vertical.fill") {
                ScrollView {
                    VStack(alignment: .leading, spacing: 4) {
                        node(.all, depth: 0, expander: nil)
                        node(.favorites, depth: 1, expander: nil)
                        node(.recent, depth: 1, expander: nil)
                        ForEach(groups, id: \.self) { g in
                            node(g, depth: 0, expander: open.contains(g))
                            if open.contains(g) {
                                ForEach(library.entries(for: g)) { e in gameLeaf(e) }
                            }
                        }
                    }
                    .padding(10)
                }
            }
            player
        }
    }

    private var groups: [ShelfNode] {
        var g: [ShelfNode] = Platform.allCases.filter { p in library.entries.contains { $0.info.platform == p } }.map { .platform($0) }
        if library.entries.contains(where: { !$0.info.bootable }) { g.append(.attention) }
        return g
    }

    private func node(_ n: ShelfNode, depth: Int, expander: Bool?) -> some View {
        let on = state.shelf == n
        return HStack(spacing: 8) {
            if let expander {
                Text(expander ? "−" : "+").font(.system(size: 14, weight: .bold, design: .monospaced))
                    .foregroundStyle(Retro.ink)
                    .frame(width: 16, height: 16).background(Color.white).overlay(Rectangle().strokeBorder(.gray, lineWidth: 1))
                    .onTapGesture { if open.contains(n) { open.remove(n) } else { open.insert(n) } }
            } else {
                Color.clear.frame(width: 16, height: 16)
            }
            ZStack(alignment: .bottomTrailing) {
                Image(systemName: "folder.fill").font(.system(size: 26)).foregroundStyle(Color(rgb: 0xF7C531))
                    .shadow(color: .black.opacity(0.5), radius: 0, x: 1, y: 1)
                if n != .all {
                    Image(systemName: n.symbol).font(.system(size: 12, weight: .bold)).foregroundStyle(n.badgeColor)
                        .padding(2).background(Circle().fill(.white)).offset(x: 3, y: 2)
                }
            }
            Text(n.title).font(Retro.font(19, .bold)).foregroundStyle(.white).lineLimit(1)
                .padding(.horizontal, 6).padding(.vertical, 3)
                .background(on ? Retro.select : .clear)
                .overlay(on ? Rectangle().strokeBorder(.white.opacity(0.7), style: StrokeStyle(lineWidth: 1, dash: [2, 2])) : nil)
            Spacer(minLength: 0)
            Text("\(library.entries(for: n).count)").font(Retro.font(13, .bold)).foregroundStyle(.white.opacity(0.6))
        }
        .padding(.leading, CGFloat(depth) * 26)
        .contentShape(Rectangle())
        .onTapGesture {
            state.shelf = n
            state.tab = n == .favorites ? .favorites : n == .recent ? .recent : .games
            if let first = library.entries(for: n).first, !library.entries(for: n).contains(where: { $0.id == state.selectedID }) {
                state.selectedID = first.id
            }
        }
    }

    private func gameLeaf(_ e: LibraryEntry) -> some View {
        HStack(spacing: 6) {
            EntryArt(entry: e, corner: 3).frame(width: 20, height: 20)
            Text(e.info.name).font(Retro.font(15, .semibold)).foregroundStyle(state.selectedID == e.id ? Retro.goldTop : .white.opacity(0.9)).lineLimit(1)
        }
        .padding(.leading, 50)
        .contentShape(Rectangle())
        .onTapGesture { state.selectedID = e.id }
    }

    private var player: some View {
        let c = controllers.controllers.first
        return HStack(spacing: 14) {
            Image(systemName: "gamecontroller.fill").font(.system(size: 40)).foregroundStyle(Color(rgb: 0xE8E8E8))
                .shadow(color: .black, radius: 0, x: 2, y: 2)
            VStack(alignment: .leading, spacing: 4) {
                Text("PLAYER 1").font(Retro.font(22)).foregroundStyle(.white)
                HStack(spacing: 8) {
                    Circle().fill(c != nil ? Retro.led : Color.orange).frame(width: 15, height: 15)
                    Text(c.map { ($0.vendorName ?? "Controller").replacingOccurrences(of: " Wireless Controller", with: "") } ?? "Keyboard").font(Retro.font(17, .medium)).foregroundStyle(.white).lineLimit(1)
                }
            }
            Spacer()
        }
        .padding(14)
        .frame(height: 92)
        .background(Retro.panelDeep)
        .clipShape(RoundedRectangle(cornerRadius: 7))
        .overlay(RoundedRectangle(cornerRadius: 7).strokeBorder(Retro.frame, lineWidth: 3))
    }
}

// MARK: library list

struct RetroLibraryPanel: View {
    @EnvironmentObject var library: LibraryStore
    @EnvironmentObject var state: RetroState
    let play: (LibraryEntry) -> Void
    @State private var dropTargeted = false

    var body: some View {
        RetroPanel(title: state.shelf == .all ? "Your Library" : state.shelf.title, symbol: "externaldrive.fill", symbolColor: Color(rgb: 0xFFB13B)) {
            VStack(spacing: 10) {
                VStack(spacing: 0) {
                    if !state.grid { header }
                    ScrollView {
                        if state.grid { grid } else { list }
                    }
                    .background(Retro.rowA)
                }
                .bevel(raised: false)
                .overlay(dropTargeted ? Rectangle().strokeBorder(Retro.goldBottom, style: StrokeStyle(lineWidth: 4, dash: [10, 6])) : nil)
                .dropDestination(for: URL.self) { urls, _ in
                    library.add(urls: urls); ActivityLog.shared.log("Adding \(urls.count) dropped item(s)...")
                    return true
                } isTargeted: { dropTargeted = $0 }
                HStack(spacing: 12) {
                    Button { state.showImporter = true } label: { Label("Add Games", systemImage: "plus") }
                    Button { library.refreshAll(); ActivityLog.shared.log("Refreshing the library...") } label: { Label("Refresh", systemImage: "arrow.clockwise") }
                    Button { state.grid.toggle() } label: {
                        Label(state.grid ? "List View" : "Icon View", systemImage: state.grid ? "list.bullet" : "square.grid.2x2")
                    }
                }
                .buttonStyle(RetroButtonStyle(size: 17))
                .labelStyle(.titleAndIcon)
            }
            .padding(10)
        }
    }

    private var games: [LibraryEntry] { library.entries(for: state.shelf) }

    private var header: some View {
        HStack(spacing: 0) {
            Text("Game").padding(.leading, 12).frame(maxWidth: .infinity, alignment: .leading)
            Rectangle().fill(Color.gray).frame(width: 1)
            Text("Last Played").padding(.horizontal, 12).frame(width: 170, alignment: .leading)
        }
        .font(Retro.font(17, .bold))
        .foregroundStyle(Retro.ink)
        .frame(height: 32)
        .background(LinearGradient(colors: [Retro.silverTop, Retro.silverBottom], startPoint: .top, endPoint: .bottom))
        .bevel()
    }

    @ViewBuilder private var list: some View {
        if games.isEmpty { empty } else {
            LazyVStack(spacing: 0) {
                ForEach(Array(games.enumerated()), id: \.element.id) { i, e in
                    let on = state.selectedID == e.id
                    HStack(spacing: 0) {
                        HStack(spacing: 12) {
                            EntryArt(entry: e, corner: 4).frame(width: 42, height: 42)
                                .overlay(RoundedRectangle(cornerRadius: 4).strokeBorder(.black.opacity(0.4), lineWidth: 1))
                            Text(e.info.name).font(Retro.font(19, .bold)).lineLimit(1)
                            if e.favorite == true { Image(systemName: "star.fill").foregroundStyle(on ? .yellow : .orange) }
                            Spacer(minLength: 0)
                        }
                        .padding(.leading, 10)
                        Rectangle().fill(Color.gray.opacity(0.5)).frame(width: 1)
                        Text(lastPlayedText(e.lastPlayed)).font(Retro.font(18, .medium)).padding(.horizontal, 12).frame(width: 170, alignment: .leading)
                    }
                    .foregroundStyle(on ? .white : Retro.ink)
                    .frame(height: 52)
                    .background(on ? Retro.select : (i % 2 == 0 ? Retro.rowA : Retro.rowB))
                    .overlay(alignment: .bottom) { Rectangle().fill(Color.gray.opacity(0.35)).frame(height: 1) }
                    .contentShape(Rectangle())
                    .onTapGesture(count: 2) { play(e) }
                    .onTapGesture { state.selectedID = e.id }
                    .contextMenu { menu(e) }
                }
            }
        }
    }

    @ViewBuilder private var grid: some View {
        if games.isEmpty { empty } else {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 120), spacing: 14)], spacing: 14) {
                ForEach(games) { e in
                    let on = state.selectedID == e.id
                    VStack(spacing: 6) {
                        EntryArt(entry: e, corner: 6).frame(width: 96, height: 96)
                            .overlay(RoundedRectangle(cornerRadius: 6).strokeBorder(on ? Retro.select : .clear, lineWidth: 4))
                        Text(e.info.name).font(Retro.font(13, .bold)).multilineTextAlignment(.center).lineLimit(2)
                            .foregroundStyle(on ? .white : Retro.ink)
                            .padding(.horizontal, 4)
                            .background(on ? Retro.select : .clear)
                    }
                    .contentShape(Rectangle())
                    .onTapGesture(count: 2) { play(e) }
                    .onTapGesture { state.selectedID = e.id }
                    .contextMenu { menu(e) }
                }
            }
            .padding(14)
        }
    }

    private var empty: some View {
        VStack(spacing: 10) {
            Image(systemName: state.shelf == .favorites ? "star" : "tray").font(.system(size: 40)).foregroundStyle(.gray)
            Text(state.shelf == .favorites ? "No favorites yet. Right-click a game to add it."
                 : state.shelf == .recent ? "Nothing played yet."
                 : "Drag games here, or click Add Games.")
                .font(Retro.font(16, .semibold)).foregroundStyle(.gray).multilineTextAlignment(.center)
        }
        .frame(maxWidth: .infinity).padding(.vertical, 60)
    }

    @ViewBuilder private func menu(_ e: LibraryEntry) -> some View {
        Button("Play") { play(e) }
        Button(e.favorite == true ? "Remove from Favorites" : "Add to Favorites") { library.toggleFavorite(e.info.path) }
        Button("Game Options...") { state.optionsFor = e }
        Button("Set Cover Art...") { pickCoverArt(library, path: e.info.path) }
        if e.coverArt != nil { Button("Use the Game's Own Art") { library.setCoverArt(e.info.path, from: nil) } }
        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: e.info.path)]) }
        Divider()
        Button("Remove from Library", role: .destructive) { library.remove(e) }
    }
}

// MARK: game art

/// Asks for a picture and uses it as the game's cover art.
@MainActor func pickCoverArt(_ library: LibraryStore, path: String) {
    let panel = NSOpenPanel()
    panel.title = "Choose cover art"
    panel.allowedContentTypes = [.image]
    panel.allowsMultipleSelection = false
    if panel.runModal() == .OK, let url = panel.url {
        library.setCoverArt(path, from: url)
        let name = library.entries.first { $0.info.path == path }?.info.name ?? "the game"
        ActivityLog.shared.log("New cover art for \(name).")
    }
}

func entryImage(_ e: LibraryEntry) -> NSImage? {
    if let p = e.coverArt, let img = NSImage(contentsOfFile: p) { return img }
    if let d = e.info.icon { return NSImage(data: d) }
    return nil
}

/// A game's small picture: its cover art or icon, else lettered tile.
struct EntryArt: View {
    let entry: LibraryEntry
    var corner: CGFloat = 4
    var body: some View {
        if let img = entryImage(entry) {
            Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                .aspectRatio(1, contentMode: .fit)
                .clipShape(RoundedRectangle(cornerRadius: corner))
        } else {
            GameArt(info: entry.info, corner: corner)
        }
    }
}

// MARK: launch panel

struct RetroLaunchPanel: View {
    @EnvironmentObject var library: LibraryStore
    @EnvironmentObject var state: RetroState
    @ObservedObject private var sessions = NativeSessions.shared
    @ObservedObject private var engine = NativeEngine.shared
    let play: (LibraryEntry) -> Void

    var body: some View {
        RetroPanel(title: "Ready to Launch", symbol: "flame.fill", symbolColor: .orange) {
            if let e = selected {
                VStack(spacing: 0) {
                    GameHeroArt(entry: e)
                        .frame(maxWidth: .infinity, maxHeight: .infinity)
                        .clipped()
                        .overlay(Rectangle().strokeBorder(Color.white.opacity(0.85), lineWidth: 3))
                        .padding([.horizontal, .top], 8)
                    VStack(alignment: .leading, spacing: 4) {
                        Text(e.info.name).font(Retro.font(28)).foregroundStyle(.white).lineLimit(2).minimumScaleFactor(0.6)
                        Text(subtitle(e)).font(Retro.font(18, .medium)).foregroundStyle(.white.opacity(0.9))
                    }
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .padding(.horizontal, 16).padding(.vertical, 10)
                    actions(e).padding(.horizontal, 16).padding(.bottom, 14)
                }
            } else {
                Text("Pick a game from your library.").font(Retro.font(18, .semibold)).foregroundStyle(.white.opacity(0.7))
                    .frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
    }

    private var selected: LibraryEntry? {
        library.entries.first { $0.id == state.selectedID } ?? library.entries(for: state.shelf).first
    }

    private func subtitle(_ e: LibraryEntry) -> String {
        let played = e.lastPlayed.map { "Last played " + lastPlayedText($0).lowercased() } ?? "Not played yet"
        return "\(e.info.platform.rawValue) · \(played)"
    }

    @ViewBuilder private func actions(_ e: LibraryEntry) -> some View {
        let app = engine.nativeApp(e.info)
        let running = app.map { sessions.isRunning($0) } ?? false
        VStack(spacing: 10) {
            if running, let app {
                HStack(spacing: 10) {
                    Button { sessions.start(app) } label: { Label("Show Game", systemImage: "macwindow") }
                    Button { sessions.stop(app); ActivityLog.shared.log("Quitting \(e.info.name)...") } label: { Label("Quit Game", systemImage: "stop.fill") }
                }
                .buttonStyle(RetroButtonStyle(size: 18))
            } else {
                Button { play(e) } label: {
                    HStack(spacing: 16) {
                        PixelSprite.rocket.frame(width: 48, height: 48)
                        Text(app != nil ? "LET'S PLAY!" : "PREPARE GAME")
                            .font(.system(size: 40, weight: .black).italic())
                            .foregroundStyle(Retro.ink)
                            .shadow(color: Color(rgb: 0xFFE9A8), radius: 0, x: 0, y: 2)
                            .minimumScaleFactor(0.6).lineLimit(1)
                    }
                    .padding(.horizontal, 14)
                }
                .buttonStyle(LaunchButtonStyle())
                .disabled(preparing(e))
            }
            if preparing(e), case .running(let step, _, let f)? = engine.builds[e.info.path]?.phase {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Preparing: \(step)" + (f.map { " \(Int($0 * 100))%" } ?? "")).font(Retro.font(14, .bold)).foregroundStyle(.white)
                    RetroProgressBar(fraction: f ?? 0)
                }
            }
            Button("Game Options") { state.optionsFor = e }
                .buttonStyle(RetroButtonStyle(size: 19))
                .frame(maxWidth: 260)
        }
    }

    private func preparing(_ e: LibraryEntry) -> Bool {
        if case .running? = engine.builds[e.info.path]?.phase { return true }
        return false
    }
}

/// The selected game's art (its cover art or icon), framed over a blurred, enlarged copy of itself.
struct GameHeroArt: View {
    let entry: LibraryEntry
    @EnvironmentObject var library: LibraryStore
    var body: some View {
        GeometryReader { g in
            ZStack {
                if let img = entryImage(entry) {
                    Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
                        .frame(width: g.size.width, height: g.size.height)
                        .blur(radius: 22).saturation(1.4)
                        .overlay(LinearGradient(colors: [.clear, .black.opacity(0.35)], startPoint: .top, endPoint: .bottom))
                    let aspect = img.size.height > 0 ? img.size.width / img.size.height : 1
                    Image(nsImage: img).resizable().aspectRatio(aspect, contentMode: .fit)
                        .frame(maxWidth: g.size.width * 0.92, maxHeight: g.size.height * 0.86)
                        .clipShape(RoundedRectangle(cornerRadius: 10))
                        .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.white, lineWidth: 4))
                        .shadow(color: .black.opacity(0.6), radius: 0, x: 5, y: 5)
                } else {
                    PixelLandscape()
                    VStack(spacing: 10) {
                        GameArt(info: entry.info, corner: 10).frame(width: g.size.height * 0.5, height: g.size.height * 0.5)
                            .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(.white, lineWidth: 4))
                            .shadow(color: .black.opacity(0.6), radius: 0, x: 4, y: 4)
                        Button("Set Cover Art...") { pickCoverArt(library, path: entry.info.path) }
                            .buttonStyle(RetroButtonStyle(size: 15))
                    }
                }
            }
            .frame(width: g.size.width, height: g.size.height)
        }
    }
}

/// A chunky 90s progress bar.
struct RetroProgressBar: View {
    let fraction: Double
    var body: some View {
        GeometryReader { g in
            HStack(spacing: 2) {
                let n = max(1, Int(g.size.width / 12))
                ForEach(0..<n, id: \.self) { i in
                    Rectangle().fill(Double(i) / Double(n) < fraction ? Retro.led : .clear).frame(width: 10)
                }
            }
            .padding(3)
        }
        .frame(height: 20)
        .background(Color.white)
        .bevel(raised: false)
    }
}

// MARK: activity and status

struct RetroActivityBar: View {
    let playing: String?
    let started: Date?
    @ObservedObject private var log = ActivityLog.shared

    var body: some View {
        RetroPanel(title: "Room Activity", symbol: "text.bubble.fill", symbolColor: .white) {
            HStack(spacing: 0) {
                ScrollViewReader { proxy in
                    ScrollView {
                        VStack(alignment: .leading, spacing: 2) {
                            ForEach(Array(log.lines.enumerated()), id: \.offset) { i, l in
                                Text("> " + l).id(i)
                            }
                        }
                        .font(.system(size: 17, design: .monospaced))
                        .foregroundStyle(Color(rgb: 0xE8F0FF))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .padding(10)
                        .textSelection(.enabled)
                    }
                    .onChange(of: log.lines.count) { n in withAnimation { proxy.scrollTo(n - 1, anchor: .bottom) } }
                }
                .background(Retro.terminal)
                Rectangle().fill(Retro.frame).frame(width: 3)
                nowPlaying.frame(width: 360)
            }
        }
    }

    private var nowPlaying: some View {
        HStack(spacing: 14) {
            Image(systemName: playing != nil ? "play.tv.fill" : "moon.zzz.fill").font(.system(size: 32))
                .foregroundStyle(.white)
            VStack(alignment: .leading, spacing: 4) {
                Text("Now Playing").font(Retro.font(18, .bold)).foregroundStyle(.white)
                if let playing, let started {
                    Text(playing).font(Retro.font(15, .semibold)).foregroundStyle(Retro.goldTop).lineLimit(1)
                    Text(started, style: .timer).font(.system(size: 15, weight: .bold, design: .monospaced)).foregroundStyle(Retro.led)
                } else {
                    Text("Nothing running").font(Retro.font(15, .medium)).foregroundStyle(.white.opacity(0.7))
                }
            }
            Spacer()
        }
        .padding(.horizontal, 16)
    }
}

struct RetroStatusBar: View {
    let text: String
    let count: Int
    let right: String
    var body: some View {
        HStack(spacing: 0) {
            HStack(spacing: 10) {
                Circle().fill(Retro.led).frame(width: 20, height: 20).overlay(Circle().strokeBorder(.black.opacity(0.4), lineWidth: 1))
                Text(text).lineLimit(1)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            Text("\(count) game\(count == 1 ? "" : "s") in library").frame(maxWidth: .infinity)
            Rectangle().fill(Color.gray).frame(width: 1, height: 22)
            Text(right).frame(width: 240, alignment: .trailing)
        }
        .font(Retro.font(18, .medium))
        .foregroundStyle(Retro.ink)
        .padding(.horizontal, 16)
        .frame(height: 38)
        .background(Color(rgb: 0xD9D9D9))
        .bevel(raised: false)
    }
}

// MARK: controllers tab

struct RetroControllersPanel: View {
    @ObservedObject private var watch = ControllerWatch.shared

    private let keys: [(String, String)] = [
        ("Left stick", "W A S D"), ("Right stick (camera)", "Shift + arrow keys"), ("D-pad", "Arrow keys"),
        ("A / B / X / Y", "J / K / U / I"), ("L / R", "Q / E"), ("ZL / ZR", "Z / C"), ("Stick clicks", "F / G"),
        ("+ / −", "Return / Tab"), ("Shake", "Space"), ("Full screen", "Ctrl + Cmd + F"), ("Quit", "Cmd + Q"),
    ]

    var body: some View {
        HStack(spacing: 10) {
            RetroPanel(title: "Connected Controllers", symbol: "gamecontroller.fill") {
                ScrollView {
                    VStack(alignment: .leading, spacing: 10) {
                        if watch.controllers.isEmpty {
                            Text("No controller connected. Pair one in System Settings › Bluetooth (a PS5 DualSense, Xbox or Switch Pro controller), or play with the keyboard.")
                                .font(Retro.font(17, .medium)).foregroundStyle(.white.opacity(0.85)).fixedSize(horizontal: false, vertical: true)
                        }
                        ForEach(Array(watch.controllers.enumerated()), id: \.offset) { i, c in
                            HStack(spacing: 14) {
                                Image(systemName: "gamecontroller.fill").font(.system(size: 34)).foregroundStyle(.white)
                                VStack(alignment: .leading, spacing: 3) {
                                    Text("PLAYER \(i + 1)").font(Retro.font(20)).foregroundStyle(Retro.goldTop)
                                    Text(c.vendorName ?? "Controller").font(Retro.font(18, .semibold)).foregroundStyle(.white)
                                    if let b = c.battery {
                                        Text("Battery \(Int(b.batteryLevel * 100))%" + (b.batteryState == .charging ? " · charging" : ""))
                                            .font(Retro.font(14, .medium)).foregroundStyle(.white.opacity(0.75))
                                    }
                                }
                                Spacer()
                                Circle().fill(Retro.led).frame(width: 16, height: 16)
                            }
                            .padding(12)
                            .background(Retro.panelDeep).overlay(Rectangle().strokeBorder(Retro.frame, lineWidth: 2))
                        }
                        Text("Controllers are matched by button position: on a PS5 controller, Circle is A, Cross is B, Triangle is X and Square is Y.")
                            .font(Retro.font(15, .medium)).foregroundStyle(.white.opacity(0.75)).fixedSize(horizontal: false, vertical: true)
                    }
                    .padding(14)
                }
            }
            RetroPanel(title: "Keyboard Controls", symbol: "keyboard.fill") {
                ScrollView {
                    VStack(spacing: 0) {
                        ForEach(Array(keys.enumerated()), id: \.offset) { i, k in
                            HStack {
                                Text(k.0).font(Retro.font(18, .bold))
                                Spacer()
                                Text(k.1).font(.system(size: 17, weight: .bold, design: .monospaced))
                            }
                            .foregroundStyle(Retro.ink)
                            .padding(.horizontal, 14).frame(height: 40)
                            .background(i % 2 == 0 ? Retro.rowA : Retro.rowB)
                        }
                    }
                    .bevel(raised: false)
                    .padding(14)
                }
            }
        }
    }
}

// MARK: window plumbing

/// Hides the system title bar buttons (the retro title bar has its own) and keeps the window a sensible size.
struct WindowConfigurator: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { ConfigView() }
    func updateNSView(_ v: NSView, context: Context) {}
    final class ConfigView: NSView {
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            guard let w = window else { return }
            for b in [NSWindow.ButtonType.closeButton, .miniaturizeButton, .zoomButton] { w.standardWindowButton(b)?.isHidden = true }
            w.titlebarAppearsTransparent = true
            w.titleVisibility = .hidden
            w.isMovableByWindowBackground = false
            w.backgroundColor = NSColor(red: 0.04, green: 0.10, blue: 0.36, alpha: 1)
        }
    }
}

/// Lets the retro title bar move the window (and zoom it on a double-click).
struct WindowDragArea: NSViewRepresentable {
    func makeNSView(context: Context) -> NSView { DragView() }
    func updateNSView(_ v: NSView, context: Context) {}
    final class DragView: NSView {
        override func mouseDown(with event: NSEvent) {
            if event.clickCount == 2 { window?.zoom(nil); return }
            window?.performDrag(with: event)
        }
    }
}

extension Notification.Name {
    static let mgrRetroTab = Notification.Name("mgrRetroTab")
    static let mgrToggleGrid = Notification.Name("mgrToggleGrid")
}
