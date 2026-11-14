import AppKit
import PetCore
import PetProtocol
import SwiftUI

/// Window or mascot, remembered across launches.
final class DisplayModel: ObservableObject {
    enum Mode: String { case window, mascot }

    @Published var mode: Mode {
        didSet {
            UserDefaults.standard.set(mode.rawValue, forKey: "displayMode")
            onChange?(mode)
        }
    }
    var onChange: ((Mode) -> Void)?

    init() {
        mode = Mode(rawValue: UserDefaults.standard.string(forKey: "displayMode") ?? "") ?? .window
    }
}

/// Mascot mode, after the Codex buddy: just the cat on the desktop (transparent GIFs from
/// Resources/PetAlpha), a toast bubble when something needs saying, and a small pill toolbar.
struct MascotView: View {
    @ObservedObject var link: PetLink
    @ObservedObject var feed: FleetFeed
    @ObservedObject var display: DisplayModel
    @ObservedObject var launcher: SessionLauncher
    var onPeek: (Agent?) -> Void                              // open the terminal peek (nil: first waiting)
    @State private var hovering = false
    @State private var composing = false
    @State private var draft = ""
    @FocusState private var draftFocused: Bool

    static let size = NSSize(width: 320, height: 400)

    var body: some View {
        VStack(spacing: 6) {
            bubble
                .frame(height: 52, alignment: .bottom)           // reserved, so the cat never jumps
            PetGIF(state: feed.state, folder: "PetAlpha", reacting: link.reacting, onDoubleClick: { display.mode = .window },
                   pack: link.firmware?.pack)
                .frame(width: 192, height: 200)
                .help("Drag to move · double-click for the full window")
            pill
                .opacity(hovering || composing ? 1 : 0.82)
            if composing { compose.transition(.opacity.combined(with: .move(edge: .top))) }
            if let r = launcher.lastResult, composing || launcher.busy {
                Text(r).font(.system(size: 10)).foregroundStyle(.secondary).lineLimit(1)
            }
        }
        .padding(.horizontal, 8)
        .frame(width: Self.size.width, height: Self.size.height, alignment: .bottom)
        .onHover { hovering = $0 }
        .animation(.easeOut(duration: 0.2), value: feed.state)
        .animation(.easeOut(duration: 0.18), value: composing)
    }

    // MARK: Start new chat — like the Codex buddy's field

    private var compose: some View {
        HStack(spacing: 8) {
            Menu {
                Section("Agent") {
                    Picker("Agent", selection: $launcher.kind) {
                        ForEach(SessionLauncher.kinds, id: \.self) { Text($0).tag($0) }
                    }
                    .pickerStyle(.inline)
                }
                Section("Folder: " + (launcher.folder as NSString).abbreviatingWithTildeInPath) {
                    Button("Choose folder…") { chooseFolder() }
                }
            } label: {
                Image(systemName: "plus").font(.system(size: 12, weight: .semibold))
                    .frame(width: 22, height: 22)
                    .background(Circle().fill(.white.opacity(0.12)))
            }
            .menuStyle(.borderlessButton).menuIndicator(.hidden).fixedSize()
            .help("\(launcher.kind) in \((launcher.folder as NSString).abbreviatingWithTildeInPath)")

            TextField("Start new chat", text: $draft)
                .textFieldStyle(.plain)
                .font(.system(size: 13))
                .focused($draftFocused)
                .onSubmit(send)
                .onExitCommand { composing = false }

            Button(action: send) {
                Image(systemName: launcher.busy ? "hourglass" : "arrow.up")
                    .font(.system(size: 11, weight: .bold))
                    .frame(width: 24, height: 24)
                    .background(Circle().fill(draft.isEmpty ? Color.white.opacity(0.15) : Color.accentColor))
            }
            .buttonStyle(.plain)
            .disabled(draft.trimmingCharacters(in: .whitespaces).isEmpty || launcher.busy)
        }
        .foregroundStyle(.white)
        .padding(.leading, 8).padding(.trailing, 6).padding(.vertical, 6)
        .frame(width: 290)
        .background(Capsule().fill(Color(white: 0.10).opacity(0.96)))
        .overlay(Capsule().stroke(.white.opacity(0.10)))
        .shadow(color: .black.opacity(0.3), radius: 8, y: 3)
    }

