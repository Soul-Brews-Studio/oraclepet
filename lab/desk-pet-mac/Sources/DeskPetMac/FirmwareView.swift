import AppKit
import Combine
import PetCore
import PetProtocol
import SwiftUI
import ImageIO
import UniformTypeIdentifiers

/// The firmware images Desk Pet can send: the one built into the app bundle (copied in by
/// `just package` from lab/jc3248-pet-idf/build) and any .bin dropped or chosen, which is
/// copied into ~/Library/Application Support/Desk Pet/Firmware so it stays after the
/// original moves.
final class FirmwareLibrary: ObservableObject {
    struct Entry: Identifiable, Equatable {
        var id: URL { url }
        let url: URL
        let info: PetFirmware
        let size: Int
        let builtIn: Bool
        let added: Date
    }

    @Published private(set) var entries: [Entry] = []
    @Published private(set) var history: [String] = UserDefaults.standard.stringArray(forKey: "firmwareHistory") ?? []
    @Published var message: String?

    let dir: URL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Desk Pet/Firmware", isDirectory: true)

    init() { reload() }

    var builtIn: Entry? { entries.first(where: \.builtIn) }
    /// The built-in image for a given board (project), when the app bundles several.
    func builtIn(for project: String?) -> Entry? {
        guard let p = project, !p.isEmpty else { return builtIn }
        return entries.first { $0.builtIn && $0.info.project == p }
    }

    func reload() {
        let fm = FileManager.default
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var out: [Entry] = []
        let bundled = Bundle.main.resourceURL?.appendingPathComponent("Firmware")
        for (folder, isBuiltIn) in [(bundled, true), (dir, false)] {
            guard let folder, let files = try? fm.contentsOfDirectory(at: folder, includingPropertiesForKeys: [.contentModificationDateKey]) else { continue }
            for url in files where url.pathExtension == "bin" {
                guard let data = try? Data(contentsOf: url), let info = OTA.describe(data) else { continue }
                let date = (try? url.resourceValues(forKeys: [.contentModificationDateKey]).contentModificationDate) ?? .distantPast
                out.append(Entry(url: url, info: info, size: data.count, builtIn: isBuiltIn, added: date))
            }
        }
        entries = out.sorted { ($0.builtIn ? 1 : 0, $0.added) > ($1.builtIn ? 1 : 0, $1.added) }
    }

    /// Copy a .bin into the library. Refuses anything that is not a pet app image.
    @discardableResult
    func add(_ src: URL) -> Entry? {
        guard let data = try? Data(contentsOf: src) else { message = "Cannot read \(src.lastPathComponent)"; return nil }
        guard let p = OTA.projectName(of: data), OTA.petProjects[p] != nil, let info = OTA.describe(data) else {
            message = "\(src.lastPathComponent) is not a pet firmware (jc3248_pet_idf / oraclepet_lcd28)"
            return nil
        }
        let stamp = "\(info.version) \(info.built)".map { $0.isLetter || $0.isNumber || $0 == "-" ? $0 : "_" }
        let dst = dir.appendingPathComponent(String(stamp) + ".bin")
        do {
            try FileManager.default.createDirectory(at: dir, withIntermediateDirectories: true)
            if FileManager.default.fileExists(atPath: dst.path) { try FileManager.default.removeItem(at: dst) }
            try data.write(to: dst)
        } catch {
            message = "Could not copy it: \(error.localizedDescription)"
            return nil
        }
        message = "Copied \(src.lastPathComponent) into Desk Pet"
        reload()
        return entries.first { $0.url == dst }
    }

    /// To the Trash, never a hard delete.
    func remove(_ e: Entry) {
        guard !e.builtIn else { return }
        try? FileManager.default.trashItem(at: e.url, resultingItemURL: nil)
        reload()
    }

    func record(_ line: String) {
        let f = DateFormatter()
        f.dateFormat = "d MMM HH:mm:ss"
        history.insert("\(f.string(from: Date()))  \(line)", at: 0)
        history = Array(history.prefix(20))
        UserDefaults.standard.set(history, forKey: "firmwareHistory")
    }
}

/// Animation packs Desk Pet can send with a partial OTA: folders holding manifest.json and
/// the state GIFs, built into the app (Resources/Packs, from lab/buddy/characters) or dropped
/// in (copied to ~/Library/Application Support/Desk Pet/Packs).
final class PackLibrary: ObservableObject {
    struct Pack: Identifiable, Equatable {
        var id: String { name }
        let name: String
        let dir: URL
        let files: [URL]
        let bytes: Int
        let builtIn: Bool
        var preview: URL? { files.first { $0.lastPathComponent == "idle.gif" } ?? files.first { $0.pathExtension == "gif" } }
    }

    @Published private(set) var packs: [Pack] = []
    @Published var message: String?
    let dir: URL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Desk Pet/Packs", isDirectory: true)

    init() { reload() }

    func reload() {
        let fm = FileManager.default
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var out: [String: Pack] = [:]
        for (root, builtIn) in [(Bundle.main.resourceURL?.appendingPathComponent("Packs"), true), (dir, false)] {
            guard let root, let subs = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: nil) else { continue }
            for d in subs {
                if let p = Self.read(d, builtIn: builtIn) { out[p.name] = p }   // a dropped copy overrides the built-in
            }
        }
        packs = out.values.sorted { $0.name < $1.name }
    }

    static func read(_ d: URL, builtIn: Bool) -> Pack? {
        let fm = FileManager.default
        guard fm.fileExists(atPath: d.appendingPathComponent("manifest.json").path),
              let files = try? fm.contentsOfDirectory(at: d, includingPropertiesForKeys: [.fileSizeKey]) else { return nil }
        let keep = files.filter { ["gif", "json"].contains($0.pathExtension) }.sorted { $0.lastPathComponent < $1.lastPathComponent }
        let bytes = keep.reduce(0) { $0 + ((try? $1.resourceValues(forKeys: [.fileSizeKey]).fileSize) ?? 0) }
        let name = d.lastPathComponent.lowercased()
        guard name.range(of: "^[a-z0-9_-]{1,23}$", options: .regularExpression) != nil else { return nil }
        return Pack(name: name, dir: d, files: keep, bytes: bytes, builtIn: builtIn)
    }

    /// Copy a dropped pack folder in. It needs a manifest.json, like the ones in lab/buddy/characters.
    func add(_ src: URL) {
        guard Self.read(src, builtIn: false) != nil else {
            message = "\(src.lastPathComponent) is not a pack folder (manifest.json + GIFs, lower-case name)"
            return
        }
        let dst = dir.appendingPathComponent(src.lastPathComponent.lowercased())
        try? FileManager.default.trashItem(at: dst, resultingItemURL: nil)
        do { try FileManager.default.copyItem(at: src, to: dst); message = "Copied pack \(dst.lastPathComponent) into Desk Pet" }
        catch { message = "Could not copy it: \(error.localizedDescription)" }
        reload()
    }

    func files(of p: Pack) -> [(name: String, data: Data)] {
        p.files.compactMap { u in (try? Data(contentsOf: u)).map { (u.lastPathComponent, $0) } }
    }
}

