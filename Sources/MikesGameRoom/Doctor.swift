import Foundation
import SwiftUI
import AppKit
import Darwin

// Performance Doctor: after every session it reads the engine's log for that game and explains, in plain English,
// what slowed it down, with one-click fixes. Before a launch it checks free memory and offers to quit the heaviest apps.

/// Where the hub keeps its own data on the work drive.
enum GizzyData {
    static func dir(_ name: String) -> URL {
        let d = HubPaths.dataRoot.appendingPathComponent(name, isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }
}

// MARK: session hooks

/// One place where a game session's start and end fan out to the features that care.
@MainActor
enum SessionHooks {
    static func started(_ app: NativeApp, process: Process) {
        NativeEngine.shared.pauseBuilds(true)
        ReplayRecorder.shared.gameStarted(app, launcherPID: process.processIdentifier)
        let name = LibraryStore.shared?.entries.first { $0.info.titleIDString.caseInsensitiveCompare(app.titleID) == .orderedSame }?.info.name ?? "a game"
        DiscordPresence.shared.gameStarted(name)
    }

    static func ended(_ app: NativeApp, started: Date, code: Int32) {
        let minutes = Date().timeIntervalSince(started) / 60
        let library = LibraryStore.shared
        let entry = library?.entries.first { $0.info.titleIDString.caseInsensitiveCompare(app.titleID) == .orderedSame }
        let name = entry?.info.name ?? "the game"
        if let entry { library?.addPlaytime(entry.info.path, seconds: minutes * 60) }
        ReplayRecorder.shared.gameEnded()
        if QuickMenu.shared.isOpen { QuickMenu.shared.close() }
        DiscordPresence.shared.gameEnded()
        NativeEngine.shared.pauseBuilds(false)
        SaveVault.shared.snapshot(app, label: "After a \(formatMinutes(minutes)) session", minutes: minutes)
        PerformanceDoctor.shared.analyze(app, name: name, started: started, exitCode: code)
    }
}

func formatMinutes(_ m: Double) -> String {
    let total = Int(m.rounded())
    if total < 1 { return "under a minute" }
    if total < 60 { return "\(total) min" }
    return "\(total / 60) h \(total % 60) m"
}

// MARK: the report

struct DoctorFinding: Codable, Identifiable, Hashable {
    enum Kind: String, Codable { case memory, gpu, shaders, slowMotion, crash, smooth }
    enum Fix: String, Codable { case dynamicResolution, fps30, closeApps }
    var id: String { kind.rawValue }
    let kind: Kind
    let title: String
    let detail: String
    let fix: Fix?
}

struct DoctorReport: Codable, Hashable {
    let titleID: String
    let gameName: String
    let date: Date
    let minutes: Double
    let smoothPercent: Int?
    let peakFootprintMB: Int?
    let findings: [DoctorFinding]
}

@MainActor
final class PerformanceDoctor: ObservableObject {
    static let shared = PerformanceDoctor()
    @Published private(set) var reports: [String: DoctorReport] = [:]
    @Published var showing: DoctorReport?
    @Published var memoryCheckOn: Bool { didSet { UserDefaults.standard.set(memoryCheckOn, forKey: "doctor.memcheck") } }

    private var dir: URL { GizzyData.dir("Doctor") }

    private init() {
        memoryCheckOn = UserDefaults.standard.object(forKey: "doctor.memcheck") as? Bool ?? true
        let dec = JSONDecoder()
        for f in (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: nil)) ?? [] where f.pathExtension == "json" {
            if let d = try? Data(contentsOf: f), let r = try? dec.decode(DoctorReport.self, from: d) { reports[r.titleID] = r }
        }
    }

    func analyze(_ app: NativeApp, name: String, started: Date, exitCode: Int32) {
        let log = app.logFile, tid = app.titleID
        let physicalMB = Int(ProcessInfo.processInfo.physicalMemory / 1_048_576)
        Task.detached(priority: .utility) {
            guard let text = try? String(contentsOf: log, encoding: .utf8) else { return }
            let report = DoctorAnalysis.run(text, titleID: tid, name: name, started: started, exitCode: exitCode, physicalMB: physicalMB)
            await MainActor.run { PerformanceDoctor.shared.store(report) }
        }
    }

    /// Reads any game's last session that has no report yet (sessions from before the Doctor existed).
    func catchUp(_ entries: [LibraryEntry]) {
        for e in entries {
            guard reports[e.info.titleIDString] == nil, let app = NativeEngine.shared.nativeApp(e.info),
                  let mod = (try? FileManager.default.attributesOfItem(atPath: app.logFile.path))?[.modificationDate] as? Date else { continue }
            analyze(app, name: e.info.name, started: mod, exitCode: 0)
        }
    }

