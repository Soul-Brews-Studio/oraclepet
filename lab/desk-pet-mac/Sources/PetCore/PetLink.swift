import CoreBluetooth
import Foundation
import os
import PetProtocol

let petLog = Logger(subsystem: "co.laris.deskpet", category: "link")

/// Nordic UART Service on the jc3248 pet (lab/jc3248-pet-idf/main/ble.c).
public enum PetGATT {
    public static let service = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    public static let rx      = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")   // write: Mac -> pet
    public static let tx      = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")   // notify: pet -> Mac
    /// Firmware update service (lab/jc3248-pet-idf/main/ota.h).
    public static let otaService = CBUUID(string: "6E400010-B5A3-F393-E0A9-E50E24DCCA9E")
    public static let otaControl = CBUUID(string: "6E400011-B5A3-F393-E0A9-E50E24DCCA9E")   // read/write/notify
    public static let otaData    = CBUUID(string: "6E400012-B5A3-F393-E0A9-E50E24DCCA9E")   // write without response
    public static let otaMic     = CBUUID(string: "6E400013-B5A3-F393-E0A9-E50E24DCCA9E")   // notify: the mic's audio
    /// Improv Wi-Fi (firmware improv.c): the standard way to send a WiFi network.
    public static let improvService = CBUUID(string: Improv.service)
    public static let improvState   = CBUUID(string: Improv.state)
    public static let improvRPC     = CBUUID(string: Improv.rpc)
    /// The pet advertises "Claude-<last two MAC bytes>".
    public static let namePrefix = "Claude-"
}

/// The BLE link to one desk pet. Lifecycle follows Cat Lab Grilling's ProbeStore: a saved
/// peripheral UUID is tried before scanning, and the link reconnects whenever it drops while
/// `wantConnection` is set. Runs on the main queue.
public final class PetLink: NSObject, ObservableObject {
    public enum LinkState: Equatable {
        case starting, bluetoothOff, unauthorized, unsupported
        case idle, scanning, connecting, discovering, connected, disconnecting

        public var label: String {
            switch self {
            case .starting: return "Starting Bluetooth…"
            case .bluetoothOff: return "Bluetooth is off"
            case .unauthorized: return "Bluetooth permission denied"
            case .unsupported: return "Bluetooth LE not supported"
            case .idle: return "Disconnected"
            case .scanning: return "Looking for the pet…"
            case .connecting: return "Connecting…"
            case .discovering: return "Discovering services…"
            case .connected: return "Pet connected"
            case .disconnecting: return "Disconnecting…"
            }
        }
    }

    @Published public private(set) var state: LinkState = .starting
    @Published public private(set) var petName: String?
    @Published public private(set) var rssi: Int?
    @Published public private(set) var lastError: String?
    @Published public private(set) var sentCount = 0
    /// True for 4 s after the pet reports a touch ({"evt":"tap"} on NUS TX): the app shows the
    /// heart with it. Same window as the firmware's REACT_MS.
    @Published public private(set) var reacting = false
    @Published public private(set) var tapCount = 0
    /// Bot stand: the pet's last report, rank -> {"pet","on","now","best","rssi","ago"} ({"evt":"stand"}).
    @Published public private(set) var standBoard: [Int: [String: Any]] = [:]
    /// What the pet runs (read from the OTA control characteristic); nil on firmware without OTA.
    @Published public private(set) var firmware: PetFirmware?
    @Published public private(set) var ota: OTAPhase = .idle
    /// Bytes the pet has acked and the image size, while an update runs (for KB and KB/s).
    @Published public private(set) var otaAcked = 0
    @Published public private(set) var otaTotal = 0
    public private(set) var otaStartedAt = Date()
    private var otaCtrl: CBCharacteristic?
    private var otaDataChar: CBCharacteristic?
    private var otaImage = Data()
    /// The rest of a queued job: files still to send (path nil = app image) and the pack to
    /// select at the end. A full OTA is one app transfer; a partial OTA is a pack's files + select.
    private var otaQueue: [(path: String?, data: Data)] = []
    private var otaCurrentPath: String?
    private var otaSelect: String?
    private var otaRun: String?
    private var otaPlay: String?
    private var otaFirstAck: Date?
    private var otaSaying = ""
    /// Jobs asked for while another transfer runs (a pack, then an app): run in order, not dropped.
    private var otaPending: [(bytes: Int, start: () -> Void)] = []
    /// The whole batch (this job + queued ones), for one overall 0-100, and the current part's 0-100.
    @Published public private(set) var otaBatchTotal = 0
    @Published public private(set) var otaBatchDone = 0
    @Published public private(set) var otaPart = 0.0
    public var otaOverall: Double {
        otaBatchTotal == 0 ? 0 : min(1, Double(otaBatchDone + otaAcked) / Double(otaBatchTotal))
    }
    private var otaInBatch = false
    private var otaLastPct = -1
    @Published public private(set) var otaQueued = 0
    /// Result of the last speaker self-test (testSpeaker()), as a sentence.
    @Published public internal(set) var speakerTest: String?
    /// The last LISTEN: what the pet's mic measured, and where its recording was saved.
    @Published public internal(set) var micLevels: MicLevels?
    @Published public internal(set) var micRecording: URL?
    @Published public internal(set) var micStatus: String?
    private var micPending = false                     // a LISTEN is out; its error is not a transfer's
    /// What the pet heard, as text (Apple speech recognition on the recording), and its language.
    @Published public internal(set) var micText: String?
    /// WiFi / voice mode (VOICE.md), from the pet: "up 192.168.1.23", "connecting", "off"...
    @Published public internal(set) var wifiState: String?
    @Published public internal(set) var improvState: String?
    @Published public internal(set) var voiceOn = false
    @Published public internal(set) var wifiNetworks: [String] = []   // the last WIFI_SCAN
    @Published public internal(set) var voiceState: String?
    var improvRPC: CBCharacteristic?
    private var micLocale = "th-TH"
    var lastSaid: String?                          // the text of the last say, for replay()
    @Published public internal(set) var lastClip: Data?     // its clip
    var micBuffer = Data()
    /// Where a batch is: job n of m (queued requests count as jobs), file k of the job's files.
    @Published public private(set) var otaJobIndex = 0
    @Published public private(set) var otaJobCount = 0
    @Published public private(set) var otaFileIndex = 0
    @Published public private(set) var otaFileCount = 0
    private var otaBytesDone = 0
    private var otaWindow = OTAWindow(total: 0, window: 0)
    private var otaLastProgress = Date()
    private var otaWatchdog: Timer?
    private var fwTimer: Timer?
    private var otaStarted = Date()
    private var otaFrom: PetFirmware?
    private var txBuffer = Data()
    private var reactSeq = 0

