import AppKit
import PetCore
import PetProtocol
import SwiftTerm
import SwiftUI

/// The terminal of an agent that is waiting on you, live, in a dark HUD panel next to the cat.
/// Opened by clicking the "Needs you" bubble. A real terminal: herdr's observe stream (raw ANSI
/// frames) drawn by SwiftTerm, read-only, so the agent's own pane is never resized or taken.
struct PeekView: View {
    @ObservedObject var feed: FleetFeed
    @ObservedObject var peek: PanePeek
    @State private var status = ""

    private var waiting: [Agent] { feed.result?.peekable ?? [] }   // waiting on you, else working

    var body: some View {
        VStack(spacing: 0) {
            if waiting.count > 1 {
                Picker("", selection: Binding(
                    get: { peek.pane ?? "" },
                    set: { p in if let a = waiting.first(where: { $0.pane == p }) { peek.show(pane: a.pane, label: a.label) } })) {
                    ForEach(waiting, id: \.pane) { Text($0.label).tag($0.pane) }
                }
                .pickerStyle(.segmented).labelsHidden()
                .padding(8)
            }
            PeekTerminal(pane: peek.pane, status: $status)
                .background(Color(red: 0.04, green: 0.04, blue: 0.06))
            HStack(spacing: 10) {
                Circle().fill(.orange).frame(width: 7, height: 7)
                Text("\(peek.label) · \(peek.pane ?? "")").font(.system(size: 11, design: .monospaced))
                    .foregroundStyle(.secondary).lineLimit(1)
                if !status.isEmpty {
                    Text(status).font(.system(size: 10)).foregroundStyle(.tertiary).lineLimit(1)
                }
                Spacer()
                Button("Copy pane id") {
                    NSPasteboard.general.clearContents()
                    NSPasteboard.general.setString(peek.pane ?? "", forType: .string)
                }
                Button("Focus in herdr") { peek.focusInHerdr() }
                    .keyboardShortcut(.defaultAction)
            }
            .controlSize(.small)
            .padding(8)
        }
        .frame(minWidth: 420, minHeight: 260)
    }
}

/// SwiftTerm view fed by `herdr terminal session observe`. Restarts the stream when the pane
/// changes; fits the font so the pane's whole grid shows (observe pads/crops to the grid we ask
/// for, so we always ask for the pane's own size).
struct PeekTerminal: NSViewRepresentable {
    var pane: String?
    @Binding var status: String

    func makeNSView(context: Context) -> FittedTerminal { FittedTerminal() }

    func updateNSView(_ v: FittedTerminal, context: Context) {
        v.onStatus = { s in DispatchQueue.main.async { status = s } }
        v.attach(pane)
    }

    static func dismantleNSView(_ v: FittedTerminal, coordinator: ()) { v.attach(nil) }
}

final class FittedTerminal: NSView, TerminalViewDelegate {
    private let term = TerminalView(frame: .zero)
    private var stream: HerdrStream?
    private var pane: String?
    private var grid = (cols: 80, rows: 24)
    private var sized: (cols: Int, rows: Int)?
    var onStatus: ((String) -> Void)?

    override init(frame: NSRect) {
        super.init(frame: frame)
        term.terminalDelegate = self
        term.nativeBackgroundColor = NSColor(red: 0.04, green: 0.04, blue: 0.06, alpha: 1)
        term.nativeForegroundColor = NSColor(white: 0.86, alpha: 1)
        term.autoresizingMask = []
        addSubview(term)
    }
    required init?(coder: NSCoder) { fatalError() }

