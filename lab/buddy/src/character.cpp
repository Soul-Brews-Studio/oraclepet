#include "character.h"
#include "hal.h"
#include <LittleFS.h>
#if !defined(BOARD_LILYGO_EPD47)
#include <AnimatedGIF.h>
#endif
#include <ArduinoJson.h>

extern TFT_eSprite spr;

static const char* STATE_NAMES[] = {
  "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart"
};
static const uint8_t N_STATES = 7;

// Text mode: manifest has "mode":"text", states contain {frames:[...],delay:N}.
// Frames are short strings rendered at text size 2, centered. No GIF pipeline.
struct TextState {
  char     frames[8][20];
  uint8_t  nFrames;
  uint16_t delayMs;
};
static TextState textStates[N_STATES];
static bool      textMode = false;
static uint8_t   textFrame = 0;
static uint32_t  textNext = 0;

static bool    loaded = false;
static Palette pal = { 0xC2A6, 0x0000, 0xFFFF, 0x8410, 0x0000 };
static char    basePath[48];
static const uint8_t MAX_GIFS = 32;
static char    gifPaths[MAX_GIFS][32];
static uint8_t stateStart[N_STATES];
static uint8_t stateCount[N_STATES];
static uint8_t stateRot[N_STATES];
static uint8_t gifTotal = 0;
static uint8_t curState = 0xFF;

#if !defined(BOARD_LILYGO_EPD47)
static AnimatedGIF gif;
#endif
static File        gifFile;
static int         gifX = 0, gifY = 0, gifW = 0, gifH = 0;
// Peek mode pins the GIF bottom to the info-panel top (y=70) so the pet
// sits on the panel edge regardless of canvas height. Home mode centers
// in the upper 140px. No padding assumed in the source art.
static const int   PEEK_TOP = 70;
static bool        peekMode = false;
// Draw target — defaults to the sprite; characterRenderTo() retargets to
// M5.Lcd for the landscape clock (both inherit TFT_eSPI).
#if defined(BOARD_LILYGO_EPD47)
static EpdSprite*   _tgt = &spr;
#elif defined(BOARD_ATD35_S3)
static LovyanGFX*  _tgt = &spr;
#else
static TFT_eSPI*   _tgt = &spr;
#endif
// Peek mode renders at half scale (2:1 nearest-neighbor in gifDrawCb) so
// the whole pet fits the 70px window instead of cropping the top.
// V1: home mode renders at GIF_SCALE (3x nearest-neighbor) so the pet is big and
// readable on the 320x480 panel; peek mode keeps the 2:1 downscale.
static const int GIF_SCALE = 3;
static void gifPlace() {
  int outW = peekMode ? gifW / 2 : gifW * GIF_SCALE;
  int outH = peekMode ? gifH / 2 : gifH * GIF_SCALE;
  gifX = (spr.width() - outW) / 2;
  if (peekMode) {
    gifY = (PEEK_TOP - outH) / 2;
  } else {
    // centre vertically in the area above the bottom HUD (~80px reserved)
    int usableH = spr.height() - 80;
    gifY = (usableH - outH) / 2;
    if (gifY < 0) gifY = 0;
  }
}
static uint32_t    nextFrameAt = 0;
static uint32_t    animPauseUntil = 0;
static uint32_t    variantStartedMs = 0;
static const uint32_t VARIANT_DWELL_MS = 5000;
static const uint32_t ANIM_PAUSE_MS    = 800;
static bool        gifOpen = false;

static uint16_t parseHexColor(const char* s, uint16_t fallback) {
  if (!s) return fallback;
  if (*s == '#') s++;
  uint32_t v = strtoul(s, nullptr, 16);
  return (uint16_t)(((v >> 19) & 0x1F) << 11 | ((v >> 10) & 0x3F) << 5 | ((v >> 3) & 0x1F));
}

#if !defined(BOARD_LILYGO_EPD47)
// --- AnimatedGIF file callbacks (LittleFS) ------------------------------

static void* gifOpenCb(const char* fname, int32_t* pSize) {
  gifFile = LittleFS.open(fname, "r");
  if (!gifFile) return nullptr;
  *pSize = gifFile.size();
  return (void*)&gifFile;
}

static void gifCloseCb(void* handle) {
  File* f = (File*)handle;
  if (f) f->close();
}

