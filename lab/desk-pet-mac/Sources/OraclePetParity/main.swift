// Edge-case vectors from the Swift PetProtocol, beyond golden/vectors.json: PSND lengths around
// the 504-sample block, clamping extremes, tidy() rounding, heartbeat escaping and clipping
// (Thai, emoji, controls, empty strings), name and path length limits, LISTEN/PROGRESS clamps.
// Compiled together with the shipping Swift sources, so the answers are the Mac app's own:
//
//   tests/swift/gen_parity.sh      (writes tests/parity_vectors.json; needs swiftc, macOS)
//
// tests/test_parity.py checks the Python library against the JSON it writes.
import Foundation
import PetProtocol

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

// deterministic noise: an LCG, so the same file comes out on every run
var seed: UInt32 = 0x1234_5678
func rnd() -> Int { seed = seed &* 1_664_525 &+ 1_013_904_223; return Int(seed >> 16) }   // 0..65535

func pcm(_ n: Int, _ kind: String) -> [Int16] {
    switch kind {
    case "sine": return (0..<n).map { Int16(9_000 * sin(2 * Double.pi * 700 * Double($0) / 16_000)) }
    case "noise": return (0..<n).map { _ in Int16(truncatingIfNeeded: rnd() - 32768) }
    case "square": return (0..<n).map { ($0 / 7) % 2 == 0 ? Int16.max : Int16.min }   // drives the clamps
    default: return [Int16](repeating: 0, count: n)
    }
}

var v: [String: Any] = [:]

var clips: [[String: Any]] = []
for n in [0, 1, 2, 503, 504, 505, 1007, 1008, 1009] {
    for kind in ["sine", "noise", "square"] where !(n == 0 && kind != "sine") {
        let p = pcm(n, kind)
        let c = PetSound.clip(p)
        clips.append(["name": "\(kind)\(n)", "pcm": p.map { Int($0) }, "clip_hex": hex(c),
                      "decoded": (PetSound.decode(c) ?? []).map { Int($0) }])
    }
}
v["psnd"] = clips

// decode of damaged / truncated clips
let base = PetSound.clip(pcm(1009, "noise"))
var damaged: [[String: Any]] = []
for cut in [15, 16, 17, 19, 20, 21, 272, 273, 400, 528, base.count - 1] {
    let d = base.prefix(cut)
    damaged.append(["clip_hex": hex(Data(d)), "decoded": PetSound.decode(Data(d)).map { $0.map { Int($0) } } as Any])
}
var bigIndex = base
bigIndex[16 + 2] = 200                                      // step index out of range: clamped to 88
damaged.append(["clip_hex": hex(bigIndex), "decoded": PetSound.decode(bigIndex).map { $0.map { Int($0) } } as Any])
v["psnd_damaged"] = damaged

var tidies: [[String: Any]] = []
let tidyInputs: [[Int16]] = [
    [], [0, 0, 0], [300, -300], [301], [0, 0, 1000, -2000, 0, 0],
    [Int16](repeating: 0, count: 500) + [400, 3, -401] + [Int16](repeating: 0, count: 500),
    [Int16.min, 1000, 3], [Int16.max, -1], [1, 2, 3, 333, 777, 999, -12345, 3],
]
for p in tidyInputs {
    tidies.append(["pcm": p.map { Int($0) }, "tidy": PetSound.tidy(p).map { Int($0) }])
}
for g in [0.5, 1.0, 0.3] {
    let p = (0..<200).map { Int16(truncatingIfNeeded: ($0 * 977) % 4001 - 2000) }
    tidies.append(["pcm": p.map { Int($0) }, "peak": g, "tidy": PetSound.tidy(p, peak: g).map { Int($0) }])
}
for secs in [0.05, 0.0625, 0.1] {                           // the length cap, scaled down
    let p = pcm(2_000, "sine")
    tidies.append(["pcm": p.map { Int($0) }, "max_seconds": secs, "tidy": PetSound.tidy(p, maxSeconds: secs).map { Int($0) }])
}
v["tidy"] = tidies

