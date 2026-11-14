import AppKit
import PetCore
import PetProtocol
import SwiftUI

/// One window: the pet as the device shows it, why it is in that state, and the link.
struct PetView: View {
    @ObservedObject var link: PetLink
    @ObservedObject var feed: FleetFeed
    @ObservedObject var display: DisplayModel
    var onPeek: (Agent) -> Void = { _ in }
    var onFirmware: () -> Void = {}
    var extra: PetLink? = nil                            // the second pet, when there is one

    static func symbol(for s: PetState) -> String {
        switch s {
        case .idle: return "pawprint.fill"
        case .busy: return "bolt.fill"
        case .attention: return "exclamationmark.circle.fill"
        case .sleep: return "moon.zzz.fill"
        }
    }

    static func tint(for s: PetState) -> Color {
        switch s {
        case .idle: return .green
        case .busy: return .blue
        case .attention: return .orange
        case .sleep: return .indigo
        }
    }

    private var petPack: String { link.firmware.map { $0.pack.isEmpty ? "siamese" : $0.pack } ?? "siamese" }

    var body: some View {
        VStack(spacing: 14) {
            screen
            stateLine
            fleetCard
            modeCard
            linkCard
        }
        .padding(16)
        .frame(width: 360)
    }

    // The pet's own screen: black, the state's GIF at 3x nearest-neighbour like the firmware.
    private var screen: some View {
        ZStack(alignment: .bottomLeading) {
            RoundedRectangle(cornerRadius: 18).fill(Color.black)
            PetGIF(state: feed.state, reacting: link.reacting, dragsWindow: false,
                   onDoubleClick: { display.mode = .mascot }, pack: link.firmware?.pack)
                .help("Double-click for mascot mode")
                .frame(width: 288, height: 300)
                .frame(maxWidth: .infinity)
                .padding(.top, 8)
            VStack(alignment: .leading, spacing: 2) {
                Text(link.petName?.replacingOccurrences(of: "Claude-", with: "\(petPack) · ") ?? petPack)
                    .font(.system(size: 15, weight: .bold, design: .monospaced))
                Text(feed.lastHeartbeat?.msg ?? "")
                    .font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary)
                    .lineLimit(1)
            }
            .foregroundStyle(.white)
            .padding(12)
        }
        .frame(height: 360)
        .overlay(alignment: .topTrailing) {
            Circle().fill(link.isLinked ? Color.green : Color.gray)
                .frame(width: 10, height: 10).padding(12)
                .help(link.state.label)
        }
    }

    private var stateLine: some View {
        HStack(spacing: 8) {
            Image(systemName: Self.symbol(for: feed.state))
                .foregroundStyle(Self.tint(for: feed.state))
            Text(feed.state.label).font(.title3.weight(.semibold))
            Spacer()
            if let sent = feed.lastSent {
                Text("sent \(sent, style: .relative) ago").font(.caption).foregroundStyle(.secondary)
            }
            Button {
                display.mode = .mascot
            } label: {
                Label("Mascot", systemImage: "cat")
            }
            .controlSize(.small)
            .help("Just the cat on the desktop, like the Codex buddy")
        }
    }

    private func agentRow(_ a: Agent, _ tint: Color, _ symbol: String) -> some View {
        Button { onPeek(a) } label: {
            HStack(spacing: 6) {
                Image(systemName: symbol).foregroundStyle(tint).font(.caption)
                Text(a.project).font(.callout.weight(.medium)).foregroundStyle(.primary)
                Text(a.title.isEmpty ? a.label : a.title).font(.caption).foregroundStyle(.secondary)
                Spacer(minLength: 4)
                Text(a.pane).font(.caption2.monospaced()).foregroundStyle(.tertiary)
            }
            .lineLimit(1).truncationMode(.tail)
            .contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("See \(a.project)'s terminal (\(a.pane))")
    }

    private var fleetCard: some View {
        GroupBox {
            if let err = feed.herdrError {
                Label(err, systemImage: "exclamationmark.triangle").foregroundStyle(.orange).font(.callout)
            } else if let r = feed.result {
                VStack(alignment: .leading, spacing: 6) {
                    HStack(spacing: 18) {
                        count("working", r.running, .blue)
                        count("waiting", r.waiting, .orange)
                        count("awake", r.total, .green)
                        count("agents", r.agents, .secondary)
                    }
                    // who: the project each agent works in and what its terminal says it is doing.
                    // Click a row to see that terminal.
                    ForEach(r.waitingAgents, id: \.pane) { agentRow($0, .orange, "exclamationmark.circle.fill") }
                    ForEach(r.workingAgents, id: \.pane) { agentRow($0, .blue, "bolt.fill") }
                }
                .frame(maxWidth: .infinity, alignment: .leading)
            } else {
                Text("Reading the herdr fleet…").foregroundStyle(.secondary)
            }
        } label: {
            Label("herdr fleet", systemImage: "square.stack.3d.up")
        }
    }

    private func count(_ title: String, _ n: Int, _ color: Color) -> some View {
        VStack(alignment: .leading, spacing: 0) {
            Text("\(n)").font(.title2.monospacedDigit().weight(.semibold)).foregroundStyle(color)
            Text(title).font(.caption).foregroundStyle(.secondary)
        }
    }

    private var modeCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Picker("", selection: $feed.mode) {
                    Text("Follow fleet").tag(FleetFeed.Mode.fleet)
                    Text("Manual").tag(FleetFeed.Mode.manual)
                }
                .pickerStyle(.segmented)
                .labelsHidden()
                if feed.mode == .manual {
                    HStack {
                        ForEach(PetState.allCases, id: \.self) { s in
                            Button {
                                feed.manualState = s
                            } label: {
                                Label(s.rawValue, systemImage: Self.symbol(for: s))
                            }
                            .buttonStyle(.bordered)
                            .tint(feed.manualState == s ? Self.tint(for: s) : nil)
                        }
                    }
                    .controlSize(.small)
                }
            }
        } label: {
            Label("What the pet shows", systemImage: "cat")
        }
    }

    private var linkCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 6) {
                HStack {
                    Text(link.state.label)
                    if let rssi = link.rssi, link.isLinked {
                        Text("\(rssi) dBm").foregroundStyle(.secondary).font(.caption)
                    }
                    Spacer()
                    Button(link.isLinked || link.state == .scanning ? "Disconnect" : "Connect") {
                        if link.isLinked || link.state == .scanning { link.disconnect() } else { link.connect() }
                    }
                    .controlSize(.small)
                }
                if let err = link.lastError {
                    Text(err).font(.caption).foregroundStyle(.red)
                }
                Button(action: onFirmware) {
                    HStack(spacing: 6) {
                        Image(systemName: "cpu")
                        Text(link.firmware.map { "Firmware \($0.version) · \($0.slot)" } ?? "Firmware")
                        Spacer()
                        Image(systemName: "chevron.right").font(.caption2)
                    }
                    .font(.caption).foregroundStyle(.secondary).contentShape(Rectangle())
                }
                .buttonStyle(.plain)
                .help("Open the Pet Firmware page: install the built-in firmware or a .bin")
                if link.ota != .idle {
                    VStack(alignment: .leading, spacing: 3) {
                        Text(link.ota.label).font(.caption).lineLimit(1)
                            .foregroundStyle({ if case .failed = link.ota { return Color.red } else { return Color.secondary } }())
                        if case .sending(let f) = link.ota { ProgressView(value: f) }
                    }
                }
                if let extra { SecondPetRow(link: extra, onFirmware: onFirmware) }
                if link.state == .unauthorized {
                    Text("System Settings → Privacy & Security → Bluetooth → allow OraclePet.")
                        .font(.caption).foregroundStyle(.secondary)
                }
                Text("Stop pet-usage / pet-fleet in pm2 first: the pet takes one connection.")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
        } label: {
            Label("Bluetooth", systemImage: "antenna.radiowaves.left.and.right")
        }
    }
}

