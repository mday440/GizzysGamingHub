import SwiftUI
import AppKit

// Storage: free space on the games' drive, what each game takes (its files, its prepared app and the work files
// preparing leaves behind), clearing work files, a space check before preparing, and the drive's health: how it is
// connected (USB speed, through a hub) and how often it has disconnected.

struct GameStorage: Identifiable {
    let id: String          // game path
    let name: String
    var gameBytes: Int64 = 0
    var appBytes: Int64 = 0
    var workBytes: Int64 = 0
    var workFolders: [URL] = []
}

@MainActor
final class StorageCenter: ObservableObject {
    static let shared = StorageCenter()

    @Published private(set) var volumeName = ""
    @Published private(set) var volumePath: String?
    @Published private(set) var freeBytes: Int64 = 0
    @Published private(set) var totalBytes: Int64 = 0
    @Published private(set) var games: [GameStorage] = []
    @Published private(set) var measuring = false
    @Published private(set) var connection: String?        // "USB · 480 Mb/s · through a hub"
    @Published private(set) var connectionWarning: String?
    @Published private(set) var dropsToday = 0
    @Published private(set) var lastDrop: Date?

    private var watching = false

    // MARK: watching the drive

    func startWatching() {
        guard !watching else { return }
        watching = true
        loadDrops()
        let nc = NSWorkspace.shared.notificationCenter
        nc.addObserver(forName: NSWorkspace.didUnmountNotification, object: nil, queue: .main) { n in
            let url = n.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL
            Task { @MainActor in StorageCenter.shared.volumeGone(url) }
        }
        nc.addObserver(forName: NSWorkspace.didMountNotification, object: nil, queue: .main) { n in
            let url = n.userInfo?[NSWorkspace.volumeURLUserInfoKey] as? URL
            Task { @MainActor in StorageCenter.shared.volumeBack(url) }
        }
        refreshVolume()
        checkConnection()
    }

    private func gamesVolume() -> URL? {
        guard let first = LibraryStore.shared?.entries.first else { return nil }
        return try? URL(fileURLWithPath: first.info.path).resourceValues(forKeys: [.volumeURLKey]).volume
    }

    private func isGamesVolume(_ url: URL?) -> Bool {
        guard let url else { return false }
        let p = url.standardizedFileURL.path
        return LibraryStore.shared?.entries.contains { $0.info.path.hasPrefix(p + "/") } ?? false
    }

    private func volumeGone(_ url: URL?) {
        guard isGamesVolume(url) else { return }
        let key = Self.dayKey()
        let n = UserDefaults.standard.integer(forKey: key) + 1
        UserDefaults.standard.set(n, forKey: key)
        UserDefaults.standard.set(Date(), forKey: "drive.lastDrop")
        loadDrops()
        let name = url?.lastPathComponent ?? "The game drive"
        ActivityLog.shared.log("⚠︎ \(name) disconnected (\(n)× today). Games on it can't load until it's back.")
        if !NativeSessions.shared.running.isEmpty {
            ActivityLog.shared.log("A game was running from it: it may freeze or show glitches. Quit and relaunch once the drive is back.")
        }
    }

    private func volumeBack(_ url: URL?) {
        guard isGamesVolume(url) else { return }
        ActivityLog.shared.log("\(url?.lastPathComponent ?? "The game drive") is back")
        refreshVolume()
        checkConnection()
    }

    private func loadDrops() {
        dropsToday = UserDefaults.standard.integer(forKey: Self.dayKey())
        lastDrop = UserDefaults.standard.object(forKey: "drive.lastDrop") as? Date
    }

    private static func dayKey() -> String {
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd"
        return "drive.drops." + f.string(from: Date())
    }

    func refreshVolume() {
        guard let v = gamesVolume() else { return }
        let r = try? v.resourceValues(forKeys: [.volumeNameKey, .volumeAvailableCapacityKey, .volumeTotalCapacityKey])
        volumeName = r?.volumeName ?? v.lastPathComponent
        volumePath = v.path
        freeBytes = Int64(r?.volumeAvailableCapacity ?? 0)
        totalBytes = Int64(r?.volumeTotalCapacity ?? 0)
    }

