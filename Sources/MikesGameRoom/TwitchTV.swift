import SwiftUI
import WebKit

// The little TV in the corner of Gizzy's Gaming Hub: a Twitch channel in Twitch's own embedded player.
// It is unloaded while a game runs (a live stream costs memory and GPU time the game needs) and comes back after.
// The big TV is a window of its own; only one of the two plays at a time.

@MainActor
final class TwitchTV: ObservableObject {
    static let shared = TwitchTV()

    @Published var channel: String { didSet { UserDefaults.standard.set(channel, forKey: "tv.channel") } }
    @Published var on: Bool { didSet { UserDefaults.standard.set(on, forKey: "tv.on") } }
    @Published var muted = true
    @Published var bigOpen = false
    @Published var bigURL: URL?
    @Published var currentGame: String?
    @Published var followGame: Bool { didSet { UserDefaults.standard.set(followGame, forKey: "tv.follow") } }
    @Published private(set) var recent: [String]

    private init() {
        let d = UserDefaults.standard
        channel = d.string(forKey: "tv.channel") ?? "twitch"
        on = d.object(forKey: "tv.on") as? Bool ?? true
        recent = d.stringArray(forKey: "tv.recent") ?? []
        followGame = d.bool(forKey: "tv.follow")
    }

    /// Switches to a channel typed by the player ("name", "twitch.tv/name" or a full link).
    func tune(_ text: String) {
        var c = text.trimmingCharacters(in: .whitespacesAndNewlines).lowercased()
        if let r = c.range(of: "twitch.tv/") { c = String(c[r.upperBound...]) }
        c = String(c.prefix { $0.isLetter || $0.isNumber || $0 == "_" })
        guard !c.isEmpty else { return }
        channel = c
        recent.removeAll { $0 == c }
        recent.insert(c, at: 0)
        recent = Array(recent.prefix(6))
        UserDefaults.standard.set(recent, forKey: "tv.recent")
        ActivityLog.shared.log("TV tuned to \(c)")
    }
}

/// Twitch's embedded player for one channel. If the embed does not report ready within 10 s,
/// it falls back to Twitch's mobile page for the channel, whose player sits at the top.
struct TwitchPlayerView: NSViewRepresentable {
    let channel: String
    let muted: Bool
    /// Page zoom: the small TV draws the player at about twice its size and shrinks it, so the player's
    /// controls and slates keep their proportions on a tiny screen.
    var zoom: CGFloat = 1
    /// Signed in on twitch.tv in the app: load Twitch's player as the page itself, so it uses that sign-in.
    var signedIn = false

    func makeCoordinator() -> Coordinator { Coordinator() }

    func makeNSView(context: Context) -> WKWebView {
        let config = WKWebViewConfiguration()
        config.mediaTypesRequiringUserActionForPlayback = []
        config.userContentController.add(context.coordinator, name: "tv")
        // On Twitch's own pages (signed-in player, mobile fallback): report ready once video plays, and let the app mute it.
        config.userContentController.addUserScript(WKUserScript(source: """
        (function(){ if (location.hostname === 'localhost') return;
          window.gizzyMute = function(m){ document.querySelectorAll('video').forEach(function(v){ v.muted = m; }); };
          var n = 0, t = setInterval(function(){ var v = document.querySelector('video');
            if (v && v.readyState >= 2) { clearInterval(t); window.webkit.messageHandlers.tv.postMessage('ready'); }
            if (++n > 60) clearInterval(t); }, 250);
        })();
        """, injectionTime: .atDocumentEnd, forMainFrameOnly: true))
        let web = WKWebView(frame: .zero, configuration: config)
        web.setValue(false, forKey: "drawsBackground")
        web.underPageBackgroundColor = .black
        web.pageZoom = zoom
        context.coordinator.web = web
        context.coordinator.signedIn = signedIn
        context.coordinator.load(channel, muted: muted)
        return web
    }

