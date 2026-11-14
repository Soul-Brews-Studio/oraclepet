#pragma once
// ─────────────────────────────────────────────────────────────────────────
//  axs15231_qspi — native ESP-IDF QSPI driver for the AXS15231 (JC3248W535).
//
//  WHY: LovyanGFX's Panel_LCD/Panel_AMOLED get the 0x02-prefixed *commands*
//  through (init clears the panel) but never render *pixels* — the AXS15231's
//  DC-less QSPI-DBI memory-write framing isn't produced. This driver speaks it
//  directly on the ESP32-S3 SPI2 peripheral.
//
//  Framing (learned from a known-good QSPI AXS15231 driver, verified on this
//  board): every command = single-line { cmd-phase 0x02, 24-bit addr = cmd<<8,
//  args }. Pixel write = { 0x32, addr = 0x2C<<8, RGB565 data in QUAD (QIO) }.
//  Window via CASET(0x2A)/RASET(0x2B). Structure mirrors the buddy/ArtronShop
//  LGFX HALs (one class, init table, push).
//
//  Drawing still happens in a LovyanGFX sprite (RGB565 in PSRAM); we just push
//  that buffer here. No ESPHome involved.
// ─────────────────────────────────────────────────────────────────────────
#if defined(BOARD_JC3248W535C)

#include <Arduino.h>
#include "driver/spi_master.h"
#include "driver/gpio.h"
#include "esp_heap_caps.h"
#include "esp_system.h"
#include <string.h>

