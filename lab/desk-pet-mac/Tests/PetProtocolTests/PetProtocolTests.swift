import Foundation
import Testing
@testable import PetProtocol

@Suite struct PetStateTests {
    @Test func firmwareRule() {
        #expect(PetState(total: 3, running: 2, waiting: 1) == .attention)   // waiting wins
        #expect(PetState(total: 3, running: 2, waiting: 0) == .busy)
        #expect(PetState(total: 0, running: 0, waiting: 0) == .sleep)
        #expect(PetState(total: 1, running: 0, waiting: 0) == .idle)
    }

    @Test func manualCountsRoundTrip() {
        for s in PetState.allCases {
            let c = s.counts
            #expect(PetState(total: c.total, running: c.running, waiting: c.waiting) == s)
        }
    }
}

@Suite struct HeartbeatTests {
    @Test func exactLine() {
        let hb = Heartbeat(total: 3, running: 1, waiting: 2, msg: "wait: neo, \"x\"",
                           epoch: 1_792_547_421, tzoff: 25_200, pct5h: 8, reset5h: 14_100)
        let line = String(decoding: hb.jsonLine(), as: UTF8.self)
        #expect(line == #"{"total":3,"running":1,"waiting":2,"msg":"wait: neo, \"x\"","epoch":1792547421,"tzoff":25200,"pct5h":8,"reset5h":14100}"# + "\n")
        // It is real JSON, and one line.
        #expect((try? JSONSerialization.jsonObject(with: hb.jsonLine())) != nil)
        #expect(line.filter { $0 == "\n" }.count == 1)
    }

    @Test func msgFitsFirmwareBuffer() {
        let thai = String(repeating: "แมว", count: 40)             // 3 bytes per character
        let hb = Heartbeat(total: 1, running: 0, waiting: 0, msg: thai, epoch: 0, tzoff: 0)
        #expect(hb.msg.utf8.count <= Heartbeat.msgLimit)
        #expect(thai.hasPrefix(hb.msg))                              // clipped on a character boundary
    }

    @Test func usageFromStatusline() {
        let now = Date(timeIntervalSince1970: 1_000)
        let json = #"{"rate_limits":{"five_hour":{"used_percentage":6,"resets_at":4600},"seven_day":{"used_percentage":25}}}"#
        let u = Usage.parse(statusline: Data(json.utf8), now: now)
        #expect(u == Usage(pct5h: 6, reset5h: 3600, pct7d: 25, reset7d: nil))
        #expect(Usage.parse(statusline: Data("nope".utf8), now: now) == Usage())
    }
}

