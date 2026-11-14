#include "hal.h"
#include <LittleFS.h>
#include <stdarg.h>
#if __has_include(<esp_mac.h>)
#include <esp_mac.h>
#else
#include <esp_system.h>
#endif
#if !defined(BOARD_LILYGO_EPD47)
#include "ble_bridge.h"
#endif
#include "data.h"
#if defined(BOARD_ATD35_S3) || defined(BOARD_LILYGO_EPD47)
#include "wifi_bridge.h"
#endif
#include "buddy.h"
#include "ui_layout.h"

#if defined(BOARD_LILYGO_EPD47)
EpdSprite spr;
#elif defined(BOARD_ATD35_S3)
TFT_eSprite spr(&tft);
#else
TFT_eSprite spr = TFT_eSprite(&M5.Lcd);
#endif

// Advertise as "Claude-XXXX" (last two BT MAC bytes) so multiple sticks
// in one room are distinguishable in the desktop picker. Name persists in
// btName for the BLUETOOTH info page.
static char btName[16] = "Claude";
#if !defined(BOARD_LILYGO_EPD47)
static void startBt() {
  uint8_t mac[6] = {0};
  esp_read_mac(mac, ESP_MAC_BT);
  snprintf(btName, sizeof(btName), "Claude-%02X%02X", mac[4], mac[5]);
  bleInit(btName);
}
#endif

#include "character.h"
#include "stats.h"
#if defined(BOARD_LILYGO_EPD47)
const int W = 960, H = 540;
const int LED_PIN = -1;
#elif defined(BOARD_ATD35_S3)
const int W = 320, H = 480;
const int LED_PIN = -1;
#else
const int W = 135, H = 240;
const int LED_PIN = 10;          // red LED, active-low
#endif
const int CX = W / 2;
const int CY_BASE = H / 2;

// Colors used across multiple UI surfaces
const uint16_t HOT   = 0xFA20;   // red-orange: warnings, impatience, deny
const uint16_t PANEL = 0x2104;   // overlay panel background

enum PersonaState { P_SLEEP, P_IDLE, P_BUSY, P_ATTENTION, P_CELEBRATE, P_DIZZY, P_HEART };
const char* stateNames[] = { "sleep", "idle", "busy", "attention", "celebrate", "dizzy", "heart" };

TamaState    tama;
PersonaState baseState   = P_SLEEP;
PersonaState activeState = P_SLEEP;
uint32_t     oneShotUntil = 0;
uint32_t     lastShakeCheck = 0;
float        accelBaseline = 1.0f;
unsigned long t = 0;

// Menu
bool    menuOpen    = false;
uint8_t menuSel     = 0;
uint8_t brightLevel = 4;           // 0..4 → ScreenBreath 20..100
bool    btnALong    = false;

enum DisplayMode { DISP_NORMAL, DISP_PET, DISP_INFO, DISP_COUNT };
uint8_t displayMode = DISP_NORMAL;
uint8_t infoPage = 0;
uint8_t petPage = 0;
const uint8_t PET_PAGES = 2;
uint8_t msgScroll = 0;
uint16_t lastLineGen = 0;
char     lastPromptId[40] = "";
uint32_t lastInteractMs = 0;
bool     dimmed = false;
bool     screenOff = false;
bool     swallowBtnA = false;
bool     swallowBtnB = false;
bool     buddyMode = false;
bool     gifAvailable = false;
const uint8_t SPECIES_GIF = 0xFF;   // species NVS sentinel: use the installed GIF

// Cycle GIF (if installed) → ASCII species 0..N-1 → GIF. Persisted to the
// existing "species" NVS key; 0xFF means GIF mode.
static void nextPet() {
  uint8_t n = buddySpeciesCount();
  if (!buddyMode) {                          // GIF → species 0
    buddyMode = true;
    buddySetSpeciesIdx(0);
    speciesIdxSave(0);
  } else if (buddySpeciesIdx() + 1 >= n && gifAvailable) {  // last species → GIF
    buddyMode = false;
    speciesIdxSave(SPECIES_GIF);
  } else {                                   // species i → species i+1
    buddyNextSpecies();
  }
  characterInvalidate();
  if (buddyMode) buddyInvalidate();
}
uint32_t wakeTransitionUntil = 0;
const uint32_t SCREEN_OFF_MS = 30000;

bool     napping = false;
uint32_t napStartMs = 0;
uint32_t promptArrivedMs = 0;

// Face-down = Z-axis dominant and negative. Debounced so a toss doesn't count.
static bool isFaceDown() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  return az < -0.7f && fabsf(ax) < 0.4f && fabsf(ay) < 0.4f;
}

static void applyBrightness() { M5.Axp.ScreenBreath(20 + brightLevel * 20); }

static void wake() {
  lastInteractMs = millis();
  if (screenOff) {
    M5.Axp.SetLDO2(true);
    applyBrightness();
    screenOff = false;
    wakeTransitionUntil = millis() + 12000;
  }
  if (dimmed) { applyBrightness(); dimmed = false; }
}
bool     responseSent = false;

static void beep(uint16_t freq, uint16_t dur) {
  if (settings().sound) M5.Beep.tone(freq, dur);
}

static void sendCmd(const char* json) {
  Serial.println(json);
  size_t n = strlen(json);
#if !defined(BOARD_LILYGO_EPD47)
  bleWrite((const uint8_t*)json, n);
  bleWrite((const uint8_t*)"\n", 1);
#endif
#if defined(BOARD_ATD35_S3) || defined(BOARD_LILYGO_EPD47)
  if (wifiClientConnected()) {
    wifiWrite((const uint8_t*)json, n);
    wifiWrite((const uint8_t*)"\n", 1);
  }
#endif
}
const uint8_t INFO_PAGES = 6;
const uint8_t INFO_PG_BUTTONS = 1;
const uint8_t INFO_PG_CREDITS = 5;

void applyDisplayMode() {
  bool peek = displayMode != DISP_NORMAL;
  characterSetPeek(peek);
  buddySetPeek(peek);
  // Clear the whole sprite on mode switch. drawInfo/drawPet clear their
  // own regions when they run, but when you switch FROM info/pet TO normal,
  // those functions stop running and their stale pixels stay behind. Full
  // clear is cheap and guarantees no leftovers between modes.
  spr.fillSprite(0x0000);
  characterInvalidate();  // redraws character on next tick (text mode path)
}

const char* menuItems[] = { "settings", "turn off", "help", "about", "demo", "close" };
const uint8_t MENU_N = 6;

bool    settingsOpen = false;
uint8_t settingsSel  = 0;
const char* settingsItems[] = { "brightness", "sound", "bluetooth", "wifi", "led", "transcript", "clock rot", "ascii pet", "reset", "back" };
const uint8_t SETTINGS_N = 10;

bool    resetOpen = false;
uint8_t resetSel  = 0;
const char* resetItems[] = { "delete char", "factory reset", "back" };
const uint8_t RESET_N = 3;
static uint32_t resetConfirmUntil = 0;
static uint8_t  resetConfirmIdx = 0xFF;

static void applySetting(uint8_t idx) {
  Settings& s = settings();
  switch (idx) {
    case 0:
      brightLevel = (brightLevel + 1) % 5;
      applyBrightness();
      return;
    case 1: s.sound = !s.sound; break;
    case 2:
      // BT toggle is a stored preference only — BLE stays live. Turning
      // BLE off cleanly would require tearing down the BLE stack which
      // the Arduino BLE library doesn't do reliably. If we need a
      // hard-off someday, stop advertising via BLEDevice::getAdvertising().
      s.bt = !s.bt;
      break;
    case 3:
      s.wifi = !s.wifi;
#if defined(BOARD_ATD35_S3)
      if (s.wifi) wifiInit();
      else WiFi.disconnect(true);
#endif
      break;
    case 4: s.led = !s.led; break;
    case 5: s.hud = !s.hud; break;
    case 6: s.clockRot = (s.clockRot + 1) % 3; break;
    case 7: nextPet(); return;
    case 8: resetOpen = true; resetSel = 0; resetConfirmIdx = 0xFF; return;
    case 9: settingsOpen = false; characterInvalidate(); return;
  }
  settingsSave();
}

