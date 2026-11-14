import Foundation

/// Firmware update over BLE, the pet side of which is lab/jc3248-pet-idf/main/ota.c (protocol
/// in ota.h). Like nRF52 DFU: a control characteristic for commands and status, a data
/// characteristic the image streams into, and the pet acks how far it got.
public enum OTA {
    /// zlib CRC-32, the same table code the pet runs over what it writes to flash.
    public static func crc32(_ data: Data) -> UInt32 {
        var crc: UInt32 = 0xFFFF_FFFF
        data.withUnsafeBytes { (p: UnsafeRawBufferPointer) in
            for b in p { crc = table[Int((crc ^ UInt32(b)) & 0xFF)] ^ (crc >> 8) }
        }
        return ~crc
    }

    private static let table: [UInt32] = (0..<256).map { i -> UInt32 in
        var c = UInt32(i)
        for _ in 0..<8 { c = (c & 1) != 0 ? 0xEDB8_8320 ^ (c >> 1) : c >> 1 }
        return c
    }

    /// 0x01 BEGIN, u32 size, u32 crc (little-endian).
    public static func begin(size: Int, crc: UInt32) -> Data {
        var d = Data([0x01])
        withUnsafeBytes(of: UInt32(size).littleEndian) { d.append(contentsOf: $0) }
        withUnsafeBytes(of: crc.littleEndian) { d.append(contentsOf: $0) }
        return d
    }

    /// 0x04 BEGIN_FILE, u32 size, u32 crc, u8 len, path ("characters/<pack>/<file>").
    public static func beginFile(size: Int, crc: UInt32, path: String) -> Data {
        var d = begin(size: size, crc: crc)
        d[0] = 0x04
        let p = Data(path.utf8)
        d.append(UInt8(p.count))
        d.append(p)
        return d
    }

    /// 0x05 SELECT_PACK, u8 len, name.
    public static func selectPack(_ name: String) -> Data {
        let n = Data(name.utf8)
        return Data([0x05, UInt8(n.count)]) + n
    }

    /// 0x06 RUN_APP, u8 len, name: run /littlefs/apps/<name>.wasm; an empty name stops the app.
    public static func runApp(_ name: String) -> Data {
        let n = Data(name.utf8)
        return Data([0x06, UInt8(n.count)]) + n
    }

    /// 0x08 PLAY, u8 len, name: play sounds/<name>.ima on the pet's speaker.
    public static func play(_ name: String) -> Data {
        let n = Data(name.utf8)
        return Data([0x08, UInt8(n.count)]) + n
    }

    /// 0x0B BEGIN_STREAM, u32 size, u32 crc: a PSND clip the pet plays while it arrives.
    public static func beginStream(size: Int, crc: UInt32) -> Data {
        var d = begin(size: size, crc: crc)
        d[0] = 0x0B
        return d
    }
    /// Path marker for a streamed job (PetLink's transfer queue).
    public static let streamPath = "@stream"

    /// 0x0C LISTEN, u16 ms, u8 flags (bit 0: send the audio back on the mic characteristic).
    public static func listen(ms: Int, sendAudio: Bool) -> Data {
        let m = UInt16(clamping: max(100, min(10_000, ms)))
        return Data([0x0C, UInt8(m & 0xFF), UInt8(m >> 8), sendAudio ? 1 : 0])
    }

    /// 0x0F WIFI_SCAN: the pet lists the networks it can see, one {"ota":"net",…} each.
    public static let wifiScan = Data([0x0F])

    /// 0x0E VOICE, u8 mode: 0 off, 1 stream speech to the voice server over WiFi (VOICE.md).
    public static func voice(_ on: Bool) -> Data { Data([0x0E, on ? 1 : 0]) }

    /// 0x0D WIFI_CONFIG: u8 len + SSID, u8 len + password, u8 len + server URL ("" = find it by
    /// mDNS). Improv (below) is the standard way; this one also sets the URL. nil if too long.
    public static func wifiConfig(ssid: String, password: String, url: String) -> Data? {
        let parts = [Data(ssid.utf8), Data(password.utf8), Data(url.utf8)]
        guard !parts[0].isEmpty, parts[0].count <= 32, parts[1].count <= 64, parts[2].count <= 127 else { return nil }
        var d = Data([0x0D])
        for p in parts { d.append(UInt8(p.count)); d.append(p) }
        return d
    }

