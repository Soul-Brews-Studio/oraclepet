import Foundation

/// The four states the jc3248 pet firmware derives from a heartbeat
/// (pet.cpp computeDesired; the same rule as the browser sim).
public enum PetState: String, CaseIterable, Sendable {
    case idle, busy, attention, sleep

    public var label: String {
        switch self {
        case .idle: return "Idle"
        case .busy: return "Busy"
        case .attention: return "Needs you"
        case .sleep: return "Asleep"
        }
    }

    /// waiting > 0 -> attention, running > 0 -> busy, total == 0 -> sleep, else idle.
    public init(total: Int, running: Int, waiting: Int) {
        if waiting > 0 { self = .attention }
        else if running > 0 { self = .busy }
        else if total == 0 { self = .sleep }
        else { self = .idle }
    }

    /// The smallest heartbeat counts that make the firmware show this state (manual mode).
    public var counts: (total: Int, running: Int, waiting: Int) {
        switch self {
        case .idle: return (1, 0, 0)
        case .busy: return (1, 1, 0)
        case .attention: return (1, 0, 1)
        case .sleep: return (0, 0, 0)
        }
    }
}

/// One line of the NUS RX protocol (ble.c parseLine). Sent as JSON + "\n".
public struct Heartbeat: Equatable, Sendable {
    public var total: Int
    public var running: Int
    public var waiting: Int
    public var msg: String
    public var epoch: Int
    public var tzoff: Int
    public var pct5h: Int?
    public var reset5h: Int?
    public var pct7d: Int?
    public var reset7d: Int?
    /// The oracle the pet names under the cat (who is waiting, else who is working). The firmware
    /// draws it at 16 px a glyph across 320 px, so at most `whoLimit` characters.
    public var who: String?

    /// The firmware copies msg into char[64]; keep it inside that, on a character boundary.
    public static let msgLimit = 63
    public static let whoLimit = 20

    public init(total: Int, running: Int, waiting: Int, msg: String,
                epoch: Int, tzoff: Int,
                pct5h: Int? = nil, reset5h: Int? = nil, pct7d: Int? = nil, reset7d: Int? = nil,
                who: String? = nil) {
        self.total = total
        self.running = running
        self.waiting = waiting
        self.msg = Heartbeat.clip(msg)
        self.epoch = epoch
        self.tzoff = tzoff
        self.pct5h = pct5h
        self.reset5h = reset5h
        self.pct7d = pct7d
        self.reset7d = reset7d
        self.who = who.map { String($0.prefix(Heartbeat.whoLimit)) }
    }

    public var state: PetState { PetState(total: total, running: running, waiting: waiting) }

    /// Clip to `msgLimit` UTF-8 bytes without splitting a character.
    static func clip(_ s: String) -> String {
        var out = ""
        for ch in s {
            if out.utf8.count + String(ch).utf8.count > msgLimit { break }
            out.append(ch)
        }
        return out
    }

    /// Stable key order, so a test can compare the exact bytes the pet receives.
    public func jsonLine() -> Data {
        var parts = [
            "\"total\":\(total)", "\"running\":\(running)", "\"waiting\":\(waiting)",
            "\"msg\":\(Heartbeat.quote(msg))", "\"epoch\":\(epoch)", "\"tzoff\":\(tzoff)",
        ]
        if let pct5h { parts.append("\"pct5h\":\(pct5h)") }
        if let reset5h { parts.append("\"reset5h\":\(reset5h)") }
        if let pct7d { parts.append("\"pct7d\":\(pct7d)") }
        if let reset7d { parts.append("\"reset7d\":\(reset7d)") }
        if let who { parts.append("\"who\":\(Heartbeat.quote(who))") }
        return Data(("{" + parts.joined(separator: ",") + "}\n").utf8)
    }

    static func quote(_ s: String) -> String {
        var out = "\""
        for u in s.unicodeScalars {
            switch u {
            case "\"": out += "\\\""
            case "\\": out += "\\\\"
            case "\n": out += "\\n"
            case "\t": out += "\\t"
            default:
                if u.value < 0x20 { out += String(format: "\\u%04x", u.value) } else { out.unicodeScalars.append(u) }
            }
        }
        return out + "\""
    }

    /// Local UTC offset in seconds for `date` (GMT+7 -> 25200), as the pet's clock expects.
    public static func tzoff(at date: Date, in zone: TimeZone = .current) -> Int {
        zone.secondsFromGMT(for: date)
    }
}

/// Claude Code rate limits, read from the statusline dump (/tmp/claude-statusline/raw.json),
/// the same file usage_bridge.py reads.
public struct Usage: Equatable, Sendable {
    public var pct5h: Int?
    public var reset5h: Int?
    public var pct7d: Int?
    public var reset7d: Int?

    public init(pct5h: Int? = nil, reset5h: Int? = nil, pct7d: Int? = nil, reset7d: Int? = nil) {
        self.pct5h = pct5h
        self.reset5h = reset5h
        self.pct7d = pct7d
        self.reset7d = reset7d
    }

    public static func parse(statusline data: Data, now: Date) -> Usage {
        guard let root = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let rl = root["rate_limits"] as? [String: Any] else { return Usage() }
        func window(_ key: String) -> (Int?, Int?) {
            guard let w = rl[key] as? [String: Any] else { return (nil, nil) }
            let pct = (w["used_percentage"] as? NSNumber)?.intValue
            let reset = (w["resets_at"] as? NSNumber).map { max(0, $0.intValue - Int(now.timeIntervalSince1970)) }
            return (pct, reset)
        }
        let (p5, r5) = window("five_hour")
        let (p7, r7) = window("seven_day")
        return Usage(pct5h: p5, reset5h: r5, pct7d: p7, reset7d: r7)
    }
}
