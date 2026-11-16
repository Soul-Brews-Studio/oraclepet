import CoreBluetooth
import Foundation

/// Every pet this Mac has seen, kept on disk: the Pet Firmware sidebar lists these instead of two
/// fixed slots, so 2 pets and 100 pets use the same window. Fed by
///   - a passive scan (advertising Claude-* pets: name, signal, last seen), only while a window asks;
///   - the links (`note(_:)`): firmware, slot, pack, project of whichever pet a link is on.
/// The two PetLinks stay the only connections; the registry never connects.
///
/// File: ~/Library/Application Support/Desk Pet/pets.json
public final class PetRegistry: NSObject, ObservableObject, CBCentralManagerDelegate {
    public static let shared = PetRegistry()

    public struct Pet: Codable, Identifiable, Equatable {
        public var id: String                 // CoreBluetooth identifier (per Mac)
        public var name: String               // Claude-XXXX as advertised
        public var label: String = ""         // what a person calls it ("Nat desk")
        public var project: String = ""       // board firmware project, "" until linked once
        public var version: String = ""       // firmware version, "" until linked once
        public var sha: String = ""
        public var slot: String = ""
        public var state: String = ""         // pending / valid
        public var pack: String = ""
        public var hasOTA: Bool? = nil        // nil = never linked; false = linked, no Pet OTA service
        public var rssi: Int? = nil
        public var lastSeen: Date = .distantPast
        public var lastLinked: Date? = nil

        public init(id: String, name: String) { self.id = id; self.name = name }

        /// Every field but id/name may be missing (older or newer pets.json): decode what is there.
        public init(from d: Decoder) throws {
            let c = try d.container(keyedBy: CodingKeys.self)
            id = try c.decode(String.self, forKey: .id)
            name = try c.decodeIfPresent(String.self, forKey: .name) ?? "Claude-????"
            label = try c.decodeIfPresent(String.self, forKey: .label) ?? ""
            project = try c.decodeIfPresent(String.self, forKey: .project) ?? ""
            version = try c.decodeIfPresent(String.self, forKey: .version) ?? ""
            sha = try c.decodeIfPresent(String.self, forKey: .sha) ?? ""
            slot = try c.decodeIfPresent(String.self, forKey: .slot) ?? ""
            state = try c.decodeIfPresent(String.self, forKey: .state) ?? ""
            pack = try c.decodeIfPresent(String.self, forKey: .pack) ?? ""
            hasOTA = try c.decodeIfPresent(Bool.self, forKey: .hasOTA)
            rssi = try c.decodeIfPresent(Int.self, forKey: .rssi)
            lastSeen = try c.decodeIfPresent(Date.self, forKey: .lastSeen) ?? .distantPast
            lastLinked = try c.decodeIfPresent(Date.self, forKey: .lastLinked)
        }

        public var uuid: UUID? { UUID(uuidString: id) }
        public var shortName: String { name.hasPrefix(PetLinkNames.prefix) ? String(name.dropFirst(PetLinkNames.prefix.count)) : name }
        public func seen(within s: TimeInterval, now: Date = Date()) -> Bool { now.timeIntervalSince(lastSeen) < s }
    }

    @Published public private(set) var pets: [Pet] = []
    /// A pet asked for (deskpet://select) before the Pet Firmware view was up: it picks it on appear.
    @Published public var requestedSelection: String?
    @Published public private(set) var scanning = false

    private var central: CBCentralManager?
    private let file: URL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Desk Pet/pets.json")
    private var saveScheduled = false

    public override init() {
        super.init()
        if let d = try? Data(contentsOf: file) {
            let dec = JSONDecoder(); dec.dateDecodingStrategy = .iso8601
            if let list = try? dec.decode([Pet].self, from: d) {
                pets = list
            } else {
                // unreadable: keep it aside instead of overwriting it on the next save
                let aside = file.deletingPathExtension().appendingPathExtension("unreadable-\(Int(Date().timeIntervalSince1970)).json")
                try? FileManager.default.moveItem(at: file, to: aside)
            }
        }
    }

    // MARK: lookup

    public func pet(_ id: UUID?) -> Pet? { id.flatMap { i in pets.first { $0.id == i.uuidString } } }