    public var isLinked: Bool { state == .connected }
    /// Linked and its OTA service found: commands (say, listen, selftest, OTA) can go out.
    public var isReady: Bool { isLinked && otaCtrl != nil }

    private var central: CBCentralManager!
    private var peripheral: CBPeripheral?
    private var rxChar: CBCharacteristic?
    private var wantConnection = true
    private var disconnectWaiters: [() -> Void] = []
    private let defaults = UserDefaults.standard
    private let savedKey: String
    /// Pets another link already holds: this link never takes them (OraclePet drives two pets).
    public var avoid: () -> Set<UUID> = { [] }
    public var peripheralID: UUID? { peripheral?.identifier }

    /// `savedKey` remembers which pet this link uses ("petPeripheralID" for the first pet).
    public init(savedKey: String = "petPeripheralID") {
        self.savedKey = savedKey
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
    }

    public func connect() {
        wantConnection = true
        lastError = nil
        startLink()
    }

    /// Let go of the pet, so another host (usage_bridge.py, a phone) can connect.
    public func disconnect(completion: (() -> Void)? = nil) {
        wantConnection = false
        if central.isScanning { central.stopScan() }
        guard let p = peripheral, p.state == .connected || p.state == .connecting else {
            peripheral = nil
            rxChar = nil
            if central.state == .poweredOn { state = .idle }
            completion?()
            return
        }
        if let completion { disconnectWaiters.append(completion) }
        state = .disconnecting
        central.cancelPeripheralConnection(p)
    }

    /// The pet this link is told to use (its CoreBluetooth id), saved for the next launch.
    public var savedID: UUID? { defaults.string(forKey: savedKey).flatMap(UUID.init(uuidString:)) }

    /// Switch this link to one exact pet (the Pet Firmware sidebar, deskpet://select): let go of
    /// the current one, save the choice, connect to it by id. Connected or advertising both work.
    public func use(_ id: UUID) {
        if peripheral?.identifier == id, state == .connected || state == .connecting { return }
        disconnect { [weak self] in
            guard let self else { return }
            self.defaults.set(id.uuidString, forKey: self.savedKey)
            self.pinned = id
            self.petName = nil
            self.firmware = nil
            self.connect()
        }
    }
    private var pinned: UUID?

    /// Forget the saved pet and scan again (a different board).
    public func forgetPet() {
        disconnect { [weak self] in
            if let k = self?.savedKey { self?.defaults.removeObject(forKey: k) }
            self?.petName = nil
            self?.connect()
        }
    }

    /// Write one heartbeat line. False when there is no link yet.
    @discardableResult
    public func send(_ line: Data) -> Bool {
        guard let p = peripheral, p.state == .connected, let ch = rxChar else { return false }
        // NUS RX takes write-without-response; chunk to the negotiated size so a long line
        // still arrives whole (the pet reassembles on "\n").
        let max = p.maximumWriteValueLength(for: .withoutResponse)
        var i = line.startIndex
        while i < line.endIndex {
            let j = line.index(i, offsetBy: max, limitedBy: line.endIndex) ?? line.endIndex
            p.writeValue(line.subdata(in: i..<j), for: ch, type: .withoutResponse)
            i = j
        }
        sentCount += 1
        return true
    }

