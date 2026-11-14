# บทที่ 3 — จอคุยกับชิปยังไง: 4 บัส 4 นิสัย

> บัสแต่ละแบบไม่ใช่แค่ "ต่อสายต่างกัน" — มันกำหนดว่า framebuffer อยู่ที่ไหน
> ใครจ่าย RAM และบั๊กหน้าตาแบบไหนจะมาหา กองจอของเรามีครบทั้ง 4 แบบ
> เลยเทียบกันด้วยของจริงได้ทั้งตาราง

## ภาพรวม

| บัส | บอร์ดของเรา | เส้นสัญญาณ | framebuffer อยู่ที่ | คอขวด |
|---|---|---|---|---|
| SPI | M5StickC Plus, C3-HMI | 1 data + clk | ใน controller (GRAM) | ความเร็ว push |
| QSPI | JC3248W535 | 4 data + clk | ใน controller (GRAM) | framing + ขนาด transaction |
| 8080 parallel | WT32-SC01 Plus | 8 data + WR | ใน controller (GRAM) | แค่หา driver ให้เจอ |
| RGB parallel | Waveshare 2.8B / 7 / 7B | 16+ data + sync | **ใน RAM ของ ESP32** | RAM ภายใน + แบนด์วิดท์ PSRAM |

เส้นแบ่งใหญ่สุดของตารางคือคอลัมน์ framebuffer: สามแบบแรก controller มี
memory ของตัวเอง (GRAM) — ส่งภาพไปแล้วจอถือไว้เอง ชิปว่าง
แบบสุดท้าย (RGB) จอโง่สนิท ไม่มี memory เลย ESP32 ต้อง stream ทุก frame
ตลอดเวลาจาก RAM ตัวเอง — และนี่คือต้นตอของบั๊กตระกูลที่เจ็บสุดในบทที่ 5

## SPI ธรรมดา — จุดเริ่มที่เข้าใจง่ายที่สุด

M5StickC Plus (135×240) คือจอแรกที่ buddy pet เกิด — เฟรมเล็ก push ไวพอ
โดยไม่ต้องคิดอะไร นิสัยของ SPI panel: มี pin DC (Data/Command) แยกบอกว่า
byte ที่วิ่งอยู่คือคำสั่งหรือ pixel ไลบรารีอย่าง LovyanGFX ขับได้แทบทุกตัว
เพราะโมเดล bus ตรงไปตรงมา

## QSPI แบบ DC-less — AXS15231 กับความพิเศษที่กัดเราแรง

JC3248W535 ใช้ AXS15231B — QSPI ที่ **ไม่มี pin DC** — แล้วจอรู้ได้ไง
ว่าอะไรคือคำสั่ง? คำตอบ: บอกกันใน opcode ของ transaction เอง
prefix `0x02` = คำสั่ง, `0x32` = pixel data

ผลคือ LovyanGFX ขับจอนี้ตรง ๆ ไม่ได้เลย เพราะ `Bus_SPI` ทั่วไปไม่รู้จัก
framing แบบนี้ (บันทึกการชนกำแพงอยู่ใน
`ψ/memory/traces/2026-05-29/1325_buddy-jc3248-axs15231-qspi-dbi-not-rendering.md`)
ทางออกที่ใช้จริงคือเขียน driver push layer เอง แล้วเก็บ LovyanGFX ไว้เป็นแค่
ตัววาดลง sprite ใน RAM — ผ่าให้ดูทั้งตัวในบทที่ 4

## 8080 parallel — บัสที่ "ไม่มีใครรองรับ" แต่จริง ๆ รองรับ

WT32-SC01 Plus ใช้ ST7796 ต่อแบบ 8-bit 8080 parallel (data 8 เส้น + WR เป็น clock)
เราเคยฟันธงว่า ESPHome ขับไม่ได้ เพราะไล่ประวัติ PR แล้วเห็น component `i80`
ถูก revert ทิ้งไป — ฟันธงผิด