    func updateNSView(_ web: WKWebView, context: Context) {
        let c = context.coordinator
        if c.channel != channel || c.signedIn != signedIn { c.signedIn = signedIn; c.load(channel, muted: muted) }
        else if c.muted != muted {
            c.muted = muted
            web.evaluateJavaScript("window.gizzyMute && window.gizzyMute(\(muted ? "true" : "false"))")
        }
    }

    static func dismantleNSView(_ web: WKWebView, coordinator: Coordinator) {
        coordinator.timer?.invalidate()
        web.stopLoading()
        web.loadHTMLString("", baseURL: nil)
        web.configuration.userContentController.removeScriptMessageHandler(forName: "tv")
    }

    final class Coordinator: NSObject, WKScriptMessageHandler {
        weak var web: WKWebView?
        var channel = ""
        var muted = true
        var ready = false
        var signedIn = false
        var timer: Timer?

        func load(_ channel: String, muted: Bool) {
            self.channel = channel; self.muted = muted; ready = false
            timer?.invalidate()
            if signedIn, let url = URL(string: "https://player.twitch.tv/?channel=\(channel)&parent=localhost&autoplay=true&muted=\(muted ? "true" : "false")") {
                web?.customUserAgent = TwitchAccount.safariAgent
                web?.load(URLRequest(url: url))
                startFallbackTimer()
                return
            }
            let html = """
            <!doctype html><html><head><meta name="viewport" content="width=device-width,initial-scale=1">
            <style>html,body{margin:0;height:100%;background:#000;overflow:hidden}#p{width:100%;height:100%}</style></head>
            <body><div id="p"></div>
            <script src="https://player.twitch.tv/js/embed/v1.js"></script>
            <script>
            var player = new Twitch.Player("p", {channel: "\(channel)", parent: ["localhost"], width: "100%", height: "100%", autoplay: true, muted: \(muted ? "true" : "false")});
            player.addEventListener(Twitch.Player.READY, function(){ window.webkit.messageHandlers.tv.postMessage("ready"); });
            window.gizzyMute = function(m){ player.setMuted(m); };
            </script></body></html>
            """
            web?.customUserAgent = nil
            web?.loadHTMLString(html, baseURL: URL(string: "https://localhost/"))
            startFallbackTimer()
        }

        private func startFallbackTimer() {
            timer = Timer.scheduledTimer(withTimeInterval: 10, repeats: false) { [weak self] _ in
                guard let self, !self.ready, let web = self.web,
                      let url = URL(string: "https://m.twitch.tv/\(self.channel)") else { return }
                web.customUserAgent = "Mozilla/5.0 (iPhone; CPU iPhone OS 18_0 like Mac OS X) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Mobile/15E148 Safari/604.1"
                web.load(URLRequest(url: url))
            }
        }

        func userContentController(_ c: WKUserContentController, didReceive message: WKScriptMessage) {
            if (message.body as? String) == "ready" { ready = true; timer?.invalidate() }
        }
    }
}

/// The small TV: antennas, a bezel, the picture and its controls. Sized for the left column.
struct MiniTV: View {
    @ObservedObject private var tv = TwitchTV.shared
    @ObservedObject private var sessions = NativeSessions.shared
    @ObservedObject private var account = TwitchAccount.shared
    @Environment(\.openWindow) private var openWindow
    @AppStorage("tv.miniChat") private var miniChat = true
    @State private var tuning = false
    @State private var typed = ""

    var body: some View {
        // chat sits beside the TV (not under it), so the TV stays low and clear of the sidebar's links
        HStack(alignment: .bottom, spacing: 12) {
            tvSet.frame(width: 400)
            if miniChat && tv.on && sessions.running.isEmpty {
                TwitchChatPanel(compact: true)
                    .frame(width: 330, height: 280)
                    .clipShape(RoundedRectangle(cornerRadius: 20))
                    .overlay(RoundedRectangle(cornerRadius: 20).strokeBorder(Orbit.line, lineWidth: 1))
            }
        }
    }

