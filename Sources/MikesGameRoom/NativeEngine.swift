import Foundation
import SwiftUI
import AppKit

/// The native engine: nxnative (https://github.com/seedds/nxnative, MIT licence), imported into
/// Engines/nxnative. A game is prepared once (its code rewritten for the Mac, its shaders translated
/// and compiled for Metal, packed into an app beside the game file) and then runs at full speed.
/// Preparing uses the Mac fully for a while (about two hours the first time for a big Switch 2 game).
@MainActor
final class NativeEngine: ObservableObject {
    static let shared = NativeEngine()

    enum BuildPhase: Equatable {
        case running(step: String, detail: String, fraction: Double?)
        case finished
        case failed(String)
    }
    struct BuildState: Equatable {
        var phase: BuildPhase
        var started: Date
        var logURL: URL
    }

    @Published private(set) var builds: [String: BuildState] = [:]   // by game path
    @Published var enginePath: String { didSet { UserDefaults.standard.set(enginePath, forKey: "native.enginePath") } }
    @Published var toolsPath: String { didSet { UserDefaults.standard.set(toolsPath, forKey: "native.toolsPath") } }
    @Published var xcodePath: String { didSet { UserDefaults.standard.set(xcodePath, forKey: "native.xcodePath") } }

    private var processes: [String: Process] = [:]
    private var adopted: [String: pid_t] = [:]          // preparations started by an earlier launch of the app
    private var watchers: [String: Task<Void, Never>] = [:]

    private init() {
        let d = UserDefaults.standard
        enginePath = d.string(forKey: "native.enginePath") ?? HubPaths.defaultEngine
        toolsPath = d.string(forKey: "native.toolsPath") ?? HubPaths.defaultTools
        xcodePath = d.string(forKey: "native.xcodePath") ?? HubPaths.defaultXcode
    }


    // MARK: what is there

    /// The game has a tested profile in the engine (others get a provisional build that may not start).
    func hasProfile(_ info: GameInfo) -> Bool {
        FileManager.default.fileExists(atPath: enginePath + "/dm/games/" + info.titleIDString)
    }
    func supports(_ info: GameInfo) -> Bool { info.platform == .switch2 || info.platform == .switch1 }

    /// The folder the engine writes the app into: the game file's folder, or the parent of a game folder.
    private func outputFolder(_ info: GameInfo) -> URL {
        let u = URL(fileURLWithPath: info.path)
        return u.deletingLastPathComponent()
    }