มุมที่มองข้าม: ทางไฟฟ้า บัส 8080 ก็คือ octal SPI ดี ๆ นี่เอง (WR ทำหน้าที่ clock)
ESPHome เลยขับผ่าน `mipi_spi` ได้ด้วย `spi: {type: octal}` + `model: WT32-SC01-PLUS`
ที่ ship มาในตัว บทเรียนที่ได้: **grep โค้ดที่ติดตั้งอยู่จริงก่อนประกาศว่า
"ทำไม่ได้"** — `esphome config` ที่ validate ผ่าน ชนะการอนุมานจากประวัติ PR ทุกครั้ง

## RGB parallel — จอโง่ที่โยนภาระทั้งหมดมาให้ชิป

Waveshare 2.8B / 7 / 7B ใช้ RGB parallel: data 16 เส้น + HSYNC/VSYNC/DE/PCLK
ชิปต้อง stream pixel สด ๆ ทุก frame เหมือนขับจอ CRT ยุคเก่า สามผลพวงที่ตามมา:

**หนึ่ง — framebuffer กิน RAM มหาศาล** 800×480×2 byte = 750KB ต้องอยู่ใน PSRAM
และโครงสร้างภายในบางส่วนกิน internal DRAM ไปด้วย โค้ดที่เคยรันสบายบนจอ QSPI
เล็ก ๆ พอย้ายมาบอร์ด RGB ก็ OOM ทันทีที่ HTTP fetch แรก — วิธีแก้ไม่ใช่ลด feature
แต่เป็น sdkconfig: `CONFIG_SPIRAM_FETCH_INSTRUCTIONS` + `CONFIG_SPIRAM_RODATA`
ย้ายตัวโค้ดไปรันจาก PSRAM คืน internal DRAM มาให้ heap (fix จริงที่ `75088da`
รายละเอียดในบทที่ 5)

**สอง — pclk ต้องพอดีกับกำลังส่งของ RAM** 2.8B ตอน bring-up เร่ง pclk
16→30MHz ตามค่า vendor demo แล้วภาพซ้อน เพราะ panel scan framebuffer
ทันรอบใหม่ก่อนที่ PSRAM จะป้อนรอบเก่าจบ — vendor ใช้ 30MHz ได้เพราะมี
bounce buffer ซึ่ง driver ฝั่ง ESPHome ไม่มีให้ตั้ง ลอกตัวเลขใครมาต้องลอก
เงื่อนไขรอบตัวเลขมาด้วย

**สาม — init บางตัวเป็นลูกผสม** ST7701S ของ 2.8B รับ init sequence ผ่าน SPI
เส้นแยกก่อน แล้วค่อยสลับไป stream ทาง RGB — สาย SPI นั้น CS ยังไปอยู่หลัง
I2C expander (TCA9554) อีกชั้น ใครจะขับเองต้องรู้ลำดับนี้ ไม่งั้น init ไม่เข้าตั้งแต่ต้น

## แล้วเลือกจอยังไง

- อยากได้ dashboard เร็ว ๆ ไม่แตะ C: จอ GRAM (SPI/QSPI/8080) + ESPHome —
  เบากว่า จอถือภาพเอง ชิปเหลือแรงไปทำงานอื่น
- อยากได้จอใหญ่ 7 นิ้วขึ้นไป: หนีตระกูล RGB ไม่พ้น — วางงบ RAM ตั้งแต่วันแรก
  (PSRAM octal เท่านั้น + sdkconfig ชุดบทที่ 5) และอย่าเลือกบอร์ดจากขนาดจออย่างเดียว
- อยากเข้าใจจอถึงกระดูก: JC3248W535 คือครูที่ดี เพราะมันบังคับให้เขียน
  ทุกชั้นเอง — ตามไปบทที่ 4

---
ถัดไป: [บทที่ 4 — ผ่า driver ที่เขียนเอง](04-native-driver.md)