    private func send() {
        guard !draft.trimmingCharacters(in: .whitespaces).isEmpty else { return }
        launcher.start(draft)
        draft = ""
    }

    private func chooseFolder() {
        NSApp.activate(ignoringOtherApps: true)
        let p = NSOpenPanel()
        p.canChooseDirectories = true
        p.canChooseFiles = false
        p.directoryURL = URL(fileURLWithPath: launcher.folder)
        p.prompt = "Use folder"
        if p.runModal() == .OK, let u = p.url { launcher.folder = u.path }
    }

    // MARK: Toast bubble — only when there is something to say

    private struct Toast { var symbol: String; var tint: Color; var title: String; var detail: String }

    private var toast: Toast? {
        if link.ota != .idle {                                    // an update speaks first
            if case .failed = link.ota {
                return Toast(symbol: "exclamationmark.triangle.fill", tint: .red, title: "Update failed", detail: link.ota.label)
            }
            return Toast(symbol: "arrow.down.circle.fill", tint: .green,
                         title: link.ota.isBusy ? "Updating pet" : "Pet updated", detail: link.ota.label)
        }
        if !link.isLinked {
            return Toast(symbol: "antenna.radiowaves.left.and.right.slash", tint: .gray,
                         title: "Pet not linked", detail: link.state.label)
        }
        if feed.mode == .manual {
            return Toast(symbol: "hand.raised.fill", tint: .purple,
                         title: "Manual", detail: feed.manualState.label)
        }
        guard let r = feed.result else { return nil }
        if r.waiting > 0 {
            return Toast(symbol: "exclamationmark.circle.fill", tint: .orange,
                         title: "Needs you", detail: r.waitingAgents.map(\.project).joined(separator: ", "))
        }
        if r.running > 0 {
            let who = r.workingAgents.map(\.project)
            return Toast(symbol: "bolt.fill", tint: .blue,
                         title: who.count == 1 ? "\(who[0]) is working" : "\(r.running) working",
                         detail: who.count == 1 ? (r.workingAgents[0].title.isEmpty ? "\(r.agents) agents in the fleet" : r.workingAgents[0].title)
                                                : who.joined(separator: ", "))
        }
        return nil                                                // idle / asleep: the cat says it
    }

    @ViewBuilder private var bubble: some View {
        if let t = toast {
            HStack(alignment: .top, spacing: 7) {
                Image(systemName: t.symbol).foregroundStyle(t.tint).font(.system(size: 13, weight: .semibold))
                VStack(alignment: .leading, spacing: 1) {
                    Text(t.title).font(.system(size: 12, weight: .semibold))
                    if !t.detail.isEmpty {
                        Text(t.detail).font(.system(size: 11)).foregroundStyle(.white.opacity(0.7))
                            .lineLimit(2).truncationMode(.tail)
                    }
                }
            }
            .foregroundStyle(.white)
            .padding(.horizontal, 12).padding(.vertical, 8)
            .background(RoundedRectangle(cornerRadius: 14, style: .continuous).fill(Color(white: 0.11).opacity(0.92)))
            // On top, so it gets the mouse (behind the text it never did): drag moves the cat,
            // a click opens the terminal of who is waiting (or working).
            .overlay(WindowDragHandle(onClick: { onPeek(nil) }))
            .help(feed.result?.peekable.isEmpty == false ? "Click to see their terminal · drag to move" : "Drag to move")
            .overlay(RoundedRectangle(cornerRadius: 14, style: .continuous).stroke(.white.opacity(0.08)))
            .shadow(color: .black.opacity(0.25), radius: 6, y: 2)
            .transition(.opacity.combined(with: .move(edge: .bottom)))
        }
    }

    // MARK: Pill toolbar

