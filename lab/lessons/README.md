# ESP32 × Display — บทเรียนจากจอจริง 8 ตัว

> ชุดบทเรียนนี้ไม่ได้แปลมาจาก datasheet — ทุกหน้ากลั่นจากงาน bring-up จริงใน repo นี้
> ระหว่าง 2026-04 ถึง 2026-07: commit จริง, บั๊กจริง, และคำว่า "verified fixed"
> ที่ปลอมไป 5 รอบก็เรื่องจริง

## จะเรียนอะไร

| บท | ไฟล์ | ประเภท | ใจความ |
|---|---|---|---|
| 1 | [01-know-your-board.md](01-know-your-board.md) | lesson | รู้จักบอร์ดก่อน flash — บทเรียนราคาแพงจากการ flash ผิดบอร์ด 2 รอบ |
| 2 | [02-first-light.md](02-first-light.md) | tutorial | จุดไฟครั้งแรก — ψ ขึ้นจอใน 60 บรรทัด (ESPHome, JC3248W535) |
| 3 | [03-how-displays-talk.md](03-how-displays-talk.md) | lesson | จอคุยกับชิปยังไง — SPI / QSPI / 8080 / RGB-parallel เทียบด้วยบอร์ดจริง 4 ตัว |
| 4 | [04-native-driver.md](04-native-driver.md) | tutorial | ผ่า driver ที่เขียนเอง — AXS15231 QSPI ตั้งแต่ init ยัน DMA |
| 5 | [05-traps.md](05-traps.md) | lesson | กับดัก 20+ ข้อ พร้อมวิธีแก้ที่ใช้ได้จริง |
| — | [examples/hello-psi.yaml](examples/hello-psi.yaml) | example | config รันได้จริงของบทที่ 2 |

## กองจอของเรา (display census)

| บอร์ด | Panel | Res | Bus | Touch | ใช้ในบท |
|---|---|---|---|---|---|
| Guition **JC3248W535** | AXS15231B | 320×480 | QSPI (DC-less) | AXS15231 I2C 0x3B | 2, 4 — บอร์ดหลักของคอร์ส |
| WT32-SC01 Plus | ST7796UI | 320×480 | 8080 parallel | FT6336U 0x38 | 3 |
| Waveshare 2.8B | ST7701S | 480×640 | RGB (SPI init) | — | 3 |
| Waveshare 7 | ST7262 | 800×480 | RGB 16-bit | GT911 + CH422G | 1, 3 |
| Waveshare 7B | RPI-style | 1024×600 | RGB | GT911 + CH32V003 | 1 |
| M5StickC Plus | built-in | 135×240 | SPI | — | 3 |
| Artronshop ATD35-S3 | LovyanGFX | 320×480 | SPI | capacitive | — |
| MakerAsia C3-HMI | ST7789 | 240×320 | SPI | FT5x06 | 3 |

## เส้นเรื่องย่อ (ประวัติจาก git)

repo นี้เกิด 2026-04-28 เป็น firmware heartbeat เปล่า ๆ ไม่มีจอ (`2f23b8d`)
หนึ่งเดือนถัดมาจอแรกก็ติด — brewing dashboard บน JC3248W535 (`17e1151`)
แล้วสัปดาห์สุดท้ายของเดือนพฤษภาก็กลายเป็นมาราธอน: บอร์ดใหม่ 4 ตัวใน 5 วัน,
driver QSPI เขียนเองจากศูนย์ (`450aa37`), cold-boot trap ที่หลอกเราไป 5 รอบ
(`208005c`, `871b17f`), จนถึง WAMR รัน GIF decoder บนชิปสำเร็จ (`a51c56a`)

รายละเอียดเต็มของแต่ละเหตุการณ์อยู่ใน ψ/memory/ — บทเรียนแต่ละหน้าจะชี้ไฟล์ต้นทางไว้ให้

## ใช้คอร์สนี้ยังไง

- อ่านเรียงบทได้ หรือกระโดดเข้าบทที่ตรงปัญหาตอนนี้ก็ได้ — แต่ละบทจบในตัว
- ทุก example ชี้กลับไปหาโค้ดจริงใน `lab/` ที่เคยรันบนฮาร์ดแวร์แล้ว
- เจอจอไม่ติด ให้เปิดบทที่ 5 ก่อน — โอกาสสูงว่ากับดักที่เจอมีคนใน repo นี้เหยียบมาแล้ว
