import Foundation
import Testing
@testable import PetProtocol

@Suite struct ThaiSpeechTests {
    @Test func namesBecomeThai() {
        #expect(ThaiSpeech.render("neo รอคุณอยู่") == "นีโอ รอคุณอยู่")
        #expect(ThaiSpeech.render("Nexus, neo-oracle and Pulse") == "เน็กซัส, นีโอ and พัลส์")
        #expect(ThaiSpeech.render("Hello pet!") == "สวัสดี เพ็ท!")
    }
    @Test func unknownWordsStay() {
        #expect(ThaiSpeech.render("zebra 42") == "zebra 42")
        #expect(ThaiSpeech.render("สวัสดีครับ") == "สวัสดีครับ")
    }
}

@Suite struct LoudTests {
    @Test func samePeakLouderAverage() {
        let pcm: [Int16] = (0..<1600).map { Int16(3000 * sin(Double($0) * 0.39)) + Int16($0 % 400 == 0 ? 29000 : 0) }
        let out = PetSound.loud(pcm)
        let rms = { (a: [Int16]) in (a.map { Double($0) * Double($0) }.reduce(0, +) / Double(a.count)).squareRoot() }
        #expect(out.map { abs(Int32($0)) }.max()! <= 31130)          // 0.95 full scale
        #expect(rms(out) > rms(pcm) * 1.3)
    }
    @Test func silenceStaysSilent() { #expect(PetSound.loud([0, 0, 0]) == [0, 0, 0]) }
    @Test func highPassDropsBassKeepsVoice() {
        let tone = { (hz: Double) in (0..<16000).map { Int16(10000 * sin(2 * Double.pi * hz * Double($0) / 16000)) } }
        let rms = { (a: [Int16]) in (a.suffix(8000).map { Double($0) * Double($0) }.reduce(0, +) / 8000).squareRoot() }
        #expect(rms(PetSound.smallSpeaker(tone(80))) < rms(tone(80)) * 0.1)      // 80 Hz: -20 dB or more
        #expect(rms(PetSound.smallSpeaker(tone(1000))) > rms(tone(1000)) * 0.95)  // speech band kept
        let lift = rms(PetSound.smallSpeaker(tone(3000))) / rms(tone(3000))
        #expect(lift > 1.6 && lift < 1.95)                                        // +5 dB presence at 3 kHz
    }
}
