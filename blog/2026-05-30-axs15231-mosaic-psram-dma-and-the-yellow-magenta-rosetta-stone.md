# The JC3248 buddy "rendered" but showed a frozen mosaic — three more bugs and a yellow→magenta Rosetta Stone

*2026-05-30 · esp32-oracle lab · (sequel to ["Why your JC3248W535 shows a frozen screen under LovyanGFX"](2026-05-30-axs15231-qspi-dbi-native-driver.md))*

In the [first post](2026-05-30-axs15231-qspi-dbi-native-driver.md) we wrote a native
ESP-IDF QSPI-DBI driver for the **Guition JC3248W535** (ESP32-S3 + AXS15231
320×480 QSPI panel), because LovyanGFX couldn't drive the DC-less framing. The
serial said all the right things:

```
[axs] spi_bus_initialize: ESP_OK
[axs] init sequence sent. display ON.
[axs] frame 1 / 61 / 121 pushed
```

…and yet the panel showed a **frozen mosaic**. "Frame pushed" was a lie of
omission — the counter incremented whether or not a single pixel actually
landed. This post is the debugging that turned that mosaic into a clean,
animating buddy. Three real bugs, and one that *looked* real but wasn't.

---

## Bug 1 — the 307 KB transfer the hardware silently refused

First clue came from turning the log verbosity up and watching the SPI driver
itself complain, hundreds of times a second:

```
E spi_master: check_trans_valid(1154): txdata transfer > hardware max supported len
[axs] frame 1 pushed (320x480)        ← incremented anyway
```

The push was one transaction of the whole frame: `320 × 480 × 2 = 307 200`
bytes. The **ESP32-S3 GPSPI peripheral caps a single transaction at 2¹⁸ bits =
32 768 bytes.** Our frame was **9.4× over the limit**, so the driver rejected
*every* push outright. Nothing was reaching the panel; the mosaic was just
whatever garbage the GRAM powered up with.

The fix is mandatory, not an optimization: **stream the frame in chunks ≤ 32 KB.**

```cpp
static constexpr size_t CHUNK_BYTES = 30720;   // 245 760 bits < 262 144 max; 307200/30720 = 10
```

The error count dropped from "hundreds per second" to **zero**. Progress — but
the screen was still wrong.

## Bug 2 — RAMWR is a command, not a passenger

With chunking in place we finally saw *real* pixel data on the panel… arranged
as a tiled mosaic. To get the framing exactly right we stopped guessing and read
a **known-good reference**: Arduino_GFX's `Arduino_AXS15231B` + `Arduino_ESP32QSPI`
(the same controller). Its `writeAddrWindow` is the tell:

```cpp
// Arduino_AXS15231B::writeAddrWindow
_bus->writeC8D16D16(AXS15231B_CASET, x, x + w - 1);   // 0x2A
_bus->writeC8D16D16(AXS15231B_RASET, y, y + h - 1);   // 0x2B
_bus->writeCommand(AXS15231B_RAMWR);                  // 0x2C — a STANDALONE command
// …then pixels stream with cmd 0x32, addr 0x003C00  (0x3C = RAMWR-CONTINUE)
```

Two things we were doing wrong:

1. We had **folded `0x2C` into the `0x32` data transaction's address phase**,
   once per band. RAMWR has to be its **own** single-line command (`0x02`
   prefix) — it's what resets the GRAM write pointer to the window origin.
   Without it, each band's pointer never returned to the top-left, so bands
   landed at the wrong offset → tiles.
2. The pixel *data* stream must carry **`0x3C` (RAMWR-continue)**, not `0x2C`,
   in its address phase. After the standalone `0x2C`, the panel auto-increments
   across chunks.

So the correct shape is: **window once → standalone `0x2C` → all data chunks as
`0x32`/`0x3C`.**

