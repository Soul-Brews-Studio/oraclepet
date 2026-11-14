// Prints oraclepet.c's gcb_table: the grapheme-break class of every code point above ASCII,
// read off Swift's own Character breaking (the Mac app's reference), one probe per class.
func one(_ s: [UInt32]) -> Bool {
  var str = ""; for v in s { str.unicodeScalars.append(Unicode.Scalar(v)!) }
  return str.count == 1
}
let a: UInt32 = 0x61, ka: UInt32 = 0x0915, virama: UInt32 = 0x094D, smile: UInt32 = 0x1F600, zwj: UInt32 = 0x200D, acute: UInt32 = 0x301
func hangul(_ v: UInt32) -> Bool { (0x1100...0x11FF).contains(v) || (0xA960...0xA97C).contains(v) || (0xAC00...0xD7A3).contains(v) || (0xD7B0...0xD7C6).contains(v) || (0xD7CB...0xD7FB).contains(v) }
// classes, as in oraclepet.c: 0 other, 1 control, 2 extend (InCB extend), 3 extend only, 4 linker, 5 spacing mark, 6 prepend, 7 pictographic, 8 InCB consonant
func cls(_ v: UInt32) -> Int {
  if (0xD800...0xDFFF).contains(v) || v == 0x0D || v == 0x0A || v == zwj || (0x1F1E6...0x1F1FF).contains(v) || hangul(v) { return 0 }
  if !one([v, acute]) { return 1 }
  if one([a, v]) {
    if !one([smile, v, zwj, smile]) { return 5 }
    if one([ka, v, ka]) { return 4 }
    return one([ka, virama, v, ka]) ? 2 : 3
  }
  if one([v, a]) { return 6 }
  if one([smile, zwj, v]) { return 7 }
  if one([ka, virama, v]) { return 8 }
  return 0
}
var entries: [UInt32] = []
var last = -1
for v in UInt32(0)...0x10FFFF {
  if v < 0x80 { continue }                       // ASCII is handled in code
  let c = cls(v)
  if c != last { entries.append(v << 4 | UInt32(c)); last = c }
}
var out = "static const uint32_t gcb_table[\(entries.count)] = {\n"
for (i, e) in entries.enumerated() {
  if i % 8 == 0 { out += "   " }
  out += " 0x" + String(e, radix: 16, uppercase: false).leftPad(8) + "u,"
  if i % 8 == 7 || i == entries.count - 1 { out += "\n" }
}
out += "};"
print(out)
extension String { func leftPad(_ n: Int) -> String { String(repeating: "0", count: max(0, n - count)) + self } }