/// Pet apps (.wasm built with lab/pet-sdk): in the app bundle (Resources/Apps) or dropped in
/// (copied to ~/Library/Application Support/Desk Pet/Apps).
final class AppLibrary: ObservableObject {
    struct App: Identifiable, Equatable {
        var id: String { name }
        let name: String
        let url: URL
        let bytes: Int
        let builtIn: Bool
        let added: Date
    }

    @Published private(set) var apps: [App] = []
    /// When each app last started on the pet (from "Pet runs <name>").
    @Published private(set) var lastRun: [String: Date] =
        (UserDefaults.standard.dictionary(forKey: "appLastRun") as? [String: Date]) ?? [:]

    func ran(_ name: String) {
        lastRun[name] = Date()
        UserDefaults.standard.set(lastRun, forKey: "appLastRun")
    }
    @Published var message: String?
    let dir: URL = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
        .appendingPathComponent("Desk Pet/Apps", isDirectory: true)

    init() { reload() }

    func reload() {
        let fm = FileManager.default
        try? fm.createDirectory(at: dir, withIntermediateDirectories: true)
        var out: [String: App] = [:]
        for (root, builtIn) in [(Bundle.main.resourceURL?.appendingPathComponent("Apps"), true), (dir, false)] {
            guard let root, let files = try? fm.contentsOfDirectory(at: root, includingPropertiesForKeys: [.fileSizeKey, .contentModificationDateKey]) else { continue }
            for u in files where u.pathExtension == "wasm" {
                let name = u.deletingPathExtension().lastPathComponent
                guard name.range(of: "^[a-z0-9_-]{1,23}$", options: .regularExpression) != nil else { continue }
                let rv = try? u.resourceValues(forKeys: [.fileSizeKey, .contentModificationDateKey])
                out[name] = App(name: name, url: u, bytes: rv?.fileSize ?? 0, builtIn: builtIn,
                                added: rv?.contentModificationDate ?? .distantPast)
            }
        }
        apps = out.values.sorted { $0.name < $1.name }
    }

    func add(_ src: URL) {
        let name = src.deletingPathExtension().lastPathComponent.lowercased()
        guard let d = try? Data(contentsOf: src), d.prefix(4) == Data([0, 0x61, 0x73, 0x6D]),
              name.range(of: "^[a-z0-9_-]{1,23}$", options: .regularExpression) != nil else {
            message = "\(src.lastPathComponent) is not a pet app (.wasm, lower-case name)"
            return
        }
        let dst = dir.appendingPathComponent(name + ".wasm")
        try? FileManager.default.trashItem(at: dst, resultingItemURL: nil)
        do { try d.write(to: dst); message = "Copied app \(name) into Desk Pet" } catch { message = error.localizedDescription }
        reload()
    }
}

/// Full page for the pet's firmware: what it runs, what Desk Pet can send it, the update in
/// progress, and what happened before. Opened from the menu bar or the Bluetooth card.
/// The Firmware page for whichever pet is picked at the top (OraclePet drives two).
/// Pet Firmware: a sidebar of every pet this Mac has seen (PetRegistry) and the chosen pet's page.
/// Two Bluetooth links stay the only connections: link A keeps the pet it drives (the fleet
/// heartbeat); picking any other pet moves link B (the workbench) to it. 2 pets or 100, same window.
struct FirmwareView: View {
    @ObservedObject var a: PetLink
    @ObservedObject var b: PetLink
    @ObservedObject var library: FirmwareLibrary
    @ObservedObject var packs: PackLibrary
    @ObservedObject var apps: AppLibrary
    @ObservedObject private var registry = PetRegistry.shared
    @StateObject private var updater = OutdatedUpdater()
    @State private var selection: String?
    @State private var search = ""
    @State private var filter = Filter.all
    @State private var renaming: PetRegistry.Pet?
    @State private var newLabel = ""

    enum Filter: String, CaseIterable, Identifiable {
        case all = "All", nearby = "Nearby", outdated = "Outdated", oldFirmware = "No OTA"
        var id: String { rawValue }
    }

    /// The newest firmware image the app holds for a board (by build time, then version).
    private func newest(for project: String) -> FirmwareLibrary.Entry? {
        library.entries.filter { $0.info.project == project }
            .max { (Self.builtDate($0.info.built) ?? .distantPast, $0.info.version) < (Self.builtDate($1.info.built) ?? .distantPast, $1.info.version) }
    }

    /// "Nov 16 2026 16:02:36" / "Nov  6 2026 09:01:02" (C __DATE__ __TIME__) -> Date. String order is wrong across months.
    static func builtDate(_ s: String) -> Date? {
        let f = DateFormatter()
        f.locale = Locale(identifier: "en_US_POSIX")
        f.dateFormat = "MMM d yyyy HH:mm:ss"
        return f.date(from: s.split(separator: " ", omittingEmptySubsequences: true).joined(separator: " "))
    }

    private func isOutdated(_ p: PetRegistry.Pet) -> Bool {
        guard p.hasOTA == true, !p.project.isEmpty, let n = newest(for: p.project) else { return false }
        return !(n.info.version == p.version && (n.info.sha.isEmpty || p.sha.isEmpty || n.info.sha.hasPrefix(p.sha) || p.sha.hasPrefix(n.info.sha)))
    }

    private var shown: [PetRegistry.Pet] {
        let q = search.lowercased()
        return registry.pets.filter { p in
            (q.isEmpty || p.name.lowercased().contains(q) || p.label.lowercased().contains(q) || p.version.lowercased().contains(q))
            && {
                switch filter {
                case .all: return true
                case .nearby: return p.seen(within: 60) || linkFor(p.id) != nil
                case .outdated: return isOutdated(p)
                case .oldFirmware: return p.hasOTA == false
                }
            }()
        }
    }

    private func linkFor(_ id: String) -> PetLink? {
        if a.peripheralID?.uuidString == id { return a }
        if b.peripheralID?.uuidString == id { return b }
        return nil
    }

    /// A transfer or USB flash is running: the list and the other send buttons wait.
    private var busy: Bool { a.ota.isBusy || b.ota.isBusy || updater.running || UsbFlasher.shared.isBusy }

    /// The link the detail page shows: the one already on the pet, else the workbench.
    private var detailLink: PetLink {
        if let s = selection, let l = linkFor(s) { return l }
        return b
    }

