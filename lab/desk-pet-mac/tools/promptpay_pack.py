"""Build the "promptpay" animation pack: a static PromptPay QR (no amount) the pet shows on a
double-tap (OraclePet QRTap). Payload byte-identical to dtinth/promptpay-qr.

    python promptpay_pack.py <out-dir>/promptpay promptpay:<phone|tax id|e-wallet>
    python promptpay_pack.py <out-dir>/promptpay mock          # MOCKUP:PROMPTPAY:PHONEBOOTH
Writes qr.gif + manifest.json, and ../preview.png; then decodes the preview and exits 1 unless
it reads back exactly the payload."""
import qrcode, json, os, re, sys
from PIL import Image, ImageDraw

def tlv(tag, value): return f"{tag}{len(value):02d}{value}"

def crc16(s):  # CRC-16/CCITT-FALSE, as EMVCo QR requires
    crc = 0xFFFF
    for b in s.encode():
        crc ^= b << 8
        for _ in range(8): crc = ((crc << 1) ^ 0x1021) & 0xFFFF if crc & 0x8000 else (crc << 1) & 0xFFFF
    return f"{crc:04X}"

def promptpay(target):
    """Static PromptPay (no amount): phone (10 digits), tax/citizen ID (13) or e-wallet (15)."""
    d = re.sub(r"\D", "", target)
    if len(d) == 10:   acct = tlv("01", ("0066" + d[1:]).rjust(13, "0"))
    elif len(d) == 13: acct = tlv("02", d)
    elif len(d) == 15: acct = tlv("03", d)
    else: raise SystemExit(f"PromptPay ID must be 10, 13 or 15 digits, got {len(d)}")
    body = tlv("00", "01") + tlv("01", "11") + tlv("29", tlv("00", "A000000677010111") + acct) \
         + tlv("58", "TH") + tlv("53", "764") + "6304"   # field order as dtinth/promptpay-qr
    return body + crc16(body)

out = sys.argv[1]; payload = sys.argv[2]
if payload == "mock": payload = "MOCKUP:PROMPTPAY:PHONEBOOTH"
if payload.startswith("promptpay:"): payload = promptpay(payload[len("promptpay:"):])
print("payload", payload)
q = qrcode.QRCode(version=None, error_correction=qrcode.constants.ERROR_CORRECT_L, box_size=1, border=2)
q.add_data(payload); q.make(fit=True)
m = q.get_matrix(); n = len(m)
W, H = 106, 133                      # the largest GIF the JC3248 draws at 3x: 318 x 399 in its 320 x 400 area
px = max(1, min(W // n, (H - 14) // n))
img = Image.new("P", (W, H), 0); img.putpalette([255,255,255, 0,0,0] + [0]*762)
d = ImageDraw.Draw(img)
CLOCK = 7                            # the pet's clock covers the top ~20 px = 7 GIF px (petclock.c, scale 2)
label = "MOCKUP" if payload.startswith("MOCKUP") else "PromptPay"
lb = d.textbbox((0, 0), label); lh = lb[3] - lb[1]
block = n*px + 4 + lh                # QR, a 4 px gap, the label: centred as one block below the clock
ox = (W - n*px)//2; oy = CLOCK + (H - CLOCK - block)//2
for y,row in enumerate(m):
    for x,v in enumerate(row):
        if v: d.rectangle([ox+x*px, oy+y*px, ox+x*px+px-1, oy+y*px+px-1], fill=1)
d.text(((W - d.textlength(label))//2, oy + n*px + 4 - lb[1]), label, fill=1)
print("layout: top margin", oy - CLOCK, "px below the clock, bottom margin", H - (oy + block), "px")
os.makedirs(out, exist_ok=True)
img.save(f"{out}/qr.gif"); img.convert("RGB").resize((W*4,H*4), Image.NEAREST).save(f"{out}/../preview.png")
states = {s: "qr.gif" for s in ["sleep","busy","attention","celebrate","dizzy","heart"]}; states["idle"] = ["qr.gif"]
json.dump({"name":"promptpay","colors":{"body":"#000000","bg":"#FFFFFF","text":"#000000","textDim":"#555555","ink":"#000000"},"states":states}, open(f"{out}/manifest.json","w"), indent=2)
print("modules", n, "px/module", px, "->", n*px, "px; on pet", n*px*3, "px")

import cv2
got = cv2.QRCodeDetector().detectAndDecode(cv2.imread(f"{out}/../preview.png"))[0]
if got != payload:
    sys.exit(f"QR check FAILED: decoded {got!r}")
print("QR check ok: decodes to the payload")