    private func startLink() {
        guard central.state == .poweredOn, wantConnection else { return }
        if let p = peripheral, p.state == .connected || p.state == .connecting { return }
        let taken = avoid()
        // A pet chosen by hand (use(_:)) comes first: connect to exactly that one.
        if let id = pinned, !taken.contains(id), let p = central.retrievePeripherals(withIdentifiers: [id]).first {
            pinned = nil
            return attach(p)
        }
        // Already connected by this Mac (after a relaunch it will not advertise).
        if let p = central.retrieveConnectedPeripherals(withServices: [PetGATT.service])
            .first(where: { ($0.name ?? "").hasPrefix(PetGATT.namePrefix) && !taken.contains($0.identifier) }) {
            return attach(p)
        }
        // The pet used last time: connect by identifier without waiting for an advertisement.
        if let saved = defaults.string(forKey: savedKey).flatMap(UUID.init(uuidString:)), !taken.contains(saved),
           let p = central.retrievePeripherals(withIdentifiers: [saved]).first {
            return attach(p)
        }
        state = .scanning
        central.scanForPeripherals(withServices: nil,
                                   options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    private func attach(_ p: CBPeripheral) {
        if central.isScanning { central.stopScan() }
        peripheral = p
        p.delegate = self
        petName = p.name
        state = .connecting
        central.connect(p, options: nil)
    }

    private func fail(_ message: String) {
        lastError = message
        petLog.error("\(message, privacy: .public)")
    }
}

extension PetLink: CBCentralManagerDelegate {
    public func centralManagerDidUpdateState(_ central: CBCentralManager) {
        switch central.state {
        case .poweredOn: state = .idle; startLink()
        case .poweredOff: state = .bluetoothOff
        case .unauthorized: state = .unauthorized
        case .unsupported: state = .unsupported
        default: state = .starting
        }
    }

    public func centralManager(_ central: CBCentralManager, didDiscover p: CBPeripheral,
                               advertisementData: [String: Any], rssi RSSI: NSNumber) {
        let name = p.name ?? (advertisementData[CBAdvertisementDataLocalNameKey] as? String) ?? ""
        guard name.hasPrefix(PetGATT.namePrefix), !avoid().contains(p.identifier) else { return }
        if let mine = peripheral, mine.state == .connecting || mine.state == .connected { return }
        if RSSI.intValue != 127 { rssi = RSSI.intValue }
        petLog.notice("found \(name, privacy: .public) rssi \(RSSI.intValue)")
        attach(p)
        petName = name
    }

    public func centralManager(_ central: CBCentralManager, didConnect p: CBPeripheral) {
        defaults.set(p.identifier.uuidString, forKey: savedKey)
        state = .discovering
        p.discoverServices([PetGATT.service, PetGATT.otaService, PetGATT.improvService])
        p.readRSSI()
    }

    public func centralManager(_ central: CBCentralManager, didFailToConnect p: CBPeripheral, error: Error?) {
        fail("Connect failed: \(error?.localizedDescription ?? "unknown error")")
        peripheral = nil
        state = .idle
        if wantConnection { DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.startLink() } }
    }

    public func centralManager(_ central: CBCentralManager, didDisconnectPeripheral p: CBPeripheral,
                               timestamp: CFAbsoluteTime, isReconnecting: Bool, error: Error?) {
        rxChar = nil
        otaCtrl = nil
        improvRPC = nil
        otaDataChar = nil
        otaLinkLost()
        if let error, wantConnection { fail("Link lost: \(error.localizedDescription)") }
        if isReconnecting { state = .connecting; return }
        peripheral = nil
        state = .idle
        let waiters = disconnectWaiters
        disconnectWaiters.removeAll()
        waiters.forEach { $0() }
        if wantConnection { DispatchQueue.main.asyncAfter(deadline: .now() + 2) { [weak self] in self?.startLink() } }
    }
}

extension PetLink: CBPeripheralDelegate {
    public func peripheral(_ p: CBPeripheral, didDiscoverServices error: Error?) {
        if let error { return fail("Service discovery failed: \(error.localizedDescription)") }
        guard let s = p.services?.first(where: { $0.uuid == PetGATT.service }) else {
            return fail("Nordic UART service not found on \(p.name ?? "peripheral")")
        }
        p.discoverCharacteristics([PetGATT.rx, PetGATT.tx], for: s)
        if let o = p.services?.first(where: { $0.uuid == PetGATT.otaService }) {
            p.discoverCharacteristics([PetGATT.otaControl, PetGATT.otaData, PetGATT.otaMic], for: o)
        }
        if let i = p.services?.first(where: { $0.uuid == PetGATT.improvService }) {
            p.discoverCharacteristics([PetGATT.improvState, PetGATT.improvRPC], for: i)
        }
    }

