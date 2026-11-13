import Foundation
import SwiftUI

enum LibraryFilter: String, CaseIterable, Identifiable {
    case all = "All Games"
    case switch2 = "Switch 2"
    case switch1 = "Switch 1"
    case homebrew = "Homebrew"
    case needsAttention = "Needs Attention"
    var id: String { rawValue }

    var symbol: String {
        switch self {
        case .all: return "square.grid.2x2.fill"
        case .switch2: return "2.square.fill"
        case .switch1: return "1.square.fill"
        case .homebrew: return "hammer.fill"
        case .needsAttention: return "exclamationmark.triangle.fill"
        }
    }
}

struct LibraryEntry: Identifiable, Codable, Hashable {
    var id: String { info.path }
    var info: GameInfo
    var added: Date
    var lastPlayed: Date?
    var favorite: Bool?
    var coverArt: String?    // a picture the player chose for the game (Cover Art folder on the work drive)
    var playSeconds: Double?  // total time played in the native engine
}

/// Persistent game library + settings, stored in ~/Library/Application Support/MikesGameRoom.
@MainActor
final class LibraryStore: ObservableObject {
    @Published var entries: [LibraryEntry] = []
    @Published var folders: [String] = []
    @Published var scanning = false
    @Published var lastError: String?
    @Published var keyStatus = "No keys loaded"

