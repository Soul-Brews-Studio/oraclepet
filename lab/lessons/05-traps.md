# บทที่ 5 — กับดัก: จ่ายครั้งเดียว ใช้ได้ทั้ง repo

> ทุกข้อในหน้านี้มีใบเสร็จ — commit, retrospective, หรือ memory file ที่บันทึก
> ตอนเหยียบจริง เรียงจากตระกูลที่เจ็บบ่อยสุด

## ตระกูล 1 — Reset กับไฟเลี้ยง

### warm reset ≠ cold boot (กับดักที่แพงที่สุดใน repo)

จอ JC3248 ผ่านการทดสอบทุกครั้งหลัง flash แต่ noise เต็มจอทุกครั้งที่ถอดปลั๊กจริง
เพราะ panel อยู่บน rail 3V3 ที่ไม่ดับตอน MCU reset — state เก่าจาก firmware
ก่อนหน้ายังค้างอยู่ init แบบขาด ๆ เลย "ผ่าน" แบบเกาะบุญเก่า

ที่ร้ายกว่า: เครื่องมือวัดของเราเองก็ตาบอดด้านนี้ — serial capture สั่ง DTR/RTS
ตอน connect ซึ่งเป็น warm reset โดยนิยาม ทุก log ที่เก็บจึงพิสูจน์อะไรไม่ได้เลย
คำว่า "verified fixed" ปลอมไป 5 รอบ คนที่จับได้คือมนุษย์ที่เดินไปถอดปลั๊ก

**Fix**: acceptance test ของ peripheral บน rail ถาวรคือถอดปลั๊กจริงเท่านั้น
และให้ init นำด้วย SWRESET เสมอ เพื่อบังคับ warm/cold ให้เดินเส้นเดียวกัน
ตัว fix จริง: vendor init เต็มตาราง ×2 (idempotent) + ลด clock ช่วง init +
หน่วง 500ms รอไฟนิ่ง (`208005c`, `871b17f`)
→ `ψ/memory/learnings/2026-05-30_warm-reset-is-not-cold-boot.md`

### เครื่องมือวัดแชร์จุดบอดกับบั๊ก

ถ้าเส้นทาง reset ของ test ไม่ใช่เส้นทาง reset ของบั๊ก จะได้ false positive
ตลอดกาล — เช็กเสมอว่า instrument รบกวนสิ่งที่วัดหรือเปล่า และเมื่อ log กับ
ตามนุษย์ขัดกัน ให้เชื่ออุปกรณ์จริง

## ตระกูล 2 — ตัวตนของบอร์ด

### flash ผิดบอร์ด เพราะเชื่อชื่อรุ่น

Waveshare 7 กับ 7B ต่างกันทั้ง resolution และ expander (CH422G vs CH32V003)
— เอกสาร vendor เองยังเขียนผิด รายละเอียดเต็มในบทที่ 1 (`e254a4a`)

### ชื่อพอร์ต USB ไม่ใช่ตัวตนบอร์ด

`usbmodemXXXX` shuffle ทุกครั้งที่ reset — ต่อ 2 บอร์ดพร้อมกันแล้ว flash
ผิดตัวไป 20 นาที **Fix**: map ด้วย MAC (`esptool chip_id`) หรือ VID:PID
(`303A:1001` = native, `1A86:xxxx` = CH343) ตั้งแต่ความกำกวมแรก

### Feature ที่ debug อยู่อาจไม่มีบนบอร์ด

2.8B รุ่นที่ซื้อมาไม่มี touch — GT911 `Calibration error` ที่ไล่อยู่คือ
การ debug อากาศ เช็ก SKU ก่อนไล่บั๊ก

## ตระกูล 3 — บัสกับ memory

รายละเอียดเชิงกลไกอยู่บทที่ 4 — สรุปหัวเป็น checklist:

- **QSPI**: หั่น ≤32KB/transaction, `0x2C` เดี่ยวแล้วต่อ `0x3C`, full frame เริ่ม y=0
- **DMA + PSRAM**: ห้ามยื่น pointer PSRAM ให้ DMA ตรง ๆ — bounce ผ่าน
  `MALLOC_CAP_DMA` scratch
- **Byte order**: panel กิน big-endian RGB565 — เจอใน subsystem ไหน
  ให้ audit ทุกตัวที่เขียน framebuffer เดียวกันทันที
- **สีเพี้ยนแบบสลับคู่** (yellow↔magenta): ก่อนแก้ render path ให้เช็กว่า
  test harness ส่ง byte ถูก order หรือเปล่า — ของเราเคย "แก้" ฝั่งที่ถูกอยู่แล้ว
  เพราะ script ทดสอบเองส่ง LSB-first

### RGB panel ใหญ่ + งานเน็ต = OOM ที่มองไม่เห็น

config ที่รันสบายบนจอ QSPI เล็ก ย้ายไป 800×480 RGB แล้ว abort loop ทันที
ที่ HTTP fetch แรก — framebuffer RGB กิน internal DRAM ที่เคยเหลือ
และ panic ก็มองไม่เห็นด้วย (ดูข้อถัดไป) **Fix ที่พิสูจน์บนฮาร์ดแวร์** (`75088da`):

```yaml
esp32:
  framework:
    sdkconfig_options:
      CONFIG_SPIRAM_FETCH_INSTRUCTIONS: y
      CONFIG_SPIRAM_RODATA: y
      CONFIG_ESP32S3_DATA_CACHE_64KB: y
```