    public func peripheral(_ p: CBPeripheral, didDiscoverCharacteristicsFor s: CBService, error: Error?) {
        if let error { return fail("Characteristic discovery failed: \(error.localizedDescription)") }
        if s.uuid == PetGATT.improvService {
            improvRPC = s.characteristics?.first(where: { $0.uuid == PetGATT.improvRPC })
            if let st = s.characteristics?.first(where: { $0.uuid == PetGATT.improvState }) {
                p.setNotifyValue(true, for: st)
                p.readValue(for: st)
            }
            return
        }
        if s.uuid == PetGATT.otaService {
            otaCtrl = s.characteristics?.first(where: { $0.uuid == PetGATT.otaControl })
            otaDataChar = s.characteristics?.first(where: { $0.uuid == PetGATT.otaData })
            if let mic = s.characteristics?.first(where: { $0.uuid == PetGATT.otaMic }) { p.setNotifyValue(true, for: mic) }
            if let c = otaCtrl {
                p.setNotifyValue(true, for: c)
                p.readValue(for: c)                     // -> firmware version
            }
            if fwTimer == nil {                         // and again, to see pending -> valid
                fwTimer = Timer.scheduledTimer(withTimeInterval: 20, repeats: true) { [weak self] _ in
                    guard let self, let p = self.peripheral, p.state == .connected, let c = self.otaCtrl, !self.ota.isBusy else { return }
                    p.readValue(for: c)
                }
            }
            return
        }
        guard let rx = s.characteristics?.first(where: { $0.uuid == PetGATT.rx }) else {
            return fail("NUS RX characteristic not found")
        }
        rxChar = rx
        if let tx = s.characteristics?.first(where: { $0.uuid == PetGATT.tx }) { p.setNotifyValue(true, for: tx) }
        state = .connected
        lastError = nil                  // an old "Link lost" (pet rebooted, reflashed) no longer applies
        petLog.notice("connected to \(p.name ?? "?", privacy: .public)")
    }

    /// Lines the pet sends on TX. Only {"evt":"tap"} so far. OTA control values go to otaReceived.
    public func peripheral(_ p: CBPeripheral, didUpdateValueFor ch: CBCharacteristic, error: Error?) {
        if ch.uuid == PetGATT.otaControl, error == nil, let v = ch.value { return otaReceived(v) }
        if ch.uuid == PetGATT.otaMic, error == nil, let v = ch.value { micBuffer.append(v); return }   // LISTEN audio
        if ch.uuid == PetGATT.improvState, error == nil, let b = ch.value?.first {
            improvState = Improv.describe(state: b)
            return
        }
        guard error == nil, ch.uuid == PetGATT.tx, let v = ch.value else { return }
        txBuffer.append(v)
        while let nl = txBuffer.firstIndex(of: 0x0A) {
            let line = txBuffer[txBuffer.startIndex..<nl]
            txBuffer.removeSubrange(txBuffer.startIndex...nl)
            if let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any], o["evt"] as? String == "say",
               let text = o["text"] as? String {
                petLog.notice("pet asks to say '\(text, privacy: .public)'")   // pet.say() in a wasm app
                say(text, fromApp: true)                                       // not what "Play again" repeats
                continue
            }
            if let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any], o["evt"] as? String == "stand" {
                // bot stand: the pet's top 3 (who stayed next to it longest), every 10 s while linked
                petLog.notice("pet stand \(String(decoding: line, as: UTF8.self), privacy: .public)")
                if let rank = o["rank"] as? Int { standBoard[rank] = o }
                continue
            }
            if let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any], o["evt"] as? String == "touch" {
                petLog.notice("pet touch ev=\(String(describing: o["ev"] ?? "?"), privacy: .public)")   // raw report, for diagnosis
            } else if let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any], o["evt"] as? String == "tap" {
                tapCount += 1
                reacting = true
                reactSeq += 1
                let seq = reactSeq
                petLog.notice("pet tapped")
                DispatchQueue.main.asyncAfter(deadline: .now() + 4) { [weak self] in
                    if self?.reactSeq == seq { self?.reacting = false }   // a newer tap extends it
                }
            }
        }
        if txBuffer.count > 4096 { txBuffer.removeAll() }
    }

    public func peripheralIsReady(toSendWriteWithoutResponse p: CBPeripheral) { otaPump() }

    public func peripheral(_ p: CBPeripheral, didReadRSSI RSSI: NSNumber, error: Error?) {
        if error == nil { rssi = RSSI.intValue }
    }
}

/// Where a firmware update is.
public enum OTAPhase: Equatable {
    case idle
    case starting
    case sending(Double)          // fraction acked by the pet
    case finishing                // all bytes on the pet; it verifies and switches slot
    case rebooting                // pet accepted the image; waiting for it to come back
    case done(String)
    case failed(String)

    public var isBusy: Bool {
        switch self {
        case .starting, .sending, .finishing, .rebooting: return true
        default: return false
        }
    }