let strings = [
    "", "a", "plain ascii", "q\"uote", "back\\slash", "new\nline", "tab\there", "cr\rlf\r\n",
    "nul\u{0}x", "\u{1}\u{1f}\u{7f}\u{80}", "สวัสดีครับ", "กำลังทำงาน: เขียนโค้ดให้แมวตัวน้อยบนโต๊ะทำงานของนัท",
    "น้ำ ที่ ไม่ ได้ สั่ง ก็ ไม่ ต้อง ดื่ม ครับ ผม", "👍🏽👨‍👩‍👧‍👦🇹🇭🇯🇵", "e\u{301}e\u{301}e\u{301}",
    String(repeating: "x", count: 62) + "ก", String(repeating: "x", count: 61) + "ก", String(repeating: "x", count: 60) + "ก",
    String(repeating: "x", count: 63), String(repeating: "x", count: 64), String(repeating: "ห", count: 22),
    "한국어 텍스트 입니다 한국어 텍스트 입니다", "\u{2028}sep\u{2029}", "zwj\u{200d}x", "1\u{fe0f}\u{20e3} keycap",
    String(repeating: "ก่", count: 15), "neo-oracle-with-a-very-long-name",
]
var beats: [[String: Any]] = []
for s in strings {
    let hb = Heartbeat(total: 1, running: 0, waiting: 0, msg: s, epoch: 1792547421, tzoff: -18000, who: s)
    beats.append(["text": s, "msg": hb.msg, "who": hb.who ?? "", "line": String(decoding: hb.jsonLine(), as: UTF8.self)])
}
let full = Heartbeat(total: 0, running: 0, waiting: 0, msg: "", epoch: 0, tzoff: 0,
                     pct5h: 0, reset5h: 1, pct7d: 100, reset7d: 604_800, who: "")
beats.append(["text": "", "msg": "", "who": "", "all_optional": true, "line": String(decoding: full.jsonLine(), as: UTF8.self)])
v["heartbeat"] = beats

var cmds: [[String: Any]] = []
for name in ["", "a", String(repeating: "z", count: 23), String(repeating: "z", count: 24), "แมว", "neo_oracle-2"] {
    cmds.append(["name": "select_pack", "args": ["name": name], "hex": hex(OTA.selectPack(name))])
    cmds.append(["name": "run_app", "args": ["name": name], "hex": hex(OTA.runApp(name))])
    cmds.append(["name": "play", "args": ["name": name], "hex": hex(OTA.play(name))])
}
for path in ["characters/p/" + String(repeating: "f", count: 82), "apps/" + String(repeating: "a", count: 23) + ".wasm",
             "sounds/x.ima", "characters/แมว/idle.gif"] {
    cmds.append(["name": "begin_file", "args": ["size": 0, "crc": 0, "path": path], "hex": hex(OTA.beginFile(size: 0, crc: 0, path: path))])
}
for (size, crc) in [(0, UInt32(0)), (1, UInt32.max), (0xFFFF_FFFF, UInt32(0x0102_0304))] {
    cmds.append(["name": "begin", "args": ["size": size, "crc": crc], "hex": hex(OTA.begin(size: size, crc: crc))])
    cmds.append(["name": "begin_stream", "args": ["size": size, "crc": crc], "hex": hex(OTA.beginStream(size: size, crc: crc))])
}
for ms in [-1, 0, 99, 100, 101, 255, 256, 9999, 10_000, 10_001, 70_000] {
    for a in [true, false] {
        cmds.append(["name": "listen", "args": ["ms": ms, "send_audio": a], "hex": hex(OTA.listen(ms: ms, sendAudio: a))])
    }
}
for pct in [-5, 0, 1, 99, 100, 101, 255, 1000] {
    cmds.append(["name": "progress", "args": ["pct": pct], "hex": hex(OTA.progress(pct))])
}
v["commands"] = cmds

var crcs: [[String: Any]] = []
for n in [0, 1, 3, 255, 256, 4096, 10_001] {
    let d = Data((0..<n).map { _ in UInt8(truncatingIfNeeded: rnd()) })
    crcs.append(["hex": hex(d), "crc32": OTA.crc32(d)])
}
v["crc32"] = crcs

// status lines: what OTAStatus.parse makes of odd but legal JSON
var stats: [[String: Any]] = []
for line in [
    #"{"ota":"ready","size":0,"window":512}"#, #"{"ota":"ready"}"#, #"{"ota":"ack"}"#, #"{"ota":"ack","off":0}"#,
    #"{"ota":"streamed","bytes":0,"crc":false}"#, #"{"ota":"heard","ms":100,"samples":1600,"rms":0,"peak":0,"freq":150,"db":7,"floor":0,"sent":false}"#,
    #"{"ota":"selftest","mic":false,"heard":false,"db":0.0,"quiet":0,"tone":0}"#, #"{"ota":"error"}"#,
    #"{"ota":"saved","path":"characters/neo/idle.gif"}"#, #"{"ota":"play","name":"say","bytes":272}"#,
    #"{"ota":"nope"}"#, #"{"evt":"tap"}"#, #"not json"#, #"[1,2]"#,
] {
    let st = OTAStatus.parse(Data(line.utf8))
    stats.append(["line": line, "swift": st.map { "\($0)" } ?? "nil"])
}
v["status"] = stats

let out = try! JSONSerialization.data(withJSONObject: v, options: [.sortedKeys])
FileHandle.standardOutput.write(out)
