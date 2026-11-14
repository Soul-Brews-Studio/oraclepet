import SwiftUI

struct ContentView: View {
    @EnvironmentObject private var ble: BuddyBLEModel
    @State private var selectedTab = 0

    private let bg = LinearGradient(
        colors: [Color(red: 0.03, green: 0.04, blue: 0.08), Color(red: 0.05, green: 0.08, blue: 0.12), Color(red: 0.02, green: 0.02, blue: 0.03)],
        startPoint: .topLeading,
        endPoint: .bottomTrailing
    )

    var body: some View {
        TabView(selection: $selectedTab) {
            buddyFullscreenPage
                .tabItem { Label("Buddy Full", systemImage: "macwindow") }
                .tag(0)

            NavigationStack { buddyPage }
                .tabItem { Label("Claude Buddy", systemImage: "rectangle.inset.filled") }
                .tag(1)

            NavigationStack { claudeHomePage }
                .tabItem { Label("Claude Home", systemImage: "house.and.flag") }
                .tag(2)

            NavigationStack { receiverConsolePage }
                .tabItem { Label("BLE RX", systemImage: "dot.radiowaves.left.and.right") }
                .tag(3)
        }
        .tint(CardputerPalette.orange)
        .preferredColorScheme(.dark)
        .onAppear {
            selectedTab = 0
        }
        .task {
            selectedTab = 0
            try? await Task.sleep(nanoseconds: 300_000_000)
            selectedTab = 0
        }
    }

    private var buddyFullscreenPage: some View {
        ZStack(alignment: .top) {
            Color.black.ignoresSafeArea()
            GeometryReader { geo in
                VStack(spacing: 0) {
                    CardputerBuddyScreen(ble: ble)
                        .frame(
                            width: geo.size.width,
                            height: min(geo.size.height, (geo.size.width / (240.0 / 135.0)) + 24)
                        )
                    Spacer(minLength: 0)
                }
                .frame(width: geo.size.width, height: geo.size.height, alignment: .top)
            }
        }
        .accessibilityLabel("Claude Buddy full page \(ble.rxProofState) polls \(ble.claudeHomePollCount) bytes \(ble.claudeHomeBytes)")
        .accessibilityIdentifier("BuddyFullPage")
    }

