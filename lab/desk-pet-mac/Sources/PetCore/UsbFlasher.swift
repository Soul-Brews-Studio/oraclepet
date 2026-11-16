import Foundation

/// Flash a pet over USB with espflash (esp-rs, one native binary, no Python): the first install
/// (bootloader + partition table + OTA data + app [+ storage]) or a rescue when Bluetooth OTA is
/// not possible. Uses the release layout of `gh release download firmware-latest`:
///
///     <board>/bootloader.bin  partition-table.bin  ota_data_initial.bin  storage.bin  app-<pack>.bin
///
/// Each part is written on its own (`espflash write-bin`), so NVS (0x9000, WiFi and settings) is
/// never touched. espflash draws its progress bar only on a terminal, so it runs under
/// `/usr/bin/script` (a pty) and the "n/total" numbers are read from that stream.
public final class UsbFlasher: ObservableObject {
    public static let shared = UsbFlasher()

    public enum Phase: Equatable {
        case idle
        case running(String)
        case done(String)
        case failed(String)
    }

    /// jc3248 / sc01 / lcd28 share this layout (lab/jc3248-pet-idf/partitions.csv).
    public static let layout: [(file: String, offset: Int)] = [
        ("bootloader.bin", 0x0),
        ("partition-table.bin", 0x8000),
        ("ota_data_initial.bin", 0xd000),
    ]
    public static let appOffset = 0x10000          // ota_0: otadata above points the boot here
    public static let storageOffset = 0x290000     // LittleFS: every pack (overwrites packs sent over BLE)

    @Published public private(set) var ports: [String] = []
    @Published public var port: String = ""
    @Published public private(set) var phase: Phase = .idle
    @Published public private(set) var progress: Double = 0
    @Published public private(set) var lastLine = ""
    @Published public var includeStorage = false
    @Published public var folder: URL? {
        didSet { UserDefaults.standard.set(folder?.path, forKey: "usbFirmwareFolder") }
    }

    public var isBusy: Bool { if case .running = phase { return true } else { return false } }
    public var espflash: String? { Self.findEspflash() }

    private var process: Process?

    public init() {
        if let p = UserDefaults.standard.string(forKey: "usbFirmwareFolder") {
            folder = URL(fileURLWithPath: p, isDirectory: true)
        }
        refreshPorts()
    }

    /// The copy inside OraclePet.app first (`just package` puts it there), then the usual installs.
    public static func findEspflash() -> String? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let bundled = Bundle.main.resourceURL?.appendingPathComponent("espflash").path
        let candidates = [bundled, "\(home)/.cargo/bin/espflash", "/opt/homebrew/bin/espflash", "/usr/local/bin/espflash"]
        return candidates.compactMap { $0 }.first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    /// USB serial ports a pet can sit on: the S3's native USB (usbmodem) or a UART bridge.
    public func refreshPorts() {
        let names = (try? FileManager.default.contentsOfDirectory(atPath: "/dev")) ?? []
        let found = names.filter { n in
            ["cu.usbmodem", "cu.usbserial", "cu.wchusbserial", "cu.SLAB_USBtoUART"].contains { n.hasPrefix($0) }
        }.sorted().map { "/dev/" + $0 }
        ports = found
        if !found.contains(port) { port = found.first ?? "" }
    }

    /// app-<pack>.bin files in the chosen folder, e.g. app-pulse.bin.
    public func appImages() -> [URL] {
        guard let folder else { return [] }
        let files = (try? FileManager.default.contentsOfDirectory(at: folder, includingPropertiesForKeys: nil)) ?? []
        return files.filter { $0.lastPathComponent.hasPrefix("app-") && $0.pathExtension == "bin" }
            .sorted { $0.lastPathComponent < $1.lastPathComponent }
    }

    /// What `flash(app:)` will write, in order. Missing base files are reported, not skipped.
    public func plan(app: URL) -> (parts: [(url: URL, offset: Int)], missing: [String]) {
        guard let folder else { return ([], ["firmware folder"]) }
        var parts: [(URL, Int)] = []
        var missing: [String] = []
        for p in Self.layout {
            let u = folder.appendingPathComponent(p.file)
            if FileManager.default.fileExists(atPath: u.path) { parts.append((u, p.offset)) } else { missing.append(p.file) }
        }
        parts.append((app, Self.appOffset))
        if includeStorage {
            let s = folder.appendingPathComponent("storage.bin")
            if FileManager.default.fileExists(atPath: s.path) { parts.append((s, Self.storageOffset)) } else { missing.append("storage.bin") }
        }
        return (parts.map { (url: $0.0, offset: $0.1) }, missing)
    }

