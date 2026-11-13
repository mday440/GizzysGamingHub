import Foundation
import SwiftUI
import AppKit

// Save time machine: after every session (and on demand) the game's committed save folder is copied to the work
// drive, so any earlier save can be brought back. The engine keeps a game's committed save in save/ and rebuilds its
// transaction copies (save.work, save.new) at each mount, so copying save/ while the game is closed is complete.

struct SaveSnapshot: Codable, Identifiable, Hashable {
    var id: String { folder }
    let folder: String          // folder name under the game's snapshot folder
    var label: String
    let date: Date
    let minutes: Double?
    let bytes: Int64
    let fingerprint: String
}

@MainActor
final class SaveVault: ObservableObject {
    static let shared = SaveVault()
    @Published private(set) var snapshots: [String: [SaveSnapshot]] = [:]   // by title ID, newest first
    @Published private(set) var working: Set<String> = []

    var root: URL { GizzyData.dir("Save Snapshots") }
    func folder(_ tid: String) -> URL { root.appendingPathComponent(tid, isDirectory: true) }

    func list(_ tid: String) -> [SaveSnapshot] {
        if let s = snapshots[tid] { return s }
        let dec = JSONDecoder()
        let dirs = (try? FileManager.default.contentsOfDirectory(at: folder(tid), includingPropertiesForKeys: nil)) ?? []
        let s = dirs.compactMap { d -> SaveSnapshot? in
            guard let data = try? Data(contentsOf: d.appendingPathComponent("snapshot.json")) else { return nil }
            return try? dec.decode(SaveSnapshot.self, from: data)
        }.sorted { $0.date > $1.date }
        snapshots[tid] = s
        return s
    }

    /// Copies the committed save, unless it is unchanged since the newest snapshot.
    func snapshot(_ app: NativeApp, label: String, minutes: Double?, force: Bool = false, then: ((SaveSnapshot?) -> Void)? = nil) {
        let tid = app.titleID, src = app.saveDir, dest = folder(tid)
        guard FileManager.default.fileExists(atPath: src.path), !working.contains(tid) else { then?(nil); return }
        let latest = list(tid).first?.fingerprint
        working.insert(tid)
        Task.detached(priority: .utility) {
            let (fp, bytes) = Self.fingerprint(src)
            var made: SaveSnapshot?
            if force || fp != latest {
                let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH.mm.ss"
                let name = f.string(from: Date())
                let target = dest.appendingPathComponent(name, isDirectory: true)
                try? FileManager.default.createDirectory(at: target, withIntermediateDirectories: true)
                if Self.ditto(src, target.appendingPathComponent("save")) {
                    let s = SaveSnapshot(folder: name, label: label, date: Date(), minutes: minutes, bytes: bytes, fingerprint: fp)
                    let enc = JSONEncoder(); enc.outputFormatting = .prettyPrinted
                    try? enc.encode(s).write(to: target.appendingPathComponent("snapshot.json"), options: .atomic)
                    made = s
                }
            }
            let result = made
            await MainActor.run {
                let vault = SaveVault.shared
                vault.working.remove(tid)
                if let result { vault.snapshots[tid] = [result] + (vault.snapshots[tid] ?? []) }
                then?(result)
            }
        }
    }

    /// Puts a snapshot back as the game's save. The current save is snapshotted first, so this can be undone.
    func restore(_ s: SaveSnapshot, app: NativeApp, name: String, done: @escaping (String) -> Void) {
        guard !NativeSessions.shared.isRunning(app) else { done("Quit the game first."); return }
        snapshot(app, label: "Before restoring “\(s.label)”", minutes: nil, force: true) { _ in
            let saved = self.folder(app.titleID).appendingPathComponent(s.folder).appendingPathComponent("save")
            let live = app.saveDir
            let staging = live.deletingLastPathComponent().appendingPathComponent("save.restore")
            Task.detached(priority: .userInitiated) {
                let fm = FileManager.default
                try? fm.removeItem(at: staging)
                var ok = Self.ditto(saved, staging)
                if ok {
                    // swap in: the old live save is already in the vault ("Before restoring …")
                    let old = live.deletingLastPathComponent().appendingPathComponent("save.replaced")
                    try? fm.removeItem(at: old)
                    if fm.fileExists(atPath: live.path) { ok = (try? fm.moveItem(at: live, to: old)) != nil }
                    if ok { ok = (try? fm.moveItem(at: staging, to: live)) != nil }
                    if ok { try? fm.removeItem(at: old) }
                    else if fm.fileExists(atPath: old.path), !fm.fileExists(atPath: live.path) { try? fm.moveItem(at: old, to: live) }
                }
                let result = ok
                await MainActor.run {
                    ActivityLog.shared.log(result ? "Restored \(name)'s save: \(s.label)" : "Could not restore \(name)'s save")
                    done(result ? "Restored. The game starts from this save next time." : "The restore failed; your save is unchanged.")
                }
            }
        }
    }

    func rename(_ s: SaveSnapshot, tid: String, to label: String) {
        var n = s; n.label = label
        let url = folder(tid).appendingPathComponent(s.folder).appendingPathComponent("snapshot.json")
        let enc = JSONEncoder(); enc.outputFormatting = .prettyPrinted
        try? enc.encode(n).write(to: url, options: .atomic)
        snapshots[tid] = (snapshots[tid] ?? []).map { $0.folder == s.folder ? n : $0 }
    }