// Tap-twice confirm: first tap arms (label flips to "really?"), second
// within 3s executes. Scrolling away clears the arm.
static void applyReset(uint8_t idx) {
  uint32_t now = millis();
  bool armed = (resetConfirmIdx == idx) && (int32_t)(now - resetConfirmUntil) < 0;

  if (idx == 2) { resetOpen = false; return; }

  if (!armed) {
    resetConfirmIdx = idx;
    resetConfirmUntil = now + 3000;
    beep(1400, 60);
    return;
  }

  beep(800, 200);
  if (idx == 0) {
    // delete char: wipe /characters/, reboot into ASCII mode
    File d = LittleFS.open("/characters");
    if (d && d.isDirectory()) {
      File e;
      while ((e = d.openNextFile())) {
        char path[80];
        snprintf(path, sizeof(path), "/characters/%s", e.name());
        if (e.isDirectory()) {
          File f;
          while ((f = e.openNextFile())) {
            char fp[128];
            snprintf(fp, sizeof(fp), "%s/%s", path, f.name());
            f.close();
            LittleFS.remove(fp);
          }
          e.close();
          LittleFS.rmdir(path);
        } else {
          e.close();
          LittleFS.remove(path);
        }
      }
      d.close();
    }
  } else {
    // factory reset: NVS namespace wipe + filesystem format + BLE bonds.
    // Clears stats, owner, petname, species, settings, GIF characters,
    // and any stored LTKs so the next desktop has to re-pair.
    _prefs.begin("buddy", false);
    _prefs.clear();
    _prefs.end();
    LittleFS.format();
#if !defined(BOARD_LILYGO_EPD47)
    bleClearBonds();
#endif
  }
  delay(300);
  ESP.restart();
}

const int MENU_HINT_H = UI_MENU_HINT_H;
#if defined(BOARD_ATD35_S3) && !defined(BOARD_LILYGO_EPD47)
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "Swipe", const char* rightLbl = "Tap") {
#else
static void drawMenuHints(const Palette& p, int mx, int mw, int hy,
                          const char* downLbl = "A", const char* rightLbl = "B") {
#endif
  spr.setTextSize(UI_FONT_BODY);
  spr.drawFastHLine(mx + UI_MENU_PAD_X, hy - 4, mw - UI_MENU_PAD_X * 2, p.textDim);
  spr.setTextColor(p.textDim, PANEL);
  int x = mx + UI_MENU_PAD_X + 2;
  spr.setCursor(x, hy); spr.print(downLbl);
  x += strlen(downLbl) * UI_GLYPH_W + 4;
  int tsz = UI_GLYPH_H / 2;
  spr.fillTriangle(x, hy + 1, x + tsz, hy + 1, x + tsz/2, hy + tsz, p.textDim);
  x = mx + mw / 2 + 4;
  spr.setCursor(x, hy); spr.print(rightLbl);
  x += strlen(rightLbl) * UI_GLYPH_W + 4;
  spr.fillTriangle(x, hy, x, hy + tsz, x + tsz/2 + 1, hy + tsz/2, p.textDim);
}

// Menu item rects for touch hit-testing
struct MenuRect { int16_t x, y, w, h; };
static MenuRect menuRects[SETTINGS_N];
static uint8_t menuRectCount = 0;

static void drawSettings() {
  const Palette& p = characterPalette();
  int mw = UI_MENU_W, mh = UI_MENU_PAD_Y * 2 + SETTINGS_N * UI_MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, UI_MENU_CORNER, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, UI_MENU_CORNER, p.textDim);
  spr.setTextSize(UI_FONT_BODY);
  Settings& s = settings();
  bool vals[] = { s.sound, s.bt, s.wifi, s.led, s.hud };
  menuRectCount = SETTINGS_N;
  for (int i = 0; i < SETTINGS_N; i++) {
    int iy = my + UI_MENU_PAD_Y + i * UI_MENU_ITEM_H;
    menuRects[i] = { (int16_t)mx, (int16_t)iy, (int16_t)mw, (int16_t)UI_MENU_ITEM_H };
    bool sel = (i == settingsSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + UI_MENU_PAD_X, iy);
    spr.print(sel ? "> " : "  ");
    spr.print(settingsItems[i]);
    spr.setCursor(mx + mw - UI_MENU_VAL_OFF, iy);
    spr.setTextColor(p.textDim, PANEL);
    if (i == 0) {
      spr.printf("%u/4", brightLevel);
    } else if (i >= 1 && i <= 5) {
      spr.setTextColor(vals[i-1] ? GREEN : p.textDim, PANEL);
      spr.print(vals[i-1] ? " on" : "off");
    } else if (i == 6) {
      static const char* const RN[] = { "auto", "port", "land" };
      spr.print(RN[s.clockRot]);
    } else if (i == 7) {
      uint8_t total = buddySpeciesCount() + (gifAvailable ? 1 : 0);
      uint8_t pos   = buddyMode ? buddySpeciesIdx() + 1 : total;
      spr.printf("%u/%u", pos, total);
    }
  }
  drawMenuHints(p, mx, mw, my + mh - MENU_HINT_H + 4, "Next", "Change");
}

static void drawReset() {
  const Palette& p = characterPalette();
  int mw = UI_MENU_W, mh = UI_MENU_PAD_Y * 2 + RESET_N * UI_MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, UI_MENU_CORNER, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, UI_MENU_CORNER, HOT);
  spr.setTextSize(UI_FONT_BODY);
  menuRectCount = RESET_N;
  for (int i = 0; i < RESET_N; i++) {
    int iy = my + UI_MENU_PAD_Y + i * UI_MENU_ITEM_H;
    menuRects[i] = { (int16_t)mx, (int16_t)iy, (int16_t)mw, (int16_t)UI_MENU_ITEM_H };
    bool sel = (i == resetSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + UI_MENU_PAD_X, iy);
    spr.print(sel ? "> " : "  ");
    bool armed = (i == resetConfirmIdx) &&
                 (int32_t)(millis() - resetConfirmUntil) < 0;
    if (armed) spr.setTextColor(HOT, PANEL);
    spr.print(armed ? "really?" : resetItems[i]);
  }
  drawMenuHints(p, mx, mw, my + mh - MENU_HINT_H + 4);
}

void menuConfirm() {
  switch (menuSel) {
    case 0: settingsOpen = true; menuOpen = false; settingsSel = 0; break;
    case 1: M5.Axp.PowerOff(); break;
    case 2:
    case 3:
      menuOpen = false;
      displayMode = DISP_INFO;
      infoPage = (menuSel == 2) ? INFO_PG_BUTTONS : INFO_PG_CREDITS;
      applyDisplayMode();
      characterInvalidate();
      break;
    case 4: dataSetDemo(!dataDemo()); break;
    case 5: menuOpen = false; characterInvalidate(); break;
  }
}

void drawMenu() {
  const Palette& p = characterPalette();
  int mw = UI_MENU_W, mh = UI_MENU_PAD_Y * 2 + MENU_N * UI_MENU_ITEM_H + MENU_HINT_H;
  int mx = (W - mw) / 2, my = (H - mh) / 2;
  spr.fillRoundRect(mx, my, mw, mh, UI_MENU_CORNER, PANEL);
  spr.drawRoundRect(mx, my, mw, mh, UI_MENU_CORNER, p.textDim);
  spr.setTextSize(UI_FONT_BODY);
  menuRectCount = MENU_N;
  for (int i = 0; i < MENU_N; i++) {
    int iy = my + UI_MENU_PAD_Y + i * UI_MENU_ITEM_H;
    menuRects[i] = { (int16_t)mx, (int16_t)iy, (int16_t)mw, (int16_t)UI_MENU_ITEM_H };
    bool sel = (i == menuSel);
    spr.setTextColor(sel ? p.text : p.textDim, PANEL);
    spr.setCursor(mx + UI_MENU_PAD_X, iy);
    spr.print(sel ? "> " : "  ");
    spr.print(menuItems[i]);
    if (i == 4) spr.print(dataDemo() ? "  on" : "  off");
  }
  drawMenuHints(p, mx, mw, my + mh - MENU_HINT_H + 4);
}

