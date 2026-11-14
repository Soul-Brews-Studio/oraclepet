import Foundation

/// A live, read-only herdr terminal stream, the way oracle-app-kit's hub reads one (OracleKit
/// HerdrStream), cut down to what the peek needs:
///
///     herdr terminal session observe <pane> --cols C --rows R
///
/// herdr writes one JSON object per line: {"type":"terminal.frame","bytes":"<base64 ANSI>",...}
/// and {"type":"terminal.closed","reason":...}. `observe` never resizes or takes the pane: the
/// agent keeps working in herdr exactly as before. Observe at the pane's own size, or the frames
/// are cropped from the top left and the agent's input box falls off the bottom.
public final class HerdrStream {
    public let pane: String
    public let cols: Int, rows: Int
    private let process = Process()
    private var buffer = Data()
    private let onFrame: (Data) -> Void
    private let onClosed: (String) -> Void

    public init(pane: String, cols: Int, rows: Int,
                onFrame: @escaping (Data) -> Void, onClosed: @escaping (String) -> Void) {
        self.pane = pane
        self.cols = cols
        self.rows = rows
        self.onFrame = onFrame
        self.onClosed = onClosed
    }

    /// The pane's own grid, from `herdr pane layout --pane <id>`.
    public static func paneGrid(_ pane: String) -> (cols: Int, rows: Int)? {
        guard let o = Herdr.json(["pane", "layout", "--pane", pane]),
              let r = o["result"] as? [String: Any], let l = r["layout"] as? [String: Any],
              let panes = l["panes"] as? [[String: Any]],
              let me = panes.first(where: { $0["pane_id"] as? String == pane }),
              let rect = me["rect"] as? [String: Any],
              let w = rect["width"] as? Int, let h = rect["height"] as? Int else { return nil }
        return (w, h)
    }

    public func start() {
        guard let path = Herdr.path else { return onClosed("herdr not found") }
        process.executableURL = URL(fileURLWithPath: path)
        process.arguments = ["terminal", "session", "observe", pane, "--cols", String(cols), "--rows", String(rows)]
        let out = Pipe()
        process.standardOutput = out
        process.standardError = FileHandle.nullDevice
        out.fileHandleForReading.readabilityHandler = { [weak self] h in
            let d = h.availableData
            if d.isEmpty { h.readabilityHandler = nil; return }
            self?.ingest(d)
        }
        process.terminationHandler = { [weak self] p in
            DispatchQueue.main.async { self?.onClosed("stream ended (\(p.terminationStatus))") }
        }
        do { try process.run() } catch { onClosed("could not start herdr: \(error.localizedDescription)") }
    }

    public func stop() {
        if process.isRunning { process.terminate() }
    }

    deinit { stop() }

    private func ingest(_ d: Data) {
        buffer.append(d)
        while let nl = buffer.firstIndex(of: 0x0A) {
            let line = buffer[buffer.startIndex..<nl]
            buffer.removeSubrange(buffer.startIndex...nl)
            guard let o = try? JSONSerialization.jsonObject(with: line) as? [String: Any] else { continue }
            switch o["type"] as? String {
            case "terminal.frame":
                if let b = o["bytes"] as? String, let bytes = Data(base64Encoded: b) {
                    DispatchQueue.main.async { self.onFrame(bytes) }
                }
            case "terminal.closed":
                let reason = o["reason"] as? String ?? "closed"
                DispatchQueue.main.async { self.onClosed(reason) }
            default:
                break
            }
        }
    }
}
