import SwiftUI
import WebKit

// Signing in to Twitch, once, for both chat and the TV's player.
//
// Twitch's device-code sign-in: the app asks Twitch for a short code and opens twitch.tv/activate in a small
// in-app window. Logging in on that page signs the TV's player in (it shares the window's cookies), and approving
// the code gives the app a token for chat (scopes user:read:chat and user:write:chat). The app never sees the
// password. Tokens live in the Mac's keychain, are checked with Twitch every hour (Twitch requires that) and are
// refreshed when they expire. Uses the same developer key as Smart TV (Settings › Smart TV).

@MainActor
final class TwitchAccount: ObservableObject {
    static let shared = TwitchAccount()
    static let scopes = "user:read:chat user:write:chat"
    nonisolated static let safariAgent = "Mozilla/5.0 (Macintosh; Intel Mac OS X 10_15_7) AppleWebKit/605.1.15 (KHTML, like Gecko) Version/18.0 Safari/605.1.15"

    struct SignIn: Equatable { let userCode: String; let url: URL; var status: String }

    @Published private(set) var login: String?
    @Published private(set) var userID: String?
    @Published private(set) var webSignedIn = false
    @Published private(set) var signIn: SignIn?
    @Published var lastError: String?
    private(set) var pending = false

    var signedIn: Bool { userID != nil }
    var hasKey: Bool { clientID != nil }
    private var clientID: String? { Keychain.get("twitch.client") }
    private var secret: String? { Keychain.get("twitch.secret") }

    private var pollTask: Task<Void, Never>?
    private var hourly: Task<Void, Never>?
    private var refreshing: Task<Bool, Never>?

    private init() {
        checkWebCookies()
        Task { await self.restore() }
    }

    private func restore() async {
        if Keychain.get("twitch.user.access") != nil { _ = await validate() }
        hourly = Task { [weak self] in
            while !Task.isCancelled {
                try? await Task.sleep(nanoseconds: 3_600_000_000_000)
                guard let self else { return }
                if Keychain.get("twitch.user.access") != nil { _ = await self.validate() }
            }
        }
    }

    // MARK: tokens

    private struct TokenResponse: Decodable { let access_token: String; let refresh_token: String? }
    private struct DeviceCode: Decodable { let device_code: String; let user_code: String; let verification_uri: String; let expires_in: Int?; let interval: Int? }

    private func store(_ t: TokenResponse) {
        Keychain.set("twitch.user.access", t.access_token)
        if let r = t.refresh_token { Keychain.set("twitch.user.refresh", r) }
    }

    /// Asks Twitch who the token belongs to. Signs out locally if Twitch no longer accepts it.
    @discardableResult
    func validate(retry: Bool = true) async -> Bool {
        guard let tok = Keychain.get("twitch.user.access") else { clearUser(); return false }
        var r = URLRequest(url: URL(string: "https://id.twitch.tv/oauth2/validate")!)
        r.setValue("OAuth " + tok, forHTTPHeaderField: "Authorization")
        guard let res = try? await URLSession.shared.data(for: r) else { return signedIn }   // offline: keep what we have
        let code = (res.1 as? HTTPURLResponse)?.statusCode ?? 0
        if code == 401 {
            if retry, await refresh() { return await validate(retry: false) }
            Keychain.set("twitch.user.access", nil); Keychain.set("twitch.user.refresh", nil)
            clearUser()
            lastError = "Twitch signed you out. Sign in again to use chat."
            return false
        }
        struct V: Decodable { let login: String; let user_id: String }
        guard let v = try? JSONDecoder().decode(V.self, from: res.0) else { return signedIn }
        let wasOut = userID == nil
        login = v.login
        userID = v.user_id
        lastError = nil
        if wasOut { TwitchChat.shared.reconnect() }
        return true
    }