    var body: some View {
        NavigationSplitView {
            VStack(spacing: 0) {
                List(selection: Binding(get: { selection }, set: { if !busy { selection = $0 } })) {
                    ForEach(shown) { p in
                        row(p).tag(p.id)
                            .contextMenu {
                                Button("Set label…") { newLabel = p.label; renaming = p }
                                Button("Remove from list") { registry.remove(p.id) }.disabled(linkFor(p.id) != nil)
                            }
                    }
                }
                .searchable(text: $search, placement: .sidebar, prompt: "Search pets")
                Divider()
                VStack(alignment: .leading, spacing: 6) {
                    Picker("Show", selection: $filter) {
                        ForEach(Filter.allCases) { Text($0.rawValue).tag($0) }
                    }
                    .labelsHidden()
                    let linked = registry.pets.filter { linkFor($0.id) != nil }.count
                    let near = registry.pets.filter { $0.seen(within: 60) }.count
                    Text("\(registry.pets.count) pets · \(near) nearby · \(linked) linked")
                        .font(.caption).foregroundStyle(.secondary)
                    if busy && !updater.running {
                        Label("Updating: pet list locked until it finishes", systemImage: "lock.fill")
                            .font(.caption).foregroundStyle(.orange)
                    }
                    let outdated = registry.pets.filter(isOutdated)
                    Button(updater.running ? "Updating \(updater.done + 1)/\(updater.total)…" : "Update outdated (\(outdated.count))") {
                        updater.start(outdated.compactMap { p in newest(for: p.project).map { (p, $0) } }, link: b, avoid: a)
                    }
                    .disabled(busy || outdated.isEmpty)
                    if updater.running { Button("Stop after this pet") { updater.stop() }.controlSize(.small) }
                    if let m = updater.message { Text(m).font(.caption).foregroundStyle(.secondary).lineLimit(2) }
                }
                .padding(10)
            }
            .navigationSplitViewColumnWidth(min: 230, ideal: 260)
        } detail: {
            if selection != nil {
                FirmwarePage(link: detailLink, library: library, packs: packs, apps: apps)
                    .id(ObjectIdentifier(detailLink))
            } else {
                Text("Pick a pet").foregroundStyle(.secondary).frame(maxWidth: .infinity, maxHeight: .infinity)
            }
        }
        .onAppear {
            registry.note(a); registry.note(b)
            if let key = registry.requestedSelection, let p = registry.find(key) { selection = p.id; registry.requestedSelection = nil }
            if selection == nil { selection = (a.peripheralID ?? b.peripheralID)?.uuidString }
        }
        .onChange(of: registry.requestedSelection) { _, key in
            guard let key, !busy, let p = registry.find(key) else { return }
            selection = p.id; registry.requestedSelection = nil
        }
        .onChange(of: selection) { _, id in
            guard let id, linkFor(id) == nil, let u = UUID(uuidString: id), !updater.running else { return }
            b.use(u)                                        // the workbench moves to the chosen pet
        }
        .onReceive(a.objectWillChange.debounce(for: .milliseconds(300), scheduler: RunLoop.main)) { _ in registry.note(a) }
        .onReceive(b.objectWillChange.debounce(for: .milliseconds(300), scheduler: RunLoop.main)) { _ in registry.note(b) }
        .onReceive(NotificationCenter.default.publisher(for: .petUpdateOutdated)) { _ in
            let outdated = registry.pets.filter(isOutdated)
            updater.start(outdated.compactMap { p in newest(for: p.project).map { (p, $0) } }, link: b, avoid: a)
        }

        .alert("Label for \(renaming?.name ?? "")", isPresented: Binding(get: { renaming != nil }, set: { if !$0 { renaming = nil } })) {
            TextField("e.g. Nat desk", text: $newLabel)
            Button("Save") { if let r = renaming { registry.setLabel(newLabel, for: r.id) }; renaming = nil }
            Button("Cancel", role: .cancel) { renaming = nil }
        }
    }

    private func row(_ p: PetRegistry.Pet) -> some View {
        let link = linkFor(p.id)
        let linked = link?.isLinked ?? false
        let dot: Color = linked ? .green : (p.seen(within: 60) ? .gray : .clear)
        return HStack(spacing: 8) {
            Circle().fill(dot).overlay(Circle().stroke(.secondary.opacity(0.5), lineWidth: dot == .clear ? 1 : 0))
                .frame(width: 8, height: 8)
                .help(linked ? "Connected" : (p.seen(within: 60) ? "Nearby, not connected" : "Not seen in the last minute"))
                .accessibilityLabel(linked ? "Connected" : (p.seen(within: 60) ? "Nearby" : "Away"))
            VStack(alignment: .leading, spacing: 1) {
                HStack(spacing: 4) {
                    Text(p.shortName).font(.body.monospaced().weight(.medium))
                    if !p.label.isEmpty { Text(p.label).foregroundStyle(.secondary).lineLimit(1) }
                }
                HStack(spacing: 4) {
                    if p.hasOTA == false {
                        Label("old firmware", systemImage: "exclamationmark.triangle.fill").foregroundStyle(.orange)
                    } else if !p.version.isEmpty {
                        Text(p.version + (p.slot.isEmpty ? "" : " · " + p.slot))
                        if isOutdated(p) { Image(systemName: "arrow.up.circle.fill").foregroundStyle(.blue).help("Newer firmware in the library") }
                    } else {
                        Text("not linked yet")
                    }
                    if let r = p.rssi, p.seen(within: 60) { Text("\(r) dBm").foregroundStyle(.tertiary) }
                }
                .font(.caption).foregroundStyle(.secondary).lineLimit(1)
            }
            Spacer(minLength: 0)
            if link === a { Image(systemName: "heart.fill").font(.caption2).foregroundStyle(.pink).help("Follows the herdr fleet (link A)") }
        }
        .padding(.vertical, 2)
    }
}

extension Notification.Name {
    /// deskpet://select?pet=5610 -> the Pet Firmware sidebar selects that pet.
    static let petSelect = Notification.Name("co.laris.deskpet.select")
    /// deskpet://update?outdated=1 -> "Update outdated" (agents, fleet scripts)
    static let petUpdateOutdated = Notification.Name("co.laris.deskpet.updateOutdated")
}

/// "Update outdated": one pet after another on the workbench link. Each step moves link B to
/// the pet, waits until it can take an update, sends the newest image for its board, waits for
/// done or failed, then goes on. Never touches link A's pet. Stops after a failure.
final class OutdatedUpdater: ObservableObject {
    @Published private(set) var running = false
    @Published private(set) var done = 0
    @Published private(set) var total = 0
    @Published private(set) var message: String?
    private var queue: [(PetRegistry.Pet, FirmwareLibrary.Entry)] = []
    private var stopRequested = false
    private weak var link: PetLink?
    private var timer: Timer?
    private var phaseStarted = Date()
    private enum Step { case connecting, sending, waiting }
    private var step = Step.connecting
    private var startedOTA = false
    private var sawBusy = false               // a .done/.failed left from the last pet is not this one