namespace axs {

static constexpr int PIN_SCLK = 47;
static constexpr int PIN_D0   = 21;   // io0 / MOSI
static constexpr int PIN_D1   = 48;
static constexpr int PIN_D2   = 40;
static constexpr int PIN_D3   = 39;
static constexpr int PIN_CS   = 45;
static constexpr int PIN_BL   = 1;    // backlight (LEDC handled by HAL; we just drive high here)
static constexpr int PIN_TINT = 3;    // touch INT (data-ready), active-low NEGEDGE — per vendor pincfg.h
static constexpr int W = 320, H = 480;

static spi_device_handle_t s_spi = nullptr;
static bool s_touch_ok = false;

// Touch data-ready latch. The AXS15231 asserts INT (GPIO3) on a falling edge ONLY
// when there is a real touch event; the vendor BSP reads I2C exclusively from that
// edge and NEVER blind-polls. Blind-polling reads stale/garbage registers between
// touches → phantom taps (which were auto-cycling the brightness). The ISR just
// latches the edge; the I2C read stays on the main thread in read_touch().
static volatile bool s_touch_irq = false;
static void IRAM_ATTR touch_isr() { s_touch_irq = true; }

// The ESP32-S3 GPSPI peripheral caps one transaction at 2^18 bits = 32 KB, so a
// full 320x480x2 = 307 KB frame is streamed in <=32 KB chunks. 30720 B = 245760
// bits < 262144 max, is even (whole RGB565 pixels), divides 307200 evenly (10).
static constexpr size_t CHUNK_BYTES = 30720;

// SPI DMA cannot reliably read the LovyanGFX sprite buffer directly: it lives in
// PSRAM and the CPU's pixel writes sit in cache, so the DMA engine reads stale
// external RAM → full-screen colour noise regardless of buffer content. We copy
// each chunk into this internal-RAM, DMA-capable scratch first (the known-good
// Arduino_GFX QSPI driver does the same). memcpy runs through cache, so it sees
// the real pixels.
static uint8_t* s_dma = nullptr;

// One QSPI transaction: cmd-phase + addr-phase + optional data; quad only for pixels.
static void txn(uint8_t cmd_prefix, uint32_t addr, const uint8_t* data, size_t len, bool quad) {
  spi_transaction_ext_t t = {};
  t.base.flags = SPI_TRANS_VARIABLE_CMD | SPI_TRANS_VARIABLE_ADDR | SPI_TRANS_VARIABLE_DUMMY;
  if (quad) t.base.flags |= SPI_TRANS_MODE_QIO;
  t.command_bits = 8;            // the 0x02 / 0x32 prefix
  t.address_bits = 24;           // cmd<<8 or 0x2C<<8
  t.dummy_bits   = 0;
  t.base.cmd  = cmd_prefix;
  t.base.addr = addr;
  t.base.length = len * 8;
  t.base.tx_buffer = (len && data) ? data : nullptr;
  esp_err_t e = spi_device_polling_transmit(s_spi, (spi_transaction_t*)&t);
  if (e != ESP_OK) {
    static uint32_t ec = 0;
    if ((ec++ % 50) == 0) Serial.printf("[axs] txn ERR: %s\n", esp_err_to_name(e));
  }
}

// AXS15231 register write: 0x02 prefix, command in the 24-bit address phase.
static void wr_cmd(uint8_t cmd, const uint8_t* args = nullptr, size_t n = 0) {
  txn(0x02, (uint32_t)cmd << 8, args, n, /*quad=*/false);
}

static void color_cycle();        // boot colour diagnostic (defined below), called from init()
static void begin_frame();        // open a full-screen GRAM write (defined below)
static void fill_solid(uint16_t); // whole-screen solid fill (defined below), used by init()

// Backlight brightness via LEDC PWM on PIN_BL. 0..255 (0 = off). LEDC is attached
// in init(); call this any time to dim/brighten (used by drag + BLE in main.cpp).
static void set_backlight(uint8_t level) { ledcWrite(PIN_BL, (uint32_t)level); }

// init table: [cmd, nargs, args...] ; nargs 0xFE = delay marker (next byte = ms,
// command sent with no args). Full AXS15231B vendor init (the JC3248W535 factory
// sequence, matching Arduino_GFX axs15231b_init_operations) so the panel comes up
// correctly from a COLD power cycle. The previous minimal init (BB/C1/BB only)
// relied on a fuller init left in the panel by a prior firmware that survived
// warm resets — a replug wiped it → noise. SWRESET first puts the chip in a known
// state (there is no hardware reset GPIO on this board; pin_rst = -1).
static const uint8_t INIT[] = {
  0x01, 0xFE, 200,                                    // SWRESET + 200ms (cold-boot: known state)
  0xBB, 8, 0x00,0x00,0x00,0x00,0x00,0x00,0x5A,0xA5,   // vendor unlock
  0xA0, 17, 0xC0,0x10,0x00,0x02,0x00,0x00,0x04,0x3F,0x20,0x05,0x3F,0x3F,0x00,0x00,0x00,0x00,0x00,
  0xA2, 31, 0x30,0x3C,0x24,0x14,0xD0,0x20,0xFF,0xE0,0x40,0x19,0x80,0x80,0x80,0x20,0xF9,0x10,0x02,0xFF,0xFF,0xF0,0x90,0x01,0x32,0xA0,0x91,0xE0,0x20,0x7F,0xFF,0x00,0x5A,
  0xD0, 30, 0xE0,0x40,0x51,0x24,0x08,0x05,0x10,0x01,0x20,0x15,0xC2,0x42,0x22,0x22,0xAA,0x03,0x10,0x12,0x60,0x14,0x1E,0x51,0x15,0x00,0x8A,0x20,0x00,0x03,0x3A,0x12,
  0xA3, 22, 0xA0,0x06,0xAA,0x00,0x08,0x02,0x0A,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x04,0x00,0x55,0x55,
  0xC1, 30, 0x31,0x04,0x02,0x02,0x71,0x05,0x24,0x55,0x02,0x00,0x41,0x00,0x53,0xFF,0xFF,0xFF,0x4F,0x52,0x00,0x4F,0x52,0x00,0x45,0x3B,0x0B,0x02,0x0D,0x00,0xFF,0x40,
  0xC3, 11, 0x00,0x00,0x00,0x50,0x03,0x00,0x00,0x00,0x01,0x80,0x01,
  0xC4, 29, 0x00,0x24,0x33,0x80,0x00,0xEA,0x64,0x32,0xC8,0x64,0xC8,0x32,0x90,0x90,0x11,0x06,0xDC,0xFA,0x00,0x00,0x80,0xFE,0x10,0x10,0x00,0x0A,0x0A,0x44,0x50,
  0xC5, 23, 0x18,0x00,0x00,0x03,0xFE,0x3A,0x4A,0x20,0x30,0x10,0x88,0xDE,0x0D,0x08,0x0F,0x0F,0x01,0x3A,0x4A,0x20,0x10,0x10,0x00,
  0xC6, 20, 0x05,0x0A,0x05,0x0A,0x00,0xE0,0x2E,0x0B,0x12,0x22,0x12,0x22,0x01,0x03,0x00,0x3F,0x6A,0x18,0xC8,0x22,
  0xC7, 20, 0x50,0x32,0x28,0x00,0xA2,0x80,0x8F,0x00,0x80,0xFF,0x07,0x11,0x9C,0x67,0xFF,0x24,0x0C,0x0D,0x0E,0x0F,
  0xC9, 4, 0x33,0x44,0x44,0x01,
  0xCF, 27, 0x2C,0x1E,0x88,0x58,0x13,0x18,0x56,0x18,0x1E,0x68,0x88,0x00,0x65,0x09,0x22,0xC4,0x0C,0x77,0x22,0x44,0xAA,0x55,0x08,0x08,0x12,0xA0,0x08,
  0xD5, 30, 0x40,0x8E,0x8D,0x01,0x35,0x04,0x92,0x74,0x04,0x92,0x74,0x04,0x08,0x6A,0x04,0x46,0x03,0x03,0x03,0x03,0x82,0x01,0x03,0x00,0xE0,0x51,0xA1,0x00,0x00,0x00,
  0xD6, 30, 0x10,0x32,0x54,0x76,0x98,0xBA,0xDC,0xFE,0x93,0x00,0x01,0x83,0x07,0x07,0x00,0x07,0x07,0x00,0x03,0x03,0x03,0x03,0x03,0x03,0x00,0x84,0x00,0x20,0x01,0x00,
  0xD7, 19, 0x03,0x01,0x0B,0x09,0x0F,0x0D,0x1E,0x1F,0x18,0x1D,0x1F,0x19,0x40,0x8E,0x04,0x00,0x20,0xA0,0x1F,
  0xD8, 12, 0x02,0x00,0x0A,0x08,0x0E,0x0C,0x1E,0x1F,0x18,0x1D,0x1F,0x19,
  0xD9, 12, 0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,
  0xDD, 12, 0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,0x1F,
  0xDF, 8, 0x44,0x73,0x4B,0x69,0x00,0x0A,0x02,0x90,
  0xE0, 17, 0x3B,0x28,0x10,0x16,0x0C,0x06,0x11,0x28,0x5C,0x21,0x0D,0x35,0x13,0x2C,0x33,0x28,0x0D,
  0xE1, 17, 0x37,0x28,0x10,0x16,0x0B,0x06,0x11,0x28,0x5C,0x21,0x0D,0x35,0x14,0x2C,0x33,0x28,0x0F,
  0xE2, 17, 0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D,
  0xE3, 17, 0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x35,0x44,0x32,0x0C,0x14,0x14,0x36,0x32,0x2F,0x0F,
  0xE4, 17, 0x3B,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0D,
  0xE5, 17, 0x37,0x07,0x12,0x18,0x0E,0x0D,0x17,0x39,0x44,0x2E,0x0C,0x14,0x14,0x36,0x3A,0x2F,0x0F,
  0xA4, 16, 0x85,0x85,0x95,0x82,0xAF,0xAA,0xAA,0x80,0x10,0x30,0x40,0x40,0x20,0xFF,0x60,0x30,
  0xA4, 4, 0x85,0x85,0x95,0x85,
  0xBB, 8, 0x00,0x00,0x00,0x00,0x00,0x00,0x00,0x00,    // vendor lock
  0x11, 0xFE, 120,                                    // sleep out, then 120ms
  0x3A, 1, 0x55,                                      // COLMOD RGB565
  0x36, 1, 0x00,                                      // MADCTL = RGB, no flip
  0x20, 0,                                            // INVOFF (invert_colors:false)
  0x29, 0xFE, 100,                                    // display on, then 100ms
  0xFF                                                // end
};

// warmup=true  → full ~10s colour-cycle warm-up (app 000 / cold boot).
// warmup=false → one short non-black settle (~1.5s), NOT a colour loop. Used by
//                apps 001+ so the bisection isn't 10s/flash. The app's own coloured
//                frames (pushed every loop) keep the panel held after the settle.
static bool init(bool warmup = true) {
  // Reset reason tells cold (POWERON, the replug case) from warm (SW/USB). On a
  // cold power-on the panel supplies are still ramping — let them settle before
  // we send the power-config init, or the init corrupts and the panel comes up
  // as noise (warm reset never showed this: it rode the prior firmware's config).
  esp_reset_reason_t rr = esp_reset_reason();
  Serial.printf("[axs] reset reason: %d (%s)\n", (int)rr,
                rr == ESP_RST_POWERON ? "COLD/POWERON" : "warm");
  Serial.println("[axs] init: SPI2 quad bus…");
  spi_bus_config_t bus = {};
  bus.sclk_io_num = PIN_SCLK;
  bus.data0_io_num = PIN_D0;
  bus.data1_io_num = PIN_D1;
  bus.data2_io_num = PIN_D2;
  bus.data3_io_num = PIN_D3;
  bus.max_transfer_sz = W * H * 2 + 64;
  bus.flags = SPICOMMON_BUSFLAG_MASTER | SPICOMMON_BUSFLAG_QUAD;
  esp_err_t e = spi_bus_initialize(SPI2_HOST, &bus, SPI_DMA_CH_AUTO);
  Serial.printf("[axs] spi_bus_initialize: %s\n", esp_err_to_name(e));
  if (e != ESP_OK) return false;

  spi_device_interface_config_t dev = {};
  dev.clock_speed_hz = 6 * 1000 * 1000;    // 6MHz: solid warm-up frames stuck at 10MHz but
                                           // high-entropy (frog) frames intermittently didn't
                                           // latch — a marginal link tolerates uniform data,
                                           // not varied. Drop the clock for headroom on the
                                           // frog's varied pixels (40→20→10→6).
  dev.mode = 0;
  dev.spics_io_num = PIN_CS;
  dev.queue_size = 4;
  dev.flags = SPI_DEVICE_HALFDUPLEX;
  e = spi_bus_add_device(SPI2_HOST, &dev, &s_spi);
  Serial.printf("[axs] spi_bus_add_device: %s\n", esp_err_to_name(e));
  if (e != ESP_OK) return false;

  // Internal-RAM, DMA-capable, 16-byte-aligned scratch (see s_dma comment).
  s_dma = (uint8_t*)heap_caps_aligned_alloc(16, CHUNK_BYTES, MALLOC_CAP_DMA | MALLOC_CAP_8BIT);
  Serial.printf("[axs] dma scratch %u B: %s\n", (unsigned)CHUNK_BYTES, s_dma ? "OK" : "FAIL");
  if (!s_dma) return false;

  // Let the panel's power rails / charge pumps settle before sending init. On a
  // cold power-on this matters; harmless on warm. (The INIT table itself starts
  // with SWRESET + 200ms.)
  delay(500);

  // Run the init table TWICE. On a marginal cold power-on the first pass can be
  // partly corrupted (panel still ramping); the second pass — after the panel has
  // had ~150ms more and SWRESET re-runs — lands cleanly. Idempotent; cheap insurance.
  for (int pass = 0; pass < 2; pass++) {
    const uint8_t* p = INIT;
    int ncmd = 0;
    while (*p != 0xFF) {
      uint8_t cmd = *p++;
      uint8_t n   = *p++;
      if (n == 0xFE) { uint8_t ms = *p++; wr_cmd(cmd); delay(ms); }
      else           { wr_cmd(cmd, p, n); p += n; }
      ncmd++;
    }
    Serial.printf("[axs] init pass %d sent (%d cmds).\n", pass + 1, ncmd);
    if (pass == 0) delay(50);
  }
  Serial.println("[axs] display ON.");

  // 8-bit PWM backlight @ 25kHz. The vendor BSP uses 5kHz, but only ever sets 0%
  // or 100% (DC = silent). We dim to INTERMEDIATE duties, where 5kHz switching makes
  // the LED-driver inductor sing in the audible band ("sound when increase/decrease").
  // 25kHz is above hearing → silent at every brightness. (8-bit caps at ~312kHz, safe.)
  ledcAttach(PIN_BL, 25000, 8);
  set_backlight(255);             // full at boot; main.cpp restores the saved level after init

  // Touch is I2C (independent of the QSPI display). LovyanGFX's touch never
  // inits because we skip Lcd.init(), so own the touch bus here too.
  s_touch_ok = lgfx::i2c::init(0, 4, 8).has_value();   // port 0, SDA=4, SCL=8
  Serial.printf("[axs] touch i2c (0x3B, sda4/scl8): %s\n", s_touch_ok ? "OK" : "FAIL");
  // Latch the touch INT (GPIO3) on its falling edge. read_touch() only reads I2C
  // after an edge → no idle reads → no phantom taps. (idle = high, so PULLUP.)
  pinMode(PIN_TINT, INPUT_PULLUP);
  attachInterrupt(PIN_TINT, touch_isr, FALLING);
  Serial.println("[axs] touch INT (gpio3, negedge) armed");

  // Panel warm-up: right after display-on (0x29) the AXS15231 won't hold content
  // until it's been driven with NON-BLACK frames for a few seconds (a black warm-up
  // left the panel blank and the pet never appeared). color_cycle() below provides
  // this; it is the proven warm-up that renders the pet.

  // Boot warm-up: the AXS15231 will not hold rendered content until it has been
  // driven with NON-BLACK frames for a few seconds after display-on (a black
  // warm-up does NOT work — it must be non-black). On a cold boot this is REQUIRED.
  if (warmup) {
    // Full ~10s colour cycle (app 000 / cold boot). Also a boot animation.
    color_cycle();
  } else {
    // Fast path (warm reflash during bisection): a single short non-black settle
    // instead of the 10s loop. The selected app then keeps pushing its own
    // coloured frames every loop, which sustains the hold.
    fill_solid(__builtin_bswap16(0xFFFF));   // one white frame
    delay(1500);
    Serial.println("[axs] fast warm-up (1.5s settle) — app rendering next");
  }
  return true;
}

// AXS15231 capacitive touch over I2C (same protocol as the Touch_AXS15231 class).
static bool read_touch(int16_t* x, int16_t* y) {
  if (!s_touch_ok) return false;
  // INT-gate (the phantom-tap fix): the AXS asserts INT only on a real touch edge.
  // No edge since the last call → don't even read I2C; the register between touches
  // holds stale garbage that used to pass as phantom taps. This mirrors the vendor
  // BSP, which reads exclusively from the INT ISR and never blind-polls.
  if (!s_touch_irq) return false;
  s_touch_irq = false;
  static constexpr uint8_t CMD[11] = { 0xb5, 0xab, 0xa5, 0x5a, 0, 0, 0, 8 };
  uint8_t d[8] = {};
  if (!lgfx::i2c::transactionWriteRead(0, 0x3B, CMD, sizeof(CMD), d, sizeof(d), 400000).has_value())
    return false;
  // Layout (vendor touch_record_struct_t): d[1] = finger count (1..2),
  // d[2..5] = x_h/x_l/y_h/y_l with the high byte in the low nibble.
  uint8_t fingers = d[1];
  if (fingers < 1 || fingers > 2) return false;
  int16_t rx = ((d[2] & 0x0F) << 8) | d[3];
  int16_t ry = ((d[4] & 0x0F) << 8) | d[5];
  // Coordinate sanity: on-panel and not the 0,0 "no-touch" sentinel.
  if (rx < 0 || rx >= W || ry < 0 || ry >= H || (rx == 0 && ry == 0)) return false;
  *x = rx; *y = ry;
  return true;
}

static void set_window(uint16_t xs, uint16_t ys, uint16_t xe, uint16_t ye) {
  uint8_t cax[4] = { (uint8_t)(xs >> 8), (uint8_t)xs, (uint8_t)(xe >> 8), (uint8_t)xe };
  uint8_t raw[4] = { (uint8_t)(ys >> 8), (uint8_t)ys, (uint8_t)(ye >> 8), (uint8_t)ye };
  wr_cmd(0x2A, cax, 4);   // CASET
  wr_cmd(0x2B, raw, 4);   // RASET
}

static uint32_t s_frames = 0;

// Open the full-screen GRAM write: window once + standalone RAMWR (0x2C). This
// matches the known-good Arduino_GFX AXS15231B path. The standalone 0x2C resets
// the GRAM pointer to the window origin; pixel chunks then continue via 0x3C.
static void begin_frame() {
  set_window(0, 0, W - 1, H - 1);          // full-screen window, once
  wr_cmd(0x2C);                            // RAMWR: latch window, reset pointer
}

// Stream `bytes` of RGB565 pixels (already MSB-first, as LovyanGFX swap565
// stores them) starting from the current GRAM pointer, in <=32 KB QUAD chunks.
// `src` may be in PSRAM (sprite) or internal RAM (test pattern); either way we
// bounce through the internal DMA scratch so the SPI DMA never reads stale PSRAM.
static void stream_pixels(const uint8_t* src, size_t bytes) {
  size_t off = 0;
  while (off < bytes) {
    size_t n = bytes - off;
    if (n > CHUNK_BYTES) n = CHUNK_BYTES;
    memcpy(s_dma, src + off, n);                       // PSRAM/cache → internal DMA RAM
    txn(0x32, (uint32_t)0x3C << 8, s_dma, n, /*quad=*/true);  // RAMWR-continue, quad
    off += n;
  }
}

// Push a sub-region (x,y,w,h) of RGB565 pixels (row-major in buf) to the panel.
// Cheap vs a full frame — used for the live brightness-bar overlay during a drag,
// so it doesn't reintroduce the ~100ms full-frame stall.
static void push_region(uint16_t x, uint16_t y, uint16_t w, uint16_t h, const uint16_t* buf) {
  if (!s_spi || !s_dma || !buf || w == 0 || h == 0) return;
  set_window(x, y, x + w - 1, y + h - 1);
  wr_cmd(0x2C);                                  // RAMWR: latch this window, reset pointer
  stream_pixels((const uint8_t*)buf, (size_t)w * h * 2);
}

// Push a full-frame RGB565 buffer (e.g. a LovyanGFX sprite in PSRAM) to the panel.
static void push(const uint16_t* buf) {
  if (!s_spi || !s_dma || !buf) return;
#ifdef AXS_SOLID_TEST
  // DIAGNOSTIC: ignore the sprite and push SOLID GREEN from this (loop) path.
  // If the screen goes green and stays → the loop push path + solid content work,
  // so the frog's VARIED pixels are what the panel won't latch. If it stays white
  // → the post-init/loop CONTEXT is broken (not content).
  begin_frame();
  { uint16_t* p16 = (uint16_t*)s_dma; for (size_t i = 0; i < CHUNK_BYTES / 2; i++) p16[i] = 0x07E0; }
  { size_t total = (size_t)W * H * 2, off = 0;
    while (off < total) { size_t n = total - off; if (n > CHUNK_BYTES) n = CHUNK_BYTES;
      txn(0x32, (uint32_t)0x3C << 8, s_dma, n, true); off += n; } }
  if ((s_frames++ % 60) == 0) Serial.println("[axs] SOLID-TEST (green, from loop)");
  return;
#endif
  if (s_frames == 0)
    Serial.printf("[axs] px[0]=%04x mid=%04x\n", buf[0], buf[(W * H) / 2]);
  begin_frame();
  stream_pixels((const uint8_t*)buf, (size_t)W * H * 2);
  if ((s_frames++ % 60) == 0)
    Serial.printf("[axs] frame %lu pushed (%dx%d, DMA-bounced 0x2C+0x3C)\n",
                  (unsigned long)s_frames, W, H);
}

// Fill the whole screen with one 16-bit value. `wire` is the EXACT little-endian
// uint16 placed in the DMA buffer; on the wire its low byte goes out first.
static void fill_solid(uint16_t wire) {
  if (!s_spi || !s_dma) return;
  begin_frame();
  uint16_t* p16 = (uint16_t*)s_dma;
  const size_t pxPerChunk = CHUNK_BYTES / 2;
  for (size_t i = 0; i < pxPerChunk; i++) p16[i] = wire;
  size_t total = (size_t)W * H * 2, off = 0;
  while (off < total) {
    size_t n = total - off; if (n > CHUNK_BYTES) n = CHUNK_BYTES;
    txn(0x32, (uint32_t)0x3C << 8, s_dma, n, /*quad=*/true);
    off += n;
  }
}

// Boot colour diagnostic: cycle full-screen solid colours with a serial label
// for each, so screen-colour can be matched to name with zero ambiguity.
//
// IMPORTANT: each colour is byte-SWAPPED to MSB-first before sending — this is
// the SAME byte order LovyanGFX's swap565 sprite buffer uses, so what you see
// here is exactly what the buddy will render. (The earlier colour-bar test sent
// NATIVE little-endian values, i.e. the wrong order, which is why it showed
// blue/red/green/magenta scrambled — the panel is MSB-first.)
static void color_cycle() {
  if (!s_spi || !s_dma) return;
  struct C { const char* name; uint16_t rgb565; };
  static const C cs[] = {
    {"RED",     0xF800},
    {"GREEN",   0x07E0},
    {"BLUE",    0x001F},
    {"YELLOW",  0xFFE0},
    {"CYAN",    0x07FF},
    {"MAGENTA", 0xF81F},
    {"WHITE",   0xFFFF},
  };
  for (auto& c : cs) {
    uint16_t wire = __builtin_bswap16(c.rgb565);   // MSB-first == sprite order
    fill_solid(wire);
    // ~1.5s/colour → ~10s total. This duration is REQUIRED: at 5s the panel
    // didn't hold the pet (settled to white); ~10s is the proven floor before
    // pushes stick. Do not shorten without re-testing on a cold panel.
    delay(1500);
  }
  Serial.println("[axs] warm-up done — buddy rendering next");
}

} // namespace axs

#endif // BOARD_JC3248W535C