@Suite struct FleetTallyTests {
    let list = #"""
    {"id":"cli:agent:list","result":{"agents":[
      {"agent_status":"working","pane_id":"w1:p1","cwd":"/opt/Code/github.com/laris-co/nexus-oracle"},
      {"agent_status":"blocked","pane_id":"w2:p1","cwd":"/x/neo-oracle","name":"neo"},
      {"agent_status":"idle","pane_id":"w3:p1","cwd":"/x/homelab"}
    ]},"type":"agent_list"}
    """#

    @Test func parsesHerdr() throws {
        let agents = try #require(Agent.parse(herdrList: Data(list.utf8)))
        #expect(agents.map(\.label) == ["nexus-oracle", "neo", "homelab"])
        #expect(Agent.parse(herdrList: Data("{}".utf8)) == nil)
    }

    @Test func busyThenQuietThenAsleep() throws {
        var t = FleetTally(quiet: 300)
        let agents = try #require(Agent.parse(herdrList: Data(list.utf8)))
        let t0 = Date(timeIntervalSince1970: 0)

        let r0 = t.update(agents, now: t0)
        #expect((r0.total, r0.running, r0.waiting) == (2, 1, 1))
        #expect(r0.msg == "wait: neo")
        #expect(r0.waitingAgents.map(\.pane) == ["w2:p1"])
        #expect(r0.workingAgents.map(\.pane) == ["w1:p1"])
        #expect(r0.peekable.map(\.pane) == ["w2:p1"])               // waiting first
        #expect(PetState(total: r0.total, running: r0.running, waiting: r0.waiting) == .attention)

        let allIdle = agents.map { Agent(pane: $0.pane, status: "idle", label: $0.label) }
        let r1 = t.update(allIdle, now: t0.addingTimeInterval(60))
        #expect(PetState(total: r1.total, running: r1.running, waiting: r1.waiting) == .idle)
        #expect(r1.msg == "fleet idle")

        let r2 = t.update(allIdle, now: t0.addingTimeInterval(301))
        #expect(PetState(total: r2.total, running: r2.running, waiting: r2.waiting) == .sleep)
        #expect(r2.msg == "fleet asleep")
    }

    @Test func closedPanesAreForgotten() {
        var t = FleetTally(quiet: 300)
        let t0 = Date(timeIntervalSince1970: 0)
        _ = t.update([Agent(pane: "a", status: "working", label: "a")], now: t0)
        let r = t.update([], now: t0.addingTimeInterval(1))
        #expect(r.total == 0)
    }
}

@Suite struct HerdrKeysTests {
    @Test func arrowsEnterTextCtrl() {
        let up: [UInt8] = [0x1B, 0x5B, 0x41], down: [UInt8] = [0x1B, 0x4F, 0x42]
        #expect(HerdrKeys.calls(for: up) == [["send-keys", "up"]])
        #expect(HerdrKeys.calls(for: down) == [["send-keys", "down"]])          // application cursor mode
        #expect(HerdrKeys.calls(for: Array("2".utf8) + [0x0D]) == [["send-text", "2"], ["send-keys", "enter"]])
        #expect(HerdrKeys.calls(for: [0x1B]) == [["send-keys", "esc"]])
        #expect(HerdrKeys.calls(for: [0x7F]) == [["send-keys", "backspace"]])
        #expect(HerdrKeys.calls(for: [0x03]) == [["send-text", "\u{03}"]])       // ctrl-c, as the raw byte
    }
}

@Suite struct OTATests {
    @Test func crcMatchesZlib() {
        #expect(OTA.crc32(Data("123456789".utf8)) == 0xCBF4_3926)       // the standard check value
        #expect(OTA.crc32(Data()) == 0)
    }

    @Test func beginIsLittleEndian() {
        #expect(OTA.begin(size: 0x0A0B0C, crc: 0xDEADBEEF) == Data([0x01, 0x0C, 0x0B, 0x0A, 0x00, 0xEF, 0xBE, 0xAD, 0xDE]))
    }

    @Test func statusLines() {
        #expect(OTAStatus.parse(Data(#"{"ota":"ready","size":100,"window":16384}"#.utf8)) == .ready(size: 100, window: 16384))
        #expect(OTAStatus.parse(Data(#"{"ota":"ack","off":4096}"#.utf8)) == .ack(4096))
        #expect(OTAStatus.parse(Data(#"{"ota":"error","why":"crc mismatch"}"#.utf8)) == .error("crc mismatch"))
        #expect(OTAStatus.parse(Data(#"{"evt":"tap"}"#.utf8)) == nil)
        #expect(PetFirmware.parse(Data(#"{"ver":"v1-3","slot":"ota_1","built":"Oct 21 2026 11:31:00"}"#.utf8))?.slot == "ota_1")
    }

    @Test func windowNeverRunsAhead() {
        var w = OTAWindow(total: 10_000, window: 4096)
        var sent = 0
        while let r = w.next(maxLen: 512) { sent += r.count }
        #expect(sent == 4096)                                            // window full, waits for an ack
        w.ack(2048)
        #expect(w.next(maxLen: 4096) == 4096..<6144)                     // only what the ack freed
        w.ack(99_999)                                                   // never past what was sent
        #expect(w.acked == 6144)
        while let _ = w.next(maxLen: 512) {}
        w.ack(10_000)
        #expect(w.allAcked && w.sent == 10_000)
    }

    @Test func projectNameFromImage() {
        var img = Data(repeating: 0, count: 300)
        img[0] = 0xE9
        img.replaceSubrange(32..<36, with: [0x32, 0x54, 0xCD, 0xAB])
        img.replaceSubrange(80..<94, with: Data("jc3248_pet_idf".utf8))
        #expect(OTA.projectName(of: img) == "jc3248_pet_idf")
        #expect(OTA.projectName(of: Data(repeating: 0, count: 300)) == nil)
    }
}

@Suite struct AgentProjectTests {
    @Test func worktreeMapsToRepo() {
        #expect(Agent.project(ofCwd: "/opt/Code/github.com/laris-co/nexus-oracle/wt/sarafim-x-21oct") == "nexus-oracle")
        #expect(Agent.project(ofCwd: "/opt/Code/github.com/Soul-Brews-Studio/herdr-orca-oracle") == "herdr-orca-oracle")
        #expect(Agent(pane: "w:p1", status: "working", label: "neo").project == "neo")   // no cwd: the label
    }
}

@Suite struct OTADescribeTests {
    @Test func realBuildIfPresent() throws {
        let url = URL(fileURLWithPath: #filePath).deletingLastPathComponent()
            .appendingPathComponent("../../../jc3248-pet-idf/build/jc3248_pet_idf.bin").standardized
        guard let img = try? Data(contentsOf: url) else { return }        // no firmware build here
        let d = try #require(OTA.describe(img))
        #expect(!d.version.isEmpty && d.built.contains("20"))
    }
}

@Suite struct WhoTests {
    @Test func waitingBeatsWorkingAndOracleIsDropped() {
        var t = FleetTally()
        let r = t.update([Agent(pane: "a", status: "working", label: "x", project: "nexus-oracle"),
                          Agent(pane: "b", status: "working", label: "y", project: "neo-oracle")], now: Date())
        #expect(r.who == "nexus +1")
        let r2 = t.update([Agent(pane: "a", status: "working", label: "x", project: "nexus-oracle"),
                           Agent(pane: "b", status: "blocked", label: "y", project: "neo-oracle")], now: Date())
        #expect(r2.who == "neo")
        let hb = Heartbeat(total: 1, running: 1, waiting: 0, msg: "", epoch: 0, tzoff: 0, who: "nexus")
        #expect(String(decoding: hb.jsonLine(), as: UTF8.self).contains(#""who":"nexus"}"#))
    }
}

@Suite struct SameBuildTests {
    @Test func shaPrefix() {
        let pet = PetFirmware(version: "v", slot: "ota_1", built: "a", sha: "e5893e747")
        #expect(pet.sameBuild(as: PetFirmware(version: "v", slot: "", built: "a", sha: "e5893e7471c0ffee")))
        #expect(!pet.sameBuild(as: PetFirmware(version: "v", slot: "", built: "a", sha: "bd09fced0359d3da")))
    }
}

@Suite struct PartialOTATests {
    @Test func fileAndSelectCommands() {
        let b = OTA.beginFile(size: 1, crc: 2, path: "characters/neo/idle.gif")
        #expect(b[0] == 0x04 && b[9] == 23 && b.suffix(23) == Data("characters/neo/idle.gif".utf8))
        #expect(OTA.selectPack("neo") == Data([0x05, 3]) + Data("neo".utf8))
        #expect(OTAStatus.parse(Data(#"{"ota":"pack","name":"neo"}"#.utf8)) == .pack("neo"))
        let fw = PetFirmware.parse(Data(#"{"ver":"v","slot":"ota_0","pack":"neo","packs":["siamese","neo"],"free":900}"#.utf8))
        #expect(fw?.packs == ["neo", "siamese"] && fw?.freeKB == 900 && fw?.pack == "neo")
    }
}

@Suite struct BoardTests {
    @Test func projectIdentifiesTheBoard() {
        let fw = PetFirmware.parse(Data(#"{"ver":"v","slot":"ota_0","project":"oraclepet_lcd28"}"#.utf8))
        #expect(fw?.project == "oraclepet_lcd28")
        #expect(OTA.boardName("oraclepet_lcd28") == "2.8in ILI9341")
        #expect(OTA.boardName("jc3248_pet_idf") == "JC3248 3.5in")
        #expect(OTA.petProjects["esp32_other"] == nil)
    }
}

@Suite struct PetSoundTests {
    @Test func headerAndBlocks() {
        let pcm = [Int16](repeating: 0, count: 1000)
        let c = PetSound.clip(pcm)
        #expect([UInt8](c.prefix(16)) == [0x50, 0x53, 0x4E, 0x44, 1, 1, 0, 0, 0x80, 0x3E, 0, 0, 0xE8, 0x03, 0, 0])
        #expect(c.count == 16 + 2 * 256)                          // 1000 samples = 2 blocks of 504
    }

    @Test func roundTripToneKeepsItsShape() throws {
        let pcm = (0..<16_000).map { Int16(16_000 * sin(2 * Double.pi * 440 * Double($0) / 16_000)) }
        let back = try #require(PetSound.decode(PetSound.clip(pcm)))
        #expect(back.count == pcm.count)
        var sig = 0.0, err = 0.0
        for (a, b) in zip(pcm, back) { sig += Double(a) * Double(a); err += Double(Int(a) - Int(b)) * Double(Int(a) - Int(b)) }
        let snr = 10 * log10(sig / max(err, 1))
        #expect(snr > 20, "SNR \(snr) dB")
    }

    @Test func lowNibbleFirst() {
        // one rising sample: the first nibble lands in the low half of the first data byte
        let c = [UInt8](PetSound.clip([1000, 1000]))
        #expect(c[16 + 4] & 0x0F != 0 && c[16 + 4] >> 4 != 0)
    }

    @Test func tidyTrimsAndScales() {
        let pcm: [Int16] = Array(repeating: 0, count: 1000) + [10_000, -10_000, 5_000] + Array(repeating: 0, count: 1000)
        let t = PetSound.tidy(pcm)
        #expect(t.count < pcm.count && t.count >= 3)
        #expect(t.map { abs(Int($0)) }.max() == Int((32767 * 0.9).rounded()))
        #expect(PetSound.isThai("นีโอรอคุณอยู่") && !PetSound.isThai("neo needs you"))
    }
}

@Suite struct StreamTests {
    @Test func beginStreamAndStatus() {
        #expect(OTA.beginStream(size: 0x0102, crc: 1)[0] == 0x0B && OTA.beginStream(size: 0x0102, crc: 1).count == 9)
        #expect(OTAStatus.parse(Data(#"{"ota":"streamed","bytes":14000,"crc":true}"#.utf8)) == .streamed(bytes: 14000, crcOK: true))
        #expect(OTAStatus.parse(Data(#"{"ota":"selftest","mic":true,"heard":true,"db":37.5,"quiet":22,"tone":191}"#.utf8))
                == .selftest(mic: true, heard: true, db: 37.5, quiet: 22, tone: 191))
    }
}

@Suite struct MicTests {
    @Test func listenCommandAndHeard() {
        #expect(OTA.listen(ms: 3000, sendAudio: true) == Data([0x0C, 0xB8, 0x0B, 1]))
        #expect(OTA.listen(ms: 50, sendAudio: false) == Data([0x0C, 0x64, 0x00, 0]))      // clamped to 100 ms
        guard case let .heard(m)? = OTAStatus.parse(Data(#"{"ota":"heard","ms":3000,"samples":48000,"rms":412,"peak":9120,"freq":1000,"db":23.4,"floor":31,"sent":true}"#.utf8)) else {
            Issue.record("heard not parsed"); return
        }
        #expect(m.freq == 1000 && m.rms == 412 && m.sent && abs(m.db - 23.4) < 0.01)
    }

    @Test func wavHeader() {
        let w = PetSound.wav([0, 1, -1])
        #expect(w.count == 44 + 6 && w.prefix(4) == Data("RIFF".utf8) && w[8..<12] == Data("WAVE".utf8))
    }
}