    /// The prepared app for this game, if there is one: the app the last preparation reported ("done: <path>"),
    /// else an app beside the game or, for a game folder, inside it, whose host binary exists and whose Info.plist
    /// names this title (DMTitleID, written by the engine).
    func preparedApp(_ info: GameInfo) -> URL? {
        let fm = FileManager.default
        func matches(_ app: URL) -> Bool {
            guard fm.fileExists(atPath: app.appendingPathComponent("Contents/MacOS/dmhost").path),
                  let plist = NSDictionary(contentsOf: app.appendingPathComponent("Contents/Info.plist")),
                  let tid = plist["DMTitleID"] as? String else { return false }
            return tid.caseInsensitiveCompare(info.titleIDString) == .orderedSame
        }
        if let p = UserDefaults.standard.string(forKey: "native.app." + info.titleIDString) {
            let u = URL(fileURLWithPath: p)
            if matches(u) { return u }
        }
        var dirs = [outputFolder(info)]
        var isDir: ObjCBool = false
        if fm.fileExists(atPath: info.path, isDirectory: &isDir), isDir.boolValue { dirs.insert(URL(fileURLWithPath: info.path), at: 0) }
        for dir in dirs {
            guard let items = try? fm.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil) else { continue }
            for app in items where app.pathExtension == "app" && matches(app) { return app }
        }
        return nil
    }

    struct ToolCheck: Identifiable { let id: String; let ok: Bool; let detail: String }
    func checkTools() -> [ToolCheck] {
        let fm = FileManager.default
        func exists(_ p: String) -> Bool { fm.fileExists(atPath: p) }
        return [
            ToolCheck(id: "Engine source", ok: exists(enginePath + "/scripts/build_ipa_from_nsp.py"), detail: enginePath),
            ToolCheck(id: "Xcode 27.2", ok: exists(xcodePath + "/Contents/Developer/usr/bin/xcodebuild"), detail: xcodePath),
            ToolCheck(id: "Python tools", ok: exists(toolsPath + "/venv/bin/python3"), detail: toolsPath + "/venv"),
            ToolCheck(id: ".NET", ok: exists(toolsPath + "/dotnet/dotnet"), detail: toolsPath + "/dotnet"),
            ToolCheck(id: "Homebrew tools", ok: exists("/opt/homebrew/bin/zstd") && exists("/opt/homebrew/bin/glslangValidator"), detail: "zstd, glslang"),
            ToolCheck(id: "ZBIC decoder", ok: exists(zbicTool()), detail: zbicTool()),
        ]
    }

    /// MikesGameRoom's own ZBIC decoder (Sources/mgrzbic), used by the engine instead of the GPLv2 zbic package.
    func zbicTool() -> String { HubPaths.zbicTool(enginePath: enginePath) }

    // MARK: preparing a game

    var logsDir: URL {
        let d = LibraryStore.supportDir.appendingPathComponent("native-builds", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    /// The engine's command line for a game: the script, the game (or, for packages, their folder) and the keys.
    private func buildArguments(_ info: GameInfo) -> Result<[String], NativeMessage> {
        var args = [enginePath + "/scripts/build_ipa_from_nsp.py", info.path, "--platform", "mac-app"]
        // encrypted formats (NSP, XCI and their compressed forms) need the player's own prod.keys, imported in Settings › Keys
        let ext = URL(fileURLWithPath: info.path).pathExtension.lowercased()
        if ["nsp", "xci", "nsz", "xcz"].contains(ext) {
            // the engine takes the folder of one game's packages: the base game with its update (and DLC) beside it
            args[1] = URL(fileURLWithPath: info.path).deletingLastPathComponent().path
            let keys = LibraryStore.keysDir.appendingPathComponent("prod.keys")
            guard FileManager.default.fileExists(atPath: keys.path) else {
                return .failure(NativeMessage(text: "This game is encrypted: import your prod.keys in Settings › Keys first."))
            }
            args += ["-k", keys.path]
        }
        return .success(args)
    }

    /// A preparation of this game that is already running on the Mac, including one started before the app restarted.
    nonisolated static func runningBuildPID(target: String) -> pid_t? {
        let ps = Process()
        ps.executableURL = URL(fileURLWithPath: "/bin/ps")
        ps.arguments = ["-Ao", "pid=,args="]
        let out = Pipe()
        ps.standardOutput = out
        ps.standardError = FileHandle.nullDevice
        guard (try? ps.run()) != nil else { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        ps.waitUntilExit()
        let needle = "build_ipa_from_nsp.py " + target + " --platform"
        for line in String(decoding: data, as: UTF8.self).split(separator: "\n") where line.contains(needle) {
            let t = line.trimmingCharacters(in: .whitespaces)
            if let pid = Int32(t.prefix { $0.isNumber }), pid > 0 { return pid }
        }
        return nil
    }

    /// Picks up a preparation of this game that is already running (from an earlier launch of the app), if there is one.
    @discardableResult
    func adoptIfRunning(_ info: GameInfo) -> Bool {
        guard processes[info.path] == nil else { return true }
        if let pid = adopted[info.path], kill(pid, 0) == 0 { return true }
        guard case .success(let args) = buildArguments(info), let pid = Self.runningBuildPID(target: args[1]) else { return false }
        let log = logsDir.appendingPathComponent(info.titleIDString + ".log")
        adopted[info.path] = pid
        builds[info.path] = BuildState(phase: .running(step: "Already preparing", detail: "continuing the preparation that was already running", fraction: nil), started: Date(), logURL: log)
        ActivityLog.shared.log("\(info.name) is already being prepared: following that preparation instead of starting another")
        watch(info.path, log: log, tid: info.titleIDString, pid: pid)
        return true
    }

    func prepare(_ info: GameInfo) {
        guard processes[info.path] == nil else { return }
        // never two preparations of one game: they share the work folder and shader cache and spoil each other's results
        if adoptIfRunning(info) { return }
        let args: [String]
        switch buildArguments(info) {
        case .success(let a): args = a
        case .failure(let m):
            builds[info.path] = BuildState(phase: .failed(m.text), started: Date(), logURL: logsDir.appendingPathComponent(info.titleIDString + ".log"))
            return
        }
        // room on the drive first (a preparation that runs out of space wastes hours)
        if let problem = StorageCenter.shared.spaceProblem(for: info) {
            builds[info.path] = BuildState(phase: .failed(problem), started: Date(), logURL: logsDir.appendingPathComponent(info.titleIDString + ".log"))
            ActivityLog.shared.log(problem)
            return
        }
        let log = logsDir.appendingPathComponent(info.titleIDString + ".log")
        FileManager.default.createFile(atPath: log.path, contents: nil)
        let p = Process()
        p.executableURL = URL(fileURLWithPath: toolsPath + "/venv/bin/python3")
        p.arguments = args
        p.currentDirectoryURL = URL(fileURLWithPath: enginePath)
        var env = ProcessInfo.processInfo.environment
        env["PATH"] = [toolsPath + "/venv/bin", toolsPath + "/dotnet", toolsPath + "/bin", "/opt/homebrew/bin", "/usr/bin", "/bin", "/usr/sbin", "/sbin"].joined(separator: ":")
        env["DOTNET_ROOT"] = toolsPath + "/dotnet"
        env["DEVELOPER_DIR"] = xcodePath + "/Contents/Developer"
        env["MGR_ZBIC"] = zbicTool()
        env["PYTHONUNBUFFERED"] = "1"
        p.environment = env
        // The engine writes straight into the log file (not through the app), so a preparation keeps its log and can
        // be followed again if the app quits or restarts while it runs. The app reads its progress from the log.
        let handle = try? FileHandle(forWritingTo: log)
        if let handle { p.standardOutput = handle; p.standardError = handle }
        let path = info.path
        builds[path] = BuildState(phase: .running(step: "Starting", detail: "", fraction: nil), started: Date(), logURL: log)
        ActivityLog.shared.log("Preparing \(info.name) for the Mac...")
        p.terminationHandler = { [weak self] proc in
            try? handle?.close()
            let code = proc.terminationStatus
            Task { @MainActor in
                guard let self else { return }
                self.processes[path] = nil
                self.finishBuild(path, log: log, ok: code == 0, code: code)
            }
        }
        do {
            try p.run()
            processes[path] = p
            watch(path, log: log, tid: info.titleIDString, pid: p.processIdentifier)
        } catch {
            try? handle?.close()
            builds[path]?.phase = .failed("Could not start the build: \(error.localizedDescription)")
        }
    }

    /// Follows a preparation's progress in its log until it ends.
    private func watch(_ path: String, log: URL, tid: String, pid: pid_t) {
        watchers[path]?.cancel()
        watchers[path] = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                guard let self else { return }
                let lines = Self.tailLines(log)
                if let done = lines.last(where: { $0.contains("] done: ") && $0.contains(".app") }),
                   let r = done.range(of: "done: "), let end = done.range(of: ".app", range: r.upperBound..<done.endIndex) {
                    UserDefaults.standard.set(String(done[r.upperBound..<end.upperBound]), forKey: "native.app." + tid)
                }
                if let last = lines.last(where: { $0.contains("[") && $0.contains("/") }) { self.progress(path, line: last) }
                if kill(pid, 0) != 0 {
                    // an adopted preparation has no exit code: the log's done line tells how it ended
                    if self.adopted[path] != nil {
                        self.adopted[path] = nil
                        // one started by an earlier launch wrote its output to that app, not the log: then the app it built tells
                        let started = self.builds[path]?.started ?? Date()
                        let ok = lines.contains { $0.contains("] done: ") && $0.contains(".app") } || Self.appBuilt(log: log, since: started)
                        self.finishBuild(path, log: log, ok: ok, code: ok ? 0 : 1)
                    }
                    self.watchers[path] = nil
                    return
                }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
        }
    }

    private func finishBuild(_ path: String, log: URL, ok: Bool, code: Int32) {
        guard case .running = builds[path]?.phase ?? .finished else { return }
        if ok { builds[path]?.phase = .finished; ActivityLog.shared.log("Game prepared: ready to play.") }
        else {
            builds[path]?.phase = .failed(lastError(log) ?? "The build stopped (code \(code)). See the log.")
            ActivityLog.shared.log("Preparing stopped (code \(code)). See Game Options for the log.")
        }
    }

    /// Whether the app the log names ("…; app: /path/Name.app") was written after the date.
    nonisolated static func appBuilt(log: URL, since: Date) -> Bool {
        guard let text = try? String(contentsOf: log, encoding: .utf8),
              let r = text.range(of: "; app: "), let end = text.range(of: ".app", range: r.upperBound..<text.endIndex) else { return false }
        let app = String(text[r.upperBound..<end.upperBound])
        guard let m = (try? FileManager.default.attributesOfItem(atPath: app))?[.modificationDate] as? Date else { return false }
        return m > since
    }

    /// The last lines of a log (progress lines end in \n or \r: the engine redraws its status line).
    nonisolated static func tailLines(_ log: URL, bytes: Int = 16_384) -> [String] {
        guard let h = try? FileHandle(forReadingFrom: log) else { return [] }
        defer { try? h.close() }
        let size = (try? h.seekToEnd()) ?? 0
        try? h.seek(toOffset: size > UInt64(bytes) ? size - UInt64(bytes) : 0)
        let data = (try? h.readToEnd()) ?? Data()
        return String(decoding: data, as: UTF8.self).split(whereSeparator: { $0 == "\n" || $0 == "\r" }).map(String.init)
    }

    func runningBuildPIDs() -> [pid_t] {
        processes.values.filter { $0.isRunning }.map { $0.processIdentifier } + adopted.values.filter { kill($0, 0) == 0 }
    }

    func cancel(_ info: GameInfo) {
        if let p = processes[info.path] { p.interrupt() }
        else if let pid = adopted[info.path] { kill(pid, SIGINT) }
    }

    /// "[10:26:56] [5/7] Mac shaders · compiling: 4%, 1,839/42,848 shaders, … · ETA 1:04:32" -> step, detail, fraction
    private func progress(_ path: String, line: String) {
        // only while it runs: the log's closing lines must not turn a finished preparation back into a running one
        guard case .running? = builds[path]?.phase else { return }
        var s = line
        if s.hasPrefix("["), let r = s.firstIndex(of: "]") { s = String(s[s.index(after: r)...]).trimmingCharacters(in: .whitespaces) }   // the time
        if s.hasPrefix("✓") { s = String(s.dropFirst()).trimmingCharacters(in: .whitespaces) }
        var step = s, detail = ""
        if let dot = s.range(of: " · ") { step = String(s[..<dot.lowerBound]); detail = String(s[dot.upperBound...]) }
        var fraction: Double? = nil
        if let m = detail.range(of: #"(\d+)%"#, options: .regularExpression), let v = Double(detail[m].dropLast()) { fraction = v / 100 }
        builds[path]?.phase = .running(step: step, detail: detail, fraction: fraction)
    }

    private func lastError(_ log: URL) -> String? {
        guard let text = try? String(contentsOf: log) else { return nil }
        let lines = text.split(whereSeparator: { $0 == "\n" || $0 == "\r" }).map(String.init)
        return lines.last(where: { $0.localizedCaseInsensitiveContains("error") || $0.localizedCaseInsensitiveContains("failed") }) ?? lines.last
    }

    // MARK: playing

    /// The prepared app, read for running (title ID, preferences domain, file places).
    func nativeApp(_ info: GameInfo) -> NativeApp? { preparedApp(info).flatMap(NativeApp.init) }

    /// Starts the prepared game from MikesGameRoom (no separate launcher window); false when it is not prepared.
    func play(_ info: GameInfo) -> Bool {
        guard let app = nativeApp(info) else { return false }
        NativeSessions.shared.start(app)
        return true
    }
}

/// Splits a byte stream into lines (ended by \n or \r), for the build's output reader thread.
final class LineBuffer: @unchecked Sendable {
    private var pending = ""
    private let lock = NSLock()
    func append(_ data: Data) -> [String] {
        lock.lock(); defer { lock.unlock() }
        pending += String(decoding: data, as: UTF8.self)
        var lines: [String] = []
        while let r = pending.firstIndex(where: { $0 == "\n" || $0 == "\r" }) {
            lines.append(String(pending[..<r])); pending = String(pending[pending.index(after: r)...])
        }
        return lines
    }
}

/// The native engine's part of a game's details: profile, prepared app, prepare / play buttons, progress.
struct NativeEnginePanel: View {
    let info: GameInfo
    @ObservedObject var engine = NativeEngine.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            Label("Native engine (nxnative)", systemImage: "bolt.fill").font(.headline)
            if !engine.supports(info) {
                Text(info.note.localizedCaseInsensitiveContains("encrypted")
                     ? "Encrypted game: import your own prod.keys (dumped from your Switch) in Settings › Keys, and it can be prepared."
                     : "Not available for this format.").foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            } else {
                Text(engine.hasProfile(info)
                     ? "Tested profile for this game."
                     : "No tested profile: a provisional build is tried, which may not start.")
                    .font(.callout).foregroundStyle(.secondary)
                switch engine.builds[info.path]?.phase {
                case .running(let step, let detail, let fraction):
                    if let fraction { ProgressView(value: fraction) } else { ProgressView().controlSize(.small) }
                    Text(step).font(.callout.bold())
                    if !detail.isEmpty { Text(detail).font(.caption).foregroundStyle(.secondary).lineLimit(2) }
                    Button("Stop Preparing", role: .destructive) { engine.cancel(info) }
                case .failed(let msg):
                    Text(msg).font(.caption).foregroundStyle(.red).textSelection(.enabled)
                    prepareButtons
                default:
                    prepareButtons
                }
            }
        }
    }

    @ViewBuilder private var prepareButtons: some View {
        if let app = engine.nativeApp(info) {
            NativeRunControls(app: app, name: info.name)
            DisclosureGroup("Game Settings") {
                NativeSettingsView(app: app).padding(.top, 4)
            }
            HStack {
                Button("Prepare Again") { engine.prepare(info) }
                if let log = engine.builds[info.path]?.logURL { Button("Preparation Log") { NSWorkspace.shared.open(log) } }
            }
            .controlSize(.small)
        } else {
            HStack {
                Button { engine.prepare(info) } label: { Label("Prepare for Mac", systemImage: "hammer.fill") }.controlSize(.large)
                Text("Uses the whole Mac for a while: about two hours the first time for a big game.")
                    .font(.caption).foregroundStyle(.secondary)
                if let log = engine.builds[info.path]?.logURL { Button("Show Log") { NSWorkspace.shared.open(log) } }
            }
        }
    }
}