    nonisolated static func ditto(_ from: URL, _ to: URL) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/ditto")
        p.arguments = [from.path, to.path]
        do { try p.run() } catch { return false }
        p.waitUntilExit()
        return p.terminationStatus == 0
    }

    /// A cheap fingerprint of a folder: every file's relative path, size and modification time.
    nonisolated static func fingerprint(_ dir: URL) -> (String, Int64) {
        let fm = FileManager.default
        var parts: [String] = [], bytes: Int64 = 0
        let keys: [URLResourceKey] = [.fileSizeKey, .contentModificationDateKey, .isRegularFileKey]
        if let e = fm.enumerator(at: dir, includingPropertiesForKeys: keys) {
            for case let u as URL in e {
                guard let v = try? u.resourceValues(forKeys: Set(keys)), v.isRegularFile == true else { continue }
                let size = Int64(v.fileSize ?? 0); bytes += size
                parts.append("\(u.path.dropFirst(dir.path.count))|\(size)|\(v.contentModificationDate?.timeIntervalSince1970 ?? 0)")
            }
        }
        var h: UInt64 = 0xcbf29ce484222325
        for b in parts.sorted().joined(separator: "\n").utf8 { h = (h ^ UInt64(b)) &* 0x100000001b3 }
        return (String(h, radix: 16), bytes)
    }
}

/// The save timeline for one game.
struct SaveTimelineView: View {
    let app: NativeApp
    let name: String
    @ObservedObject private var vault = SaveVault.shared
    @ObservedObject private var sessions = NativeSessions.shared
    @Environment(\.dismiss) private var dismiss
    @State private var confirm: SaveSnapshot?
    @State private var editing: SaveSnapshot?
    @State private var newLabel = ""
    @State private var message: String?

    var body: some View {
        let list = vault.snapshots[app.titleID] ?? vault.list(app.titleID)
        VStack(alignment: .leading, spacing: 14) {
            OrbitLabel("Save time machine", color: Orbit.mint)
            Text(name).font(Orbit.display(24, 600))
            Text("A copy of your save is kept after every session, on the NVME drive. Bring any of them back; the save you have now is kept too.")
                .font(Orbit.body(14, 500)).foregroundStyle(Orbit.dim).fixedSize(horizontal: false, vertical: true)
            ScrollView {
                VStack(spacing: 8) {
                    if list.isEmpty {
                        Text("No snapshots yet. One is made when you finish a session, or with Snapshot now.")
                            .font(Orbit.body(14, 500)).foregroundStyle(Orbit.mute).padding(20)
                    }
                    ForEach(Array(list.enumerated()), id: \.element.id) { i, s in
                        HStack(spacing: 14) {
                            Circle().fill(i == 0 ? Orbit.mint : Orbit.lavender.opacity(0.5)).frame(width: 10, height: 10)
                            VStack(alignment: .leading, spacing: 3) {
                                Text(s.label).font(Orbit.body(15, 700))
                                Text(s.date.formatted(date: .abbreviated, time: .shortened) + " · " + ByteCountFormatter.string(fromByteCount: s.bytes, countStyle: .file))
                                    .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
                            }
                            Spacer()
                            Button("Rename") { editing = s; newLabel = s.label }.buttonStyle(.link)
                            Button("Show") {
                                NSWorkspace.shared.activateFileViewerSelecting([vault.folder(app.titleID).appendingPathComponent(s.folder)])
                            }.buttonStyle(.link)
                            Button("Restore") { confirm = s }.disabled(sessions.isRunning(app))
                        }
                        .padding(.horizontal, 14).frame(height: 56)
                        .background(RoundedRectangle(cornerRadius: 14).fill(Orbit.card))
                    }
                }
            }
            .frame(height: 360)
            if let message { Text(message).font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint) }
            HStack {
                Button(vault.working.contains(app.titleID) ? "Copying…" : "Snapshot now") {
                    vault.snapshot(app, label: "Saved by hand", minutes: nil, force: true) { s in
                        message = s == nil ? "There is no save yet." : "Snapshot made."
                    }
                }
                .disabled(sessions.isRunning(app) || vault.working.contains(app.titleID))
                Button("Open snapshots folder") { NSWorkspace.shared.open(vault.folder(app.titleID)) }
                Spacer()
                Button("Done") { dismiss() }.keyboardShortcut(.defaultAction)
            }
        }
        .padding(26)
        .frame(width: 640)
        .background(Orbit.bg)
        .preferredColorScheme(.dark)
        .alert("Restore this save?", isPresented: Binding(get: { confirm != nil }, set: { if !$0 { confirm = nil } }), presenting: confirm) { s in
            Button("Restore") { vault.restore(s, app: app, name: name) { message = $0 } }
            Button("Cancel", role: .cancel) {}
        } message: { s in
            Text("\(name) will start from “\(s.label)”. Your current save is copied to the timeline first, so you can switch back.")
        }
        .alert("Rename snapshot", isPresented: Binding(get: { editing != nil }, set: { if !$0 { editing = nil } })) {
            TextField("Name", text: $newLabel)
            Button("Save") { if let e = editing { vault.rename(e, tid: app.titleID, to: newLabel) } }
            Button("Cancel", role: .cancel) {}
        }
    }
}
