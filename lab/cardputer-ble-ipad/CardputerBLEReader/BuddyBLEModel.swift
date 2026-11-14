import Foundation
import CoreBluetooth
import SwiftUI
import CryptoKit

@MainActor
final class BuddyBLEModel: NSObject, ObservableObject {
    enum LinkState: String {
        case idle = "Idle"
        case scanning = "Scanning"
        case connecting = "Connecting"
        case connected = "Connected"
        case disconnecting = "Disconnecting"
        case unsupported = "Bluetooth unavailable"
    }

    @Published var state: LinkState = .idle
    @Published var bluetoothState = "unknown"
    @Published var discovered: [DiscoveredBuddy] = []
    @Published var selectedID: UUID?
    @Published var log: [String] = []
    @Published var controlFeedback = "Ready — scan has not started"
    @Published var rxServerName = "Claude-iPad"
    @Published var rxServerState = "RX server idle"
    @Published var rxServerActive = false
    @Published var rxServerWriteCount = 0
    @Published var rxServerBytes = 0
    @Published var rxServerCentrals = 0
    @Published var rxServerLastCentral = "—"

    @Published var claudeHomeURL = UserDefaults.standard.string(forKey: "ClaudeHomeURL") ?? "http://192.168.1.141:8765/heartbeat"
    @Published var claudeHomeActive = false
    @Published var claudeHomeState = "Claude Home idle"
    @Published var claudeHomePollCount = 0
    @Published var claudeHomeBytes = 0
    @Published var claudeHomeLastFetch = "—"
    @Published var claudeHomeRaw = ""
    @Published var claudeHomePretty = ""
    @Published var claudeHomeKind = "none"
    @Published var claudeHomeRows: [DecodedRow] = []
    @Published var claudeHomeError = ""

    @Published var lastJSON = ""
    @Published var draftRows: [DecodedRow] = []
    @Published var lastTXPretty = ""
    @Published var lastLiveTXPretty = ""
    @Published var lastLiveTXRows: [DecodedRow] = []
    @Published var txProofState = "DRAFT ONLY — not sent"
    @Published var txAckState = "No write attempted"
    @Published var lastTXHash = "—"
    @Published var lastTXBytes = 0
    @Published var lastTXChunks = 0
    @Published var lastTXDevice = "—"
    @Published var lastTXCharacteristic = "—"
    @Published var lastTXWriteType = "—"
    @Published var lastTXTimestamp = "—"
    @Published var pendingWriteResponses = 0
    @Published var lastRXRaw = ""
    @Published var lastRXPretty = ""
    @Published var lastRXKind = "none"
    @Published var lastRXRows: [DecodedRow] = []
    @Published var statusRows: [DecodedRow] = []
    @Published var helloRows: [DecodedRow] = []
    @Published var permissionRows: [DecodedRow] = []
    @Published var heartbeatRXRows: [DecodedRow] = []
    @Published var genericAckRows: [DecodedRow] = []
    @Published var lastAdvertName = ""
    @Published var lastAdvertPayload = ""
    @Published var lastAdvertRSSI = "—"
    @Published var lastAdvertTimestamp = "—"

    @Published var promptEnabled = false
    @Published var total = 0
    @Published var running = 0
    @Published var waiting = 0
    @Published var entries = 0
    @Published var tokens = 0
    @Published var tokensToday = 0
    @Published var message = "DRAFT TEST — no live Claude source"
    @Published var pct5h = 0
    @Published var reset5h = 0
    @Published var pct7d = 0
    @Published var reset7d = 0
    @Published var brightness = 180
    @Published var promptID = "ipad-permission-1"
    @Published var promptTool = "Bash"
    @Published var promptHint = "approve test heartbeat"
    @Published var nameToSend = "Claude iPad"
    @Published var ownerToSend = "Nat"
    @Published var chatContent = "iPad BLE test event"

    private let nusService = CBUUID(string: "6E400001-B5A3-F393-E0A9-E50E24DCCA9E")
    private let rxUUID = CBUUID(string: "6E400002-B5A3-F393-E0A9-E50E24DCCA9E")
    private let txUUID = CBUUID(string: "6E400003-B5A3-F393-E0A9-E50E24DCCA9E")

    private var central: CBCentralManager!
    private var peripheralManager: CBPeripheralManager!
    private var peripherals: [UUID: CBPeripheral] = [:]
    private var connectedPeripheral: CBPeripheral?
    private var rxCharacteristic: CBCharacteristic?
    private var txCharacteristic: CBCharacteristic?
    private var serverRXCharacteristic: CBMutableCharacteristic?
    private var serverTXCharacteristic: CBMutableCharacteristic?
    private var notifyBuffer = Data()
    private var rxServerBuffer = Data()
    private var claudeHomeTask: Task<Void, Never>?
    private var lastAdvertSignature = ""

    override init() {
        super.init()
        central = CBCentralManager(delegate: self, queue: nil)
        peripheralManager = CBPeripheralManager(delegate: self, queue: nil)
        Task { @MainActor [weak self] in
            try? await Task.sleep(nanoseconds: 700_000_000)
            self?.startClaudeHome()
        }
    }

    deinit {
        claudeHomeTask?.cancel()
    }

