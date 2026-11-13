import Foundation
import SwiftUI
import IOKit.ps
import UserNotifications

// Overnight preparation: games queued for "tonight" are prepared one at a time inside a night window, when the Mac
// has been idle for a while and nobody is playing. Preparing pauses while a game runs. When one finishes, a Mac
// notification (and, if set, a push to the ntfy app on the phone) says it is ready.

@MainActor
final class PrepQueue: ObservableObject {
    static let shared = PrepQueue()

    @Published private(set) var queue: [String] { didSet { UserDefaults.standard.set(queue, forKey: "prep.queue") } }   // game paths
    @Published var startHour: Int { didSet { UserDefaults.standard.set(startHour, forKey: "prep.start") } }
    @Published var endHour: Int { didSet { UserDefaults.standard.set(endHour, forKey: "prep.end") } }
    @Published var ntfyTopic: String { didSet { UserDefaults.standard.set(ntfyTopic, forKey: "prep.ntfy") } }
    @Published private(set) var current: String?
    @Published private(set) var status = ""

    private var timer: Timer?
    private var activity: NSObjectProtocol?

    private init() {
        let d = UserDefaults.standard
        queue = d.stringArray(forKey: "prep.queue") ?? []
        startHour = d.object(forKey: "prep.start") as? Int ?? 1
        endHour = d.object(forKey: "prep.end") as? Int ?? 7
        ntfyTopic = d.string(forKey: "prep.ntfy") ?? ""
    }

    func start() {
        guard timer == nil else { return }
        timer = Timer.scheduledTimer(withTimeInterval: 60, repeats: true) { _ in Task { @MainActor in PrepQueue.shared.tick() } }
        tick()
    }

    func add(_ info: GameInfo) {
        guard !queue.contains(info.path) else { return }
        queue.append(info.path)
        ActivityLog.shared.log("\(info.name) will be prepared tonight (\(hourText(startHour))–\(hourText(endHour)))")
        UNUserNotificationCenter.current().requestAuthorization(options: [.alert, .sound]) { _, _ in }
        tick()
    }
    func remove(_ path: String) { queue.removeAll { $0 == path } }
    func contains(_ info: GameInfo) -> Bool { queue.contains(info.path) }

    var inWindow: Bool {
        let h = Calendar.current.component(.hour, from: Date())
        return startHour <= endHour ? (h >= startHour && h < endHour) : (h >= startHour || h < endHour)
    }

    /// Seconds since the last keyboard or mouse input.
    private var idleSeconds: Double {
        CGEventSource.secondsSinceLastEventType(.combinedSessionState, eventType: CGEventType(rawValue: ~0)!)
    }

    private func tick() {
        let engine = NativeEngine.shared
        // the one in progress finished?
        if let cur = current, case .running? = engine.builds[cur]?.phase {
            status = "Preparing tonight"; return
        } else if let cur = current {
            current = nil
            let name = LibraryStore.shared?.entries.first { $0.info.path == cur }?.info.name ?? "A game"
            if case .finished? = engine.builds[cur]?.phase {
                queue.removeAll { $0 == cur }
                notify("\(name) is ready to play", "Prepared overnight by Gizzy's Gaming Hub.")
            } else {
                notify("\(name) could not be prepared", "Open Gizzy's Gaming Hub for the log.")
                queue.removeAll { $0 == cur }
            }
        }
        keepAwake(!queue.isEmpty && inWindow)
        guard let next = queue.first else { status = ""; return }
        guard inWindow else { status = "\(queue.count) queued for \(hourText(startHour))"; return }
        guard NativeSessions.shared.running.isEmpty else { status = "Waiting: a game is running"; return }
        guard idleSeconds > 600 else { status = "Waiting for the Mac to be idle"; return }
        guard let entry = LibraryStore.shared?.entries.first(where: { $0.info.path == next }) else { queue.removeFirst(); return }
        if engine.preparedApp(entry.info) != nil { queue.removeFirst(); return }
        current = next
        status = "Preparing \(entry.info.name)"
        engine.prepare(entry.info)
    }

    /// Keeps the Mac from idle sleep during the window while there is work queued.
    private func keepAwake(_ on: Bool) {
        if on, activity == nil {
            activity = ProcessInfo.processInfo.beginActivity(options: [.idleSystemSleepDisabled, .suddenTerminationDisabled], reason: "Preparing games overnight")
        } else if !on, let a = activity {
            ProcessInfo.processInfo.endActivity(a); activity = nil
        }
    }

