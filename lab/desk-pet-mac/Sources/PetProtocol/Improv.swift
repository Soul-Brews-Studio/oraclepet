import Foundation

/// Improv Wi-Fi over BLE (https://www.improv-wifi.com/ble/): the standard way to give a device a
/// WiFi network. The pet (firmware improv.c) has the Improv service next to its own.
public enum Improv {
    public static let service = "00467768-6228-2272-4663-277478268000"
    public static let state   = "00467768-6228-2272-4663-277478268001"
    public static let error   = "00467768-6228-2272-4663-277478268002"
    public static let rpc     = "00467768-6228-2272-4663-277478268003"
    public static let result  = "00467768-6228-2272-4663-277478268004"

    /// RPC 0x01 "send Wi-Fi settings": cmd, len, ssid len, ssid, password len, password, checksum
    /// (the low byte of the sum of everything before it). nil if a field is too long.
    public static func wifiSettings(ssid: String, password: String) -> Data? {
        let s = [UInt8](ssid.utf8), p = [UInt8](password.utf8)
        guard !s.isEmpty, s.count <= 32, p.count <= 64 else { return nil }
        let payload = [UInt8(s.count)] + s + [UInt8(p.count)] + p
        var d = [0x01, UInt8(payload.count)] + payload
        d.append(UInt8(truncatingIfNeeded: d.reduce(0) { $0 + Int($1) }))
        return Data(d)
    }

    /// RPC 0x02 identify (the pet chirps).
    public static let identify = Data([0x02, 0x00, 0x02])

    /// What the state characteristic says.
    public static func describe(state: UInt8) -> String {
        switch state {
        case 0x01: return "needs authorization"
        case 0x02: return "ready for WiFi"
        case 0x03: return "connecting to WiFi…"
        case 0x04: return "on WiFi"
        default:   return "state \(state)"
        }
    }
}
