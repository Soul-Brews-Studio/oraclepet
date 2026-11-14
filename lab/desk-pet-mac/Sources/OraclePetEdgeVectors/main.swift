// Edge cases for the C core, answered by the Swift PetProtocol (the implementation the pets
// already accept). golden/vectors.json pins the main layouts; this pins the corners it does
// not reach: clipping and escaping in the heartbeat, clamped arguments, truncated clips,
// speech tidying, the send window, and the status lines Swift knows.
//   tools/gen_edge_vectors.sh  ->  tests/edge_vectors.json
import Foundation
import PetProtocol

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }
func le16(_ s: [Int16]) -> Data {
    var d = Data()
    for v in s { withUnsafeBytes(of: v.littleEndian) { d.append(contentsOf: $0) } }
    return d
}

var v: [String: Any] = [:]
v["generated_by"] = "lab/desk-pet-mac Sources/PetProtocol (Swift), oraclepet-lib/core/tools/edge_vectors.swift"

// Heartbeat lines: escaping, msg clipped to 63 bytes and who to 20 characters on character
// (grapheme cluster) boundaries, negative numbers, every optional key.
let x = { (n: Int) in String(repeating: "x", count: n) }
let hbCases: [(String, Int, Int, Int, String, Int, Int, Int?, Int?, Int?, Int?, String?)] = [
    ("minimal", 0, 0, 0, "", 0, 0, nil, nil, nil, nil, nil),
    ("all_keys", 5, 2, 1, "2 working", 1792547421, -18000, 0, 0, 100, 604800, "hermes"),
    ("escapes", 1, 0, 0, "a\u{01}b\rc\td\ne\\f\"g\u{7f}h\u{1f}", 1, 2, nil, nil, nil, nil, "q\"\\"),
    ("thai_long", 2, 0, 2, "รอ: นีโอ, ไฮเปอร์, เนกซัส, โอดิน, พัลส์, เฮอร์มีส", 3, 25200, nil, nil, nil, nil, "นีโอ"),
    ("clip_exact", 1, 1, 0, x(63), 4, 0, nil, nil, nil, nil, nil),
    ("clip_over", 1, 1, 0, x(64), 4, 0, nil, nil, nil, nil, nil),
    ("clip_skin_tone", 1, 1, 0, x(58) + "👍🏽", 5, 0, nil, nil, nil, nil, nil),
    ("clip_combining", 1, 1, 0, x(61) + "e\u{301}", 6, 0, nil, nil, nil, nil, nil),
    ("clip_thai_am", 1, 1, 0, x(60) + "กำ", 7, 0, nil, nil, nil, nil, nil),
    ("clip_thai_fits", 1, 1, 0, x(57) + "กำ", 7, 0, nil, nil, nil, nil, nil),
    ("clip_flag", 1, 1, 0, x(57) + "🇹🇭", 8, 0, nil, nil, nil, nil, nil),
    ("clip_two_flags", 1, 1, 0, x(48) + "🇹🇭🇯🇵", 8, 0, nil, nil, nil, nil, nil),
    ("clip_zwj", 1, 1, 0, x(50) + "👨‍👩‍👧", 9, 0, nil, nil, nil, nil, nil),
    ("clip_keycap", 1, 1, 0, x(58) + "1\u{FE0F}\u{20E3}", 10, 0, nil, nil, nil, nil, nil),
    ("clip_crlf", 1, 1, 0, x(62) + "\r\n", 11, 0, nil, nil, nil, nil, nil),
    ("who_long", 1, 0, 0, "fleet idle", 12, 0, nil, nil, nil, nil, "abcdefghijklmnopqrstuvwxyz"),
    ("who_thai", 1, 0, 1, "wait", 13, 0, nil, nil, nil, nil, "นีโอนีโอนีโอนีโอนีโอนีโอนีโอนีโอ"),
    ("who_emoji", 1, 0, 1, "wait", 14, 0, nil, nil, nil, nil, "👨‍👩‍👧🇹🇭👍🏽e\u{301}abcdefghijklmnopqrstuvwxyz"),
]
v["heartbeat"] = hbCases.map { c -> [String: Any] in
    let hb = Heartbeat(total: c.1, running: c.2, waiting: c.3, msg: c.4, epoch: c.5, tzoff: c.6,
                       pct5h: c.7, reset5h: c.8, pct7d: c.9, reset7d: c.10, who: c.11)
    var args: [String: Any] = ["total": c.1, "running": c.2, "waiting": c.3, "msg": c.4, "epoch": c.5, "tzoff": c.6]
    if let p = c.7 { args["pct5h"] = p }
    if let p = c.8 { args["reset5h"] = p }
    if let p = c.9 { args["pct7d"] = p }
    if let p = c.10 { args["reset7d"] = p }
    if let w = c.11 { args["who"] = w }
    return ["name": c.0, "args": args, "line": String(decoding: hb.jsonLine(), as: UTF8.self)]
}

