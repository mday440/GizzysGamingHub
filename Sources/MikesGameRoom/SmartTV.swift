import Foundation
import SwiftUI
import WebKit
import Security

// Smart TV: when a game is selected, the TV can tune to someone streaming that game live. That needs a free Twitch
// developer key (a Client ID and Secret from dev.twitch.tv, kept in the Mac's keychain). Without one, the TV can open
// the game's Twitch category in the big TV, where any stream can be picked.

enum Keychain {
    static func get(_ account: String) -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "Gizzy's Gaming Hub",
                                kSecAttrAccount as String: account, kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var out: CFTypeRef?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }
    static func set(_ account: String, _ value: String?) {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: "Gizzy's Gaming Hub", kSecAttrAccount as String: account]
        SecItemDelete(base as CFDictionary)
        guard let value, !value.isEmpty else { return }
        var add = base; add[kSecValueData as String] = Data(value.utf8)
        SecItemAdd(add as CFDictionary, nil)
    }
}

/// The two Twitch API calls the TV needs (Helix: search a category, then its top live stream).
actor TwitchAPI {
    static let shared = TwitchAPI()
    private var token: (String, Date)?

    static var hasKey: Bool { Keychain.get("twitch.client") != nil && Keychain.get("twitch.secret") != nil }

    func forgetToken() { token = nil }

    private func appToken(client: String, secret: String) async throws -> String {
        if let t = token, t.1 > Date() { return t.0 }
        var c = URLComponents(string: "https://id.twitch.tv/oauth2/token")!
        c.queryItems = [URLQueryItem(name: "client_id", value: client), URLQueryItem(name: "client_secret", value: secret),
                        URLQueryItem(name: "grant_type", value: "client_credentials")]
        var r = URLRequest(url: c.url!); r.httpMethod = "POST"
        let (data, _) = try await URLSession.shared.data(for: r)
        struct T: Decodable { let access_token: String; let expires_in: Double }
        let t = try JSONDecoder().decode(T.self, from: data)
        token = (t.access_token, Date().addingTimeInterval(t.expires_in - 120))
        return t.access_token
    }

    private func get<T: Decodable>(_ path: String, _ items: [URLQueryItem], as: T.Type) async throws -> T {
        guard let client = Keychain.get("twitch.client"), let secret = Keychain.get("twitch.secret") else { throw URLError(.userAuthenticationRequired) }
        let tok = try await appToken(client: client, secret: secret)
        var c = URLComponents(string: "https://api.twitch.tv/helix/" + path)!
        c.queryItems = items
        var r = URLRequest(url: c.url!)
        r.setValue(client, forHTTPHeaderField: "Client-Id")
        r.setValue("Bearer " + tok, forHTTPHeaderField: "Authorization")
        let (data, resp) = try await URLSession.shared.data(for: r)
        if (resp as? HTTPURLResponse)?.statusCode == 401 { token = nil }
        return try JSONDecoder().decode(T.self, from: data)
    }

    struct Live: Decodable { let user_login: String; let user_name: String; let viewer_count: Int }

    /// The most-watched live stream of the game, or nil.
    func topStream(for gameName: String) async throws -> Live? {
        struct Cats: Decodable { struct C: Decodable { let id: String; let name: String }; let data: [C] }
        struct Streams: Decodable { let data: [Live] }
        let q = TwitchAPI.searchName(gameName)
        let cats = try await get("search/categories", [URLQueryItem(name: "query", value: q), URLQueryItem(name: "first", value: "5")], as: Cats.self)
        guard let cat = cats.data.first(where: { $0.name.caseInsensitiveCompare(q) == .orderedSame }) ?? cats.data.first else { return nil }
        let s = try await get("streams", [URLQueryItem(name: "game_id", value: cat.id), URLQueryItem(name: "type", value: "live"),
                                          URLQueryItem(name: "first", value: "1")], as: Streams.self)
        return s.data.first
    }

    /// "Super Mario Bros Wonders Switch 2 Edition" → "Super Mario Bros Wonders"
    nonisolated static func searchName(_ name: String) -> String {
        var n = name
        for tail in ["Nintendo Switch 2 Edition", "Switch 2 Edition", "– Nintendo Switch 2 Edition", "Switch 2"] {
            n = n.replacingOccurrences(of: tail, with: "", options: .caseInsensitive)
        }
        return n.trimmingCharacters(in: CharacterSet(charactersIn: " -–:"))
    }

    /// The game's Twitch category page (works without a key).
    nonisolated static func categoryURL(_ name: String) -> URL? {
        let slug = searchName(name).lowercased()
            .replacingOccurrences(of: "&", with: "and")
            .components(separatedBy: CharacterSet.alphanumerics.inverted).filter { !$0.isEmpty }.joined(separator: "-")
        return URL(string: "https://www.twitch.tv/directory/category/\(slug)")
    }
}