    var connectedName: String {
        connectedPeripheral?.name ?? discovered.first(where: { $0.id == selectedID })?.name ?? "none"
    }

    var isScanning: Bool { state == .scanning }

    var receiverReadySummary: String {
        let active = rxServerActive ? "advertising" : "idle"
        return "\(active) as \(rxServerName) · writes \(rxServerWriteCount) · bytes \(rxServerBytes)"
    }

    var hasConnectionHandle: Bool {
        connectedPeripheral != nil || selectedID.flatMap({ peripherals[$0] }) != nil
    }

    var linkRows: [DecodedRow] {
        [
            DecodedRow(key: "control.last", value: controlFeedback),
            DecodedRow(key: "central", value: bluetoothState),
            DecodedRow(key: "link.state", value: state.rawValue),
            DecodedRow(key: "scan.active", value: isScanning ? "yes" : "no"),
            DecodedRow(key: "found.count", value: String(discovered.count)),
            DecodedRow(key: "connected.name", value: connectedName),
            DecodedRow(key: "nus.rx", value: rxCharacteristic == nil ? "not ready" : "ready"),
            DecodedRow(key: "nus.tx", value: txCharacteristic == nil ? "not ready" : "notify ready")
        ]
    }

    var nusReadySummary: String {
        "RX write \(rxCharacteristic == nil ? "no" : "yes") · TX notify \(txCharacteristic == nil ? "no" : "yes")"
    }

    var receiverRows: [DecodedRow] {
        [
            DecodedRow(key: "server.name", value: rxServerName),
            DecodedRow(key: "server.state", value: rxServerState),
            DecodedRow(key: "server.active", value: rxServerActive ? "yes" : "no"),
            DecodedRow(key: "server.centrals", value: String(rxServerCentrals)),
            DecodedRow(key: "server.last_central", value: rxServerLastCentral),
            DecodedRow(key: "server.rx_writes", value: String(rxServerWriteCount)),
            DecodedRow(key: "server.rx_bytes", value: String(rxServerBytes)),
            DecodedRow(key: "rx.kind", value: lastRXKind),
            DecodedRow(key: "rx.raw", value: lastRXRaw.isEmpty ? "none" : lastRXRaw)
        ]
    }

    var hasLiveRX: Bool { !lastRXRaw.isEmpty }
    var hasLiveHome: Bool { !claudeHomeRaw.isEmpty && claudeHomeError.isEmpty }
    var hasLiveBuddyData: Bool { hasLiveRX || hasLiveHome }

    var rxProofState: String {
        if hasLiveRX { return "REAL BLE RX — \(lastRXKind)" }
        if hasLiveHome { return "REAL CLAUDE HOME — \(claudeHomeKind)" }
        return "LISTENING — no live data yet"
    }

    var claudeHomeSummary: String {
        let active = claudeHomeActive ? "polling" : "idle"
        return "\(active) · polls \(claudeHomePollCount) · bytes \(claudeHomeBytes) · \(claudeHomeLastFetch)"
    }

    var claudeHomeProofRows: [DecodedRow] {
        [
            DecodedRow(key: "home.url", value: claudeHomeURL),
            DecodedRow(key: "home.state", value: claudeHomeState),
            DecodedRow(key: "home.active", value: claudeHomeActive ? "yes" : "no"),
            DecodedRow(key: "home.polls", value: String(claudeHomePollCount)),
            DecodedRow(key: "home.bytes", value: String(claudeHomeBytes)),
            DecodedRow(key: "home.last_fetch", value: claudeHomeLastFetch),
            DecodedRow(key: "home.kind", value: claudeHomeKind),
            DecodedRow(key: "home.error", value: claudeHomeError.isEmpty ? "none" : claudeHomeError)
        ]
    }

    var buddyDisplayRows: [DecodedRow] {
        if !heartbeatRXRows.isEmpty { return heartbeatRXRows }
        if !claudeHomeRows.isEmpty { return claudeHomeRows }
        if lastRXKind.contains("heartbeat") { return lastRXRows.filter { !$0.key.hasPrefix("advert.") && $0.key != "source" } }
        return []
    }

    var buddyDisplayMessage: String {
        if let msg = buddyDisplayRows.first(where: { $0.key == "msg" })?.value, !msg.isEmpty { return msg }
        if hasLiveHome { return claudeHomeRaw }
        if !lastAdvertPayload.isEmpty { return lastAdvertPayload }
        return lastRXRaw
    }

    var protocolSummary: String {
        "Python buddy_protocol.py: heartbeat total/running/waiting/msg/entries/tokens/tokens_today/prompt; RX status ack includes name/owner/sec + bat/sys/stats/version."
    }

    var proofRows: [DecodedRow] {
        [
            DecodedRow(key: "tx.state", value: txProofState),
            DecodedRow(key: "tx.os_ack", value: txAckState),
            DecodedRow(key: "tx.device", value: lastTXDevice),
            DecodedRow(key: "tx.char", value: lastTXCharacteristic),
            DecodedRow(key: "tx.write_type", value: lastTXWriteType),
            DecodedRow(key: "tx.bytes", value: String(lastTXBytes)),
            DecodedRow(key: "tx.chunks", value: String(lastTXChunks)),
            DecodedRow(key: "tx.sha256", value: lastTXHash),
            DecodedRow(key: "tx.time", value: lastTXTimestamp),
            DecodedRow(key: "rx.kind", value: lastRXKind),
            DecodedRow(key: "rx.raw", value: lastRXRaw.isEmpty ? "none" : lastRXRaw)
        ]
    }

