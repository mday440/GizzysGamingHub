import Foundation
import MGRCore

/// Converts a fixed-size C char array (imported as a tuple) into a Swift String.
func cTupleString<T>(_ tuple: T) -> String {
    withUnsafeBytes(of: tuple) { raw in
        let bytes = raw.prefix { $0 != 0 }
        return String(decoding: bytes, as: UTF8.self)
    }
}

enum Platform: String, Codable, CaseIterable {
    case switch2 = "Switch 2"
    case switch1 = "Switch 1"
    case homebrew = "Homebrew"
    case unknown = "Unknown"

    init(_ p: mgr_platform) {
        switch p {
        case MGR_PLATFORM_SWITCH2: self = .switch2
        case MGR_PLATFORM_SWITCH1: self = .switch1
        case MGR_PLATFORM_HOMEBREW: self = .homebrew
        default: self = .unknown
        }
    }
}

/// Everything the UI needs to know about a game, copied out of the C core.
struct GameInfo: Codable, Hashable {
    var path: String
    var name: String
    var publisher: String
    var displayVersion: String
    var version: UInt32
    var titleID: UInt64
    var format: String
    var platform: Platform
    var packageCount: Int
    var updateApplied: Bool
    var romfsFiles: UInt64
    var exefs: [String]
    var bootable: Bool
    var buildID: String
    var note: String
    var icon: Data?

    var titleIDString: String { String(format: "%016llX", titleID) }
    var versionLabel: String {
        if !displayVersion.isEmpty { return displayVersion }
        if version == 0 { return "1.0" }
        return "rev \(version >> 16)"
    }
}

enum Core {
    static var version: String { String(cString: mgr_version()) }

    /// Reads a game container. Runs synchronously; call from a background queue.
    static func inspect(path: String) -> Result<GameInfo, Error> {
        var err = [CChar](repeating: 0, count: 256)
        guard let g = mgr_open(path, &err, err.count) else {
            let msg = String(cString: err)
            return .failure(NSError(domain: "MikesGameRoom", code: 1,
                                    userInfo: [NSLocalizedDescriptionKey: msg.isEmpty ? "Could not open game" : msg]))
        }
        defer { mgr_close(g) }
        let i = mgr_info(g)!.pointee

        var exefs: [String] = []
        withUnsafeBytes(of: i.exefs_names) { raw in
            for k in 0..<Int(i.exefs_count) {
                let slice = raw[(k * 32)..<(k * 32 + 32)].prefix { $0 != 0 }
                exefs.append(String(decoding: slice, as: UTF8.self))
            }
        }
        var buildID = ""
        if i.has_build_id != 0 {
            withUnsafeBytes(of: i.build_id) { raw in
                var bytes = Array(raw)
                while bytes.count > 20, bytes.last == 0 { bytes.removeLast() }
                buildID = bytes.map { String(format: "%02X", $0) }.joined()
            }
        }
        var icon: Data? = nil
        var iconPtr: UnsafePointer<UInt8>? = nil
        var iconLen: Int = 0
        if mgr_icon(g, &iconPtr, &iconLen) != 0, let p = iconPtr, iconLen > 0 {
            icon = Data(bytes: p, count: iconLen)
        }
        var name = cTupleString(i.name)
        if name.isEmpty { name = (path as NSString).lastPathComponent }

        return .success(GameInfo(
            path: path,
            name: name,
            publisher: cTupleString(i.publisher),
            displayVersion: cTupleString(i.display_version),
            version: i.version,
            titleID: i.title_id,
            format: cTupleString(i.format_name),
            platform: Platform(i.platform),
            packageCount: Int(i.package_count),
            updateApplied: i.update_applied != 0,
            romfsFiles: i.romfs_file_count,
            exefs: exefs,
            bootable: i.bootable != 0,
            buildID: buildID,
            note: cTupleString(i.note),
            icon: icon))
    }

    /// Loads key files into the core. Returns number of keys loaded.
    @discardableResult
    static func loadKeys(at url: URL) -> Int {
        Int(mgr_keys_load(url.path))
    }

    static var keyCount: Int { Int(mgr_keys_count()) }
    static var hasHeaderKey: Bool { mgr_keys_have_header_key() != 0 }

    static let fileExtensions: Set<String> = ["nup", "nsp", "xci", "nca", "nso", "nro", "nsz", "xcz"]

    static func looksLikeGame(_ url: URL) -> Bool {
        var isDir: ObjCBool = false
        guard FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir) else { return false }
        if isDir.boolValue {
            let fm = FileManager.default
            for sub in ["exefs", "romfs", "base/exefs", "base/romfs"] {
                if fm.fileExists(atPath: url.appendingPathComponent(sub).path) { return true }
            }
            return fm.fileExists(atPath: url.appendingPathComponent("main.npdm").path)
        }
        return fileExtensions.contains(url.pathExtension.lowercased())
    }
}
