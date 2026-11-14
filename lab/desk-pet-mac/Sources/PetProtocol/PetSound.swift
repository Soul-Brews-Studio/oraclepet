import Foundation

/// Sound clips for the pet's speaker: IMA ADPCM, 16 kHz mono, in a small "PSND" file the
/// firmware plays from LittleFS (lab/jc3248-pet-idf/main/audio.c play_clip).
///
///     header (16 bytes): "PSND", u8 version = 1, u8 codec = 1 (IMA ADPCM), u16 0,
///                        u32 sample rate, u32 sample count            (little-endian)
///     block (256 bytes): i16 predictor, u8 step index, u8 0, 252 bytes = 504 samples,
///                        low nibble first
///
/// Every block restarts the decoder from its header, so one bad block cannot spoil the
/// rest. The encoder updates its own predictor with exactly the decoder's arithmetic, so
/// the pet hears what this file says, bit for bit.
public enum PetSound {
    public static let sampleRate = 16_000
    public static let blockBytes = 256
    public static let blockSamples = 504

    public static let stepTable: [Int] = [
        7, 8, 9, 10, 11, 12, 13, 14, 16, 17, 19, 21, 23, 25, 28, 31, 34, 37, 41, 45,
        50, 55, 60, 66, 73, 80, 88, 97, 107, 118, 130, 143, 157, 173, 190, 209, 230, 253, 279, 307,
        337, 371, 408, 449, 494, 544, 598, 658, 724, 796, 876, 963, 1060, 1166, 1282, 1411, 1552, 1707, 1878, 2066,
        2272, 2499, 2749, 3024, 3327, 3660, 4026, 4428, 4871, 5358, 5894, 6484, 7132, 7845, 8630, 9493, 10442, 11487, 12635, 13899,
        15289, 16818, 18500, 20350, 22385, 24623, 27086, 29794, 32767,
    ]
    public static let indexTable: [Int] = [-1, -1, -1, -1, 2, 4, 6, 8, -1, -1, -1, -1, 2, 4, 6, 8]

    /// The decoder step, shared by encode and decode.
    @inline(__always)
    static func step(_ nib: Int, _ pred: inout Int, _ index: inout Int) {
        let s = stepTable[index]
        var diff = s >> 3
        if nib & 1 != 0 { diff += s >> 2 }
        if nib & 2 != 0 { diff += s >> 1 }
        if nib & 4 != 0 { diff += s }
        pred += (nib & 8 != 0) ? -diff : diff
        pred = min(32767, max(-32768, pred))
        index = min(88, max(0, index + indexTable[nib]))
    }

    /// 16 kHz mono samples -> a PSND clip.
    public static func clip(_ pcm: [Int16], sampleRate: Int = sampleRate) -> Data {
        var d = Data("PSND".utf8)
        d.append(contentsOf: [1, 1, 0, 0])
        withUnsafeBytes(of: UInt32(sampleRate).littleEndian) { d.append(contentsOf: $0) }
        withUnsafeBytes(of: UInt32(pcm.count).littleEndian) { d.append(contentsOf: $0) }
        var pred = 0, index = 0
        var i = 0
        while i < pcm.count {
            var block = [UInt8](repeating: 0, count: blockBytes)
            let p16 = Int16(clamping: pred)
            block[0] = UInt8(truncatingIfNeeded: p16)
            block[1] = UInt8(truncatingIfNeeded: p16 >> 8)
            block[2] = UInt8(index)
            for k in 0..<blockSamples where i + k < pcm.count {
                var diff = Int(pcm[i + k]) - pred
                var nib = 0
                if diff < 0 { nib = 8; diff = -diff }
                var t = stepTable[index]
                if diff >= t { nib |= 4; diff -= t }
                t >>= 1
                if diff >= t { nib |= 2; diff -= t }
                t >>= 1
                if diff >= t { nib |= 1 }
                step(nib, &pred, &index)
                block[4 + k / 2] |= UInt8(nib << ((k & 1) * 4))
            }
            d.append(contentsOf: block)
            i += blockSamples
        }
        return d
    }

    /// A PSND clip -> samples (what the pet will play), or nil if it is not one.
    public static func decode(_ clip: Data) -> [Int16]? {
        let b = [UInt8](clip)
        guard b.count >= 16, b[0...3].elementsEqual("PSND".utf8), b[4] == 1, b[5] == 1 else { return nil }
        let total = Int(b[12]) | Int(b[13]) << 8 | Int(b[14]) << 16 | Int(b[15]) << 24
        var out: [Int16] = []
        out.reserveCapacity(total)
        var off = 16
        while out.count < total, off + 4 <= b.count {
            var pred = Int(Int16(bitPattern: UInt16(b[off]) | UInt16(b[off + 1]) << 8))
            var index = min(88, Int(b[off + 2]))
            let n = min(blockSamples, total - out.count)
            for k in 0..<n {
                let byte = off + 4 + k / 2
                guard byte < b.count else { return out }
                let nib = Int(b[byte] >> ((k & 1) * 4)) & 0x0F
                step(nib, &pred, &index)
                out.append(Int16(pred))
            }
            off += blockBytes
        }
        return out
    }

