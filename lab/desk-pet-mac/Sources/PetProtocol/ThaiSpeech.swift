import Foundation

/// Text for the Thai voice. The Thai voice skips or mangles words in Latin letters, so the
/// names the pets say most (oracles, animation packs, a few app words) are written in Thai
/// script first: "neo รอคุณอยู่" -> "นีโอ รอคุณอยู่". Unknown English words are left alone.
public enum ThaiSpeech {
    /// lower-case English word -> how a Thai speaker says it
    public static let words: [String: String] = [
        // oracles of the fleet
        "nexus": "เน็กซัส", "neo": "นีโอ", "pulse": "พัลส์", "hermes": "เฮอร์มีส", "odin": "โอดิน",
        "atlas": "แอตลาส", "iris": "ไอริส", "mother": "มาเธอร์", "calliope": "คาลลิโอพี", "volt": "โวลต์",
        "digger": "ดิกเกอร์", "athena": "อาธีน่า", "maeon": "แม่ออน", "homelab": "โฮมแล็บ",
        "homekeeper": "โฮมคีปเปอร์", "timekeeper": "ไทม์คีปเปอร์", "thor": "ธอร์", "loki": "โลกิ",
        "freya": "เฟรยา", "huginn": "ฮูกิน", "munin": "มูนิน", "sleipnir": "สเลปเนียร์", "noah": "โนอาห์",
        "arra": "อาร์ร่า", "transcriber": "ทรานสคริปเบอร์",
        // packs and the pet itself
        "siamese": "ไซมีส", "cat": "แมว", "bufo": "บูโฟ", "clawd": "คลอด", "claude": "คลอด",
        "oracle": "ออราเคิล", "oraclepet": "ออราเคิลเพ็ท", "pet": "เพ็ท",
        // words apps and announcements use
        "hello": "สวัสดี", "hi": "หวัดดี", "ok": "โอเค", "okay": "โอเค", "yes": "ใช่", "no": "ไม่",
        "test": "ทดสอบ", "speaker": "ลำโพง", "mic": "ไมค์", "wifi": "ไวไฟ", "bluetooth": "บลูทูธ",
        "update": "อัปเดต", "firmware": "เฟิร์มแวร์", "app": "แอป", "agent": "เอเจนต์", "agents": "เอเจนต์",
    ]

    /// Every known English word (whole word, any case; "neo-oracle" -> "นีโอ") in Thai script.
    public static func render(_ text: String) -> String {
        var out = ""
        var word = ""
        func flush() {
            guard !word.isEmpty else { return }
            let key = word.lowercased()
            if let th = words[key] {
                out += th
            } else if key.hasSuffix("-oracle"), let th = words[String(key.dropLast(7))] {
                out += th
            } else {
                out += word
            }
            word = ""
        }
        for ch in text {
            if ch.isASCII && (ch.isLetter || (ch == "-" && !word.isEmpty)) { word.append(ch) } else { flush(); out.append(ch) }
        }
        flush()
        return out
    }
}