    static var supportDir: URL {
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        let dir = base.appendingPathComponent("MikesGameRoom", isDirectory: true)
        try? FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
        return dir
    }
    private var libraryFile: URL { Self.supportDir.appendingPathComponent("library.json") }
    private var foldersFile: URL { Self.supportDir.appendingPathComponent("folders.json") }
    static var keysDir: URL {
        let d = supportDir.appendingPathComponent("keys", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    /// The app's library, for features that run outside the views (session hooks, the overnight queue).
    static weak var shared: LibraryStore?

    init() {
        reloadKeys()
        load()
        LibraryStore.shared = self
    }

    func addPlaytime(_ path: String, seconds: Double) {
        guard seconds > 0, let i = entries.firstIndex(where: { $0.info.path == path }) else { return }
        entries[i].playSeconds = (entries[i].playSeconds ?? 0) + seconds
        save()
    }

    // MARK: persistence
    private func load() {
        let dec = JSONDecoder()
        if let d = try? Data(contentsOf: libraryFile), let e = try? dec.decode([LibraryEntry].self, from: d) {
            entries = e.filter { FileManager.default.fileExists(atPath: $0.info.path) }
        }
        if let d = try? Data(contentsOf: foldersFile), let f = try? dec.decode([String].self, from: d) {
            folders = f
        }
    }

    private func save() {
        let enc = JSONEncoder()
        enc.outputFormatting = [.prettyPrinted]
        if let d = try? enc.encode(entries) { try? d.write(to: libraryFile, options: .atomic) }
        if let d = try? enc.encode(folders) { try? d.write(to: foldersFile, options: .atomic) }
    }

    // MARK: keys
    func reloadKeys() {
        let fm = FileManager.default
        let files = (try? fm.contentsOfDirectory(at: Self.keysDir, includingPropertiesForKeys: nil)) ?? []
        for f in files where f.pathExtension.lowercased() == "keys" || f.pathExtension.lowercased() == "txt" {
            Core.loadKeys(at: f)
        }
        updateKeyStatus()
    }

    private func updateKeyStatus() {
        if Core.keyCount == 0 { keyStatus = "No keys loaded" }
        else { keyStatus = "\(Core.keyCount) keys loaded" + (Core.hasHeaderKey ? "" : " (header_key missing)") }
    }

    /// Copies the user's own key files into the app's support folder and loads them.
    func importKeys(_ urls: [URL]) {
        for url in urls {
            let dest = Self.keysDir.appendingPathComponent(url.lastPathComponent.hasSuffix(".keys") ? url.lastPathComponent : url.lastPathComponent + ".keys")
            try? FileManager.default.removeItem(at: dest)
            do {
                try FileManager.default.copyItem(at: url, to: dest)
                Core.loadKeys(at: dest)
            } catch {
                lastError = "Could not import \(url.lastPathComponent): \(error.localizedDescription)"
            }
        }
        updateKeyStatus()
        refreshAll()
    }

    // MARK: library
    func filtered(_ f: LibraryFilter, search: String) -> [LibraryEntry] {
        let base: [LibraryEntry]
        switch f {
        case .all: base = entries
        case .switch2: base = entries.filter { $0.info.platform == .switch2 }
        case .switch1: base = entries.filter { $0.info.platform == .switch1 }
        case .homebrew: base = entries.filter { $0.info.platform == .homebrew }
        case .needsAttention: base = entries.filter { !$0.info.bootable }
        }
        let s = search.trimmingCharacters(in: .whitespaces).lowercased()
        let list = s.isEmpty ? base : base.filter {
            $0.info.name.lowercased().contains(s) || $0.info.titleIDString.lowercased().contains(s) || $0.info.publisher.lowercased().contains(s)
        }
        return list.sorted { $0.info.name.localizedCaseInsensitiveCompare($1.info.name) == .orderedAscending }
    }

    func count(_ f: LibraryFilter) -> Int { filtered(f, search: "").count }

    /// Adds files or folders. Folders that are not themselves a game dump are scanned recursively.
    func add(urls: [URL]) {
        scanning = true
        let existing = Set(entries.map { $0.info.path })
        Task.detached(priority: .userInitiated) {
            var found: [GameInfo] = []
            var errors: [String] = []
            for url in urls {
                for candidate in LibraryStore.discover(url) where !existing.contains(candidate.path) {
                    switch Core.inspect(path: candidate.path) {
                    case .success(let info): found.append(info)
                    case .failure(let e): errors.append("\(candidate.lastPathComponent): \(e.localizedDescription)")
                    }
                }
            }
            let foundGames = found
            let errorList = errors
            await MainActor.run {
                let now = Date()
                for info in foundGames where !self.entries.contains(where: { $0.info.path == info.path }) {
                    self.entries.append(LibraryEntry(info: info, added: now, lastPlayed: nil))
                }
                if !errorList.isEmpty { self.lastError = errorList.prefix(5).joined(separator: "\n") }
                self.scanning = false
                self.save()
            }
        }
    }

    func addFolder(_ url: URL) {
        if !folders.contains(url.path) { folders.append(url.path) }
        save()
        add(urls: [url])
    }

    func removeFolder(_ path: String) {
        folders.removeAll { $0 == path }
        save()
    }

    func rescanFolders() { add(urls: folders.map { URL(fileURLWithPath: $0) }) }

    func remove(_ entry: LibraryEntry) {
        entries.removeAll { $0.id == entry.id }
        save()
    }

    func markPlayed(_ path: String) {
        if let i = entries.firstIndex(where: { $0.info.path == path }) {
            entries[i].lastPlayed = Date()
            save()
        }
    }

    /// Where chosen cover pictures are kept: on the work drive with MikesGameRoom's other data.
    static var coverArtDir: URL {
        let d = HubPaths.dataRoot.appendingPathComponent("Cover Art", isDirectory: true)
        try? FileManager.default.createDirectory(at: d, withIntermediateDirectories: true)
        return d
    }

    /// Copies a picture into the Cover Art folder and uses it for the game (nil url: back to the game's own icon).
    func setCoverArt(_ path: String, from url: URL?) {
        guard let i = entries.firstIndex(where: { $0.info.path == path }) else { return }
        if let url {
            let dest = Self.coverArtDir.appendingPathComponent(entries[i].info.titleIDString + "-" + String(Int(Date().timeIntervalSince1970)) + "." + (url.pathExtension.isEmpty ? "png" : url.pathExtension.lowercased()))
            do { try FileManager.default.copyItem(at: url, to: dest); entries[i].coverArt = dest.path }
            catch { lastError = "Could not use \(url.lastPathComponent): \(error.localizedDescription)"; return }
        } else {
            entries[i].coverArt = nil
        }
        save()
    }

    func toggleFavorite(_ path: String) {
        if let i = entries.firstIndex(where: { $0.info.path == path }) {
            entries[i].favorite = !(entries[i].favorite ?? false)
            save()
        }
    }

    /// Re-reads every game (e.g. after keys were imported).
    func refreshAll() {
        let paths = entries.map { $0.info.path }
        scanning = true
        Task.detached(priority: .userInitiated) {
            var updated: [String: GameInfo] = [:]
            for p in paths { if case .success(let info) = Core.inspect(path: p) { updated[p] = info } }
            let results = updated
            await MainActor.run {
                for i in self.entries.indices {
                    if let info = results[self.entries[i].info.path] { self.entries[i].info = info }
                }
                self.scanning = false
                self.save()
            }
        }
    }

    nonisolated static func discover(_ url: URL) -> [URL] {
        if Core.looksLikeGame(url) { return [url] }
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir), isDir.boolValue else { return [] }
        var out: [URL] = []
        let fm = FileManager.default
        guard let items = try? fm.contentsOfDirectory(at: url, includingPropertiesForKeys: [.isDirectoryKey],
                                                      options: [.skipsHiddenFiles]) else { return [] }
        for item in items {
            if Core.looksLikeGame(item) { out.append(item); continue }
            let dir = (try? item.resourceValues(forKeys: [.isDirectoryKey]).isDirectory) ?? false
            if dir, out.count < 5000 { out.append(contentsOf: discover(item)) }
        }
        return out
    }
}
