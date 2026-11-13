import SwiftUI
import AppKit

struct PlayerView: View {
    let path: String
    @EnvironmentObject var library: LibraryStore
    @StateObject private var session: EmulatorSession
    @State private var showRegisters = false

    init(path: String) {
        self.path = path
        _session = StateObject(wrappedValue: EmulatorSession(path: path))
    }

    private var info: GameInfo? { library.entries.first { $0.info.path == path }?.info }

    var body: some View {
        VSplitView {
            VStack(spacing: 0) {
                screen
                controls
            }
            .frame(minHeight: 380)
            console
                .frame(minHeight: 160)
        }
        .navigationTitle(info?.name ?? (path as NSString).lastPathComponent)
        .onAppear { session.boot() }
        .onDisappear { session.close() }
    }

    // The "TV": shows status until the GPU backend can present frames.
    private var screen: some View {
        ZStack {
            RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color.black)
            RoundedRectangle(cornerRadius: 14, style: .continuous)
                .fill(RadialGradient(colors: [Brand.purple.opacity(0.35), .clear], center: .center, startRadius: 10, endRadius: 420))
            VStack(spacing: 14) {
                if let info {
                    GameArt(info: info, corner: 16).frame(width: 120, height: 120)
                        .shadow(color: Brand.pink.opacity(0.5), radius: 18)
                } else {
                    Image(nsImage: Brand.appIcon).resizable().frame(width: 110, height: 110)
                }
                Text(session.statusTitle)
                    .font(.system(size: 26, weight: .black, design: .rounded))
                    .foregroundStyle(Brand.gradient)
                if !session.statusDetail.isEmpty {
                    Text(session.statusDetail)
                        .font(.callout)
                        .foregroundStyle(.white.opacity(0.75))
                        .multilineTextAlignment(.center)
                        .frame(maxWidth: 560)
                        .textSelection(.enabled)
                }
                if session.phase == .running || session.phase == .booting {
                    ProgressView().controlSize(.small).tint(.white)
                }
                Text("Video output appears here once the GPU backend lands. CPU, loader and kernel run now.")
                    .font(.caption2)
                    .foregroundStyle(.white.opacity(0.4))
            }
            .padding(30)
        }
        .aspectRatio(16.0 / 9.0, contentMode: .fit)
        .padding([.horizontal, .top], 16)
    }

    private var controls: some View {
        HStack(spacing: 10) {
            switch session.phase {
            case .running:
                Button { session.pause() } label: { Label("Pause", systemImage: "pause.fill") }
            case .paused:
                Button { session.resume() } label: { Label("Resume", systemImage: "play.fill") }
                Button { session.step() } label: { Label("Step", systemImage: "forward.frame.fill") }
            default:
                EmptyView()
            }
            Button { session.restart() } label: { Label("Restart", systemImage: "arrow.counterclockwise") }
            Toggle(isOn: $showRegisters) { Label("Registers", systemImage: "cpu") }.toggleStyle(.button)
            Spacer()
            stat("Instructions", formatted(session.instructions))
            stat("Speed", String(format: "%.1f MIPS", session.mips))
            stat("SVCs", "\(session.svcCalls)")
            stat("PC", String(format: "%016llx", session.pc), mono: true)
        }
        .padding(.horizontal, 16).padding(.vertical, 10)
        .popover(isPresented: $showRegisters, arrowEdge: .top) { registers }
    }

    private func stat(_ k: String, _ v: String, mono: Bool = false) -> some View {
        VStack(alignment: .trailing, spacing: 1) {
            Text(k).font(.caption2).foregroundStyle(.secondary)
            Text(v).font(.system(size: 12, weight: .semibold, design: mono ? .monospaced : .rounded))
        }
    }

    private var registers: some View {
        let regs = session.regs
        return ScrollView {
            LazyVGrid(columns: [GridItem(.fixed(190)), GridItem(.fixed(190))], alignment: .leading, spacing: 4) {
                ForEach(0..<regs.count, id: \.self) { i in
                    Text(String(format: "x%-2ld %016llx", i, regs[i])).font(.system(size: 11, design: .monospaced))
                }
                Text(String(format: "sp  %016llx", session.sp)).font(.system(size: 11, design: .monospaced))
                Text(String(format: "pc  %016llx", session.pc)).font(.system(size: 11, design: .monospaced))
            }
            .padding(12)
        }
        .frame(width: 420, height: 380)
    }

    private var console: some View {
        VStack(spacing: 0) {
            HStack {
                Label("Console", systemImage: "terminal").font(.caption.bold())
                Spacer()
                Button("Copy") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(session.log, forType: .string)
                }
                .buttonStyle(.borderless).font(.caption)
            }
            .padding(.horizontal, 12).padding(.vertical, 6)
            Divider()
            ScrollViewReader { proxy in
                ScrollView {
                    Text(session.log.isEmpty ? " " : session.log)
                        .font(.system(size: 11, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                        .padding(10)
                    Color.clear.frame(height: 1).id("bottom")
                }
                .background(Color(nsColor: .textBackgroundColor))
                .onChange(of: session.log) { _ in proxy.scrollTo("bottom", anchor: .bottom) }
            }
        }
    }

    private func formatted(_ n: UInt64) -> String {
        let f = NumberFormatter()
        f.numberStyle = .decimal
        return f.string(from: NSNumber(value: n)) ?? "\(n)"
    }
}