    /// 0x0A SELFTEST: the pet beeps; with a mic (lcd28) it reports whether it heard itself.
    public static let selftest = Data([0x0A])

    /// 0x07 PROGRESS, u8 pct: the overall % Desk Pet shows, so the pet shows the same number.
    public static func progress(_ pct: Int) -> Data { Data([0x07, UInt8(max(0, min(100, pct)))]) }

    public static let end = Data([0x02])
    public static let abort = Data([0x03])

    /// Version and build time of an app image, in the same shape the pet reports for itself
    /// (esp_app_desc_t: version[32] @16, project_name[32] @48, time[16] @80, date[16] @96).
    public static func describe(_ image: Data) -> PetFirmware? {
        guard projectName(of: image) != nil else { return nil }
        let base = 32
        func str(_ off: Int, _ len: Int) -> String {
            String(decoding: image.subdata(in: base + off..<base + off + len).prefix { $0 != 0 }, as: UTF8.self)
        }
        let sha = image.subdata(in: base + 144..<base + 152).map { String(format: "%02x", $0) }.joined()   // app_elf_sha256, first 8 bytes
        var f = PetFirmware(version: str(16, 32), slot: "", built: "\(str(96, 16)) \(str(80, 16))", sha: sha)
        f.project = str(48, 32)
        return f
    }

    /// Pet firmware projects, one per board (lab/jc3248-pet-idf/main/board.h).
    public static let petProjects: [String: String] = [
        "jc3248_pet_idf": "JC3248 3.5in",
        "oraclepet_lcd28": "2.8in ILI9341",
        "oraclepet_sc01": "WT32-SC01 Plus",
    ]
    public static func boardName(_ project: String) -> String { petProjects[project] ?? project }

    /// Basic checks before sending anything: an ESP app image starts with 0xE9 and carries an
    /// app description (magic 0xABCD5432) whose project name the pet compares with its own.
    public static func projectName(of image: Data) -> String? {
        let descOff = 24 + 8                       // esp_image_header_t + first segment header
        guard image.count > descOff + 80, image.first == 0xE9 else { return nil }
        let magic = image.subdata(in: descOff..<descOff + 4).withUnsafeBytes { $0.loadUnaligned(as: UInt32.self) }
        guard UInt32(littleEndian: magic) == 0xABCD_5432 else { return nil }
        let name = image.subdata(in: descOff + 48..<descOff + 80)   // project_name[32] after version[32]
        return String(decoding: name.prefix { $0 != 0 }, as: UTF8.self)
    }
}

/// One status line the pet notifies on the control characteristic.
public enum OTAStatus: Equatable {
    case ready(size: Int, window: Int)
    case ack(Int)
    case done
    case saved(String)            // a file landed (partial OTA)
    case pack(String)             // the pet now shows this pack
    case app(String)              // the pet now runs this app ("" = stopped)
    case play(String)             // the pet is playing this sound
    case streamed(bytes: Int, crcOK: Bool)   // a live stream finished
    case heard(MicLevels)                    // a LISTEN finished
    case selftest(mic: Bool, heard: Bool, db: Double, quiet: Int, tone: Int)
    case error(String)

    public static func parse(_ line: Data) -> OTAStatus? {
        guard let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any],
              let kind = o["ota"] as? String else { return nil }
        switch kind {
        case "ready": return .ready(size: o["size"] as? Int ?? 0, window: o["window"] as? Int ?? 4096)
        case "ack": return (o["off"] as? Int).map(OTAStatus.ack)
        case "done": return .done
        case "saved": return .saved(o["path"] as? String ?? "")
        case "pack": return .pack(o["name"] as? String ?? "")
        case "app": return .app(o["name"] as? String ?? "")
        case "play": return .play(o["name"] as? String ?? "")
        case "streamed": return .streamed(bytes: o["bytes"] as? Int ?? 0, crcOK: o["crc"] as? Bool ?? false)
        case "heard":
            return .heard(MicLevels(ms: o["ms"] as? Int ?? 0, samples: o["samples"] as? Int ?? 0, rms: o["rms"] as? Int ?? 0,
                                    peak: o["peak"] as? Int ?? 0, freq: o["freq"] as? Int ?? 0,
                                    db: (o["db"] as? Double) ?? Double(o["db"] as? Int ?? 0), floor: o["floor"] as? Int ?? 0,
                                    sent: o["sent"] as? Bool ?? false))
        case "selftest":
            return .selftest(mic: o["mic"] as? Bool ?? false, heard: o["heard"] as? Bool ?? false,
                             db: o["db"] as? Double ?? 0, quiet: o["quiet"] as? Int ?? 0, tone: o["tone"] as? Int ?? 0)
        case "error": return .error(o["why"] as? String ?? "unknown error")
        default: return nil
        }
    }
}

