import Foundation

/// Where the hub keeps its engine, tools and data.
/// A fresh install keeps everything in ~/Library/Application Support/MikesGameRoom; an existing setup on the
/// "NVME SSD" work drive (the original developer's) keeps using that drive, so nothing moves under it.
enum HubPaths {
    static let legacyDrive = "/Volumes/NVME SSD"
    static var support: URL {   // the same folder as LibraryStore.supportDir (not called from here: that one is main-actor isolated)
        let base = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        return dir(base.appendingPathComponent("MikesGameRoom", isDirectory: true))
    }

    private static func dir(_ u: URL) -> URL {
        try? FileManager.default.createDirectory(at: u, withIntermediateDirectories: true)
        return u
    }
    private static func exists(_ p: String) -> Bool { FileManager.default.fileExists(atPath: p) }

    /// The hub's own data (cover art, Performance Doctor notes, save snapshots).
    static var dataRoot: URL {
        let legacy = legacyDrive + "/MikesGameRoom Data"
        if exists(legacy) { return URL(fileURLWithPath: legacy, isDirectory: true) }
        return dir(support.appendingPathComponent("Data", isDirectory: true))
    }

    /// The engine copy the hub uses when it was not built from a source tree: unpacked from the app on first run.
    static var installedEngine: URL { support.appendingPathComponent("Engine/nxnative", isDirectory: true) }
    /// The engine that ships inside the app (Contents/Resources/Engine/nxnative), copied out by the first-run setup.
    static var bundledEngine: URL? {
        let u = Bundle.main.resourceURL?.appendingPathComponent("Engine/nxnative", isDirectory: true)
        return u.flatMap { exists($0.appendingPathComponent("scripts/build_ipa_from_nsp.py").path) ? $0 : nil }
    }
    static var defaultEngine: String {
        var u = Bundle.main.bundleURL   // a build/…app inside a source tree uses that tree's engine
        for _ in 0..<4 {
            u.deleteLastPathComponent()
            let e = u.appendingPathComponent("Engines/nxnative")
            if exists(e.appendingPathComponent("scripts/build_ipa_from_nsp.py").path) { return e.path }
        }
        let legacy = legacyDrive + "/MikesGameRoom/Engines/nxnative"
        if exists(legacy + "/scripts/build_ipa_from_nsp.py") { return legacy }
        return installedEngine.path
    }
    /// Python environment and .NET for the engine.
    static var defaultTools: String {
        let legacy = legacyDrive + "/nxnative-tools"
        if exists(legacy + "/venv/bin/python3") { return legacy }
        return support.appendingPathComponent("Tools", isDirectory: true).path
    }
    static var defaultXcode: String {
        for p in [legacyDrive + "/Applications/Xcode.app", legacyDrive + "/Applications/Xcode-27.2.app", "/Applications/Xcode-27.2.app", "/Applications/Xcode.app", "/Applications/Xcode-beta.app"]
        where exists(p + "/Contents/Developer/usr/bin/xcodebuild") { return p }
        return "/Applications/Xcode.app"
    }
    /// MikesGameRoom's own ZBIC decoder: inside the app (Contents/MacOS/mgrzbic), beside it, or in the source tree's .build.
    static func zbicTool(enginePath: String) -> String {
        if let inside = Bundle.main.executableURL?.deletingLastPathComponent().appendingPathComponent("mgrzbic").path, exists(inside) { return inside }
        let beside = Bundle.main.bundleURL.deletingLastPathComponent().appendingPathComponent("mgrzbic").path
        if exists(beside) { return beside }
        return URL(fileURLWithPath: enginePath).deletingLastPathComponent().deletingLastPathComponent().appendingPathComponent(".build/mgrzbic").path
    }
}