// Clock orientation: gravity along the in-plane X axis means the stick is
// on its side. Signed counter for hysteresis on both transitions — same
// pattern as face-down nap.
//   0 = portrait (sprite path, pet sleeps underneath)
//   1 = landscape, BtnA-side down (M5.Lcd rotation 1)
//   3 = landscape, USB-side down (M5.Lcd rotation 3)
static uint8_t clockOrient   = 0;
static int8_t  orientFrames  = 0;
static uint8_t paintedOrient = 0;
// RTC and IMU share an I2C bus. Reading the RTC at 60fps starves the IMU
// reads in clockUpdateOrient — orientation detection gets noisy. Cache the
// time once per second; mood logic and drawClock both read from here.
static RTC_TimeTypeDef _clkTm;
static RTC_DateTypeDef _clkDt;
uint32_t               _clkLastRead = 0;   // zeroed by data.h on time-sync
static bool            _onUsb       = false;
static void clockRefreshRtc() {
  if (millis() - _clkLastRead < 1000) return;
  _clkLastRead = millis();
  _onUsb = M5.Axp.GetVBusVoltage() > 4.0f;
  M5.Rtc.GetTime(&_clkTm);
  M5.Rtc.GetDate(&_clkDt);
}

static void clockUpdateOrient() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  uint8_t lock = settings().clockRot;
  if (lock == 1) { clockOrient = 0; return; }
  if (lock == 2) {
    // Locked landscape: never drop to 0, but still pick 1 vs 3 from
    // gravity so the cradle works either way up. Need a strong tilt
    // for the 1↔3 swap so handling jitter doesn't flip it; otherwise
    // hold whatever we last had (or 1 from boot).
    if (clockOrient == 0) clockOrient = (ax >= 0) ? 1 : 3;
    if      (ax >  0.5f && clockOrient != 1) clockOrient = 1;
    else if (ax < -0.5f && clockOrient != 3) clockOrient = 3;
    return;
  }
  // Dual threshold: strict to enter (must be clearly sideways), loose to
  // stay (tolerate ~65° of tilt). With one shared threshold a slight lean
  // while sitting on the long edge puts ax right at the boundary and the
  // counter ratchets down in ~half a second.
  bool side = (clockOrient == 0)
    ? fabsf(ax) > 0.7f && fabsf(ay) < 0.5f && fabsf(az) < 0.5f
    : fabsf(ax) > 0.4f;
  if (side) { if (orientFrames < 20) orientFrames++; }
  else      { if (orientFrames > -10) orientFrames--; }
  if (clockOrient == 0 && orientFrames >= 15) {
    clockOrient = (ax > 0) ? 1 : 3;
  } else if (clockOrient != 0 && orientFrames <= -8) {
    clockOrient = 0;
  } else if (clockOrient != 0 && side) {
    // Direct 1↔3: a fast flip keeps |ax|>0.7 (just changes sign), so
    // `side` never drops and the exit-via-0 path can't fire. Watch for
    // ax sign disagreeing with the stored orientation.
    static int8_t swapFrames = 0;
    uint8_t want = (ax > 0) ? 1 : 3;
    if (want != clockOrient) { if (++swapFrames >= 8) { clockOrient = want; swapFrames = 0; } }
    else swapFrames = 0;
  }
}

// Clock face: shown when charging on USB with nothing else going on.
// Portrait paints the upper ~110px to the sprite; pet renders below.
// Landscape draws direct to LCD with rotation — sprite stays untouched.
static const char* const MON[] = {
  "Jan","Feb","Mar","Apr","May","Jun","Jul","Aug","Sep","Oct","Nov","Dec"
};
static const char* const DOW[] = {"Sun","Mon","Tue","Wed","Thu","Fri","Sat"};

static uint8_t clockDow() { return _clkDt.WeekDay % 7; }
static void drawClock() {
  const Palette& p = characterPalette();
  char hm[6]; snprintf(hm, sizeof(hm), "%02u:%02u", _clkTm.Hours, _clkTm.Minutes);
  char ss[4]; snprintf(ss, sizeof(ss), ":%02u", _clkTm.Seconds);
  uint8_t mi = (_clkDt.Month >= 1 && _clkDt.Month <= 12) ? _clkDt.Month - 1 : 0;
  char dl[8]; snprintf(dl, sizeof(dl), "%s %02u", MON[mi], _clkDt.Date);

  if (clockOrient == 0) {
    paintedOrient = 0;
    spr.fillRect(0, UI_CLOCK_CLEAR_Y, W, H - UI_CLOCK_CLEAR_Y, p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(UI_CLOCK_TIME_SZ); spr.setTextColor(p.text, p.bg);    spr.drawString(hm, CX, UI_CLOCK_TIME_Y);
    spr.setTextSize(UI_CLOCK_SEC_SZ);  spr.setTextColor(p.textDim, p.bg); spr.drawString(ss, CX, UI_CLOCK_SEC_Y);
    spr.setTextSize(UI_CLOCK_DATE_SZ);                                     spr.drawString(dl, CX, UI_CLOCK_DATE_Y);
    spr.setTextDatum(TL_DATUM);
    return;
  }

#if !defined(BOARD_LILYGO_EPD47)
  // Landscape: 240×135 direct-to-LCD. Full fill only on entry; after that
  // text glyph bg cells repaint themselves and the pet box (small, ~90×50)
  // gets a fillRect each pet tick — small enough not to tear.
  M5.Lcd.setRotation(clockOrient);
  static uint8_t lastSec = 0xFF;
  bool repaint = paintedOrient != clockOrient;
  if (repaint) { M5.Lcd.fillScreen(p.bg); paintedOrient = clockOrient; lastSec = 0xFF; }

  // Seconds tick at 1Hz; redrawing 3 strings at 60fps is 180 SPI ops/sec
  // for nothing. Gate on the second changing (or full repaint).
  if (repaint || _clkTm.Seconds != lastSec) {
    lastSec = _clkTm.Seconds;
    char wdl[12]; snprintf(wdl, sizeof(wdl), "%s %s %02u", DOW[clockDow()], MON[mi], _clkDt.Date);
    char ssl[3]; snprintf(ssl, sizeof(ssl), "%02u", _clkTm.Seconds);
    M5.Lcd.setTextDatum(MC_DATUM);
    M5.Lcd.setTextSize(3); M5.Lcd.setTextColor(p.text, p.bg);    M5.Lcd.drawString(hm, 170, 42);
    M5.Lcd.setTextSize(2); M5.Lcd.setTextColor(p.textDim, p.bg); M5.Lcd.drawString(ssl, 170, 72);
                                                                  M5.Lcd.drawString(wdl, 170, 102);
    M5.Lcd.setTextDatum(TL_DATUM);
    M5.Lcd.setTextSize(1);
  }

  // Pet on left at 5 fps. Clear includes the overlay-particle zone above
  // the body (y<30) — species draw Zzz/hearts there via BUDDY_Y_OVERLAY=6
  // which doesn't go through _yb, so the box has to cover it.
  static uint32_t lastPetTick = 0;
  if (millis() - lastPetTick >= 200) {
    lastPetTick = millis();
    if (buddyMode) {
      M5.Lcd.fillRect(0, 0, 115, 90, p.bg);
      buddyRenderTo(&M5.Lcd, activeState);
    } else {
      characterSetState(activeState);
      characterRenderTo(&M5.Lcd, 57, 45);
    }
  }
  M5.Lcd.setRotation(0);
#endif
}

PersonaState derive(const TamaState& s) {
  if (!s.connected)            return P_IDLE;
  if (s.sessionsWaiting > 0)   return P_ATTENTION;
  if (s.recentlyCompleted)     return P_CELEBRATE;
  if (s.sessionsRunning >= 3)  return P_BUSY;
  return P_IDLE;   // connected, 0+ sessions, nothing urgent — hang out
}

void triggerOneShot(PersonaState s, uint32_t durMs) {
  activeState = s;
  oneShotUntil = millis() + durMs;
}

bool checkShake() {
  float ax, ay, az;
  M5.Imu.getAccelData(&ax, &ay, &az);
  float mag = sqrtf(ax*ax + ay*ay + az*az);
  float delta = fabsf(mag - accelBaseline);
  accelBaseline = accelBaseline * 0.95f + mag * 0.05f;
  return delta > 0.8f;
}




// Persistent screen-level title row ("INFO  n/3") matching the PET header,
// then a per-page section label below it. The fixed title is the cue that
// B cycles pages here just like it does on PET.
static void _infoHeader(const Palette& p, int& y, const char* section, uint8_t page) {
  spr.setTextSize(UI_FONT_BODY);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(UI_TEXT_MARGIN, y); spr.print("Info");
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - UI_GLYPH_W * 4, y); spr.printf("%u/%u", page + 1, INFO_PAGES);
  y += UI_LINE_H + 4;
  spr.setTextColor(p.body, p.bg);
  spr.setCursor(UI_TEXT_MARGIN, y); spr.print(section);
  y += UI_LINE_H + 4;
}

