import Foundation
import Speech

/// Speech to text for what the pet's mic heard: Apple speech recognition on the WAV that
/// LISTEN saved. On-device first; if that stalls or fails, Apple's server recognizer.
/// Same strategy as nat-build-with-oracle/voice-call-local native/stt.swift.
/// Needs NSSpeechRecognitionUsageDescription in the app (asked once).
public enum PetHearing {
    /// `locale`: "th-TH", "en-US", ... Calls back on the main queue with the text ("" = nothing heard)
    /// and whether it came from the on-device recognizer.
    public static func transcribe(_ wav: URL, locale: String,
                                  completion: @escaping (_ text: String, _ onDevice: Bool, _ error: String?) -> Void) {
        SFSpeechRecognizer.requestAuthorization { status in
            DispatchQueue.main.async {
                guard status == .authorized else {
                    return completion("", false, "speech recognition not allowed (System Settings > Privacy > Speech Recognition)")
                }
                guard let r = SFSpeechRecognizer(locale: Locale(identifier: locale)), r.isAvailable else {
                    return completion("", false, "no recognizer for \(locale)")
                }
                attempt(r, wav, onDevice: r.supportsOnDeviceRecognition, completion: completion)
            }
        }
    }

    private static func attempt(_ r: SFSpeechRecognizer, _ wav: URL, onDevice: Bool,
                                completion: @escaping (String, Bool, String?) -> Void) {
        let req = SFSpeechURLRecognitionRequest(url: wav)
        req.shouldReportPartialResults = false
        req.addsPunctuation = true
        req.requiresOnDeviceRecognition = onDevice
        var done = false
        var task: SFSpeechRecognitionTask?
        let finish: (String, String?) -> Void = { text, err in
            guard !done else { return }
            done = true
            task?.cancel()
            completion(text, onDevice, err)
        }
        let fallBack: () -> Void = {
            guard !done else { return }
            done = true
            task?.cancel()
            attempt(r, wav, onDevice: false, completion: completion)
        }
        task = r.recognitionTask(with: req) { result, error in
            DispatchQueue.main.async {
                if let result, result.isFinal { return finish(result.bestTranscription.formattedString, nil) }
                if let error {
                    if onDevice { return fallBack() }
                    // "no speech detected" is an answer, not a failure
                    finish("", (error as NSError).code == 1110 ? nil : error.localizedDescription)
                }
            }
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + (onDevice ? 6 : 20)) {
            if onDevice { fallBack() } else { finish("", "recognizer timed out") }
        }
    }
}
