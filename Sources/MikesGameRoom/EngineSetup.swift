import SwiftUI
import AppKit

/// First-run setup: installs what the native engine needs (Resources/Setup/setup.sh) and shows each step.
@MainActor
final class EngineSetup: ObservableObject {
    static let shared = EngineSetup()

    enum StepState: Equatable { case waiting, running, done, failed(String) }
    struct Step: Identifiable { let id: Int; let title: String; var state: StepState = .waiting }

    @Published var steps: [Step] = [
        Step(id: 1, title: "Checking this Mac"),
        Step(id: 2, title: "Checking Xcode and the Metal compiler"),
        Step(id: 3, title: "Installing the native engine"),
        Step(id: 4, title: "Installing zstd and glslang (Homebrew)"),
        Step(id: 5, title: "Setting up Python and the engine's packages"),
        Step(id: 6, title: "Installing .NET (shader translator)"),
    ]
    @Published var running = false
    @Published var finished = false
    @Published var log: [String] = []

    /// Everything a preparation needs is in place.
    var ready: Bool { NativeEngine.shared.checkTools().allSatisfy { $0.ok } }

    func run() {
        guard !running, let script = Bundle.main.url(forResource: "setup", withExtension: "sh", subdirectory: "Setup") else {
            if Bundle.main.url(forResource: "setup", withExtension: "sh", subdirectory: "Setup") == nil { log.append("setup.sh is missing from the app") }
            return
        }
        let engine = NativeEngine.shared
        // a fresh install uses the engine copy in Application Support (the setup copies it out of the app)
        if HubPaths.bundledEngine != nil, !FileManager.default.fileExists(atPath: engine.enginePath + "/scripts/build_ipa_from_nsp.py") {
            engine.enginePath = HubPaths.installedEngine.path
        }
        for i in steps.indices { steps[i].state = .waiting }
        running = true; finished = false; log = []
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/bin/bash")
        p.arguments = [script.path]
        var env = ProcessInfo.processInfo.environment
        env["ENGINE_SRC"] = HubPaths.bundledEngine?.path ?? ""
        env["ENGINE_DST"] = engine.enginePath
        env["TOOLS"] = engine.toolsPath
        env["XCODE"] = engine.xcodePath
        p.environment = env
        let pipe = Pipe()
        p.standardOutput = pipe; p.standardError = pipe
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let data = h.availableData
            guard !data.isEmpty else { return }
            let text = String(decoding: data, as: UTF8.self)
            Task { @MainActor in self?.consume(text) }
        }
        p.terminationHandler = { [weak self] proc in
            let ok = proc.terminationStatus == 0
            Task { @MainActor in
                guard let self else { return }
                pipe.fileHandleForReading.readabilityHandler = nil
                self.running = false
                self.finished = ok && self.steps.allSatisfy { $0.state == .done }
            }
        }
        do { try p.run() } catch { running = false; log.append("could not start the setup: \(error.localizedDescription)") }
    }

    private var partial = ""
    private func consume(_ text: String) {
        partial += text
        while let nl = partial.firstIndex(of: "\n") {
            let line = String(partial[..<nl]); partial.removeSubrange(...nl)
            let parts = line.split(separator: " ", maxSplits: 2).map(String.init)
            if parts.count >= 2, let n = Int(parts[1]), let i = steps.firstIndex(where: { $0.id == n }) {
                switch parts[0] {
                case "STEP": steps[i].state = .running; continue
                case "OK": steps[i].state = .done; continue
                case "FAIL": steps[i].state = .failed(parts.count > 2 ? parts[2] : "failed"); continue
                default: break
                }
            }
            if line != "DONE" { log.append(line); if log.count > 400 { log.removeFirst(log.count - 400) } }
        }
    }
}

struct EngineSetupView: View {
    @ObservedObject private var setup = EngineSetup.shared
    private let close: () -> Void
    init(close: @escaping () -> Void) { self.close = close }

    var body: some View {
        VStack(alignment: .leading, spacing: 14) {
            Text("Set up the game engine").font(.title2.bold())
            Text("Gizzy's Gaming Hub prepares your own Switch games to run natively on this Mac. That needs a few free tools: Xcode (from the App Store), Homebrew, Python and .NET. This setup installs the rest for you and takes about 10–20 minutes the first time.")
                .foregroundStyle(.secondary).fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 12) {
                Link("Get Xcode (App Store)", destination: URL(string: "macappstore://apps.apple.com/app/id497799835")!)
                Link("Get Homebrew", destination: URL(string: "https://brew.sh")!)
            }.font(.callout)
            VStack(alignment: .leading, spacing: 8) {
                ForEach(setup.steps) { s in
                    HStack(alignment: .top, spacing: 8) {
                        icon(s.state).frame(width: 18)
                        VStack(alignment: .leading, spacing: 2) {
                            Text(s.title)
                            if case .failed(let why) = s.state { Text(why).font(.caption).foregroundStyle(.orange).fixedSize(horizontal: false, vertical: true) }
                        }
                    }
                }
            }
            ScrollViewReader { proxy in
                ScrollView {
                    VStack(alignment: .leading, spacing: 1) {
                        ForEach(Array(setup.log.enumerated()), id: \.offset) { i, l in Text(l).font(.system(size: 10, design: .monospaced)).id(i) }
                    }.frame(maxWidth: .infinity, alignment: .leading)
                }
                .frame(height: 110).padding(6).background(Color.black.opacity(0.25)).clipShape(RoundedRectangle(cornerRadius: 6))
                .onChange(of: setup.log.count) { n in proxy.scrollTo(n - 1, anchor: .bottom) }
            }
            HStack {
                if setup.finished { Label("All set: you can prepare games now.", systemImage: "checkmark.seal.fill").foregroundStyle(.green) }
                Spacer()
                Button(setup.finished ? "Done" : "Later") { close() }.disabled(setup.running)
                Button(setup.running ? "Working…" : "Run Setup") { setup.run() }
                    .keyboardShortcut(.defaultAction).disabled(setup.running || setup.finished)
            }
        }
        .padding(22).frame(width: 560)
    }

    @ViewBuilder private func icon(_ s: EngineSetup.StepState) -> some View {
        switch s {
        case .waiting: Image(systemName: "circle").foregroundStyle(.secondary)
        case .running: ProgressView().controlSize(.small)
        case .done: Image(systemName: "checkmark.circle.fill").foregroundStyle(.green)
        case .failed: Image(systemName: "exclamationmark.triangle.fill").foregroundStyle(.orange)
        }
    }
}
