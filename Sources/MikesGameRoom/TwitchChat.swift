import SwiftUI
import Combine

// Live chat for whatever channel the TV is showing. Reads through Twitch's EventSub WebSocket
// (channel.chat.message), sends through the Send Chat Message API, as the signed-in account.
// Follows the TV: when the channel changes (by hand or by "TV follows the selected game"), chat changes with it.

@MainActor
final class TwitchChat: ObservableObject {
    static let shared = TwitchChat()

    struct Piece: Hashable {
        enum Kind: Hashable { case text(String), emote(url: String, name: String) }
        let kind: Kind
    }
    struct Message: Identifiable {
        let id: String
        let name: String
        let color: Color
        let badges: [String]
        let pieces: [Piece]
        let mine: Bool
    }

    @Published private(set) var messages: [Message] = []
    @Published private(set) var connected = false
    @Published private(set) var state = ""
    @Published var sendError: String?
    @Published private(set) var images: [String: NSImage] = [:]

    private(set) var channel: String?
    private var broadcasterID: String?
    private var socket: URLSessionWebSocketTask?
    private var generation = 0
    private var badgeURLs: [String: String] = [:]
    private var loading: Set<String> = []
    private var sink: AnyCancellable?

    private init() {
        sink = TwitchTV.shared.$channel.removeDuplicates().dropFirst().sink { [weak self] _ in
            Task { @MainActor in self?.reconnect() }
        }
        reconnect()
    }

    // MARK: connection

    func reconnect() {
        disconnect(clear: false)
        let ch = TwitchTV.shared.channel
        if channel != ch { messages = []; channel = ch; broadcasterID = nil; sendError = nil }
        guard TwitchAccount.shared.signedIn, TwitchAccount.shared.userID != nil else { state = "Not signed in"; return }
        generation += 1
        let gen = generation
        state = "Joining \(ch)…"
        Task { await self.start(channel: ch, gen: gen) }
    }

    func disconnect(clear: Bool) {
        generation += 1
        socket?.cancel(with: .goingAway, reason: nil)
        socket = nil
        connected = false
        if clear { messages = []; state = "Not signed in" }
    }

    private func start(channel ch: String, gen: Int) async {
        struct Users: Decodable { struct U: Decodable { let id: String; let login: String }; let data: [U] }
        guard let r = await TwitchAccount.shared.helix("GET", "users", query: [URLQueryItem(name: "login", value: ch)]) else {
            if gen == generation { state = "Can't reach Twitch"; retryLater(gen) }
            return
        }
        guard gen == generation else { return }
        guard r.1 == 200, let u = (try? JSONDecoder().decode(Users.self, from: r.0))?.data.first else {
            state = "Couldn't find \(ch) on Twitch"
            return
        }
        broadcasterID = u.id
        await loadBadges(broadcaster: u.id)
        guard gen == generation else { return }
        connect(URL(string: "wss://eventsub.wss.twitch.tv/ws")!, gen: gen, subscribe: true)
    }

    private func retryLater(_ gen: Int) {
        Task {
            try? await Task.sleep(nanoseconds: 5_000_000_000)
            if gen == self.generation { self.reconnect() }
        }
    }

    private func connect(_ url: URL, gen: Int, subscribe: Bool) {
        let ws = URLSession.shared.webSocketTask(with: url)
        socket = ws
        ws.resume()
        Task { await self.listen(ws, gen: gen, subscribe: subscribe) }
    }

    private func listen(_ ws: URLSessionWebSocketTask, gen: Int, subscribe: Bool) async {
        var needSubscribe = subscribe
        while gen == generation {
            let msg: URLSessionWebSocketTask.Message
            do { msg = try await ws.receive() } catch {
                if gen == generation, socket === ws {
                    connected = false
                    state = "Reconnecting…"
                    retryLater(gen)
                }
                return
            }
            var data: Data?
            switch msg {
            case .string(let s): data = s.data(using: .utf8)
            case .data(let d): data = d
            @unknown default: data = nil
            }
            guard let data,
                  let obj = (try? JSONSerialization.jsonObject(with: data)) as? [String: Any],
                  let meta = obj["metadata"] as? [String: Any],
                  let type = meta["message_type"] as? String else { continue }
            let payload = obj["payload"] as? [String: Any] ?? [:]
            switch type {
            case "session_welcome":
                if needSubscribe, let s = payload["session"] as? [String: Any], let sid = s["id"] as? String {
                    needSubscribe = false
                    await subscribeChat(session: sid, gen: gen)
                } else if gen == generation {
                    connected = true
                }
            case "session_reconnect":
                // Twitch is moving us to another server; subscriptions carry over
                if let s = payload["session"] as? [String: Any], let u = s["reconnect_url"] as? String, let url = URL(string: u) {
                    connect(url, gen: gen, subscribe: false)
                    let old = ws
                    Task { try? await Task.sleep(nanoseconds: 5_000_000_000); old.cancel(with: .goingAway, reason: nil) }
                    return
                }
            case "notification":
                if let ev = payload["event"] as? [String: Any] { add(ev) }
            case "revocation":
                connected = false
                state = "Twitch closed this chat feed"
            default:
                break   // session_keepalive
            }
        }
        ws.cancel(with: .goingAway, reason: nil)
    }

