import AVFoundation
import Foundation
import PetProtocol
import Security

/// Paxa Labs TTS (paxa-tts-flash-v2): Thai, English and Mandarin, even mixed in one sentence,
/// so "neo รอคุณอยู่" needs no translation. POST https://api.paxalabs.com/v1/tts as WAV, then
/// 16 kHz mono for the pet. The API key lives in the macOS Keychain (set it in the Voice card).
/// Docs: https://paxalabs.com/docs/api/text-to-speech
public enum PaxaVoice {
    public static let voices = ["khanomkrok", "nomyen", "tako", "foithong", "massaman", "thongek", "panang", "oliang"]
    private static let service = "co.laris.oraclepet.paxa"

    // MARK: key (Keychain)

    public static var hasKey: Bool { key != nil }

    static var key: String? {
        let q: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service,
                                kSecReturnData as String: true, kSecMatchLimit as String: kSecMatchLimitOne]
        var out: AnyObject?
        guard SecItemCopyMatching(q as CFDictionary, &out) == errSecSuccess, let d = out as? Data,
              let k = String(data: d, encoding: .utf8), !k.isEmpty else { return nil }
        return k
    }

    /// Save (or with "" remove) the API key.
    public static func setKey(_ k: String) {
        let base: [String: Any] = [kSecClass as String: kSecClassGenericPassword, kSecAttrService as String: service]
        SecItemDelete(base as CFDictionary)
        let t = k.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        var add = base
        add[kSecValueData as String] = Data(t.utf8)
        SecItemAdd(add as CFDictionary, nil)
    }

    // MARK: speech

    /// 16 kHz mono samples for `text`, or nil (no key, network, credits...) with the reason.
    public static func speak(_ text: String, voice: String, completion: @escaping ([Int16]?, String?) -> Void) {
        guard let key else { return completion(nil, "no Paxa API key") }
        var req = URLRequest(url: URL(string: "https://api.paxalabs.com/v1/tts")!, timeoutInterval: 20)
        req.httpMethod = "POST"
        req.setValue("Bearer \(key)", forHTTPHeaderField: "Authorization")
        req.setValue("application/json", forHTTPHeaderField: "Content-Type")
        req.httpBody = try? JSONSerialization.data(withJSONObject: [
            "text": text, "voice": voice, "model": "paxa-tts-flash-v2", "format": "wav"])
        URLSession.shared.dataTask(with: req) { data, resp, err in
            let status = (resp as? HTTPURLResponse)?.statusCode ?? 0
            guard err == nil, status == 200, let data else {
                let why = err?.localizedDescription ?? "Paxa \(status): \(String(decoding: (data ?? Data()).prefix(160), as: UTF8.self))"
                return DispatchQueue.main.async { completion(nil, why) }
            }
            let pcm = decode16k(data)
            DispatchQueue.main.async { completion(pcm, pcm == nil ? "Paxa audio did not decode" : nil) }
        }.resume()
    }

    /// Any audio file AVAudioFile reads (Paxa's WAV, Edge's MP3) -> 16 kHz mono Int16.
    static func decode16k(_ data: Data, ext: String = "wav") -> [Int16]? {
        let tmp = FileManager.default.temporaryDirectory.appendingPathComponent("voice-\(UUID().uuidString).\(ext)")
        defer { try? FileManager.default.removeItem(at: tmp) }
        guard (try? data.write(to: tmp)) != nil, let file = try? AVAudioFile(forReading: tmp),
              let inBuf = AVAudioPCMBuffer(pcmFormat: file.processingFormat, frameCapacity: AVAudioFrameCount(file.length)),
              (try? file.read(into: inBuf)) != nil,
              let out = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(PetSound.sampleRate), channels: 1, interleaved: true),
              let conv = AVAudioConverter(from: file.processingFormat, to: out),
              let outBuf = AVAudioPCMBuffer(pcmFormat: out, frameCapacity: AVAudioFrameCount(Double(inBuf.frameLength) * out.sampleRate / file.processingFormat.sampleRate) + 1024)
        else { return nil }
        var fed = false
        var err: NSError?
        conv.convert(to: outBuf, error: &err) { _, status in
            if fed { status.pointee = .endOfStream; return nil }
            fed = true
            status.pointee = .haveData
            return inBuf
        }
        guard err == nil, let p = outBuf.int16ChannelData?[0] else { return nil }
        return Array(UnsafeBufferPointer(start: p, count: Int(outBuf.frameLength)))
    }
}