    private var tvSet: some View {
        VStack(spacing: 0) {
            // antennas
            ZStack {
                Capsule().fill(Orbit.lavender.opacity(0.7)).frame(width: 3, height: 40).rotationEffect(.degrees(-28)).offset(x: -18, y: 4)
                Capsule().fill(Orbit.lavender.opacity(0.7)).frame(width: 3, height: 40).rotationEffect(.degrees(28)).offset(x: 18, y: 4)
                Circle().fill(Orbit.lavender).frame(width: 11, height: 11).offset(y: 22)
            }
            .frame(height: 40)
            VStack(spacing: 10) {
                screen
                    .aspectRatio(16 / 9, contentMode: .fit)
                    .clipShape(RoundedRectangle(cornerRadius: 12))
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color.black.opacity(0.6), lineWidth: 1))
                controls
            }
            .padding(12)
            .background(RoundedRectangle(cornerRadius: 20).fill(Orbit.panel))
            .overlay(RoundedRectangle(cornerRadius: 20).strokeBorder(Orbit.line, lineWidth: 1))
        }
    }

    @ViewBuilder private var screen: some View {
        if !sessions.running.isEmpty {
            placeholder("pause.fill", "Paused while you play")
        } else if !tv.on {
            placeholder("power", "TV off")
        } else if tv.bigOpen {
            Button { tv.bringBack() } label: { placeholder("rectangle.on.rectangle", "On the big TV · click to bring back") }
                .buttonStyle(.plain)
                .help("Bring the picture back to the small TV")
        } else {
            TwitchPlayerView(channel: tv.channel, muted: tv.muted, zoom: 0.8, signedIn: account.webSignedIn)
        }
    }

    private func placeholder(_ symbol: String, _ text: String) -> some View {
        ZStack {
            Color.black
            VStack(spacing: 6) {
                Image(systemName: symbol).font(.system(size: 28))
                Text(text).font(Orbit.body(15, 700))
            }
            .foregroundStyle(Orbit.mute)
        }
    }

    private var controls: some View {
        HStack(spacing: 10) {
            Button { typed = ""; tuning = true } label: {
                HStack(spacing: 5) {
                    Circle().fill(tv.on && sessions.running.isEmpty ? Color(rgb: 0xFF5A5A) : Orbit.mute).frame(width: 8, height: 8)
                    Text(tv.channel).font(Orbit.body(15, 700)).lineLimit(1)
                }
            }
            .buttonStyle(.plain)
            .help("Change channel")
            .popover(isPresented: $tuning, arrowEdge: .trailing) { tuner }
            Spacer(minLength: 4)
            icon(tv.muted ? "speaker.slash.fill" : "speaker.wave.2.fill", tv.muted ? "Unmute" : "Mute") { tv.muted.toggle() }
            icon(miniChat ? "bubble.left.and.bubble.right.fill" : "bubble.left.and.bubble.right", miniChat ? "Hide chat" : "Show chat") { miniChat.toggle() }
            icon("arrow.up.left.and.arrow.down.right", "Big TV") { tv.bigOpen = true; openWindow(id: "tv") }
            icon("power", tv.on ? "Turn off" : "Turn on") { tv.on.toggle() }
        }
        .foregroundStyle(Orbit.dim)
    }

    private func icon(_ symbol: String, _ help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) { Image(systemName: symbol).font(.system(size: 16, weight: .semibold)).frame(width: 32, height: 32).contentShape(Rectangle()) }
            .buttonStyle(.plain)
            .help(help)
            .accessibilityLabel(help)
    }

    private var tuner: some View {
        VStack(alignment: .leading, spacing: 10) {
            Text("Twitch channel").font(Orbit.body(13, 700))
            HStack {
                TextField("channel name or link", text: $typed)
                    .textFieldStyle(.roundedBorder)
                    .frame(width: 200)
                    .onSubmit { tv.tune(typed); tv.on = true; tuning = false }
                Button("Watch") { tv.tune(typed); tv.on = true; tuning = false }
                    .disabled(typed.trimmingCharacters(in: .whitespaces).isEmpty)
            }
            if let game = tv.currentGame {
                Button("Browse \(TwitchAPI.searchName(game)) streams in the big TV") {
                    tv.browse(game); tuning = false; tv.bigOpen = true; openWindow(id: "tv")
                }
                .buttonStyle(.link)
            }
            if !tv.recent.isEmpty {
                Text("Recent").font(Orbit.body(12, 700)).foregroundStyle(.secondary)
                ForEach(tv.recent, id: \.self) { c in
                    Button(c) { tv.tune(c); tv.on = true; tuning = false }.buttonStyle(.link)
                }
            }
        }
        .padding(14)
    }
}

