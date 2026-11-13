import Foundation
import SwiftUI
import AppKit

// MikesGameRoom as an nxnative front end (Engines/nxnative/docs/frontends.md): it starts a prepared game
// itself (the app's Contents/MacOS/nxnative with DM_LAUNCHER=0, so no separate settings window), draws the
// game's own settings from the app's launcher.json and keeps the player's choices where nxnative reads them
// (launcher.<VAR> in the app's preferences domain), follows the running game, and looks after its log,
// save and cheats. The engine's host is built for each game, so every game runs in a process of its own.

/// One prepared game app and the places nxnative keeps its files.
struct NativeApp {
    let url: URL
    let titleID: String       // DMTitleID
    let bundleID: String      // CFBundleIdentifier: the preferences domain of the saved choices

    init?(_ url: URL) {
        guard let plist = NSDictionary(contentsOf: url.appendingPathComponent("Contents/Info.plist")),
              let tid = plist["DMTitleID"] as? String else { return nil }
        self.url = url
        titleID = tid.uppercased()
        bundleID = (plist["CFBundleIdentifier"] as? String) ?? "org.nxnative.\(tid.lowercased())"
    }

    var launcher: URL { url.appendingPathComponent("Contents/MacOS/nxnative") }
    var settingsFile: URL { url.appendingPathComponent("Contents/Resources/dm/launcher.json") }
    var dataDir: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Application Support/nxnative/\(titleID)", isDirectory: true)
    }
    var saveDir: URL { dataDir.appendingPathComponent("save", isDirectory: true) }
    var cheatsDir: URL { dataDir.appendingPathComponent("cheats", isDirectory: true) }
    var logFile: URL {
        FileManager.default.homeDirectoryForCurrentUser.appendingPathComponent("Library/Logs/nxnative/\(titleID).log")
    }
}

// MARK: the game's settings (launcher.json, schema 1)

struct NativeSetting: Decodable, Identifiable {
    let env: String
    let kind: String              // choice, toggle, range
    let section: String?
    let label: String
    let note: String?
    let defaultValue: String
    let level: String?            // "basic" rows are shown first
    let options: [[String]]?      // choice: [value, label]
    let on: String?
    let off: String?
    let min: Double?
    let max: Double?
    let step: Double?
    let needs: String?            // "VAR=value": only applies while that setting has that value
    var id: String { env }

    enum CodingKeys: String, CodingKey {
        case env, kind, section, label, note, level, options, on, off, min, max, step, needs
        case defaultValue = "default"
    }
}

struct NativeSettingsFile: Decodable {
    let schema: Int?
    let settings: [NativeSetting]
}

/// Reads and writes the player's choices for one game app, as nxnative's own launcher and settings screens do.
@MainActor
final class NativeSettingsModel: ObservableObject {
    let app: NativeApp
    let settings: [NativeSetting]
    @Published private(set) var values: [String: String] = [:]

    init(app: NativeApp) {
        self.app = app
        if let data = try? Data(contentsOf: app.settingsFile),
           let file = try? JSONDecoder().decode(NativeSettingsFile.self, from: data),
           (file.schema ?? 1) == 1 {
            settings = file.settings.filter { ["choice", "toggle", "range"].contains($0.kind) }
        } else {
            settings = []
        }
        for s in settings { values[s.env] = saved(s) ?? s.defaultValue }
    }

    var sections: [String] {
        var seen: [String] = []
        for s in settings { let n = s.section ?? "Game"; if !seen.contains(n) { seen.append(n) } }
        return seen
    }

    /// A saved value counts only while this build still offers it (as the launcher checks them).
    private func saved(_ s: NativeSetting) -> String? {
        guard let v = CFPreferencesCopyAppValue("launcher.\(s.env)" as CFString, app.bundleID as CFString) as? String else { return nil }
        return valid(s, v) ? v : nil
    }
    private func valid(_ s: NativeSetting, _ v: String) -> Bool {
        switch s.kind {
        case "choice": return s.options?.contains { $0.first == v } ?? false
        case "toggle": return v == s.on || v == s.off
        case "range":
            guard let d = Double(v) else { return false }
            return d >= (s.min ?? -.infinity) && d <= (s.max ?? .infinity)
        default: return false
        }
    }