    func startReceiveServer() {
        guard peripheralManager.state == .poweredOn else {
            rxServerState = "RX server blocked — Bluetooth peripheral is \(peripheralStateName(peripheralManager.state))"
            controlFeedback = rxServerState
            appendLog(rxServerState)
            return
        }
        if rxServerActive {
            rxServerState = "RX server already advertising as \(rxServerName)"
            controlFeedback = rxServerState
            appendLog("RX server start skipped; already active")
            return
        }

        peripheralManager.stopAdvertising()
        peripheralManager.removeAllServices()
        rxServerBuffer.removeAll(keepingCapacity: true)

        let rx = CBMutableCharacteristic(
            type: rxUUID,
            properties: [.write, .writeWithoutResponse],
            value: nil,
            permissions: [.writeable]
        )
        let tx = CBMutableCharacteristic(
            type: txUUID,
            properties: [.notify, .read],
            value: nil,
            permissions: [.readable]
        )
        let service = CBMutableService(type: nusService, primary: true)
        service.characteristics = [rx, tx]
        serverRXCharacteristic = rx
        serverTXCharacteristic = tx
        peripheralManager.add(service)
        rxServerState = "RX server adding NUS service"
        controlFeedback = "Starting receive-only NUS peripheral"
        appendLog("RX server add NUS service; will advertise as \(rxServerName)")
    }

    func stopReceiveServer() {
        peripheralManager.stopAdvertising()
        peripheralManager.removeAllServices()
        rxServerActive = false
        rxServerCentrals = 0
        rxServerLastCentral = "—"
        serverRXCharacteristic = nil
        serverTXCharacteristic = nil
        rxServerBuffer.removeAll(keepingCapacity: true)
        rxServerState = "RX server stopped"
        controlFeedback = "Receive server stopped"
        appendLog("RX server stopped")
    }

    func scan() {
        guard central.state == .poweredOn else {
            controlFeedback = "Scan blocked — Bluetooth is \(bluetoothState)"
            appendLog("Bluetooth is not powered on: \(bluetoothState)")
            return
        }
        guard state != .connected else {
            controlFeedback = "Scan blocked — disconnect from \(connectedName) first"
            appendLog("scan skipped: already connected to \(connectedName)")
            return
        }
        if state == .scanning {
            controlFeedback = "Already scanning — waiting for Claude/NUS adverts"
            appendLog("scan already active")
            return
        }

        discovered.removeAll()
        selectedID = nil
        rxCharacteristic = nil
        txCharacteristic = nil
        notifyBuffer.removeAll(keepingCapacity: true)
        state = .scanning
        controlFeedback = "Scanning now — looking for Claude/NUS or oracle:* Python beacons"
        appendLog("scan started: all BLE adverts; filter Claude*, NUS, or oracle: local-name beacons")
        central.scanForPeripherals(withServices: nil, options: [CBCentralManagerScanOptionAllowDuplicatesKey: false])
    }

    func stopScan() {
        let wasScanning = state == .scanning
        central.stopScan()
        if wasScanning {
            state = .idle
            controlFeedback = "Scan stopped by user"
            appendLog("scan stopped")
        } else {
            controlFeedback = "Stop pressed — no active scan"
            appendLog("stop scan pressed; no active scan")
        }
    }

    func connect(_ buddy: DiscoveredBuddy) {
        guard let peripheral = peripherals[buddy.id] else {
            controlFeedback = "Connect failed — peripheral handle missing"
            appendLog("connect failed: no peripheral handle for \(buddy.name)")
            return
        }
        central.stopScan()
        selectedID = buddy.id
        state = .connecting
        controlFeedback = "Connecting to \(buddy.name)"
        appendLog("connect \(buddy.name)")
        connectedPeripheral = peripheral
        peripheral.delegate = self
        central.connect(peripheral, options: nil)
    }

    func disconnect() {
        central.stopScan()
        let selectedPeripheral = selectedID.flatMap { peripherals[$0] }
        var candidates: [CBPeripheral] = []
        if let connectedPeripheral { candidates.append(connectedPeripheral) }
        if let selectedPeripheral { candidates.append(selectedPeripheral) }
        candidates.append(contentsOf: central.retrieveConnectedPeripherals(withServices: [nusService]))

        var seen = Set<UUID>()
        let unique = candidates.filter { seen.insert($0.identifier).inserted }
        guard !unique.isEmpty else {
            state = .idle
            controlFeedback = "Disconnect pressed — no active connection"
            appendLog("disconnect pressed; no active connection")
            return
        }

        state = .disconnecting
        controlFeedback = "Disconnecting \(unique.map { $0.name ?? String($0.identifier.uuidString.prefix(8)) }.joined(separator: ", "))"
        unique.forEach { central.cancelPeripheralConnection($0) }
        appendLog("disconnect requested for \(unique.count) peripheral(s)")
    }

