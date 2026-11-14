import Foundation
import Security

/// The WiFi network the pets join for the voice stream (VOICE.md). The SSID is a setting; the
/// password lives in the macOS Keychain (service co.laris.oraclepet.wifi, account = SSID) and
/// only ever goes to a pet, over BLE, inside the Improv RPC.
public enum PetWiFi {
    private static let service = "co.laris.oraclepet.wifi"

    public static var ssid: String? {
        get { UserDefaults.standard.string(forKey: "wifiSSID").flatMap { $0.isEmpty ? nil : $0 } }
        set { UserDefaults.standard.set(newValue, forKey: "wifiSSID") }
    }

    public static func password(for ssid: String) -> String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecAttrAccount as String: ssid, kSecReturnData as String: true,
                                kSecMatchLimit as String: kSecMatchLimitOne]
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data else { return nil }
        return String(data: d, encoding: .utf8)
    }

    public static func save(ssid: String, password: String) {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                   kSecAttrAccount as String: ssid]
        SecItemDelete(base as CFDictionary)
        var add = base
        add[kSecValueData as String] = Data(password.utf8)
        SecItemAdd(add as CFDictionary, nil)
        Self.ssid = ssid
    }

    /// Send the saved network to a pet. False when nothing is saved or the pet cannot take it.
    @discardableResult
    public static func send(to link: PetLink) -> Bool {
        guard let s = ssid, let p = password(for: s) else { return false }
        return link.sendWiFi(ssid: s, password: p)
    }
}
