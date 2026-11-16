import AppKit
import Combine
import PetCore
import PetProtocol
import SwiftUI

final class AppDelegate: NSObject, NSApplicationDelegate, NSMenuDelegate {
    private let link = PetLink()
    /// The second pet (e.g. the 2.8in board): same heartbeat, its own saved pet. Created at
    /// launch, after `link` learns to leave it alone.
    private var link2: PetLink!
    private lazy var feed = FleetFeed(link: link)
    private let display = DisplayModel()
    private let launcher = SessionLauncher()
    private let peek = PanePeek()
    private var peekPanel: PeekPanel?
    private var window: NSWindow!
    private var mascot: MascotPanel!
    private var modeItem: NSMenuItem!
    private var statusItem: NSStatusItem!
    private var connectItem: NSMenuItem!
    private var firmwareItem: NSMenuItem!
    private let library = FirmwareLibrary()
    private let packs = PackLibrary()
    private let apps = AppLibrary()
    private var firmwareWindow: NSWindow?
    private var settingsWindow: NSWindow?
    private var bag = Set<AnyCancellable>()

    func applicationDidFinishLaunching(_ notification: Notification) {
        // two pets, two links, never the same board
        link.avoid = { [weak self] in Set([self?.link2?.peripheralID].compactMap { $0 }) }
        link2 = PetLink(savedKey: "petPeripheralID2")
        link2.avoid = { [weak self] in Set([self?.link.peripheralID].compactMap { $0 }) }
        feed.extraLinks = [link2]
        buildMainMenu()
        buildWindow()
        mascot = MascotPanel(root: MascotView(link: link, feed: feed, display: display, launcher: launcher,
                                              onPeek: { [weak self] in self?.showPeek($0) }))
        buildStatusItem()
        display.onChange = { [weak self] in self?.apply($0) }
        apply(display.mode)
        // Refresh the menu bar whenever the link or the heartbeat changes.
        link.objectWillChange.merge(with: feed.objectWillChange)
            .receive(on: DispatchQueue.main)
            .sink { [weak self] in DispatchQueue.main.async { self?.refreshStatusItem() } }
            .store(in: &bag)
        feed.start()
        refreshStatusItem()
        NSApp.activate(ignoringOtherApps: true)
        scheduleSnapshotIfRequested()
    }

    // Release the pet before quitting, so the next host connects at once.
    private var released = false
    func applicationShouldTerminate(_ sender: NSApplication) -> NSApplication.TerminateReply {
        if released || !link.isLinked { return .terminateNow }
        link.disconnect { [weak self] in
            self?.released = true
            NSApp.reply(toApplicationShouldTerminate: true)
        }
        DispatchQueue.main.asyncAfter(deadline: .now() + 2) { NSApp.reply(toApplicationShouldTerminate: true) }
        return .terminateLater
    }

    func applicationShouldHandleReopen(_ sender: NSApplication, hasVisibleWindows flag: Bool) -> Bool {
        apply(display.mode)                          // Dock click brings back whichever mode is on
        return true
    }

    // MARK: Window or mascot

    private func apply(_ mode: DisplayModel.Mode) {
        switch mode {
        case .window:
            mascot.orderOut(nil)
            window.makeKeyAndOrderFront(nil)
            NSApp.activate(ignoringOtherApps: true)
        case .mascot:
            window.orderOut(nil)
            mascot.orderFrontRegardless()
        }
    }

    // MARK: Peek at a waiting agent's terminal