static int32_t gifReadCb(GIFFILE* pFile, uint8_t* pBuf, int32_t iLen) {
  File* f = (File*)pFile->fHandle;
  int32_t n = f->read(pBuf, iLen);
  pFile->iPos = f->position();
  return n;
}

static int32_t gifSeekCb(GIFFILE* pFile, int32_t iPosition) {
  File* f = (File*)pFile->fHandle;
  f->seek(iPosition);
  pFile->iPos = (int32_t)f->position();
  return pFile->iPos;
}

// --- Draw callback: one scanline → line buffer → pushImage ------------
// Transparent pixels get the character's bg color so each frame fully
// paints its region — no ghosting from prior frames.

static void gifDrawCb(GIFDRAW* d) {
  uint16_t* pal16 = d->pPalette;
  uint8_t*  src   = d->pPixels;
  uint8_t   t     = d->ucTransparent;
  bool      hasT  = d->ucHasTransparency;
  int       srcY  = d->iY + d->y;
  // GIFs are unoptimized full-frame (gifsicle --unoptimize --lossy) so
  // transparent always means background — no disposal/delta handling.
  // The -O2/-O3 sub-rect + delta-transparency path was tried and reverted:
  // disposal semantics are encoder-dependent and don't compose with the
  // 2:1 peek downscale's sample alignment.
  auto put = [&](int x, int y, uint8_t idx) {
    _tgt->drawPixel(x, y, (hasT && idx == t) ? pal.bg : pal16[idx]);
  };

  if (peekMode) {
    if (srcY & 1) return;
    int y = gifY + (srcY >> 1);
    if (y < 0 || y >= PEEK_TOP) return;
    int x0 = gifX + (d->iX >> 1);
    int w  = d->iWidth >> 1;
    for (int i = 0; i < w; i++) put(x0 + i, y, src[i << 1]);
    return;
  }

  // Home mode: GIF_SCALE upscale via NxN fillRect blocks (nearest-neighbor).
  int w = d->iWidth;
  if (w > 256) w = 256;
  int oy = gifY + srcY * GIF_SCALE;
  if (oy + GIF_SCALE <= 0 || oy >= spr.height()) return;
  for (int i = 0; i < w; i++) {
    uint8_t idx = src[i];
    uint16_t color = (hasT && idx == t) ? pal.bg : pal16[idx];
    int ox = gifX + (d->iX + i) * GIF_SCALE;
    int cx0 = ox < 0 ? 0 : ox;
    int cy0 = oy < 0 ? 0 : oy;
    int cx1 = ox + GIF_SCALE; if (cx1 > spr.width())  cx1 = spr.width();
    int cy1 = oy + GIF_SCALE; if (cy1 > spr.height()) cy1 = spr.height();
    if (cx0 < cx1 && cy0 < cy1) _tgt->fillRect(cx0, cy0, cx1 - cx0, cy1 - cy0, color);
  }
}
#endif // !BOARD_LILYGO_EPD47

// --- Public -------------------------------------------------------------