/// What the pet says about itself when the control characteristic is read.
public struct PetFirmware: Equatable {
    public var version: String
    public var slot: String
    public var built: String
    /// First 16 hex digits of the ELF SHA-256: tells two builds apart when version and build
    /// time match. "" from firmware older than this field.
    public var sha: String = ""
    /// The firmware project = which board it is for ("jc3248_pet_idf", "oraclepet_lcd28").
    public var project: String = ""
    /// "pending" until the new image marks itself good, then "valid" ("" from older firmware).
    public var state: String = ""
    /// Animation packs on the pet's storage, the one it shows, and free storage in KB.
    public var pack: String = ""
    public var packs: [String] = []
    public var freeKB: Int = 0
    /// Pet apps (WebAssembly, lab/pet-sdk) on the pet, the running one, and its last error.
    public var app: String = ""
    public var apps: [String] = []
    public var appError: String = ""

    public init(version: String, slot: String, built: String, sha: String = "") {
        self.version = version
        self.slot = slot
        self.built = built
        self.sha = sha
    }

    /// The same build, by hash when both sides have one.
    public func sameBuild(as o: PetFirmware) -> Bool {
        // The pet reports CONFIG_APP_RETRIEVE_LEN_ELF_SHA hex digits (9 by default); compare
        // on the shorter of the two.
        let n = min(sha.count, o.sha.count)
        return n >= 8 ? sha.prefix(n) == o.sha.prefix(n) : (version == o.version && built == o.built)
    }

    public static func parse(_ data: Data) -> PetFirmware? {
        guard let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any],
              let v = o["ver"] as? String else { return nil }
        var f = PetFirmware(version: v, slot: o["slot"] as? String ?? "?", built: o["built"] as? String ?? "",
                            sha: o["sha"] as? String ?? "")
        f.state = o["state"] as? String ?? ""
        f.project = o["project"] as? String ?? ""
        f.pack = o["pack"] as? String ?? ""
        f.packs = (o["packs"] as? [String] ?? []).sorted()
        f.freeKB = o["free"] as? Int ?? 0
        f.app = o["app"] as? String ?? ""
        f.apps = (o["apps"] as? [String] ?? []).sorted()
        f.appError = o["apperr"] as? String ?? ""
        return f
    }
}

/// Flow control for write-without-response: never more than `window` bytes beyond the last ack.
public struct OTAWindow {
    public let total: Int
    public private(set) var sent = 0
    public private(set) var acked = 0
    public var window: Int

    public init(total: Int, window: Int) {
        self.total = total
        self.window = window
    }

    /// The next byte range to send, at most `maxLen` long, or nil when the window is full or
    /// everything is sent.
    public mutating func next(maxLen: Int) -> Range<Int>? {
        guard sent < total, sent - acked < window, maxLen > 0 else { return nil }
        let n = min(maxLen, total - sent, window - (sent - acked))
        let r = sent..<sent + n
        sent += n
        return r
    }

    public mutating func ack(_ off: Int) { acked = max(acked, min(off, sent)) }

    public var allAcked: Bool { acked == total }
    public var fraction: Double { total == 0 ? 0 : Double(acked) / Double(total) }
}

/// What the pet's mic measured during a LISTEN (PROTOCOL.md 4.6).
public struct MicLevels: Equatable {
    public var ms, samples, rms, peak, freq: Int
    public var db: Double
    public var floor: Int
    public var sent: Bool
    public init(ms: Int, samples: Int, rms: Int, peak: Int, freq: Int, db: Double, floor: Int, sent: Bool) {
        self.ms = ms; self.samples = samples; self.rms = rms; self.peak = peak
        self.freq = freq; self.db = db; self.floor = floor; self.sent = sent
    }
    public var summary: String {
        String(format: "rms %d (floor %d), peak %d, strongest %d Hz %+.1f dB", rms, floor, peak, freq, db)
    }
}