// Commands with arguments Swift clamps or counts in bytes
v["commands"] = [
    ["name": "progress", "args": ["pct": -5], "hex": hex(OTA.progress(-5))],
    ["name": "progress", "args": ["pct": 100], "hex": hex(OTA.progress(100))],
    ["name": "progress", "args": ["pct": 150], "hex": hex(OTA.progress(150))],
    ["name": "listen", "args": ["ms": 50, "send_audio": false], "hex": hex(OTA.listen(ms: 50, sendAudio: false))],
    ["name": "listen", "args": ["ms": 100, "send_audio": true], "hex": hex(OTA.listen(ms: 100, sendAudio: true))],
    ["name": "listen", "args": ["ms": 10000, "send_audio": true], "hex": hex(OTA.listen(ms: 10000, sendAudio: true))],
    ["name": "listen", "args": ["ms": 20000, "send_audio": false], "hex": hex(OTA.listen(ms: 20000, sendAudio: false))],
    ["name": "begin", "args": ["size": 0, "crc": 0], "hex": hex(OTA.begin(size: 0, crc: 0))],
    ["name": "begin", "args": ["size": 4294967295, "crc": 4294967295], "hex": hex(OTA.begin(size: 4294967295, crc: 4294967295))],
    ["name": "begin_file", "args": ["size": 5, "crc": 1, "path": "sounds/say.ima"], "hex": hex(OTA.beginFile(size: 5, crc: 1, path: "sounds/say.ima"))],
    ["name": "begin_file", "args": ["size": 6, "crc": 2, "path": "apps/แมว.wasm"], "hex": hex(OTA.beginFile(size: 6, crc: 2, path: "apps/แมว.wasm"))],
    ["name": "select_pack", "args": ["name": ""], "hex": hex(OTA.selectPack(""))],
    ["name": "run_app", "args": ["name": "abcdefghijklmnopqrstuvw"], "hex": hex(OTA.runApp("abcdefghijklmnopqrstuvw"))],
    ["name": "play", "args": ["name": "เสียง"], "hex": hex(OTA.play("เสียง"))],
]

