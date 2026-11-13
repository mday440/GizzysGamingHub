import Foundation
import SwiftUI
import AppKit
import ScreenCaptureKit
import AVFoundation
import VideoToolbox
import Carbon.HIToolbox

// Instant replay: while a game runs, its window and sound are captured into a rolling 30-second memory buffer
// (video compressed by the Mac's media engine as it arrives). Control-Command-R saves the last 30 seconds as an MP4
// in the Replays folder on the work drive. Off by default; macOS asks once for Screen Recording permission.

@MainActor
final class ReplayRecorder: NSObject, ObservableObject, SCStreamDelegate {
    static let shared = ReplayRecorder()

    @Published var enabled: Bool {
        didSet { UserDefaults.standard.set(enabled, forKey: "replay.on"); if !enabled { stop() } }
    }
    @Published private(set) var recording = false
    @Published private(set) var status = ""
    @Published private(set) var lastClip: URL?

    let seconds: Double = 30
    private var stream: SCStream?
    private var engine: ReplayEngine?
    private var clipName = "Game"
    private var hotKey: EventHotKeyRef?
    private var menuHotKey: EventHotKeyRef?
    private var searching: Task<Void, Never>?

    var folder: URL { GizzyData.dir("Replays") }

    private override init() {
        enabled = UserDefaults.standard.bool(forKey: "replay.on")
        super.init()
    }

    // MARK: hotkey (Control-Command-R, system-wide)

    func installHotKey() {
        guard hotKey == nil else { return }
        var spec = EventTypeSpec(eventClass: OSType(kEventClassKeyboard), eventKind: UInt32(kEventHotKeyPressed))
        InstallEventHandler(GetApplicationEventTarget(), { _, event, _ in
            var hk = EventHotKeyID()
            GetEventParameter(event, EventParamName(kEventParamDirectObject), EventParamType(typeEventHotKeyID), nil,
                              MemoryLayout<EventHotKeyID>.size, nil, &hk)
            let which = hk.id
            Task { @MainActor in
                if which == 2 { QuickMenu.shared.toggle() } else { ReplayRecorder.shared.saveClip() }
            }
            return noErr
        }, 1, &spec, nil, nil)
        let id = EventHotKeyID(signature: OSType(0x475A_5259), id: 1)   // 'GZRY'
        RegisterEventHotKey(UInt32(kVK_ANSI_R), UInt32(cmdKey | controlKey), id, GetApplicationEventTarget(), 0, &hotKey)
        // Control-Command-G: the in-game quick menu
        let menuID = EventHotKeyID(signature: OSType(0x475A_5259), id: 2)
        RegisterEventHotKey(UInt32(kVK_ANSI_G), UInt32(cmdKey | controlKey), menuID, GetApplicationEventTarget(), 0, &menuHotKey)
    }

    // MARK: following the game

    func gameStarted(_ app: NativeApp, launcherPID: pid_t) {
        guard enabled else { return }
        clipName = LibraryStore.shared?.entries.first { $0.info.titleIDString.caseInsensitiveCompare(app.titleID) == .orderedSame }?.info.name ?? "Game"
        searching?.cancel()
        searching = Task { @MainActor in
            for _ in 0..<45 {
                if Task.isCancelled { return }
                if let w = await findWindow(launcherPID) { await start(w); return }
                try? await Task.sleep(nanoseconds: 2_000_000_000)
            }
            status = "Could not find the game's window to record"
        }
    }

    func gameEnded() {
        searching?.cancel()
        stop()   // the buffer stays, so the last 30 s can still be saved after quitting
    }

    /// Records this hub's own window for 8 seconds and saves it: a check that recording works.
    func test() {
        Task { @MainActor in
            do {
                let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
                let me = ProcessInfo.processInfo.processIdentifier
                guard let w = content.windows.filter({ $0.owningApplication?.processID == me && $0.frame.width > 300 }).max(by: { $0.frame.width < $1.frame.width }) else {
                    status = "No window to test with"; return
                }
                clipName = "Replay test"
                await start(w)
                status = "Testing: recording for 8 seconds…"
                try? await Task.sleep(nanoseconds: 8_000_000_000)
                saveClip()
                stop()
            } catch {
                status = "Screen Recording permission is needed: System Settings › Privacy & Security › Screen & System Audio Recording › turn on Gizzy's Gaming Hub, then reopen the hub."
            }
        }
    }

    private func findWindow(_ launcher: pid_t) async -> SCWindow? {
        do {
            let content = try await SCShareableContent.excludingDesktopWindows(false, onScreenWindowsOnly: true)
            let pids = Set(ProcessTree.descendants(of: launcher) + [launcher])
            return content.windows
                .filter { pids.contains($0.owningApplication?.processID ?? -1) && $0.frame.width > 320 && $0.frame.height > 200 }
                .max { $0.frame.width * $0.frame.height < $1.frame.width * $1.frame.height }
        } catch {
            status = "Screen Recording permission is needed: System Settings › Privacy & Security › Screen & System Audio Recording › turn on Gizzy's Gaming Hub, then reopen the hub."
            return nil
        }
    }