    private var buddyPage: some View {
        ZStack {
            bg.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    fullBuddyHero
                    claudeHomeCard
                    liveReceiveProof
                    receiverCard
                }
                .padding(.horizontal, 22)
                .padding(.vertical, 16)
            }
        }
        .navigationTitle("Claude Buddy")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarColorScheme(.dark, for: .navigationBar)
    }


    private var claudeHomePage: some View {
        ZStack {
            bg.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    claudeHomeCard
                    panel("Claude Home raw payload", subtitle: "This is the exact JSON fetched from Python. The Buddy tab renders from these fields when HOME is live.") {
                        rowsBox(title: "home state", rows: ble.claudeHomeProofRows)
                        rowsBox(title: "decoded home fields", rows: ble.claudeHomeRows)
                        jsonBox(title: "pretty home JSON", text: ble.claudeHomePretty.isEmpty ? ble.claudeHomeRaw : ble.claudeHomePretty)
                    }
                    logCard
                }
                .padding(.horizontal, 22)
                .padding(.vertical, 16)
            }
        }
        .navigationTitle("Claude Home")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarColorScheme(.dark, for: .navigationBar)
    }

    private var receiverConsolePage: some View {
        ZStack {
            bg.ignoresSafeArea()
            ScrollView {
                VStack(alignment: .leading, spacing: 18) {
                    receiverCard
                    scanCard
                    rxCard
                    protocolMapCard
                    logCard
                }
                .padding(.horizontal, 22)
                .padding(.vertical, 16)
            }
        }
        .navigationTitle("BLE RX")
        .navigationBarTitleDisplayMode(.inline)
        .toolbarColorScheme(.dark, for: .navigationBar)
    }

    private var fullBuddyHero: some View {
        VStack(alignment: .leading, spacing: 18) {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .center, spacing: 24) {
                    fullBuddyCopy
                    Spacer(minLength: 18)
                    CardputerBuddyScreen(ble: ble)
                        .frame(width: 760, height: 427.5)
                }
                VStack(alignment: .leading, spacing: 18) {
                    fullBuddyCopy
                    CardputerBuddyScreen(ble: ble)
                        .frame(maxWidth: 900)
                        .aspectRatio(240.0 / 135.0, contentMode: .fit)
                }
            }

            receiverControls
        }
        .foregroundStyle(.white)
        .padding(24)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 28).fill(CardputerPalette.black.opacity(0.86))
                RoundedRectangle(cornerRadius: 28).fill(
                    LinearGradient(colors: [CardputerPalette.dark.opacity(0.96), CardputerPalette.black.opacity(0.40), CardputerPalette.orange.opacity(0.12)], startPoint: .topLeading, endPoint: .bottomTrailing)
                )
            }
        )
        .overlay(RoundedRectangle(cornerRadius: 28).stroke(CardputerPalette.orange.opacity(0.35), lineWidth: 1))
    }

    private var fullBuddyCopy: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Text("Claude Buddy")
                    .font(.system(size: 46, weight: .black, design: .rounded))
                statusPill("full page", color: CardputerPalette.orange)
            }
            Text("Real-use iPad Buddy. It can receive Claude status from Python two ways: Claude Home HTTP feed now, and BLE NUS RX when macOS Bluetooth permissions allow. The big screen renders only received Python data — no mock counters, no iPad draft values.")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.74))
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 8) {
                statusPill(ble.bluetoothState, color: ble.bluetoothState == "powered on" ? .green : CardputerPalette.orange)
                statusPill(ble.claudeHomeActive ? "Home polling" : "Home idle", color: ble.claudeHomeActive ? .green : CardputerPalette.orange)
                statusPill(ble.rxServerActive ? "BLE RX live" : "BLE RX idle", color: ble.rxServerActive ? .green : CardputerPalette.orange)
                statusPill(ble.hasLiveBuddyData ? "REAL DATA" : "no data yet", color: ble.hasLiveBuddyData ? .green : CardputerPalette.yellow)
            }
            Text("Run path now: ble_bridge.py --serve-http → Claude Home tab → Buddy screen. BLE path remains: ble_bridge.py → NUS RX → Buddy screen.")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.white.opacity(0.58))
                .textSelection(.enabled)
        }
        .frame(maxWidth: 560, alignment: .leading)
    }

    private var receiverControls: some View {
        HStack(spacing: 10) {
            Button(ble.rxServerActive ? "RX server live" : "Start RX server", action: ble.startReceiveServer)
                .buttonStyle(.borderedProminent)
                .tint(ble.rxServerActive ? .green : CardputerPalette.cyan)
            Button("Stop RX server", action: ble.stopReceiveServer).buttonStyle(.bordered)
            Button("Clear RX", action: ble.clearRX).buttonStyle(.bordered)
            Spacer()
            Text(ble.receiverReadySummary)
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.white.opacity(0.70))
                .textSelection(.enabled)
        }
    }


    private var claudeHomeControls: some View {
        VStack(alignment: .leading, spacing: 10) {
            TextField("Claude Home URL", text: $ble.claudeHomeURL)
                .textFieldStyle(.roundedBorder)
                .textInputAutocapitalization(.never)
                .autocorrectionDisabled()
                .font(.system(.body, design: .monospaced))
            HStack(spacing: 10) {
                Button(ble.claudeHomeActive ? "Home polling" : "Start Home", action: ble.startClaudeHome)
                    .buttonStyle(.borderedProminent)
                    .tint(ble.claudeHomeActive ? .green : CardputerPalette.orange)
                Button("Poll once", action: ble.pollClaudeHomeNow).buttonStyle(.bordered)
                Button("Stop Home", action: ble.stopClaudeHome).buttonStyle(.bordered)
                Spacer()
                Text(ble.claudeHomeSummary)
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.70))
                    .textSelection(.enabled)
            }
        }
    }

    private var claudeHomeCard: some View {
        panel("Claude Home Python feed", subtitle: "ใช้งานจริงตอนนี้: run Python HTTP feed on the Mac, then iPad polls this URL and renders the full Buddy screen from real claude-statusline data.") {
            claudeHomeControls
            Text("Mac feed command: uv run --with bleak python lab/buddy/bridge/ble_bridge.py --serve-http --host 0.0.0.0 --port 8765")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
            rowsBox(title: "home proof", rows: ble.claudeHomeProofRows)
            rowsBox(title: "decoded home fields", rows: Array(ble.claudeHomeRows.prefix(10)))
            jsonBox(title: "Last Claude Home JSON", text: ble.claudeHomePretty.isEmpty ? ble.claudeHomeRaw : ble.claudeHomePretty)
        }
    }

    private var liveReceiveProof: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Live receive proof")
                        .font(.title2.bold())
                    Text("ข้อมูลจริงมาจาก Python เท่านั้น: Claude Home HTTP feed now, or BLE write/notify when available. Draft iPad values never drive the Buddy screen.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                statusPill(ble.hasLiveBuddyData ? ble.rxProofState : "WAITING", color: ble.hasLiveBuddyData ? .green : .orange)
            }

            Grid(horizontalSpacing: 12, verticalSpacing: 12) {
                GridRow {
                    proofTile(
                        title: "1 · Claude Home feed",
                        badge: ble.hasLiveHome ? "HOME LIVE" : "WAITING",
                        color: ble.hasLiveHome ? .green : .orange,
                        body: "Python serves /heartbeat from /tmp/claude-statusline/raw.json. The Buddy screen can use this now without BLE scan permissions.",
                        rows: ble.claudeHomeProofRows
                    )
                    proofTile(
                        title: "2 · BLE RX writes",
                        badge: ble.rxServerWriteCount > 0 ? "BYTES IN" : "NO WRITE",
                        color: ble.rxServerWriteCount > 0 ? .green : .orange,
                        body: "Counts increment only when a central writes to NUS RX. This remains the BLE proof path.",
                        rows: ble.receiverRows
                    )
                    proofTile(
                        title: "3 · Decoded Buddy data",
                        badge: ble.hasLiveBuddyData ? ble.rxProofState : "EMPTY",
                        color: ble.hasLiveBuddyData ? .green : .purple,
                        body: ble.hasLiveBuddyData ? ble.buddyDisplayMessage : "No real Python data received yet. The full Buddy screen remains idle/listening.",
                        rows: ble.buddyDisplayRows.isEmpty ? [DecodedRow(key: "data", value: "none yet")] : Array(ble.buddyDisplayRows.prefix(7))
                    )
                }
            }
        }
        .padding(20)
        .cardGlass()
    }

    private var receiverCard: some View {
        panel("Receive from Python BLE bridge", subtitle: "BLE fallback path: iPad acts as Claude-iPad NUS peripheral. ble_bridge.py connects and writes when host Bluetooth permissions allow.") {
            receiverControls
            rowsBox(title: "receiver state", rows: ble.receiverRows)
            jsonBox(title: "Last real BLE RX JSON/raw", text: ble.lastRXPretty.isEmpty ? ble.lastRXRaw : ble.lastRXPretty)
            Text("maw command to run on this Mac: maw run <target> 'cd /opt/Code/github.com/Soul-Brews-Studio/esp32-oracle/agents/3-claude-buddy && uv run --with bleak lab/buddy/bridge/ble_bridge.py' — then maw peek <target> to inspect the bridge logs.")
                .font(.system(.caption, design: .monospaced))
                .foregroundStyle(.secondary)
                .textSelection(.enabled)
        }
    }

    private var heroCard: some View {
        VStack(alignment: .leading, spacing: 18) {
            ViewThatFits(in: .horizontal) {
                HStack(alignment: .center, spacing: 24) {
                    heroCopy
                    Spacer(minLength: 18)
                    CardputerBuddyScreen(ble: ble)
                        .frame(width: 520, height: 292.5)
                }
                VStack(alignment: .leading, spacing: 18) {
                    heroCopy
                    CardputerBuddyScreen(ble: ble)
                        .frame(maxWidth: 560)
                        .aspectRatio(240.0 / 135.0, contentMode: .fit)
                }
            }

            HStack(spacing: 10) {
                Button(ble.isScanning ? "Scanning…" : "Scan Claude/NUS", action: ble.scan)
                    .buttonStyle(.borderedProminent)
                    .tint(CardputerPalette.cyan)
                Button("Stop scan", action: ble.stopScan).buttonStyle(.bordered)
                Button("Disconnect", action: ble.disconnect).buttonStyle(.bordered)
                Spacer()
                Text("NUS: \(ble.nusReadySummary)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.70))
            }

            HStack(spacing: 10) {
                statusPill("control", color: CardputerPalette.cyan)
                Text(ble.controlFeedback)
                    .font(.system(.callout, design: .monospaced))
                    .foregroundStyle(.white.opacity(0.78))
                    .textSelection(.enabled)
            }
        }
        .foregroundStyle(.white)
        .padding(24)
        .background(
            ZStack {
                RoundedRectangle(cornerRadius: 28).fill(CardputerPalette.black.opacity(0.82))
                RoundedRectangle(cornerRadius: 28).fill(
                    LinearGradient(colors: [CardputerPalette.dark.opacity(0.90), CardputerPalette.black.opacity(0.35), CardputerPalette.orange.opacity(0.10)], startPoint: .topLeading, endPoint: .bottomTrailing)
                )
            }
        )
        .overlay(RoundedRectangle(cornerRadius: 28).stroke(CardputerPalette.orange.opacity(0.30), lineWidth: 1))
    }

    private var heroCopy: some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack(spacing: 10) {
                Text("Claude Buddy")
                    .font(.system(size: 42, weight: .black, design: .rounded))
                statusPill("Cardputer UI", color: CardputerPalette.orange)
            }
            Text("iPad BLE console redrawn with the same 240×135 chrome as device/apps: dark header, orange hairline, idle/pair/live states, Y/N/Q hint strip.")
                .font(.title3)
                .foregroundStyle(.white.opacity(0.72))
                .fixedSize(horizontal: false, vertical: true)
            HStack(spacing: 8) {
                statusPill(ble.bluetoothState, color: ble.bluetoothState == "powered on" ? .green : CardputerPalette.orange)
                statusPill(ble.state.rawValue, color: ble.state == .connected ? .green : CardputerPalette.orange)
                statusPill(ble.txProofState.contains("LIVE") ? "live source" : "draft-safe", color: ble.txProofState.contains("LIVE") ? .green : CardputerPalette.orange)
            }
        }
        .frame(maxWidth: 510, alignment: .leading)
    }

    private var proofDashboard: some View {
        VStack(alignment: .leading, spacing: 14) {
            HStack {
                VStack(alignment: .leading, spacing: 4) {
                    Text("Live data proof")
                        .font(.title2.bold())
                    Text("แยกให้ชัด: ค่าที่กรอกในแอป = draft/local, เขียนผ่าน CoreBluetooth = live TX, notify จากบอร์ด = live RX.")
                        .foregroundStyle(.secondary)
                }
                Spacer()
                statusPill(ble.txProofState.contains("LIVE") ? "LIVE TX" : "DRAFT", color: ble.txProofState.contains("LIVE") ? .green : .orange)
            }

            Grid(horizontalSpacing: 12, verticalSpacing: 12) {
                GridRow {
                    proofTile(
                        title: "1 · Draft/local",
                        badge: "NOT SENT YET",
                        color: .orange,
                        body: "These are iPad-generated test values until a writable NUS RX characteristic is ready and Send is tapped.",
                        rows: ble.draftRows.isEmpty ? [DecodedRow(key: "state", value: "No draft generated yet")] : Array(ble.draftRows.prefix(5))
                    )
                    proofTile(
                        title: "2 · Live BLE TX",
                        badge: ble.txProofState.contains("LIVE") ? "REAL WRITE" : "NO WRITE",
                        color: ble.txProofState.contains("LIVE") ? .green : .orange,
                        body: ble.txAckState,
                        rows: Array(ble.proofRows.prefix(9))
                    )
                    proofTile(
                        title: "3 · Device RX",
                        badge: ble.lastRXRaw.isEmpty ? "NO REPLY" : "REAL NOTIFY",
                        color: ble.lastRXRaw.isEmpty ? .purple : .green,
                        body: ble.lastRXRaw.isEmpty ? "Only fills when the buddy sends a TX notify. Empty means no device data received yet." : ble.lastRXKind,
                        rows: ble.lastRXRows.isEmpty ? [DecodedRow(key: "rx", value: "none yet")] : Array(ble.lastRXRows.prefix(6))
                    )
                }
            }

            HStack {
                Button("Send draft heartbeat", action: ble.sendHeartbeat).buttonStyle(.borderedProminent).tint(.green)
                Button("Ask status", action: ble.sendStatusRequest).buttonStyle(.borderedProminent).tint(.purple)
                Button("Clear RX", action: ble.clearRX).buttonStyle(.bordered)
                Spacer()
                Text("Last device: \(ble.connectedName)")
                    .font(.system(.caption, design: .monospaced))
                    .foregroundStyle(.secondary)
            }
        }
        .padding(20)
        .cardGlass()
    }

    private var scanCard: some View {
        panel("Found buddies", subtitle: "Connect only when a Claude/NUS advertisement is physically present.") {
            if ble.discovered.isEmpty {
                emptyState("No Claude/NUS devices yet", "This screen is honest: no live peripheral found, so any heartbeat values are draft/local only.")
            } else {
                ForEach(ble.discovered) { buddy in
                    HStack {
                        VStack(alignment: .leading, spacing: 3) {
                            Text(buddy.name).font(.headline)
                            Text(buddy.id.uuidString).font(.caption).foregroundStyle(.secondary)
                            Text("NUS advertised: \(buddy.advertisedNUS ? "yes" : "no")")
                                .font(.caption2)
                                .foregroundStyle(.secondary)
                        }
                        Spacer()
                        statusPill("RSSI \(buddy.rssi)", color: .cyan)
                        Button("Connect") { ble.connect(buddy) }.buttonStyle(.borderedProminent).tint(.cyan)
                    }
                    Divider().overlay(.white.opacity(0.1))
                }
            }
        }
    }

    private var heartbeatCard: some View {
        panel("Heartbeat builder", subtitle: "App-generated test payload matching buddy_protocol.py. It becomes real TX only after the proof card shows CoreBluetooth write evidence.") {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 230), spacing: 12)], spacing: 12) {
                metricStepper("total", value: $ble.total, range: 0...999)
                metricStepper("running", value: $ble.running, range: 0...999)
                metricStepper("waiting", value: $ble.waiting, range: 0...999)
                metricStepper("entries", value: $ble.entries, range: 0...10_000)
                metricStepper("tokens", value: $ble.tokens, range: 0...5_000_000, step: 100)
                metricStepper("tokens_today", value: $ble.tokensToday, range: 0...5_000_000, step: 1000)
                metricStepper("pct5h", value: $ble.pct5h, range: 0...100)
                metricStepper("reset5h", value: $ble.reset5h, range: 0...604_800, step: 300)
                metricStepper("pct7d", value: $ble.pct7d, range: 0...100)
                metricStepper("reset7d", value: $ble.reset7d, range: 0...604_800, step: 3600)
                metricStepper("brightness", value: $ble.brightness, range: 0...255)
            }
            TextField("msg", text: $ble.message)
                .textFieldStyle(.roundedBorder)
            Toggle("Include permission prompt", isOn: $ble.promptEnabled)
            if ble.promptEnabled {
                TextField("prompt.id", text: $ble.promptID).textFieldStyle(.roundedBorder)
                TextField("prompt.tool", text: $ble.promptTool).textFieldStyle(.roundedBorder)
                TextField("prompt.hint", text: $ble.promptHint).textFieldStyle(.roundedBorder)
            }
            Text("No session numbers are hardcoded as real. These fields start at zero and remain DRAFT until the proof card records a live BLE write; true device data appears only under Device RX.")
                .font(.caption)
                .foregroundStyle(.orange)
            HStack {
                Button("Send draft heartbeat", action: ble.sendHeartbeat).buttonStyle(.borderedProminent).tint(.green)
                Button("Send zeroed sample", action: ble.sendSampleHeartbeat).buttonStyle(.bordered)
                Button("Ask device status", action: ble.sendStatusRequest).buttonStyle(.bordered)
            }
            jsonBox(title: "Draft / last attempted JSON", text: ble.lastTXPretty.isEmpty ? ble.lastJSON : ble.lastTXPretty)
            jsonBox(title: "Last live BLE TX JSON", text: ble.lastLiveTXPretty)
        }
    }

    private var commandCard: some View {
        panel("Commands", subtitle: "Other safe Python protocol shapes. Unpair still requires on-device confirmation.") {
            Grid(alignment: .leading, horizontalSpacing: 14, verticalSpacing: 8) {
                GridRow { Text("name"); TextField("name", text: $ble.nameToSend).textFieldStyle(.roundedBorder) }
                GridRow { Text("owner"); TextField("owner", text: $ble.ownerToSend).textFieldStyle(.roundedBorder) }
                GridRow { Text("chat content"); TextField("content", text: $ble.chatContent).textFieldStyle(.roundedBorder) }
            }
            HStack {
                Button("Send name", action: ble.sendNameCommand).buttonStyle(.bordered)
                Button("Send owner", action: ble.sendOwnerCommand).buttonStyle(.bordered)
                Button("Send time", action: ble.sendTimeTick).buttonStyle(.bordered)
                Button("Send chat evt", action: ble.sendChatEvent).buttonStyle(.bordered)
                Button("Unpair", role: .destructive, action: ble.sendUnpairRequest).buttonStyle(.bordered)
            }
            Text("File-push commands are intentionally hidden because the Python firmware refuses them on unauthenticated BLE.")
                .font(.caption)
                .foregroundStyle(.secondary)
        }
    }

    private var rxCard: some View {
        panel("RX notify decode", subtitle: "Only real device notifications appear here; nothing is prefilled.") {
            rowsBox(title: "Flattened RX fields", rows: ble.lastRXRows)
            rowsBox(title: "Status ack fields", rows: ble.statusRows)
            rowsBox(title: "Hello fields", rows: ble.helloRows)
            rowsBox(title: "Permission fields", rows: ble.permissionRows)
            rowsBox(title: "Heartbeat RX fields", rows: ble.heartbeatRXRows)
            rowsBox(title: "Generic ack fields", rows: ble.genericAckRows)
            jsonBox(title: "Pretty RX JSON", text: ble.lastRXPretty)
        }
    }

    private var protocolMapCard: some View {
        panel("Protocol map from buddy_protocol.py", subtitle: "Static reference, not live data.") {
            LazyVGrid(columns: [GridItem(.adaptive(minimum: 320), spacing: 12)], spacing: 12) {
                ForEach(BuddyProtocolCatalog.facts) { fact in
                    VStack(alignment: .leading, spacing: 6) {
                        statusPill(fact.section, color: .cyan)
                        Text(fact.name).font(.headline)
                        Text(fact.shape).font(.system(.caption, design: .monospaced)).textSelection(.enabled)
                        Text(fact.note).font(.caption).foregroundStyle(.secondary)
                    }
                    .padding(12)
                    .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 14))
                }
            }
        }
    }

    private var logCard: some View {
        panel("Event log", subtitle: "Every scan/connect/write/notify event with timestamps.") {
            if ble.log.isEmpty {
                emptyState("No events yet", "Tap Scan or Send to create evidence.")
            } else {
                ForEach(Array(ble.log.enumerated()), id: \.offset) { _, line in
                    Text(line)
                        .font(.system(.caption, design: .monospaced))
                        .frame(maxWidth: .infinity, alignment: .leading)
                        .textSelection(.enabled)
                }
            }
        }
    }

    private func proofTile(title: String, badge: String, color: Color, body: String, rows: [DecodedRow]) -> some View {
        VStack(alignment: .leading, spacing: 10) {
            HStack {
                Text(title).font(.headline)
                Spacer()
                statusPill(badge, color: color)
            }
            Text(body)
                .font(.caption)
                .foregroundStyle(.secondary)
                .lineLimit(3)
            rowsBox(title: "evidence", rows: rows)
        }
        .padding(14)
        .frame(maxWidth: .infinity, minHeight: 245, alignment: .topLeading)
        .background(color.opacity(0.10), in: RoundedRectangle(cornerRadius: 18))
        .overlay(RoundedRectangle(cornerRadius: 18).stroke(color.opacity(0.25), lineWidth: 1))
    }

    private func metricStepper(_ title: String, value: Binding<Int>, range: ClosedRange<Int>, step: Int = 1) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption).foregroundStyle(.secondary)
            Stepper(value: value, in: range, step: step) {
                Text(value.wrappedValue.formatted()).font(.title3.monospacedDigit().bold())
            }
        }
        .padding(12)
        .background(.white.opacity(0.055), in: RoundedRectangle(cornerRadius: 14))
    }

    private func rowsBox(title: String, rows: [DecodedRow]) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption.bold()).foregroundStyle(.secondary)
            if rows.isEmpty {
                Text("none yet").font(.caption).foregroundStyle(.secondary)
            } else {
                ForEach(rows) { row in
                    HStack(alignment: .firstTextBaseline) {
                        Text(row.key)
                            .font(.system(.caption2, design: .monospaced))
                            .foregroundStyle(.secondary)
                            .frame(width: 92, alignment: .leading)
                        Text(row.value)
                            .font(.system(.caption2, design: .monospaced))
                            .lineLimit(2)
                            .textSelection(.enabled)
                    }
                }
            }
        }
        .padding(8)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.black.opacity(0.18), in: RoundedRectangle(cornerRadius: 10))
    }

    private func jsonBox(title: String, text: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.caption.bold()).foregroundStyle(.secondary)
            if text.isEmpty {
                Text("none yet").font(.caption).foregroundStyle(.secondary)
            } else {
                Text(text)
                    .font(.system(.caption, design: .monospaced))
                    .textSelection(.enabled)
            }
        }
        .padding(10)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.black.opacity(0.22), in: RoundedRectangle(cornerRadius: 12))
    }

    private func panel<Content: View>(_ title: String, subtitle: String? = nil, @ViewBuilder content: () -> Content) -> some View {
        VStack(alignment: .leading, spacing: 12) {
            VStack(alignment: .leading, spacing: 4) {
                Text(title).font(.title3.bold())
                if let subtitle { Text(subtitle).font(.callout).foregroundStyle(.secondary) }
            }
            content()
        }
        .padding(18)
        .cardGlass()
    }

    private func statusPill(_ text: String, color: Color) -> some View {
        Text(text)
            .font(.system(.caption, design: .rounded).bold())
            .padding(.horizontal, 10)
            .padding(.vertical, 6)
            .foregroundStyle(color)
            .background(color.opacity(0.14), in: Capsule())
            .overlay(Capsule().stroke(color.opacity(0.35), lineWidth: 1))
    }

    private func emptyState(_ title: String, _ message: String) -> some View {
        VStack(alignment: .leading, spacing: 6) {
            Text(title).font(.headline)
            Text(message).foregroundStyle(.secondary)
        }
        .padding(14)
        .frame(maxWidth: .infinity, alignment: .leading)
        .background(.white.opacity(0.05), in: RoundedRectangle(cornerRadius: 14))
    }
}