    private var pill: some View {
        HStack(spacing: 0) {
            pillButton(composing ? "xmark" : "square.and.pencil", help: "Start a new agent session") {
                composing.toggle()
                if composing { DispatchQueue.main.async { draftFocused = true } }
            }
            divider
            pillButton("macwindow", help: "Open the OraclePet window") { display.mode = .window }
            divider
            pillButton(feed.mode == .fleet ? "dot.radiowaves.left.and.right" : "hand.raised",
                       help: feed.mode == .fleet ? "Following the herdr fleet — click for manual"
                                                 : "Manual — click to follow the fleet") {
                feed.mode = feed.mode == .fleet ? .manual : .fleet
            }
            divider
            Menu {
                Button("Open Window") { display.mode = .window }
                Picker("Pet shows", selection: $feed.mode) {
                    Text("Follow fleet").tag(FleetFeed.Mode.fleet)
                    Text("Manual").tag(FleetFeed.Mode.manual)
                }
                if feed.mode == .manual {
                    Picker("Manual state", selection: $feed.manualState) {
                        ForEach(PetState.allCases, id: \.self) { Text($0.label).tag($0) }
                    }
                }
                Divider()
                Button(link.isLinked || link.state == .scanning ? "Disconnect Pet" : "Connect Pet") {
                    if link.isLinked || link.state == .scanning { link.disconnect() } else { link.connect() }
                }
                Divider()
                Button("Quit OraclePet") { NSApp.terminate(nil) }
            } label: {
                Image(systemName: "chevron.down").font(.system(size: 12, weight: .semibold))
                    .frame(width: 34, height: 26)
            }
            .menuStyle(.borderlessButton)
            .menuIndicator(.hidden)
            .fixedSize()
        }
        .foregroundStyle(.white)
        .padding(.horizontal, 6).padding(.vertical, 3)
        .background(Capsule().fill(Color(white: 0.12).opacity(0.94)))
        .overlay(Capsule().stroke(.white.opacity(0.08)))
        .shadow(color: .black.opacity(0.25), radius: 5, y: 2)
    }

    private var divider: some View {
        Rectangle().fill(.white.opacity(0.18)).frame(width: 1, height: 16)
    }

    private func pillButton(_ symbol: String, help: String, action: @escaping () -> Void) -> some View {
        Button(action: action) {
            Image(systemName: symbol).font(.system(size: 13, weight: .medium))
                .frame(width: 34, height: 26)
                .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help(help)
    }
}

/// The borderless floating panel that holds the mascot. Same recipe as the Codex buddy and
/// homelab codex-fleet's MascotOverlay: non-activating, clear, floating, on every Space,
/// dragged by its background. Remembers where it was left.
final class MascotPanel: NSPanel {
    private static let originKey = "mascotOrigin"

    init(root: some View) {
        super.init(contentRect: NSRect(origin: .zero, size: MascotView.size),
                   styleMask: [.nonactivatingPanel, .borderless],
                   backing: .buffered, defer: false)
        isOpaque = false
        backgroundColor = .clear
        hasShadow = false
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary, .stationary]
        isMovableByWindowBackground = true
        hidesOnDeactivate = false
        isReleasedWhenClosed = false
        let host = FirstMouseHostingView(rootView: root)
        host.layer?.backgroundColor = .clear
        contentView = host
        setFrameOrigin(Self.savedOrigin() ?? Self.defaultOrigin())
        NotificationCenter.default.addObserver(forName: NSWindow.didMoveNotification, object: self, queue: .main) { [weak self] _ in
            guard let o = self?.frame.origin else { return }
            UserDefaults.standard.set([o.x, o.y], forKey: Self.originKey)
        }
    }

    // Buttons and the menu in the pill need key status; the panel still never activates the app.
    override var canBecomeKey: Bool { true }

    private static func savedOrigin() -> NSPoint? {
        guard let a = UserDefaults.standard.array(forKey: originKey) as? [Double], a.count == 2 else { return nil }
        let p = NSPoint(x: a[0], y: a[1])
        // Only if it is still on a screen (a display may have been unplugged).
        let probe = NSRect(origin: p, size: MascotView.size).insetBy(dx: 60, dy: 60)
        return NSScreen.screens.contains { $0.visibleFrame.intersects(probe) } ? p : nil
    }

    private static func defaultOrigin() -> NSPoint {
        let v = NSScreen.main?.visibleFrame ?? NSRect(x: 0, y: 0, width: 1440, height: 900)
        return NSPoint(x: v.maxX - MascotView.size.width - 40, y: v.minY + 40)
    }
}

/// The first click on a panel that is not key would only make it key, so tapping the bubble or
/// a pill button needed two clicks. Like the Codex buddy, one click acts.
final class FirstMouseHostingView<Content: View>: NSHostingView<Content> {
    override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
}