    public var label: String {
        switch self {
        case .idle: return ""
        case .starting: return "Starting update…"
        case .sending(let f): return "Sending to the pet… \(Int(f * 100))%"
        case .finishing: return "Pet is checking what it received…"   // firmware, a pack or an app; .rebooting names firmware
        case .rebooting: return "Pet is restarting into the new firmware…"
        case .done(let m), .failed(let m): return m
        }
    }
}

// MARK: - Firmware update (nRF52-DFU style; protocol in lab/jc3248-pet-idf/main/ota.h)
extension PetLink {
    public var canUpdateFirmware: Bool { state == .connected && otaCtrl != nil && otaDataChar != nil && !ota.isBusy }
    /// Linked with an OTA service: a request now runs at once or waits its turn.
    /// Buttons that start a transfer: off while one runs (a firmware write must not be cut by a
    /// second request or by switching pets). deskpet:// URLs still queue behind it.
    public var canQueueUpdate: Bool { state == .connected && otaCtrl != nil && otaDataChar != nil && !ota.isBusy }

    /// Full OTA: send an app image (build/jc3248_pet_idf.bin); the pet reboots into it.
    public func updateFirmware(_ image: Data) {
        guard let name = OTA.projectName(of: image) else { ota = .failed("Not an ESP32 app image"); return }
        guard OTA.petProjects[name] != nil else { ota = .failed("Not a pet firmware: \(name)"); return }
        // each board has its own project; the pet would refuse the other board's image anyway
        if let mine = firmware?.project, !mine.isEmpty, mine != name {
            ota = .failed("That firmware is for the \(OTA.boardName(name)) board; this pet is \(OTA.boardName(mine))")
            return
        }
        startJob([(nil, image)], select: nil)
    }

    /// Partial OTA: copy an animation pack (manifest.json + GIFs) into the pet's storage and show
    /// it. No reboot, the firmware stays; a pack is ~150 KB, a few seconds.
    public func uploadPack(_ name: String, files: [(name: String, data: Data)]) {
        let jobs = files.map { (path: Optional("characters/\(name)/\($0.name)"), data: $0.data) }
        // manifest last: a pack without one is skipped by the pet until it is complete
        let ordered = jobs.filter { !$0.path!.hasSuffix("manifest.json") } + jobs.filter { $0.path!.hasSuffix("manifest.json") }
        startJob(ordered, select: name)
    }

    /// Show a pack that is already on the pet.
    public func selectPack(_ name: String) { startJob([], select: name) }

    /// Partial OTA of a pet app: copy the .wasm into apps/ on the pet and run it.
    public func uploadApp(_ name: String, wasm: Data) {
        guard wasm.prefix(4) == Data([0x00, 0x61, 0x73, 0x6D]) else { ota = .failed("\(name) is not a WebAssembly module"); return }
        startJob([(path: "apps/\(name).wasm", data: wasm)], select: nil, run: name)
    }

    /// Speak through the pet's speaker: macOS speech -> a 16 kHz IMA ADPCM clip (a few KB)
    /// -> sounds/say.ima on the pet -> PLAY. Thai text gets the Thai voice.
    /// Play this pet's last spoken clip again (kept per pet: a wasm app on another pet, or on
    /// this one, speaking in between must not change what "again" means).
    public func replay() {
        guard let clip = lastClip else { return }
        startJob([(path: OTA.streamPath, data: clip)], select: nil, run: nil, play: nil, saying: lastSaid ?? "again")
    }
    public func say(_ text: String, language: String? = nil, fromApp: Bool = false) {
        let t = text.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        PetVoice.shared.clip(for: String(t.prefix(400)), language: language) { [weak self] clip in
            guard let self, let clip else { return }
            petLog.notice("say '\(t, privacy: .public)': \(clip.count) bytes, streamed")
            if !fromApp { self.lastSaid = t; self.lastClip = clip }
            // live: the pet starts talking after the first block instead of after the whole clip
            self.startJob([(path: OTA.streamPath, data: clip)], select: nil, run: nil, play: nil, saying: t)
        }
    }

    /// LISTEN: the pet records its mic for `ms`, measures it, and (sendAudio) sends the audio
    /// back; it is saved as /tmp/oraclepet-mic-<pet>.wav.
    /// `locale` ("th-TH", "en-US"): the language to transcribe the recording in (nil = no text).
    public func listen(ms: Int = 3000, sendAudio: Bool = true, locale: String? = "th-TH") {
        guard let p = peripheral, p.state == .connected, let c = otaCtrl else { return }
        micBuffer = Data()
        micPending = true
        micText = nil
        micLocale = locale ?? ""
        micStatus = "Listening for \(ms) ms…"
        p.writeValue(OTA.listen(ms: ms, sendAudio: sendAudio), for: c, type: .withResponse)
    }

