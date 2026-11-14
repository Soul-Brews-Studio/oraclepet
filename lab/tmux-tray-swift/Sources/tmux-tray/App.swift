import AppKit
import Foundation

// tmux-tray — a native macOS menubar widget that fronts the `tmux-remote`
// backend (homekeeper-oracle/scripts/tmux-remote/server.ts on :3456).
//
// The whole chain, proven end to end:
//   menubar click → this app → GET http://localhost:3456/api/sessions
//                 → server.ts → ssh white.local → `tmux list-windows -a`
//
// Idiom learned from steipete/codexbar: NSStatusItem + .accessory policy +
// URLSession polling, menu rebuilt lazily via NSMenuDelegate.

// MARK: - Model (mirrors tmux-remote's GET /api/sessions JSON)

struct TmuxWindow: Decodable { let index: Int?; let name: String; let active: Bool }
struct TmuxSession: Decodable { let name: String; let windows: [TmuxWindow] }
// maw-js GET /api/ls wraps the sessions: {ok, node, oracle, sessions:[…]}
private struct LSResponse: Decodable { let sessions: [TmuxSession] }

// Default to 127.0.0.1, NOT "localhost": macOS resolves localhost→IPv6 (::1) first,
// but bun binds IPv4 only → the connection silently fails. Override host/port with
// the TMUX_REMOTE_URL env var (e.g. http://127.0.0.1:3457).
let backend = URL(string: ProcessInfo.processInfo.environment["TMUX_REMOTE_URL"] ?? "http://127.0.0.1:3456")!

// MARK: - Tray controller

@MainActor
final class TrayController: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private var statusItem: NSStatusItem!
    private let menu = NSMenu()
    private var cache: [TmuxSession]?   // nil = backend offline / not fetched yet
    private var mirrors: [String: PTYMirror] = [:]   // target → open live-mirror window

    func applicationDidFinishLaunching(_ note: Notification) {
        NSApp.setActivationPolicy(.accessory)            // menubar-only, no Dock icon (like codexbar)
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        applyGlyph(online: false)
        menu.delegate = self
        statusItem.menu = menu
        Task { await pollLoop() }                        // async loop instead of Timer (no @Sendable capture)
    }

    /// Poll /api/sessions every 2s → refresh cache + menubar glyph. The menu
    /// items are (re)built lazily in `menuNeedsUpdate`, so an open menu is never disturbed.
    private func pollLoop() async {
        while true {
            let sessions = await Self.fetchSessions()
            cache = sessions
            applyGlyph(online: sessions != nil)
            try? await Task.sleep(for: .seconds(2))
        }
    }

    nonisolated private static func fetchSessions() async -> [TmuxSession]? {
        var req = URLRequest(url: backend.appendingPathComponent("api/ls"))   // maw-js session list
        req.timeoutInterval = 2
        req.cachePolicy = .reloadIgnoringLocalAndRemoteCacheData
        guard let (data, resp) = try? await URLSession.shared.data(for: req),
              (resp as? HTTPURLResponse)?.statusCode == 200,
              let ls = try? JSONDecoder().decode(LSResponse.self, from: data)
        else { return nil }
        return ls.sessions
    }

    private func applyGlyph(online: Bool) {
        guard let button = statusItem.button else { return }
        let img = NSImage(systemSymbolName: "terminal", accessibilityDescription: "tmux Remote")
        img?.isTemplate = true
        button.image = img
        button.appearsDisabled = !online                 // dim when the backend is down
        if button.image == nil { button.title = online ? "tmux" : "tmux·" }  // text fallback
    }

    // MARK: NSMenuDelegate — rebuild from cache just before the menu opens.

    func menuNeedsUpdate(_ menu: NSMenu) {
        menu.removeAllItems()
        if let sessions = cache {
            let windows = sessions.reduce(0) { $0 + $1.windows.count }
            addInfo(menu, "🟢  white.local — \(sessions.count) sessions · \(windows) windows")
            menu.addItem(.separator())
            for s in sessions {
                let item = NSMenuItem(title: s.name, action: nil, keyEquivalent: "")
                let sub = NSMenu()
                for w in s.windows.sorted(by: { ($0.index ?? 0) < ($1.index ?? 0) }) {
                    let mi = NSMenuItem(title: "\(w.active ? "●" : "○")  \(w.index ?? 0): \(w.name)",
                                        action: #selector(attachWindow(_:)), keyEquivalent: "")
                    mi.target = self
                    mi.representedObject = "\(s.name):\(w.index ?? 0)"   // target "session:window"
                    sub.addItem(mi)
                }
                item.submenu = sub
                menu.addItem(item)
            }
        } else {
            addInfo(menu, "🔴  tmux-remote offline (:3456)")
            addInfo(menu, "    start: bun …/tmux-remote/server.ts")
        }
        menu.addItem(.separator())
        addAction(menu, "Open tmux Remote…", #selector(openBackend), key: "o")
        addAction(menu, "Quit", #selector(quitApp), key: "q")
    }

    // MARK: helpers + actions

    private func addInfo(_ menu: NSMenu, _ title: String) {
        let i = NSMenuItem(title: title, action: nil, keyEquivalent: "")
        i.isEnabled = false
        menu.addItem(i)
    }

    private func addAction(_ menu: NSMenu, _ title: String, _ sel: Selector, key: String) {
        let i = NSMenuItem(title: title, action: sel, keyEquivalent: key)
        i.target = self
        menu.addItem(i)
    }

    /// Click a window → open (or focus) a live read-only PTY mirror over /ws/pty.
    @objc private func attachWindow(_ sender: NSMenuItem) {
        guard let target = sender.representedObject as? String else { return }
        if let existing = mirrors[target] { existing.focus(); return }
        let mirror = PTYMirror(target: target, backend: backend) { [weak self] in
            self?.mirrors[target] = nil
        }
        mirrors[target] = mirror
        mirror.start()
    }

    @objc private func openBackend() { NSWorkspace.shared.open(backend) }
    @objc private func quitApp() { NSApp.terminate(nil) }
}

// MARK: - Entry point (@main + @MainActor — the Swift 6 concurrency-safe bootstrap)

@main
struct TmuxTrayApp {
    @MainActor static func main() {
        let app = NSApplication.shared
        let controller = TrayController()
        app.delegate = controller
        app.run()
    }
}
