import SwiftUI
import AppKit
import UniformTypeIdentifiers

struct LibraryView: View {
    @EnvironmentObject var library: LibraryStore
    @Environment(\.openWindow) private var openWindow
    @State private var filter: LibraryFilter? = .all
    @State private var search = ""
    @State private var showImporter = false
    @State private var selected: LibraryEntry?
    @State private var dropTargeted = false

    private let columns = [GridItem(.adaptive(minimum: 170, maximum: 210), spacing: 22)]

    var body: some View {
        NavigationSplitView {
            sidebar
        } detail: {
            detail
        }
        .searchable(text: $search, placement: .toolbar, prompt: "Search games")
        .toolbar {
            ToolbarItemGroup(placement: .primaryAction) {
                if library.scanning { ProgressView().controlSize(.small) }
                Button { showImporter = true } label: { Label("Add Games", systemImage: "plus") }
                    .help("Add game files or folders")
            }
        }
        .fileImporter(isPresented: $showImporter, allowedContentTypes: [.item, .folder], allowsMultipleSelection: true) { result in
            if case .success(let urls) = result { library.add(urls: urls) }
        }
        .onReceive(NotificationCenter.default.publisher(for: .mgrAddGames)) { _ in showImporter = true }
        .sheet(item: $selected) { entry in
            GameDetailView(entry: entry) {
                selected = nil
                play(entry)
            }
            .environmentObject(library)
        }
        .alert("Some items could not be added", isPresented: Binding(
            get: { library.lastError != nil }, set: { if !$0 { library.lastError = nil } })) {
            Button("OK", role: .cancel) { library.lastError = nil }
        } message: {
            Text(library.lastError ?? "")
        }
    }

    private var sidebar: some View {
        List(selection: $filter) {
            Section {
                HStack(spacing: 10) {
                    Image(nsImage: Brand.appIcon).resizable().frame(width: 38, height: 38)
                    VStack(alignment: .leading, spacing: 0) {
                        Text("Mike's").font(.system(size: 12, weight: .heavy, design: .rounded)).foregroundStyle(.secondary)
                        Text("Game Room").font(.system(size: 17, weight: .black, design: .rounded))
                            .foregroundStyle(Brand.gradient)
                    }
                }
                .padding(.vertical, 6)
            }
            Section("Library") {
                ForEach(LibraryFilter.allCases) { f in
                    Label(f.rawValue, systemImage: f.symbol)
                        .badge(library.count(f))
                        .tag(f)
                }
            }
        }
        .navigationSplitViewColumnWidth(min: 200, ideal: 220)
        .safeAreaInset(edge: .bottom) {
            VStack(alignment: .leading, spacing: 4) {
                Label(library.keyStatus, systemImage: Core.hasHeaderKey ? "key.fill" : "key")
                    .font(.caption).foregroundStyle(.secondary)
                Text("Core \(Core.version)").font(.caption2).foregroundStyle(.tertiary)
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(12)
        }
    }

    @ViewBuilder private var detail: some View {
        let games = library.filtered(filter ?? .all, search: search)
        ZStack {
            if library.entries.isEmpty {
                EmptyLibraryView { showImporter = true }
            } else if games.isEmpty {
                Text("No games match").font(.title3).foregroundStyle(.secondary)
            } else {
                ScrollView {
                    LazyVGrid(columns: columns, spacing: 26) {
                        ForEach(games) { entry in
                            GameCard(info: entry.info)
                                .onTapGesture(count: 2) { play(entry) }
                                .onTapGesture { selected = entry }
                                .contextMenu {
                                    Button("Play") { play(entry) }
                                    Button("Details") { selected = entry }
                                    Button("Open in the Emulator (experimental)") { openEmulator(entry) }
                                    Button("Show in Finder") {
                                        NSWorkspace.shared.activateFileViewerSelecting([URL(fileURLWithPath: entry.info.path)])
                                    }
                                    Divider()
                                    Button("Remove from Library", role: .destructive) { library.remove(entry) }
                                }
                        }
                    }
                    .padding(28)
                }
            }
            if dropTargeted {
                RoundedRectangle(cornerRadius: 24)
                    .strokeBorder(Brand.gradient, style: StrokeStyle(lineWidth: 5, dash: [14, 10]))
                    .padding(16)
                    .overlay(Text("Drop games to add them").font(.title2.bold()).foregroundStyle(Brand.gradient))
            }
        }
        .frame(maxWidth: .infinity, maxHeight: .infinity)
        .navigationTitle((filter ?? .all).rawValue)
        .dropDestination(for: URL.self) { urls, _ in
            library.add(urls: urls)
            return true
        } isTargeted: { dropTargeted = $0 }
    }

    /// Play: the native engine's prepared app when there is one; otherwise the details, where the game can be prepared.
    private func play(_ entry: LibraryEntry) {
        if NativeEngine.shared.play(entry.info) { library.markPlayed(entry.info.path); return }
        selected = entry
    }
    /// MikesGameRoom's own emulator (Sources/MGRCore): still in development, kept for research.
    private func openEmulator(_ entry: LibraryEntry) {
        library.markPlayed(entry.info.path)
        openWindow(id: "player", value: entry.info.path)
    }
}

struct EmptyLibraryView: View {
    var add: () -> Void
    var body: some View {
        VStack(spacing: 18) {
            Image(nsImage: Brand.appIcon).resizable().frame(width: 150, height: 150)
                .shadow(color: Brand.pink.opacity(0.4), radius: 24, y: 10)
            Text("Welcome to Mike's Game Room")
                .font(.system(size: 30, weight: .black, design: .rounded))
                .foregroundStyle(Brand.gradient)
            Text("Drag your games here, or add files and folders.\nSupports NUP, NSP, XCI, NCA, NSO, NRO and extracted ExeFS/RomFS folders.")
                .multilineTextAlignment(.center)
                .foregroundStyle(.secondary)
            Button(action: add) {
                Label("Add Games", systemImage: "plus.circle.fill").font(.headline).padding(.horizontal, 10).padding(.vertical, 4)
            }
            .buttonStyle(.borderedProminent)
            .tint(Brand.pink)
            .controlSize(.large)
        }
        .padding(40)
    }
}

struct GameArt: View {
    let info: GameInfo
    var corner: CGFloat = 18