#if !defined(BOARD_LILYGO_EPD47)
void drawPasskey() {
  const Palette& p = characterPalette();
  spr.fillSprite(p.bg);
  spr.setTextSize(UI_FONT_BODY);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(UI_TEXT_MARGIN * 2, UI_PASSKEY_LBL_Y);
  spr.print("BLUETOOTH PAIRING");
  spr.setCursor(UI_TEXT_MARGIN * 2, UI_PASSKEY_HINT_Y);
  spr.print("enter on desktop:");
  spr.setTextSize(UI_PASSKEY_CODE_SZ);
  spr.setTextColor(p.text, p.bg);
  char b[8]; snprintf(b, sizeof(b), "%06lu", (unsigned long)blePasskey());
  int codeW = 6 * UI_PASSKEY_CODE_SZ * 6;
  spr.setCursor((W - codeW) / 2, UI_PASSKEY_CODE_Y);
  spr.print(b);
}
#endif // !BOARD_LILYGO_EPD47

void drawInfo() {
  const Palette& p = characterPalette();
  const int TOP = UI_PANEL_TOP;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_FONT_BODY);
  int y = TOP + 2;
  auto ln = [&](const char* fmt, ...) {
    char b[40]; va_list a; va_start(a, fmt); vsnprintf(b, sizeof(b), fmt, a); va_end(a);
    spr.setCursor(UI_TEXT_MARGIN, y); spr.print(b); y += UI_LINE_H;
  };

  if (infoPage == 0) {
    _infoHeader(p, y, "ABOUT", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("I watch your Claude");
    ln("desktop sessions.");
    y += UI_SECTION_GAP;
    ln("I sleep when nothing's");
    ln("happening, wake when");
    ln("you start working,");
    ln("get impatient when");
    ln("approvals pile up.");
    y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);
#if defined(BOARD_ATD35_S3)
    ln("Tap to approve prompts.");
#else
    ln("Press A on a prompt");
    ln("to approve from here.");
#endif
    y += UI_SECTION_GAP;
    spr.setTextColor(p.textDim, p.bg);
    ln("18 species. Settings");
    ln("> ascii pet to cycle.");

  } else if (infoPage == 1) {
#if defined(BOARD_ATD35_S3)
    _infoHeader(p, y, "CONTROLS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("Tap");
    spr.setTextColor(p.textDim, p.bg); ln("  select menu item");
    ln("  approve/deny prompt"); y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);    ln("Swipe left/right");
    spr.setTextColor(p.textDim, p.bg); ln("  switch screens"); y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);    ln("Swipe up/down");
    spr.setTextColor(p.textDim, p.bg); ln("  cycle pages"); y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);    ln("Long press");
    spr.setTextColor(p.textDim, p.bg); ln("  open/close menu");
#else
    _infoHeader(p, y, "BUTTONS", infoPage);
    spr.setTextColor(p.text, p.bg);    ln("A   front");
    spr.setTextColor(p.textDim, p.bg); ln("    next screen");
    ln("    approve prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("B   right side");
    spr.setTextColor(p.textDim, p.bg); ln("    next page");
    ln("    deny prompt"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("hold A");
    spr.setTextColor(p.textDim, p.bg); ln("    menu"); y += 4;
    spr.setTextColor(p.text, p.bg);    ln("Power  left side");
    spr.setTextColor(p.textDim, p.bg); ln("    tap = screen off");
    ln("    hold 6s = off");
#endif

  } else if (infoPage == 2) {
    _infoHeader(p, y, "CLAUDE", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("  sessions  %u", tama.sessionsTotal);
    ln("  running   %u", tama.sessionsRunning);
    ln("  waiting   %u", tama.sessionsWaiting);
    y += 8;
    spr.setTextColor(p.text, p.bg);
    ln("LINK");
    spr.setTextColor(p.textDim, p.bg);
    ln("  via       %s", dataScenarioName());
#if defined(BOARD_LILYGO_EPD47)
    ln("  wifi      %s", wifiConnected() ? "yes" : "no");
#else
    ln("  ble       %s", !bleConnected() ? "-" : bleSecure() ? "encrypted" : "OPEN");
#endif
    uint32_t age = (millis() - tama.lastUpdated) / 1000;
    ln("  last msg  %lus", (unsigned long)age);
    ln("  state     %s", stateNames[activeState]);

  } else if (infoPage == 3) {
    _infoHeader(p, y, "DEVICE", infoPage);

    int vBat_mV = (int)(M5.Axp.GetBatVoltage() * 1000);
    int iBat_mA = (int)M5.Axp.GetBatCurrent();
    int vBus_mV = (int)(M5.Axp.GetVBusVoltage() * 1000);
    int pct = (vBat_mV - 3200) / 10;
    if (pct < 0) pct = 0; if (pct > 100) pct = 100;
    bool usb = vBus_mV > 4000;
    bool charging = usb && iBat_mA > 1;
    bool full = usb && vBat_mV > 4100 && iBat_mA < 10;

    spr.setTextColor(p.text, p.bg);
    spr.setTextSize(UI_FONT_TITLE);
    spr.setCursor(UI_TEXT_MARGIN, y);
    spr.printf("%d%%", pct);
    spr.setTextSize(UI_FONT_BODY);
    spr.setTextColor(full ? GREEN : (charging ? HOT : p.textDim), p.bg);
    spr.setCursor(UI_TEXT_MARGIN + UI_GLYPH_W * 5, y + UI_GLYPH_H / 2);
    spr.print(full ? "full" : (charging ? "charging" : (usb ? "usb" : "battery")));
    y += UI_LINE_H * 2;

    spr.setTextColor(p.textDim, p.bg);
    ln("  battery  %d.%02dV", vBat_mV/1000, (vBat_mV%1000)/10);
    ln("  current  %+dmA", iBat_mA);
    if (usb) ln("  usb in   %d.%02dV", vBus_mV/1000, (vBus_mV%1000)/10);
    y += UI_SECTION_GAP;

    spr.setTextColor(p.text, p.bg);
    ln("SYSTEM");
    spr.setTextColor(p.textDim, p.bg);
    if (ownerName()[0]) ln("  owner    %s", ownerName());
    uint32_t up = millis() / 1000;
    ln("  uptime   %luh %02lum", up / 3600, (up / 60) % 60);
    ln("  heap     %uKB", ESP.getFreeHeap() / 1024);
    ln("  bright   %u/4", brightLevel);
#if defined(BOARD_LILYGO_EPD47)
    ln("  wifi     %s", wifiConnected() ? "yes" : "off");
#else
    ln("  bt       %s", settings().bt ? (dataBtActive() ? "linked" : "on") : "off");
#endif
    ln("  temp     %dC", (int)M5.Axp.GetTempInAXP192());

  } else if (infoPage == 4) {
#if defined(BOARD_LILYGO_EPD47)
    _infoHeader(p, y, "WIFI", infoPage);
    spr.setTextColor(wifiConnected() ? GREEN : (settings().wifi ? HOT : p.textDim), p.bg);
    spr.setTextSize(UI_FONT_TITLE);
    spr.setCursor(UI_TEXT_MARGIN, y);
    spr.print(wifiConnected() ? "connected" : (settings().wifi ? "connecting" : "off"));
    spr.setTextSize(UI_FONT_BODY);
    y += UI_LINE_H * 2;
    if (wifiConnected()) {
      spr.setTextColor(GREEN, p.bg);
      ln("  %s", WiFi.localIP().toString().c_str());
      spr.setTextColor(p.textDim, p.bg);
      ln("  port %d", WIFI_BRIDGE_PORT);
      ln("  client %s", wifiClientConnected() ? "yes" : "no");
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().wifi) {
      spr.setTextColor(p.textDim, p.bg);
      ln("  waiting for connection...");
      y += UI_SECTION_GAP;
      spr.setTextColor(p.text, p.bg);
      ln("TO CONNECT");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Set WiFi via serial:");
      ln(" {\"wifi_set\":{\"ssid\":\"...\",");
      ln("  \"pass\":\"...\"}}");
    }
#else
    _infoHeader(p, y, "BLUETOOTH", infoPage);
    bool linked = settings().bt && dataBtActive();

    spr.setTextColor(linked ? GREEN : (settings().bt ? HOT : p.textDim), p.bg);
    spr.setTextSize(UI_FONT_TITLE);
    spr.setCursor(UI_TEXT_MARGIN, y);
    spr.print(linked ? "linked" : (settings().bt ? "discover" : "off"));
    spr.setTextSize(UI_FONT_BODY);
    y += UI_LINE_H * 2;

    spr.setTextColor(p.text, p.bg);
    ln("  %s", btName);
    spr.setTextColor(p.textDim, p.bg);
    uint8_t mac[6] = {0};
    esp_read_mac(mac, ESP_MAC_BT);
    ln("  %02X:%02X:%02X:%02X:%02X:%02X",
       mac[0],mac[1],mac[2],mac[3],mac[4],mac[5]);
    y += UI_SECTION_GAP;

    if (linked) {
      uint32_t age = (millis() - tama.lastUpdated) / 1000;
      ln("  last msg  %lus", (unsigned long)age);
    } else if (settings().bt) {
      spr.setTextColor(p.text, p.bg);
      ln("TO PAIR");
      spr.setTextColor(p.textDim, p.bg);
      ln(" Open Claude desktop");
      ln(" > Developer");
      ln(" > Hardware Buddy");
      y += UI_SECTION_GAP;
      ln(" auto-connects via BLE");
    }
#if defined(BOARD_ATD35_S3)
    y += UI_SECTION_GAP;
    spr.setTextColor(p.textDim, p.bg);
    ln("WIFI");
    if (wifiConnected()) {
      spr.setTextColor(GREEN, p.bg);
      ln("  %s", WiFi.localIP().toString().c_str());
      spr.setTextColor(p.textDim, p.bg);
      ln("  port %d", WIFI_BRIDGE_PORT);
      ln("  client %s", wifiClientConnected() ? "yes" : "no");
    } else if (settings().wifi) {
      spr.setTextColor(HOT, p.bg);
      ln("  connecting...");
    } else {
      ln("  off");
    }
#endif
#endif

  } else {
    _infoHeader(p, y, "CREDITS", infoPage);
    spr.setTextColor(p.textDim, p.bg);
    ln("made by");
    y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);
    ln("Felix Rieseberg");
    y += UI_LINE_H;
    spr.setTextColor(p.textDim, p.bg);
    ln("source");
    y += UI_SECTION_GAP;
    spr.setTextColor(p.text, p.bg);
    ln("github.com/anthropics");
    ln("/claude-desktop-buddy");
    y += UI_LINE_H;
    spr.setTextColor(p.textDim, p.bg);
    ln("hardware");
    y += UI_SECTION_GAP;
#if defined(BOARD_LILYGO_EPD47)
    ln("LilyGo T5 4.7\" EPD");
    ln("ESP32 WROVER-E + ED047TC1");
#elif defined(BOARD_ATD35_S3)
    ln("ATD3.5-S3 (ArtronShop)");
    ln("ESP32-S3 + ST7796");
#else
    ln("M5StickC Plus");
    ln("ESP32 + AXP192");
#endif
  }
}


