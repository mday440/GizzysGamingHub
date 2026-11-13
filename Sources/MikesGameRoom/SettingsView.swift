import SwiftUI
import AppKit

struct SettingsView: View {
    @EnvironmentObject var library: LibraryStore
    @State private var importKeys = false
    @State private var addFolder = false

    var body: some View {
        TabView {
            NativeEngineSettings().tabItem { Label("Native Engine", systemImage: "bolt.fill") }
            keysTab.tabItem { Label("Keys", systemImage: "key.fill") }
            foldersTab.tabItem { Label("Game Folders", systemImage: "folder.fill") }
            aboutTab.tabItem { Label("About", systemImage: "info.circle.fill") }
        }
        .padding(20)
    }

    private var keysTab: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Console keys").font(.title3.bold())
            Text("Encrypted Switch formats (NSP, XCI, NCA) need keys dumped from your own console: prod.keys, and title.keys for digital titles. NUP files, extracted folders, NSO and NRO need no keys. Keys never leave this Mac.")
                .foregroundStyle(.secondary)
                .fixedSize(horizontal: false, vertical: true)
            HStack {
                Image(systemName: Core.hasHeaderKey ? "checkmark.seal.fill" : "xmark.seal")
                    .foregroundStyle(Core.hasHeaderKey ? Color.green : Color.orange)
                Text(library.keyStatus)
            }
            HStack {
                Button("Import Key Files...") { importKeys = true }
                Button("Open Keys Folder") { NSWorkspace.shared.open(LibraryStore.keysDir) }
            }
            Spacer()
        }
        .fileImporter(isPresented: $importKeys, allowedContentTypes: [.item], allowsMultipleSelection: true) { r in
            if case .success(let urls) = r { library.importKeys(urls) }
        }
    }

    private var foldersTab: some View {
        VStack(alignment: .leading, spacing: 12) {
            Text("Folders scanned for games").font(.title3.bold())
            List {
                ForEach(library.folders, id: \.self) { f in
                    HStack {
                        Image(systemName: "folder")
                        Text(f).lineLimit(1).truncationMode(.middle)
                        Spacer()
                        Button(role: .destructive) { library.removeFolder(f) } label: { Image(systemName: "minus.circle") }
                            .buttonStyle(.borderless)
                    }
                }
            }
            .frame(minHeight: 180)
            HStack {
                Button("Add Folder...") { addFolder = true }
                Button("Rescan Now") { library.rescanFolders() }.disabled(library.folders.isEmpty)
                if library.scanning { ProgressView().controlSize(.small) }
            }
        }
        .fileImporter(isPresented: $addFolder, allowedContentTypes: [.folder], allowsMultipleSelection: false) { r in
            if case .success(let urls) = r, let u = urls.first { library.addFolder(u) }
        }
    }

    private var aboutTab: some View {
        VStack(spacing: 12) {
            Image(nsImage: Brand.appIcon).resizable().frame(width: 110, height: 110)
            Text("Gizzy's Gaming Hub").font(Orbit.display(24, 800)).foregroundStyle(Brand.gradient)
            Text("Core \(Core.version)").foregroundStyle(.secondary)
            Text("Switch 1 & Switch 2 emulator for macOS.\nAArch64 interpreter · Horizon HLE kernel · NUP/NSP/XCI/NCA/NSO/NRO loaders.")
                .multilineTextAlignment(.center).font(.callout)
            Text("Native engine: nxnative by seedds (MIT licence), with Ryujinx's shader translator (MIT) and its other credited parts.").font(.caption).foregroundStyle(.tertiary).multilineTextAlignment(.center)
            Text("Uses Zstandard (BSD license) by Meta Platforms, Inc.").font(.caption).foregroundStyle(.tertiary)
            Text("Fonts: Unbounded and Manrope, SIL Open Font License 1.1.").font(.caption).foregroundStyle(.tertiary)
            Spacer()
        }
        .frame(maxWidth: .infinity)
    }
}