    func start(_ jobs: [(PetRegistry.Pet, FirmwareLibrary.Entry)], link: PetLink, avoid: PetLink) {
        let held = avoid.peripheralID?.uuidString
        queue = jobs.filter { $0.0.id != held }
        let skipped = jobs.count - queue.count
        guard !queue.isEmpty else { message = skipped > 0 ? "Only the fleet pet is outdated: update it from its own page" : nil; return }
        self.link = link
        total = queue.count; done = 0; stopRequested = false; running = true
        message = skipped > 0 ? "Skipping the fleet pet (link A): update it from its page" : nil
        next()
    }

    func stop() { stopRequested = true; message = "Stopping after this pet" }

    private func next() {
        timer?.invalidate()
        guard running, let link else { return finish(nil) }
        if stopRequested || queue.isEmpty { return finish(queue.isEmpty ? "Updated \(done) of \(total)" : "Stopped: \(done) of \(total) updated") }
        let (pet, _) = queue[0]
        guard let id = pet.uuid else { queue.removeFirst(); return next() }
        message = "\(pet.name): connecting"
        step = .connecting; startedOTA = false; phaseStarted = Date()
        link.use(id)
        timer = Timer.scheduledTimer(withTimeInterval: 1, repeats: true) { [weak self] _ in self?.tick() }
    }

    private func tick() {
        guard let link, !queue.isEmpty else { return }
        let (pet, image) = queue[0]
        let waited = Date().timeIntervalSince(phaseStarted)
        switch step {
        case .connecting:
            if link.peripheralID?.uuidString == pet.id, link.canUpdateFirmware, link.firmware != nil || waited > 15 {
                guard let data = try? Data(contentsOf: image.url) else { return fail(pet, "cannot read \(image.url.lastPathComponent)") }
                if let fw = link.firmware, fw.project != image.info.project { return fail(pet, "board is \(fw.project), image is \(image.info.project)") }
                message = "\(pet.name): sending \(image.info.version)"
                link.updateFirmware(data)
                if case .failed(let m) = link.ota { return fail(pet, m) }   // refused on the spot
                step = .sending; phaseStarted = Date(); sawBusy = link.ota.isBusy
            } else if waited > 45 {
                fail(pet, "not reachable in 45 s")
            }
        case .sending, .waiting:
            if link.ota.isBusy { sawBusy = true }
            switch link.ota {
            case .done(let m) where sawBusy: message = "\(pet.name): \(m)"; done += 1; queue.removeFirst(); next()
            case .failed(let m) where sawBusy: fail(pet, m)
            case .sending(let f): message = "\(pet.name): \(Int(f * 100))%"
            default: if waited > 240 { fail(pet, "no result in 4 min") }
            }
        }
    }

    private func fail(_ pet: PetRegistry.Pet, _ why: String) { finish("\(pet.name): \(why) — stopped (\(done) of \(total) updated)") }

    private func finish(_ m: String?) {
        timer?.invalidate(); timer = nil
        running = false; queue = []
        if let m { message = m }
    }
}

struct FirmwarePage: View {
    @ObservedObject var link: PetLink
    @AppStorage("voiceAnnounce") private var announce = true
    @AppStorage("voiceLanguage") private var voiceLanguage = "th"
    @AppStorage("voiceEngine") private var voiceEngine = "macos"
    @AppStorage("paxaVoice") private var paxaVoice = "khanomkrok"
    @State private var paxaKey = ""
    @State private var paxaHasKey = PaxaVoice.hasKey
    @State private var sayText = ""
    @ObservedObject var library: FirmwareLibrary
    @ObservedObject var packs: PackLibrary
    @ObservedObject var apps: AppLibrary
    @ObservedObject private var usb = UsbFlasher.shared
    @State private var usbApp: URL?
    @State private var dropping = false

    var body: some View {
        ScrollView {
            VStack(alignment: .leading, spacing: 16) {
                header
                packsCard                                      // most used: change the character
                progressCard                                   // always there: the page never shifts
                onPetCard
                appsCard
                libraryCard
                usbCard
                voiceCard
                Text("First time on a pet: flash once over USB (the OTA partition table and bootloader). After that every update goes over Bluetooth, and a bad image rolls back to the previous one by itself.")
                    .font(.caption).foregroundStyle(.tertiary)
            }
            .padding(20)
        }
        .frame(minWidth: 520, minHeight: 560)
        .onDrop(of: [.fileURL], isTargeted: $dropping) { providers in
            for p in providers {
                _ = p.loadObject(ofClass: URL.self) { url, _ in
                    guard let url else { return }
                    var isDir: ObjCBool = false
                    FileManager.default.fileExists(atPath: url.path, isDirectory: &isDir)
                    DispatchQueue.main.async {
                        if isDir.boolValue { packs.add(url) } else if url.pathExtension == "wasm" { apps.add(url) } else { library.add(url) }
                    }   // folder = pack, .bin = firmware
                }
            }
            return true
        }
        .onChange(of: link.ota) { _, phase in
            switch phase {
            case .done(let m):
                library.record("✓ " + m)
                if m.hasPrefix("Pet runs ") {
                    apps.ran(String(m.dropFirst(9).prefix { $0 != " " }))
                }
            case .failed(let m): library.record("✗ " + m)
            default: break
            }
        }
    }

    private var header: some View {
        HStack(spacing: 12) {
            Image(systemName: "cpu").font(.system(size: 30)).foregroundStyle(.orange)
            VStack(alignment: .leading, spacing: 2) {
                Text("Pet Firmware").font(.title2.weight(.semibold))
                Text("\(link.petName ?? "No pet") · \(link.state.label)").foregroundStyle(.secondary)
            }
            Spacer()
        }
    }

    private func otherBoard(_ e: FirmwareLibrary.Entry) -> Bool {
        guard let p = link.firmware?.project, !p.isEmpty else { return false }
        return p != e.info.project
    }

    private func onPet(_ e: FirmwareLibrary.Entry) -> Bool {
        guard let fw = link.firmware else { return false }
        return fw.sameBuild(as: e.info) && !(fw.sha.isEmpty && !e.info.sha.isEmpty)   // an old pet without sha: offer the update
    }

