import AppKit
import Foundation
import SwiftTerm

// PTYMirror — a live, interactive mirror of one tmux pane, over maw-js's /ws/pty.
//
// Protocol (per maw-js src/core/transport/pty.ts, as used by HomekeeperLive):
//   client → server: JSON  {type:"attach", target:"session:window", cols, rows, replayLines?}
//                    JSON  {type:"resize", cols, rows}
//                    JSON  {type:"detach"}
//                    binary bytes = keystrokes → PTY stdin
//   server → client: binary bytes = PTY stdout (ANSI) ; JSON {type:"attached"|"error"|…}
//
// Render + input are both delegated to SwiftTerm.TerminalView — a full VT/xterm
// emulator. It parses ANSI itself and translates key events (arrows, ctrl combos,
// paste, …) into the right PTY bytes, surfaced via TerminalViewDelegate.send.
// (Replaces the hand-rolled ANSIParser + PTYTextView + TerminalKeys.)

@MainActor
final class PTYMirror: NSObject, NSWindowDelegate, TerminalViewDelegate {
    let target: String
    private let wsURL: URL
    private var task: URLSessionWebSocketTask?
    private let window: NSWindow
    private var terminal: TerminalView!   // IUO: built after super.init() so we can set the delegate to self
    private var onClose: (() -> Void)?

    init(target: String, backend: URL, onClose: @escaping () -> Void) {
        self.target = target
        self.onClose = onClose

        var comps = URLComponents(url: backend, resolvingAgainstBaseURL: false)!
        comps.scheme = (backend.scheme == "https") ? "wss" : "ws"
        comps.path = "/ws/pty"
        self.wsURL = comps.url!

        let frame = NSRect(x: 0, y: 0, width: 780, height: 470)
        window = NSWindow(contentRect: frame,
                          styleMask: [.titled, .closable, .resizable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "tmux · \(target)  (interactive)"
        window.isReleasedWhenClosed = false              // we own the lifetime

        super.init()
        window.delegate = self

        // TerminalView scrolls itself — no NSScrollView wrapper. Build after
        // super.init() so `self` is available as the terminalDelegate.
        let tv = TerminalView(frame: frame)
        tv.terminalDelegate = self
        tv.font = .monospacedSystemFont(ofSize: 12, weight: .regular)
        tv.autoresizingMask = [.width, .height]
        window.contentView = tv
        self.terminal = tv
    }

    func start() {
        focus()
        terminal.feed(text: "· connecting \(wsURL.absoluteString) …\r\n")
        let t = URLSession.shared.webSocketTask(with: URLRequest(url: wsURL, timeoutInterval: 5))
        task = t
        t.resume()
        // send() queues until the handshake completes, so no didOpen delegate needed.
        sendJSON(["type": "attach", "target": target, "cols": 110, "rows": 34, "replayLines": 300])
        receiveLoop()
    }

    func focus() {
        if !window.isVisible { window.center() }
        window.makeKeyAndOrderFront(nil)
        window.makeFirstResponder(terminal)        // capture keystrokes
        NSApp.activate(ignoringOtherApps: true)
    }

    private func sendJSON(_ obj: [String: Any]) {
        guard let data = try? JSONSerialization.data(withJSONObject: obj),
              let s = String(data: data, encoding: .utf8) else { return }
        task?.send(.string(s)) { _ in }
    }

    private func receiveLoop() {
        guard let task else { return }
        Task { [weak self] in
            while true {
                do {
                    let msg = try await task.receive()
                    guard let self else { return }
                    switch msg {
                    case .data(let d):
                        // feed() must hit SwiftTerm on the main thread; receive() may
                        // resume off-main, so hop explicitly.
                        let bytes = [UInt8](d)[...]
                        await MainActor.run { self.terminal.feed(byteArray: bytes) }
                    case .string(let s):
                        await MainActor.run { self.handleControl(s) }
                    @unknown default:
                        break
                    }
                } catch {
                    guard let self else { return }
                    await MainActor.run { self.terminal.feed(text: "\r\n· disconnected\r\n") }
                    return
                }
            }
        }
    }

    private func handleControl(_ s: String) {
        if let d = s.data(using: .utf8),
           let o = try? JSONSerialization.jsonObject(with: d) as? [String: Any],
           let type = o["type"] as? String {
            if type == "error" { terminal.feed(text: "· error: \(o["message"] as? String ?? "?")\r\n") }
            else { terminal.feed(text: "· [\(type)]\r\n") }
        }
    }

    private func stop() {
        sendJSON(["type": "detach"])
        task?.cancel(with: .goingAway, reason: nil)
        task = nil
        onClose?()
        onClose = nil
    }

    // MARK: - NSWindowDelegate
    func windowWillClose(_ notification: Notification) { stop() }

    // MARK: - TerminalViewDelegate
    // SwiftTerm drives both halves of the protocol: it parses incoming ANSI and
    // emits the right PTY bytes for keystrokes/resizes through these callbacks.

    /// Keystrokes (already translated to PTY bytes) → PTY stdin (binary frame).
    func send(source: TerminalView, data: ArraySlice<UInt8>) {
        task?.send(.data(Data(data))) { _ in }
    }

    /// View geometry changed → tell the PTY its new size.
    func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {
        sendJSON(["type": "resize", "cols": newCols, "rows": newRows])
    }

    func scrolled(source: TerminalView, position: Double) {}

    func setTerminalTitle(source: TerminalView, title: String) {
        window.title = "tmux · \(target) — \(title)"
    }

    func hostCurrentDirectoryUpdate(source: TerminalView, directory: String?) {}

    func clipboardCopy(source: TerminalView, content: Data) {
        if let s = String(data: content, encoding: .utf8) {
            NSPasteboard.general.clearContents()
            NSPasteboard.general.setString(s, forType: .string)
        }
    }

    func rangeChanged(source: TerminalView, startY: Int, endY: Int) {}

    func requestOpenLink(source: TerminalView, link: String, params: [String: String]) {
        if let url = URL(string: link) { NSWorkspace.shared.open(url) }
    }

    func bell(source: TerminalView) { NSSound.beep() }

    func iTermContent(source: TerminalView, content: ArraySlice<UInt8>) {}
}