// Greedy word-wrap into fixed-width rows. Continuation rows get a leading
// space. Returns number of rows written.
static uint8_t wrapInto(const char* in, char out[][UI_WRAP_COLS], uint8_t maxRows, uint8_t width) {
  uint8_t row = 0, col = 0;
  const char* p = in;
  while (*p && row < maxRows) {
    while (*p == ' ') p++;                     // skip leading spaces
    // measure next word
    const char* w = p;
    while (*p && *p != ' ') p++;
    uint8_t wlen = p - w;
    if (wlen == 0) break;
    uint8_t need = (col > 0 ? 1 : 0) + wlen;
    if (col + need > width) {
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;              // continuation indent
    }
    if (col > 1 || (col == 1 && out[row][0] != ' ')) out[row][col++] = ' ';
    else if (col == 1 && row > 0) {}           // already have the indent space
    // hard-break words that still don't fit
    while (wlen > width - col) {
      uint8_t take = width - col;
      memcpy(&out[row][col], w, take); col += take; w += take; wlen -= take;
      out[row][col] = 0;
      if (++row >= maxRows) return row;
      out[row][0] = ' '; col = 1;
    }
    memcpy(&out[row][col], w, wlen); col += wlen;
  }
  if (col > 0 && row < maxRows) { out[row][col] = 0; row++; }
  return row;
}

// Touch zones for approve/deny buttons (populated by drawApproval)
static int16_t approveX, approveY, approveW, approveH;
static int16_t denyX, denyY, denyW, denyH;

static void drawApproval() {
  const Palette& p = characterPalette();
  const int AREA = UI_APPROVAL_H;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.drawFastHLine(0, H - AREA, W, p.textDim);

  spr.setTextSize(UI_FONT_BODY);
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(UI_TEXT_MARGIN, H - AREA + UI_TEXT_MARGIN);
  uint32_t waited = (millis() - promptArrivedMs) / 1000;
  if (waited >= 10) spr.setTextColor(HOT, p.bg);
  spr.printf("approve? %lus", (unsigned long)waited);

  int toolLen = strlen(tama.promptTool);
  spr.setTextColor(p.text, p.bg);
  spr.setTextSize(toolLen <= 12 ? UI_FONT_TITLE : UI_FONT_BODY);
  spr.setCursor(UI_TEXT_MARGIN, H - AREA + UI_LINE_H + UI_TEXT_MARGIN);
  spr.print(tama.promptTool);
  spr.setTextSize(UI_FONT_BODY);

  spr.setTextColor(p.textDim, p.bg);
  int hlen = strlen(tama.promptHint);
  spr.setCursor(UI_TEXT_MARGIN, H - AREA + UI_LINE_H * 3 + 4);
  char hfmt[8]; snprintf(hfmt, sizeof(hfmt), "%%.%ds", UI_HINT_CHARS);
  spr.printf(hfmt, tama.promptHint);
  if (hlen > UI_HINT_CHARS) {
    spr.setCursor(UI_TEXT_MARGIN, H - AREA + UI_LINE_H * 4 + 4);
    spr.printf(hfmt, tama.promptHint + UI_HINT_CHARS);
  }

#if defined(BOARD_ATD35_S3)
  // Big tappable buttons
  int btnY = H - UI_BTN_H - UI_BTN_MARGIN;
  approveX = UI_BTN_MARGIN; approveY = btnY;
  approveW = UI_BTN_W; approveH = UI_BTN_H;
  denyX = W - UI_BTN_W - UI_BTN_MARGIN; denyY = btnY;
  denyW = UI_BTN_W; denyH = UI_BTN_H;

  if (responseSent) {
    spr.fillRoundRect(W/2 - 60, btnY, 120, UI_BTN_H, UI_BTN_R, p.textDim);
    spr.setTextColor(p.bg, p.textDim);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(UI_FONT_BODY);
    spr.drawString("sent...", W/2, btnY + UI_BTN_H/2);
    spr.setTextDatum(TL_DATUM);
  } else {
    spr.fillRoundRect(approveX, approveY, approveW, approveH, UI_BTN_R, GREEN);
    spr.setTextColor(0x0000, GREEN);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(UI_FONT_TITLE);
    spr.drawString("Approve", approveX + approveW/2, approveY + approveH/2);

    spr.fillRoundRect(denyX, denyY, denyW, denyH, UI_BTN_R, HOT);
    spr.setTextColor(0xFFFF, HOT);
    spr.drawString("Deny", denyX + denyW/2, denyY + denyH/2);
    spr.setTextDatum(TL_DATUM);
    spr.setTextSize(UI_FONT_BODY);
  }
#else
  if (responseSent) {
    spr.setTextColor(p.textDim, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("sent...");
  } else {
    spr.setTextColor(GREEN, p.bg);
    spr.setCursor(4, H - 12);
    spr.print("A: approve");
    spr.setTextColor(HOT, p.bg);
    spr.setCursor(W - 48, H - 12);
    spr.print("B: deny");
  }
#endif
}

static void tinyHeart(int x, int y, bool filled, uint16_t col) {
  int r = UI_HEART_R;
  if (filled) {
    spr.fillCircle(x - r, y, r, col);
    spr.fillCircle(x + r, y, r, col);
    spr.fillTriangle(x - r*2, y + 1, x + r*2, y + 1, x, y + r*2 + 1, col);
  } else {
    spr.drawCircle(x - r, y, r, col);
    spr.drawCircle(x + r, y, r, col);
    spr.drawLine(x - r*2, y + 1, x, y + r*2 + 1, col);
    spr.drawLine(x + r*2, y + 1, x, y + r*2 + 1, col);
  }
}

static void drawPetStats(const Palette& p) {
  const int TOP = UI_PANEL_TOP;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_FONT_BODY);
  int y = TOP + UI_LINE_H * 2;
  int labelX = UI_TEXT_MARGIN + 2;
  int dataX = UI_TEXT_MARGIN + UI_GLYPH_W * 8;

  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(labelX, y - 2); spr.print("mood");
  uint8_t mood = statsMoodTier();
  uint16_t moodCol = (mood >= 3) ? RED : (mood >= 2) ? HOT : p.textDim;
  for (int i = 0; i < 4; i++) tinyHeart(dataX + i * UI_HEART_SPACE, y + 2, i < mood, moodCol);

  y += UI_LINE_H * 2;
  spr.setCursor(labelX, y - 2); spr.print("fed");
  uint8_t fed = statsFedProgress();
  for (int i = 0; i < 10; i++) {
    int px = dataX + i * UI_STAT_DOT_SPACE;
    if (i < fed) spr.fillCircle(px, y + 1, UI_STAT_DOT_R, p.body);
    else spr.drawCircle(px, y + 1, UI_STAT_DOT_R, p.textDim);
  }

  y += UI_LINE_H * 2;
  spr.setCursor(labelX, y - 2); spr.print("energy");
  uint8_t en = statsEnergyTier();
  uint16_t enCol = (en >= 4) ? 0x07FF : (en >= 2) ? 0xFFE0 : HOT;
  for (int i = 0; i < 5; i++) {
    int px = dataX + i * UI_ENERGY_SPACE;
    if (i < en) spr.fillRect(px, y - UI_ENERGY_H/2, UI_ENERGY_W, UI_ENERGY_H, enCol);
    else spr.drawRect(px, y - UI_ENERGY_H/2, UI_ENERGY_W, UI_ENERGY_H, p.textDim);
  }

  y += UI_LINE_H * 2;
  spr.fillRoundRect(labelX, y - 2, UI_LEVEL_W, UI_LEVEL_H, UI_LEVEL_R, p.body);
  spr.setTextColor(p.bg, p.body);
  spr.setCursor(labelX + UI_GLYPH_W/2, y + (UI_LEVEL_H - UI_GLYPH_H)/2);
  spr.printf("Lv %u", stats().level);

  y += UI_LINE_H * 2 + 4;
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(labelX, y);
  spr.printf("approved %u", stats().approvals);
  spr.setCursor(labelX, y + UI_LINE_H);
  spr.printf("denied   %u", stats().denials);
  uint32_t nap = stats().napSeconds;
  spr.setCursor(labelX, y + UI_LINE_H * 2);
  spr.printf("napped   %luh%02lum", nap/3600, (nap/60)%60);
  auto tokFmt = [&](const char* label, uint32_t v, int yPx) {
    spr.setCursor(labelX, yPx);
    if (v >= 1000000)   spr.printf("%s%lu.%luM", label, v/1000000, (v/100000)%10);
    else if (v >= 1000) spr.printf("%s%lu.%luK", label, v/1000, (v/100)%10);
    else                spr.printf("%s%lu", label, v);
  };
  tokFmt("tokens   ", stats().tokens, y + UI_LINE_H * 3);
  tokFmt("today    ", tama.tokensToday, y + UI_LINE_H * 4);
}