    func refresh() async -> Bool {
        if let r = refreshing { return await r.value }
        let t = Task<Bool, Never> { [weak self] in
            guard let self, let rt = Keychain.get("twitch.user.refresh"), let cid = self.clientID else { return false }
            var items = [("grant_type", "refresh_token"), ("refresh_token", rt), ("client_id", cid)]
            if let s = self.secret { items.append(("client_secret", s)) }
            guard let res = await self.postForm("https://id.twitch.tv/oauth2/token", items), res.1 == 200,
                  let tok = try? JSONDecoder().decode(TokenResponse.self, from: res.0) else { return false }
            self.store(tok)
            return true
        }
        refreshing = t
        let ok = await t.value
        refreshing = nil
        return ok
    }

    private func clearUser() {
        login = nil
        userID = nil
    }

    // MARK: sign in / out

    func beginSignIn() {
        lastError = nil
        guard let cid = clientID else {
            lastError = "Add your Twitch key in Settings › Smart TV first."
            return
        }
        pollTask?.cancel()
        signIn = nil
        pending = true
        pollTask = Task { [weak self] in
            guard let self else { return }
            defer { if !Task.isCancelled { self.pending = false } }
            guard let res = await self.postForm("https://id.twitch.tv/oauth2/device", [("client_id", cid), ("scopes", Self.scopes)]),
                  res.1 == 200,
                  let dc = try? JSONDecoder().decode(DeviceCode.self, from: res.0),
                  let url = URL(string: dc.verification_uri) else {
                self.lastError = "Twitch didn't start the sign-in. Check the key in Settings › Smart TV."
                return
            }
            self.signIn = SignIn(userCode: dc.user_code, url: url, status: "Log in on the page below, then press Authorize.")
            var interval = Double(max(1, dc.interval ?? 5))
            let deadline = Date().addingTimeInterval(Double(dc.expires_in ?? 1800))
            while !Task.isCancelled, Date() < deadline {
                try? await Task.sleep(nanoseconds: UInt64(interval * 1_000_000_000))
                if Task.isCancelled { return }
                var items = [("client_id", cid), ("scopes", Self.scopes), ("device_code", dc.device_code),
                             ("grant_type", "urn:ietf:params:oauth:grant-type:device_code")]
                if let s = self.secret { items.append(("client_secret", s)) }
                guard let r = await self.postForm("https://id.twitch.tv/oauth2/token", items) else { continue }
                if r.1 == 200, let tok = try? JSONDecoder().decode(TokenResponse.self, from: r.0) {
                    self.store(tok)
                    _ = await self.validate(retry: false)
                    self.signIn = nil
                    self.checkWebCookies()
                    ActivityLog.shared.log("Signed in to Twitch as \(self.login ?? "you")")
                    return
                }
                let body = String(data: r.0, encoding: .utf8) ?? ""
                if body.contains("authorization_pending") { continue }
                if body.contains("slow_down") { interval += 5; continue }
                self.signIn = nil
                self.lastError = "The Twitch sign-in was declined or ran out of time. Try again."
                return
            }
            if !Task.isCancelled {
                self.signIn = nil
                self.lastError = "The sign-in code expired. Try again."
            }
        }
    }

    func cancelSignIn() {
        pending = false
        pollTask?.cancel()
        pollTask = nil
        signIn = nil
    }

    func signOut() {
        if let tok = Keychain.get("twitch.user.access"), let cid = clientID {
            Task { _ = await self.postForm("https://id.twitch.tv/oauth2/revoke", [("client_id", cid), ("token", tok)]) }
        }
        Keychain.set("twitch.user.access", nil)
        Keychain.set("twitch.user.refresh", nil)
        clearUser()
        TwitchChat.shared.disconnect(clear: true)
        // sign the player out too
        let store = WKWebsiteDataStore.default()
        store.fetchDataRecords(ofTypes: WKWebsiteDataStore.allWebsiteDataTypes()) { records in
            let twitch = records.filter { $0.displayName.lowercased().contains("twitch") }
            store.removeData(ofTypes: WKWebsiteDataStore.allWebsiteDataTypes(), for: twitch) {
                Task { @MainActor in TwitchAccount.shared.checkWebCookies() }
            }
        }
        ActivityLog.shared.log("Signed out of Twitch")
    }