    var body: some View {
        ZStack {
            if let data = info.icon, let img = NSImage(data: data) {
                Image(nsImage: img).resizable().aspectRatio(contentMode: .fill)
            } else {
                LinearGradient(colors: placeholderColors, startPoint: .topLeading, endPoint: .bottomTrailing)
                Text(initials)
                    .font(.system(size: 54, weight: .black, design: .rounded))
                    .foregroundStyle(.white.opacity(0.92))
                    .shadow(radius: 6)
            }
        }
        .aspectRatio(1, contentMode: .fit)
        .clipShape(RoundedRectangle(cornerRadius: corner, style: .continuous))
    }

    private var initials: String {
        let skip: Set<String> = ["the", "a", "an", "of", "and"]
        let words = info.name.split { !$0.isLetter && !$0.isNumber }.filter { !skip.contains($0.lowercased()) }.prefix(2)
        let s = words.compactMap { $0.first.map(String.init) }.joined()
        return s.isEmpty ? "?" : s.uppercased()
    }

    private var placeholderColors: [Color] {
        let palettes: [[Color]] = [
            [Brand.purple, Brand.pink], [Brand.pink, Brand.orange], [Color.teal, Brand.purple],
            [Color.blue, Color.teal], [Brand.orange, Color.yellow], [Color.indigo, Brand.pink]]
        let h = info.name.unicodeScalars.reduce(0) { ($0 &* 31 &+ Int($1.value)) & 0x7FFFFFFF }
        return palettes[h % palettes.count]
    }
}

struct PlatformBadge: View {
    let platform: Platform
    var body: some View {
        Text(platform.rawValue)
            .font(.system(size: 10, weight: .heavy, design: .rounded))
            .padding(.horizontal, 7).padding(.vertical, 3)
            .background(color.opacity(0.9), in: Capsule())
            .foregroundStyle(.white)
    }
    private var color: Color {
        switch platform {
        case .switch2: return Brand.pink
        case .switch1: return Color.red
        case .homebrew: return Color.green
        case .unknown: return Color.gray
        }
    }
}

struct GameCard: View {
    let info: GameInfo
    @State private var hover = false