    private func start(_ window: SCWindow) async {
        stop()
        let filter = SCContentFilter(desktopIndependentWindow: window)
        let c = SCStreamConfiguration()
        let scale = NSScreen.screens.first(where: { $0.frame.intersects(window.frame) })?.backingScaleFactor ?? 2
        var w = window.frame.width * scale, h = window.frame.height * scale
        if w > 1920 { h = h * 1920 / w; w = 1920 }
        c.width = Int(w) & ~1; c.height = Int(h) & ~1
        c.minimumFrameInterval = CMTime(value: 1, timescale: 60)
        c.pixelFormat = kCVPixelFormatType_420YpCbCr8BiPlanarVideoRange
        c.queueDepth = 6
        c.showsCursor = false
        c.capturesAudio = true
        c.sampleRate = 48_000
        c.channelCount = 2
        c.excludesCurrentProcessAudio = true
        let eng = ReplayEngine(seconds: seconds)
        let s = SCStream(filter: filter, configuration: c, delegate: self)
        do {
            try s.addStreamOutput(eng, type: .screen, sampleHandlerQueue: eng.videoQueue)
            try s.addStreamOutput(eng, type: .audio, sampleHandlerQueue: eng.audioQueue)
            try await s.startCapture()
            stream = s; engine = eng; recording = true
            status = "Recording the last \(Int(seconds)) s · press ⌃⌘R to save a clip"
        } catch {
            status = "Recording could not start: \(error.localizedDescription)"
        }
    }

    private func stop() {
        if let s = stream { Task { try? await s.stopCapture() } }
        stream = nil
        recording = false
    }

    nonisolated func stream(_ stream: SCStream, didStopWithError error: Error) {
        Task { @MainActor in ReplayRecorder.shared.recording = false; ReplayRecorder.shared.status = "Recording stopped" }
    }

    func saveClip() {
        guard enabled || clipName == "Replay test", let engine else { NSSound.beep(); return }
        let f = DateFormatter(); f.dateFormat = "yyyy-MM-dd HH.mm.ss"
        let safe = clipName.replacingOccurrences(of: "/", with: "-").replacingOccurrences(of: ":", with: " -")
        let url = folder.appendingPathComponent("\(safe) \(f.string(from: Date())).mp4")
        engine.write(to: url) { ok in
            Task { @MainActor in
                let r = ReplayRecorder.shared
                if ok {
                    r.lastClip = url
                    NSSound(named: "Glass")?.play()
                    ActivityLog.shared.log("Saved a replay: \(url.lastPathComponent)")
                    r.status = "Saved \(url.lastPathComponent)"
                } else {
                    NSSound.beep()
                    r.status = "Nothing to save yet"
                }
            }
        }
    }
}

/// The rolling buffer: H.264 frames from the media engine, and the raw game sound, both trimmed to the last few seconds.
final class ReplayEngine: NSObject, SCStreamOutput, @unchecked Sendable {
    let seconds: Double
    let videoQueue = DispatchQueue(label: "gizzy.replay.video")
    let audioQueue = DispatchQueue(label: "gizzy.replay.audio")
    private let lock = NSLock()
    private var video: [CMSampleBuffer] = []
    private var audio: [CMSampleBuffer] = []
    private var session: VTCompressionSession?

    init(seconds: Double) { self.seconds = seconds }

    deinit { if let s = session { VTCompressionSessionInvalidate(s) } }