    /// Is the TV's player signed in? (Twitch's own login cookie in the app's web views.)
    func checkWebCookies() {
        WKWebsiteDataStore.default().httpCookieStore.getAllCookies { cookies in
            let on = cookies.contains { $0.name == "auth-token" && $0.domain.contains("twitch.tv") && !$0.value.isEmpty }
            Task { @MainActor in
                let a = TwitchAccount.shared
                if a.webSignedIn != on { a.webSignedIn = on }
            }
        }
    }

    // MARK: requests

    func postForm(_ url: String, _ items: [(String, String)]) async -> (Data, Int)? {
        var r = URLRequest(url: URL(string: url)!)
        r.httpMethod = "POST"
        r.setValue("application/x-www-form-urlencoded", forHTTPHeaderField: "Content-Type")
        var c = URLComponents()
        c.queryItems = items.map { URLQueryItem(name: $0.0, value: $0.1) }
        r.httpBody = c.percentEncodedQuery?.replacingOccurrences(of: "+", with: "%2B").data(using: .utf8)
        guard let res = try? await URLSession.shared.data(for: r) else { return nil }
        return (res.0, (res.1 as? HTTPURLResponse)?.statusCode ?? 0)
    }

    /// A Twitch API call as the signed-in user. Refreshes the token once if Twitch says it expired.
    func helix(_ method: String, _ path: String, query: [URLQueryItem] = [], body: [String: Any]? = nil, retry: Bool = true) async -> (Data, Int)? {
        guard let cid = clientID, let tok = Keychain.get("twitch.user.access") else { return nil }
        var c = URLComponents(string: "https://api.twitch.tv/helix/" + path)!
        if !query.isEmpty { c.queryItems = query }
        var r = URLRequest(url: c.url!)
        r.httpMethod = method
        r.setValue(cid, forHTTPHeaderField: "Client-Id")
        r.setValue("Bearer " + tok, forHTTPHeaderField: "Authorization")
        if let body {
            r.setValue("application/json", forHTTPHeaderField: "Content-Type")
            r.httpBody = try? JSONSerialization.data(withJSONObject: body)
        }
        guard let res = try? await URLSession.shared.data(for: r) else { return nil }
        let code = (res.1 as? HTTPURLResponse)?.statusCode ?? 0
        if code == 401, retry, await refresh() { return await helix(method, path, query: query, body: body, retry: false) }
        return (res.0, code)
    }
}

// MARK: sign-in window

struct TwitchSignInView: View {
    @ObservedObject private var account = TwitchAccount.shared

    var body: some View {
        VStack(spacing: 0) {
            HStack(spacing: 14) {
                Image(systemName: account.signedIn ? "checkmark.circle.fill" : "person.crop.circle")
                    .font(.system(size: 24))
                    .foregroundStyle(account.signedIn ? Orbit.mint : Color(rgb: 0x9146FF))
                VStack(alignment: .leading, spacing: 3) {
                    Text(account.signedIn ? "Signed in as \(account.login ?? "")" : "Sign in to Twitch").font(Orbit.body(16, 700))
                    Text(status).font(Orbit.body(12, 500)).foregroundStyle(Orbit.dim)
                }
                Spacer()
                if let code = account.signIn?.userCode {
                    VStack(spacing: 2) {
                        Text("YOUR CODE").font(Orbit.body(9, 700)).foregroundStyle(Orbit.mute)
                        Text(code).font(Orbit.display(20, 800)).tracking(3).textSelection(.enabled)
                    }
                    .padding(.horizontal, 14).padding(.vertical, 8)
                    .background(RoundedRectangle(cornerRadius: 12).fill(Orbit.bg))
                    .overlay(RoundedRectangle(cornerRadius: 12).strokeBorder(Color(rgb: 0x9146FF).opacity(0.6), lineWidth: 1))
                }
            }
            .padding(16)
            .background(Orbit.card)

            if let url = account.signIn?.url, !account.signedIn {
                SignInWeb(url: url)
                HStack {
                    Text("Page not working? You can sign in from Safari instead; chat will still work, the TV's player just won't be signed in.")
                        .font(Orbit.body(11, 500)).foregroundStyle(Orbit.mute)
                    Spacer()
                    Button("Open in Safari") { NSWorkspace.shared.open(url) }
                }
                .padding(10)
                .background(Orbit.card)
            } else {
                ZStack {
                    Orbit.bg
                    if !account.signedIn && account.lastError == nil { ProgressView() }
                }
            }
        }
        .frame(minWidth: 520, minHeight: 640)
        .foregroundStyle(Orbit.text)
        .preferredColorScheme(.dark)
        .onAppear { if account.signIn == nil && !account.signedIn && !account.pending { account.beginSignIn() } }
        .onChange(of: account.signedIn) { on in
            guard on else { return }
            DispatchQueue.main.asyncAfter(deadline: .now() + 1.5) {
                for w in NSApp.windows where w.title == "Sign in to Twitch" { w.close() }
            }
        }
        .onDisappear { if !account.signedIn { account.cancelSignIn() } }
    }