    var body: some View {
        VStack(alignment: .leading, spacing: 8) {
            ZStack(alignment: .topLeading) {
                GameArt(info: info)
                PlatformBadge(platform: info.platform).padding(8)
                if !info.bootable {
                    Image(systemName: "exclamationmark.triangle.fill")
                        .foregroundStyle(.yellow)
                        .padding(6)
                        .background(.black.opacity(0.55), in: Circle())
                        .padding(8)
                        .frame(maxWidth: .infinity, alignment: .topTrailing)
                        .help(info.note.isEmpty ? "Not bootable" : info.note)
                }
            }
            .shadow(color: .black.opacity(hover ? 0.35 : 0.18), radius: hover ? 14 : 6, y: hover ? 8 : 3)
            .scaleEffect(hover ? 1.04 : 1)
            Text(info.name).font(.system(size: 13, weight: .semibold)).lineLimit(2)
            Text("\(info.format) · \(info.versionLabel)").font(.caption).foregroundStyle(.secondary)
        }
        .contentShape(Rectangle())
        .onHover { h in withAnimation(.spring(response: 0.25)) { hover = h } }
    }
}

struct GameDetailView: View {
    let entry: LibraryEntry
    var play: () -> Void
    @Environment(\.dismiss) private var dismiss

    var body: some View {
        let i = entry.info
        ScrollView {
        VStack(alignment: .leading, spacing: 18) {
            HStack(alignment: .top, spacing: 20) {
                GameArt(info: i, corner: 22).frame(width: 170, height: 170)
                VStack(alignment: .leading, spacing: 6) {
                    PlatformBadge(platform: i.platform)
                    Text(i.name).font(.system(size: 26, weight: .black, design: .rounded))
                    if !i.publisher.isEmpty { Text(i.publisher).foregroundStyle(.secondary) }
                    Spacer(minLength: 8)
                    HStack {
                        Button(action: play) {
                            Label("Play", systemImage: "play.fill").font(.headline).frame(minWidth: 90)
                        }
                        .buttonStyle(.borderedProminent).tint(Brand.pink).controlSize(.large)
                        .disabled(NativeEngine.shared.preparedApp(i) == nil)
                        Button("Close") { dismiss() }.controlSize(.large)
                    }
                }
            }
            NativeEnginePanel(info: i)
                .padding(12)
                .frame(maxWidth: .infinity, alignment: .leading)
                .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 12))
            if !i.note.isEmpty {
                Label(i.note, systemImage: "info.circle.fill")
                    .font(.callout)
                    .padding(10)
                    .frame(maxWidth: .infinity, alignment: .leading)
                    .background(Color.yellow.opacity(0.15), in: RoundedRectangle(cornerRadius: 10))
            }
            Grid(alignment: .leading, horizontalSpacing: 18, verticalSpacing: 7) {
                row("Title ID", i.titleIDString)
                row("Version", i.displayVersion.isEmpty ? "NCM v\(i.version)" : "\(i.displayVersion)  (NCM v\(i.version))")
                row("Format", i.format)
                row("Packages", "\(i.packageCount)" + (i.updateApplied ? "  · update applied" : ""))
                row("ExeFS", i.exefs.isEmpty ? "-" : i.exefs.joined(separator: ", "))
                row("RomFS", "\(i.romfsFiles) files")
                if !i.buildID.isEmpty { row("Build ID", i.buildID) }
                row("Location", i.path)
            }
            .font(.callout)
        }
        .padding(26)
        }
        .frame(width: 620)
        .frame(minHeight: 420, idealHeight: 720, maxHeight: 820)
    }

    @ViewBuilder private func row(_ k: String, _ v: String) -> some View {
        GridRow {
            Text(k).foregroundStyle(.secondary)
            Text(v).textSelection(.enabled).lineLimit(3).font(.system(.callout, design: k == "Build ID" || k == "Title ID" ? .monospaced : .default))
        }
    }
}
