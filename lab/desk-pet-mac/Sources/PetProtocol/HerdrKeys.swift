import Foundation

/// What you type into the peek's terminal, as herdr pane calls on the agent's pane:
/// ["send-keys", "up"], ["send-text", "abc"], … (the caller adds "pane" and the pane id).
/// Arrow keys arrive as ESC [ A–D (or ESC O A–D in application-cursor mode).
public enum HerdrKeys {
    public static func calls(for b: [UInt8]) -> [[String]] {
        var out: [[String]] = []
        var text = ""
        func flush() { if !text.isEmpty { out.append(["send-text", text]); text = "" } }
        func key(_ k: String) { flush(); out.append(["send-keys", k]) }
        var i = 0
        while i < b.count {
            let c = b[i]
            if c == 0x1B, i + 2 < b.count, b[i + 1] == 0x5B || b[i + 1] == 0x4F {   // ESC [ x / ESC O x
                switch b[i + 2] {
                case 0x41: key("up"); case 0x42: key("down")
                case 0x43: key("right"); case 0x44: key("left")
                default: break
                }
                i += 3; continue
            }
            switch c {
            case 0x1B: key("esc")
            case 0x0D, 0x0A: key("enter")
            case 0x09: key("tab")
            case 0x7F, 0x08: key("backspace")
            default:
                if c >= 0x20 { text.unicodeScalars.append(Unicode.Scalar(c)) }
                else { flush(); out.append(["send-text", String(Unicode.Scalar(c))]) }   // ctrl-x etc.
            }
            i += 1
        }
        flush()
        return out
    }
}