    private func notify(_ title: String, _ body: String) {
        ActivityLog.shared.log(title)
        let c = UNMutableNotificationContent(); c.title = title; c.body = body; c.sound = .default
        UNUserNotificationCenter.current().add(UNNotificationRequest(identifier: UUID().uuidString, content: c, trigger: nil))
        let topic = ntfyTopic.trimmingCharacters(in: .whitespaces)
        guard !topic.isEmpty, let url = URL(string: "https://ntfy.sh/" + topic) else { return }
        var r = URLRequest(url: url); r.httpMethod = "POST"
        r.setValue(title, forHTTPHeaderField: "Title")
        r.httpBody = Data(body.utf8)
        URLSession.shared.dataTask(with: r).resume()
    }

    func hourText(_ h: Int) -> String {
        var c = DateComponents(); c.hour = h
        let d = Calendar.current.date(from: c) ?? Date()
        return d.formatted(date: .omitted, time: .shortened)
    }
}

// MARK: pausing a preparation while a game runs

extension NativeEngine {
    /// Stops (SIGSTOP) or continues (SIGCONT) every preparation and all its child tools.
    func pauseBuilds(_ pause: Bool) {
        let pids = runningBuildPIDs()
        guard !pids.isEmpty else { return }
        var all: [pid_t] = []
        for p in pids { all.append(p); all.append(contentsOf: ProcessTree.descendants(of: p)) }
        for p in all { kill(p, pause ? SIGSTOP : SIGCONT) }
        ActivityLog.shared.log(pause ? "Preparing paused while you play" : "Preparing resumed")
    }
}

enum ProcessTree {
    /// Every process below pid (children, grandchildren, …).
    static func descendants(of pid: pid_t) -> [pid_t] {
        var mib: [Int32] = [CTL_KERN, KERN_PROC, KERN_PROC_ALL, 0]
        var size = 0
        guard sysctl(&mib, 4, nil, &size, nil, 0) == 0 else { return [] }
        let count = size / MemoryLayout<kinfo_proc>.stride
        var procs = [kinfo_proc](repeating: kinfo_proc(), count: count + 16)
        size = procs.count * MemoryLayout<kinfo_proc>.stride
        guard sysctl(&mib, 4, &procs, &size, nil, 0) == 0 else { return [] }
        let n = size / MemoryLayout<kinfo_proc>.stride
        var children: [pid_t: [pid_t]] = [:]
        for p in procs.prefix(n) { children[p.kp_eproc.e_ppid, default: []].append(p.kp_proc.p_pid) }
        var out: [pid_t] = [], stack = children[pid] ?? []
        while let p = stack.popLast() { out.append(p); stack.append(contentsOf: children[p] ?? []) }
        return out
    }
}

/// Settings card for the overnight queue.
struct PrepQueueSettings: View {
    @ObservedObject private var q = PrepQueue.shared
    @EnvironmentObject var library: LibraryStore

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text("Prepare between")
                Picker("", selection: $q.startHour) { ForEach(0..<24, id: \.self) { Text(q.hourText($0)).tag($0) } }.frame(width: 110)
                Text("and")
                Picker("", selection: $q.endHour) { ForEach(0..<24, id: \.self) { Text(q.hourText($0)).tag($0) } }.frame(width: 110)
            }
            .font(Orbit.body(14, 500))
            Text("Games you mark “Prepare tonight” are prepared one at a time in this window, once the Mac has been idle for 10 minutes. Leave Gizzy's Gaming Hub open; it keeps the Mac awake for the work and pauses if you start playing.")
                .font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
            HStack {
                TextField("ntfy topic (optional)", text: $q.ntfyTopic).textFieldStyle(.roundedBorder).frame(width: 260)
                Text("Install the free ntfy app on your iPhone and subscribe to the same made-up topic name to get a push when a game is ready.")
                    .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
            }
            if q.queue.isEmpty {
                Text("Nothing queued.").font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute)
            } else {
                ForEach(q.queue, id: \.self) { p in
                    HStack {
                        Text(library.entries.first { $0.info.path == p }?.info.name ?? URL(fileURLWithPath: p).lastPathComponent).font(Orbit.body(14, 700))
                        Spacer()
                        Button("Remove") { q.remove(p) }.buttonStyle(.link)
                    }
                }
                if !q.status.isEmpty { Text(q.status).font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint) }
            }
        }
    }
}