ย้ายตัวโค้ดไปรันจาก PSRAM คืน DRAM ให้ heap — แค่ประกาศ `psram:` เฉย ๆ ไม่พอ

### Panic ที่มองไม่เห็น

`logger.hardware_uart: USB_SERIAL_JTAG` แต่เสียบสาย CH343 อยู่ =
backtrace หายเงียบ เห็นแค่ `rst:0xc` ที่หน้าตาเหมือน watchdog
**Fix**: ชี้ `UART0` เมื่อดูผ่าน bridge และถอด address `0x42xxxxxx`
ด้วย `xtensa-esp-elf-addr2line -e firmware.elf`

### pclk ลอกใครมาต้องลอกเงื่อนไขมาด้วย

เร่ง pclk ตาม vendor demo แล้วภาพซ้อน — เขามี bounce buffer เราไม่มี
และตอนนั้นเราแก้สองอย่างพร้อมกัน (color_order + pclk) เลยแยกอาการไม่ออก
**เปลี่ยนทีละตัวแปรเสมอ**

## ตระกูล 4 — นิสัยของ framework

- **`esphome upload` ไม่ compile** — มันส่ง binary เก่า ต้อง `esphome run`
  หรือ `compile` ก่อน จับได้จาก timestamp "compiled on" ใน log บอร์ด
- **AXS15231 ไม่มี landscape** — hardware ไม่มี axis-swap และ LVGL software
  rotation บน `mipi_spi` ได้ภาพเละ เป็น dead end จริง ไม่ใช่บั๊กให้ไล่ —
  อยากได้ landscape เลือกจอ RGB ที่ rotation-aware (`138f799`)
- **LVGL rotation ต้องประกาศตอน compile** — `rotation: 90` ถึงจะจอง
  rotate buffer ให้ toggle runtime ได้ ผ่าน action ของ ESPHome เท่านั้น
- **GT911 หลัง expander อย่าใส่ `reset_pin`** — reset เป็นงานของ expander
  init (CH422G/CH32V003/TCA9554) ให้ component แค่ probe address (0x5D→0x14)
- **Component PR ที่ยังไม่ merge ให้ pin ที่ SHA** — `github://pr#...` +
  refresh คือ build ที่พังได้เองเงียบ ๆ ตอนคน force-push
- **glyph ที่ไม่ประกาศจะหายเงียบ** — ESPHome สร้าง font เฉพาะตัวอักษรใน
  `glyphs:` ภาษาไทยต้อง merge Sarabun ผ่าน `extras:` (สระ/วรรณยุกต์จะไม่ stack
  เพราะไม่มี shaping engine — ยอมรับเป็น compromise ที่รู้ตัว)

## ตระกูล 5 — Performance บนจอ

- **`lv_label_set_text` เดินสตริงทั้งก้อนสองรอบ** — terminal viewer ช้า 1000ms
  ถูกโทษว่า JSON parser (ผิด) ของจริงคือ text layout: ตัดข้อความให้เหลือแค่
  ที่เห็นบนจอก่อนประกอบ แล้วใช้ `lv_label_set_text_static` — 1000→455ms (`e6d0bbe`)
- **render loop บล็อก touch** — full-frame push ~100ms ทำให้ลากนิ้วหน่วง
  ทั้งที่ log บอกว่า handler ยิง **Fix**: หยุดงาน push ระหว่างนิ้วแตะค้าง
  แล้ว verify ประสบการณ์จริงบนเครื่อง ไม่ใช่แค่ logic ยิง
- **widget "หรู" อาจไม่ได้ compile มาด้วยซ้ำ** — เช็ก `lv_conf.h` ที่ใช้จริง
  ก่อนออกแบบรอบ `lv_spangroup`/`lv_table` (build ของ ESPHome ปิดไว้หมด)

## ตระกูล 6 — วิธีคิด (ราคาแพงกว่าทุกข้อข้างบน)

- **Diagnostic flag ต้องเสียงดัง** — `-DAXS_MINIMAL_LOOP=1` ค้างใน
  platformio.ini ทำให้ "flash โปรแกรมจริง" กลายเป็น color loop ตลอดกาล
  debug กันครึ่งวัน: flag ชั่วคราวต้องพิมพ์ banner บอกตัวเองตอน boot หรือลบทิ้งทันที
- **การทดลองที่ทุก case ให้ผลเหมือนกัน = harness พัง ไม่ใช่ข้อสรุป** —
  bisection gallery ที่ทุก app ขึ้น color loop เหมือนกัน เพราะ warmup 10 วินาที
  บังหน้า app จริง ทำ probe ให้ระบุตัวเองได้ก่อน (เลขใหญ่ ๆ กลางจอ) แล้วค่อยเชื่อผล
- **grep โค้ดที่ติดตั้งจริง ก่อนประกาศว่า "ทำไม่ได้"** — เราเคยฟันธงผิดว่า
  ESPHome ขับ 8080 ไม่ได้ จากการอ่านประวัติ PR (บทที่ 3)
- **คำขอเชิงคุณภาพ ต้องผ่านเลขก่อน commit** — "เสียงดังขึ้นอีก" ที่ volume 0.70
  คือ ~870mA บน budget USB 500mA — brown-out ตอนแตะจอพอดี กำลังไฟ ∝ volume²

---

หน้านี้จะยาวขึ้นเรื่อย ๆ — เหยียบกับดักใหม่เมื่อไหร่ เขียนใบเสร็จลง ψ/memory
แล้วสรุปกลับมาที่นี่ กับดักที่จ่ายแล้วไม่ควรมีใครจ่ายซ้ำ
