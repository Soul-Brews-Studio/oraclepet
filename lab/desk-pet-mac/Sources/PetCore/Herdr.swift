import Foundation

/// The herdr CLI, as a GUI app has to call it: by absolute path (no shell PATH), off the main
/// thread, JSON in stdout.
public enum Herdr {
    public static var path: String? { FleetFeed.findHerdr() }

    /// Run herdr with `args`; stdout when it exits 0.
    public static func run(_ args: [String]) -> Data? {
        guard let p = path else { return nil }
        return FleetFeed.run(p, args)
    }

    public static func json(_ args: [String]) -> [String: Any]? {
        run(args).flatMap { try? JSONSerialization.jsonObject(with: $0) as? [String: Any] }
    }
}

/// "Start new chat": a new herdr workspace in `folder`, an agent of `kind` started in its pane,
/// then the prompt sent to that pane (addressed by pane id, never by name).
public final class SessionLauncher: ObservableObject {
    public static let kinds = ["claude", "codex"]

    @Published public var folder: String { didSet { UserDefaults.standard.set(folder, forKey: "newChatFolder") } }
    @Published public var kind: String { didSet { UserDefaults.standard.set(kind, forKey: "newChatKind") } }
    @Published public private(set) var busy = false
    @Published public private(set) var lastResult: String?
    @Published public private(set) var lastPane: String?

    public init() {
        let home = FileManager.default.homeDirectoryForCurrentUser.path
        let sandbox = home + "/sandbox"
        folder = UserDefaults.standard.string(forKey: "newChatFolder")
            ?? (FileManager.default.fileExists(atPath: sandbox) ? sandbox : home)
        kind = UserDefaults.standard.string(forKey: "newChatKind") ?? "claude"
    }

    /// "fix the login bug please" -> "fix-the-login" (herdr label and agent name).
    public static func slug(_ prompt: String) -> String {
        let words = prompt.lowercased()
            .components(separatedBy: CharacterSet.alphanumerics.inverted)
            .filter { !$0.isEmpty }.prefix(3)
        return words.isEmpty ? "deskpet-chat" : words.joined(separator: "-")
    }

    public func start(_ prompt: String) {
        let text = prompt.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !text.isEmpty, !busy else { return }
        busy = true
        lastResult = "Starting \(kind)…"
        let folder = folder, kind = kind, name = Self.slug(text)
        DispatchQueue.global(qos: .userInitiated).async {
            let result = Self.launch(text, folder: folder, kind: kind, name: name)
            DispatchQueue.main.async {
                self.busy = false
                self.lastResult = result.message
                self.lastPane = result.pane
            }
        }
    }

    private static func launch(_ text: String, folder: String, kind: String, name: String) -> (message: String, pane: String?) {
        guard Herdr.path != nil else { return ("herdr not found", nil) }
        guard let created = Herdr.json(["workspace", "create", "--cwd", folder, "--label", name, "--focus"]),
              let r = created["result"] as? [String: Any],
              let root = r["root_pane"] as? [String: Any],
              let pane = root["pane_id"] as? String else {
            return ("Could not create a herdr workspace in \(folder)", nil)
        }
        guard Herdr.run(["agent", "start", name, "--kind", kind, "--pane", pane, "--timeout", "60000"]) != nil else {
            return ("\(kind) did not start in \(pane)", pane)
        }
        guard Herdr.run(["agent", "prompt", pane, text]) != nil else {
            return ("\(kind) started in \(pane), but the prompt was not accepted", pane)
        }
        return ("Started \(kind) in \(pane) · \(name)", pane)
    }
}

/// Which waiting agent the peek shows. The terminal itself streams in the app (HerdrStream +
/// SwiftTerm); this only holds the selection and the herdr actions around it.
public final class PanePeek: ObservableObject {
    @Published public private(set) var pane: String?
    @Published public private(set) var label = ""
    private let work = DispatchQueue(label: "co.laris.deskpet.peek")

    public init() {}

    public func show(pane: String, label: String) {
        self.pane = pane
        self.label = label
    }

    public func stop() {}

    /// Bring the pane's herdr space to the front.
    public func focusInHerdr() {
        guard let pane, let ws = pane.split(separator: ":").first.map(String.init) else { return }
        work.async { _ = Herdr.run(["workspace", "focus", ws]) }
    }
}