    func showPeek(_ agent: Agent?) {
        guard let target = agent ?? feed.result?.peekable.first else { return }
        peek.show(pane: target.pane, label: target.label)
        if peekPanel == nil {
            let p = PeekPanel(root: PeekView(feed: feed, peek: peek))
            NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: p, queue: .main) { [weak self] _ in
                self?.peek.stop()
            }
            // First time: open beside the cat (left of it if there is room).
            if p.frame.origin == .zero || !NSScreen.screens.contains(where: { $0.visibleFrame.intersects(p.frame) }) {
                let m = mascot.frame
                let x = m.minX - p.frame.width - 8 >= (mascot.screen?.visibleFrame.minX ?? 0) ? m.minX - p.frame.width - 8 : m.maxX + 8
                p.setFrameOrigin(NSPoint(x: x, y: m.minY + 40))
            }
            peekPanel = p
        }
        peekPanel?.orderFrontRegardless()
        peekPanel?.makeKey()
    }

    @objc private func toggleMode() {
        display.mode = display.mode == .window ? .mascot : .window
    }

    // MARK: Window

    private func buildWindow() {
        window = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 360, height: 600),
                          styleMask: [.titled, .closable, .miniaturizable],
                          backing: .buffered, defer: false)
        window.title = "OraclePet"
        window.isReleasedWhenClosed = false
        let hosting = FirstMouseHostingView(rootView: PetView(link: link, feed: feed, display: display,
                                                      onPeek: { [weak self] in self?.showPeek($0) },
                                                      onFirmware: { [weak self] in self?.showFirmware() },
                                                      extra: link2))
        window.contentView = hosting
        window.setContentSize(hosting.fittingSize)
        window.setFrameAutosaveName("DeskPetMainWindow")
        window.center()
    }

    @objc private func showWindow() {
        display.mode = .window
        apply(.window)
    }

    /// `DESKPET_SNAPSHOT=/tmp/x.png open "OraclePet.app"` writes a PNG of the window after
    /// DESKPET_SNAPSHOT_DELAY seconds (default 4): lets an agent check the layout without
    /// screen-recording permission. It draws the real view hierarchy, GIF included.
    private func scheduleSnapshotIfRequested() {
        let env = ProcessInfo.processInfo.environment
        guard let path = env["DESKPET_SNAPSHOT"], !path.isEmpty else { return }
        let delay = Double(env["DESKPET_SNAPSHOT_DELAY"] ?? "") ?? 4
        DispatchQueue.main.asyncAfter(deadline: .now() + delay) { [weak self] in
            guard let self else { return }
            let target = env["DESKPET_SNAPSHOT_VIEW"] == "mascot" ? self.mascot : self.window
            guard let view = target?.contentView,
                  let rep = view.bitmapImageRepForCachingDisplay(in: view.bounds) else { return }
            view.cacheDisplay(in: view.bounds, to: rep)
            try? rep.representation(using: .png, properties: [:])?.write(to: URL(fileURLWithPath: path))
        }
    }

    // MARK: Menu bar

    private func buildStatusItem() {
        statusItem = NSStatusBar.system.statusItem(withLength: NSStatusItem.variableLength)
        let menu = NSMenu()
        menu.delegate = self
        menu.addItem(withTitle: "Show OraclePet", action: #selector(showWindow), keyEquivalent: "").target = self
        modeItem = menu.addItem(withTitle: "Mascot Mode", action: #selector(toggleMode), keyEquivalent: "")
        modeItem.target = self
        connectItem = menu.addItem(withTitle: "Disconnect", action: #selector(toggleConnection), keyEquivalent: "")
        connectItem.target = self
        firmwareItem = menu.addItem(withTitle: "Pet Firmware…", action: #selector(showFirmware), keyEquivalent: "")
        menu.addItem(withTitle: "Settings…", action: #selector(showSettings), keyEquivalent: ",").target = self
        firmwareItem.target = self
        menu.addItem(.separator())
        menu.addItem(withTitle: "Quit OraclePet", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        statusItem.menu = menu
    }

    private func refreshStatusItem() {
        guard let button = statusItem?.button else { return }
        let s = feed.state
        button.image = NSImage(systemSymbolName: PetView.symbol(for: s), accessibilityDescription: s.label)
        button.imagePosition = .imageLeading
        let w = feed.lastHeartbeat?.waiting ?? 0
        button.title = link.isLinked ? (w > 0 ? " \(w)" : "") : " –"
        button.toolTip = "\(s.label) · \(link.state.label)"
        if link.ota.isBusy {                          // the update owns the menu bar while it runs
            button.image = NSImage(systemSymbolName: "arrow.down.circle", accessibilityDescription: "Updating")
            if case .sending(let f) = link.ota { button.title = " \(Int(f * 100))%" } else { button.title = " …" }
            button.toolTip = link.ota.label
        }
    }

    func menuWillOpen(_ menu: NSMenu) {
        connectItem.title = (link.isLinked || link.state == .scanning) ? "Disconnect" : "Connect"
        modeItem.title = display.mode == .window ? "Mascot Mode" : "Window Mode"
        firmwareItem.title = link.ota.isBusy ? "Pet Firmware… (\(link.ota.label))" : "Pet Firmware…"
        firmwareItem.toolTip = link.firmware.map { "Pet runs \($0.version) on \($0.slot), built \($0.built)" }
            ?? "This pet's firmware has no OTA service; flash it once over USB"
    }

    // MARK: Firmware update over BLE (nRF52-DFU style; lab/jc3248-pet-idf/main/ota.h)

    @objc func showSettings() {
        if settingsWindow == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 460, height: 420),
                             styleMask: [.titled, .closable], backing: .buffered, defer: false)
            w.title = "OraclePet Settings"
            w.isReleasedWhenClosed = false
            w.contentView = NSHostingView(rootView: SettingsView(pets: [link, link2].compactMap { $0 }))
            w.center()
            settingsWindow = w
        }
        NSApp.activate(ignoringOtherApps: true)
        settingsWindow?.makeKeyAndOrderFront(nil)
    }

    @objc func showFirmware() {
        if firmwareWindow == nil {
            let w = NSWindow(contentRect: NSRect(x: 0, y: 0, width: 860, height: 720),
                             styleMask: [.titled, .closable, .miniaturizable, .resizable],
                             backing: .buffered, defer: false)
            w.title = "Pet Firmware"
            w.isReleasedWhenClosed = false
            w.contentView = FirstMouseHostingView(rootView: FirmwareView(a: link, b: link2, library: library, packs: packs, apps: apps))
            w.contentMinSize = NSSize(width: 780, height: 560)          // sidebar + page
            w.setFrameAutosaveName("DeskPetFirmwareSplit")
            // the sidebar's passive scan runs only while this window is open
            NotificationCenter.default.addObserver(forName: NSWindow.willCloseNotification, object: w, queue: .main) { _ in
                PetRegistry.shared.setScanning(false)
            }
            w.center()
            firmwareWindow = w
        }
        PetRegistry.shared.setScanning(true)
        library.reload()
        packs.reload()
        apps.reload()
        NSApp.activate(ignoringOtherApps: true)
        firmwareWindow?.makeKeyAndOrderFront(nil)
    }

    private func sendFirmware(_ url: URL) {
        guard let image = try? Data(contentsOf: url) else {
            return NSLog("DeskPet OTA: cannot read \(url.path)")
        }
        NSLog("DeskPet OTA: sending \(url.lastPathComponent), \(image.count) bytes")
        target(for: lastPetParam).updateFirmware(image)
    }

    /// `pet=D350` on a deskpet:// link picks the pet by the end of its name; default: the first.
    private var lastPetParam: String?
    private func target(for pet: String?) -> PetLink {
        guard let pet, !pet.isEmpty else { return link }
        return [link, link2].compactMap { $0 }.first { ($0.petName ?? "").hasSuffix(pet) } ?? link
    }

    /// deskpet://ota?file=/abs/path/jc3248_pet_idf.bin  (what `just ota` opens); deskpet://firmware opens the page
    func application(_ application: NSApplication, open urls: [URL]) { handle(urls, tries: 0) }

    /// A deskpet:// URL that names a pet waits until that pet is linked (right after launch the
    /// links are still connecting, and the URL went to the wrong pet or nowhere). 30 s at most.
    private func handle(_ urls: [URL], tries: Int) {
        let pet = urls.lazy.compactMap { URLComponents(url: $0, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "pet" })?.value }.first
        // deskpet://select?pet=5610 (or a label): open Pet Firmware with that pet chosen in the sidebar
        if let sel = urls.first(where: { $0.scheme == "deskpet" && $0.host == "select" }) {
            let key = URLComponents(url: sel, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "pet" })?.value
            showFirmware()
            if let key { PetRegistry.shared.requestedSelection = key }     // picked up even on first open
            if urls.count == 1 { return }
        }
        // deskpet://update?outdated=1: every outdated pet in the sidebar, one after another (link B)
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "update" }) {
            showFirmware()
            DispatchQueue.main.asyncAfter(deadline: .now() + 0.5) { NotificationCenter.default.post(name: .petUpdateOutdated, object: nil) }
            if urls.count == 1 { return }
        }
        let needsPet = urls.contains { $0.scheme == "deskpet" && !["firmware", "settings", "select", "update"].contains($0.host ?? "") }
        if needsPet, tries < 30, !target(for: pet).isReady || (pet.map { !(target(for: $0).petName ?? "").hasSuffix($0) } ?? false) {
            DispatchQueue.main.asyncAfter(deadline: .now() + 1) { [weak self] in self?.handle(urls, tries: tries + 1) }
            return
        }
        lastPetParam = pet
        let link = target(for: lastPetParam)
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "firmware" }) { showFirmware() }   // deskpet://firmware
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "settings" }) { showSettings() }   // deskpet://settings
        // deskpet://app?file=/abs/x.wasm (`just app`): send the app and run it; deskpet://app?run=name|stop
        for url in urls where url.scheme == "deskpet" && url.host == "app" {
            let q = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            showFirmware()
            if let f = q.first(where: { $0.name == "file" })?.value, let d = try? Data(contentsOf: URL(fileURLWithPath: f)) {
                apps.add(URL(fileURLWithPath: f))
                link.uploadApp(URL(fileURLWithPath: f).deletingPathExtension().lastPathComponent.lowercased(), wasm: d)
            } else if let r = q.first(where: { $0.name == "run" })?.value {
                link.runApp(r == "stop" ? "" : r)
            }
        }
        // deskpet://listen?ms=3000&pet=D350[&lang=en-US|none]: the pet records its mic and sends the
        // audio back; the Mac turns it into text (Thai by default)
        for url in urls where url.scheme == "deskpet" && url.host == "listen" {
            let q = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            let ms = Int(q.first(where: { $0.name == "ms" })?.value ?? "") ?? 3000
            let lang = q.first(where: { $0.name == "lang" })?.value      // th-TH (default), en-US, none
            link.listen(ms: ms, sendAudio: true, locale: lang == "none" ? nil : (lang ?? "th-TH"))
        }
        // deskpet://selftest?pet=D350: the pet beeps; with a mic it reports whether it heard itself
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "selftest" }) { link.testSpeaker() }
        // deskpet://wifi?pet=D350: send the WiFi saved in Settings (Improv); deskpet://voice?on=1&pet=D350
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "wifi" }) { PetWiFi.send(to: link) }
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "wifiscan" }) { link.scanWiFi() }   // deskpet://wifiscan?pet=D350
        for url in urls where url.scheme == "deskpet" && url.host == "voice" {
            let on = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "on" })?.value
            link.setVoice(on != "0")
        }
        // deskpet://replay?pet=D350: the last clip again, no rendering
        if urls.contains(where: { $0.scheme == "deskpet" && $0.host == "replay" }) { link.replay() }
        // deskpet://say?text=hello&pet=D350 (`just say hello`): the pet speaks it
        for url in urls where url.scheme == "deskpet" && url.host == "say" {
            guard let t = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems?.first(where: { $0.name == "text" })?.value else { continue }
            link.say(t)
        }
        // deskpet://pack?name=neo (`just pack neo`): send a built-in pack and show it
        // deskpet://pack?dir=/abs/packs/mycat (lab/pet-studio): copy that pack folder in first, then send it
        for url in urls where url.scheme == "deskpet" && url.host == "pack" {
            let q = URLComponents(url: url, resolvingAgainstBaseURL: false)?.queryItems ?? []
            var name = q.first(where: { $0.name == "name" })?.value
            if let d = q.first(where: { $0.name == "dir" })?.value {
                let src = URL(fileURLWithPath: d)
                packs.add(src)
                name = src.lastPathComponent.lowercased()
            }
            guard let n = name, let p = packs.packs.first(where: { $0.name == n }) else { continue }
            showFirmware()
            link.uploadPack(p.name, files: packs.files(of: p))
        }
        for url in urls where url.scheme == "deskpet" && url.host == "ota" {
            guard let f = URLComponents(url: url, resolvingAgainstBaseURL: false)?
                .queryItems?.first(where: { $0.name == "file" })?.value else { continue }
            showFirmware()
            sendFirmware(URL(fileURLWithPath: f))
        }
    }

    @objc private func toggleConnection() {
        if link.isLinked || link.state == .scanning { link.disconnect() } else { link.connect() }
    }

    // MARK: Main menu (so Cmd-Q and Cmd-W work)

    private func buildMainMenu() {
        let main = NSMenu()
        let appItem = NSMenuItem()
        main.addItem(appItem)
        let appMenu = NSMenu()
        appMenu.addItem(withTitle: "Quit OraclePet", action: #selector(NSApplication.terminate(_:)), keyEquivalent: "q")
        appItem.submenu = appMenu
        let windowItem = NSMenuItem()
        main.addItem(windowItem)
        let windowMenu = NSMenu(title: "Window")
        windowMenu.addItem(withTitle: "Close", action: #selector(NSWindow.performClose(_:)), keyEquivalent: "w")
        windowMenu.addItem(withTitle: "Minimize", action: #selector(NSWindow.performMiniaturize(_:)), keyEquivalent: "m")
        windowItem.submenu = windowMenu
        NSApp.mainMenu = main
    }
}
