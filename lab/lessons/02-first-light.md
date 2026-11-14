# บทที่ 2 — จุดไฟครั้งแรก: ψ ขึ้นจอใน 60 บรรทัด

> เส้นทางที่สั้นที่สุดจากกล่องพัสดุถึง pixel แรก: ESPHome บน JC3248W535
> ไม่ต้องเขียน C แม้แต่บรรทัดเดียว

## ทำไมเริ่มที่ ESPHome

จอตัวเดียวกัน เรามีโค้ดสามชั้นใน repo นี้: ESPHome (declarative yaml),
Arduino + LovyanGFX (`lab/jc3248-pet`), และ ESP-IDF native (`lab/jc3248-pet-idf`)
บทเรียนไต่จากชั้นบนลงล่าง — ESPHome ให้จอติดเร็วที่สุดเพราะ driver ของ
AXS15231 ถูกเขียนไว้ให้แล้วใน component `mipi_spi` เหลือแค่บอก pin ให้ถูก

ส่วนชั้นล่างสุด (เขียน driver เอง) รออยู่ในบทที่ 4 — พอผ่านบทนี้จะเห็นเองว่า
yaml ไม่กี่บรรทัดนี้ซ่อนงานอะไรไว้ข้างใต้บ้าง

## ของที่ต้องมี

- บอร์ด Guition JC3248W535 (ESP32-S3, flash 16MB, PSRAM 8MB octal)
- สาย USB-C — เสียบแล้วควรเห็นพอร์ต `usbmodem` (native USB, VID `303A:1001`
  — ทวนวิธีเช็กได้ในบทที่ 1)
- `esphome` CLI (`pipx install esphome` หรือ `uvx esphome`)

## Config เต็ม

ไฟล์จริงอยู่ที่ [examples/hello-psi.yaml](examples/hello-psi.yaml) — หัวใจคือ 3 block นี้

**หนึ่ง — บัส QSPI** จอนี้ไม่ใช่ SPI ธรรมดา แต่เป็น quad SPI: data 4 เส้น

```yaml
spi:
  - id: lcd_spi
    type: quad
    clk_pin: 47
    data_pins: [21, 48, 40, 39]
```

**สอง — ตัวจอ** ตรงนี้แหละที่ ESPHome ทำงานแทนเราทั้งหมด: vendor init sequence
30+ คำสั่ง, การ frame คำสั่งแบบ DC-less ทั้งหมดอยู่หลัง `model: AXS15231` บรรทัดเดียว

```yaml
display:
  - platform: mipi_spi
    model: AXS15231
    id: lcd
    spi_id: lcd_spi
    cs_pin: 45
    dimensions:
      width: 320
      height: 480
    data_rate: 40MHz
    lambda: |-
      it.fill(Color::BLACK);
      it.print(160, 200, id(noto_big), Color(0xFF, 0xC8, 0x00),
               TextAlign::CENTER, "ψ");
      it.print(160, 280, id(noto_small), Color::WHITE,
               TextAlign::CENTER, "first light");
```

**สาม — backlight** จอนี้ไม่มี pin RST แต่ backlight ต้องสั่งเปิดเอง
ไม่งั้น render สำเร็จแค่ไหนก็เห็นแต่จอดำ

```yaml
output:
  - platform: ledc
    pin: 1
    id: backlight_pwm
    frequency: 25000Hz   # 25kHz — พ้นย่านหูได้ยิน (ดูกับดักเสียงหวีดในบทที่ 5)

light:
  - platform: monochromatic
    id: backlight
    output: backlight_pwm
    restore_mode: ALWAYS_ON
```

## Flash

```sh
cd lab/lessons/examples
cp ../../jc3248w535/secrets.yaml.example secrets.yaml   # เติม wifi ให้เรียบร้อย
esphome run hello-psi.yaml
```

`esphome run` = compile แล้ว flash ต่อเลย รอบแรกกิน 3–5 นาที (build ESP-IDF ทั้งก้อน)
รอบถัดไปเร็วขึ้นมากเพราะ cache

จุดที่คนพลาดบ่อยที่สุดหลังจากนี้: แก้ yaml แล้วสั่ง `esphome upload` เฉย ๆ —
**upload ไม่ compile ให้** มันส่ง firmware.bin ก้อนเก่าขึ้นบอร์ดซ้ำ
แก้แล้วต้อง `esphome run` (หรือ `compile` ก่อน `upload`) เสมอ

## ควรเห็นอะไร

จอสว่าง พื้นดำ ψ สีทองกลางจอ กับคำว่า first light ข้างใต้
ถ้าเห็นแบบนั้นแปลว่าทั้งสาย: USB → toolchain → QSPI bus → vendor init →
framebuffer → backlight ทำงานครบทุกข้อ

| อาการ | ไปดูที่ |
|---|---|
| จอดำสนิท | backlight block หาย หรือ `restore_mode` ไม่ใช่ ALWAYS_ON |
| จอขาว/noise | เช็กว่า `model: AXS15231` และ pin ตรงตารางข้างบนทุกตัว |
| flash ไม่เข้า | ผิดพอร์ต — กลับไปบทที่ 1 หัวข้อสอง USB-C |
| ตัวอักษรไม่ขึ้นแต่จอสว่าง | `glyphs:` ของ font ต้องมี ψ อยู่ในสตริงด้วย |

ข้อสุดท้ายเจอกันเกือบทุกคน: ESPHome สร้าง glyph เฉพาะตัวอักษรที่ประกาศ
ตัวไหนไม่อยู่ใน `glyphs:` ก็หายไปเงียบ ๆ ไม่มี error

## ก่อนไปบทถัดไป

ลองถอดปลั๊กจริง (ไม่ใช่กด reset) แล้วเสียบใหม่ดูว่าจอกลับมาเหมือนเดิมไหม
ฟังดูจุกจิก แต่ความต่างระหว่าง warm reset กับ cold boot คือกับดักที่แพงที่สุด
ใน repo นี้ — เต็ม ๆ อยู่ในบทที่ 5

---
ถัดไป: [บทที่ 3 — จอคุยกับชิปยังไง](03-how-displays-talk.md)
