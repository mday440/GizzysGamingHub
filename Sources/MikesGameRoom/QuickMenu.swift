import SwiftUI
import AppKit
import GameController

// The in-game quick menu. During a game, press both sticks (L3 + R3) or ⌃⌘G: the game pauses (the hub stops its
// processes with SIGSTOP, so it neither runs nor reads the controller) and this menu shows on top of it.
// Resume, keep a save point, save the last 30 s as a clip, restart the game, or quit to the hub.

@MainActor
final class QuickMenu: ObservableObject {
    static let shared = QuickMenu()
    static var enabled: Bool { UserDefaults.standard.object(forKey: "quickmenu.enabled") as? Bool ?? true }

    enum Item: Int, CaseIterable, Identifiable {
        case resume, savePoint, clip, restart, quit
        var id: Int { rawValue }
        var title: String {
            switch self {
            case .resume: return "Resume"
            case .savePoint: return "Keep a save point"
            case .clip: return "Save the last 30 seconds"
            case .restart: return "Restart game"
            case .quit: return "Quit to Gizzy's Gaming Hub"
            }
        }
        var symbol: String {
            switch self {
            case .resume: return "play.fill"
            case .savePoint: return "clock.arrow.circlepath"
            case .clip: return "record.circle"
            case .restart: return "arrow.counterclockwise"
            case .quit: return "house.fill"
            }
        }
    }

    @Published private(set) var isOpen = false
    @Published var selection: Item = .resume
    @Published private(set) var note: String?
    @Published private(set) var gameName = ""
    @Published private(set) var sessionText = ""
    @Published private(set) var batteryText = ""

    private var panel: NSPanel?
    private var paused: [pid_t] = []

    private var session: NativeSessions.Session? { NativeSessions.shared.running.values.first }

    func toggle() { isOpen ? close() : show() }

    func show() {
        guard Self.enabled, !isOpen, let s = session else { return }
        // pause the game: the launcher and everything below it (the host)
        let root = s.process.processIdentifier
        paused = [root] + ProcessTree.descendants(of: root)
        for p in paused.reversed() { kill(p, SIGSTOP) }

        gameName = LibraryStore.shared?.entries.first { $0.info.titleIDString.caseInsensitiveCompare(s.app.titleID) == .orderedSame }?.info.name ?? "Your game"
        sessionText = "Playing for " + formatMinutes(Date().timeIntervalSince(s.started) / 60)
        if let b = GCController.controllers().first(where: { $0.extendedGamepad != nil })?.battery {
            batteryText = "Controller \(Int((b.batteryLevel * 100).rounded()))%" + (b.batteryState == .charging ? " · charging" : "")
        } else { batteryText = "" }
        selection = .resume
        note = nil

        let screen = NSScreen.main ?? NSScreen.screens.first
        guard let screen else { resumeGame(); return }
        let p = NSPanel(contentRect: screen.frame, styleMask: [.borderless, .nonactivatingPanel], backing: .buffered, defer: false)
        p.level = .screenSaver
        p.collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary, .ignoresCycle]
        p.isOpaque = false
        p.backgroundColor = .clear
        p.hasShadow = false
        p.hidesOnDeactivate = false
        p.isReleasedWhenClosed = false
        p.contentView = NSHostingView(rootView: QuickMenuView())
        p.setFrame(screen.frame, display: true)
        p.orderFrontRegardless()
        panel = p
        isOpen = true
    }

    /// Closes the menu and lets the game run again.
    func close() {
        resumeGame()
        panel?.orderOut(nil); panel?.close(); panel = nil
        isOpen = false
    }

    private func resumeGame() {
        for p in paused { kill(p, SIGCONT) }
        paused = []
    }

    // controller (HubPad) while the menu is open
    func pad(_ a: PadAction) {
        let all = Item.allCases
        switch a {
        case .up: selection = all[(selection.rawValue - 1 + all.count) % all.count]
        case .down: selection = all[(selection.rawValue + 1) % all.count]
        case .accept: activate(selection)
        case .back: close()
        default: break
        }
    }

    func activate(_ item: Item) {
        guard let s = session else { close(); return }
        switch item {
        case .resume:
            close()
        case .savePoint:
            note = "Keeping a save point…"
            SaveVault.shared.snapshot(s.app, label: "Save point from the quick menu", minutes: nil, force: true) { snap in
                Task { @MainActor in QuickMenu.shared.note = snap != nil ? "Save point kept. Find it under Saves." : "There's no save to keep yet." }
            }
        case .clip:
            if ReplayRecorder.shared.recording {
                ReplayRecorder.shared.saveClip()
                note = "Saving the last 30 seconds to Replays…"
            } else {
                note = "Instant replay is off. Turn it on in Settings › Instant replay."
            }
        case .restart:
            let app = s.app
            close()
            ActivityLog.shared.log("Restarting \(gameName)")
            NativeSessions.shared.stop(app)
            Task { @MainActor in
                for _ in 0..<100 {   // up to 20 s for it to close
                    if !NativeSessions.shared.isRunning(app) { break }
                    try? await Task.sleep(nanoseconds: 200_000_000)
                }
                try? await Task.sleep(nanoseconds: 500_000_000)
                NativeSessions.shared.start(app)
            }
        case .quit:
            let app = s.app
            close()
            NativeSessions.shared.stop(app)
        }
    }
}

