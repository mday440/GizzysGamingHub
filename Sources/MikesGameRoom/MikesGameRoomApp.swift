import SwiftUI
import AppKit

@main
struct MikesGameRoomApp: App {
    @NSApplicationDelegateAdaptor(AppDelegate.self) private var appDelegate
    @StateObject private var library = LibraryStore()

    init() { Orbit.registerFonts() }

    var body: some Scene {
        WindowGroup("Gizzy's Gaming Hub", id: "library") {
            OrbitRootView()
                .environmentObject(library)
                .frame(minWidth: 1180, minHeight: 840)
                .onOpenURL { url in library.add(urls: [url]) }
        }
        .windowStyle(.hiddenTitleBar)
        .defaultSize(width: 1440, height: 900)
        .commands {
            CommandGroup(replacing: .newItem) {}
            CommandGroup(after: .toolbar) {
                Button("Toggle List / Icon View") { NotificationCenter.default.post(name: .mgrToggleGrid, object: nil) }
                    .keyboardShortcut("l")
                Divider()
                ForEach(RetroTab.allCases) { t in
                    Button(t.rawValue) { NotificationCenter.default.post(name: .mgrRetroTab, object: t) }
                }
            }
            CommandGroup(replacing: .help) {
                Button("Controls Guide") { NotificationCenter.default.post(name: .mgrRetroTab, object: RetroTab.controllers) }
            }
            CommandMenu("Library") {
                Button("Add Games...") { NotificationCenter.default.post(name: .mgrAddGames, object: nil) }
                    .keyboardShortcut("o")
                Button("Rescan Game Folders") { library.rescanFolders() }
                    .keyboardShortcut("r", modifiers: [.command, .shift])
                Button("Refresh Library") { library.refreshAll() }
            }
        }

        Window("Gizzy TV", id: "tv") {
            BigTVView()
                .preferredColorScheme(.dark)
        }
        .defaultSize(width: 1300, height: 560)

        Window("Sign in to Twitch", id: "twitch-signin") {
            TwitchSignInView()
        }
        .defaultSize(width: 560, height: 760)

        WindowGroup("Player", id: "player", for: String.self) { $path in
            if let path {
                PlayerView(path: path)
                    .environmentObject(library)
                    .frame(minWidth: 820, minHeight: 640)
            }
        }

        Settings {
            SettingsView()
                .environmentObject(library)
                .frame(width: 560, height: 460)
        }
    }
}

final class AppDelegate: NSObject, NSApplicationDelegate {
    func applicationDidFinishLaunching(_ notification: Notification) {
        NSApp.setActivationPolicy(.regular)
        NSApp.activate(ignoringOtherApps: true)
    }
    func applicationShouldTerminateAfterLastWindowClosed(_ sender: NSApplication) -> Bool { true }
}

extension Notification.Name {
    static let mgrAddGames = Notification.Name("mgrAddGames")
}

/// Brand colours used across the app (match the icon).
enum Brand {
    static let purple = Color(red: 0.48, green: 0.18, blue: 0.97)
    static let pink = Color(red: 0.95, green: 0.03, blue: 0.64)
    static let orange = Color(red: 1.0, green: 0.54, blue: 0.0)
    static let gradient = LinearGradient(colors: [purple, pink, orange], startPoint: .topLeading, endPoint: .bottomTrailing)
    static var appIcon: NSImage { NSApp.applicationIconImage ?? NSImage() }
}