bool characterInit(const char* name) {
  // Mount LittleFS; format-on-fail so a corrupt/blank partition (the JC3248
  // shipped with "Corrupted dir pair") becomes a clean, mountable FS ready to
  // receive a character package over the bridge. begin() also fails when already
  // mounted (reload) — fall through to the open("/") check in that case.
  if (!LittleFS.begin(/*formatOnFail=*/true)) {
    if (!LittleFS.open("/")) {
      Serial.println("[char] LittleFS mount failed (even after format)");
      return false;
    }
  }

  // No name → scan /characters/ for the first directory present.
  // Makes the boot character whatever you last installed.
  static char scanned[24];
  if (!name) {
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e = d.openNextFile();
      while (e) {
        if (e.isDirectory()) {
          const char* n = strrchr(e.name(), '/');
          strncpy(scanned, n ? n + 1 : e.name(), sizeof(scanned) - 1);
          scanned[sizeof(scanned) - 1] = 0;
          name = scanned;
          break;
        }
        e = d.openNextFile();
      }
      d.close();
    }
    if (!name) { Serial.println("[char] no characters installed"); return false; }
  }

  snprintf(basePath, sizeof(basePath), "/characters/%s", name);
  char mpath[64];
  snprintf(mpath, sizeof(mpath), "%s/manifest.json", basePath);

  File mf = LittleFS.open(mpath, "r");
  if (!mf) {
    Serial.printf("[char] manifest not found: %s\n", mpath);
    return false;
  }

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, mf);
  mf.close();
  if (err) {
    Serial.printf("[char] manifest parse: %s\n", err.c_str());
    return false;
  }

  JsonObject colors = doc["colors"];
  pal.body    = parseHexColor(colors["body"],    pal.body);
  pal.bg      = parseHexColor(colors["bg"],      pal.bg);
  pal.text    = parseHexColor(colors["text"],    pal.text);
  pal.textDim = parseHexColor(colors["textDim"], pal.textDim);
  pal.ink     = parseHexColor(colors["ink"],     pal.ink);

  const char* mode = doc["mode"];
  textMode = (mode && strcmp(mode, "text") == 0);

  JsonObject states = doc["states"];

  if (textMode) {
    for (uint8_t i = 0; i < N_STATES; i++) {
      TextState& ts = textStates[i];
      ts.nFrames = 0;
      ts.delayMs = 200;
      JsonObject st = states[STATE_NAMES[i]];
      if (st.isNull()) continue;
      ts.delayMs = st["delay"] | 200;
      JsonArray fr = st["frames"];
      for (JsonVariant v : fr) {
        if (ts.nFrames >= 8) break;
        const char* s = v.as<const char*>();
        strncpy(ts.frames[ts.nFrames], s ? s : "", 19);
        ts.frames[ts.nFrames][19] = 0;
        ts.nFrames++;
      }
    }
    loaded = true;
    Serial.printf("[char] loaded '%s' (text mode, %d states)\n", name, N_STATES);
    return true;
  }

  gifTotal = 0;
  for (uint8_t i = 0; i < N_STATES; i++) {
    stateStart[i] = gifTotal;
    stateCount[i] = 0;
    stateRot[i]   = 0;
    JsonVariant v = states[STATE_NAMES[i]];
    if (v.is<JsonArray>()) {
      for (JsonVariant e : v.as<JsonArray>()) {
        if (gifTotal >= MAX_GIFS) break;
        const char* fn = e.as<const char*>();
        if (fn) { snprintf(gifPaths[gifTotal], 32, "%s", fn); gifTotal++; stateCount[i]++; }
      }
    } else {
      const char* fn = v.as<const char*>();
      if (fn) { snprintf(gifPaths[gifTotal], 32, "%s", fn); gifTotal++; stateCount[i] = 1; }
    }
  }

#if defined(BOARD_LILYGO_EPD47)
  loaded = false;  // EPD: no GIF support
  return false;
#else
  gif.begin(LITTLE_ENDIAN_PIXELS);
  loaded = true;
#endif
  Serial.printf("[char] loaded '%s' from %s\n", (const char*)doc["name"], basePath);
  return true;
}

bool characterLoaded() { return loaded; }
const Palette& characterPalette() { return pal; }