    private func subscribeChat(session: String, gen: Int) async {
        guard let b = broadcasterID, let me = TwitchAccount.shared.userID else { return }
        let body: [String: Any] = [
            "type": "channel.chat.message", "version": "1",
            "condition": ["broadcaster_user_id": b, "user_id": me],
            "transport": ["method": "websocket", "session_id": session]]
        let r = await TwitchAccount.shared.helix("POST", "eventsub/subscriptions", body: body)
        guard gen == generation else { return }
        if let r, r.1 == 202 || r.1 == 200 {
            connected = true
            state = "Live chat · \(channel ?? "")"
        } else {
            connected = false
            state = r?.1 == 401 || r?.1 == 403 ? "Twitch refused chat. Try signing in again." : "Couldn't join chat (\(r?.1 ?? 0))"
        }
    }

    // MARK: messages

    private func add(_ ev: [String: Any]) {
        let login = ev["chatter_user_login"] as? String ?? ""
        let name = ev["chatter_user_name"] as? String ?? login
        let id = ev["message_id"] as? String ?? UUID().uuidString
        let color = Self.color(hex: ev["color"] as? String ?? "") ?? Self.fallbackColor(login)
        var badges: [String] = []
        for b in ev["badges"] as? [[String: Any]] ?? [] {
            if let set = b["set_id"] as? String, let v = b["id"] as? String, let u = badgeURLs["\(set)/\(v)"] {
                badges.append(u)
                loadImage(u, height: 15)
            }
        }
        let message = ev["message"] as? [String: Any] ?? [:]
        var pieces: [Piece] = []
        for f in message["fragments"] as? [[String: Any]] ?? [] {
            let text = f["text"] as? String ?? ""
            if (f["type"] as? String) == "emote", let e = f["emote"] as? [String: Any], let eid = e["id"] as? String {
                let u = "https://static-cdn.jtvnw.net/emoticons/v2/\(eid)/static/dark/1.0"
                pieces.append(Piece(kind: .emote(url: u, name: text)))
                loadImage(u, height: 22)
            } else {
                pieces.append(Piece(kind: .text(text)))
            }
        }
        if pieces.isEmpty { pieces = [Piece(kind: .text(message["text"] as? String ?? ""))] }
        messages.append(Message(id: id, name: name, color: color, badges: badges, pieces: pieces,
                                mine: login == TwitchAccount.shared.login))
        if messages.count > 250 { messages.removeFirst(messages.count - 250) }
    }