    func clearRX() {
        lastRXRaw = ""
        lastRXPretty = ""
        lastRXKind = "none"
        lastRXRows = []
        statusRows = []
        helloRows = []
        permissionRows = []
        heartbeatRXRows = []
        genericAckRows = []
        claudeHomeRaw = ""
        claudeHomePretty = ""
        claudeHomeKind = "none"
        claudeHomeRows = []
        claudeHomeError = ""
        claudeHomeBytes = 0
        claudeHomePollCount = 0
        claudeHomeLastFetch = "—"
        lastAdvertName = ""
        lastAdvertPayload = ""
        lastAdvertRSSI = "—"
        lastAdvertTimestamp = "—"
        lastAdvertSignature = ""
        rxServerWriteCount = 0
        rxServerBytes = 0
        rxServerBuffer.removeAll(keepingCapacity: true)
        appendLog("cleared decoded RX/Home data")
    }

    func startClaudeHome() {
        guard let url = normalizedClaudeHomeURL() else {
            claudeHomeError = "Invalid URL"
            claudeHomeState = "Claude Home URL invalid"
            controlFeedback = claudeHomeState
            appendLog("Claude Home invalid URL: \(claudeHomeURL)")
            return
        }
        claudeHomeURL = url.absoluteString
        UserDefaults.standard.set(claudeHomeURL, forKey: "ClaudeHomeURL")
        claudeHomeTask?.cancel()
        claudeHomeActive = true
        claudeHomeState = "Polling Claude Home"
        claudeHomeError = ""
        controlFeedback = "Claude Home polling started"
        appendLog("Claude Home polling \(claudeHomeURL)")
        claudeHomeTask = Task { [weak self] in
            while !Task.isCancelled {
                await self?.fetchClaudeHomeOnce()
                try? await Task.sleep(nanoseconds: 5_000_000_000)
            }
        }
    }

    func stopClaudeHome() {
        claudeHomeTask?.cancel()
        claudeHomeTask = nil
        claudeHomeActive = false
        claudeHomeState = "Claude Home stopped"
        controlFeedback = claudeHomeState
        appendLog("Claude Home polling stopped")
    }

    func pollClaudeHomeNow() {
        Task { await fetchClaudeHomeOnce() }
    }

    private func normalizedClaudeHomeURL() -> URL? {
        var raw = claudeHomeURL.trimmingCharacters(in: .whitespacesAndNewlines)
        if raw.isEmpty { return nil }
        if !raw.contains("://") { raw = "http://" + raw }
        guard var comps = URLComponents(string: raw) else { return nil }
        if comps.path.isEmpty || comps.path == "/" { comps.path = "/heartbeat" }
        return comps.url
    }

    private func fetchClaudeHomeOnce() async {
        guard let url = normalizedClaudeHomeURL() else {
            claudeHomeError = "Invalid URL"
            claudeHomeState = "Claude Home URL invalid"
            return
        }
        do {
            var request = URLRequest(url: url)
            request.cachePolicy = .reloadIgnoringLocalCacheData
            request.timeoutInterval = 8
            let (data, response) = try await URLSession.shared.data(for: request)
            let status = (response as? HTTPURLResponse)?.statusCode ?? 0
            guard (200..<300).contains(status) else { throw URLError(.badServerResponse) }
            decodeClaudeHomeData(data)
        } catch {
            claudeHomeError = error.localizedDescription
            claudeHomeState = "Claude Home error"
            claudeHomeLastFetch = displayTimestamp()
            appendLog("Claude Home error: \(error.localizedDescription)")
        }
    }

    func sendHeartbeat() {
        let heartbeat = BuddyHeartbeat(
            total: total,
            running: running,
            waiting: promptEnabled ? max(1, waiting) : waiting,
            msg: message,
            entries: entries,
            tokens: tokens,
            tokens_today: tokensToday,
            epoch: Int(Date().timeIntervalSince1970),
            tzoff: TimeZone.current.secondsFromGMT(),
            pct5h: pct5h,
            reset5h: reset5h,
            pct7d: pct7d,
            reset7d: reset7d,
            brightness: brightness,
            prompt: promptEnabled ? BuddyPrompt(id: promptID.isEmpty ? UUID().uuidString : promptID, tool: promptTool, hint: promptHint) : nil
        )
        send(heartbeat)
    }

    func sendSampleHeartbeat() {
        send(.sample(promptEnabled: promptEnabled))
    }

    func sendStatusRequest() {
        sendRawObject(["cmd": "status"])
    }

    func sendNameCommand() {
        sendRawObject(["cmd": "name", "name": nameToSend])
    }

    func sendOwnerCommand() {
        sendRawObject(["cmd": "owner", "owner": ownerToSend])
    }

    func sendUnpairRequest() {
        sendRawObject(["cmd": "unpair"])
    }

    func sendTimeTick() {
        sendRawObject(["time": Int(Date().timeIntervalSince1970)])
    }

    func sendChatEvent() {
        sendRawObject(["evt": "message", "role": "user", "content": chatContent])
    }

