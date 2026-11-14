import Foundation
import PetProtocol

/// Microsoft Edge's online neural voices (free, no key), through the edge-tts command line run by
/// uvx, as nat-build-with-oracle/voice-call-local does. Thai male "th-TH-NiwatNeural" by default.
/// Edge sometimes returns no audio for an English run inside Thai text ("neo oracle"); then the
/// text is tried again with the names written in Thai (ThaiSpeech).
public enum EdgeVoice {
    public static let voices = ["th-TH-NiwatNeural", "th-TH-PremwadeeNeural", "en-US-GuyNeural", "en-US-AriaNeural"]

    static var uvx: String? {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        return ["/opt/homebrew/bin/uvx", "\(home)/.local/bin/uvx", "/usr/local/bin/uvx"]
            .first { FileManager.default.isExecutableFile(atPath: $0) }
    }

    public static var available: Bool { uvx != nil }

    /// 16 kHz mono samples, or nil with the reason. Runs off the main queue; calls back on it.
    public static func speak(_ text: String, voice: String, completion: @escaping ([Int16]?, String?) -> Void) {
        guard let uvx else { return completion(nil, "uvx not found (brew install uv)") }
        DispatchQueue.global(qos: .userInitiated).async {
            var why = "no audio from Edge"
            for attempt in [text, ThaiSpeech.render(text)] where !attempt.isEmpty {
                let mp3 = FileManager.default.temporaryDirectory.appendingPathComponent("edge-\(UUID().uuidString).mp3")
                defer { try? FileManager.default.removeItem(at: mp3) }
                let p = Process()
                p.executableURL = URL(fileURLWithPath: uvx)
                p.arguments = ["-q", "edge-tts", "--voice", voice, "--text", attempt, "--write-media", mp3.path]
                p.standardOutput = FileHandle.nullDevice
                let err = Pipe()
                p.standardError = err
                do { try p.run() } catch { why = "edge-tts did not start: \(error.localizedDescription)"; break }
                p.waitUntilExit()
                if let d = try? Data(contentsOf: mp3), d.count > 1000, let pcm = PaxaVoice.decode16k(d, ext: "mp3") {
                    return DispatchQueue.main.async { completion(pcm, nil) }
                }
                let msg = String(decoding: err.fileHandleForReading.readDataToEndOfFile().suffix(200), as: UTF8.self)
                if !msg.isEmpty { why = msg.split(separator: "\n").last.map(String.init) ?? why }
            }
            DispatchQueue.main.async { completion(nil, why) }
        }
    }
}