    /// Posts into the channel's chat as the signed-in account. Returns true when Twitch accepted it.
    func send(_ text: String) async -> Bool {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return false }
        guard let b = broadcasterID, let me = TwitchAccount.shared.userID else {
            sendError = "Chat isn't connected yet."
            return false
        }
        sendError = nil
        guard let r = await TwitchAccount.shared.helix("POST", "chat/messages",
                                                       body: ["broadcaster_id": b, "sender_id": me, "message": String(t.prefix(500))]) else {
            sendError = "Couldn't reach Twitch."
            return false
        }
        struct R: Decodable {
            struct Drop: Decodable { let message: String? }
            struct D: Decodable { let is_sent: Bool; let drop_reason: Drop? }
            let data: [D]?
            let message: String?
        }
        let d = try? JSONDecoder().decode(R.self, from: r.0)
        if r.1 == 200, let first = d?.data?.first {
            if first.is_sent { return true }
            sendError = first.drop_reason?.message ?? "Twitch didn't post that message."
            return false
        }
        sendError = d?.message ?? "Twitch didn't post that message (\(r.1))."
        return false
    }

    // MARK: badges, emotes, colours

    private func loadBadges(broadcaster: String) async {
        struct B: Decodable {
            struct V: Decodable { let id: String; let image_url_1x: String }
            struct S: Decodable { let set_id: String; let versions: [V] }
            let data: [S]
        }
        var map: [String: String] = [:]
        let calls: [(String, [URLQueryItem])] = [("chat/badges/global", []),
                                                  ("chat/badges", [URLQueryItem(name: "broadcaster_id", value: broadcaster)])]
        for (path, q) in calls {
            if let r = await TwitchAccount.shared.helix("GET", path, query: q), r.1 == 200,
               let b = try? JSONDecoder().decode(B.self, from: r.0) {
                for s in b.data { for v in s.versions { map["\(s.set_id)/\(v.id)"] = v.image_url_1x } }
            }
        }
        badgeURLs = map
    }

    private func loadImage(_ u: String, height: CGFloat) {
        guard images[u] == nil, !loading.contains(u), let url = URL(string: u) else { return }
        loading.insert(u)
        Task {
            if let res = try? await URLSession.shared.data(from: url), let img = NSImage(data: res.0), img.size.height > 0 {
                img.size = NSSize(width: img.size.width * height / img.size.height, height: height)
                self.images[u] = img
            }
            self.loading.remove(u)
        }
    }

    static func color(hex: String) -> Color? {
        var h = hex.trimmingCharacters(in: .whitespaces)
        if h.hasPrefix("#") { h.removeFirst() }
        guard h.count == 6, let v = UInt32(h, radix: 16) else { return nil }
        // lift very dark name colours so they read on the night background
        let r = Double((v >> 16) & 0xFF) / 255, g = Double((v >> 8) & 0xFF) / 255, b = Double(v & 0xFF) / 255
        let lum = 0.2126 * r + 0.7152 * g + 0.0722 * b
        let lift = lum < 0.35 ? 0.35 - lum : 0
        return Color(red: min(1, r + lift), green: min(1, g + lift), blue: min(1, b + lift))
    }

    static func fallbackColor(_ login: String) -> Color {
        let palette: [UInt32] = [0xFF7A7A, 0x7CF0C5, 0x6FD6FF, 0xFFD36E, 0xE58BD8, 0x9BD36A, 0xB9A6FF, 0xFF9E7A]
        let h = login.unicodeScalars.reduce(UInt32(7)) { ($0 &* 31) &+ $1.value }
        return Color(rgb: palette[Int(h % UInt32(palette.count))])
    }
}

// MARK: the chat panel (big TV: full; mini TV: compact)

struct TwitchChatPanel: View {
    var compact: Bool
    @ObservedObject private var chat = TwitchChat.shared
    @ObservedObject private var account = TwitchAccount.shared
    @ObservedObject private var tv = TwitchTV.shared
    @Environment(\.openWindow) private var openWindow
    @State private var draft = ""
    @State private var sending = false
    @FocusState private var focused: Bool

    private var size: CGFloat { compact ? 12 : 14 }

    var body: some View {
        VStack(spacing: 0) {
            if !compact { header; Divider().overlay(Orbit.line) }
            if account.signedIn {
                list
                input
            } else {
                signInPrompt
            }
        }
        .background(Orbit.card)
        .foregroundStyle(Orbit.text)
    }

    private var header: some View {
        HStack(spacing: 8) {
            Image(systemName: "bubble.left.and.bubble.right.fill").foregroundStyle(Orbit.lavender)
            Text("Chat").font(Orbit.body(15, 700))
            Spacer()
            Circle().fill(chat.connected ? Orbit.mint : Orbit.mute).frame(width: 7, height: 7)
            Text(chat.connected ? tv.channel : chat.state).font(Orbit.body(12, 600)).foregroundStyle(Orbit.dim).lineLimit(1)
        }
        .padding(.horizontal, 12).padding(.vertical, 10)
    }

    private var list: some View {
        ScrollViewReader { proxy in
            ScrollView {
                LazyVStack(alignment: .leading, spacing: compact ? 3 : 6) {
                    ForEach(chat.messages) { m in row(m).id(m.id) }
                }
                .padding(.horizontal, compact ? 8 : 12)
                .padding(.vertical, compact ? 6 : 10)
            }
            .overlay {
                if chat.messages.isEmpty {
                    Text(chat.connected ? "Chat is quiet. Say hi to \(tv.channel)!" : chat.state)
                        .font(Orbit.body(size, 600)).foregroundStyle(Orbit.mute)
                        .multilineTextAlignment(.center).padding()
                }
            }
            .onChange(of: chat.messages.last?.id) { id in
                guard let id else { return }
                withAnimation(.easeOut(duration: 0.15)) { proxy.scrollTo(id, anchor: .bottom) }
            }
            .onAppear { if let id = chat.messages.last?.id { proxy.scrollTo(id, anchor: .bottom) } }
        }
    }