    private var status: String {
        if account.signedIn { return account.webSignedIn ? "Chat and the TV's player are signed in. This window will close." : "Chat is signed in. This window will close." }
        if let e = account.lastError { return e }
        return account.signIn?.status ?? "Asking Twitch for a code…"
    }
}

private struct SignInWeb: NSViewRepresentable {
    let url: URL
    func makeCoordinator() -> C { C() }
    func makeNSView(context: Context) -> WKWebView {
        let w = WKWebView(frame: .zero, configuration: WKWebViewConfiguration())   // default store: shared with the TV's player
        w.customUserAgent = TwitchAccount.safariAgent
        w.navigationDelegate = context.coordinator
        w.uiDelegate = context.coordinator
        w.load(URLRequest(url: url))
        context.coordinator.loaded = url
        return w
    }
    func updateNSView(_ w: WKWebView, context: Context) {
        if context.coordinator.loaded != url { context.coordinator.loaded = url; w.load(URLRequest(url: url)) }
    }
    final class C: NSObject, WKNavigationDelegate, WKUIDelegate {
        var loaded: URL?
        func webView(_ w: WKWebView, didFinish navigation: WKNavigation!) {
            Task { @MainActor in TwitchAccount.shared.checkWebCookies() }
        }
        // pop-ups (e.g. "log in with…") open in the same view
        func webView(_ w: WKWebView, createWebViewWith configuration: WKWebViewConfiguration,
                     for action: WKNavigationAction, windowFeatures: WKWindowFeatures) -> WKWebView? {
            if action.targetFrame == nil { w.load(action.request) }
            return nil
        }
    }
}

/// Settings row: who is signed in.
struct TwitchAccountRow: View {
    @ObservedObject private var account = TwitchAccount.shared
    @Environment(\.openWindow) private var openWindow

    var body: some View {
        VStack(alignment: .leading, spacing: 6) {
            HStack(spacing: 10) {
                Image(systemName: account.signedIn ? "person.crop.circle.fill.badge.checkmark" : "person.crop.circle")
                    .font(.system(size: 18))
                    .foregroundStyle(account.signedIn ? Orbit.mint : Orbit.mute)
                if account.signedIn {
                    Text("Signed in as \(account.login ?? "")").font(Orbit.body(14, 700))
                    Text(account.webSignedIn ? "chat + player" : "chat only").font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
                    Spacer()
                    if !account.webSignedIn {
                        Button("Sign in the player") { account.beginSignIn(); openWindow(id: "twitch-signin") }
                    }
                    Button("Sign out") { account.signOut() }
                } else {
                    Text("Twitch chat: not signed in").font(Orbit.body(14, 700))
                    Spacer()
                    Button("Sign in with Twitch") {
                        account.beginSignIn()
                        if account.hasKey { openWindow(id: "twitch-signin") }
                    }
                    .disabled(!account.hasKey)
                }
            }
            if let e = account.lastError {
                Text(e).font(Orbit.body(12, 600)).foregroundStyle(Orbit.peach)
            } else if !account.hasKey {
                Text("Save the Twitch key above first; sign-in uses it.").font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
            }
        }
    }
}