    /// "5610", "Claude-5610", a label, or a full identifier.
    public func find(_ key: String) -> Pet? {
        let k = key.lowercased()
        return pets.first { $0.id.lowercased() == k }
            ?? pets.first { $0.name.lowercased() == k || $0.shortName.lowercased() == k }
            ?? pets.first { !$0.label.isEmpty && $0.label.lowercased() == k }
            ?? pets.first { $0.name.lowercased().hasSuffix(k) }
    }

    // MARK: inputs

    /// What a link knows about the pet it is on. Call on every link change (state, firmware).
    private var linkedAt: [UUID: Date] = [:]

    public func note(_ link: PetLink) {
        guard let id = link.peripheralID else { return }
        if link.isLinked { if linkedAt[id] == nil { linkedAt[id] = Date() } } else { linkedAt[id] = nil }
        update(id: id, name: link.petName) { p in
            p.lastSeen = Date()
            if link.isLinked {
                p.lastLinked = Date()
                if let r = link.rssi { p.rssi = r }
                if let fw = link.firmware {
                    p.hasOTA = true
                    p.project = fw.project; p.version = fw.version; p.sha = fw.sha
                    p.slot = fw.slot; p.state = fw.state; p.pack = fw.pack
                } else if !link.isReady, p.hasOTA != true,
                          let since = linkedAt[id], Date().timeIntervalSince(since) > 5 {
                    p.hasOTA = false            // linked 5 s, still no Pet OTA service: old firmware
                }
            }
        }
    }

    public func setLabel(_ label: String, for id: String) {
        guard let i = pets.firstIndex(where: { $0.id == id }) else { return }
        pets[i].label = label.trimmingCharacters(in: .whitespaces)
        scheduleSave()
    }

    /// Drop a pet from the list (it comes back if it is seen again).
    public func remove(_ id: String) {
        pets.removeAll { $0.id == id }
        scheduleSave()
    }

    private func update(id: UUID, name: String?, _ change: (inout Pet) -> Void) {
        let key = id.uuidString
        if let i = pets.firstIndex(where: { $0.id == key }) {
            if let n = name, !n.isEmpty { pets[i].name = n }
            change(&pets[i])
        } else {
            var p = Pet(id: key, name: name ?? "Claude-????")
            change(&p)
            pets.append(p)
            pets.sort { $0.name < $1.name }
        }
        scheduleSave()
    }

    // MARK: passive scan (windows ask while they are open)

    /// On while the Pet Firmware window is open (AppDelegate: shown / will close). Idempotent.
    public func setScanning(_ on: Bool) {
        wantScan = on
        if on {
            if central == nil { central = CBCentralManager(delegate: self, queue: nil) }
            else if central?.state == .poweredOn, !(central?.isScanning ?? false) { beginScan() }
        } else if central?.isScanning == true {
            central?.stopScan(); scanning = false
        }
    }
    private var wantScan = false

    private func beginScan() {
        guard wantScan else { return }
        central?.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: true])
        scanning = true
    }

    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        if central.state == .poweredOn { beginScan() } else { scanning = false }
    }

    private var lastHeard: [UUID: Date] = [:]
    public func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? p.name ?? ""
        guard name.hasPrefix(PetLinkNames.prefix) else { return }
        let now = Date()
        if let t = lastHeard[p.identifier], now.timeIntervalSince(t) < 2 { return }   // ~1 update / 2 s per pet
        lastHeard[p.identifier] = now
        update(id: p.identifier, name: name) { pet in
            pet.lastSeen = now
            if RSSI.intValue != 127 { pet.rssi = RSSI.intValue }
        }
    }

    // MARK: disk

    private func scheduleSave() {
        guard !saveScheduled else { return }
        saveScheduled = true
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in
            guard let self else { return }
            self.saveScheduled = false
            let enc = JSONEncoder(); enc.dateEncodingStrategy = .iso8601; enc.outputFormatting = [.prettyPrinted, .sortedKeys]
            try? FileManager.default.createDirectory(at: self.file.deletingLastPathComponent(), withIntermediateDirectories: true)
            if let d = try? enc.encode(self.pets) { try? d.write(to: self.file, options: .atomic) }
        }
    }
}

/// The advertised-name prefix, shared with PetLink.
public enum PetLinkNames { public static let prefix = "Claude-" }
