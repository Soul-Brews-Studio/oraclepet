import Combine
import Foundation
import PetProtocol

/// Polls `herdr agent list`, builds a heartbeat, and sends it to the pet every `period` seconds.
/// In manual mode the chosen state is sent instead of the fleet's. Runs on the main queue;
/// the herdr call itself runs on a background queue.
public final class FleetFeed: ObservableObject {
    public enum Mode: String, CaseIterable { case fleet, manual }

    @Published public var mode: Mode = .fleet { didSet { tick() } }
    @Published public var manualState: PetState = .idle { didSet { if mode == .manual { tick() } } }
    @Published public private(set) var result: FleetTally.Result?
    @Published public private(set) var herdrError: String?
    @Published public private(set) var lastHeartbeat: Heartbeat?
    @Published public private(set) var lastSent: Date?

    public var state: PetState { lastHeartbeat?.state ?? .sleep }

    public let link: PetLink
    /// More pets that get the same heartbeat (OraclePet's second pet).
    public var extraLinks: [PetLink] = []
    public var period: TimeInterval = 5
    public var statuslinePath = "/tmp/claude-statusline/raw.json"

    private var tally = FleetTally(quiet: 300)
    private var timer: Timer?
    private let work = DispatchQueue(label: "co.laris.deskpet.herdr")
    private lazy var herdrPath: String? = Self.findHerdr()

    public init(link: PetLink) {
        self.link = link
    }

    public func start() {
        tick()
        timer?.invalidate()
        timer = Timer.scheduledTimer(withTimeInterval: period, repeats: true) { [weak self] _ in self?.tick() }
        // the status file also follows the pets as they change (a say finished, LISTEN heard
        // something), so a script or the MCP server reads a result within ~0.3 s, not a tick
        changes = Publishers.MergeMany(([link] + extraLinks).map { $0.objectWillChange })
            .debounce(for: .milliseconds(300), scheduler: DispatchQueue.main)
            .sink { [weak self] _ in
                guard let self, let hb = self.lastHeartbeat else { return }
                self.writeStatus(hb, now: Date())
            }
    }
    private var changes: AnyCancellable?

    /// Poll (fleet mode), build the heartbeat, send it if the pet is linked.
    public func tick() {
        if mode == .manual {
            let c = manualState.counts
            emit(total: c.total, running: c.running, waiting: c.waiting, msg: "manual: \(manualState.rawValue)")
            return
        }
        guard let herdr = herdrPath else {
            herdrError = "herdr not found (looked in ~/.local/bin, /opt/homebrew/bin, /usr/local/bin)"
            return
        }
        work.async { [weak self] in
            let data = Self.run(herdr, ["agent", "list"])
            DispatchQueue.main.async {
                guard let self else { return }
                guard let data, let agents = Agent.parse(herdrList: data) else {
                    self.herdrError = "herdr agent list failed — is herdr running?"
                    return
                }
                self.herdrError = nil
                let r = self.tally.update(agents, now: Date())
                self.result = r
                self.announce(r)
                if self.mode == .fleet { self.emit(total: r.total, running: r.running, waiting: r.waiting, msg: r.msg, who: r.who) }
            }
        }
    }

    // MARK: Voice announcements (the pets speak; PetVoice + PetLink.say)

    /// UserDefaults: "voiceAnnounce" (Bool, default on), "voiceLanguage" ("th", the default, or "en").
    public static let announceKey = "voiceAnnounce"
    public static let languageKey = "voiceLanguage"
    private var waitingBefore: Set<String> = []
    private var spokeAt: [String: Date] = [:]

    /// Someone started waiting: every linked pet says "<oracle> needs you" (Thai: "<oracle> รอคุณอยู่").
    /// Once per oracle per 3 minutes; nothing on the first poll after launch.
    private func announce(_ r: FleetTally.Result) {
        let now = Date()
        let names = Set(r.waitingAgents.map { a in a.project.hasSuffix("-oracle") ? String(a.project.dropLast(7)) : a.project })
        defer { waitingBefore = names; primed = true }
        guard primed, UserDefaults.standard.object(forKey: Self.announceKey) as? Bool ?? true else { return }
        let fresh = names.subtracting(waitingBefore).filter { now.timeIntervalSince(spokeAt[$0] ?? .distantPast) > 180 }
        guard !fresh.isEmpty else { return }
        fresh.forEach { spokeAt[$0] = now }
        let list = fresh.sorted().joined(separator: ", ")
        let thai = UserDefaults.standard.string(forKey: Self.languageKey) != "en"
        let text = thai ? "\(list) รอคุณอยู่" : (fresh.count == 1 ? "\(list) needs you" : "\(list) need you")
        for l in [link] + extraLinks where l.isLinked { l.say(text, language: thai ? "th-TH" : "en-US") }
    }
    private var primed = false                // the first poll after launch only records who waits