private enum CardputerPalette {
    static let orange = Color(red: 0.80, green: 0.47, blue: 0.36) // 0xCC785C
    static let cream = Color(red: 0.94, green: 0.93, blue: 0.90)  // 0xF0EEE6
    static let dark = Color(red: 0.12, green: 0.12, blue: 0.12)   // 0x1F1F1F
    static let black = Color.black
    static let grayMid = Color(red: 0.47, green: 0.47, blue: 0.47)
    static let green = Color.green
    static let cyan = Color.cyan
    static let yellow = Color.yellow
    static let red = Color.red
}

private struct CardputerBuddyScreen: View {
    @ObservedObject var ble: BuddyBLEModel

    private var liveRows: [DecodedRow] { ble.buddyDisplayRows }
    private var hasLiveHeartbeat: Bool { ble.hasLiveBuddyData && !liveRows.isEmpty }
    private var hasPrompt: Bool { !value("prompt.id").isEmpty || !value("prompt.tool").isEmpty }

    var body: some View {
        GeometryReader { geo in
            let scale = min(geo.size.width / 240.0, geo.size.height / 135.0)
            ZStack(alignment: .topLeading) {
                screenBody
                    .frame(width: 240, height: 135)
                    .scaleEffect(scale, anchor: .topLeading)
                    .frame(width: 240 * scale, height: 135 * scale, alignment: .topLeading)
            }
            .frame(maxWidth: .infinity, maxHeight: .infinity, alignment: .center)
        }
        .aspectRatio(240.0 / 135.0, contentMode: .fit)
        .padding(12)
        .background(
            RoundedRectangle(cornerRadius: 22)
                .fill(LinearGradient(colors: [.black, Color(red: 0.03, green: 0.03, blue: 0.035)], startPoint: .top, endPoint: .bottom))
        )
        .overlay(RoundedRectangle(cornerRadius: 22).stroke(.white.opacity(0.10), lineWidth: 1))
        .shadow(color: CardputerPalette.orange.opacity(0.18), radius: 24, x: 0, y: 10)
    }

