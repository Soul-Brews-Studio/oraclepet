import Combine
import Foundation
import PetCore

/// A tap shows the pet's heart (the firmware does that). A double or triple tap (two presses
/// within 0.6 s) -> the shop's PromptPay QR (the "promptpay" pack) for 30 s, then back to the
/// pack it showed. A tap while the QR is up -> back at once (taps in its first 1.2 s are the
/// rest of the double/triple tap and are ignored).
///
///     state ──tap──▶ heart          state ──double/triple tap──▶ QR (30 s) ──tap or 30 s──▶ state
/// The pet already reports every touch ({"evt":"tap"} on NUS TX); no firmware change.
/// The pack comes from the app (Resources/Packs/promptpay, from lab/buddy/characters) or from
/// ~/Library/Application Support/Desk Pet/Packs/promptpay, and is sent the first time.
final class QRTap {
    static let pack = "promptpay"
    let doubleTap: TimeInterval = 0.6            // two presses closer than this = double tap
    let showFor: TimeInterval = 30

    private var subs = Set<AnyCancellable>()
    private var lastTap: [ObjectIdentifier: Date] = [:]
    private var restore: [ObjectIdentifier: (pack: String, work: DispatchWorkItem)] = [:]

    init(links: [PetLink]) {
        for l in links {
            l.$tapCount.dropFirst().receive(on: RunLoop.main)
                .sink { [weak self, weak l] _ in if let l { self?.tap(l) } }
                .store(in: &subs)
        }
    }

    private var lastEvent: [ObjectIdentifier: Date] = [:]
    private var shownAt: [ObjectIdentifier: Date] = [:]  // when the QR was asked for (triple tap keeps it)
    let samePress: TimeInterval = 0.12                   // drop exact repeats only; the firmware groups a press

    private func tap(_ l: PetLink) {
        let k = ObjectIdentifier(l), now = Date()
        defer { lastEvent[k] = now }
        if let e = lastEvent[k], now.timeIntervalSince(e) < samePress { return }   // same press
        if let shown = shownAt[k], now.timeIntervalSince(shown) < 1.2 { return }   // rest of a double/triple tap
        if l.firmware?.pack == Self.pack {                   // QR up: one tap puts the pet back
            lastTap[k] = nil
            toggle(l)
        } else if let t = lastTap[k], now.timeIntervalSince(t) < doubleTap {
            lastTap[k] = nil                                 // double (or triple) tap
            toggle(l)
        } else {
            lastTap[k] = now                                 // single tap: just the heart
        }
    }

    private func toggle(_ l: PetLink) {
        let k = ObjectIdentifier(l)
        guard l.canQueueUpdate, let fw = l.firmware else { return }      // linked, OTA-capable, idle
        if fw.pack == Self.pack {                                         // QR up: put the pet back
            restore[k]?.work.cancel()
            let back = restore[k]?.pack ?? fw.packs.first { $0 != Self.pack } ?? "siamese"
            restore[k] = nil
            l.selectPack(back)
            return
        }
        let before = fw.pack
        shownAt[k] = Date()
        if fw.packs.contains(Self.pack) {
            l.selectPack(Self.pack)
        } else if let files = Self.files() {
            l.uploadPack(Self.pack, files: files)                        // first time: copy it, then show
        } else {
            return
        }
        let work = DispatchWorkItem { [weak self, weak l] in
            guard let self, let l else { return }
            self.restore[k] = nil
            if l.firmware?.pack == Self.pack, l.canQueueUpdate { l.selectPack(before) }
        }
        restore[k] = (before, work)
        DispatchQueue.main.asyncAfter(deadline: .now() + showFor, execute: work)
    }

    static func files() -> [(name: String, data: Data)]? {
        let support = FileManager.default.urls(for: .applicationSupportDirectory, in: .userDomainMask)[0]
            .appendingPathComponent("Desk Pet/Packs/\(pack)")
        let bundled = Bundle.main.resourceURL?.appendingPathComponent("Packs/\(pack)")
        for dir in [support, bundled].compactMap({ $0 }) {               // a dropped-in (real) QR wins
            guard let names = try? FileManager.default.contentsOfDirectory(atPath: dir.path),
                  names.contains("manifest.json") else { continue }
            let files = names.filter { $0 == "manifest.json" || $0.hasSuffix(".gif") }.sorted()
                .compactMap { n in (try? Data(contentsOf: dir.appendingPathComponent(n))).map { (name: n, data: $0) } }
            if !files.isEmpty { return files }
        }
        return nil
    }
}