/// The pack's GIF for a state, animated, scaled with nearest-neighbour filtering.
/// GIFs ship in the app bundle under Resources/Pet (device set) or Resources/PetAlpha (mascot).
/// With `onDoubleClick` set (mascot mode) the cat itself is the drag handle, like the Codex
/// buddy: mouse-down drags the window, a double-click calls back.
struct PetGIF: NSViewRepresentable {
    var state: PetState
    var folder = "Pet"                                  // "PetAlpha": transparent set for mascot mode
    var reacting = false                                // the pet was touched: show heart.gif
    var dragsWindow = true                              // mascot: the cat is the drag handle
    var onDoubleClick: (() -> Void)? = nil
    /// The pack the pet shows (from its OTA status); nil or "siamese" uses the bundled set.
    var pack: String? = nil

    func makeNSView(context: Context) -> DragImageView {
        let v = DragImageView()
        v.animates = true
        v.imageScaling = .scaleProportionallyUpOrDown
        v.wantsLayer = true
        v.layer?.magnificationFilter = .nearest
        v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)
        v.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        return v
    }

    func updateNSView(_ v: DragImageView, context: Context) {
        v.onDoubleClick = onDoubleClick
        v.dragsWindow = dragsWindow
        let name = reacting ? "heart" : state.rawValue
        let key = "\(pack ?? "")/\(folder)/\(name)"
        guard v.identifier?.rawValue != key else { return }   // keep the animation running between ticks
        v.identifier = NSUserInterfaceItemIdentifier(key)
        // Another pack on the pet: its GIF from Desk Pet's packs, keyed transparent in mascot mode.
        if let pack, pack != "siamese", let url = PackFiles.url(pack: pack, state: name) {
            v.image = folder == "PetAlpha" ? KeyedGIF.image(for: url) : NSImage(contentsOf: url)
            if v.image != nil { return }
        }
        v.image = Bundle.main.url(forResource: name, withExtension: "gif", subdirectory: folder)
            .flatMap(NSImage.init(contentsOf:))
    }
}