    private func emit(total: Int, running: Int, waiting: Int, msg: String, who: String? = nil) {
        let now = Date()
        let usage = (try? Data(contentsOf: URL(fileURLWithPath: statuslinePath)))
            .map { Usage.parse(statusline: $0, now: now) } ?? Usage()
        let hb = Heartbeat(total: total, running: running, waiting: waiting, msg: msg,
                           epoch: Int(now.timeIntervalSince1970), tzoff: Heartbeat.tzoff(at: now),
                           pct5h: usage.pct5h, reset5h: usage.reset5h, pct7d: usage.pct7d, reset7d: usage.reset7d, who: who)
        lastHeartbeat = hb
        if link.send(hb.jsonLine()) { lastSent = now }
        for l in extraLinks { l.send(hb.jsonLine()) }
        writeStatus(hb, now: now)
    }

    /// `/tmp/deskpet-status.json`, rewritten every tick: lets a script or an agent check what the
    /// app is doing (link state, which pet, the last heartbeat and whether it was sent) without
    /// screen access or the unified log.
    public var statusPath = "/tmp/deskpet-status.json"
    private func writeStatus(_ hb: Heartbeat, now: Date) {
        let iso = ISO8601DateFormatter()
        var o: [String: Any] = [
            "at": iso.string(from: now),
            "pid": ProcessInfo.processInfo.processIdentifier,
            "mode": mode.rawValue,
            "link": "\(link.state)",
            "pet": link.petName ?? NSNull(),
            "state": hb.state.rawValue,
            "heartbeat": String(decoding: hb.jsonLine(), as: UTF8.self).trimmingCharacters(in: .newlines),
            "sentCount": link.sentCount,
            "tapCount": link.tapCount,
            "reacting": link.reacting,
        ]
        o["lastSent"] = lastSent.map { iso.string(from: $0) } ?? NSNull()
        o["lastError"] = link.lastError ?? NSNull()
        o["herdrError"] = herdrError ?? NSNull()
        o["firmware"] = link.firmware.map { "\($0.version) on \($0.slot), built \($0.built), sha \($0.sha), \($0.state)" } ?? NSNull()
        o["ota"] = link.ota == .idle ? NSNull() : link.ota.label
        o["pets"] = ([link] + extraLinks).map { l -> [String: Any] in
            ["name": l.petName ?? NSNull(), "link": "\(l.state)", "firmware": l.firmware.map { "\($0.version) on \($0.slot), \($0.project), sha \($0.sha), \($0.state)" } ?? NSNull(),
             "pack": l.firmware?.pack ?? NSNull(), "app": l.firmware?.app ?? NSNull(), "ota": l.ota == .idle ? NSNull() : l.ota.label,
             "speakerTest": l.speakerTest ?? NSNull(),
             "mic": l.micStatus ?? NSNull(),
             "micFreq": l.micLevels?.freq ?? NSNull(), "micDb": l.micLevels?.db ?? NSNull(),
             "micRms": l.micLevels?.rms ?? NSNull(), "micFloor": l.micLevels?.floor ?? NSNull(),
             "micWav": l.micRecording?.path ?? NSNull(), "micText": l.micText ?? NSNull(),
             "wifi": l.wifiState ?? NSNull(), "improv": l.improvState ?? NSNull(),
             "voice": l.voiceOn, "voiceState": l.voiceState ?? NSNull(), "wifiNets": l.wifiNetworks]
        }
        if let fw = link.firmware {
            o["pack"] = fw.pack
            o["packs"] = fw.packs
            o["app"] = fw.app
            o["apps"] = fw.apps
            o["appError"] = fw.appError
        }
        if let d = try? JSONSerialization.data(withJSONObject: o, options: [.prettyPrinted, .sortedKeys]) {
            try? d.write(to: URL(fileURLWithPath: statusPath), options: .atomic)
        }
    }

    /// A GUI app does not get the shell's PATH, so look in the usual places.
    static func findHerdr() -> String? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return ["\(home)/.local/bin/herdr", "/opt/homebrew/bin/herdr", "/usr/local/bin/herdr"]
            .first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    static func run(_ path: String, _ args: [String]) -> Data? {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: path)
        p.arguments = args
        let out = Pipe()
        p.standardOutput = out
        p.standardError = FileHandle.nullDevice
        do { try p.run() } catch { return nil }
        let data = out.fileHandleForReading.readDataToEndOfFile()
        p.waitUntilExit()
        return p.terminationStatus == 0 ? data : nil
    }
}