    func value(_ s: NativeSetting) -> String { values[s.env] ?? s.defaultValue }

    func set(_ s: NativeSetting, _ v: String) {
        guard valid(s, v) else { return }
        values[s.env] = v
        CFPreferencesSetAppValue("launcher.\(s.env)" as CFString, v as CFString, app.bundleID as CFString)
        CFPreferencesAppSynchronize(app.bundleID as CFString)
    }

    func resetAll() {
        for s in settings {
            CFPreferencesSetAppValue("launcher.\(s.env)" as CFString, nil, app.bundleID as CFString)
            values[s.env] = s.defaultValue
        }
        CFPreferencesAppSynchronize(app.bundleID as CFString)
    }

    /// Whether a row's "needs" condition holds (its row is greyed out otherwise).
    func applies(_ s: NativeSetting) -> Bool {
        guard let n = s.needs, let eq = n.firstIndex(of: "=") else { return true }
        let k = String(n[..<eq]), v = String(n[n.index(after: eq)...])
        return values[k] == nil || values[k] == v
    }

    var changedCount: Int { settings.filter { value($0) != $0.defaultValue }.count }
}

// MARK: running a game

/// A message for the player, as an error (Result needs an Error type).
struct NativeMessage: Error { let text: String }

@MainActor
final class NativeSessions: ObservableObject {
    static let shared = NativeSessions()

    struct Session {
        let process: Process
        let started: Date
        let app: NativeApp
    }
    enum Outcome: Equatable {
        case quit
        case gpuLost
        case crashed(String)
        case failed(String)
    }

    @Published private(set) var running: [String: Session] = [:]   // by title ID
    @Published private(set) var lastOutcome: [String: Outcome] = [:]

    func isRunning(_ app: NativeApp) -> Bool { running[app.titleID] != nil }

    /// Starts the game: the app's own launcher, without its settings window. It reads the saved choices,
    /// sets the host's environment from the bundle and runs the host (dmhost) as its child, which opens the game's window.
    func start(_ app: NativeApp) {
        if let s = running[app.titleID] {
            NSRunningApplication(processIdentifier: s.process.processIdentifier)?.activate()
            return
        }
        lastOutcome[app.titleID] = nil
        let p = Process()
        p.executableURL = app.launcher
        p.arguments = ["--no-launcher"]
        var env = ProcessInfo.processInfo.environment
        env["DM_LAUNCHER"] = "0"
        // the game opens full screen (its loading and "Preparing pipelines" screens too), like the hub; the hub's
        // "Open the app in full screen" switch covers both
        env["DM_FULLSCREEN"] = IntroPrefs.fullScreen ? "1" : "0"
        p.environment = env
        p.currentDirectoryURL = app.url.deletingLastPathComponent()
        p.standardOutput = FileHandle.nullDevice
        p.standardError = FileHandle.nullDevice
        let tid = app.titleID
        p.terminationHandler = { proc in
            let code = proc.terminationStatus, reason = proc.terminationReason
            Task { @MainActor in NativeSessions.shared.ended(tid, code: code, signalled: reason == .uncaughtSignal) }
        }
        do {
            try p.run()
            running[tid] = Session(process: p, started: Date(), app: app)
            SessionHooks.started(app, process: p)
            PreShow.shared.begin(app)   // Starlight Cinema covers the loading
        } catch {
            lastOutcome[tid] = .failed("Could not start the game: \(error.localizedDescription)")
            ActivityLog.shared.log("Could not start the game: \(error.localizedDescription)")
        }
    }