    private func store(_ r: DoctorReport) {
        reports[r.titleID] = r
        let enc = JSONEncoder(); enc.outputFormatting = .prettyPrinted
        if let d = try? enc.encode(r) { try? d.write(to: dir.appendingPathComponent(r.titleID + ".json"), options: .atomic) }
        let issues = r.findings.filter { $0.kind != .smooth }.count
        ActivityLog.shared.log(issues == 0 ? "Performance Doctor: \(r.gameName) ran smoothly"
                                           : "Performance Doctor: \(issues) suggestion\(issues == 1 ? "" : "s") for \(r.gameName)")
    }

    /// Applies a fix to the game's saved settings (the ones nxnative's launcher reads at the next start).
    func apply(_ fix: DoctorFinding.Fix, app: NativeApp) -> String {
        let m = NativeSettingsModel(app: app)
        func set(_ env: String, _ v: String) -> Bool {
            guard let s = m.settings.first(where: { $0.env == env }) else { return false }
            m.set(s, v); return true
        }
        switch fix {
        case .dynamicResolution:
            return set("DM_DYNAMIC_RESOLUTION", "1") ? "Dynamic resolution is on from the next start." : "This game has no Dynamic resolution setting."
        case .fps30:
            return set("DM_FPS_TARGET", "30") ? "The game runs at a steady 30 fps from the next start." : "This game has no frame rate setting."
        case .closeApps:
            memoryCheckOn = true
            return "Before each launch, the hub checks free memory and offers to quit the heaviest apps."
        }
    }
}

/// The log reading itself, kept apart from the UI so it runs off the main thread.
enum DoctorAnalysis {
    struct Window { var frame = 0, p50 = 0.0, submit = 0.0, gpu = 0.0, dropped = 0, frames = 0, footprint = 0, cpu = 0.0, wall = 0.0 }