extension TwitchTV {
    /// Called when the selected game changes: tunes to a live stream of it when "Follow the game" is on and a key is set.
    func follow(_ e: LibraryEntry?) {
        guard followGame, let e, TwitchAPI.hasKey, NativeSessions.shared.running.isEmpty else { return }
        let name = e.info.name
        Task {
            do {
                if let live = try await TwitchAPI.shared.topStream(for: name) {
                    await MainActor.run {
                        guard TwitchTV.shared.channel != live.user_login else { return }
                        TwitchTV.shared.channel = live.user_login
                        ActivityLog.shared.log("TV: \(live.user_name) is playing \(TwitchAPI.searchName(name)) live")
                    }
                } else {
                    await MainActor.run { ActivityLog.shared.log("TV: nobody is streaming \(TwitchAPI.searchName(name)) right now") }
                }
            } catch {
                await MainActor.run { ActivityLog.shared.log("TV: could not reach Twitch (check the key in Settings)") }
            }
        }
    }

    /// Opens the game's Twitch category in the big TV.
    func browse(_ name: String) {
        bigURL = TwitchAPI.categoryURL(name)
    }
}

/// A plain web page (the big TV's category browser).
struct WebPageView: NSViewRepresentable {
    let url: URL
    func makeNSView(context: Context) -> WKWebView {
        let c = WKWebViewConfiguration()
        c.mediaTypesRequiringUserActionForPlayback = []
        let w = WKWebView(frame: .zero, configuration: c)
        w.load(URLRequest(url: url))
        return w
    }
    func updateNSView(_ w: WKWebView, context: Context) {
        if w.url?.host == nil || (w.url?.path.hasPrefix("/directory") == true && w.url != url && context.coordinator.last != url) {
            w.load(URLRequest(url: url))
        }
        context.coordinator.last = url
    }
    static func dismantleNSView(_ w: WKWebView, coordinator: C) {
        w.stopLoading()
        w.loadHTMLString("", baseURL: nil)
    }
    func makeCoordinator() -> C { C() }
    final class C { var last: URL? }
}

/// Settings card: the Twitch key and the follow switch.
struct SmartTVSettings: View {
    @ObservedObject private var tv = TwitchTV.shared
    @State private var client = Keychain.get("twitch.client") ?? ""
    @State private var secret = Keychain.get("twitch.secret") ?? ""
    @State private var status: String?

    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("TV follows the selected game", isOn: $tv.followGame).toggleStyle(.switch)
            Text("Tunes the TV to the most-watched live stream of the game you pick. Needs a free Twitch developer key: at dev.twitch.tv › Your Console › Register Your Application (any name, OAuth redirect http://localhost, category Application Integration), then copy its Client ID and a new Secret here. They stay in your Mac's keychain.")
                .font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
            HStack {
                TextField("Client ID", text: $client).textFieldStyle(.roundedBorder)
                SecureField("Client Secret", text: $secret).textFieldStyle(.roundedBorder)
                Button("Save key") {
                    Keychain.set("twitch.client", client.trimmingCharacters(in: .whitespacesAndNewlines))
                    Keychain.set("twitch.secret", secret.trimmingCharacters(in: .whitespacesAndNewlines))
                    Task { await TwitchAPI.shared.forgetToken() }
                    status = "Checking…"
                    Task {
                        do { _ = try await TwitchAPI.shared.topStream(for: "Mario Kart World"); await MainActor.run { status = "Key works." } }
                        catch { await MainActor.run { status = "Twitch did not accept that key." } }
                    }
                }
            }
            if let status { Text(status).font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint) }
            Text("Without a key, the TV's channel menu can still open the selected game's Twitch page in the big TV.")
                .font(Orbit.body(12, 500)).foregroundStyle(Orbit.mute)
            Divider().overlay(Orbit.line).padding(.vertical, 4)
            TwitchAccountRow()
        }
    }
}