static void drawPetHowTo(const Palette& p) {
  const int TOP = UI_PANEL_TOP;
  spr.fillRect(0, TOP, W, H - TOP, p.bg);
  spr.setTextSize(UI_FONT_BODY);
  int y = TOP + 2;
  auto ln = [&](uint16_t c, const char* s) {
    spr.setTextColor(c, p.bg); spr.setCursor(UI_TEXT_MARGIN + 2, y); spr.print(s); y += UI_LINE_H;
  };
  auto gap = [&]() { y += UI_SECTION_GAP; };

  y += UI_LINE_H + 4;

  ln(p.body,    "MOOD");
  ln(p.textDim, " approve fast = up");
  ln(p.textDim, " deny lots = down"); gap();

  ln(p.body,    "FED");
  ln(p.textDim, " 50K tokens =");
  ln(p.textDim, " level up + confetti"); gap();

  ln(p.body,    "ENERGY");
  ln(p.textDim, " face-down to nap");
  ln(p.textDim, " refills to full"); gap();

  ln(p.textDim, "idle 30s = off");
#if defined(BOARD_ATD35_S3)
  ln(p.textDim, "tap = wake"); gap();
  ln(p.textDim, "swipe: screens/pages");
  ln(p.textDim, "long press: menu");
#else
  ln(p.textDim, "any button = wake"); gap();
  ln(p.textDim, "A: screens  B: page");
  ln(p.textDim, "hold A: menu");
#endif
}

void drawPet() {
  const Palette& p = characterPalette();
  int y = UI_PANEL_TOP;

  if (petPage == 0) drawPetStats(p);
  else drawPetHowTo(p);

  spr.setTextSize(UI_FONT_BODY);
  spr.setTextColor(p.text, p.bg);
  spr.setCursor(UI_TEXT_MARGIN, y + 2);
  if (ownerName()[0]) {
    spr.printf("%s's %s", ownerName(), petName());
  } else {
    spr.print(petName());
  }
  spr.setTextColor(p.textDim, p.bg);
  spr.setCursor(W - UI_GLYPH_W * 4, y + 2);
  spr.printf("%u/%u", petPage + 1, PET_PAGES);
}

void drawHUD() {
  if (tama.promptId[0]) { drawApproval(); return; }
  const Palette& p = characterPalette();
  const int SHOW = UI_HUD_LINES, LH = UI_HUD_LINE_H, WIDTH = UI_WRAP_WIDTH;
  const int AREA = SHOW * LH + 4;
  spr.fillRect(0, H - AREA, W, AREA, p.bg);
  spr.setTextSize(UI_FONT_BODY);

  if (tama.lineGen != lastLineGen) { msgScroll = 0; lastLineGen = tama.lineGen; wake(); }

  if (tama.nLines == 0) {
    spr.setTextColor(p.text, p.bg);
    spr.setCursor(UI_TEXT_MARGIN, H - LH - 2);
    spr.print(tama.msg);
    return;
  }

  static char disp[32][UI_WRAP_COLS];
  static uint8_t srcOf[32];
  uint8_t nDisp = 0;
  for (uint8_t i = 0; i < tama.nLines && nDisp < 32; i++) {
    uint8_t got = wrapInto(tama.lines[i], &disp[nDisp], 32 - nDisp, WIDTH);
    for (uint8_t j = 0; j < got; j++) srcOf[nDisp + j] = i;
    nDisp += got;
  }

  uint8_t maxBack = (nDisp > SHOW) ? (nDisp - SHOW) : 0;
  if (msgScroll > maxBack) msgScroll = maxBack;

  int end = (int)nDisp - msgScroll;
  int start = end - SHOW; if (start < 0) start = 0;
  uint8_t newest = tama.nLines - 1;
  for (int i = 0; start + i < end; i++) {
    uint8_t row = start + i;
    bool fresh = (srcOf[row] == newest) && (msgScroll == 0);
    spr.setTextColor(fresh ? p.text : p.textDim, p.bg);
    spr.setCursor(UI_TEXT_MARGIN, H - AREA + 2 + i * LH);
    spr.print(disp[row]);
  }
  if (msgScroll > 0) {
    spr.setTextColor(p.body, p.bg);
    spr.setCursor(W - UI_GLYPH_W * 3, H - LH - 2);
    spr.printf("-%u", msgScroll);
  }
}

