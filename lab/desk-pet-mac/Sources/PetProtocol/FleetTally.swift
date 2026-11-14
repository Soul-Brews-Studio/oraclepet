import Foundation

/// One agent from `herdr agent list`.
public struct Agent: Equatable, Sendable {
    public var pane: String
    public var status: String        // working | blocked | idle | done | …
    public var label: String         // agent name, else the last path component of its cwd
    public var project: String       // the repo it works in (".../<repo>/wt/<branch>" -> <repo>)
    public var title: String         // its terminal title: what it is doing right now

    public init(pane: String, status: String, label: String, project: String = "", title: String = "") {
        self.pane = pane
        self.status = status
        self.label = label
        self.project = project.isEmpty ? label : project
        self.title = title
    }

    /// The repo behind a cwd. A worktree lives at <repo>/wt/<slug>, so the folder before "wt".
    public static func project(ofCwd cwd: String) -> String {
        let parts = cwd.split(separator: "/").map(String.init)
        if let i = parts.lastIndex(of: "wt"), i > 0 { return parts[i - 1] }
        return parts.last ?? ""
    }

    /// Decode the JSON that `herdr agent list` prints. Returns nil when it is not that shape.
    public static func parse(herdrList data: Data) -> [Agent]? {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let result = root["result"] as? [String: Any],
              let agents = result["agents"] as? [[String: Any]] else { return nil }
        return agents.map { a in
            let cwd = (a["cwd"] as? String) ?? ""
            let last = cwd.split(separator: "/").last.map(String.init) ?? ""
            let name = (a["name"] as? String).flatMap { $0.isEmpty ? nil : $0 }
            return Agent(pane: (a["pane_id"] as? String) ?? "?",
                         status: (a["agent_status"] as? String) ?? "",
                         label: name ?? (last.isEmpty ? ((a["pane_id"] as? String) ?? "?") : last),
                         project: project(ofCwd: cwd),
                         title: (a["terminal_title_stripped"] as? String) ?? "")
        }
    }
}

/// Turns successive fleet polls into heartbeat counts.
///
/// working -> running (busy), blocked -> waiting (attention). An agent counts toward `total`
/// while it is busy and for `quiet` seconds after, so a fleet that just finished shows idle,
/// and one that has been quiet that long shows sleep (total 0).
public struct FleetTally: Sendable {
    public var quiet: TimeInterval
    private var lastBusy: [String: Date] = [:]

    public init(quiet: TimeInterval = 300) { self.quiet = quiet }

    public struct Result: Equatable, Sendable {
        public var total: Int
        public var running: Int
        public var waiting: Int
        public var msg: String
        /// Short name for the pet's screen: who waits on you, else who is working ("" when nobody).
        public var who: String {
            let names = (waitingAgents.isEmpty ? workingAgents : waitingAgents).map { a -> String in
                a.project.hasSuffix("-oracle") ? String(a.project.dropLast(7)) : a.project
            }
            var seen = Set<String>()
            let unique = names.filter { seen.insert($0).inserted }
            guard let first = unique.first else { return "" }
            let more = unique.count > 1 ? " +\(unique.count - 1)" : ""
            return String(first.prefix(Heartbeat.whoLimit - more.count)) + more
        }
        public var waitingOn: [String]
        public var waitingAgents: [Agent]      // pane + label, for peeking at their terminals
        public var workingAgents: [Agent] = []
        public var agents: Int

        /// Who the peek opens on: the first agent waiting on you, else the first one working.
        public var peekable: [Agent] { waitingAgents.isEmpty ? workingAgents : waitingAgents }
    }

    public mutating func update(_ agents: [Agent], now: Date) -> Result {
        var running = 0, waiting = 0
        var waitingOn: [String] = []
        var waitingAgents: [Agent] = []
        var workingAgents: [Agent] = []
        for a in agents {
            switch a.status {
            case "working":
                running += 1
                workingAgents.append(a)
                lastBusy[a.pane] = now
            case "blocked":
                waiting += 1
                lastBusy[a.pane] = now
                waitingOn.append(a.label)
                waitingAgents.append(a)
            default:
                break
            }
        }
        // Forget panes that have closed, and count the ones busy recently.
        let open = Set(agents.map(\.pane))
        lastBusy = lastBusy.filter { open.contains($0.key) }
        let awake = lastBusy.values.filter { now.timeIntervalSince($0) < quiet }.count

        let msg: String
        if !waitingOn.isEmpty { msg = "wait: " + waitingOn.joined(separator: ", ") }
        else if running > 0 { msg = "\(running) working" }
        else { msg = awake > 0 ? "fleet idle" : "fleet asleep" }
        return Result(total: awake, running: running, waiting: waiting, msg: msg,
                      waitingOn: waitingOn, waitingAgents: waitingAgents, workingAgents: workingAgents,
                      agents: agents.count)
    }
}