// One-shot half-scale render to an arbitrary surface (M5.Lcd for the
// landscape clock). Caller owns clearing. Advances frame timing so
// animation runs even when characterTick() is bypassed.
#if defined(BOARD_LILYGO_EPD47)
void characterRenderTo(EpdSprite* tgt, int cx, int cy) { (void)tgt; (void)cx; (void)cy; }
#else
#if defined(BOARD_ATD35_S3)
void characterRenderTo(LovyanGFX* tgt, int cx, int cy) {
#else
void characterRenderTo(TFT_eSPI* tgt, int cx, int cy) {
#endif
  if (!gifOpen) return;
  auto prevT = _tgt; bool prevP = peekMode; int px = gifX, py = gifY;
  _tgt = tgt; peekMode = true;
  gifX = cx - gifW / 4;
  gifY = cy - gifH / 4;
  uint32_t now = millis();
  if (now >= nextFrameAt) {
    int delayMs = 0;
    if (!gif.playFrame(false, &delayMs)) { gif.reset(); gif.playFrame(false, &delayMs); }
    nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
  }
  _tgt = prevT; peekMode = prevP; gifX = px; gifY = py;
}
#endif

void characterSetPeek(bool peek) {
  if (peekMode == peek) return;
  peekMode = peek;
  characterInvalidate();
}

void characterClose() {
#if !defined(BOARD_LILYGO_EPD47)
  if (gifOpen) { gif.close(); gifOpen = false; }
#endif
  loaded = false;
  textMode = false;
  curState = 0xFF;
}

void characterInvalidate() {
  if (!loaded) return;
  if (textMode) {
    spr.fillSprite(pal.bg);
    uint8_t s = curState; curState = 0xFF;
    characterSetState(s);
    return;
  }
#if !defined(BOARD_LILYGO_EPD47)
  if (gifOpen) { gif.close(); gifOpen = false; }
  animPauseUntil = 0;
  uint8_t s = curState; curState = 0xFF;
  characterSetState(s);
#endif
}

void characterSetState(uint8_t s) {
  if (!loaded || s >= N_STATES || s == curState) return;

  if (textMode) {
    curState = s;
    textFrame = 0;
    textNext = 0;
    spr.fillSprite(pal.bg);
    return;
  }

#if defined(BOARD_LILYGO_EPD47)
  curState = s;
  return;
#else
  if (gifOpen) { gif.close(); gifOpen = false; }
  animPauseUntil = 0;
  curState = s;

  if (stateCount[s] == 0) {
    Serial.printf("[char] no gif for state %d\n", s);
    return;
  }

  uint8_t idx = stateStart[s] + stateRot[s];
  char full[80];
  snprintf(full, sizeof(full), "%s/%s", basePath, gifPaths[idx]);
  if (gif.open(full, gifOpenCb, gifCloseCb, gifReadCb, gifSeekCb, gifDrawCb)) {
    gifOpen = true;
    gifW = gif.getCanvasWidth();
    gifH = gif.getCanvasHeight();
    gifPlace();
    spr.fillSprite(pal.bg);   // bias upward, leave room for HUD
    nextFrameAt = 0;
    variantStartedMs = millis();
    Serial.printf("[char] %s: %dx%d @ (%d,%d) heap=%u\n",
      gifPaths[idx], gifW, gifH, gifX, gifY, ESP.getFreeHeap());
  } else {
    Serial.printf("[char] open failed: %s (err %d)\n", full, gif.getLastError());
  }
#endif // !BOARD_LILYGO_EPD47
}

void characterTick() {
  if (!loaded) return;

  if (textMode) {
    TextState& ts = textStates[curState];
    if (ts.nFrames == 0) return;
    uint32_t now = millis();
    if (now < textNext) return;
    textNext = now + ts.delayMs;

    // Clear a band around the text, not the whole sprite — keeps overlays
    // like the approval panel and the HUD untouched.
    int cy = peekMode ? 35 : 60;
    spr.fillRect(0, cy - 14, spr.width(), 28, pal.bg);

    const char* line = ts.frames[textFrame];
    int len = strlen(line);
    int tw = len * 12;                                    // size-2 glyph width
    spr.setTextColor(pal.body, pal.bg);
    spr.setTextSize(2);
    spr.setCursor((spr.width() - tw) / 2, cy - 8);
    spr.print(line);

    textFrame = (textFrame + 1) % ts.nFrames;
    return;
  }

#if defined(BOARD_LILYGO_EPD47)
  return;  // EPD: no GIF playback
#else
  uint32_t now = millis();

  if (!gifOpen) {
    // Between animations in a rotation: hold the last frame, then open
    // the next gif when the pause elapses.
    if (animPauseUntil && now >= animPauseUntil) {
      animPauseUntil = 0;
      uint8_t s = curState; curState = 0xFF;
      characterSetState(s);
    }
    return;
  }
  if (now < nextFrameAt) return;

  int delayMs = 0;
  if (!gif.playFrame(false, &delayMs)) {
    // End of animation. Single-gif states freeze on the last frame instead
    // of reopening — the LittleFS open + GIF header decode is a multi-ms
    // blocking burst, and during sleep state it was looping every ~4s,
    // possibly starving the BT controller. The sprite already holds the
    // last frame; just stop ticking. Multi-gif states (idle rotation)
    // still advance after a brief pause.
    if (stateCount[curState] == 1) {
      gif.close();
      gifOpen = false;
      return;
    }
    // Multi-variant: loop the same GIF until the dwell window elapses, then
    // rotate. Short bufo idles (~0.5s/loop) get ~10 plays instead of one
    // flash + 3s freeze.
    if (now - variantStartedMs < VARIANT_DWELL_MS) {
      gif.reset();
      nextFrameAt = now;
      return;
    }
    gif.close(); gifOpen = false;
    stateRot[curState] = (stateRot[curState] + 1) % stateCount[curState];
    animPauseUntil = now + ANIM_PAUSE_MS;
    return;
  }
  nextFrameAt = now + (delayMs > 0 ? delayMs : 100);
#endif // !BOARD_LILYGO_EPD47
}