    func attach(_ newPane: String?) {
        guard newPane != pane else { return }
        stream?.stop()
        stream = nil
        pane = newPane
        guard let p = newPane else { return }
        onStatus?("connecting…")
        DispatchQueue.global(qos: .userInitiated).async { [weak self] in
            let g = HerdrStream.paneGrid(p) ?? (cols: 120, rows: 40)
            DispatchQueue.main.async {
                guard let self, self.pane == p else { return }
                self.grid = g
                self.term.feed(text: "\u{1b}c")              // RIS: a clean screen for the new pane
                self.sized = nil
                self.needsLayout = true
                let s = HerdrStream(pane: p, cols: g.cols, rows: g.rows,
                                    onFrame: { [weak self] bytes in self?.term.feed(byteArray: ArraySlice([UInt8](bytes))) },
                                    onClosed: { [weak self] why in self?.onStatus?(why) })
                self.stream = s
                s.start()
                self.onStatus?("live · \(g.cols)×\(g.rows) · click to type")
                self.window?.makeFirstResponder(self.term)
            }
        }
    }

    /// Largest font that holds the pane's whole grid in this view; the terminal view is sized to
    /// exactly that grid and centred, so herdr's cursor addressing lands where it should.
    override func layout() {
        super.layout()
        let W = bounds.width - 12, H = bounds.height - 12
        guard W > 50, H > 50 else { return }
        var size: CGFloat = 16
        var font = NSFont.monospacedSystemFont(ofSize: size, weight: .regular)
        func cell(_ f: NSFont) -> CGSize {
            let w = ("W" as NSString).size(withAttributes: [.font: f]).width
            let h = ceil(f.ascender - f.descender + f.leading)
            return CGSize(width: w, height: h)
        }
        while size > 5 {
            font = NSFont.monospacedSystemFont(ofSize: size, weight: .regular)
            let c = cell(font)
            if CGFloat(grid.cols) * c.width <= W && CGFloat(grid.rows) * c.height <= H { break }
            size -= 0.5
        }
        if term.font.pointSize != font.pointSize { term.font = font }
        let c = cell(font)
        let tw = ceil(CGFloat(grid.cols) * c.width) + 2, th = ceil(CGFloat(grid.rows) * c.height) + 2
        term.frame = NSRect(x: (bounds.width - tw) / 2, y: (bounds.height - th) / 2, width: tw, height: th)
        if sized.map({ $0 != grid }) ?? true {             // grid the pane has; herdr addresses cells in it
            term.resize(cols: grid.cols, rows: grid.rows)
            sized = grid
        }
    }

    deinit { stream?.stop() }

    // Keys: the stream stays observe (the agent's pane is never resized or taken); what you type
    // here is replayed into the pane with `herdr pane send-keys` / `send-text`, in order.
    private let keys = DispatchQueue(label: "co.laris.deskpet.keys")

    func send(source: TerminalView, data: ArraySlice<UInt8>) {
        guard let pane else { return }
        let calls = HerdrKeys.calls(for: Array(data))
        keys.async { for args in calls { _ = Herdr.run(["pane"] + args.prefix(1) + [pane] + args.dropFirst()) } }
    }

    func sizeChanged(source: TerminalView, newCols: Int, newRows: Int) {}
    func setTerminalTitle(source: TerminalView, title: String) {}
    func hostCurrentDirectoryUpdate(source: TerminalView, directory: String?) {}
    func scrolled(source: TerminalView, position: Double) {}
    func clipboardCopy(source: TerminalView, content: Data) {
        NSPasteboard.general.clearContents()
        NSPasteboard.general.setString(String(decoding: content, as: UTF8.self), forType: .string)
    }
    func rangeChanged(source: TerminalView, startY: Int, endY: Int) {}
}

/// Floating HUD panel for the peek: titled (so it can be moved and resized), dark, on every Space.
final class PeekPanel: NSPanel {
    init(root: some View) {
        super.init(contentRect: NSRect(x: 0, y: 0, width: 720, height: 420),
                   styleMask: [.titled, .closable, .resizable, .utilityWindow, .hudWindow, .nonactivatingPanel],
                   backing: .buffered, defer: false)
        title = "Agent terminal"
        level = .floating
        collectionBehavior = [.canJoinAllSpaces, .fullScreenAuxiliary]
        isReleasedWhenClosed = false
        hidesOnDeactivate = false
        contentView = FirstMouseHostingView(rootView: root)
        setFrameAutosaveName("DeskPetPeek")
    }
}