    private func transcribe(_ wav: URL) {
        micText = "…"
        let started = Date()
        PetHearing.transcribe(wav, locale: micLocale) { [weak self] text, onDevice, err in
            guard let self else { return }
            let secs = String(format: "%.1f", Date().timeIntervalSince(started))
            self.micText = err.map { "(\($0))" } ?? (text.isEmpty ? "(no words)" : text)
            petLog.notice("mic text (\(self.micLocale, privacy: .public), \(onDevice ? "on device" : "server", privacy: .public), \(secs, privacy: .public) s): \(self.micText ?? "", privacy: .public)")
        }
    }

    /// WiFi for the voice stream, through the standard Improv RPC (the pet keeps it in NVS).
    public func sendWiFi(ssid: String, password: String) -> Bool {
        guard let p = peripheral, p.state == .connected, let c = improvRPC,
              let d = Improv.wifiSettings(ssid: ssid, password: password) else { return false }
        wifiState = "sending…"
        p.writeValue(d, for: c, type: .withResponse)
        return true
    }

    /// Ask the pet which WiFi networks it sees (2.4 GHz); results land in wifiNetworks.
    public func scanWiFi() {
        guard let p = peripheral, p.state == .connected, let c = otaCtrl else { return }
        wifiNetworks = []
        p.writeValue(OTA.wifiScan, for: c, type: .withResponse)
    }

    /// Voice mode: the pet streams what it hears (speech only) to the voice server on this Mac.
    public func setVoice(_ on: Bool) {
        guard let p = peripheral, p.state == .connected, let c = otaCtrl else { return }
        p.writeValue(OTA.voice(on), for: c, type: .withResponse)
    }

    /// Speaker self-test: the pet beeps, and a pet with a mic says whether it heard it.
    public func testSpeaker() {
        guard let p = peripheral, p.state == .connected, let c = otaCtrl else { return }
        speakerTest = "Beeping…"
        p.writeValue(OTA.selftest, for: c, type: .withResponse)
    }

    /// Run an app already on the pet; "" stops the running app.
    public func runApp(_ name: String) { startJob([], select: nil, run: name) }

    private func startJob(_ files: [(path: String?, data: Data)], select: String?, run: String? = nil,
                          play: String? = nil, saying: String = "") {
        if ota.isBusy {
            let bytes = files.reduce(0) { $0 + $1.data.count }
            otaPending.append((bytes, { [weak self] in self?.startJob(files, select: select, run: run, play: play, saying: saying) }))
            otaBatchTotal += bytes
            otaQueued = otaPending.count
            otaJobCount = otaJobIndex + otaPending.count
            petLog.notice("OTA: busy, queued (\(self.otaPending.count) waiting)")
            return
        }
        guard peripheral?.state == .connected, otaCtrl != nil, otaDataChar != nil else {
            ota = .failed("The pet's firmware has no OTA service yet: flash it once over USB")
            return
        }
        if !otaInBatch {                             // a new batch: this job plus anything already queued
            otaBatchDone = 0
            otaBatchTotal = files.reduce(0) { $0 + $1.data.count } + otaPending.reduce(0) { $0 + $1.bytes }
        }
        otaJobIndex = otaPending.isEmpty && !otaInBatch ? 1 : otaJobIndex + 1
        otaJobCount = otaJobIndex + otaPending.count
        otaInBatch = true
        otaFileIndex = 0
        otaFileCount = files.count
        otaQueue = files
        otaSelect = select
        otaRun = run
        otaPlay = play
        otaSaying = saying
        otaFrom = firmware
        otaStarted = Date()
        otaStartedAt = otaStarted
        otaBytesDone = 0
        otaAcked = 0
        otaTotal = files.reduce(0) { $0 + $1.data.count }
        otaLastProgress = Date()
        ota = .starting
        otaWatchdog?.invalidate()
        otaWatchdog = Timer.scheduledTimer(withTimeInterval: 2, repeats: true) { [weak self] _ in self?.otaCheckStall() }
        otaNext()
    }

    /// Begin the next queued transfer, or select the pack, or finish.
    private func otaNext() {
        guard let p = peripheral, let c = otaCtrl else { return otaFinish(.failed("Link lost")) }
        if !otaQueue.isEmpty {
            let job = otaQueue.removeFirst()
            otaFileIndex += 1
            otaPart = 0
            otaImage = job.data
            otaCurrentPath = job.path
            otaLastProgress = Date()
            ota = .starting
            let crc = OTA.crc32(job.data)
            let cmd: Data
            if job.path == OTA.streamPath { cmd = OTA.beginStream(size: job.data.count, crc: crc) }
            else if let path = job.path { cmd = OTA.beginFile(size: job.data.count, crc: crc, path: path) }
            else { cmd = OTA.begin(size: job.data.count, crc: crc) }
            otaFirstAck = nil
            petLog.notice("OTA: \(job.path ?? "app image", privacy: .public) \(job.data.count) bytes")
            p.writeValue(cmd, for: c, type: .withResponse)
        } else if let name = otaSelect {
            otaSelect = nil
            ota = .finishing
            p.writeValue(OTA.selectPack(name), for: c, type: .withResponse)
        } else if let name = otaRun {
            otaRun = nil
            ota = .finishing
            p.writeValue(OTA.runApp(name), for: c, type: .withResponse)
        } else if let name = otaPlay {
            otaPlay = nil
            ota = .finishing
            p.writeValue(OTA.play(name), for: c, type: .withResponse)
        } else {
            otaFinish(.done("Sent \(otaTotal / 1024) KB in \(Int(Date().timeIntervalSince(otaStarted))) s"))
        }
    }

