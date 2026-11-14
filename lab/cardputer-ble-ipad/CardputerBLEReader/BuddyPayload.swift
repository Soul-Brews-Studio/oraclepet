import Foundation

struct BuddyPrompt: Codable, Equatable {
    var id: String
    var tool: String
    var hint: String
}

/// Host -> device heartbeat. The first seven fields are the exact
/// `buddy_protocol.py` heartbeat shape; the quota/time/brightness fields are
/// the JC ESP-IDF bridge extensions parsed by `lab/jc3248-pet-idf/main/ble.c`.
struct BuddyHeartbeat: Codable, Equatable {
    var total: Int
    var running: Int
    var waiting: Int
    var msg: String
    var entries: Int
    var tokens: Int
    var tokens_today: Int
    var epoch: Int
    var tzoff: Int
    var pct5h: Int?
    var reset5h: Int?
    var pct7d: Int?
    var reset7d: Int?
    var brightness: Int?
    var prompt: BuddyPrompt?

    static func sample(promptEnabled: Bool = false) -> BuddyHeartbeat {
        let now = Int(Date().timeIntervalSince1970)
        let offset = TimeZone.current.secondsFromGMT()
        return BuddyHeartbeat(
            total: 0,
            running: 0,
            waiting: promptEnabled ? 1 : 0,
            msg: "DRAFT TEST — no live Claude source",
            entries: 0,
            tokens: 0,
            tokens_today: 0,
            epoch: now,
            tzoff: offset,
            pct5h: 0,
            reset5h: 0,
            pct7d: 0,
            reset7d: 0,
            brightness: 180,
            prompt: promptEnabled ? BuddyPrompt(id: UUID().uuidString, tool: "Bash", hint: "approve test heartbeat") : nil
        )
    }

    func jsonLine() throws -> Data {
        let encoder = JSONEncoder()
        encoder.outputFormatting = [.sortedKeys]
        var data = try encoder.encode(self)
        data.append(0x0A)
        return data
    }
}

struct DecodedRow: Identifiable, Equatable {
    var key: String
    var value: String
    var id: String { key }
}

struct ProtocolFact: Identifiable, Equatable {
    var section: String
    var name: String
    var shape: String
    var note: String
    var id: String { "\(section):\(name):\(shape)" }
}

enum BuddyProtocolCatalog {
    static let facts: [ProtocolFact] = [
        ProtocolFact(section: "Transport", name: "Service", shape: "6e400001-b5a3-f393-e0a9-e50e24dcca9e", note: "Nordic UART Service"),
        ProtocolFact(section: "Transport", name: "RX write", shape: "6e400002-b5a3-f393-e0a9-e50e24dcca9e", note: "host/iPad → buddy JSON lines"),
        ProtocolFact(section: "Transport", name: "TX notify", shape: "6e400003-b5a3-f393-e0a9-e50e24dcca9e", note: "buddy → host/iPad JSON lines"),
        ProtocolFact(section: "Inbound", name: "heartbeat", shape: "total,running,waiting,msg,entries,tokens,tokens_today,prompt{id,tool,hint}", note: "exact buddy_protocol.py data; no cmd key"),
        ProtocolFact(section: "Inbound", name: "JC extras", shape: "epoch,tzoff,pct5h,reset5h,pct7d,reset7d,brightness", note: "extra fields parsed by JC ESP-IDF BLE bridge"),
        ProtocolFact(section: "Inbound", name: "status", shape: "{cmd:status}", note: "requests ack=status"),
        ProtocolFact(section: "Inbound", name: "name/owner", shape: "{cmd:name,name:...} / {cmd:owner,owner:...}", note: "rename identity band"),
        ProtocolFact(section: "Inbound", name: "unpair", shape: "{cmd:unpair}", note: "requires on-device confirmation"),
        ProtocolFact(section: "Inbound", name: "time tick", shape: "{time:epoch}", note: "recognized and dropped by Python protocol"),
        ProtocolFact(section: "Inbound", name: "chat event", shape: "{evt,role,content}", note: "recognized raw Claude event shape"),
        ProtocolFact(section: "Outbound", name: "hello", shape: "{cmd:hello,name,owner,version}", note: "sent after secure/dummy connection"),
        ProtocolFact(section: "Outbound", name: "permission", shape: "{cmd:permission,id,decision}", note: "decision is once or deny"),
        ProtocolFact(section: "Outbound", name: "status ack", shape: "{ack:status,name,owner,sec,bat{pct,mV,mA,usb},sys{up,heap},stats{appr,deny,vel,nap,lvl},version}", note: "all status data from buddy_protocol.py"),
        ProtocolFact(section: "Outbound", name: "generic ack", shape: "{ack:<cmd>,ok,...}", note: "name/owner/file/unpair responses")
    ]
}