    private var screenBody: some View {
        ZStack(alignment: .topLeading) {
            CardputerPalette.black
            header
            Rectangle().fill(CardputerPalette.orange).frame(width: 240, height: 1).offset(y: 20)
            mainPanel.offset(y: 21)
            hintStrip.offset(y: 111)
        }
        .clipShape(RoundedRectangle(cornerRadius: 4))
        .overlay(RoundedRectangle(cornerRadius: 4).stroke(CardputerPalette.grayMid.opacity(0.45), lineWidth: 1))
    }

    private var header: some View {
        ZStack(alignment: .topLeading) {
            CardputerPalette.dark.frame(width: 240, height: 20)
            screenText("Claude Buddy", x: 6, y: 5, color: CardputerPalette.orange)
            HStack { Spacer(); screenText(statusIcon.text, color: statusIcon.color) }
                .frame(width: 234, height: 20)
                .offset(x: 0, y: 5)
        }
    }

    private var mainPanel: some View {
        ZStack(alignment: .topLeading) {
            CardputerPalette.black.frame(width: 240, height: 90)
            if hasLiveHeartbeat {
                connectedMain
            } else if ble.rxServerCentrals > 0 {
                linkedNoHeartbeatMain
            } else if ble.claudeHomeActive {
                homeListeningMain
            } else if ble.rxServerActive {
                listeningMain
            } else {
                idleMain
            }
        }
    }

