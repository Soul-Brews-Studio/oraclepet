// swift-tools-version: 6.1
import PackageDescription

// Layout follows laris-co/homelab ψ/lab/inkbird-mac (Cat Lab Grilling): a pure protocol target
// that tests can reach, a core target with the Bluetooth link and no AppKit, and a thin app.
let package = Package(
    name: "DeskPetMac",
    platforms: [.macOS(.v14)],
    products: [
        .executable(name: "DeskPetMac", targets: ["DeskPetMac"]),
    ],
    dependencies: [
        // A real terminal emulator for the peek: herdr's observe stream is raw ANSI frames.
        // (The kit's hub uses Ghostty for this; SwiftTerm is the light option for a menu-bar pet.)
        .package(url: "https://github.com/migueldeicaza/SwiftTerm.git", from: "1.20.0"),
    ],
    targets: [
        // Heartbeat JSON, fleet tally and pet state: no Core Bluetooth, no Process, so it is testable.
        .target(
            name: "PetProtocol",
            path: "Sources/PetProtocol"
        ),
        // NUS link to the pet + the herdr fleet poller. No AppKit.
        .target(
            name: "PetCore",
            dependencies: ["PetProtocol"],
            path: "Sources/PetCore",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        .executableTarget(
            name: "DeskPetMac",
            dependencies: ["PetProtocol", "PetCore", .product(name: "SwiftTerm", package: "SwiftTerm")],
            path: "Sources/DeskPetMac",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // Golden test vectors for the cross-platform library (lab/oraclepet-lib), answered by
        // PetProtocol itself: lab/oraclepet-lib/tools/gen_vectors.sh runs it.
        .executableTarget(
            name: "OraclePetVectors",
            dependencies: ["PetProtocol"],
            path: "Sources/OraclePetVectors",
            swiftSettings: [.swiftLanguageMode(.v5)]
        ),
        // lab/oraclepet-lib core/tests/edge_vectors.json (C core corner cases), answered by PetProtocol
        .executableTarget(name: "OraclePetEdgeVectors", dependencies: ["PetProtocol"], path: "Sources/OraclePetEdgeVectors",
                          swiftSettings: [.swiftLanguageMode(.v5)]),
        // lab/oraclepet-lib core/tests/hb_cases.h (heartbeat clipping cases), answered by PetProtocol
        .executableTarget(name: "OraclePetHeartbeatCases", dependencies: ["PetProtocol"], path: "Sources/OraclePetHeartbeatCases",
                          swiftSettings: [.swiftLanguageMode(.v5)]),
        // lab/oraclepet-lib python/tests/parity_vectors.json (Python parity), answered by PetProtocol
        .executableTarget(name: "OraclePetParity", dependencies: ["PetProtocol"], path: "Sources/OraclePetParity",
                          swiftSettings: [.swiftLanguageMode(.v5)]),
        .testTarget(
            name: "PetProtocolTests",
            dependencies: ["PetProtocol"],
            path: "Tests/PetProtocolTests"
        ),
    ]
)
