import Foundation
import Testing
@testable import PetProtocol

@Suite struct VoiceWiFiTests {
    @Test func improvWifiSettingsBytes() {
        // improv-wifi.com example layout: 01, len, 4 "ssid", 4 "pass", checksum
        let d = [UInt8](Improv.wifiSettings(ssid: "ssid", password: "pass")!)
        #expect(Array(d.prefix(2)) == [0x01, 10])
        #expect(Array(d[2...11]) == [4] + Array("ssid".utf8) + [4] + Array("pass".utf8))
        #expect(d.last == UInt8(truncatingIfNeeded: d.dropLast().reduce(0) { $0 + Int($1) }))
        #expect(Improv.wifiSettings(ssid: "", password: "x") == nil)
        #expect(Improv.wifiSettings(ssid: String(repeating: "a", count: 33), password: "") == nil)
    }
    @Test func voiceAndWifiConfigBytes() {
        #expect(OTA.voice(true) == Data([0x0E, 1]))
        #expect(OTA.voice(false) == Data([0x0E, 0]))
        let d = [UInt8](OTA.wifiConfig(ssid: "home", password: "pw", url: "")!)
        #expect(d == [0x0D, 4] + Array("home".utf8) + [2] + Array("pw".utf8) + [0])
    }
}
