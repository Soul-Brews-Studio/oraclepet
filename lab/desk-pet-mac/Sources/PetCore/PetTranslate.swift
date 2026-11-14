import Foundation
import Translation

/// English -> Thai on the Mac (Apple's on-device Translation, no network) for the Thai voice:
/// "Hello, nexus and neo need you" -> "สวัสดี nexus และ neo ต้องการคุณ". Calls back on the main
/// queue with the text unchanged when Thai is not installed or the Mac is older than macOS 26.
public enum PetTranslate {
    public static func toThai(_ text: String, completion: @escaping (String) -> Void) {
        guard #available(macOS 26, *) else { return completion(text) }
        Task { @MainActor in
            let en = Locale.Language(identifier: "en"), th = Locale.Language(identifier: "th")
            guard await LanguageAvailability().status(from: en, to: th) == .installed else { return completion(text) }
            let session = TranslationSession(installedSource: en, target: th)
            let out = (try? await session.translate(text))?.targetText ?? text
            completion(out.isEmpty ? text : out)
        }
    }
}