struct QuickMenuView: View {
    @ObservedObject private var menu = QuickMenu.shared

    var body: some View {
        GeometryReader { geo in
            let vh = geo.size.height / 100
            ZStack {
                Color.black.opacity(0.62)
                RadialGradient(colors: [Color(rgb: 0x7C3AED).opacity(0.18), .clear], center: .center, startRadius: 0, endRadius: 60 * vh)
                VStack(alignment: .leading, spacing: 0) {
                    HStack(spacing: 12) {
                        if let url = Bundle.main.url(forResource: "GizzyLogo", withExtension: "png"), let logo = NSImage(contentsOf: url) {
                            Image(nsImage: logo).resizable().aspectRatio(contentMode: .fit).frame(height: 4.6 * vh)
                        }
                        Spacer()
                        Text("PAUSED").font(Orbit.display(1.3 * vh, 600)).tracking(0.4 * vh).foregroundStyle(Orbit.mint)
                    }
                    Text(menu.gameName).font(Orbit.display(3 * vh, 800)).lineLimit(2).minimumScaleFactor(0.6)
                        .padding(.top, 2.4 * vh)
                    Text([menu.sessionText, menu.batteryText].filter { !$0.isEmpty }.joined(separator: " · "))
                        .font(Orbit.body(1.6 * vh, 600)).foregroundStyle(Orbit.mute).padding(.top, 0.6 * vh)
                    VStack(spacing: 0.8 * vh) {
                        ForEach(QuickMenu.Item.allCases) { item in row(item, vh: vh) }
                    }
                    .padding(.top, 2.6 * vh)
                    Text(menu.note ?? " ").font(Orbit.body(1.5 * vh, 600)).foregroundStyle(Orbit.lavender)
                        .padding(.top, 1.6 * vh)
                    Text("▲▼ choose · ✕ / A select · ○ / B resume · L3 + R3 or ⌃⌘G closes")
                        .font(Orbit.body(1.3 * vh, 600)).foregroundStyle(Orbit.mute).padding(.top, 0.8 * vh)
                }
                .padding(4 * vh)
                .frame(width: min(62 * vh, geo.size.width * 0.8))
                .background(RoundedRectangle(cornerRadius: 26).fill(LinearGradient(colors: [Orbit.panel.opacity(0.97), Orbit.card.opacity(0.97)], startPoint: .top, endPoint: .bottom)))
                .overlay(RoundedRectangle(cornerRadius: 26).strokeBorder(Orbit.line, lineWidth: 1))
                .shadow(color: .black.opacity(0.6), radius: 50, y: 20)
            }
            .frame(width: geo.size.width, height: geo.size.height)
        }
        .ignoresSafeArea()
        .foregroundStyle(Orbit.text)
    }

    private func row(_ item: QuickMenu.Item, vh: CGFloat) -> some View {
        let on = menu.selection == item
        return Button { menu.selection = item; menu.activate(item) } label: {
            HStack(spacing: 1.6 * vh) {
                Image(systemName: item.symbol).font(.system(size: 2 * vh, weight: .semibold)).frame(width: 3 * vh)
                Text(item.title).font(Orbit.body(2 * vh, 700))
                Spacer()
                if on { Image(systemName: "chevron.right").font(.system(size: 1.6 * vh, weight: .bold)) }
            }
            .padding(.horizontal, 2 * vh).padding(.vertical, 1.5 * vh)
            .foregroundStyle(on ? Orbit.ink : Orbit.text)
            .background(RoundedRectangle(cornerRadius: 14).fill(on ? Orbit.lavender : Orbit.card.opacity(0.6)))
            .overlay(RoundedRectangle(cornerRadius: 14).strokeBorder(on ? .clear : Orbit.line, lineWidth: 1))
            .contentShape(RoundedRectangle(cornerRadius: 14))
        }
        .buttonStyle(.plain)
        .onHover { if $0 { menu.selection = item } }
    }
}

struct QuickMenuSettings: View {
    @AppStorage("quickmenu.enabled") private var on = true
    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Toggle("Quick menu during games", isOn: $on)
            Text("Press both sticks together (L3 + R3) or ⌃⌘G while playing. The game pauses while the menu is open: resume, keep a save point, save a replay clip, restart or quit to the hub.")
                .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
        }
        .font(Orbit.body(13))
    }
}
