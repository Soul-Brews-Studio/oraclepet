import AVFoundation
import CryptoKit
import Foundation
import PetProtocol

/// Speech for the pet's speaker: macOS speech synthesis (Thai "Kanya" for Thai text, an
/// English voice otherwise) rendered to 16 kHz mono, trimmed, normalised and packed as a
/// PSND IMA ADPCM clip (PetProtocol/PetSound.swift) that the pet plays from LittleFS.
/// One phrase at a time; the synthesizer is kept alive while it renders.
public final class PetVoice {
    public static let shared = PetVoice()

    private let synth = AVSpeechSynthesizer()
    private let work = DispatchQueue(label: "co.laris.deskpet.voice")
    private var busy = false
    private var waiting: [(String, String?, (Data?) -> Void)] = []

    /// `language`: "th-TH" / "en-US" / nil = the "voiceLanguage" setting (Thai unless set to "en"),
    /// and Thai whenever the text has Thai in it.
    public func clip(for text: String, language: String? = nil, completion: @escaping (Data?) -> Void) {
        let file = Self.cacheFile(text, language)
        if let d = try? Data(contentsOf: file), !d.isEmpty {         // said before: no rendering, no credits
            petLog.notice("voice cache hit: \(file.lastPathComponent, privacy: .public) (\(d.count) bytes)")
            return completion(d)
        }
        render(text, language) { d in
            if let d {
                try? FileManager.default.createDirectory(at: file.deletingLastPathComponent(), withIntermediateDirectories: true)
                try? d.write(to: file)
            }
            completion(d)
        }
    }