    private var onPetCard: some View {
        GroupBox {
            HStack(alignment: .top) {
                if let fw = link.firmware {
                    VStack(alignment: .leading, spacing: 4) {
                        Text(fw.version).font(.title3.monospaced().weight(.medium))
                        Text("Built \(fw.built) · running from \(fw.slot)\(fw.sha.isEmpty ? "" : " · " + fw.sha)").font(.callout).foregroundStyle(.secondary)
                        if fw.state == "pending" {
                            Label("Proving itself: rolls back if it resets before its first heartbeat", systemImage: "hourglass")
                                .font(.caption).foregroundStyle(.orange)
                        } else if fw.state == "valid" {
                            Label("Confirmed: this image stays after a reset", systemImage: "checkmark.shield")
                                .font(.caption).foregroundStyle(.green)
                        }
                    }
                    Spacer()
                    if let b = library.builtIn(for: fw.project) {
                        if onPet(b) {
                            Label("Up to date", systemImage: "checkmark.seal.fill").foregroundStyle(.green)
                        } else {
                            Button { install(b) } label: { Label("Install built-in", systemImage: "arrow.down.circle.fill") }
                                .buttonStyle(.borderedProminent).disabled(!link.canQueueUpdate)
                        }
                    }
                } else {
                    Text(link.isLinked ? "This pet's firmware has no OTA service yet: flash it once over USB."
                                       : "Connect the pet to see its firmware.")
                        .foregroundStyle(.secondary)
                    Spacer()
                }
            }
            .padding(6)
        } label: {
            Label("On the pet", systemImage: "cat")
        }
    }

