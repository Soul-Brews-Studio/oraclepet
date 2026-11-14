// swift-tools-version: 5.9
import PackageDescription

// A tiny native macOS menubar app (AppKit NSStatusItem) that lists the tmux
// sessions served by `tmux-remote` (localhost:3456). No Tauri, no web bundle —
// pure Swift, runs with `swift run`. Idiom learned from steipete/codexbar
// (NSStatusItem + .accessory activation policy + URLSession polling).
let package = Package(
    name: "tmux-tray",
    platforms: [.macOS(.v13)],   // SwiftTerm v1.13.0 itself targets .macOS(.v13) → no bump needed
    dependencies: [
        // SwiftTerm: full VT/xterm emulator + AppKit TerminalView. Owns ANSI parsing
        // and key translation, so PTYTextView / TerminalKeys / ANSIParser are retired.
        .package(url: "https://github.com/migueldeicaza/SwiftTerm.git", from: "1.13.0"),
    ],
    targets: [
        .executableTarget(
            name: "tmux-tray",
            dependencies: [
                .product(name: "SwiftTerm", package: "SwiftTerm"),
            ],
            path: "Sources/tmux-tray"
        )
    ]
)
