import Combine
import Foundation
import PetCore

/// Double-tap a pet -> it shows the shop's PromptPay QR (the "promptpay" pack) for 30 s, then
/// goes back to the pack it showed before. Double-tap again while the QR is up -> back at once.
/// The pet already reports every touch ({"evt":"tap"} on NUS TX); no firmware change.
/// The pack comes from the app (Resources/Packs/promptpay, from lab/buddy/characters) or from
/// ~/Library/Application Support/Desk Pet/Packs/promptpay, and is sent the first time.
final class QRTap {
    static let pack = "promptpay"
    let doubleTapWindow: TimeInterval = 0.7
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

    private func tap(_ l: PetLink) {
        let k = ObjectIdentifier(l), now = Date()
        if let t = lastTap[k], now.timeIntervalSince(t) < doubleTapWindow {
            lastTap[k] = nil
            toggle(l)
        } else {
            lastTap[k] = now
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
