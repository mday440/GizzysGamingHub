import SwiftUI
import Darwin

// Discord status: "Playing The Legend of Zelda · in Gizzy's Gaming Hub" with the time played, through the Discord app's
// local Rich Presence connection (a Unix socket, $TMPDIR/discord-ipc-N). It needs the Application ID of a free Discord
// application named "Gizzy's Gaming Hub" (the name Discord shows), made at discord.com/developers/applications.
// Nothing goes over the internet from here: the Discord app on this Mac does that.

@MainActor
final class DiscordPresence: ObservableObject {
    static let shared = DiscordPresence()

    @Published var enabled: Bool {
        didSet { UserDefaults.standard.set(enabled, forKey: "discord.on"); enabled ? refresh() : ipc.clear() }
    }
    @Published var appID: String {
        didSet { UserDefaults.standard.set(appID.trimmingCharacters(in: .whitespaces), forKey: "discord.appid"); ipc.reset() }
    }
    @Published private(set) var status = ""

    private let ipc = DiscordIPC()
    private var playing: (name: String, since: Date)?
    private var keepAlive: Task<Void, Never>?

    private init() {
        enabled = UserDefaults.standard.bool(forKey: "discord.on")
        appID = UserDefaults.standard.string(forKey: "discord.appid") ?? ""
    }

    func gameStarted(_ name: String) {
        playing = (name, Date())
        refresh()
    }

    func gameEnded() {
        playing = nil
        refresh()
    }

    func refresh() {
        guard enabled else { return }
        // say it again every minute: Discord forgets the status when it restarts, and it may start after the hub
        if keepAlive == nil {
            keepAlive = Task { @MainActor [weak self] in
                while !Task.isCancelled {
                    try? await Task.sleep(nanoseconds: 60_000_000_000)
                    guard let self, self.enabled else { continue }
                    self.refresh()
                }
            }
        }
        let id = appID.trimmingCharacters(in: .whitespaces)
        guard id.count >= 15, id.allSatisfy(\.isNumber) else { status = "Add the Application ID below."; return }
        var activity: [String: Any] = ["assets": ["large_image": "gizzy", "large_text": "Gizzy's Gaming Hub"]]
        if let p = playing {
            activity["details"] = "Playing \(p.name)"
            activity["state"] = "in Gizzy's Gaming Hub"
            activity["timestamps"] = ["start": Int(p.since.timeIntervalSince1970)]
        } else {
            activity["details"] = "Browsing the game library"
            activity["state"] = "Gizzy's Gaming Hub"
        }
        ipc.setActivity(clientID: id, activity: activity) { ok, why in
            Task { @MainActor in DiscordPresence.shared.status = ok ? "Showing on Discord." : why }
        }
    }
}

/// The Rich Presence connection, on a queue of its own (the socket calls block).
final class DiscordIPC: @unchecked Sendable {
    private let queue = DispatchQueue(label: "gizzy.discord")
    private var fd: Int32 = -1
    private var client = ""

    func reset() { queue.async { self.disconnect() } }

    func clear() {
        queue.async {
            guard self.fd >= 0 else { return }
            _ = self.send(op: 1, ["cmd": "SET_ACTIVITY", "args": ["pid": Int(getpid())], "nonce": UUID().uuidString])
            _ = self.receive()
            self.disconnect()
        }
    }

    func setActivity(clientID: String, activity: [String: Any], done: @escaping @Sendable (Bool, String) -> Void) {
        let payload: [String: Any] = ["cmd": "SET_ACTIVITY", "args": ["pid": Int(getpid()), "activity": activity], "nonce": UUID().uuidString]
        queue.async {
            for attempt in 0..<2 {
                if self.fd < 0 || self.client != clientID {
                    self.disconnect()
                    guard self.connect() else { done(false, "Discord isn't running on this Mac."); return }
                    guard self.send(op: 0, ["v": 1, "client_id": clientID]),
                          let ready = self.receive(), (ready["evt"] as? String) == "READY" else {
                        self.disconnect()
                        done(false, "Discord didn't accept that Application ID.")
                        return
                    }
                    self.client = clientID
                }
                if self.send(op: 1, payload), let reply = self.receive() {
                    if (reply["evt"] as? String) == "ERROR" {
                        let msg = (reply["data"] as? [String: Any])?["message"] as? String ?? "an error"
                        done(false, "Discord said: \(msg)")
                    } else {
                        done(true, "")
                    }
                    return
                }
                self.disconnect()   // the connection went stale (Discord restarted): try once more
                if attempt == 1 { done(false, "Lost the connection to Discord.") }
            }
        }
    }