    private var idleMain: some View {
        ZStack(alignment: .topLeading) {
            screenText("Waiting to pair...", x: 6, y: 7, color: CardputerPalette.cream)
            screenText("Open Claude, go to", x: 6, y: 27, color: CardputerPalette.grayMid)
            screenText("Settings > Buddy", x: 6, y: 45, color: CardputerPalette.grayMid)
            screenText("and pick this one", x: 6, y: 63, color: CardputerPalette.grayMid)
        }
    }

    private var homeListeningMain: some View {
        ZStack(alignment: .topLeading) {
            screenText("Polling Claude Home", x: 6, y: 7, color: CardputerPalette.cream)
            screenText("Python /heartbeat", x: 6, y: 27, color: CardputerPalette.cyan)
            screenText("real JSON appears", x: 6, y: 45, color: CardputerPalette.grayMid)
            screenText("on this screen", x: 6, y: 63, color: CardputerPalette.grayMid)
        }
    }

    private var listeningMain: some View {
        ZStack(alignment: .topLeading) {
            screenText("Waiting for bridge...", x: 6, y: 7, color: CardputerPalette.cream)
            screenText("iPad is Claude-iPad", x: 6, y: 27, color: CardputerPalette.cyan)
            screenText("run ble_bridge.py", x: 6, y: 45, color: CardputerPalette.grayMid)
            screenText("real RX appears here", x: 6, y: 63, color: CardputerPalette.grayMid)
        }
    }