    public func cancelFirmwareUpdate() {
        otaPending.removeAll()
        otaQueued = 0
        guard ota.isBusy else { return }
        if let p = peripheral, let c = otaCtrl, p.state == .connected { p.writeValue(OTA.abort, for: c, type: .withResponse) }
        otaQueue = []
        otaSelect = nil
        otaRun = nil
        otaPlay = nil
        otaFinish(.failed("Update cancelled"))
    }

    fileprivate func otaReceived(_ v: Data) {
        // WiFi and voice (VOICE.md): in the firmware read and in {"ota":"wifi"|"voice"} notes
        if let o = try? JSONSerialization.jsonObject(with: v) as? [String: Any] {
            if let w = o["wifi"] as? String { wifiState = w }
            if let m = o["voice"] as? Int { voiceOn = m == 1 }
            if let vs = o["voicestate"] as? String { voiceState = vs }
            if o["ota"] as? String == "wifi" {
                let st = o["state"] as? String ?? "?"
                wifiState = st == "up" ? "up \(o["ip"] as? String ?? "")" : st
                return
            }
            if o["ota"] as? String == "net", let ssid = o["ssid"] as? String {
                let line = "\(ssid) (\(o["rssi"] as? Int ?? 0) dBm, ch \(o["ch"] as? Int ?? 0)\((o["open"] as? Bool) == true ? ", open" : ""))"
                if !wifiNetworks.contains(line) { wifiNetworks.append(line) }
                return
            }
            if o["ota"] as? String == "voice" {
                voiceOn = (o["mode"] as? Int) == 1
                voiceState = o["state"] as? String
                return
            }
        }
        if let fw = PetFirmware.parse(v) {
            firmware = fw
            if ota == .rebooting {
                let secs = Int(Date().timeIntervalSince(otaStarted))
                let moved = otaFrom.map { $0.slot != fw.slot } ?? true
                otaFinish(moved ? .done("Updated in \(secs) s: now \(fw.version) on \(fw.slot)")
                                : .failed("Pet came back on \(fw.slot) again: the new image was rolled back"))
            }
            return
        }
        guard let st = OTAStatus.parse(v) else { return }
        if case let .heard(levels) = st {                              // LISTEN finished (not a transfer)
            micPending = false
            micLevels = levels
            if let pcm = PetSound.decode(micBuffer), !pcm.isEmpty {
                let name = (petName ?? "pet").replacingOccurrences(of: "Claude-", with: "")
                let url = URL(fileURLWithPath: "/tmp/oraclepet-mic-\(name).wav")
                try? PetSound.wav(pcm).write(to: url)
                micRecording = url
                micStatus = "Heard: \(levels.summary) · \(pcm.count) samples saved"
                if !micLocale.isEmpty { transcribe(url) }
            } else {
                micStatus = "Heard: \(levels.summary)\(levels.sent ? " · audio did not decode (\(micBuffer.count) bytes)" : "")"
            }
            petLog.notice("mic: \(self.micStatus ?? "", privacy: .public)")
            return
        }
        if case let .error(why) = st, micPending, !ota.isBusy {          // LISTEN refused ("no mic"), not a transfer
            micPending = false
            micStatus = why == "no mic" ? "This pet has no mic" : "Pet could not listen: \(why)"
            petLog.notice("mic: \(self.micStatus ?? "", privacy: .public)")
            return
        }
        if case let .selftest(mic, heard, db, quiet, tone) = st {      // not part of a transfer
            speakerTest = !mic ? "Beeped (this pet has no mic to check itself)"
                : heard ? String(format: "Heard by its mic: %+.1f dB at 1 kHz (rms %d vs %d quiet)", db, tone, quiet)
                : String(format: "Its mic heard nothing (%+.1f dB): speaker or amp off", db)
            return
        }
        otaLastProgress = Date()
        switch st {
        case .ready(let size, let window):
            guard ota == .starting, size == otaImage.count else { return }
            otaWindow = OTAWindow(total: size, window: window)
            ota = .sending(otaOverall)
            otaPump()
        case .ack(let off):
            guard case .sending = ota else { return }
            otaWindow.ack(off)
            if otaFirstAck == nil, off > 0 { otaFirstAck = Date() }
            otaAcked = otaBytesDone + otaWindow.acked
            otaPart = otaWindow.fraction
            let pct = Int(otaOverall * 100)
            if otaCurrentPath != nil || otaJobCount > 1, pct != otaLastPct,    // packs/apps: the pet draws it
               let p = peripheral, let c = otaCtrl {
                otaLastPct = pct
                p.writeValue(OTA.progress(pct), for: c, type: .withResponse)
            }
            ota = .sending(otaOverall)
            if otaWindow.allAcked {
                ota = .finishing
                if let p = peripheral, let c = otaCtrl { p.writeValue(OTA.end, for: c, type: .withResponse) }
            } else {
                otaPump()
            }
        case .saved:
            otaBytesDone += otaImage.count
            otaNext()
        case .pack(let name):
            if let p = peripheral, let c = otaCtrl { p.readValue(for: c) }   // refresh the pack list
            if otaRun != nil || otaPlay != nil { return otaNext() }
            otaFinish(.done("Pet now shows \(name) · \(otaTotal / 1024) KB in \(Int(Date().timeIntervalSince(otaStarted))) s"))
        case .streamed(let bytes, let crcOK):
            let first = otaFirstAck.map { String(format: "%.2f", $0.timeIntervalSince(otaStarted)) } ?? "?"
            otaBytesDone += otaImage.count
            otaFinish(.done("Pet says \u{201C}\(otaSaying)\u{201D} · streamed \(bytes / 1024) KB, sound after \(first) s\(crcOK ? "" : " (CRC mismatch)")"))
        case .play:
            let secs = String(format: "%.1f", Date().timeIntervalSince(otaStarted))
            otaFinish(.done(otaSaying.isEmpty ? "Pet plays the sound" : "Pet says \u{201C}\(otaSaying)\u{201D} · \(otaTotal / 1024) KB in \(secs) s"))
        case .app(let name):
            if let p = peripheral, let c = otaCtrl { p.readValue(for: c) }
            if otaPlay != nil { return otaNext() }
            let sent = otaTotal > 0 ? " · \(otaTotal) bytes in \(Int(Date().timeIntervalSince(otaStarted))) s" : ""
            otaFinish(.done(name.isEmpty ? "App stopped" : "Pet runs \(name)\(sent)"))
        case .done:
            ota = .rebooting
            otaLastProgress = Date()
        case .selftest, .heard:
            break                                    // handled above, before the transfer switch
        case .error(let why):
            otaQueue = []
            otaSelect = nil
            otaRun = nil
            otaPlay = nil
            otaFinish(.failed("Pet refused the update: \(why)"))
        }
    }