    /// Trim quiet ends, scale to `peak` of full scale, cap the length.
    public static func tidy(_ pcm: [Int16], peak: Double = 0.9, maxSeconds: Double = 6) -> [Int16] {
        let quiet: Int32 = 300
        guard let first = pcm.firstIndex(where: { abs(Int32($0)) > quiet }),
              let last = pcm.lastIndex(where: { abs(Int32($0)) > quiet }) else { return [] }
        let pad = sampleRate / 50                                // keep 20 ms around the speech
        var s = Array(pcm[max(0, first - pad)...min(pcm.count - 1, last + pad)])
        if s.count > Int(maxSeconds * Double(sampleRate)) { s = Array(s.prefix(Int(maxSeconds * Double(sampleRate)))) }
        let m = s.map { abs(Int32($0)) }.max() ?? 1
        let g = Double(32767) * peak / Double(max(m, 1))
        return s.map { Int16(clamping: Int((Double($0) * g).rounded())) }
    }

    /// For a small speaker: a 2nd-order high-pass (RBJ biquad, Q 0.707) that removes the bass
    /// it cannot play. Bass only made the pets sound boomy and distorted ("ทุ้มๆ แตกๆ") and used
    /// up the headroom the voice needs.
    /// Then a presence peak (+`presence` dB at 3 kHz, Q 1): the consonants that make speech
    /// crisp on a small speaker ("ไม่ค่อยคม" without it).
    public static func smallSpeaker(_ pcm: [Int16], cutoff: Double = 300, presence: Double = 5,
                                    sampleRate: Int = sampleRate) -> [Int16] {
        let sr = Double(sampleRate)
        // high-pass
        var w = 2 * Double.pi * cutoff / sr, al = sin(w) / (2 * 0.7071), c = cos(w)
        let hp = biquad(b: [(1 + c) / 2, -(1 + c), (1 + c) / 2], a: [1 + al, -2 * c, 1 - al])
        // presence peak
        let A = pow(10, presence / 40)
        w = 2 * Double.pi * 3000 / sr; al = sin(w) / 2; c = cos(w)
        let pk = biquad(b: [1 + al * A, -2 * c, 1 - al * A], a: [1 + al / A, -2 * c, 1 - al / A])
        return pk(hp(pcm.map(Double.init))).map { Int16(clamping: Int($0.rounded())) }
    }

    private static func biquad(b: [Double], a: [Double]) -> ([Double]) -> [Double] {
        let b0 = b[0] / a[0], b1 = b[1] / a[0], b2 = b[2] / a[0], a1 = a[1] / a[0], a2 = a[2] / a[0]
        return { xs in
            var x1 = 0.0, x2 = 0.0, y1 = 0.0, y2 = 0.0
            return xs.map { x in
                let y = b0 * x + b1 * x1 + b2 * x2 - a1 * y1 - a2 * y2
                x2 = x1; x1 = x; y2 = y1; y1 = y
                return y
            }
        }
    }

    /// Louder speech at the same peak: a soft limiter (tanh) that lifts the quiet parts of
    /// the voice. `drive` 1 = unchanged; 1.6 is about +2.5 dB and keeps speech clear (3 made it muddy). Used by
    /// OraclePet's own speech only; not part of the PSND format.
    public static func loud(_ pcm: [Int16], drive: Double = 1.6, peak: Double = 0.9) -> [Int16] {
        guard drive > 1, let m = pcm.map({ abs(Int32($0)) }).max(), m > 0 else { return pcm }
        let norm = tanh(drive)
        return pcm.map { s in
            let x = Double(s) / Double(m)
            return Int16(clamping: Int((tanh(drive * x) / norm * peak * 32767).rounded()))
        }
    }

    /// 16-bit mono WAV (for saving what the pet's mic heard).
    public static func wav(_ pcm: [Int16], sampleRate: Int = sampleRate) -> Data {
        var d = Data("RIFF".utf8)
        func u32(_ v: Int) { withUnsafeBytes(of: UInt32(v).littleEndian) { d.append(contentsOf: $0) } }
        func u16(_ v: Int) { withUnsafeBytes(of: UInt16(v).littleEndian) { d.append(contentsOf: $0) } }
        u32(36 + pcm.count * 2)
        d.append(contentsOf: Array("WAVEfmt ".utf8))
        u32(16); u16(1); u16(1); u32(sampleRate); u32(sampleRate * 2); u16(2); u16(16)
        d.append(contentsOf: Array("data".utf8))
        u32(pcm.count * 2)
        for s in pcm { withUnsafeBytes(of: s.littleEndian) { d.append(contentsOf: $0) } }
        return d
    }

    /// Whether `text` should use the Thai voice.
    public static func isThai(_ text: String) -> Bool {
        text.unicodeScalars.contains { (0x0E00...0x0E7F).contains($0.value) }
    }
}