    private func send(_ heartbeat: BuddyHeartbeat) {
        do {
            let data = try heartbeat.jsonLine()
            rememberDraft(data)
            write(data)
        } catch {
            appendLog("encode failed: \(error.localizedDescription)")
        }
    }

    private func sendRawObject(_ object: [String: Any]) {
        do {
            var data = try JSONSerialization.data(withJSONObject: object, options: [.sortedKeys])
            data.append(0x0A)
            rememberDraft(data)
            write(data)
        } catch {
            appendLog("encode failed: \(error.localizedDescription)")
        }
    }

    private func rememberDraft(_ data: Data) {
        lastJSON = String(decoding: data, as: UTF8.self)
        let trimmed = data.dropLast(data.last == 0x0A ? 1 : 0)
        if let obj = try? JSONSerialization.jsonObject(with: Data(trimmed)),
           let pretty = prettyJSONString(obj) {
            lastTXPretty = pretty
            draftRows = flattenJSON(obj)
        } else {
            lastTXPretty = lastJSON
            draftRows = [DecodedRow(key: "raw", value: lastJSON)]
        }
    }

    private func write(_ data: Data) {
        guard let p = connectedPeripheral, let rx = rxCharacteristic else {
            txProofState = "DRAFT ONLY — not sent"
            txAckState = "No live BLE path: connect to Claude/NUS first"
            lastTXDevice = connectedName
            lastTXCharacteristic = "—"
            lastTXWriteType = "—"
            lastTXBytes = data.count
            lastTXChunks = 0
            lastTXHash = sha256Hex(data)
            lastTXTimestamp = displayTimestamp()
            appendLog("not ready: connect first and wait for RX characteristic")
            return
        }

        let writeType: CBCharacteristicWriteType
        if rx.properties.contains(.write) {
            writeType = .withResponse
        } else if rx.properties.contains(.writeWithoutResponse) {
            writeType = .withoutResponse
        } else {
            txProofState = "DRAFT ONLY — RX characteristic is not writable"
            txAckState = "RX props=\(rx.properties.rawValue)"
            appendLog("RX characteristic is not writable: props=\(rx.properties.rawValue)")
            return
        }

        let maxLen = max(20, p.maximumWriteValueLength(for: writeType))
        var chunks: [Data] = []
        var start = data.startIndex
        while start < data.endIndex {
            let end = data.index(start, offsetBy: maxLen, limitedBy: data.endIndex) ?? data.endIndex
            chunks.append(Data(data[start..<end]))
            start = end
        }

        lastLiveTXPretty = lastTXPretty
        lastLiveTXRows = draftRows
        lastTXHash = sha256Hex(data)
        lastTXBytes = data.count
        lastTXChunks = chunks.count
        lastTXDevice = p.name ?? p.identifier.uuidString
        lastTXCharacteristic = rx.uuid.uuidString.lowercased()
        lastTXWriteType = writeType == .withResponse ? "withResponse" : "withoutResponse"
        lastTXTimestamp = displayTimestamp()
        pendingWriteResponses = writeType == .withResponse ? chunks.count : 0
        txProofState = "LIVE BLE TX — handed to CoreBluetooth"
        txAckState = writeType == .withResponse ? "waiting for OS write ack (0/\(chunks.count))" : "queued withoutResponse; OS ack unavailable"

        for chunk in chunks {
            p.writeValue(chunk, for: rx, type: writeType)
        }
        appendLog("LIVE TX \(data.count) bytes / \(chunks.count) chunk(s) to \(lastTXCharacteristic) hash=\(lastTXHash.prefix(12))")
    }

    private func handleServerWrite(_ data: Data, central: CBCentral) {
        rxServerWriteCount += 1
        rxServerBytes += data.count
        rxServerLastCentral = central.identifier.uuidString
        rxServerBuffer.append(data)
        while let nl = rxServerBuffer.firstIndex(of: 0x0A) {
            let line = rxServerBuffer[..<nl]
            rxServerBuffer.removeSubrange(...nl)
            decodeIncomingLine(Data(line))
        }
        if rxServerBuffer.count > 8192 {
            appendLog("RX server buffer overflow; clearing \(rxServerBuffer.count) bytes")
            rxServerBuffer.removeAll(keepingCapacity: true)
        }
    }

    private func handleNotify(_ data: Data) {
        notifyBuffer.append(data)
        while let nl = notifyBuffer.firstIndex(of: 0x0A) {
            let line = notifyBuffer[..<nl]
            notifyBuffer.removeSubrange(...nl)
            decodeIncomingLine(Data(line))
        }
        if notifyBuffer.count > 4096 {
            appendLog("RX buffer overflow; clearing \(notifyBuffer.count) bytes")
            notifyBuffer.removeAll(keepingCapacity: true)
        }
    }