    private func row(_ m: TwitchChat.Message) -> some View {
        var t = Text("")
        for b in m.badges {
            if let img = chat.images[b] { t = t + Text(Image(nsImage: img)).baselineOffset(-2) + Text(" ") }
        }
        t = t + Text(m.name).font(Orbit.body(size, 800)).foregroundColor(m.color) + Text(": ").foregroundColor(Orbit.mute)
        for p in m.pieces {
            switch p.kind {
            case .text(let s): t = t + Text(s)
            case .emote(let u, let name):
                if let img = chat.images[u] { t = t + Text(Image(nsImage: img)).baselineOffset(-6) } else { t = t + Text(name) }
            }
        }
        return t
            .font(Orbit.body(size, 500))
            .foregroundColor(Orbit.text)
            .fixedSize(horizontal: false, vertical: true)
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(.vertical, m.mine ? 2 : 0)
            .padding(.horizontal, m.mine ? 4 : 0)
            .background(RoundedRectangle(cornerRadius: 6).fill(Orbit.lavender.opacity(m.mine ? 0.10 : 0)))
            .textSelection(.enabled)
    }

    private var input: some View {
        VStack(alignment: .leading, spacing: 4) {
            if let e = chat.sendError {
                Text(e).font(Orbit.body(11, 600)).foregroundStyle(Orbit.peach).lineLimit(2)
            }
            HStack(spacing: 8) {
                TextField(chat.connected ? "Chat as \(account.login ?? "you")" : "Chat is connecting…", text: $draft)
                    .textFieldStyle(.plain)
                    .font(Orbit.body(size, 500))
                    .padding(.horizontal, 10).padding(.vertical, compact ? 6 : 9)
                    .background(RoundedRectangle(cornerRadius: 10).fill(Orbit.bg))
                    .overlay(RoundedRectangle(cornerRadius: 10).strokeBorder(focused ? Orbit.lavender.opacity(0.7) : Orbit.line, lineWidth: 1))
                    .focused($focused)
                    .onSubmit(send)
                    .disabled(!chat.connected)
                Button(action: send) {
                    Image(systemName: "paperplane.fill").font(.system(size: compact ? 13 : 15, weight: .semibold))
                        .frame(width: compact ? 28 : 34, height: compact ? 28 : 34)
                        .background(Circle().fill(draft.isEmpty ? Orbit.line : Color(rgb: 0x9146FF)))
                }
                .buttonStyle(.plain)
                .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty || sending || !chat.connected)
                .help("Send to \(tv.channel)'s chat")
            }
        }
        .padding(compact ? 6 : 10)
    }

    private func send() {
        let text = draft
        guard !text.trimmingCharacters(in: .whitespaces).isEmpty, !sending else { return }
        sending = true
        Task {
            if await chat.send(text) { draft = "" }
            sending = false
            focused = true
        }
    }

    private var signInPrompt: some View {
        VStack(spacing: compact ? 6 : 10) {
            Image(systemName: "bubble.left.and.bubble.right").font(.system(size: compact ? 18 : 28)).foregroundStyle(Orbit.lavender)
            Text("Sign in with Twitch to read and join \(tv.channel)'s chat")
                .font(Orbit.body(size, 600)).foregroundStyle(Orbit.dim).multilineTextAlignment(.center)
            Button {
                account.beginSignIn()
                if account.hasKey { openWindow(id: "twitch-signin") }
            } label: {
                Text("Sign in with Twitch").font(Orbit.body(size, 700))
                    .padding(.horizontal, 14).padding(.vertical, compact ? 5 : 8)
                    .background(Capsule().fill(Color(rgb: 0x9146FF)))
            }
            .buttonStyle(.plain)
            if let e = account.lastError {
                Text(e).font(Orbit.body(11, 600)).foregroundStyle(Orbit.peach).multilineTextAlignment(.center)
            }
        }
        .padding(compact ? 8 : 16)
        .frame(maxWidth: .infinity, maxHeight: .infinity)
    }
}