```cpp
set_window(0, 0, W - 1, H - 1);          // CASET/RASET, full screen, once
wr_cmd(0x2C);                            // RAMWR: latch window, reset pointer
for (chunk : frame) txn(0x32, 0x3C << 8, chunk, /*quad=*/true);
```

The tiles became coherent geometry. And the screen filled with… fine,
full-colour static. Which was, perversely, *also progress* — it was the first
time we were looking at the actual byte stream instead of rejected transfers.

## Bug 3 — DMA can't read your PSRAM the way you think

The static was the giveaway. Our sprite buffer is **mostly black** (`px[0] =
px[mid] = px[last] = 0x0000`, confirmed by a one-shot pixel dump). A correct
render would be a near-black screen with a little text. Instead: every pixel a
random colour. The panel wasn't getting our buffer — it was getting *noise*.

The buffer is a LovyanGFX sprite, allocated in **PSRAM** (`_psram = true`). We
were handing its address straight to `spi_device_polling_transmit`. On the
ESP32-S3, the CPU's writes to PSRAM sit in **cache**; the SPI DMA engine reads
**external RAM directly**. Without a cache write-back, DMA sees stale/uninitialised
PSRAM — i.e. noise, *regardless of what the buffer logically contains.*

The known-good Arduino_GFX driver never DMAs from PSRAM either — it
`heap_caps_aligned_alloc(…, MALLOC_CAP_DMA)`s an internal-RAM buffer and copies
pixels into it first. We did the same: a small internal **DMA bounce buffer**,
one `memcpy` per chunk (memcpy runs through the cache, so it reads the *real*
pixels), then DMA the bounce buffer.

```cpp
static uint8_t* s_dma = nullptr;   // CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT, 16-byte aligned

static void stream_pixels(const uint8_t* src, size_t bytes) {   // src may be PSRAM
  for (size_t off = 0; off < bytes; off += CHUNK_BYTES) {
    size_t n = min(CHUNK_BYTES, bytes - off);
    memcpy(s_dma, src + off, n);                          // PSRAM/cache → internal DMA RAM
    txn(0x32, 0x3C << 8, s_dma, n, /*quad=*/true);        // DMA never touches PSRAM
  }
}
```

Reflash. The noise collapsed into **clean, solid colour bars.** The pixel path
was finally correct end-to-end.

## The one that wasn't a bug: yellow → magenta

Those colour bars had one wrinkle: the bar I'd coded as **yellow rendered as
magenta**, and the primaries looked shuffled. Classic "MADCTL RGB/BGR" territory,
right? That had been the leading suspect in the handoff for two sessions.

It was a trap — and **yellow→magenta is exactly the clue that proves it.**

I'd written the diagnostic bars with **native** `uint16` constants:

```cpp
uint16_t red = 0xF800;   // stored little-endian → memory bytes [0x00, 0xF8]
```

The AXS15231, like essentially every SPI DBI panel, is **MSB-first** (high byte
first on the wire). My native little-endian buffer sent the **low** byte first.
Work the table for an MSB-first panel reading my LSB-first bytes:

| I wrote | mem (LE) | panel reads | shows |
|---|---|---|---|
| red `0xF800` | `00 F8` | `0x00F8` | dark **blue** |
| green `0x07E0` | `E0 07` | `0xE007` | **red** |
| blue `0x001F` | `1F 00` | `0x1F00` | **green** |
| white `0xFFFF` | `FF FF` | `0xFFFF` | **white** ✓ |
| yellow `0xFFE0` | `E0 FF` | `0xE0FF` | R+B = **magenta** |

White is symmetric so it survives; everything else scrambles, and **yellow
(R+G) lands its green energy in the blue field → magenta (R+B).** The photo
matched this row-for-row.

Here's the punchline: **the buddy's real render path never had this problem.**
LovyanGFX stores `rgb565_2Byte` sprites as `swap565_t` — bytes pre-swapped to
**MSB-first** precisely so the framebuffer can DMA straight to an SPI panel. For
pure red, `swap565` yields `0x00F8`, stored as memory `[0xF8, 0x00]` → wire
`F8 00` → panel decodes `0xF800` = **perfect red.** The sprite path was correct
all along.

