import SwiftUI
import GameController

// Controller navigation for the hub itself: pick a game, launch it, flip the TV's channels, open settings.
// It listens only while the hub is the app in front and no game is running (the game then has the controller).
//
//   ◀ ▶  (d-pad or left stick)  previous / next game        ▲ ▼  previous / next system (All, Favorites, …)
//   A / ✕  launch                 B / ○  back / close         X / □  game options      Y / △  favourite
//   L1 / R1  previous / next TV channel                      L2  TV sound on / off    R2  big TV on / off
//   Menu (Options)  settings      View (Create)  controllers

enum PadAction: Hashable {
    case left, right, up, down, accept, back, options, favorite, channelPrev, channelNext, mute, bigTV, settings, controllers
}

@MainActor
final class HubPad: ObservableObject {
    static let shared = HubPad()

    /// What the hub does with an action; set by the hub's window.
    var handler: ((PadAction) -> Void)?
    /// Whether the connected controller is a PlayStation one (button names in the hints).
    @Published private(set) var playStation = false
    @Published private(set) var connected = false

    private var loop: Task<Void, Never>?
    private var held: [PadAction: Date] = [:]        // pressed since; for the d-pad's repeat
    private var lastRepeat: [PadAction: Date] = [:]
    private var comboHeld = false

    private init() {}

    func start() {
        guard loop == nil else { return }
        GCController.shouldMonitorBackgroundEvents = true   // the quick menu's L3 + R3 while a game is in front
        loop = Task { @MainActor [weak self] in
            while !Task.isCancelled {
                self?.poll()
                try? await Task.sleep(nanoseconds: 16_000_000)
            }
        }
    }

    private func poll() {
        let pads = GCController.controllers().compactMap { $0.extendedGamepad }
        connected = !pads.isEmpty
        if let c = GCController.controllers().first(where: { $0.extendedGamepad != nil }) {
            let name = (c.productCategory + " " + (c.vendorName ?? "")).lowercased()
            let ps = name.contains("dualsense") || name.contains("dualshock") || name.contains("playstation")
            if ps != playStation { playStation = ps }
        }
        // the in-game quick menu: both sticks pressed together while a game runs
        let combo = pads.contains { ($0.leftThumbstickButton?.isPressed ?? false) && ($0.rightThumbstickButton?.isPressed ?? false) }
        if combo && !comboHeld && !NativeSessions.shared.running.isEmpty { QuickMenu.shared.toggle(); held.removeAll() }
        comboHeld = combo
        if combo { return }
        // only while the hub is in front and no game has the controller (or the quick menu is open: the game is paused)
        guard QuickMenu.shared.isOpen || (NSApp.isActive && NativeSessions.shared.running.isEmpty), NSApp.keyWindow?.title != "Sign in to Twitch" else {
            held.removeAll(); lastRepeat.removeAll()
            return
        }
        var now: Set<PadAction> = []
        for g in pads {
            let x = g.leftThumbstick.xAxis.value, y = g.leftThumbstick.yAxis.value
            if g.dpad.left.isPressed || x < -0.6 { now.insert(.left) }
            if g.dpad.right.isPressed || x > 0.6 { now.insert(.right) }
            if g.dpad.up.isPressed || y > 0.6 { now.insert(.up) }
            if g.dpad.down.isPressed || y < -0.6 { now.insert(.down) }
            if g.buttonA.isPressed { now.insert(.accept) }
            if g.buttonB.isPressed { now.insert(.back) }
            if g.buttonX.isPressed { now.insert(.options) }
            if g.buttonY.isPressed { now.insert(.favorite) }
            if g.leftShoulder.isPressed { now.insert(.channelPrev) }
            if g.rightShoulder.isPressed { now.insert(.channelNext) }
            if g.leftTrigger.value > 0.6 { now.insert(.mute) }
            if g.rightTrigger.value > 0.6 { now.insert(.bigTV) }
            if g.buttonMenu.isPressed { now.insert(.settings) }
            if g.buttonOptions?.isPressed == true { now.insert(.controllers) }
        }
        let t = Date()
        for a in now {
            if let since = held[a] {
                // directions repeat while held: after 0.4 s, every 0.14 s
                guard [.left, .right, .up, .down].contains(a), t.timeIntervalSince(since) > 0.4,
                      t.timeIntervalSince(lastRepeat[a] ?? since) > 0.14 else { continue }
                lastRepeat[a] = t
                emit(a)
            } else {
                held[a] = t
                lastRepeat[a] = nil
                emit(a)
            }
        }
        for a in held.keys where !now.contains(a) { held[a] = nil; lastRepeat[a] = nil }
    }
}

extension HubPad {
    fileprivate func emit(_ a: PadAction) {
        if QuickMenu.shared.isOpen { QuickMenu.shared.pad(a) } else { handler?(a) }
    }
}

/// The button hints along the bottom of the orbit while a controller is connected.
struct PadHints: View {
    @ObservedObject private var pad = HubPad.shared

    var body: some View {
        if pad.connected {
            let ps = pad.playStation
            HStack(spacing: 18) {
                hint("◀ ▶", "Game")
                hint("▲ ▼", "System")
                hint(ps ? "✕" : "A", "Launch")
                hint(ps ? "□" : "X", "Options")
                hint(ps ? "△" : "Y", "Favorite")
                hint(ps ? "L1 R1" : "LB RB", "TV channel")
                hint(ps ? "L2" : "LT", "TV sound")
                hint(ps ? "R2" : "RT", "Big TV")
                hint(ps ? "Options" : "Menu", "Settings")
            }
            .font(Orbit.body(12, 600))
            .foregroundStyle(Orbit.mute)
            .padding(.horizontal, 16).padding(.vertical, 8)
            .background(Capsule().fill(Orbit.panel.opacity(0.7)))
            .overlay(Capsule().strokeBorder(Orbit.line, lineWidth: 1))
        }
    }

    private func hint(_ key: String, _ label: String) -> some View {
        HStack(spacing: 5) {
            Text(key).font(Orbit.body(12, 800)).foregroundStyle(Orbit.lavender)
            Text(label)
        }
    }
}