    static func run(_ text: String, titleID: String, name: String, started: Date, exitCode: Int32, physicalMB: Int) -> DoctorReport {
        var windows: [Int: Window] = [:]
        var frameTimes: [(Int, Double)] = []     // frame → seconds, from longframe lines
        var hitches = 0, hitchMs = 0.0
        var slowRatios: [Double] = []

        func num(_ s: Substring, after key: String) -> Double? {
            guard let r = s.range(of: key) else { return nil }
            let rest = s[r.upperBound...].drop { $0 == " " }
            let n = rest.prefix { $0.isNumber || $0 == "." }
            return Double(n)
        }
        func frameNo(_ s: Substring, after key: String) -> Int? { num(s, after: key).map { Int($0) } }

        for line in text.split(separator: "\n") {
            if line.contains("[nvn] timing up to frame"), let f = frameNo(line, after: "timing up to frame") {
                var w = windows[f] ?? Window(); w.frame = f
                w.p50 = num(line, after: "frame time p50") ?? 0
                w.submit = num(line, after: "NVN submit+present p50") ?? 0
                w.gpu = num(line, after: "GPU p50") ?? 0
                windows[f] = w
            } else if line.contains("[nvn] on-screen up to frame"), let f = frameNo(line, after: "on-screen up to frame") {
                var w = windows[f] ?? Window(); w.frame = f
                w.frames = Int(num(line, after: "on-screen up to frame \(f):") ?? 300)
                w.dropped = Int(num(line, after: "dropped") ?? 0)
                w.footprint = Int(num(line, after: "footprint") ?? 0)
                windows[f] = w
            } else if line.contains("translation detail up to frame"), let f = frameNo(line, after: "translation detail up to frame") {
                var w = windows[f] ?? Window(); w.frame = f
                w.cpu = num(line, after: "CPU") ?? 0
                w.wall = num(line, after: "wall clock") ?? 0
                windows[f] = w
            } else if line.contains("[longframe] frame"), let f = frameNo(line, after: "[longframe] frame") {
                if let t = num(line, after: "frame \(f) (") { frameTimes.append((f, t)) }
                if let ms = num(line, after: "shader libraries"), let r = line.range(of: "shader libraries") {
                    // "... new pipelines N + shader libraries L, T ms in all"
                    let after = line[r.upperBound...]
                    if let comma = after.firstIndex(of: ","), let t = Double(after[after.index(after: comma)...].drop { $0 == " " }.prefix { $0.isNumber || $0 == "." }), t >= 100 {
                        hitches += 1; hitchMs += t
                    }
                    _ = ms
                }
            } else if line.contains("[nvn] frame step: game time"), let r = num(line, after: "s in"), let g = num(line, after: "game time"), r > 20 {
                slowRatios.append(g / r)
            }
        }

        let ws = windows.values.filter { $0.p50 > 0 }.sorted { $0.frame < $1.frame }
        func minuteAt(_ frame: Int) -> Double {
            if let near = frameTimes.min(by: { abs($0.0 - frame) < abs($1.0 - frame) }) { return near.1 / 60 }
            return Double(frame) / 3600
        }
        // the session's length: the log's own clock when it has one, else the time since the start
        let totalMinutes = frameTimes.map(\.1).max().map { $0 / 60 } ?? Date().timeIntervalSince(started) / 60
        var findings: [DoctorFinding] = []
        let peak = ws.map(\.footprint).max() ?? 0
        let smooth = ws.isEmpty ? nil : Int((Double(ws.filter { $0.p50 <= 17.5 }.count) / Double(ws.count) * 100).rounded())

        // memory: a big footprint, and frames waiting far longer than they worked
        let starved = ws.filter { $0.footprint > physicalMB * 62 / 100 && $0.submit > 20 && ($0.cpu > 0 ? $0.wall > $0.cpu * 3 : true) }
        if starved.count >= 2, let first = starved.first {
            let fps = Int((1000 / max(starved.map(\.p50).reduce(0, +) / Double(starved.count), 1)).rounded())
            findings.append(DoctorFinding(kind: .memory,
                title: "Ran low on memory after about \(formatMinutes(minuteAt(first.frame)))",
                detail: "The game reached \(String(format: "%.1f", Double(peak) / 1024)) GB on a \(physicalMB / 1024) GB Mac, and frames spent most of their time waiting for memory (about \(fps) fps there). Quitting browsers, chat and video apps before playing leaves the game room.",
                fix: .closeApps))
        }
        else if peak > physicalMB * 75 / 100 {
            findings.append(DoctorFinding(kind: .memory,
                title: "Used \(String(format: "%.1f", Double(peak) / 1024)) GB of your \(physicalMB / 1024) GB: close to the limit",
                detail: "The game itself needs most of this Mac's memory. Anything else open (a browser, chat or video apps) pushes it into swapping, which slows it down as a session goes on. Quit other apps before long sessions.",
                fix: .closeApps))
        }
        // GPU: more than one frame's time on the GPU, so the frame rate halves
        let starvedFrames = Set(starved.map(\.frame))
        let gpuBound = ws.filter { $0.gpu > 16.8 && $0.p50 > 17.5 && !starvedFrames.contains($0.frame) }
        if !ws.isEmpty, Double(gpuBound.count) / Double(ws.count) > 0.15 {
            let gpu = gpuBound.map(\.gpu).sorted()[gpuBound.count / 2]
            findings.append(DoctorFinding(kind: .gpu,
                title: "The graphics chip couldn't keep 60 fps in \(Int(Double(gpuBound.count) / Double(ws.count) * 100))% of the session",
                detail: "Heavy scenes needed about \(String(format: "%.0f", gpu)) ms of GPU time per frame; 60 fps allows 16.7 ms, so the game dropped to 30 there and the switching felt like stutter. Dynamic resolution lowers the resolution in heavy scenes to hold 60; a steady 30 fps removes the switching.",
                fix: .dynamicResolution))
        }
        if let worst = slowRatios.min(), worst < 0.8 {
            findings.append(DoctorFinding(kind: .slowMotion,
                title: "Parts of the session ran in slow motion",
                detail: "At its slowest the game ran at \(Int(worst * 100))% of normal speed, because frames took longer than the game allows. The fixes above bring it back to full speed.",
                fix: nil))
        }
        if hitches > 0 {
            findings.append(DoctorFinding(kind: .shaders,
                title: "\(hitches) pause\(hitches == 1 ? "" : "s") while new effects were built",
                detail: "The first time an effect appears, the engine builds it for your Mac (about \(Int(hitchMs / Double(hitches))) ms each). It keeps them, so the same scenes won't pause again.",
                fix: nil))
        }
        if exitCode != 0 && exitCode != 15 {
            findings.append(DoctorFinding(kind: .crash,
                title: exitCode == 70 ? "macOS stopped the game's graphics" : "The game stopped unexpectedly (code \(exitCode))",
                detail: exitCode == 70 ? "macOS revoked the game's GPU access. Starting it again usually works; if it repeats, restart the Mac." : "Its log has the details: Game options › Game Log.",
                fix: nil))
        }
        if findings.isEmpty {
            findings.append(DoctorFinding(kind: .smooth, title: "Smooth session", detail: "No slowdowns worth fixing" + (smooth.map { ": \($0)% of the time at full speed." } ?? "."), fix: nil))
        }
        return DoctorReport(titleID: titleID, gameName: name, date: Date(), minutes: totalMinutes, smoothPercent: smooth,
                            peakFootprintMB: peak > 0 ? peak : nil, findings: findings)
    }
}