    /// ~/Library/Caches/OraclePet/voice/<sha256 of engine, voice, language, text, shaping>.ima
    static func cacheFile(_ text: String, _ language: String?) -> URL {
        let d = UserDefaults.standard
        let engine: String
        switch d.string(forKey: "voiceEngine") {
        case "paxa" where PaxaVoice.hasKey: engine = "paxa:" + (d.string(forKey: "paxaVoice") ?? "khanomkrok")
        case "edge" where EdgeVoice.available: engine = "edge:" + (d.string(forKey: "edgeVoice") ?? "th-TH-NiwatNeural")
        default: engine = "macos"
        }
        let id = [engine, language ?? d.string(forKey: "voiceLanguage") ?? "th", "shape2", text].joined(separator: "|")
        let h = SHA256.hash(data: Data(id.utf8)).map { String(format: "%02x", $0) }.joined()
        return FileManager.default.urls(for: .cachesDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("OraclePet/voice/\(h.prefix(32)).ima")
    }

    private func render(_ text: String, _ language: String?, completion: @escaping (Data?) -> Void) {
        // Edge (voice engine "edge"): free neural voices, Thai male Niwat by default
        if UserDefaults.standard.string(forKey: "voiceEngine") == "edge", EdgeVoice.available {
            let voice = UserDefaults.standard.string(forKey: "edgeVoice") ?? "th-TH-NiwatNeural"
            EdgeVoice.speak(text, voice: voice) { pcm, why in
                if let pcm, !pcm.isEmpty {
                    let t = PetSound.tidy(PetSound.smallSpeaker(pcm), maxSeconds: 25)
                    return completion(t.isEmpty ? nil : PetSound.clip(PetSound.loud(t)))
                }
                PetVoice.lastPaxaError = "Edge: \(why ?? "?")"
                self.macClip(for: text, language: language, completion: completion)   // fall back to macOS
            }
            return
        }
        // Paxa (voice engine setting "paxa" + a key): reads Thai, English and both mixed as written
        if UserDefaults.standard.string(forKey: "voiceEngine") == "paxa", PaxaVoice.hasKey {
            let voice = UserDefaults.standard.string(forKey: "paxaVoice") ?? "khanomkrok"
            PaxaVoice.speak(text, voice: voice) { pcm, why in
                if let pcm, !pcm.isEmpty {
                    let t = PetSound.tidy(PetSound.smallSpeaker(pcm), maxSeconds: 25)
                    return completion(t.isEmpty ? nil : PetSound.clip(PetSound.loud(t)))
                }
                PetVoice.lastPaxaError = why
                self.macClip(for: text, language: language, completion: completion)   // fall back to macOS
            }
            return
        }
        macClip(for: text, language: language, completion: completion)
    }

    /// Why the last Paxa request fell back to the macOS voice (nil = it did not).
    public static var lastPaxaError: String?

    private func macClip(for text: String, language: String?, completion: @escaping (Data?) -> Void) {
        let english = UserDefaults.standard.string(forKey: "voiceLanguage") == "en"
        let thai = language.map { $0.hasPrefix("th") } ?? (PetSound.isThai(text) || !english)
        let queue: (String) -> Void = { t in
            DispatchQueue.main.async {
                self.waiting.append((t, thai ? "th-TH" : (language ?? "en-US"), completion))
                self.next()
            }
        }
        // Thai voice, English words: translate on the Mac first ("need you" -> "ต้องการคุณ")
        let latin = text.unicodeScalars.contains { ("a"..."z").contains($0) || ("A"..."Z").contains($0) }
        if thai && latin && !PetSound.isThai(text) { PetTranslate.toThai(text, completion: queue) } else { queue(text) }
    }

    private func next() {
        guard !busy, !waiting.isEmpty else { return }
        busy = true
        let (text, language, completion) = waiting.removeFirst()
        let english = UserDefaults.standard.string(forKey: "voiceLanguage") == "en"
        let lang = language ?? (PetSound.isThai(text) || !english ? "th-TH" : "en-US")
        // the Thai voice drops words in Latin letters: write the known ones in Thai first
        let u = AVSpeechUtterance(string: lang.hasPrefix("th") ? ThaiSpeech.render(text) : text)
        u.voice = AVSpeechSynthesisVoice(language: lang) ?? AVSpeechSynthesisVoice(language: "en-US")
        u.rate = AVSpeechUtteranceDefaultSpeechRate
        var samples: [Int16] = []
        var converter: AVAudioConverter?
        let out = AVAudioFormat(commonFormat: .pcmFormatInt16, sampleRate: Double(PetSound.sampleRate),
                                channels: 1, interleaved: true)!
        var finished = false
        let finish: () -> Void = { [weak self] in
            guard !finished else { return }
            finished = true
            let pcm = PetSound.loud(PetSound.tidy(PetSound.smallSpeaker(samples), maxSeconds: 25))   // no bass, a little louder
            let data = pcm.isEmpty ? nil : PetSound.clip(pcm)
            DispatchQueue.main.async {
                completion(data)
                self?.busy = false
                self?.next()
            }
        }
        synth.write(u) { buffer in
            guard let pcm = buffer as? AVAudioPCMBuffer, pcm.frameLength > 0 else { return finish() }   // empty = done
            if converter == nil { converter = AVAudioConverter(from: pcm.format, to: out) }
            guard let conv = converter else { return }
            let ratio = out.sampleRate / pcm.format.sampleRate
            let cap = AVAudioFrameCount(Double(pcm.frameLength) * ratio + 64)
            guard let dst = AVAudioPCMBuffer(pcmFormat: out, frameCapacity: cap) else { return }
            var fed = false
            var err: NSError?
            conv.convert(to: dst, error: &err) { _, status in
                if fed { status.pointee = .noDataNow; return nil }
                fed = true
                status.pointee = .haveData
                return pcm
            }
            if let p = dst.int16ChannelData?[0] {
                samples.append(contentsOf: UnsafeBufferPointer(start: p, count: Int(dst.frameLength)))
            }
        }
        // a voice that never calls back with the empty buffer must not wedge the queue
        work.asyncAfter(deadline: .now() + 15) { finish() }
    }
}