    /// One layout for every phase: title + percent on one line, the bar always in the same place,
    /// one detail line. Sizes never change, so nothing jumps while files come and go.
    private var progressCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack(alignment: .center, spacing: 12) {
                    // the big number is always there (%, ✓ or ✗), so the row never changes height
                    Text(percentText)
                        .font(.system(size: 34, weight: .semibold, design: .rounded)).monospacedDigit()
                        .foregroundStyle(tint == .primary ? Color.accentColor : tint)
                        .frame(width: 96, alignment: .leading)
                    VStack(alignment: .leading, spacing: 2) {
                        Text(progressTitle).font(.headline).foregroundStyle(tint).lineLimit(1)
                        Text(detailLine).font(.caption.monospacedDigit()).foregroundStyle(.secondary).lineLimit(1)
                    }
                    Spacer(minLength: 4)
                    Button("Cancel") { link.cancelFirmwareUpdate() }
                        .controlSize(.small)
                        .opacity(link.ota.isBusy ? 1 : 0).disabled(!link.ota.isBusy)
                }
                .frame(height: 44)
                ProgressView(value: progressValue).tint(tint == .primary ? .accentColor : tint)
                // the current part (one file of a pack, the app, the firmware), its own 0-100
                HStack(spacing: 8) {
                    Text(partLabel).font(.caption2.monospacedDigit()).foregroundStyle(.secondary)
                        .frame(width: 118, alignment: .leading)
                    ProgressView(value: link.ota.isBusy ? link.otaPart : 0).controlSize(.mini).tint(.secondary)
                    Text(link.ota.isBusy ? "\(Int(link.otaPart * 100))%" : "").font(.caption2.monospacedDigit())
                        .foregroundStyle(.secondary).frame(width: 34, alignment: .trailing)
                }
                .opacity(link.ota.isBusy ? 1 : 0.35)
                // what happened before, newest on top, with date and time
                if !library.history.isEmpty {
                    Divider().padding(.top, 2)
                    ForEach(Array(library.history.prefix(6).enumerated()), id: \.offset) { i, line in
                        historyRow(line, newest: i == 0)
                    }
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        } label: {
            Label("Update", systemImage: "arrow.triangle.2.circlepath")
        }
    }

    private var tint: Color {
        switch link.ota {
        case .failed: return .red
        case .done: return .green
        default: return .primary
        }
    }

    private var progressValue: Double {
        switch link.ota {
        case .sending: return link.otaOverall               // the whole batch, 0-100 once
        case .finishing, .rebooting, .done: return 1
        case .idle: return 0
        default: return link.otaOverall
        }
    }

    private var percentText: String {
        switch link.ota {
        case .failed: return "✗"
        case .done: return "✓"
        case .idle: return "–"
        default: return "\(Int(progressValue * 100))%"
        }
    }

    private var progressTitle: String {
        switch link.ota {
        case .starting, .sending: return "Sending"
        case .finishing: return "Pet is checking it…"
        case .rebooting: return "Pet is restarting…"
        case .done: return "Done"
        case .failed: return "Failed"
        case .idle: return "Ready"
        }
    }

    private var partLabel: String {
        guard link.ota.isBusy else { return "part" }
        let job = link.otaJobCount > 1 ? "job \(link.otaJobIndex)/\(link.otaJobCount) " : ""
        return link.otaFileCount > 1 ? "\(job)file \(link.otaFileIndex)/\(link.otaFileCount)" : "\(job)part"
    }

    private var detailLine: String {
        var parts: [String] = []
        if link.ota.isBusy {
            if link.otaBatchTotal > 0 { parts.append(rateLine) }
        } else if link.ota == .idle {
            parts.append("Send firmware, a pack or an app below; requests queue up")
        } else {
            parts.append(link.ota.label)
        }
        return parts.joined(separator: " · ")
    }

    private var rateLine: String {
        let secs = max(Date().timeIntervalSince(link.otaStartedAt), 0.1)
        let kbps = Double(link.otaAcked) / 1024 / secs
        let done = link.otaBatchDone + link.otaAcked
        let left = kbps > 0 ? Int(Double(link.otaBatchTotal - done) / 1024 / kbps) : 0
        return "\(done / 1024) / \(link.otaBatchTotal / 1024) KB · \(String(format: "%.1f", kbps)) KB/s · about \(left) s left"
    }

    private var libraryCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                ForEach(library.entries) { e in
                    HStack(spacing: 10) {
                        Image(systemName: e.builtIn ? "shippingbox.fill" : "doc.fill")
                            .foregroundStyle(e.builtIn ? .orange : .secondary).frame(width: 18)
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(spacing: 6) {
                                Text(e.info.version).font(.callout.monospaced())
                                if e.builtIn { tag("built-in", .orange) }
                                tag(OTA.boardName(e.info.project), .blue)
                                if let p = link.firmware?.project, !p.isEmpty, p != e.info.project { tag("for another board", .secondary) }
                                if onPet(e) { tag("on the pet", .green) }
                            }
                            Text("Built \(e.info.built) · \(e.size / 1024) KB · \(e.info.sha)").font(.caption).foregroundStyle(.secondary)
                        }
                        Spacer()
                        Button("Install") { install(e) }
                            .disabled(!link.canQueueUpdate || onPet(e) || otherBoard(e))
                    }
                    .contextMenu {
                        Button("Show in Finder") { NSWorkspace.shared.activateFileViewerSelecting([e.url]) }
                        if !e.builtIn { Button("Move to Trash") { library.remove(e) } }
                    }
                    Divider()
                }
                dropZone
                if let m = library.message {
                    Text(m).font(.caption).foregroundStyle(.secondary)
                }
            }
            .padding(6)
        } label: {
            Label("Firmware in Desk Pet", systemImage: "tray.full")
        }
    }

    // MARK: Animation packs (partial OTA)

    private var packsCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Text("Partial update: only the animation goes over, the firmware stays and nothing restarts.")
                    .font(.caption).foregroundStyle(.secondary)
                if let fw = link.firmware, !fw.packs.isEmpty {
                    HStack(spacing: 6) {
                        Text("On the pet:").font(.caption).foregroundStyle(.secondary)
                        ScrollView(.horizontal, showsIndicators: false) { HStack(spacing: 6) {
                        ForEach(fw.packs, id: \.self) { name in
                            Button(name) { link.selectPack(name) }
                                .buttonStyle(.bordered).controlSize(.small)
                                .tint(name == fw.pack ? .green : nil)
                                .disabled(name == fw.pack || !link.canQueueUpdate)
                                .help(name == fw.pack ? "Showing now" : "Show \(name) now (no upload)")
                        }
                        } }
                        Text("\(fw.freeKB) KB free").font(.caption2).foregroundStyle(.tertiary)
                    }
                    .lineLimit(1)
                }
                LazyVGrid(columns: [GridItem(.adaptive(minimum: 112), spacing: 10)], spacing: 10) {
                    ForEach(packs.packs) { p in packTile(p) }
                }
                if let m = packs.message { Text(m).font(.caption).foregroundStyle(.secondary) }
                Text("Drop a pack folder (manifest.json + GIFs, as in lab/buddy/characters) anywhere on this page to copy it in.")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
            .padding(6)
        } label: {
            Label("Animation packs", systemImage: "sparkles.rectangle.stack")
        }
    }

    private func packTile(_ p: PackLibrary.Pack) -> some View {
        let onPet = link.firmware?.packs.contains(p.name) ?? false
        let showing = link.firmware?.pack == p.name
        return VStack(spacing: 4) {
            GIFThumb(url: p.preview)
                .frame(width: 72, height: 75)
                .clipped()
            Text(p.name).font(.callout.weight(.medium)).lineLimit(1)
            Text("\(p.bytes / 1024) KB\(showing ? " · showing" : onPet ? " · on pet" : "")").font(.caption2).foregroundStyle(.secondary)
            Button(showing ? "Showing" : "Send") { link.uploadPack(p.name, files: packs.files(of: p)) }
                .controlSize(.small)
                .disabled(showing || !link.canQueueUpdate)
                .help(onPet ? "Send again (replaces the copy on the pet) and show it" : "Copy to the pet and show it")
        }
        .padding(8)
        .frame(maxWidth: .infinity, minHeight: 150, maxHeight: 150)       // every tile the same box
        .background(Color.secondary.opacity(0.08), in: RoundedRectangle(cornerRadius: 10))
    }

    // MARK: Voice (the pet speaks: macOS speech -> IMA ADPCM clip -> the pet's speaker)

    private var voiceCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    TextField(voiceLanguage == "th" ? "พิมพ์ให้น้องพูด…" : "Say something on the pet…", text: $sayText)
                        .textFieldStyle(.roundedBorder)
                        .onSubmit(say)
                    Button("Say", action: say)
                        .keyboardShortcut(.return, modifiers: [.command])
                        .disabled(sayText.trimmingCharacters(in: .whitespaces).isEmpty || !link.canQueueUpdate)
                    Button("Play again") { link.replay() }
                        .disabled(link.lastClip == nil || !link.canQueueUpdate)
                }
                HStack {
                    Toggle("Speak when an agent needs you", isOn: $announce)
                    Spacer()
                    Picker("", selection: $voiceLanguage) {
                        Text("English").tag("en")
                        Text("ไทย").tag("th")
                    }
                    .pickerStyle(.segmented).labelsHidden().frame(width: 140)
                }
                .font(.callout)
                HStack {
                    Picker("Voice", selection: $voiceEngine) {
                        Text("macOS").tag("macos")
                        Text("Paxa (Thai+English)").tag("paxa")
                    }
                    .pickerStyle(.segmented).frame(width: 260)
                    if voiceEngine == "paxa" {
                        Picker("", selection: $paxaVoice) {
                            ForEach(PaxaVoice.voices, id: \.self) { Text($0).tag($0) }
                        }
                        .labelsHidden().frame(width: 120)
                    }
                }
                .font(.callout)
                if voiceEngine == "paxa" {
                    HStack {
                        SecureField(paxaHasKey ? "Paxa API key saved in the Keychain" : "Paxa API key (paxalabs.com)", text: $paxaKey)
                            .textFieldStyle(.roundedBorder)
                        Button(paxaHasKey && paxaKey.isEmpty ? "Remove key" : "Save key") {
                            PaxaVoice.setKey(paxaKey); paxaKey = ""; paxaHasKey = PaxaVoice.hasKey
                        }
                        .controlSize(.small)
                    }
                    Text(paxaHasKey ? "Paxa reads Thai and English as written (about 10 credits per 1000 characters); if a request fails the macOS voice speaks instead."
                                    : "Without a key the macOS voice speaks.")
                        .font(.caption2).foregroundStyle(.tertiary)
                }
                HStack {
                    Button("Test speaker") { link.testSpeaker() }.controlSize(.small).disabled(!link.isLinked)
                    if let t = link.speakerTest { Text(t).font(.caption).foregroundStyle(.secondary) }
                }
                HStack {
                    Button("Listen 3 s") { link.listen(ms: 3000, sendAudio: true) }.controlSize(.small).disabled(!link.isLinked)
                    if let url = link.micRecording {
                        Button("Play recording") { NSSound(contentsOf: url, byReference: false)?.play() }.controlSize(.small)
                    }
                    if let t = link.micStatus { Text(t).font(.caption).foregroundStyle(.secondary).lineLimit(1) }
                }
                if let t = link.micText {
                    Text("Heard words: \(t)").font(.callout).textSelection(.enabled).lineLimit(3)
                }
                Text("Spoken by macOS in Thai (names like neo or nexus are read in Thai; pick English to keep English), sent as a 16 kHz clip of a few KB, played by the pet. Wasm apps can speak too: pet.say(\"…\").")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
            .padding(6)
        } label: {
            Label("Voice", systemImage: "speaker.wave.2.fill")
        }
    }

    private func say() {
        let t = sayText.trimmingCharacters(in: .whitespacesAndNewlines)
        guard !t.isEmpty else { return }
        link.say(t, language: PetSound.isThai(t) ? "th-TH" : (voiceLanguage == "th" ? "th-TH" : "en-US"))
        sayText = ""
    }

    // MARK: Pet apps (WebAssembly)

    private var appsCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                Text("A pet app is a few KB of WebAssembly (lab/pet-sdk) running on the pet's firmware. Sending one is a partial update too.")
                    .font(.caption).foregroundStyle(.secondary)
                if let fw = link.firmware {
                    HStack(spacing: 6) {
                        Text(fw.app.isEmpty ? "No app running: the pet follows the fleet" : "Running: \(fw.app)").font(.callout)
                        Spacer()
                        if !fw.app.isEmpty {
                            Button("Stop app") { link.runApp("") }.controlSize(.small).disabled(!link.canQueueUpdate)
                        }
                    }
                    if !fw.appError.isEmpty {
                        Text(fw.appError).font(.caption).foregroundStyle(.red)
                    }
                }
                ForEach(apps.apps) { a in
                    let onPet = link.firmware?.apps.contains(a.name) ?? false
                    let running = link.firmware?.app == a.name
                    HStack(spacing: 10) {
                        Image(systemName: "puzzlepiece.extension.fill").foregroundStyle(.purple).frame(width: 18)
                        VStack(alignment: .leading, spacing: 1) {
                            HStack(spacing: 6) {
                                Text(a.name).font(.callout.monospaced())
                                if a.builtIn { tag("built-in", .orange) }
                                if running { tag("running", .green) } else if onPet { tag("on the pet", .secondary) }
                            }
                            Text(appLine(a)).font(.caption.monospacedDigit()).foregroundStyle(.secondary)
                        }
                        Spacer()
                        if onPet && !running {
                            Button("Run") { link.runApp(a.name) }.disabled(!link.canQueueUpdate)
                        }
                        Button(onPet ? "Send again" : "Send & run") {
                            if let d = try? Data(contentsOf: a.url) { link.uploadApp(a.name, wasm: d) }
                        }
                        .disabled(!link.canQueueUpdate)
                    }
                }
                if let m = apps.message { Text(m).font(.caption).foregroundStyle(.secondary) }
                Text("Drop a .wasm on this page to copy it in.  Build one: lab/pet-sdk/build.sh examples/hello.c")
                    .font(.caption2).foregroundStyle(.tertiary)
            }
            .padding(6)
        } label: {
            Label("Pet apps (WebAssembly)", systemImage: "puzzlepiece.extension")
        }
    }

    private static let stamp: DateFormatter = { let f = DateFormatter(); f.dateFormat = "d MMM HH:mm:ss"; return f }()

    private func appLine(_ a: AppLibrary.App) -> String {
        var t = "\(a.bytes) bytes · built \(Self.stamp.string(from: a.added))"
        if let r = apps.lastRun[a.name] { t += " · ran \(Self.stamp.string(from: r))" }
        return t
    }

    private func tag(_ s: String, _ c: Color) -> some View {
        Text(s).font(.caption2.weight(.semibold)).padding(.horizontal, 6).padding(.vertical, 1)
            .background(c.opacity(0.18), in: Capsule()).foregroundStyle(c)
    }

    private var dropZone: some View {
        HStack {
            Image(systemName: "square.and.arrow.down").font(.title3)
            VStack(alignment: .leading, spacing: 2) {
                Text("Drop a .bin here to copy it into Desk Pet")
                Text("lab/jc3248-pet-idf/build/*.bin (jc3248) · build-lcd28/*.bin (2.8in)").font(.caption.monospaced()).foregroundStyle(.secondary)
            }
            Spacer()
            Button("Choose…") { choose() }
        }
        .padding(12)
        .frame(maxWidth: .infinity)
        .background(RoundedRectangle(cornerRadius: 10).strokeBorder(style: StrokeStyle(lineWidth: 1.5, dash: [6, 4]))
            .foregroundStyle(dropping ? Color.accentColor : Color.secondary.opacity(0.5)))
    }

    /// "21 Oct 12:30:05  ✓ Pet runs hello-zig" -> time column + mark + text
    private func historyRow(_ line: String, newest: Bool) -> some View {
        let parts = line.components(separatedBy: "  ")
        let when = parts.first ?? ""
        let what = parts.dropFirst().joined(separator: "  ")
        let ok = what.hasPrefix("✓")
        return HStack(spacing: 8) {
            Text(when).font(.caption.monospacedDigit()).foregroundStyle(newest ? .primary : .secondary)
                .frame(width: 118, alignment: .leading)
            Image(systemName: ok ? "checkmark.circle.fill" : "xmark.circle.fill")
                .foregroundStyle(ok ? .green : .red).font(.caption)
            Text(what.dropFirst(2)).font(.caption).foregroundStyle(newest ? .primary : .secondary).lineLimit(1)
            Spacer(minLength: 0)
        }
    }

    // MARK: USB (espflash, esp-rs): first install or rescue

    private var usbCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 8) {
                HStack {
                    Picker("Port", selection: $usb.port) {
                        if usb.ports.isEmpty { Text("No USB pet").tag("") }
                        ForEach(usb.ports, id: \.self) { Text($0.replacingOccurrences(of: "/dev/", with: "")).tag($0) }
                    }
                    .frame(maxWidth: 260)
                    Button { usb.refreshPorts() } label: { Image(systemName: "arrow.clockwise") }
                        .help("Look for USB ports again")
                    Spacer()
                    if usb.espflash == nil { tag("espflash missing", .red) } else { tag("espflash", .blue) }
                }
                HStack {
                    Button(usb.folder == nil ? "Choose firmware folder…" : usb.folder!.lastPathComponent) { chooseUsbFolder() }
                        .help(usb.folder?.path ?? "A board folder from the release: bootloader.bin, partition-table.bin, ota_data_initial.bin, app-<pack>.bin")
                    let images = usb.appImages()
                    Picker("App", selection: $usbApp) {
                        if images.isEmpty { Text("no app-*.bin").tag(URL?.none) }
                        ForEach(images, id: \.self) { Text($0.lastPathComponent).tag(URL?.some($0)) }
                    }
                    .frame(maxWidth: 240)
                    .onAppear { if usbApp == nil { usbApp = images.first } }
                    .onChange(of: usb.folder) { _, _ in usbApp = usb.appImages().first }
                }
                Toggle("Also write storage (every pack; replaces packs sent over Bluetooth)", isOn: $usb.includeStorage)
                    .font(.callout)
                HStack {
                    Button(usb.isBusy ? "Flashing…" : "Flash over USB") { if let a = usbApp { usb.flash(app: a) } }
                        .disabled(usb.isBusy || link.ota.isBusy || usbApp == nil || usb.port.isEmpty || usb.espflash == nil)
                        .buttonStyle(.borderedProminent)
                    if usb.isBusy { Button("Stop") { usb.cancel() } }
                    Spacer()
                }
                if usb.isBusy || usb.progress > 0 {
                    ProgressView(value: usb.progress)
                }
                switch usb.phase {
                case .idle: EmptyView()
                case .running(let m): Text(m).font(.caption.monospaced()).foregroundStyle(.secondary)
                case .done(let m): Label(m, systemImage: "checkmark.circle.fill").foregroundStyle(.green).font(.callout)
                case .failed(let m): Label(m, systemImage: "xmark.octagon.fill").foregroundStyle(.red).font(.callout)
                }
                if !usb.lastLine.isEmpty {
                    Text(usb.lastLine).font(.caption.monospaced()).foregroundStyle(.tertiary).lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        } label: {
            Label("USB (espflash)", systemImage: "cable.connector")
        }
        .onAppear { usb.refreshPorts() }
    }

    private func chooseUsbFolder() {
        let panel = NSOpenPanel()
        panel.title = "Choose a board folder (e.g. oraclepet-fw/jc3248)"
        panel.canChooseDirectories = true
        panel.canChooseFiles = false
        if panel.runModal() == .OK, let u = panel.url { usb.folder = u }
    }

    private var historyCard: some View {
        GroupBox {
            VStack(alignment: .leading, spacing: 4) {
                ForEach(Array(library.history.prefix(6).enumerated()), id: \.offset) { _, line in
                    Text(line).font(.caption.monospaced()).foregroundStyle(.secondary).lineLimit(1)
                }
            }
            .frame(maxWidth: .infinity, alignment: .leading)
            .padding(6)
        } label: {
            Label("History", systemImage: "clock")
        }
    }

    private func choose() {
        let panel = NSOpenPanel()
        panel.title = "Choose a pet firmware (jc3248_pet_idf.bin)"
        panel.allowedContentTypes = [UTType(filenameExtension: "bin") ?? .data]
        panel.allowsMultipleSelection = true
        if panel.runModal() == .OK { panel.urls.forEach { library.add($0) } }
    }

    private func install(_ e: FirmwareLibrary.Entry) {
        guard let data = try? Data(contentsOf: e.url) else { return }
        link.updateFirmware(data)
    }
}