/// Settings tab: where the engine and its tools are, and whether they are all there.
struct NativeEngineSettings: View {
    @ObservedObject var engine = NativeEngine.shared
    @State private var checks: [NativeEngine.ToolCheck] = []
    @State private var showSetup = false

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Native engine").font(.title3.bold())
            Text("Games run natively with nxnative (MIT licence), imported into MikesGameRoom. Preparing a game needs Xcode 27.2, Python, .NET and a few Homebrew tools.")
                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            LabeledContent("Engine") { TextField("", text: $engine.enginePath) }
            LabeledContent("Tools") { TextField("", text: $engine.toolsPath) }
            LabeledContent("Xcode") { TextField("", text: $engine.xcodePath) }
            ForEach(checks) { c in
                HStack {
                    Image(systemName: c.ok ? "checkmark.circle.fill" : "xmark.circle").foregroundStyle(c.ok ? Color.green : Color.orange)
                    Text(c.id).bold()
                    Text(c.detail).font(.caption).foregroundStyle(.secondary).lineLimit(1).truncationMode(.middle)
                }
            }
            HStack {
                Button("Check Again") { checks = engine.checkTools() }
                Button("Run Setup…") { showSetup = true }
            }
            Spacer()
        }
        .onAppear { checks = engine.checkTools() }
        .sheet(isPresented: $showSetup, onDismiss: { checks = engine.checkTools() }) { EngineSetupView { showSetup = false } }
    }
}