    private func connect() -> Bool {
        let tmp = ProcessInfo.processInfo.environment["TMPDIR"] ?? NSTemporaryDirectory()
        for i in 0..<10 {
            let path = (tmp as NSString).appendingPathComponent("discord-ipc-\(i)")
            let s = socket(AF_UNIX, SOCK_STREAM, 0)
            guard s >= 0 else { continue }
            var one: Int32 = 1
            setsockopt(s, SOL_SOCKET, SO_NOSIGPIPE, &one, socklen_t(MemoryLayout<Int32>.size))
            var tv = timeval(tv_sec: 3, tv_usec: 0)
            setsockopt(s, SOL_SOCKET, SO_RCVTIMEO, &tv, socklen_t(MemoryLayout<timeval>.size))
            var addr = sockaddr_un()
            addr.sun_family = sa_family_t(AF_UNIX)
            let bytes = Array(path.utf8.prefix(MemoryLayout.size(ofValue: addr.sun_path) - 1))
            withUnsafeMutableBytes(of: &addr.sun_path) { buf in
                for (k, b) in bytes.enumerated() { buf[k] = b }
                buf[bytes.count] = 0
            }
            let ok = withUnsafePointer(to: &addr) {
                $0.withMemoryRebound(to: sockaddr.self, capacity: 1) { Darwin.connect(s, $0, socklen_t(MemoryLayout<sockaddr_un>.size)) }
            }
            if ok == 0 { fd = s; return true }
            close(s)
        }
        return false
    }

    private func disconnect() {
        if fd >= 0 { close(fd) }
        fd = -1
        client = ""
    }

    private func send(op: Int32, _ json: [String: Any]) -> Bool {
        guard fd >= 0, let body = try? JSONSerialization.data(withJSONObject: json) else { return false }
        var header = Data()
        withUnsafeBytes(of: op.littleEndian) { header.append(contentsOf: $0) }
        withUnsafeBytes(of: Int32(body.count).littleEndian) { header.append(contentsOf: $0) }
        let frame = header + body
        return frame.withUnsafeBytes { raw -> Bool in
            var off = 0
            while off < frame.count {
                let n = write(fd, raw.baseAddress! + off, frame.count - off)
                if n <= 0 { return false }
                off += n
            }
            return true
        }
    }

    private func receive() -> [String: Any]? {
        guard fd >= 0, let head = readExactly(8) else { return nil }
        let len = head.withUnsafeBytes { Int(Int32(littleEndian: $0.load(fromByteOffset: 4, as: Int32.self))) }
        guard len > 0, len < 1 << 20, let body = readExactly(len) else { return nil }
        return (try? JSONSerialization.jsonObject(with: body)) as? [String: Any]
    }

    private func readExactly(_ n: Int) -> Data? {
        var out = Data(count: n)
        var got = 0
        let ok = out.withUnsafeMutableBytes { raw -> Bool in
            while got < n {
                let r = read(fd, raw.baseAddress! + got, n - got)
                if r <= 0 { return false }
                got += r
            }
            return true
        }
        return ok ? out : nil
    }
}

struct DiscordSettings: View {
    @ObservedObject private var d = DiscordPresence.shared

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("Show what I'm playing on Discord", isOn: $d.enabled).toggleStyle(.switch)
            Text("Discord shows \"Playing <game> · in Gizzy's Gaming Hub\" with the time played. One-time setup: at discord.com/developers/applications click New Application, name it Gizzy's Gaming Hub (that name appears on your profile), then copy its Application ID here. Optional: under Rich Presence › Art Assets, upload the Gizzy logo named gizzy. The Discord app needs to be open on this Mac.")
                .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
            HStack {
                TextField("Application ID", text: $d.appID).textFieldStyle(.roundedBorder).frame(maxWidth: 280)
                Button("Test") { d.refresh() }.disabled(!d.enabled)
            }
            if !d.status.isEmpty { Text(d.status).font(Orbit.body(12, 700)).foregroundStyle(d.status.hasPrefix("Showing") ? Orbit.mint : Orbit.peach) }
        }
        .font(Orbit.body(13))
    }
}