/// A looping GIF thumbnail with the device's screen colour keyed out (pack GIFs are drawn on the
/// pet's background, black for most). Only background connected to the frame edge goes, so
/// outlines, pupils and a black cat (neo) stay. Re-encoded once per file and cached.
struct GIFThumb: NSViewRepresentable {
    var url: URL?
    func makeNSView(context: Context) -> NSImageView {
        let v = NSImageView()
        v.animates = true
        v.imageScaling = .scaleProportionallyUpOrDown
        v.setContentCompressionResistancePriority(.defaultLow, for: .horizontal)   // the frame decides the size
        v.setContentCompressionResistancePriority(.defaultLow, for: .vertical)
        v.setContentHuggingPriority(.defaultLow, for: .horizontal)
        v.setContentHuggingPriority(.defaultLow, for: .vertical)
        return v
    }
    func updateNSView(_ v: NSImageView, context: Context) {
        guard v.toolTip != url?.path else { return }
        v.toolTip = url?.path
        v.image = url.flatMap(KeyedGIF.image(for:))
    }
}

enum KeyedGIF {
    private static var cache: [URL: NSImage] = [:]

    static func image(for url: URL) -> NSImage? {
        if let c = cache[url] { return c }
        guard let src = CGImageSourceCreateWithURL(url as CFURL, nil) else { return nil }
        let n = CGImageSourceGetCount(src)
        let data = NSMutableData()
        guard n > 0, let dst = CGImageDestinationCreateWithData(data, "com.compuserve.gif" as CFString, n, nil) else { return nil }
        CGImageDestinationSetProperties(dst, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFLoopCount: 0]] as CFDictionary)
        for i in 0..<n {
            guard let frame = CGImageSourceCreateImageAtIndex(src, i, nil), let keyed = key(frame) else { continue }
            let props = CGImageSourceCopyPropertiesAtIndex(src, i, nil) as? [CFString: Any]
            let gif = props?[kCGImagePropertyGIFDictionary] as? [CFString: Any]
            let delay = gif?[kCGImagePropertyGIFUnclampedDelayTime] ?? gif?[kCGImagePropertyGIFDelayTime] ?? 0.1
            CGImageDestinationAddImage(dst, keyed, [kCGImagePropertyGIFDictionary: [kCGImagePropertyGIFDelayTime: delay]] as CFDictionary)
        }
        guard CGImageDestinationFinalize(dst), let img = NSImage(data: data as Data) else { return nil }
        cache[url] = img
        return img
    }

    /// Flood-fill the background colour (the top-left pixel's) from the edges to transparent.
    private static func key(_ cg: CGImage) -> CGImage? {
        let w = cg.width, h = cg.height
        guard let ctx = CGContext(data: nil, width: w, height: h, bitsPerComponent: 8, bytesPerRow: w * 4,
                                  space: CGColorSpaceCreateDeviceRGB(),
                                  bitmapInfo: CGImageAlphaInfo.premultipliedLast.rawValue),
              let buf = ctx.data?.bindMemory(to: UInt8.self, capacity: w * h * 4) else { return nil }
        ctx.draw(cg, in: CGRect(x: 0, y: 0, width: w, height: h))
        // the pack's background is whatever the corner holds (black for most, dark grey for clawd)
        let r0 = Int(buf[0]), g0 = Int(buf[1]), b0 = Int(buf[2])
        func dark(_ i: Int) -> Bool { abs(Int(buf[i * 4]) - r0) + abs(Int(buf[i * 4 + 1]) - g0) + abs(Int(buf[i * 4 + 2]) - b0) < 36 }
        var seen = [Bool](repeating: false, count: w * h)
        var stack: [Int] = []
        for x in 0..<w { stack.append(x); stack.append((h - 1) * w + x) }
        for y in 0..<h { stack.append(y * w); stack.append(y * w + w - 1) }
        while let i = stack.popLast() {
            if seen[i] || !dark(i) { continue }
            seen[i] = true
            buf[i * 4] = 0; buf[i * 4 + 1] = 0; buf[i * 4 + 2] = 0; buf[i * 4 + 3] = 0
            let x = i % w, y = i / w
            if x > 0 { stack.append(i - 1) }
            if x < w - 1 { stack.append(i + 1) }
            if y > 0 { stack.append(i - w) }
            if y < h - 1 { stack.append(i + w) }
        }
        return ctx.makeImage()
    }
}