/// The big TV window.
struct BigTVView: View {
    @ObservedObject private var tv = TwitchTV.shared
    @ObservedObject private var sessions = NativeSessions.shared
    @ObservedObject private var account = TwitchAccount.shared
    @AppStorage("tv.bigChat") private var bigChat = true
    var body: some View {
        ZStack {
            Color.black
            // the player exists only while the window is open: a closed window's views can stay alive, and with them the stream
            if !tv.bigOpen {
                EmptyView()
            } else if sessions.running.isEmpty, let url = tv.bigURL {
                VStack(spacing: 0) {
                    HStack {
                        Button("‹ Back to \(tv.channel)") { tv.bigURL = nil }
                        Spacer()
                        Text("Click a stream, then use its name in the TV's channel menu to keep it on the small TV.").font(.caption).foregroundStyle(.secondary)
                    }
                    .padding(8)
                    WebPageView(url: url)
                }
            } else if sessions.running.isEmpty {
                HStack(spacing: 0) {
                    TwitchPlayerView(channel: tv.channel, muted: tv.muted, signedIn: account.webSignedIn)
                    if bigChat {
                        TwitchChatPanel(compact: false)
                            .frame(width: 340)
                            .overlay(alignment: .leading) { Rectangle().fill(Orbit.line).frame(width: 1) }
                    }
                }
            } else {
                Text("Paused while you play").font(Orbit.body(16, 700)).foregroundStyle(Orbit.mute)
            }
        }
        .frame(minWidth: 480, minHeight: 270)
        .background(WindowCloseWatcher { TwitchTV.shared.bigOpen = false })
        .onAppear { tv.bigOpen = true }
        .onDisappear { tv.bigOpen = false }
        .navigationTitle("Gizzy TV · \(tv.channel)")
        .toolbar {
            ToolbarItem {
                Button { bigChat.toggle() } label: {
                    Label(bigChat ? "Hide chat" : "Show chat", systemImage: bigChat ? "bubble.left.and.bubble.right.fill" : "bubble.left.and.bubble.right")
                }
                .help(bigChat ? "Hide chat" : "Show chat")
            }
        }
    }
}

/// Calls back when the window holding it closes (SwiftUI does not always end a closed window's views).
struct WindowCloseWatcher: NSViewRepresentable {
    let onClose: @MainActor () -> Void
    func makeNSView(context: Context) -> NSView { V(onClose: onClose) }
    func updateNSView(_ v: NSView, context: Context) {}
    final class V: NSView {
        let onClose: @MainActor () -> Void
        private var token: NSObjectProtocol?
        init(onClose: @escaping @MainActor () -> Void) { self.onClose = onClose; super.init(frame: .zero) }
        required init?(coder: NSCoder) { fatalError() }
        override func viewDidMoveToWindow() {
            super.viewDidMoveToWindow()
            if let token { NotificationCenter.default.removeObserver(token) }
            guard let w = window else { return }
            let cb = onClose
            token = NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: w, queue: .main) { _ in
                Task { @MainActor in cb() }
            }
        }
        deinit { if let token { NotificationCenter.default.removeObserver(token) } }
    }
}

extension TwitchTV {
    /// Closes the big TV window, so the picture comes back to the small TV.
    func bringBack() {
        for w in NSApp.windows where w.title.hasPrefix("Gizzy TV") { w.close() }
        bigOpen = false
    }
}