/// NSImageView swallows mouse-down, so a borderless panel cannot be dragged by it, and a SwiftUI
/// tap gesture on top does the same. This one moves the window itself (mouseDragged by the mouse
/// delta) and turns a double-click into `onDoubleClick`. Not window.performDrag: its tracking
/// loop swallowed the second click, so double-click never fired.
final class DragImageView: NSImageView {
    var onDoubleClick: (() -> Void)?
    var dragsWindow = true
    private var drag = WindowDrag()

    override func mouseDown(with event: NSEvent) {
        guard onDoubleClick != nil else { return super.mouseDown(with: event) }
        if event.clickCount == 2 { onDoubleClick?(); return }
        if dragsWindow { drag.begin(window) }
    }
    override func mouseDragged(with event: NSEvent) {
        guard onDoubleClick != nil, dragsWindow else { return super.mouseDragged(with: event) }
        drag.move(window)
    }
    override var mouseDownCanMoveWindow: Bool { false }       // we move it ourselves (mascot only)
}

/// Moves a window by how far the mouse travelled since `begin`, in screen coordinates.
struct WindowDrag {
    private var startMouse = NSPoint.zero
    private var startOrigin = NSPoint.zero

    mutating func begin(_ w: NSWindow?) {
        startMouse = NSEvent.mouseLocation
        startOrigin = w?.frame.origin ?? .zero
    }
    func move(_ w: NSWindow?) {
        let m = NSEvent.mouseLocation
        w?.setFrameOrigin(NSPoint(x: startOrigin.x + m.x - startMouse.x, y: startOrigin.y + m.y - startMouse.y))
    }
}

/// A transparent strip that drags its window: put it behind views (like the bubble) that should
/// be grab-able in a borderless panel.
struct WindowDragHandle: NSViewRepresentable {
    var onClick: (() -> Void)? = nil

    final class Handle: NSView {
        var onClick: (() -> Void)?
        private var drag = WindowDrag()
        private var moved = false
        override func acceptsFirstMouse(for event: NSEvent?) -> Bool { true }
        override func mouseDown(with event: NSEvent) { drag.begin(window); moved = false }
        override func mouseDragged(with event: NSEvent) { drag.move(window); moved = true }
        override func mouseUp(with event: NSEvent) { if !moved { onClick?() } }   // a click, not a drag
    }
    func makeNSView(context: Context) -> Handle { Handle() }
    func updateNSView(_ v: Handle, context: Context) { v.onClick = onClick }
}

/// Where a pack's GIF for a state lives: Desk Pet's built-in packs (Resources/Packs) or a dropped
/// one (Application Support/Desk Pet/Packs), using the pack's manifest.json for the file name
/// (bufo and the older cats name their files differently).
enum PackFiles {
    private static var manifests: [String: [String: Any]] = [:]

    static func dir(_ pack: String) -> URL? {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Desk Pet/Packs/\(pack)")
        if FileManager.default.fileExists(atPath: support.path) { return support }
        return Bundle.main.resourceURL?.appendingPathComponent("Packs/\(pack)")
    }

    static func url(pack: String, state: String) -> URL? {
        guard let d = dir(pack) else { return nil }
        if manifests[pack] == nil,
           let data = try? Data(contentsOf: d.appendingPathComponent("manifest.json")),
           let o = try? JSONSerialization.jsonObject(with: data) as? [String: Any] {
            manifests[pack] = o["states"] as? [String: Any] ?? [:]
        }
        let states = manifests[pack] ?? [:]
        let pick: (Any?) -> String? = { v in (v as? String) ?? (v as? [String])?.first }
        // a state the pack lacks falls back to idle, as the firmware does
        for s in [state, state == "heart" ? "celebrate" : state, "idle"] {
            if let f = pick(states[s]) {
                let u = d.appendingPathComponent(f)
                if FileManager.default.fileExists(atPath: u.path) { return u }
            }
        }
        let plain = d.appendingPathComponent("\(state).gif")
        return FileManager.default.fileExists(atPath: plain.path) ? plain : nil
    }
}

/// One line for the second pet in the Bluetooth card: name, board, link, what it shows.
struct SecondPetRow: View {
    @ObservedObject var link: PetLink
    var onFirmware: () -> Void

    var body: some View {
        Button(action: onFirmware) {
            HStack(spacing: 6) {
                Circle().fill(link.isLinked ? Color.green : Color.gray).frame(width: 7, height: 7)
                Text(link.petName ?? "Second pet").font(.caption.weight(.medium))
                if let fw = link.firmware {
                    Text("\(OTA.boardName(fw.project)) · \(fw.pack.isEmpty ? "–" : fw.pack)\(fw.app.isEmpty ? "" : " · app " + fw.app)")
                        .font(.caption).foregroundStyle(.secondary)
                } else {
                    Text(link.state.label).font(.caption).foregroundStyle(.secondary)
                }
                Spacer()
                Image(systemName: "chevron.right").font(.caption2).foregroundStyle(.secondary)
            }
            .lineLimit(1).contentShape(Rectangle())
        }
        .buttonStyle(.plain)
        .help("The second pet gets the same fleet heartbeat. Its firmware, packs and apps: the Pet Firmware page, second tab.")
    }
}