// MARK: memory check before a launch

enum MemoryCheck {
    /// Memory macOS can hand to a new app without swapping: free, inactive, purgeable and speculative pages.
    static func availableMB() -> Int {
        var stats = vm_statistics64()
        var count = mach_msg_type_number_t(MemoryLayout<vm_statistics64>.size / MemoryLayout<integer_t>.size)
        let r = withUnsafeMutablePointer(to: &stats) {
            $0.withMemoryRebound(to: integer_t.self, capacity: Int(count)) { host_statistics64(mach_host_self(), HOST_VM_INFO64, $0, &count) }
        }
        guard r == KERN_SUCCESS else { return Int.max }
        let pages = UInt64(stats.free_count) + UInt64(stats.inactive_count) + UInt64(stats.purgeable_count) + UInt64(stats.speculative_count)
        return Int(pages * UInt64(vm_kernel_page_size) / 1_048_576)
    }

    struct HeavyApp: Identifiable { let app: NSRunningApplication; let mb: Int; var id: pid_t { app.processIdentifier } }

    /// The regular apps using the most memory (Finder and the hub itself left out).
    static func heavyApps(limit: Int = 6) -> [HeavyApp] {
        let me = ProcessInfo.processInfo.processIdentifier
        return NSWorkspace.shared.runningApplications
            .filter { $0.activationPolicy == .regular && $0.processIdentifier != me && $0.bundleIdentifier != "com.apple.finder" }
            .compactMap { a -> HeavyApp? in
                var info = rusage_info_v4()
                let r = withUnsafeMutablePointer(to: &info) { p in
                    p.withMemoryRebound(to: rusage_info_t?.self, capacity: 1) { proc_pid_rusage(a.processIdentifier, RUSAGE_INFO_V4, $0) }
                }
                guard r == 0 else { return nil }
                let mb = Int(info.ri_phys_footprint / 1_048_576)
                return mb >= 250 ? HeavyApp(app: a, mb: mb) : nil
            }
            .sorted { $0.mb > $1.mb }
            .prefix(limit).map { $0 }
    }

    /// What a game needs: its last session's peak, else about 10 GB (a Switch 2 game on this engine).
    @MainActor static func neededMB(_ app: NativeApp) -> Int {
        (PerformanceDoctor.shared.reports[app.titleID]?.peakFootprintMB).map { min($0, 13_000) } ?? 10_000
    }
}

/// Shown before a launch when free memory is short.
struct MemoryCheckSheet: View {
    let gameName: String
    let neededMB: Int
    let launch: () -> Void
    @Environment(\.dismiss) private var dismiss
    @State private var apps: [MemoryCheck.HeavyApp] = MemoryCheck.heavyApps()
    @State private var picked: Set<pid_t> = []
    @State private var available = MemoryCheck.availableMB()
    @State private var quitting = false
    @ObservedObject private var doctor = PerformanceDoctor.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            OrbitLabel("Performance Doctor", color: Orbit.peach)
            Text("Free up memory for \(gameName)?").font(Orbit.display(22, 600))
            Text("About \(gb(available)) is free and the game uses about \(gb(neededMB)). Without room it slows down as the session goes on. These apps use the most:")
                .font(Orbit.body(14, 500)).foregroundStyle(Orbit.dim).fixedSize(horizontal: false, vertical: true)
            VStack(spacing: 0) {
                ForEach(apps) { a in
                    Toggle(isOn: Binding(get: { picked.contains(a.id) }, set: { if $0 { picked.insert(a.id) } else { picked.remove(a.id) } })) {
                        HStack {
                            if let icon = a.app.icon { Image(nsImage: icon).resizable().frame(width: 22, height: 22) }
                            Text(a.app.localizedName ?? "App").font(Orbit.body(14, 700))
                            Spacer()
                            Text(gb(a.mb)).font(Orbit.body(14, 700)).foregroundStyle(Orbit.lavender)
                        }
                    }
                    .toggleStyle(.checkbox)
                    .padding(.horizontal, 12).frame(height: 40)
                }
                if apps.isEmpty {
                    Text("No single app is using much; macOS itself may be holding the memory.").font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute).padding(12)
                }
            }
            .background(RoundedRectangle(cornerRadius: 14).fill(Orbit.card))
            Text("Quitting asks each app to close normally, so it can save your work first.").font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
            HStack {
                Toggle("Check before every launch", isOn: $doctor.memoryCheckOn).toggleStyle(.checkbox).font(Orbit.body(13, 500))
                Spacer()
                Button("Cancel") { dismiss() }
                Button("Launch anyway") { dismiss(); launch() }
                Button(quitting ? "Quitting…" : "Quit \(picked.count) & launch") { quitAndLaunch() }
                    .keyboardShortcut(.defaultAction)
                    .disabled(picked.isEmpty || quitting)
            }
        }
        .padding(26)
        .frame(width: 560)
        .background(Orbit.bg)
        .preferredColorScheme(.dark)
        .onAppear { picked = Set(apps.prefix(3).map(\.id)) }
    }

    private func gb(_ mb: Int) -> String { String(format: "%.1f GB", Double(mb) / 1024) }

    private func quitAndLaunch() {
        quitting = true
        let targets = apps.filter { picked.contains($0.id) }.map(\.app)
        for a in targets { a.terminate() }
        Task { @MainActor in
            for _ in 0..<16 where targets.contains(where: { !$0.isTerminated }) { try? await Task.sleep(nanoseconds: 500_000_000) }
            let names = targets.compactMap(\.localizedName).joined(separator: ", ")
            ActivityLog.shared.log("Freed memory: quit \(names)")
            dismiss(); launch()
        }
    }
}