    private func decodeClaudeHomeData(_ data: Data) {
        let raw = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !raw.isEmpty else { return }
        claudeHomeRaw = raw
        claudeHomeBytes += data.count
        claudeHomePollCount += 1
        claudeHomeLastFetch = displayTimestamp()
        guard let obj = try? JSONSerialization.jsonObject(with: Data(raw.utf8)), let dict = obj as? [String: Any] else {
            claudeHomeKind = "non-JSON"
            claudeHomePretty = raw
            claudeHomeRows = [DecodedRow(key: "raw", value: raw)]
            claudeHomeState = "Claude Home received non-JSON"
            claudeHomeError = ""
            appendLog("Claude Home non-JSON: \(raw)")
            return
        }
        let rows = flattenJSON(obj)
        claudeHomePretty = prettyJSONString(obj) ?? raw
        claudeHomeRows = rows
        let heartbeatKeys = ["total", "running", "waiting", "tokens", "tokens_today", "entries", "prompt", "msg"]
        claudeHomeKind = heartbeatKeys.contains(where: { dict[$0] != nil }) ? "home heartbeat" : "home JSON"
        claudeHomeState = "Claude Home live"
        claudeHomeError = ""
        controlFeedback = "Claude Home real data received"
        appendLog("Claude Home RX \(claudeHomeKind): \(raw)")
    }

    private func decodeIncomingLine(_ data: Data) {
        let raw = String(decoding: data, as: UTF8.self).trimmingCharacters(in: .whitespacesAndNewlines)
        guard !raw.isEmpty else { return }
        lastRXRaw = raw
        guard let obj = try? JSONSerialization.jsonObject(with: data), let dict = obj as? [String: Any] else {
            lastRXKind = "non-JSON"
            lastRXPretty = raw
            lastRXRows = [DecodedRow(key: "raw", value: raw)]
            appendLog("RX non-JSON: \(raw)")
            return
        }
        let rows = flattenJSON(obj)
        lastRXPretty = prettyJSONString(obj) ?? raw
        lastRXRows = rows
        classifyIncoming(dict, rows: rows)
        appendLog("RX \(lastRXKind): \(raw)")
    }

    private func handleOracleAdvertisement(name: String, payload: String, rssi: NSNumber, peripheralID: UUID) {
        let trimmed = payload.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !trimmed.isEmpty else { return }
        let signature = "\(peripheralID.uuidString)|\(trimmed)|\(rssi)"
        lastAdvertName = name
        lastAdvertPayload = trimmed
        lastAdvertRSSI = String(rssi.intValue)
        lastAdvertTimestamp = displayTimestamp()

        let metadata = [
            DecodedRow(key: "source", value: "BLE advertisement local name"),
            DecodedRow(key: "advert.name", value: name),
            DecodedRow(key: "advert.payload", value: trimmed),
            DecodedRow(key: "advert.rssi", value: String(rssi.intValue)),
            DecodedRow(key: "advert.time", value: lastAdvertTimestamp)
        ]
        lastRXRaw = trimmed

        if let data = trimmed.data(using: .utf8),
           let obj = try? JSONSerialization.jsonObject(with: data),
           let dict = obj as? [String: Any] {
            let rows = flattenJSON(obj)
            lastRXPretty = prettyJSONString(obj) ?? trimmed
            lastRXRows = metadata + rows
            classifyIncoming(dict, rows: rows)
            if lastRXKind == "heartbeat" { lastRXKind = "advertisement heartbeat" }
        } else {
            lastRXKind = "advertisement:oracle"
            lastRXPretty = trimmed
            lastRXRows = metadata
        }

        if signature != lastAdvertSignature {
            lastAdvertSignature = signature
            appendLog("RX oracle advert RSSI \(rssi): \(trimmed)")
        }
    }

    private func classifyIncoming(_ dict: [String: Any], rows: [DecodedRow]) {
        if let cmd = dict["cmd"] as? String {
            switch cmd {
            case "hello":
                lastRXKind = "hello"
                helloRows = rows
            case "permission":
                lastRXKind = "permission decision"
                permissionRows = rows
            default:
                lastRXKind = "cmd:\(cmd)"
            }
            return
        }

        if let ack = dict["ack"] as? String {
            lastRXKind = "ack:\(ack)"
            if ack == "status" {
                statusRows = rows
            } else {
                genericAckRows = rows
            }
            return
        }

        if dict.keys.count == 1, dict["time"] != nil {
            lastRXKind = "time tick"
            return
        }

        if dict["evt"] != nil, dict["role"] != nil {
            lastRXKind = "chat event"
            return
        }

        let heartbeatKeys = ["total", "running", "waiting", "tokens", "tokens_today", "entries", "prompt", "msg"]
        if heartbeatKeys.contains(where: { dict[$0] != nil }) {
            lastRXKind = "heartbeat"
            heartbeatRXRows = rows
            return
        }

        lastRXKind = "unclassified JSON"
    }

    private func appendLog(_ line: String) {
        let stamp = DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
        log.insert("\(stamp)  \(line)", at: 0)
        if log.count > 120 { log.removeLast(log.count - 120) }
    }
}

struct DiscoveredBuddy: Identifiable, Equatable {
    var id: UUID
    var name: String
    var rssi: Int
    var advertisedNUS: Bool
}

