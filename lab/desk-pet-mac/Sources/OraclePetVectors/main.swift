// Generates golden/vectors.json from the Swift PetProtocol that ships in OraclePet (the
// implementation the pets already accept). Every other implementation must reproduce it.
//   lab/oraclepet-lib/tools/gen_vectors.sh   (swift run OraclePetVectors > golden/vectors.json)
import Foundation
import PetProtocol

func hex(_ d: Data) -> String { d.map { String(format: "%02x", $0) }.joined() }

var v: [String: Any] = [:]
v["protocol"] = 1
v["generated_by"] = "lab/desk-pet-mac Sources/PetProtocol (Swift), lab/desk-pet-mac Sources/OraclePetVectors"
v["ima_step"] = PetSound.stepTable
v["ima_index"] = PetSound.indexTable

v["crc32"] = ["", "123456789", "OraclePet", "สวัสดี"].map { s -> [String: Any] in
    ["text_utf8": s, "crc32": OTA.crc32(Data(s.utf8))]
}

v["commands"] = [
    ["name": "begin", "args": ["size": 0x0A0B0C, "crc": 0xDEADBEEF], "hex": hex(OTA.begin(size: 0x0A0B0C, crc: 0xDEADBEEF))],
    ["name": "begin_file", "args": ["size": 1000, "crc": 0x12345678, "path": "characters/neo/idle.gif"],
     "hex": hex(OTA.beginFile(size: 1000, crc: 0x12345678, path: "characters/neo/idle.gif"))],
    ["name": "begin_stream", "args": ["size": 4096, "crc": 0xCAFEBABE], "hex": hex(OTA.beginStream(size: 4096, crc: 0xCAFEBABE))],
    ["name": "end", "args": [String: Any](), "hex": hex(OTA.end)],
    ["name": "abort", "args": [String: Any](), "hex": hex(OTA.abort)],
    ["name": "select_pack", "args": ["name": "neo"], "hex": hex(OTA.selectPack("neo"))],
    ["name": "run_app", "args": ["name": "talker"], "hex": hex(OTA.runApp("talker"))],
    ["name": "run_app", "args": ["name": ""], "hex": hex(OTA.runApp(""))],
    ["name": "progress", "args": ["pct": 42], "hex": hex(OTA.progress(42))],
    ["name": "play", "args": ["name": "say"], "hex": hex(OTA.play("say"))],
    ["name": "selftest", "args": [String: Any](), "hex": hex(OTA.selftest)],
    ["name": "listen", "args": ["ms": 3000, "send_audio": true], "hex": hex(OTA.listen(ms: 3000, sendAudio: true))],
    ["name": "listen", "args": ["ms": 500, "send_audio": false], "hex": hex(OTA.listen(ms: 500, sendAudio: false))],
]

let hb = Heartbeat(total: 3, running: 1, waiting: 2, msg: "wait: neo, \"x\"", epoch: 1792547421, tzoff: 25200,
                   pct5h: 8, reset5h: 14100, who: "neo")
v["heartbeat"] = ["args": ["total": 3, "running": 1, "waiting": 2, "msg": "wait: neo, \"x\"", "epoch": 1792547421,
                           "tzoff": 25200, "pct5h": 8, "reset5h": 14100, "who": "neo"],
                  "line": String(decoding: hb.jsonLine(), as: UTF8.self)]

// PSND clips: a 440 Hz tone (1000 samples) and an edge case (odd count, extremes)
let tone = (0..<1000).map { Int16(12_000 * sin(2 * Double.pi * 440 * Double($0) / 16_000)) }
let edge: [Int16] = [0, 1000, -1000, 32767, -32768, 5, -5, 300, 0]
var clips: [[String: Any]] = []
for (name, pcm) in [("tone440", tone), ("edge", edge)] {
    let c = PetSound.clip(pcm)
    clips.append(["name": name, "pcm": pcm.map { Int($0) }, "clip_hex": hex(c),
                  "decoded": (PetSound.decode(c) ?? []).map { Int($0) }])
}
v["psnd"] = clips

// firmware image description (a synthetic 300-byte image)
var img = Data(repeating: 0, count: 300)
img[0] = 0xE9
img.replaceSubrange(32..<36, with: [0x32, 0x54, 0xCD, 0xAB])
func put(_ s: String, _ off: Int) { let b = Array(s.utf8); img.replaceSubrange(off..<off + b.count, with: b) }
put("v1.2.3", 32 + 16); put("oraclepet_lcd28", 32 + 48); put("12:34:56", 32 + 80); put("Oct 21 2026", 32 + 96)
img.replaceSubrange(32 + 144..<32 + 176, with: (0..<32).map { UInt8($0) })
let info = OTA.describe(img)!
v["image"] = ["hex": hex(img), "project": OTA.projectName(of: img)!, "version": info.version, "built": info.built, "sha16": info.sha]

// status lines the pet sends, and what they mean
v["status"] = [
    ["line": #"{"ota":"ready","size":100,"window":16384}"#, "kind": "ready"],
    ["line": #"{"ota":"ack","off":4096}"#, "kind": "ack"],
    ["line": #"{"ota":"streamed","bytes":14000,"crc":true}"#, "kind": "streamed"],
    ["line": #"{"ota":"heard","ms":3000,"samples":48000,"rms":412,"peak":9120,"freq":1000,"db":23.4,"floor":31}"#, "kind": "heard"],
    ["line": #"{"ota":"error","why":"no mic"}"#, "kind": "error"],
]
v["uuids"] = ["nus_service": "6e400001-b5a3-f393-e0a9-e50e24dcca9e", "nus_rx": "6e400002-b5a3-f393-e0a9-e50e24dcca9e",
              "nus_tx": "6e400003-b5a3-f393-e0a9-e50e24dcca9e", "ota_service": "6e400010-b5a3-f393-e0a9-e50e24dcca9e",
              "ota_control": "6e400011-b5a3-f393-e0a9-e50e24dcca9e", "ota_data": "6e400012-b5a3-f393-e0a9-e50e24dcca9e",
              "ota_mic": "6e400013-b5a3-f393-e0a9-e50e24dcca9e"]
v["projects"] = OTA.petProjects.keys.sorted()

let out = try! JSONSerialization.data(withJSONObject: v, options: [.prettyPrinted, .sortedKeys])
FileHandle.standardOutput.write(out)