/// The last session's report.
struct DoctorReportView: View {
    let report: DoctorReport
    let app: NativeApp?
    @Environment(\.dismiss) private var dismiss
    @State private var applied: [String: String] = [:]

    var body: some View {
        VStack(alignment: .leading, spacing: 16) {
            OrbitLabel("Performance Doctor", color: Orbit.mint)
            Text(report.gameName).font(Orbit.display(24, 600))
            Text("Session of \(formatMinutes(report.minutes)), \(report.date.formatted(date: .abbreviated, time: .shortened))"
                 + (report.smoothPercent.map { " · \($0)% at full speed" } ?? ""))
                .font(Orbit.body(14, 500)).foregroundStyle(Orbit.dim)
            ScrollView {
                VStack(alignment: .leading, spacing: 12) {
                    ForEach(report.findings) { f in
                        VStack(alignment: .leading, spacing: 8) {
                            HStack(spacing: 10) {
                                Image(systemName: icon(f.kind)).foregroundStyle(color(f.kind))
                                Text(f.title).font(Orbit.body(16, 700))
                            }
                            Text(f.detail).font(Orbit.body(14, 500)).foregroundStyle(Orbit.dim).fixedSize(horizontal: false, vertical: true)
                            if let fix = f.fix, let app {
                                if let done = applied[f.id] {
                                    Label(done, systemImage: "checkmark.circle.fill").font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint)
                                } else {
                                    HStack {
                                        Button(fixTitle(fix)) { applied[f.id] = PerformanceDoctor.shared.apply(fix, app: app) }
                                        if fix == .dynamicResolution {
                                            Button("Steady 30 fps instead") { applied[f.id] = PerformanceDoctor.shared.apply(.fps30, app: app) }
                                        }
                                    }
                                }
                            }
                        }
                        .padding(16)
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .background(RoundedRectangle(cornerRadius: 16).fill(Orbit.card))
                    }
                }
            }
            .frame(maxHeight: 420)
            HStack {
                if let app { Button("Open game log") { NSWorkspace.shared.open(app.logFile) } }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(26)
        .frame(width: 600)
        .background(Orbit.bg)
        .preferredColorScheme(.dark)
    }

    private func fixTitle(_ f: DoctorFinding.Fix) -> String {
        switch f {
        case .dynamicResolution: return "Turn on Dynamic resolution"
        case .fps30: return "Use a steady 30 fps"
        case .closeApps: return "Check memory before each launch"
        }
    }
    private func icon(_ k: DoctorFinding.Kind) -> String {
        switch k {
        case .memory: return "memorychip"
        case .gpu: return "cpu"
        case .shaders: return "sparkles"
        case .slowMotion: return "tortoise.fill"
        case .crash: return "exclamationmark.triangle.fill"
        case .smooth: return "checkmark.seal.fill"
        }
    }
    private func color(_ k: DoctorFinding.Kind) -> Color {
        switch k {
        case .smooth: return Orbit.mint
        case .shaders: return Orbit.lavender
        default: return Orbit.peach
        }
    }
}
