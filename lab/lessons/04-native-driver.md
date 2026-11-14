# บทที่ 4 — ผ่า driver ที่เขียนเอง: AXS15231 ตั้งแต่ init ยัน DMA

> บทที่ 2 จอติดด้วย yaml บรรทัดเดียว `model: AXS15231` — บทนี้เปิดฝาดูว่า
> ข้างใต้บรรทัดนั้นมีอะไร โดยไล่จาก driver จริงที่เราเขียนแล้ว rewrite อีกรอบ
> โค้ดทั้งหมดอยู่ที่ `lab/jc3248-pet-idf/main/display.c`

## สถาปัตยกรรม: แบ่งงานที่ตะเข็บ

หลักที่ใช้ทั้ง repo นี้เรียกว่า *port at the right seam* — วาดภาพกับส่งภาพ
เป็นคนละงาน แยกชั้นกันเด็ดขาด:

```
แอป (pet, HUD, GIF)
   │  วาดลง framebuffer ธรรมดาใน PSRAM (s_fb, RGB565 320×480)
   ▼
display_flush()
   │  ยกทั้ง frame ไปให้ panel — ตรงนี้แหละคือ driver
   ▼
AXS15231B บนบัส QSPI
```

ชั้นวาดพกไปบอร์ดไหนก็ได้ ชั้น push เท่านั้นที่ผูกกับฮาร์ดแวร์
ตอน port pet จาก M5Stack มา JC3248 เราเลยเขียนใหม่แค่ชั้นล่าง —
histogram การเรียกใช้ API ก่อน port แล้วจะเห็นว่า 58% ของ M5 API
เป็นของที่บอร์ดใหม่ไม่มี (PMIC, IMU, ปุ่ม) การ "port ทั้งไลบรารี" จึงเป็นโจทย์หลอก

## ปัญหาแรก: จอนี้ไม่มี pin DC

จอ SPI ทั่วไปมีขา DC ไว้ชี้ว่า byte ที่วิ่งคือคำสั่งหรือข้อมูล — AXS15231 ไม่มี
มันใช้ opcode เปิดหัว transaction แทน:

| opcode | ความหมาย |
|---|---|
| `0x02` | สิ่งที่ตามมาคือคำสั่ง (command) |
| `0x32` | สิ่งที่ตามมาคือ pixel data |

driver มือแรก (Arduino, `lab/jc3248-pet`) เราประกอบ transaction เองทุกอัน
พอ rewrite เป็น ESP-IDF พบว่า component `esp_lcd_axs15231b` ของ Espressif
เข้ารหัสแบบเดียวกันเป๊ะ (`cmd<<8 | 0x02<<24` ใน `tx_param`) เลยยืมชั้นนั้นของ
component มาใช้ แล้วเก็บเฉพาะของที่เป็นความรู้เฉพาะบอร์ดเราไว้ — ตะเข็บอีกชั้น

## ปัญหาที่สอง: vendor init table 30+ คำสั่ง

panel ยุคนี้ต้องการชุด register ปลดล็อกจาก vendor ก่อนใช้งาน — ค่าพวก
`I_A0`, `I_C1`, `I_E0` ยาวเป็นหางว่าวใน `display.c` (transcribe มาจาก driver
Arduino ที่พิสูจน์แล้ว) ไม่มีใครรู้ความหมายครบทุก byte และไม่ต้องรู้ —
กติกาคือ **ใช้ตารางเต็ม ห้ามตัดทอน**

ทำไมห้ามตัด: ตอน bring-up แรกเราใช้ init แบบสั้น จอก็ติดดี ผ่านทุกการทดสอบ —
จนกระทั่งถอดปลั๊กจริง จอกลายเป็น noise เต็มจอทุกครั้ง ที่ผ่านมามันติดเพราะ
**อาศัย init ที่ firmware ตัวก่อนหน้าทิ้งไว้ใน panel** (rail 3V3 ของจอไม่ดับ
ตอน MCU reset — state เก่าเลยรอด) นี่คือกับดัก warm-reset-vs-cold-boot
ตัวเต็มอยู่บทที่ 5 แต่ร่องรอยของมันฝังอยู่ในโค้ดนี้สามจุด: ตารางเต็ม,
`SWRESET` นำหน้า, และ `warmup_color_cycle()` ตอน cold boot

## ปัญหาที่สาม: ส่งทั้ง frame ยังไงไม่ให้เละ