    /// Asks the game to quit (the launcher passes SIGTERM on to the host, which closes as on ⌘Q).
    func stop(_ app: NativeApp) {
        running[app.titleID]?.process.terminate()
    }

    private func ended(_ tid: String, code: Int32, signalled: Bool) {
        let app = running[tid]?.app
        let started = running[tid]?.started
        running[tid] = nil
        // back to the hub (its own full-screen space), not the desktop
        NSApp.activate(ignoringOtherApps: true)
        NSApp.windows.first { $0.isVisible && $0.canBecomeMain }?.makeKeyAndOrderFront(nil)
        defer { if let app, let started { SessionHooks.ended(app, started: started, code: signalled ? 128 + code : code) } }
        // the launcher's exit codes (docs/frontends.md): 0 quit, 70 GPU access revoked, 134 abort
        switch (code, signalled) {
        case (0, false), (15, true): lastOutcome[tid] = .quit
            ActivityLog.shared.log("Game closed.")
        case (70, false): lastOutcome[tid] = .gpuLost
            ActivityLog.shared.log("The game stopped: macOS revoked its GPU access. Start it again.")
        default:
            let tail = app.flatMap { Self.logTail($0.logFile, lines: 3) } ?? ""
            lastOutcome[tid] = .crashed("The game stopped unexpectedly (code \(code))." + (tail.isEmpty ? "" : "\n" + tail))
            ActivityLog.shared.log("The game stopped unexpectedly (code \(code)). Its log is under Game Options.")
        }
    }

    static func logTail(_ url: URL, lines n: Int) -> String? {
        guard let text = try? String(contentsOf: url, encoding: .utf8) else { return nil }
        return text.split(separator: "\n").suffix(n).joined(separator: "\n")
    }

    // MARK: saves

    /// Where save backups go: on the work drive with MikesGameRoom's other data.
    static var backupsDir: URL {
        let d = HubPaths.dataRoot.appendingPathComponent("Save Backups", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    /// Zips the game's save folder into the backups folder; returns the zip, or an error message.
    func backUpSave(_ app: NativeApp, name: String) -> Result<URL, NativeMessage> {
        guard FileManager.default.fileExists(atPath: app.saveDir.path) else { return .failure(NativeMessage(text: "There is no save yet: play the game first.")) }
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let safe = name.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: " -")
        let zip = Self.backupsDir.appendingPathComponent("\(safe) \(f.string(from: Date())).zip")
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        p.arguments = ["-c", "-k", "--keepParent", app.saveDir.path, zip.path]
        do { try p.run() } catch { return .failure(NativeMessage(text: error.localizedDescription)) }
        p.waitUntilExit()
        return p.terminationStatus == 0 ? .success(zip) : .failure(NativeMessage(text: "The backup failed (ditto \(p.terminationStatus))."))
    }
}

// MARK: views

/// Running state, play / stop, the last run's outcome, and the log, save and cheats buttons.
struct NativeRunControls: View {
    let app: NativeApp
    let name: String
    @ObservedObject var sessions = NativeSessions.shared
    @State private var message: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            HStack {
                if let s = sessions.running[app.titleID] {
                    Image(systemName: "circle.fill").foregroundStyle(.green).font(.caption)
                    Text("Running").bold()
                    Text(s.started, style: .timer).monospacedDigit().foregroundStyle(.secondary)
                    Spacer()
                    Button("Show Game") { sessions.start(app) }
                    Button("Quit Game", role: .destructive) { sessions.stop(app) }
                } else {
                    Button { sessions.start(app) } label: { Label("Play", systemImage: "play.fill") }.controlSize(.large)
                }
            }
            switch sessions.lastOutcome[app.titleID] {
            case .gpuLost?:
                Text("macOS stopped the game's graphics (GPU access revoked). Start it again; if it keeps happening, restart the Mac.")
                    .font(.caption).foregroundStyle(.orange)
            case .crashed(let m)?, .failed(let m)?:
                Text(m).font(.caption).foregroundStyle(.red).textSelection(.enabled).lineLimit(5)
            default: EmptyView()
            }
            HStack {
                Button("Game Log") { NSWorkspace.shared.open(app.logFile) }
                    .disabled(!FileManager.default.fileExists(atPath: app.logFile.path))
                Button("Save Folder") { NSWorkspace.shared.open(app.saveDir) }
                    .disabled(!FileManager.default.fileExists(atPath: app.saveDir.path))
                Button("Back Up Save") {
                    switch sessions.backUpSave(app, name: name) {
                    case .success(let zip): message = "Saved \(zip.lastPathComponent)"; NSWorkspace.shared.activateFileViewerSelecting([zip])
                    case .failure(let m): message = m.text
                    }
                }
                Button("Cheats Folder") {
                    try? FileManager.default.createDirectory(at: app.cheatsDir, withIntermediateDirectories: true)
                    NSWorkspace.shared.open(app.cheatsDir)
                }
            }
            .controlSize(.small)
            if let message { Text(message).font(.caption).foregroundStyle(.secondary) }
        }
    }
}