extension BuddyBLEModel: CBCentralManagerDelegate {
    nonisolated func centralManagerDidUpdateState(_ central: CBCentralManager) {
        Task { @MainActor in
            switch central.state {
            case .poweredOn:
                bluetoothState = "powered on"
                if state == .unsupported { state = .idle }
                controlFeedback = "Bluetooth powered on — ready to scan"
                appendLog("Bluetooth powered on")
            case .poweredOff: bluetoothState = "powered off"; state = .unsupported
            case .unsupported: bluetoothState = "unsupported"; state = .unsupported
            case .unauthorized: bluetoothState = "unauthorized"; state = .unsupported
            case .resetting: bluetoothState = "resetting"
            case .unknown: bluetoothState = "unknown"
            @unknown default: bluetoothState = "unknown new state"
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDiscover peripheral: CBPeripheral, advertisementData: [String : Any], rssi RSSI: NSNumber) {
        Task { @MainActor in
            let advertised = advertisementData[CBAdvertisementDataLocalNameKey] as? String
            let serviceUUIDs = advertisementData[CBAdvertisementDataServiceUUIDsKey] as? [CBUUID] ?? []
            let advertisesNUS = serviceUUIDs.contains(nusService)
            if let advertised, advertised.hasPrefix("oracle:") {
                let payload = String(advertised.dropFirst("oracle:".count))
                handleOracleAdvertisement(name: advertised, payload: payload, rssi: RSSI, peripheralID: peripheral.identifier)
                return
            }
            let name = advertised ?? peripheral.name ?? (advertisesNUS ? "Claude/NUS \(peripheral.identifier.uuidString.prefix(8))" : "(unnamed)")
            guard name.hasPrefix("Claude") || advertisesNUS else { return }
            peripherals[peripheral.identifier] = peripheral
            let buddy = DiscoveredBuddy(id: peripheral.identifier, name: name, rssi: RSSI.intValue, advertisedNUS: advertisesNUS)
            if let idx = discovered.firstIndex(where: { $0.id == buddy.id }) {
                discovered[idx] = buddy
            } else {
                discovered.append(buddy)
                appendLog("found \(name) RSSI \(RSSI) NUS=\(advertisesNUS)")
            }
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didConnect peripheral: CBPeripheral) {
        Task { @MainActor in
            state = .connected
            controlFeedback = "Connected — discovering NUS service"
            appendLog("connected \(peripheral.name ?? peripheral.identifier.uuidString)")
            peripheral.discoverServices([nusService])
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didFailToConnect peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            state = .idle
            controlFeedback = "Connect failed — \(error?.localizedDescription ?? "unknown")"
            appendLog("connect failed: \(error?.localizedDescription ?? "unknown")")
        }
    }

    nonisolated func centralManager(_ central: CBCentralManager, didDisconnectPeripheral peripheral: CBPeripheral, error: Error?) {
        Task { @MainActor in
            state = .idle
            rxCharacteristic = nil
            txCharacteristic = nil
            connectedPeripheral = nil
            notifyBuffer.removeAll(keepingCapacity: true)
            txProofState = "DRAFT ONLY — disconnected"
            txAckState = error?.localizedDescription ?? "disconnected normally"
            controlFeedback = "Disconnected — \(error?.localizedDescription ?? "normal")"
            appendLog("disconnected: \(error?.localizedDescription ?? "normal")")
        }
    }
}

extension BuddyBLEModel: CBPeripheralDelegate {
    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverServices error: Error?) {
        Task { @MainActor in
            if let error { appendLog("service discovery failed: \(error.localizedDescription)"); return }
            for service in peripheral.services ?? [] where service.uuid == nusService {
                appendLog("NUS service ready")
                peripheral.discoverCharacteristics([rxUUID, txUUID], for: service)
            }
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didDiscoverCharacteristicsFor service: CBService, error: Error?) {
        Task { @MainActor in
            if let error { appendLog("characteristic discovery failed: \(error.localizedDescription)"); return }
            for c in service.characteristics ?? [] {
                if c.uuid == rxUUID {
                    rxCharacteristic = c
                    appendLog("RX write ready props=\(c.properties.rawValue)")
                } else if c.uuid == txUUID {
                    txCharacteristic = c
                    appendLog("TX notify ready props=\(c.properties.rawValue)")
                    if c.properties.contains(.notify) { peripheral.setNotifyValue(true, for: c) }
                }
            }
        }
    }


    nonisolated func peripheral(_ peripheral: CBPeripheral, didWriteValueFor characteristic: CBCharacteristic, error: Error?) {
        Task { @MainActor in
            guard characteristic.uuid == rxUUID else { return }
            if let error {
                txProofState = "LIVE BLE TX — OS write failed"
                txAckState = error.localizedDescription
                appendLog("TX write failed: \(error.localizedDescription)")
                return
            }
            if pendingWriteResponses > 0 { pendingWriteResponses -= 1 }
            let done = lastTXChunks - pendingWriteResponses
            txProofState = pendingWriteResponses == 0 ? "LIVE BLE TX — OS write acknowledged" : "LIVE BLE TX — partial OS ack"
            txAckState = "OS ack \(done)/\(max(lastTXChunks, 1)) at \(displayTimestamp())"
            appendLog("TX write acknowledged \(done)/\(max(lastTXChunks, 1))")
        }
    }

    nonisolated func peripheral(_ peripheral: CBPeripheral, didUpdateValueFor characteristic: CBCharacteristic, error: Error?) {
        Task { @MainActor in
            if let error { appendLog("notify error: \(error.localizedDescription)"); return }
            guard let data = characteristic.value else { return }
            handleNotify(data)
        }
    }
}

extension BuddyBLEModel: CBPeripheralManagerDelegate {
    nonisolated func peripheralManagerDidUpdateState(_ peripheral: CBPeripheralManager) {
        Task { @MainActor in
            let stateName = peripheralStateName(peripheral.state)
            rxServerState = "Bluetooth peripheral \(stateName)"
            appendLog("Bluetooth peripheral \(stateName)")
            if peripheral.state == .poweredOn, !rxServerActive {
                startReceiveServer()
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, didAdd service: CBService, error: Error?) {
        Task { @MainActor in
            if let error {
                rxServerState = "RX server add service failed — \(error.localizedDescription)"
                appendLog(rxServerState)
                return
            }
            peripheral.startAdvertising([
                CBAdvertisementDataLocalNameKey: rxServerName,
                CBAdvertisementDataServiceUUIDsKey: [nusService]
            ])
            rxServerState = "RX server advertising as \(rxServerName)"
            appendLog(rxServerState)
        }
    }

    nonisolated func peripheralManagerDidStartAdvertising(_ peripheral: CBPeripheralManager, error: Error?) {
        Task { @MainActor in
            if let error {
                rxServerActive = false
                rxServerState = "RX server advertising failed — \(error.localizedDescription)"
                appendLog(rxServerState)
            } else {
                rxServerActive = true
                rxServerState = "RX server live as \(rxServerName)"
                controlFeedback = "Receive-only NUS peripheral live — run ble_bridge.py"
                appendLog("RX server live; waiting for Python bridge writes")
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didSubscribeTo characteristic: CBCharacteristic) {
        Task { @MainActor in
            if characteristic.uuid == txUUID {
                rxServerCentrals += 1
                rxServerLastCentral = central.identifier.uuidString
                rxServerState = "Python bridge subscribed to TX notify"
                appendLog("RX server central subscribed: \(central.identifier.uuidString)")
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, central: CBCentral, didUnsubscribeFrom characteristic: CBCharacteristic) {
        Task { @MainActor in
            if characteristic.uuid == txUUID {
                rxServerCentrals = max(0, rxServerCentrals - 1)
                rxServerState = rxServerActive ? "RX server live as \(rxServerName)" : "RX server idle"
                appendLog("RX server central unsubscribed: \(central.identifier.uuidString)")
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveWrite requests: [CBATTRequest]) {
        Task { @MainActor in
            for request in requests {
                guard request.characteristic.uuid == rxUUID else {
                    peripheral.respond(to: request, withResult: .requestNotSupported)
                    continue
                }
                if let value = request.value {
                    handleServerWrite(value, central: request.central)
                    appendLog("RX server write \(value.count) bytes from \(request.central.identifier.uuidString)")
                }
                peripheral.respond(to: request, withResult: .success)
            }
        }
    }

    nonisolated func peripheralManager(_ peripheral: CBPeripheralManager, didReceiveRead request: CBATTRequest) {
        Task { @MainActor in
            guard request.characteristic.uuid == txUUID else {
                peripheral.respond(to: request, withResult: .requestNotSupported)
                return
            }
            request.value = Data("iPad receiver; notifications are only sent for real replies\n".utf8)
            peripheral.respond(to: request, withResult: .success)
        }
    }
}

private func peripheralStateName(_ state: CBManagerState) -> String {
    switch state {
    case .poweredOn: return "powered on"
    case .poweredOff: return "powered off"
    case .unsupported: return "unsupported"
    case .unauthorized: return "unauthorized"
    case .resetting: return "resetting"
    case .unknown: return "unknown"
    @unknown default: return "unknown new state"
    }
}

private func prettyJSONString(_ obj: Any) -> String? {
    guard JSONSerialization.isValidJSONObject(obj),
          let data = try? JSONSerialization.data(withJSONObject: obj, options: [.prettyPrinted, .sortedKeys]) else {
        return nil
    }
    return String(decoding: data, as: UTF8.self)
}

private func flattenJSON(_ value: Any, prefix: String = "") -> [DecodedRow] {
    if let dict = value as? [String: Any] {
        return dict.keys.sorted().flatMap { key -> [DecodedRow] in
            let next = prefix.isEmpty ? key : "\(prefix).\(key)"
            return flattenJSON(dict[key] ?? NSNull(), prefix: next)
        }
    }
    if let array = value as? [Any] {
        return array.enumerated().flatMap { idx, item in
            flattenJSON(item, prefix: "\(prefix)[\(idx)]")
        }
    }
    let display: String
    if value is NSNull {
        display = "null"
    } else if let bool = value as? Bool {
        display = bool ? "true" : "false"
    } else if let number = value as? NSNumber {
        display = number.stringValue
    } else if let string = value as? String {
        display = string
    } else {
        display = String(describing: value)
    }
    return [DecodedRow(key: prefix.isEmpty ? "value" : prefix, value: display)]
}


private func sha256Hex(_ data: Data) -> String {
    SHA256.hash(data: data).map { String(format: "%02x", $0) }.joined()
}

private func displayTimestamp() -> String {
    DateFormatter.localizedString(from: Date(), dateStyle: .none, timeStyle: .medium)
}