    private var linkedNoHeartbeatMain: some View {
        ZStack(alignment: .topLeading) {
            identityLine
            screenText("PAIR.. waiting data", x: 6, y: 21, color: CardputerPalette.cream)
            screenText("central subscribed", x: 6, y: 37, color: CardputerPalette.cyan)
            screenText("no heartbeat write yet", x: 6, y: 53, color: CardputerPalette.grayMid)
        }
    }

    private var connectedMain: some View {
        ZStack(alignment: .topLeading) {
            identityLine
            screenText(queueLine, x: 6, y: 21, color: .white)
            screenText(tokensLine, x: 6, y: 37, color: CardputerPalette.cyan)
            if hasPrompt {
                promptBox
            } else {
                screenText(clipped(value("msg", fallback: ble.buddyDisplayMessage), max: 31), x: 6, y: 53, color: CardputerPalette.grayMid)
            }
            footerLine
        }
    }

    private var identityLine: some View {
        ZStack(alignment: .topLeading) {
            screenText(clipped(deviceName, max: 22), x: 6, y: 5, color: CardputerPalette.orange)
            if !deviceOwner.isEmpty {
                screenText("<- " + clipped(deviceOwner, max: 13), x: 92, y: 5, color: CardputerPalette.grayMid)
            }
        }
    }