    /// Fill the window: CoreBluetooth's own queue says when to stop (canSendWriteWithoutResponse)
    /// and the pet's acks say how far ahead we may run.
    fileprivate func otaPump() {
        guard case .sending = ota, let p = peripheral, let d = otaDataChar else { return }
        let max = p.maximumWriteValueLength(for: .withoutResponse)
        while p.canSendWriteWithoutResponse, let r = otaWindow.next(maxLen: max) {
            p.writeValue(otaImage.subdata(in: r), for: d, type: .withoutResponse)
        }
    }

    fileprivate func otaLinkLost() {
        switch ota {
        case .rebooting: break                          // expected: the pet restarts into the new image
        case .finishing where otaCurrentPath == nil && otaSelect == nil && otaQueue.isEmpty:
            ota = .rebooting                            // app image: the done notify may not have made it out
        case .starting, .sending, .finishing:
            otaQueue = []
            otaSelect = nil
            otaRun = nil
            otaPlay = nil
            otaFinish(.failed("Link lost during the update; the pet keeps what it had"))
        default: break
        }
    }

    private func otaCheckStall() {
        guard ota.isBusy else { return }
        let quiet = Date().timeIntervalSince(otaLastProgress)
        if ota == .rebooting {
            if quiet > 60 { otaFinish(.failed("The pet did not come back after the update")) }
        } else if quiet > 20 {
            cancelFirmwareUpdate()
            ota = .failed("Update stalled: no progress from the pet for 20 s")
        }
    }

    private func otaFinish(_ phase: OTAPhase) {
        otaBatchDone += otaTotal                     // this job counts as done for the batch bar
        otaAcked = 0
        otaWatchdog?.invalidate()
        otaWatchdog = nil
        otaImage = Data()
        otaCurrentPath = nil
        ota = phase
        petLog.notice("OTA: \(phase.label, privacy: .public)")
        if otaPending.isEmpty { otaInBatch = false }
        if !otaPending.isEmpty {                     // the next queued job, once this one has settled
            let next = otaPending.removeFirst().start
            otaQueued = otaPending.count
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { [weak self] in
                guard let self else { return }
                if self.canUpdateFirmware { next() } else { self.otaPending.insert((0, next), at: 0); self.otaQueued = self.otaPending.count }
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 60) { [weak self] in   // the result shows for a minute
            if self?.ota == phase { self?.ota = .idle }
        }
    }
}
