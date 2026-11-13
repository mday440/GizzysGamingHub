import Foundation
import SwiftUI
import MGRCore

/// A point-in-time view of the emulated CPU, passed from the CPU thread to the UI.
struct EmuSnapshot: Sendable {
    var status: UInt32
    var pc: UInt64
    var instructions: UInt64
    var svcCalls: UInt64
    var sp: UInt64
    var regs: [UInt64]
    var message: String
    var newLog: String
}

/// Owns the C game + emulator handles. All C calls happen on one serial queue.
final class EmuWorker: @unchecked Sendable {
    private var game: OpaquePointer?
    private var emu: OpaquePointer?
    private let queue = DispatchQueue(label: "mikesgameroom.cpu", qos: .userInteractive)
    private let lock = NSLock()
    private var _paused = false
    private var _shutdown = false
    var paused: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _paused }
        set { lock.lock(); _paused = newValue; lock.unlock() }
    }
    private var shutdown: Bool {
        get { lock.lock(); defer { lock.unlock() }; return _shutdown }
        set { lock.lock(); _shutdown = newValue; lock.unlock() }
    }

    /// Opens the game and builds the emulator. Calls back with an error string or nil.
    func boot(path: String, completion: @escaping @Sendable (String?, EmuSnapshot?) -> Void) {
        queue.async {
            self.teardown()
            var err = [CChar](repeating: 0, count: 256)
            guard let g = mgr_open(path, &err, err.count) else {
                completion(String(cString: err), nil); return
            }
            self.game = g
            guard let e = mgr_emu_create(g, &err, err.count) else {
                completion(String(cString: err), nil); return
            }
            self.emu = e
            completion(nil, self.snapshot())
        }
    }

    /// Runs the CPU in slices, reporting after each slice, until it stops, is paused or torn down.
    func run(sliceInstructions: UInt64 = 2_000_000, onUpdate: @escaping @Sendable (EmuSnapshot) -> Void) {
        paused = false
        queue.async {
            guard let e = self.emu else { return }
            while !self.paused && !self.shutdown {
                let st = mgr_emu_run(e, sliceInstructions)
                onUpdate(self.snapshot())
                if st != MGR_EMU_RUNNING { break }
            }
        }
    }

    func step(onUpdate: @escaping @Sendable (EmuSnapshot) -> Void) {
        queue.async {
            guard let e = self.emu else { return }
            _ = mgr_emu_run(e, 1)
            onUpdate(self.snapshot())
        }
    }

    private func snapshot() -> EmuSnapshot {
        guard let e = emu else {
            return EmuSnapshot(status: 0, pc: 0, instructions: 0, svcCalls: 0, sp: 0, regs: [], message: "", newLog: "")
        }
        var s = mgr_emu_state()
        mgr_emu_get_state(e, &s)
        var log = ""
        var buf = [CChar](repeating: 0, count: 65536)
        while true {
            let n = mgr_emu_take_log(e, &buf, buf.count)
            if n == 0 { break }
            log += String(cString: buf)
        }
        let regs = withUnsafeBytes(of: s.x) { Array($0.bindMemory(to: UInt64.self)) }
        return EmuSnapshot(status: s.status.rawValue, pc: s.pc, instructions: s.instructions, svcCalls: s.svc_calls,
                           sp: s.sp, regs: regs, message: cTupleString(s.message), newLog: log)
    }

    private func teardown() {
        if let e = emu { mgr_emu_destroy(e); emu = nil }
        if let g = game { mgr_close(g); game = nil }
    }

    func close() {
        shutdown = true
        queue.async { self.teardown() }
    }

    deinit {
        if let e = emu { mgr_emu_destroy(e) }
        if let g = game { mgr_close(g) }
    }
}

/// UI-facing emulator session for one game window.
@MainActor
final class EmulatorSession: ObservableObject {
    enum Phase: Equatable { case idle, booting, running, paused, stopped(String), failed(String) }

    @Published var phase: Phase = .idle
    @Published var log = ""
    @Published var instructions: UInt64 = 0
    @Published var svcCalls: UInt64 = 0
    @Published var pc: UInt64 = 0
    @Published var sp: UInt64 = 0
    @Published var regs: [UInt64] = []
    @Published var mips: Double = 0

    let path: String
    private let worker = EmuWorker()
    private var lastSample = Date()
    private var lastInstructions: UInt64 = 0

    init(path: String) { self.path = path }

    var statusTitle: String {
        switch phase {
        case .idle: return "Ready"
        case .booting: return "Booting..."
        case .running: return "Running"
        case .paused: return "Paused"
        case .stopped: return "Stopped"
        case .failed: return "Could not start"
        }
    }

    var statusDetail: String {
        switch phase {
        case .stopped(let m), .failed(let m): return m
        case .running: return "Executing game code on the AArch64 interpreter"
        case .paused: return "CPU paused"
        default: return ""
        }
    }

    func boot(autoRun: Bool = true) {
        phase = .booting
        log = ""
        worker.boot(path: path) { [weak self] err, snap in
            Task { @MainActor in
                guard let self else { return }
                if let err {
                    self.phase = .failed(err.isEmpty ? "Unknown error" : err)
                    return
                }
                if let snap { self.apply(snap) }
                self.phase = .paused
                if autoRun { self.resume() }
            }
        }
    }

    func resume() {
        guard phase == .paused else { return }
        phase = .running
        lastSample = Date()
        lastInstructions = instructions
        worker.run { [weak self] snap in
            Task { @MainActor in self?.apply(snap) }
        }
    }

    func pause() {
        guard phase == .running else { return }
        worker.paused = true
        phase = .paused
    }

    func step() {
        guard phase == .paused else { return }
        worker.step { [weak self] snap in Task { @MainActor in self?.apply(snap) } }
    }

    func restart() {
        worker.paused = true
        boot(autoRun: true)
    }

    func close() { worker.close() }

    private func apply(_ s: EmuSnapshot) {
        if !s.newLog.isEmpty {
            log += s.newLog
            if log.count > 400_000 { log = String(log.suffix(300_000)) }
        }
        let now = Date()
        let dt = now.timeIntervalSince(lastSample)
        if dt > 0.25 {
            mips = Double(s.instructions &- lastInstructions) / dt / 1_000_000
            lastSample = now
            lastInstructions = s.instructions
        }
        instructions = s.instructions
        svcCalls = s.svcCalls
        pc = s.pc
        sp = s.sp
        regs = s.regs
        switch s.status {
        case MGR_EMU_RUNNING.rawValue, MGR_EMU_READY.rawValue:
            break
        case MGR_EMU_EXITED.rawValue:
            phase = .stopped("The game exited normally. \(s.message)")
        case MGR_EMU_BLOCKED.rawValue:
            phase = .stopped(s.message)
        default:
            phase = .stopped(s.message)
        }
    }
}