    private var promptBox: some View {
        ZStack(alignment: .topLeading) {
            Rectangle().stroke(CardputerPalette.orange, lineWidth: 1).frame(width: 234, height: 35).offset(x: 3, y: 53)
            screenText("PERM: " + clipped(value("prompt.tool").isEmpty ? ble.promptTool : value("prompt.tool"), max: 22), x: 7, y: 57, color: CardputerPalette.orange)
            screenText(clipped(value("prompt.hint").isEmpty ? ble.promptHint : value("prompt.hint"), max: 31), x: 7, y: 73, color: CardputerPalette.cream)
        }
    }

    private var footerLine: some View {
        ZStack(alignment: .topLeading) {
            CardputerPalette.black.frame(width: 240, height: 15).offset(y: 75)
            if ble.hasLiveHome && !ble.hasLiveRX {
                screenText("p:\(ble.claudeHomePollCount) b:\(ble.claudeHomeBytes)", x: 6, y: 77, color: CardputerPalette.grayMid)
                screenText("HOME", x: 196, y: 77, color: CardputerPalette.cream)
            } else {
                screenText("w:\(ble.rxServerWriteCount) b:\(ble.rxServerBytes)", x: 6, y: 77, color: CardputerPalette.grayMid)
                screenText("RX", x: 212, y: 77, color: CardputerPalette.cream)
            }
        }
    }