// PSND: the golden 440 Hz tone, cut short at various lengths, and broken headers
let tone = (0..<1000).map { Int16(12_000 * sin(2 * Double.pi * 440 * Double($0) / 16_000)) }
let toneClip = PetSound.clip(tone)
let full = PetSound.decode(toneClip)!
v["psnd_truncated"] = [16, 17, 19, 20, 21, 100, 271, 272, 273, 274, 275, 400, 527, 528].map { n -> [String: Any] in
    let d = PetSound.decode(toneClip.prefix(n))!
    precondition(Array(full.prefix(d.count)) == d, "a cut clip decodes to a prefix")
    return ["len": n, "samples": d.count]
}
var badMagic = toneClip; badMagic[0] = 0x51
var badVersion = toneClip; badVersion[4] = 2
var badCodec = toneClip; badCodec[5] = 2
v["psnd_invalid"] = [
    ["name": "short", "hex": hex(toneClip.prefix(15)), "decodes": PetSound.decode(toneClip.prefix(15)) != nil],
    ["name": "magic", "hex": hex(badMagic.prefix(16)), "decodes": PetSound.decode(badMagic) != nil],
    ["name": "version", "hex": hex(badVersion.prefix(16)), "decodes": PetSound.decode(badVersion) != nil],
    ["name": "codec", "hex": hex(badCodec.prefix(16)), "decodes": PetSound.decode(badCodec) != nil],
]
// A block whose step index byte is out of range (the decoder clamps it to 88) and an empty clip
var wild = PetSound.clip([Int16](repeating: 0, count: 10))
wild[16 + 2] = 200
wild[16 + 4] = 0x7F
v["psnd_misc"] = [
    ["name": "index_clamped", "clip_hex": hex(wild), "decoded": PetSound.decode(wild)!.map { Int($0) }],
    ["name": "empty", "clip_hex": hex(PetSound.clip([])), "decoded": PetSound.decode(PetSound.clip([]))!.map { Int($0) }],
    ["name": "two_blocks", "clip_hex": hex(PetSound.clip((0..<505).map { Int16(truncatingIfNeeded: $0 * 131) })),
     "decoded": PetSound.decode(PetSound.clip((0..<505).map { Int16(truncatingIfNeeded: $0 * 131) }))!.map { Int($0) }],
]

// Speech tidy: trim |s| <= 300 ends, keep 20 ms, 0.9 peak, 6 s cap
var speech = [Int16](repeating: 0, count: 800)
speech += (0..<1200).map { Int16(6000 * sin(2 * Double.pi * Double($0) / 37)) }
speech += (0..<300).map { Int16(($0 * 37) % 501 - 250) }
speech += [Int16](repeating: 0, count: 900)
let quiet: [Int16] = (0..<500).map { [300, -300, 0, 17][$0 % 4] }
let edges: [Int16] = [400] + [Int16](repeating: 0, count: 10) + [-32768] + [Int16](repeating: 0, count: 10) + [301]
let rounding: [Int16] = [0, 301, -301, 1000, -1000, 333, -333, 32767, 12345, -12345, 5, -5, 0]
v["tidy"] = [("speech", speech), ("quiet", quiet), ("edges", edges), ("rounding", rounding)].map { c -> [String: Any] in
    ["name": c.0, "pcm": c.1.map { Int($0) }, "out": PetSound.tidy(c.1).map { Int($0) }]
}
// too long: 100000 loud samples, kept to the first 6 s (formula shared with test_core.c)
let long: [Int16] = (0..<100_000).map { Int16(($0 * 7919) % 20001 - 10000) }
let longOut = PetSound.tidy(long)
v["tidy_long"] = ["n": long.count, "out_len": longOut.count, "out_crc32": OTA.crc32(le16(longOut)),
                  "formula": "((i * 7919) % 20001) - 10000"]

v["is_thai"] = ["", "hello", "สวัสดี", "mixed ทดสอบ text", "\u{0E00}", "\u{0E7F}", "\u{0E80}", "\u{0DFF}", "é👍"].map {
    ["text": $0, "thai": PetSound.isThai($0)]
}

// Firmware images: every string field full width (no NUL), the other board, broken magics
func image(_ version: String, _ project: String, _ time: String, _ date: String, first: UInt8 = 0xE9,
           magic: [UInt8] = [0x32, 0x54, 0xCD, 0xAB]) -> Data {
    var img = Data(repeating: 0, count: 300)
    img[0] = first
    img.replaceSubrange(32..<36, with: magic)
    func put(_ s: String, _ off: Int) { let b = Array(s.utf8); img.replaceSubrange(off..<off + b.count, with: b) }
    put(version, 32 + 16); put(project, 32 + 48); put(time, 32 + 80); put(date, 32 + 96)
    img.replaceSubrange(32 + 144..<32 + 176, with: (0..<32).map { UInt8(255 - $0) })
    return img
}
let imgCases: [(String, Data)] = [
    ("full_width", image(String(repeating: "v", count: 32), String(repeating: "p", count: 32),
                         String(repeating: "t", count: 16), String(repeating: "d", count: 16))),
    ("jc3248", image("6ac87a5-dirty", "jc3248_pet_idf", "13:23:37", "Oct 21 2026")),
    ("not_e9", image("1", "oraclepet_lcd28", "1", "1", first: 0xE8)),
    ("bad_magic", image("1", "oraclepet_lcd28", "1", "1", magic: [0x32, 0x54, 0xCD, 0xAA])),
]
v["image"] = imgCases.map { c -> [String: Any] in
    guard let f = OTA.describe(c.1) else { return ["name": c.0, "hex": hex(c.1), "valid": false] }
    return ["name": c.0, "hex": hex(c.1), "valid": true, "project": f.project, "version": f.version,
            "built": f.built, "sha16": f.sha, "pet_project": OTA.petProjects[f.project] != nil,
            "board": OTA.boardName(f.project)]
}