    func stream(_ stream: SCStream, didOutputSampleBuffer sb: CMSampleBuffer, of type: SCStreamOutputType) {
        guard sb.isValid else { return }
        switch type {
        case .screen:
            // only complete frames carry a picture
            guard let attach = (CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[SCStreamFrameInfo: Any]])?.first,
                  let raw = attach[.status] as? Int, SCFrameStatus(rawValue: raw) == .complete,
                  let pb = CMSampleBufferGetImageBuffer(sb) else { return }
            encode(pb, pts: CMSampleBufferGetPresentationTimeStamp(sb))
        case .audio:
            lock.lock(); audio.append(sb); trim(&audio); lock.unlock()
        default: break
        }
    }

    private func encode(_ pb: CVPixelBuffer, pts: CMTime) {
        if session == nil {
            var s: VTCompressionSession?
            VTCompressionSessionCreate(allocator: nil, width: Int32(CVPixelBufferGetWidth(pb)), height: Int32(CVPixelBufferGetHeight(pb)),
                                       codecType: kCMVideoCodecType_H264, encoderSpecification: nil, imageBufferAttributes: nil,
                                       compressedDataAllocator: nil, outputCallback: nil, refcon: nil, compressionSessionOut: &s)
            guard let s else { return }
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_RealTime, value: kCFBooleanTrue)
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_ProfileLevel, value: kVTProfileLevel_H264_High_AutoLevel)
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AllowFrameReordering, value: kCFBooleanFalse)
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_AverageBitRate, value: NSNumber(value: 12_000_000))
            VTSessionSetProperty(s, key: kVTCompressionPropertyKey_MaxKeyFrameIntervalDuration, value: NSNumber(value: 1))
            VTCompressionSessionPrepareToEncodeFrames(s)
            session = s
        }
        guard let s = session else { return }
        VTCompressionSessionEncodeFrame(s, imageBuffer: pb, presentationTimeStamp: pts, duration: .invalid,
                                        frameProperties: nil, infoFlagsOut: nil) { [weak self] status, _, out in
            guard status == noErr, let out, let self else { return }
            self.lock.lock(); self.video.append(out); self.trim(&self.video); self.lock.unlock()
        }
    }

    /// Drops buffers older than the window (plus a little, so a keyframe is always at hand).
    private func trim(_ list: inout [CMSampleBuffer]) {
        guard let last = list.last else { return }
        let cutoff = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(last)) - seconds - 2
        if let i = list.firstIndex(where: { CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp($0)) >= cutoff }), i > 0 {
            list.removeFirst(i)
        }
    }

    private static func isKeyframe(_ sb: CMSampleBuffer) -> Bool {
        guard let a = CMSampleBufferGetSampleAttachmentsArray(sb, createIfNecessary: false) as? [[CFString: Any]], let d = a.first else { return true }
        return (d[kCMSampleAttachmentKey_NotSync] as? Bool) != true
    }

    /// Writes the last `seconds` to an MP4: the video as it was encoded, the sound encoded to AAC.
    func write(to url: URL, done: @escaping @Sendable (Bool) -> Void) {
        lock.lock(); let v = video, a = audio; lock.unlock()
        guard let lastV = v.last else { done(false); return }
        let end = CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp(lastV))
        guard let startIndex = v.firstIndex(where: { Self.isKeyframe($0) && CMTimeGetSeconds(CMSampleBufferGetPresentationTimeStamp($0)) >= end - seconds })
                ?? v.firstIndex(where: { Self.isKeyframe($0) }) else { done(false); return }
        let vids = Array(v[startIndex...])
        let start = CMSampleBufferGetPresentationTimeStamp(vids[0])
        let auds = a.filter { CMTimeCompare(CMSampleBufferGetPresentationTimeStamp($0), start) >= 0 }

        guard let writer = try? AVAssetWriter(outputURL: url, fileType: .mp4) else { done(false); return }
        let vIn = AVAssetWriterInput(mediaType: .video, outputSettings: nil, sourceFormatHint: CMSampleBufferGetFormatDescription(vids[0]))
        vIn.expectsMediaDataInRealTime = false
        writer.add(vIn)
        var aIn: AVAssetWriterInput?
        if let first = auds.first {
            let i = AVAssetWriterInput(mediaType: .audio, outputSettings: [AVFormatIDKey: kAudioFormatMPEG4AAC, AVSampleRateKey: 48_000,
                                                                            AVNumberOfChannelsKey: 2, AVEncoderBitRateKey: 160_000],
                                       sourceFormatHint: CMSampleBufferGetFormatDescription(first))
            i.expectsMediaDataInRealTime = false
            if writer.canAdd(i) { writer.add(i); aIn = i }
        }
        guard writer.startWriting() else { done(false); return }
        writer.startSession(atSourceTime: start)
        let group = DispatchGroup()
        let q = DispatchQueue(label: "gizzy.replay.write")
        func feed(_ input: AVAssetWriterInput, _ list: [CMSampleBuffer], _ queue: DispatchQueue) {
            group.enter()
            var i = 0
            input.requestMediaDataWhenReady(on: queue) {
                while input.isReadyForMoreMediaData && i < list.count {
                    if !input.append(list[i]) { i = list.count; break }
                    i += 1
                }
                if i >= list.count { input.markAsFinished(); group.leave() }
            }
        }
        feed(vIn, vids, q)
        if let aIn { feed(aIn, auds, DispatchQueue(label: "gizzy.replay.write.audio")) }
        group.notify(queue: q) {
            writer.finishWriting { done(writer.status == .completed) }
        }
    }
}

/// Settings card for instant replay.
struct ReplaySettings: View {
    @ObservedObject private var r = ReplayRecorder.shared
    var body: some View {
        VStack(alignment: .leading, spacing: 10) {
            Toggle("Instant replay while playing", isOn: $r.enabled).toggleStyle(.switch)
            Text("Keeps the last 30 seconds of the game (picture and sound) in memory, about 60 MB, encoded by the Mac's media engine so it costs the game very little. Press Control-Command-R to save a clip. The first time, macOS asks for Screen Recording permission; turn it on for Gizzy's Gaming Hub and reopen the hub.")
                .font(Orbit.body(13, 500)).foregroundStyle(Orbit.mute).fixedSize(horizontal: false, vertical: true)
            HStack {
                Button("Test on this window") { r.test() }
                Button("Open replays folder") { NSWorkspace.shared.open(r.folder) }
                if let clip = r.lastClip { Button("Play last clip") { NSWorkspace.shared.open(clip) } }
            }
            if !r.status.isEmpty { Text(r.status).font(Orbit.body(13, 700)).foregroundStyle(Orbit.mint).fixedSize(horizontal: false, vertical: true) }
        }
    }
}