/// The game's own settings, drawn from its launcher.json. Choices apply at the game's next start.
struct NativeSettingsView: View {
    @StateObject var model: NativeSettingsModel
    @State private var showAll = false

    init(app: NativeApp) { _model = StateObject(wrappedValue: NativeSettingsModel(app: app)) }

    var body: some View {
        if model.settings.isEmpty {
            Text("This build lists no settings.").font(.caption).foregroundStyle(.secondary)
        } else {
            VStack(alignment: .leading, spacing: 10) {
                ForEach(model.sections, id: \.self) { section in
                    let rows = model.settings.filter { ($0.section ?? "Game") == section && (showAll || $0.level == "basic") }
                    if !rows.isEmpty {
                        Text(section).font(.subheadline.bold()).foregroundStyle(.secondary)
                        ForEach(rows) { s in row(s) }
                    }
                }
                HStack {
                    Toggle("Show all settings", isOn: $showAll).toggleStyle(.switch).controlSize(.small)
                    Spacer()
                    if model.changedCount > 0 {
                        Button("Reset to Defaults") { model.resetAll() }.controlSize(.small)
                    }
                }
                Text("Changes apply the next time the game starts.").font(.caption).foregroundStyle(.secondary)
            }
        }
    }

    @ViewBuilder private func row(_ s: NativeSetting) -> some View {
        VStack(alignment: .leading, spacing: 2) {
            switch s.kind {
            case "choice":
                Picker(s.label, selection: Binding(get: { model.value(s) }, set: { model.set(s, $0) })) {
                    ForEach(s.options ?? [], id: \.self) { o in Text(o.count > 1 ? o[1] : o[0]).tag(o[0]) }
                }
                .pickerStyle(.menu)
            case "toggle":
                Toggle(s.label, isOn: Binding(get: { model.value(s) == s.on }, set: { model.set(s, ($0 ? s.on : s.off) ?? "") }))
            default:   // range
                let lo = s.min ?? 0, hi = s.max ?? 1, st = s.step ?? 0.1
                HStack {
                    Text(s.label)
                    Slider(value: Binding(get: { Double(model.value(s)) ?? lo },
                                          set: { model.set(s, Self.format(($0 / st).rounded() * st, step: st)) }),
                           in: lo...hi)
                    Text(model.value(s)).monospacedDigit().frame(width: 40, alignment: .trailing)
                }
            }
            if let note = s.note, !note.isEmpty {
                Text(note).font(.caption).foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            }
        }
        .disabled(!model.applies(s))
        .opacity(model.applies(s) ? 1 : 0.5)
    }

    private static func format(_ v: Double, step: Double) -> String {
        let digits = step >= 1 ? 0 : max(1, Int((-log10(step)).rounded(.up)))
        return String(format: "%.\(digits)f", v)
    }
}