frame เต็มคือ 320×480×2 = 307KB แต่ GPSPI ของ ESP32-S3 จำกัด ~32KB
ต่อ transaction — ยัดทีเดียวภาพเละ ต้องหั่นเป็น chunk แล้ว chunk ที่สอง
เป็นต้นไปห้ามส่งคำสั่ง `RAMWR (0x2C)` ซ้ำ ต้องใช้ `RAMWRC (0x3C)` (write
continue) ไม่งั้น pointer ใน GRAM เด้งกลับไปมุมจอทุก chunk — ภาพออกมาเป็นโมเสก

มีเงื่อนไขซ่อนอีกข้อ: component เลือก 0x2C/0x3C จาก `y_start == 0`
full-frame push เลย**ต้องเริ่มที่ y=0 เสมอ** เพื่อให้ 0x2C ตัวแรก
reset row pointer — คอมเมนต์หัวไฟล์ `display.c` เขียนเรื่องนี้ไว้กันลืม

## ปัญหาที่สี่: DMA มองไม่เห็น cache

framebuffer อยู่ใน PSRAM ส่วน DMA อ่าน memory ตรง ๆ ไม่ผ่าน CPU cache —
ยื่น pointer PSRAM ให้ DMA ดื้อ ๆ ก็จะได้ข้อมูลค้าง cache ที่ยังไม่ flush:
ภาพ noise สีรุ้งเต็มจอ วิธีแก้คือ bounce: memcpy ทีละ chunk จาก PSRAM
ลง scratch buffer ที่จองด้วย `MALLOC_CAP_DMA` (internal RAM) แล้วค่อยให้
DMA push จากตรงนั้น — ใน IDF component ชั้นนี้มีให้แล้ว แต่ driver มือแรก
เราชนเองเต็ม ๆ และต้องเข้าใจมันอยู่ดีเวลา debug

## ปัญหาที่ห้า: panel นี้กิน big-endian

RGB565 บน host เป็น little-endian แต่ panel ต้องการ byte สลับ — บั๊กตัวนี้
โผล่ครั้งแรกตอน GIF render (สีเพี้ยนหมด) แก้แล้วก็ประกาศงาน "เสร็จ" —
สองวันถัดมาจุด status บน HUD ที่ควรเขียวกลับแดง เพราะ HUD เขียนสีลง
framebuffer เดียวกันด้วย byte-order เก่า

บทเรียนที่จ่ายไปสองรอบ: quirk ของ platform ที่เจอใน subsystem หนึ่ง
(byte order, timing, cache) คือคำใบ้ให้ **ไล่ audit ทุก producer ของ
resource เดียวกันทันที** อย่ารอให้บั๊กตัวที่สองมาบอกเอง

## เกร็ดปิดท้าย: backlight ก็มีบทเรียน

`display.c` ตั้ง LEDC ที่ 25kHz พร้อมคอมเมนต์ `vendor 5kHz sings` —
ค่า 5kHz ของ vendor ใช้ได้เพราะ firmware โรงงานสั่งแค่ 0% กับ 100%
พอเราหรี่จริง duty กลางย่านทำให้ขดลวดร้องหวีดในย่านหูได้ยิน
ขยับพ้น 20kHz เสียงก็หาย — สเปกที่ vendor ไม่เคยใช้ vendor ก็ไม่เคยเจอบั๊กของมัน

## สรุปเป็นแผนที่

| ชั้น | ใครทำ | ความรู้ที่ฝังอยู่ |
|---|---|---|
| วาด | แอป + gfx ลง `s_fb` (PSRAM) | ไม่รู้จักฮาร์ดแวร์เลย — big-endian เท่านั้นที่ต้องรู้ |
| จัด frame | `display_flush()` | เริ่ม y=0, ทั้ง frame |
| หั่น + bounce | esp_lcd component | ≤32KB/txn, PSRAM→DMA scratch |
| framing | esp_lcd component | 0x02/0x32, 0x2C แล้วต่อด้วย 0x3C |
| ปลุก panel | vendor init table ของเรา | ตารางเต็ม + SWRESET + warmup ตอน cold boot |

อ่านจบแล้วกลับไปมอง `model: AXS15231` ในบทที่ 2 อีกรอบ — บรรทัดเดียวนั้น
คือทั้งตารางนี้ที่มีคนจ่ายค่าเล่าเรียนแทนไปแล้ว

---
ถัดไป: [บทที่ 5 — กับดัก](05-traps.md)