void setup() {
  Serial.begin(115200);
  delay(500);
  Serial.println("[boot] starting...");
#if defined(BOARD_LILYGO_EPD47)
  M5.setSpr(&spr);
#endif
  M5.begin();
  Serial.println("[boot] M5.begin() done");
  M5.Lcd.setRotation(0);
  M5.Imu.Init();
  M5.Beep.begin();
#if !defined(BOARD_LILYGO_EPD47)
  startBt();
#endif
#if defined(BOARD_ATD35_S3) || defined(BOARD_LILYGO_EPD47)
  wifiLoadCredentials();
#endif
  if (LED_PIN >= 0) {
    pinMode(LED_PIN, OUTPUT);
    digitalWrite(LED_PIN, HIGH);   // off
  }
  applyBrightness();
  lastInteractMs = millis();
  statsLoad();
  settingsLoad();
#if defined(BOARD_ATD35_S3) || defined(BOARD_LILYGO_EPD47)
  if (settings().wifi) wifiInit();
#endif
  petNameLoad();
  buddyInit();

  // BLE stays always-on; s.bt is stored as a preference only.
#if defined(BOARD_JC3248W535C)
  // Native AXS push DMAs W*H*2 raw bytes — the sprite MUST be 16bpp RGB565.
  // A default/8bpp sprite would feed palette indices as pixels ("white and
  // blue" garbage) and read past the buffer. Force depth before allocating.
  spr.setColorDepth(16);
#endif
  void* sprBuf = spr.createSprite(W, H);
  Serial.printf("[boot] sprite create: %s (%dx%d) depth=%dbpp\n",
                sprBuf ? "OK" : "FAIL", W, H, (int)spr.getColorDepth());
  characterInit(nullptr);  // scan /characters/ for whatever is installed
  gifAvailable = characterLoaded();
  // species NVS: 0..N-1 = ASCII species, 0xFF = use GIF (also the default,
  // so a fresh install lands on the GIF). With no GIF installed, 0xFF falls
  // through to buddyInit()'s clamped default.
#if defined(BOARD_LILYGO_EPD47)
  buddyMode = true;  // EPD: ASCII only, no GIF
  gifAvailable = false;
#else
  buddyMode = !(gifAvailable && speciesIdxLoad() == SPECIES_GIF);
#endif
  applyDisplayMode();

  {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextDatum(MC_DATUM);
    spr.setTextSize(UI_FONT_TITLE);
    if (ownerName()[0]) {
      char line[40];
      snprintf(line, sizeof(line), "%s's", ownerName());
      spr.setTextColor(p.text, p.bg);   spr.drawString(line, W/2, H/2 - UI_LINE_H);
      spr.setTextColor(p.body, p.bg);   spr.drawString(petName(), W/2, H/2 + UI_LINE_H);
    } else {
      spr.setTextColor(p.body, p.bg);   spr.drawString("Hello!", W/2, H/2 - UI_LINE_H);
      spr.setTextSize(UI_FONT_BODY);
      spr.setTextColor(p.textDim, p.bg);
      spr.drawString("a buddy appears", W/2, H/2 + UI_LINE_H);
    }
    spr.setTextDatum(TL_DATUM); spr.setTextSize(UI_FONT_BODY);
    buddyPush(spr);
    delay(1800);
  }

  Serial.printf("buddy: %s\n", buddyMode ? "ASCII mode" : "GIF character loaded");
}

void loop() {
  M5.update();
  M5.Beep.update();
  t++;
  uint32_t now = millis();

  dataPoll(&tama);
  if (statsPollLevelUp()) triggerOneShot(P_CELEBRATE, 3000);
  baseState = derive(tama);

  // After waking the screen, hold sleep for 12s so users see the wake-up
  // animation. Urgent states (attention, celebrate, busy) override this.
  if (baseState == P_IDLE && (int32_t)(now - wakeTransitionUntil) < 0) baseState = P_SLEEP;

  if ((int32_t)(now - oneShotUntil) >= 0) activeState = baseState;

  // LED: pulse on attention, otherwise off
  if (LED_PIN >= 0) {
    if (activeState == P_ATTENTION && settings().led) {
      digitalWrite(LED_PIN, (now / 400) % 2 ? LOW : HIGH);
    } else {
      digitalWrite(LED_PIN, HIGH);
    }
  }

  // shake → dizzy + force scenario advance
  if (now - lastShakeCheck > 50) {
    lastShakeCheck = now;
    if (!menuOpen && !screenOff && checkShake() && (int32_t)(now - oneShotUntil) >= 0) {
      wake();
      triggerOneShot(P_DIZZY, 2000);
      Serial.println("shake: dizzy");
    }
  }

  // BtnA: step through fake scenarios
  // Prompt arrival: beep, reset response flag
  if (strcmp(tama.promptId, lastPromptId) != 0) {
    strncpy(lastPromptId, tama.promptId, sizeof(lastPromptId)-1);
    lastPromptId[sizeof(lastPromptId)-1] = 0;
    responseSent = false;
    if (tama.promptId[0]) {
      promptArrivedMs = millis();
      wake();
      beep(1200, 80);   // alert chirp
      // Jump to the approval screen no matter what was open — drawApproval
      // only runs from drawHUD which only runs in DISP_NORMAL.
      displayMode = DISP_NORMAL;
      menuOpen = settingsOpen = resetOpen = false;
      applyDisplayMode();
      characterInvalidate();
      if (buddyMode) buddyInvalidate();
    }
  }

  bool inPrompt = tama.promptId[0] && !responseSent;

#if defined(BOARD_ATD35_S3)
  // Touch gesture handling
  auto g = M5.Touch.gesture;
  if (g != TouchState::NONE) {
    wake();
    if (g == TouchState::LONG_PRESS) {
      beep(800, 60);
      if (resetOpen) { resetOpen = false; }
      else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
      else { menuOpen = !menuOpen; menuSel = 0; if (!menuOpen) characterInvalidate(); }
    } else if (g == TouchState::TAP) {
      int tx = M5.Touch.tapX, ty = M5.Touch.tapY;
      if (inPrompt && !responseSent) {
        if (tx >= approveX && tx < approveX + approveW && ty >= approveY && ty < approveY + approveH) {
          char cmd[96];
          snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
          sendCmd(cmd);
          responseSent = true;
          uint32_t tookS = (millis() - promptArrivedMs) / 1000;
          statsOnApproval(tookS);
          beep(2400, 60);
          if (tookS < 5) triggerOneShot(P_HEART, 2000);
        } else if (tx >= denyX && tx < denyX + denyW && ty >= denyY && ty < denyY + denyH) {
          char cmd[96];
          snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
          sendCmd(cmd);
          responseSent = true;
          statsOnDenial();
          beep(600, 60);
        }
      } else if (resetOpen) {
        for (int i = 0; i < (int)menuRectCount; i++) {
          if (ty >= menuRects[i].y && ty < menuRects[i].y + menuRects[i].h) {
            resetSel = i; beep(2400, 30); applyReset(i); break;
          }
        }
      } else if (settingsOpen) {
        for (int i = 0; i < (int)menuRectCount; i++) {
          if (ty >= menuRects[i].y && ty < menuRects[i].y + menuRects[i].h) {
            settingsSel = i; beep(2400, 30); applySetting(i); break;
          }
        }
      } else if (menuOpen) {
        for (int i = 0; i < (int)menuRectCount; i++) {
          if (ty >= menuRects[i].y && ty < menuRects[i].y + menuRects[i].h) {
            menuSel = i; beep(2400, 30); menuConfirm(); break;
          }
        }
      }
    } else if (g == TouchState::SWIPE_LEFT || g == TouchState::SWIPE_RIGHT) {
      if (!menuOpen && !settingsOpen && !resetOpen && !inPrompt) {
        beep(1800, 30);
        if (g == TouchState::SWIPE_LEFT)
          displayMode = (displayMode + 1) % DISP_COUNT;
        else
          displayMode = (displayMode + DISP_COUNT - 1) % DISP_COUNT;
        applyDisplayMode();
      }
    } else if (g == TouchState::SWIPE_UP || g == TouchState::SWIPE_DOWN) {
      if (!menuOpen && !settingsOpen && !resetOpen) {
        if (displayMode == DISP_INFO) {
          beep(2400, 30);
          if (g == TouchState::SWIPE_UP)
            infoPage = (infoPage + 1) % INFO_PAGES;
          else
            infoPage = (infoPage + INFO_PAGES - 1) % INFO_PAGES;
        } else if (displayMode == DISP_PET) {
          beep(2400, 30);
          petPage = (petPage + 1) % PET_PAGES;
          applyDisplayMode();
        } else {
          beep(2400, 30);
          msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
        }
      }
    }
    M5.Touch.consume();
  }
#endif

  // Button-press wake. Track which button woke the screen so its full
  // press cycle (including long-press) is swallowed — you don't want
  // BtnA-to-wake to also cycle displayMode or open the menu.
  if (M5.BtnA.isPressed() || M5.BtnB.isPressed()) {
    if (screenOff) {
      if (M5.BtnA.isPressed()) swallowBtnA = true;
      if (M5.BtnB.isPressed()) swallowBtnB = true;
    }
    wake();
  }

  // AXP power button (left side): short-press toggles screen off.
  // Long-press (6s) still powers off the device via AXP hardware.
  if (M5.Axp.GetBtnPress() == 0x02) {
    if (screenOff) {
      wake();
    } else {
      M5.Axp.SetLDO2(false);
      screenOff = true;
    }
  }

  if (M5.BtnA.pressedFor(600) && !btnALong && !swallowBtnA) {
    btnALong = true;
    beep(800, 60);
    if (resetOpen) { resetOpen = false; }
    else if (settingsOpen) { settingsOpen = false; characterInvalidate(); }
    else {
      menuOpen = !menuOpen;
      menuSel = 0;
      if (!menuOpen) characterInvalidate();
    }
    Serial.println(menuOpen ? "menu open" : "menu close");
  }
  if (M5.BtnA.wasReleased()) {
    if (!btnALong && !swallowBtnA) {
      if (inPrompt) {
        char cmd[96];
        snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"once\"}", tama.promptId);
        sendCmd(cmd);
        responseSent = true;
        uint32_t tookS = (millis() - promptArrivedMs) / 1000;
        statsOnApproval(tookS);
        beep(2400, 60);
        if (tookS < 5) triggerOneShot(P_HEART, 2000);
      } else if (resetOpen) {
        beep(1800, 30);
        resetSel = (resetSel + 1) % RESET_N;
        resetConfirmIdx = 0xFF;
      } else if (settingsOpen) {
        beep(1800, 30);
        settingsSel = (settingsSel + 1) % SETTINGS_N;
      } else if (menuOpen) {
        beep(1800, 30);
        menuSel = (menuSel + 1) % MENU_N;
      } else {
        beep(1800, 30);
        displayMode = (displayMode + 1) % DISP_COUNT;
        applyDisplayMode();
      }
    }
    btnALong = false;
    swallowBtnA = false;
  }

  // BtnB: pet → heart
  if (M5.BtnB.wasPressed()) {
    if (swallowBtnB) { swallowBtnB = false; }
    else
    if (inPrompt) {
      char cmd[96];
      snprintf(cmd, sizeof(cmd), "{\"cmd\":\"permission\",\"id\":\"%s\",\"decision\":\"deny\"}", tama.promptId);
      sendCmd(cmd);
      responseSent = true;
      statsOnDenial();
      beep(600, 60);
    } else if (resetOpen) {
      beep(2400, 30);
      applyReset(resetSel);
    } else if (settingsOpen) {
      beep(2400, 30);
      applySetting(settingsSel);
    } else if (menuOpen) {
      beep(2400, 30);
      menuConfirm();
    } else if (displayMode == DISP_INFO) {
      beep(2400, 30);
      infoPage = (infoPage + 1) % INFO_PAGES;
    } else if (displayMode == DISP_PET) {
      beep(2400, 30);
      petPage = (petPage + 1) % PET_PAGES;
      applyDisplayMode();
    } else {
      beep(2400, 30);
      msgScroll = (msgScroll >= 30) ? 0 : msgScroll + 1;
    }
  }

  // blink bookkeeping

  // Charging clock: takes over the home screen when on USB power, no
  // overlays, no prompt, no live Claude data, and the RTC has been set
  // by the bridge. Pet sleeps underneath. Exit restores Y via
  // applyDisplayMode() so the next mode-switch isn't visually offset.
  clockRefreshRtc();   // 1Hz internal throttle; also caches _onUsb
  // Show the clock when nothing is happening — bridge heartbeat alone
  // doesn't count as activity (it's the only way to get the RTC synced).
  bool clocking = displayMode == DISP_NORMAL
               && !menuOpen && !settingsOpen && !resetOpen && !inPrompt
               && tama.sessionsRunning == 0 && tama.sessionsWaiting == 0
               && dataRtcValid() && _onUsb;
  if (clocking) clockUpdateOrient();
  else { clockOrient = 0; orientFrames = 0; paintedOrient = 0; }
  bool landscapeClock = clocking && clockOrient != 0;

  static bool wasClocking = false;
  static bool wasLandscape = false;
  if (clocking != wasClocking || landscapeClock != wasLandscape) {
    if (clocking && !landscapeClock) characterSetPeek(true);
    else applyDisplayMode();
    characterInvalidate();
    if (buddyMode) buddyInvalidate();
    wasClocking = clocking;
    wasLandscape = landscapeClock;
  }
  if (clocking) {
    uint8_t dow = clockDow();
    bool weekend = (dow == 0 || dow == 6);
    bool friday  = (dow == 5);

    uint8_t h = _clkTm.Hours;
    if (h >= 1 && h < 7)             activeState = P_SLEEP;
    else if (weekend)                activeState = (now/8000 % 6 == 0) ? P_HEART : P_SLEEP;
    else if (h < 9)                  activeState = (now/6000 % 4 == 0) ? P_IDLE  : P_SLEEP;
    else if (h == 12)                activeState = (now/5000 % 3 == 0) ? P_HEART : P_IDLE;
    else if (friday && h >= 15)      activeState = (now/4000 % 3 == 0) ? P_CELEBRATE : P_IDLE;
    else if (h >= 22 || h == 0)      activeState = (now/7000 % 3 == 0) ? P_DIZZY : P_SLEEP;
    else                             activeState = (now/10000 % 5 == 0) ? P_SLEEP : P_IDLE;
  }