    public func flash(app: URL) {
        guard !isBusy else { return }
        guard let tool = espflash else { return fail("espflash not found (cargo install espflash, or rebuild OraclePet with it bundled)") }
        refreshPorts()
        guard !port.isEmpty else { return fail("No USB pet found: plug it in with a data cable") }
        let (parts, missing) = plan(app: app)
        guard missing.isEmpty else { return fail("Missing in the firmware folder: " + missing.joined(separator: ", ")) }

        let sizes = parts.map { (try? FileManager.default.attributesOfItem(atPath: $0.url.path)[.size] as? Int) ?? 0 }
        let total = max(1, sizes.reduce(0, +))
        let started = Date()
        let port = self.port
        progress = 0
        phase = .running("Connecting to \(port)…")

        DispatchQueue.global(qos: .userInitiated).async { [self] in
            var doneBytes = 0
            for (i, part) in parts.enumerated() {
                let name = part.url.lastPathComponent
                let label = "\(i + 1)/\(parts.count) \(name) @ 0x\(String(part.offset, radix: 16))"
                DispatchQueue.main.async { self.phase = .running(label) }
                let base = doneBytes, size = sizes[i]
                let ok = self.run(tool, [
                    "write-bin", "--chip", "esp32s3", "--port", port, "--non-interactive",
                    "--skip-update-check", "--baud", "921600",
                    "0x" + String(part.offset, radix: 16), part.url.path,
                ]) { frac in
                    DispatchQueue.main.async { self.progress = Double(base) / Double(total) + frac * Double(size) / Double(total) }
                }
                guard ok else {
                    DispatchQueue.main.async { self.fail("\(name): \(self.lastLine.isEmpty ? "espflash failed" : self.lastLine)") }
                    return
                }
                doneBytes += size
            }
            let secs = Int(Date().timeIntervalSince(started))
            DispatchQueue.main.async {
                self.progress = 1
                self.phase = .done("Flashed \(app.lastPathComponent) over USB in \(secs) s (\(parts.count) parts)")
            }
        }
    }

    /// Stop: end `script` AND the espflash it started (else espflash keeps the port open).
    public func cancel() {
        process?.terminate()
        let k = Process()
        k.executableURL = URL(fileURLWithPath: "/usr/bin/pkill")
        k.arguments = ["-f", "espflash write-bin --chip esp32s3 --port \(port)"]
        try? k.run()
    }

    private func fail(_ m: String) { phase = .failed(m) }

    /// Runs one espflash command under a pty; `onFraction` gets 0…1 from its "n/total" progress.
    /// Blocks the calling (background) thread until espflash exits; true on exit 0.
    private func run(_ tool: String, _ args: [String], onFraction: @escaping (Double) -> Void) -> Bool {
        let p = Process()
        p.executableURL = URL(fileURLWithPath: "/usr/bin/script")
        p.arguments = ["-q", "/dev/null", tool] + args
        let pipe = Pipe()
        p.standardOutput = pipe
        p.standardError = pipe
        p.standardInput = FileHandle.nullDevice
        var buffer = ""
        pipe.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            guard !d.isEmpty, let s = String(data: d, encoding: .utf8) else { return }
            buffer += s
            // indicatif redraws with \r; plain log lines end in \n
            let pieces = buffer.components(separatedBy: CharacterSet(charactersIn: "\r\n"))
            buffer = pieces.last ?? ""
            for raw in pieces.dropLast() {
                let line = Self.stripANSI(raw).trimmingCharacters(in: .whitespaces)
                guard !line.isEmpty else { continue }
                if let f = Self.fraction(line) { onFraction(f) }
                DispatchQueue.main.async { self?.lastLine = line }
            }
        }
        do { try p.run() } catch {
            DispatchQueue.main.async { self.lastLine = "could not start espflash: \(error.localizedDescription)" }
            return false
        }
        DispatchQueue.main.async { self.process = p }
        p.waitUntilExit()
        pipe.fileHandleForReading.readabilityHandler = nil
        DispatchQueue.main.async { self.process = nil }
        return p.terminationStatus == 0
    }

    /// "… 123/456 …" -> 0.27 (the last n/total pair on the line, n <= total).
    static func fraction(_ line: String) -> Double? {
        let ns = line as NSString
        guard let re = try? NSRegularExpression(pattern: "(\\d+)/(\\d+)") else { return nil }
        guard let m = re.matches(in: line, range: NSRange(location: 0, length: ns.length)).last,
              let n = Double(ns.substring(with: m.range(at: 1))),
              let t = Double(ns.substring(with: m.range(at: 2))), t > 0, n <= t else { return nil }
        return n / t
    }

    static func stripANSI(_ s: String) -> String {
        s.replacingOccurrences(of: "\u{1B}\\[[0-9;?]*[ -/]*[@-~]", with: "", options: .regularExpression)
    }
}