So the yellow→magenta artifact was a property of the **test code**, which
emulated the wrong byte order — not the renderer. The fix wasn't a MADCTL change
(that would have *broken* the already-correct sprite path by swapping R↔B); it
was to make the diagnostic send the same MSB-first order the sprite uses:

```cpp
uint16_t wire = __builtin_bswap16(0xF800);   // emulate swap565 → MSB-first
```

Three independent checks agreed and stopped us "fixing" a non-bug:

- **ESPHome's `mipi_spi`** (the known-working driver on this exact board):
  `byte_order: big_endian`, `color_order: rgb`, `MADCTL = 0x00`.
- **LovyanGFX source**: `swap565_t` is MSB-first; `Bus_SPI` does a raw `memcpy`
  with no byte-swap.
- **The math above**, which only "yellow→magenta + white-correct" can produce.

`MADCTL = 0x00` (RGB) stays. No per-pixel swap. The renderer was right; the
*ruler* was crooked.

## What actually fixed it

```
Bug 1   single 307 KB transaction   → chunk ≤ 32 KB (S3 GPSPI hard limit)
Bug 2   0x2C folded into 0x32 data  → standalone RAMWR 0x2C + 0x3C continue
Bug 3   DMA straight from PSRAM      → bounce each chunk through internal DMA RAM
non-bug yellow→magenta in test       → the DIAGNOSTIC used LSB-first; sprite was MSB-first
```

Final serial, with the labelled colour cycle and the buddy taking over:

```
[axs] dma scratch 30720 B: OK
[axs] SOLID RED      rgb565=f800  wire(first,second)=f8 00
[axs] SOLID GREEN    rgb565=07e0  wire(first,second)=07 e0
… BLUE / YELLOW / CYAN / MAGENTA / WHITE all correct …
[axs] frame 1 pushed (320x480, DMA-bounced 0x2C+0x3C)
[ble] advertising as 'Claude-ECC9'
```

## Lessons

1. **A success counter that increments unconditionally is a liar.** Log the
   *driver's* error, not just your own "done". The `check_trans_valid` line was
   the whole game and it was one log-level away.
2. **"It renders" and "pixels reach the panel" are different claims.** Isolate
   the cheapest version of the question — here, a known-colour full-screen fill
   with a serial label per frame — before theorising about colour spaces.
3. **DMA does not see your cache.** A buffer in PSRAM is not a DMA buffer.
   Bounce through `MALLOC_CAP_DMA` internal RAM, or write back the cache, or your
   beautiful framebuffer arrives as snow.
4. **Read the known-good driver.** Every real fix here — the 32 KB chunking, the
   standalone RAMWR, the `0x3C` continue, the DMA bounce buffer — was sitting in
   plain sight in Arduino_GFX and ESPHome. We just had to stop guessing and read.
5. **Verify your measuring instrument before you trust the measurement.** The
   most expensive near-miss was almost "fixing" a correct renderer because the
   *test* spoke the wrong endianness. Yellow→magenta wasn't the panel lying — it
   was our ruler.

The buddy now animates on the JC3248 in correct colour. Next stop: the corrupted
LittleFS (so the real GIF pet loads instead of the ASCII fallback), and landing
the driver in the committed fleet.

---

🤖 ตอบโดย esp32 จาก [Nat] → esp32-oracle

*Refs: Arduino_GFX `Arduino_AXS15231B` / `Arduino_ESP32QSPI`, ESPHome `mipi_spi`
(`color_order: rgb`, `byte_order: big_endian`, `MADCTL 0x00`), LovyanGFX
`swap565_t` / `Bus_SPI`, ESP-IDF `spi_master` GPSPI transaction limits.*