#if !defined(BOARD_LILYGO_EPD47)
  static uint32_t lastPasskey = 0;
  uint32_t pk = blePasskey();
  if (pk && !lastPasskey) { wake(); beep(1800, 60); }
  lastPasskey = pk;
#endif

  if (napping || screenOff || landscapeClock) {
    // skip sprite render — face-down, powered off, or landscape clock
    // (which draws direct-to-LCD below)
  } else if (buddyMode) {
    buddyTick(activeState);
  } else if (characterLoaded()) {
    characterSetState(activeState);
    characterTick();
  } else {
    const Palette& p = characterPalette();
    spr.fillSprite(p.bg);
    spr.setTextColor(p.textDim, p.bg);
    spr.setTextSize(1);
    if (xferActive()) {
      uint32_t done = xferProgress(), total = xferTotal();
      spr.setCursor(8, 90);
      spr.print("installing");
      spr.setCursor(8, 102);
      spr.printf("%luK / %luK", done/1024, total/1024);
      int barW = W - 16;
      spr.drawRect(8, 116, barW, 8, p.textDim);
      if (total > 0) {
        int fill = (int)((uint64_t)barW * done / total);
        if (fill > 1) spr.fillRect(9, 117, fill - 1, 6, p.body);
      }
    } else {
      spr.setCursor(8, 100);
      spr.print("no character loaded");
    }
  }
  if (landscapeClock) {
    drawClock();
  } else if (!napping && !screenOff) {
#if !defined(BOARD_LILYGO_EPD47)
    if (blePasskey()) drawPasskey();
    else
#endif
    if (clocking) drawClock();
    else if (displayMode == DISP_INFO) drawInfo();
    else if (displayMode == DISP_PET) drawPet();
    else if (settings().hud) drawHUD();
    if (resetOpen) drawReset();
    else if (settingsOpen) drawSettings();
    else if (menuOpen) drawMenu();
    buddyPush(spr);
  }

  // Face-down nap: dim immediately, pause animations, accumulate sleep time.
  // Skipped during approval — you're holding it to read, not sleeping it.
  // Exit needs sustained not-down so IMU noise at the threshold doesn't
  // bounce brightness between 8 and full every few frames.
  static int8_t faceDownFrames = 0;
  if (!inPrompt) {
    bool down = isFaceDown();
    if (down)       { if (faceDownFrames < 20) faceDownFrames++; }
    else            { if (faceDownFrames > -10) faceDownFrames--; }
  }

  if (!napping && faceDownFrames >= 15) {
    napping = true;
    napStartMs = now;
    M5.Axp.ScreenBreath(8);
    dimmed = true;
  } else if (napping && faceDownFrames <= -8) {
    napping = false;
    statsOnNapEnd((now - napStartMs) / 1000);
    statsOnWake();
    wake();
  }

  // millis() not the cached `now`: wake() runs after `now` is captured,
  // so now - lastInteractMs underflows when a button is held → flicker.
  // No auto-off on USB power — clock face wants to stay visible while charging.
  if (!screenOff && !inPrompt && !_onUsb
      && millis() - lastInteractMs > SCREEN_OFF_MS) {
    M5.Axp.SetLDO2(false);
    screenOff = true;
  }

#if defined(BOARD_LILYGO_EPD47)
  delay(screenOff ? 200 : 100);  // EPD: no need for fast loop
#else
  delay(screenOff ? 100 : 16);
#endif
}