    /// How the drive is connected, from System Information's USB tree (its link speed and whether a hub sits between).
    func checkConnection() {
        guard let v = volumePath ?? gamesVolume()?.path else { return }
        Task.detached(priority: .utility) {
            let result = Self.usbLink(volume: v)
            await MainActor.run {
                let s = StorageCenter.shared
                guard let r = result else { s.connection = "Not a USB drive (or not found in the USB tree)"; s.connectionWarning = nil; return }
                s.connection = "USB · \(r.speed)" + (r.hubs.isEmpty ? " · plugged straight into the Mac" : " · through \(r.hubs.joined(separator: " › "))")
                var warn: [String] = []
                if r.speed.contains("480 Mb") || r.speed.contains("12 Mb") {
                    warn.append("It's running at USB 2 speed (about 35 MB/s), so big games load slowly. A 10 Gb/s USB-C cable straight into the Mac fixes that.")
                }
                if !r.hubs.isEmpty {
                    warn.append("It's connected through a hub or dock. If the hub drops, the drive drops with it: plug it straight into the Mac.")
                }
                s.connectionWarning = warn.isEmpty ? nil : warn.joined(separator: " ")
            }
        }
    }

    // "Link Speed" of the USB device holding the volume's disk, and the hubs above it
    nonisolated static func usbLink(volume: String) -> (speed: String, hubs: [String])? {
        var st = statfs()
        guard statfs(volume, &st) == 0 else { return nil }
        let dev = withUnsafeBytes(of: st.f_mntfromname) { raw in String(cString: raw.bindMemory(to: CChar.self).baseAddress!) }
        var disk = dev.replacingOccurrences(of: "/dev/", with: "")
        if let r = disk.range(of: #"s\d+$"#, options: .regularExpression) { disk.removeSubrange(r) }   // disk4s2 → disk4

        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/sbin/system_profiler")
        p.arguments = ["SPUSBHostDataType"]
        let out = Pipe(); p.standardOutput = out; p.standardError = FileHandle.nullDevice
        guard (try? p.run()) != nil else { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()

        // walk the indented tree: a stack of (indent, name), each device's Link Speed, until "BSD Name: diskN"
        var stack: [(indent: Int, name: String, speed: String?)] = []
        for raw in String(decoding: data, as: UTF8.self).components(separatedBy: "\n") {
            let indent = raw.prefix { $0 == " " }.count
            let line = raw.trimmingCharacters(in: .whitespaces)
            if line.isEmpty { continue }
            if line.hasSuffix(":") && !line.contains(": ") {
                while let last = stack.last, last.indent >= indent { stack.removeLast() }
                stack.append((indent, String(line.dropLast()), nil))
            } else if line.hasPrefix("Link Speed:"), !stack.isEmpty {
                stack[stack.count - 1].speed = String(line.dropFirst("Link Speed:".count)).trimmingCharacters(in: .whitespaces)
            } else if line == "BSD Name: \(disk)" {
                // the nearest device above with a link speed holds the disk; devices above it named "Hub" are hubs
                guard let devIndex = stack.lastIndex(where: { $0.speed != nil }) else { return nil }
                let hubs = stack[..<devIndex].filter { $0.name.localizedCaseInsensitiveContains("hub") && $0.speed != nil }.map { $0.name }
                return (stack[devIndex].speed ?? "?", hubs)
            }
        }
        return nil
    }

    // MARK: what each game takes

    func measure() {
        guard !measuring, let entries = LibraryStore.shared?.entries else { return }
        refreshVolume()
        measuring = true
        let items = entries.map { e -> (String, String, String, String, URL?) in
            (e.info.path, e.info.name, e.info.titleIDString, e.info.format, NativeEngine.shared.preparedApp(e.info))
        }
        Task.detached(priority: .utility) {
            var out: [GameStorage] = []
            for (path, name, tid, _, app) in items {
                var g = GameStorage(id: path, name: name)
                g.gameBytes = Self.packageBytes(path)
                if let app { g.appBytes = Self.size(of: app) }
                g.workFolders = Self.workFolders(path: path, tid: tid)
                g.workBytes = g.workFolders.reduce(0) { $0 + Self.size(of: $1) }
                out.append(g)
            }
            let result = out.sorted { $0.gameBytes + $0.appBytes + $0.workBytes > $1.gameBytes + $1.appBytes + $1.workBytes }
            await MainActor.run {
                StorageCenter.shared.games = result
                StorageCenter.shared.measuring = false
            }
        }
    }

    /// The work folders preparing leaves: <title ID>_build and _survey, beside the game file or inside a game folder.
    nonisolated static func workFolders(path: String, tid: String) -> [URL] {
        let u = URL(fileURLWithPath: path)
        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        let bases = isDir.boolValue ? [u, u.deletingLastPathComponent()] : [u.deletingLastPathComponent()]
        var out: [URL] = []
        for b in bases {
            for suffix in ["_build", "_survey"] {
                let w = b.appendingPathComponent(tid + suffix, isDirectory: true)
                if FileManager.default.fileExists(atPath: w.path), !out.contains(w) { out.append(w) }
            }
        }
        return out
    }

    /// The game's own files: a package with its update/DLC packages beside it, or a dump folder's romfs and exefs.
    nonisolated static func packageBytes(_ path: String) -> Int64 {
        let u = URL(fileURLWithPath: path)
        var isDir: ObjCBool = false
        FileManager.default.fileExists(atPath: path, isDirectory: &isDir)
        if isDir.boolValue {
            return ["romfs", "exefs"].reduce(0) { $0 + size(of: u.appendingPathComponent($1)) }
        }
        let exts: Set<String> = ["nsp", "xci", "nsz", "xcz", "nup", "nca"]
        let dir = u.deletingLastPathComponent()
        let files = (try? FileManager.default.contentsOfDirectory(at: dir, includingPropertiesForKeys: [.fileSizeKey])) ?? []
        let packages = files.filter { exts.contains($0.pathExtension.lowercased()) }
        let total = packages.reduce(Int64(0)) { $0 + Int64((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        return total > 0 ? total : Int64((try? u.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0)
    }

    nonisolated static func size(of url: URL) -> Int64 {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { return 0 }
        if !isDir.boolValue { return Int64((try? url.resourceValues(forKeys: [.totalFileAllocatedSizeKey]).totalFileAllocatedSize) ?? 0) }
        var total: Int64 = 0
        let keys: [URLResourceKey] = [.totalFileAllocatedSizeKey, .isRegularFileKey]
        if let e = FileManager.default.enumerator(at: url, includingPropertiesForKeys: keys, options: [], errorHandler: { _, _ in true }) {
            for case let f as URL in e {
                if let r = try? f.resourceValues(forKeys: Set(keys)), r.isRegularFile == true { total += Int64(r.totalFileAllocatedSize ?? 0) }
            }
        }
        return total
    }

    /// Deletes a game's work folders (re-preparing makes them again). Refuses while that game is preparing or running.
    func clearWork(_ g: GameStorage, done: @escaping (String) -> Void) {
        guard let entry = LibraryStore.shared?.entries.first(where: { $0.info.path == g.id }) else { return }
        if case .running? = NativeEngine.shared.builds[g.id]?.phase { done("\(g.name) is being prepared right now."); return }
        if let app = NativeEngine.shared.nativeApp(entry.info), NativeSessions.shared.isRunning(app) { done("Quit \(g.name) first."); return }
        let folders = g.workFolders.filter { $0.lastPathComponent.hasSuffix("_build") || $0.lastPathComponent.hasSuffix("_survey") }
        let bytes = g.workBytes
        Task.detached(priority: .utility) {
            var failed: [String] = []
            for f in folders { do { try FileManager.default.removeItem(at: f) } catch { failed.append(f.lastPathComponent) } }
            await MainActor.run {
                if failed.isEmpty {
                    ActivityLog.shared.log("Cleared \(g.name)'s work files: \(Self.gb(bytes)) freed")
                    done("Freed \(Self.gb(bytes)).")
                } else {
                    done("Couldn't remove \(failed.joined(separator: ", ")).")
                }
                StorageCenter.shared.measure()
            }
        }
    }

    // MARK: the check before preparing

    /// nil when there is room to prepare the game; otherwise why not. Warns in the flight log when it may be tight.
    func spaceProblem(for info: GameInfo) -> String? {
        let u = URL(fileURLWithPath: info.path)
        guard let r = try? u.resourceValues(forKeys: [.volumeAvailableCapacityKey, .volumeNameKey]) else { return nil }
        let free = Int64(r.volumeAvailableCapacity ?? 0)
        let pkg = Self.packageBytes(info.path)
        guard pkg > 0 else { return nil }
        let drive = r.volumeName ?? "the drive"
        // a first preparation's work files take about 3× the game, and the app (copied, not cloned, on HFS+) about 2-3× more
        if Double(free) < Double(pkg) * 2.5 {
            return "Not enough space on \(drive): preparing \(info.name) needs roughly \(Self.gb(Int64(Double(pkg) * 5))) free, and \(Self.gb(free)) is free. Clear work files in Settings › Storage, or move the game to a bigger drive."
        }
        if Double(free) < Double(pkg) * 5 {
            ActivityLog.shared.log("⚠︎ Space is tight on \(drive) (\(Self.gb(free)) free): \(info.name) may run out while preparing. Settings › Storage can free space.")
        }
        return nil
    }

    nonisolated static func gb(_ b: Int64) -> String {
        let g = Double(b) / 1_000_000_000
        return g >= 100 ? String(format: "%.0f GB", g) : String(format: "%.1f GB", g)
    }
}

// MARK: Settings card

struct StorageSettings: View {
    @ObservedObject private var s = StorageCenter.shared
    @State private var confirm: GameStorage?
    @State private var message: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(alignment: .firstTextBaseline) {
                Text(s.volumeName.isEmpty ? "Game drive" : s.volumeName).font(Orbit.body(15, 700))
                Spacer()
                Text("\(StorageCenter.gb(s.freeBytes)) free of \(StorageCenter.gb(s.totalBytes))").font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint)
            }
            if s.totalBytes > 0 {
                GeometryReader { g in
                    ZStack(alignment: .leading) {
                        Capsule().fill(Orbit.bg)
                        Capsule().fill(LinearGradient(colors: [Color(rgb: 0x7C3AED), Orbit.lavender], startPoint: .leading, endPoint: .trailing))
                            .frame(width: g.size.width * CGFloat(1 - Double(s.freeBytes) / Double(max(1, s.totalBytes))))
                    }
                }
                .frame(height: 8)
            }
            if let c = s.connection { Label(c, systemImage: "cable.connector").font(Orbit.body(12, 600)).foregroundStyle(Orbit.dim) }
            if let w = s.connectionWarning {
                Label(w, systemImage: "exclamationmark.triangle.fill").font(Orbit.body(12, 600)).foregroundStyle(Orbit.peach)
                    .fixedSize(horizontal: false, vertical: true)
            }
            if s.dropsToday > 0 {
                Label("Disconnected \(s.dropsToday)× today" + (s.lastDrop.map { ", last at " + $0.formatted(date: .omitted, time: .shortened) } ?? ""),
                      systemImage: "bolt.horizontal.circle").font(Orbit.body(12, 700)).foregroundStyle(Orbit.peach)
            }

            Divider().overlay(Orbit.line)
            HStack {
                Text("Game").frame(maxWidth: .infinity, alignment: .leading)
                Text("Game files").frame(width: 90, alignment: .trailing)
                Text("Mac app").frame(width: 90, alignment: .trailing)
                Text("Work files").frame(width: 90, alignment: .trailing)
                Spacer().frame(width: 110)
            }
            .font(Orbit.body(11, 700)).foregroundStyle(Orbit.mute)
            if s.games.isEmpty && s.measuring { ProgressView().controlSize(.small) }
            ForEach(s.games) { g in
                HStack {
                    Text(g.name).font(Orbit.body(13, 700)).lineLimit(1).frame(maxWidth: .infinity, alignment: .leading)
                    Text(StorageCenter.gb(g.gameBytes)).frame(width: 90, alignment: .trailing)
                    Text(g.appBytes > 0 ? StorageCenter.gb(g.appBytes) : "–").frame(width: 90, alignment: .trailing)
                    Text(g.workBytes > 0 ? StorageCenter.gb(g.workBytes) : "–").frame(width: 90, alignment: .trailing)
                    Button("Clear work files") { confirm = g }
                        .disabled(g.workBytes == 0)
                        .frame(width: 110, alignment: .trailing)
                }
                .font(Orbit.body(13, 500))
            }
            HStack {
                Button(s.measuring ? "Measuring…" : "Measure again") { s.measure(); s.checkConnection() }.disabled(s.measuring)
                if let m = message { Text(m).font(Orbit.body(12, 700)).foregroundStyle(Orbit.mint) }
            }
            Text("Work files are what preparing a game leaves behind. They're not needed to play, only to re-prepare faster (after a game or engine update). Your games, Mac apps and saves are never touched.")
                .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
        }
        .onAppear { s.startWatching(); if s.games.isEmpty { s.measure() } }
        .alert(item: $confirm) { g in
            Alert(title: Text("Clear \(g.name)'s work files?"),
                  message: Text("This permanently deletes \(StorageCenter.gb(g.workBytes)) of work files. The game and its Mac app stay. Re-preparing later rebuilds them, which takes longer."),
                  primaryButton: .destructive(Text("Clear \(StorageCenter.gb(g.workBytes))")) {
                      s.clearWork(g) { message = $0 }
                  },
                  secondaryButton: .cancel())
        }
    }
}