// Send window: one scripted run of next/ack (nil = nothing to send now)
var w = OTAWindow(total: 10_000, window: 4096)
var steps: [[String: Any]] = []
func next(_ maxLen: Int) {
    let r = w.next(maxLen: maxLen)
    steps.append(["op": "next", "arg": maxLen, "off": r?.lowerBound ?? -1, "len": r?.count ?? 0,
                  "sent": w.sent, "acked": w.acked, "done": w.allAcked])
}
func ack(_ off: Int) {
    w.ack(off)
    steps.append(["op": "ack", "arg": off, "sent": w.sent, "acked": w.acked, "done": w.allAcked])
}
for _ in 0..<20 { next(244) }
ack(1000); next(244); next(0); ack(500); next(5000); ack(9000); next(4096); next(4096)
ack(12000); next(10); ack(10000); next(10)
v["window"] = ["total": 10_000, "window": 4096, "steps": steps]
var w0 = OTAWindow(total: 0, window: 4096)
v["window_empty"] = ["done_at_start": w0.allAcked, "next_len": w0.next(maxLen: 100)?.count ?? 0]

// Status lines as Swift's OTAStatus reads them ("none" = nil)
let statusLines = [
    #"{"ota":"done"}"#,
    #"{"ota":"saved","path":"characters/neo/idle.gif"}"#,
    #"{"ota":"pack","name":"siamese"}"#,
    #"{"ota":"app","name":""}"#,
    #"{"ota":"play","name":"say","bytes":4112}"#,
    #"{"ota":"selftest","mic":true,"heard":true,"db":31.5,"quiet":12,"tone":800}"#,
    #"{"ota":"error","why":"app talker: trap \"unreachable\""}"#,
    #"{"ota":"error"}"#,
    #"{"ota":"ready","size":5}"#,
    #"{"ota":"ack"}"#,
    #"{"ota":"bogus"}"#,
    #"{"ver":"6ac87a5-dirty","project":"oraclepet_lcd28"}"#,
    #"  { "ota" : "ack" , "off" : 16384 }  "#,
    #"{"off":1,"ota":"ack"}"#,
    #"hello"#,
    #"{"ota":"streamed","bytes":14000,"crc":false}"#,
    #"{"ota":"ready","size":100,"extra":{"a":[1,2,{"b":"}"}]},"window":512}"#,
    #"{"ota":"error","why":"café 👍"}"#,
    #"{"ota":"ack","off":-3}"#,
    #"{"ota":7}"#,
    #"{"ota":"heard","ms":3000,"samples":48000,"rms":412,"peak":9120,"freq":1000,"db":23.4,"floor":31,"sent":true}"#,
    #"{"ota":"heard","ms":100,"samples":1600,"rms":0,"peak":0,"freq":150,"db":-3,"floor":0,"sent":false}"#,
    #"{"ota":"heard"}"#,
    #"{"ota":"selftest","mic":false,"heard":false,"db":0.0,"quiet":0,"tone":0}"#,
    // malformed or unusual: a cut notification must not read as a smaller number
    #"{"ota":"ack","off":40"#,
    #"{"ota":"ack","off":4096} x"#,
    #"{"ota":"ack","off":012}"#,
    #"{"ota":"ack","off":1,"off":2}"#,
    #"{"ota":"ack","off":1e3}"#,
    #"{"ota":"ack","off":4096.0}"#,
    #"{"ota":"ack","off":4096.5}"#,
    #"{"ota":"ack","off":"4096"}"#,
    #"{"ota":"ack","off":4096,}"#,
    #"{"ota":"ack" "off":4096}"#,
    #"{"ota":"error","why":"x\q"}"#,
    #"{"ota":"error","why":"\ud83d"}"#,
    "{\"ota\":\"error\",\"why\":\"tab\there\"}",
    #"{"ota":"streamed","bytes":10,"crc":1}"#,
    #"{"ota":"ready","size":100,"window":16384,"nested":{"a":[true,false,null,-0.5e-3,"]"]}}"#,
    #"{"ota":"ready","size":100,"window":16384,"bad":[1,,2]}"#,
    #"{"ota":"ready","size":100,"window":16384,"bad":tru}"#,
    #"[{"ota":"done"}]"#,
    #"{"ota":"ready","size":100,"window":16384,"arr":[1,2,]}"#,
    #"{"ota":"ack",,"off":5}"#,
    #"{,"ota":"done"}"#,
    #"{"ota":"done",}"#,
    #"{"ota":"ack","off":+5}"#,
    #"{"ota":"ack","off":-0}"#,
    #"{"ota":"ack","off":5.}"#,
    #"{"ota":"ack","off":.5}"#,
    #"{'ota':'done'}"#,
    #"{"ota":"pack","name":"é\/\u0000x"}"#,
    #"{"ota":"error","why":"\udc4d"}"#,
    #"{"ota":"error","why":"\ud83dA"}"#,
    "{\"ota\":\"done\"}\u{0}",
    "\u{FEFF}{\"ota\":\"done\"}",
    "{\"ota\":\"done\"}\n",
    #"{"ota":"done"} {"ota":"done"}"#,
    #"{"ota":"ack","off":4096}//"#,
    #"{"ota":"pack","name":"a\u0000b"}"#,
]
v["status"] = statusLines.map { line -> [String: Any] in
    var o: [String: Any] = ["line": line, "kind": "none"]
    guard let st = OTAStatus.parse(Data(line.utf8)) else { return o }
    switch st {
    case let .ready(size, window): o["kind"] = "ready"; o["size"] = size; o["window"] = window
    case let .ack(off): o["kind"] = "ack"; o["off"] = off
    case .done: o["kind"] = "done"
    case let .saved(path): o["kind"] = "saved"; o["name"] = path
    case let .pack(name): o["kind"] = "pack"; o["name"] = name
    case let .app(name): o["kind"] = "app"; o["name"] = name
    case let .play(name): o["kind"] = "play"; o["name"] = name
    case let .streamed(bytes, crcOK): o["kind"] = "streamed"; o["bytes"] = bytes; o["crc"] = crcOK
    case let .heard(m):
        o["kind"] = "heard"; o["ms"] = m.ms; o["samples"] = m.samples; o["rms"] = m.rms; o["peak"] = m.peak
        o["freq"] = m.freq; o["db"] = m.db; o["floor"] = m.floor; o["sent"] = m.sent
    case let .selftest(mic, heard, db, quiet, tone):
        o["kind"] = "selftest"; o["mic"] = mic; o["heard"] = heard; o["db"] = db; o["quiet"] = quiet; o["tone"] = tone
    case let .error(why): o["kind"] = "error"; o["why"] = why
    }
    return o
}

// WAV for a saved recording (PetSound.wav): the 44-byte header, then the samples
v["wav"] = [("three", [Int16]([0, 1, -1])), ("empty", [Int16]())].map { c -> [String: Any] in
    ["name": c.0, "pcm": c.1.map { Int($0) }, "hex": hex(PetSound.wav(c.1))]
}

let out = try! JSONSerialization.data(withJSONObject: v, options: [.prettyPrinted, .sortedKeys])
FileHandle.standardOutput.write(out)