    private var hintStrip: some View {
        ZStack(alignment: .topLeading) {
            Rectangle().fill(CardputerPalette.orange).frame(width: 240, height: 1)
            CardputerPalette.dark.frame(width: 240, height: 23).offset(y: 1)
            screenText("Y once", x: 8, y: 6, color: CardputerPalette.cream)
            screenText("N deny", x: 96, y: 6, color: CardputerPalette.cream)
            screenText("Q exit", x: 196, y: 6, color: CardputerPalette.cream)
        }
        .frame(width: 240, height: 24)
    }

    private var statusIcon: (text: String, color: Color) {
        if ble.hasLiveRX { return ("LINKED", CardputerPalette.green) }
        if ble.hasLiveHome { return ("HOME", CardputerPalette.green) }
        if ble.rxServerCentrals > 0 { return ("PAIR..", CardputerPalette.yellow) }
        if ble.claudeHomeActive { return ("HOME..", CardputerPalette.cyan) }
        if ble.rxServerActive { return ("ADV", CardputerPalette.cyan) }
        switch ble.state {
        case .connected, .connecting:
            return ("PAIR..", CardputerPalette.yellow)
        case .disconnecting, .unsupported:
            return ("OFF", CardputerPalette.red)
        case .scanning, .idle:
            return ("ADV", CardputerPalette.cyan)
        }
    }

    private var deviceName: String {
        rowValue(in: ble.statusRows, key: "name") ?? rowValue(in: ble.helloRows, key: "name") ?? "Claude iPad"
    }

    private var deviceOwner: String {
        rowValue(in: ble.statusRows, key: "owner") ?? rowValue(in: ble.helloRows, key: "owner") ?? (ble.hasLiveHome ? "Home feed" : "BLE RX")
    }

    private var queueLine: String {
        "Q: \(countValue("running"))run \(countValue("waiting"))wait \(countValue("total"))tot"
    }

    private var tokensLine: String {
        let raw = Int(value("tokens_today", fallback: value("tokens", fallback: "0"))) ?? 0
        let tok = raw.formatted().replacingOccurrences(of: ",", with: "'")
        return "Today: \(tok) tok"
    }

    private func value(_ key: String, fallback: String = "") -> String {
        rowValue(in: liveRows, key: key) ?? fallback
    }

    private func countValue(_ key: String) -> String {
        let raw = value(key, fallback: "0").lowercased()
        if raw == "false" { return "0" }
        if raw == "true" { return "1" }
        return raw
    }

    private func rowValue(in rows: [DecodedRow], key: String) -> String? {
        rows.first(where: { $0.key == key })?.value
    }

    private func clipped(_ text: String, max: Int) -> String {
        guard text.count > max else { return text }
        return String(text.prefix(max - 1)) + "…"
    }

    private func screenText(_ text: String, x: CGFloat = 0, y: CGFloat = 0, color: Color) -> some View {
        Text(text)
            .font(.system(size: 9, weight: .regular, design: .monospaced))
            .foregroundStyle(color)
            .lineLimit(1)
            .fixedSize()
            .offset(x: x, y: y)
    }
}

private extension View {
    func cardGlass() -> some View {
        self
            .foregroundStyle(.white)
            .frame(maxWidth: .infinity, alignment: .leading)
            .background(
                ZStack {
                    RoundedRectangle(cornerRadius: 24)
                        .fill(Color(red: 0.06, green: 0.07, blue: 0.10).opacity(0.92))
                    RoundedRectangle(cornerRadius: 24)
                        .fill(
                            LinearGradient(
                                colors: [
                                    CardputerPalette.orange.opacity(0.10),
                                    Color.white.opacity(0.045),
                                    Color.black.opacity(0.18)
                                ],
                                startPoint: .topLeading,
                                endPoint: .bottomTrailing
                            )
                        )
                }
            )
            .overlay(RoundedRectangle(cornerRadius: 24).stroke(.white.opacity(0.10), lineWidth: 1))
            .shadow(color: .black.opacity(0.24), radius: 24, x: 0, y: 14)
    }
}

#Preview {
    ContentView().environmentObject(BuddyBLEModel())
}
