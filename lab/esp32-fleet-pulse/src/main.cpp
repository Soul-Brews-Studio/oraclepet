// esp32-fleet-pulse — the embedded sibling of HomekeeperWidget.
//
// An ESP32-S3 (Guition JC3248W535, AXS15231 320x480 QSPI) joins WiFi and talks to
// a maw backend on 192.168.1.109:3456. Two views, toggled by tapping the screen:
//   • CONTENT (default) — streams a tmux pane's live text from /api/capture?target=…
//                         (default target: esp32-oracle = 52-esp32:1)
//   • LIST              — the fleet session list from /api/ls
//
// Render model: ONE 320x480 RGB565 sprite in PSRAM is the framebuffer; the whole
// view is drawn into it each refresh, then a single axs::push() blits it. This is
// the only TU that calls axs:: (the HAL keeps static state).
//
// Concurrency model (the responsiveness win): the blocking HTTP GET (up to 12s)
// lives on a FreeRTOS task pinned to CORE 0 (netTask). The Arduino loop() runs on
// CORE 1 and does ONLY touch + render — it never calls HTTPClient, so a fetch that
// stalls for 12s can no longer freeze touch. netTask fetches into a task-private
// staging Model (no lock held during GET/JSON), then publishes with a single
// mutex-guarded memcpy. loop() snapshots the shared Model under the same mutex and
// renders the snapshot. The slow HTTP read NEVER holds the lock.
//
// Pacing (fleet learning "pace every loop path"): loop() delay(20) on every path;
// netTask vTaskDelay between fetches. Never busy-spin.

#define LGFX_USE_V1
#include <LovyanGFX.hpp>
#include <Arduino.h>
#include "axs15231_qspi.h"        // namespace axs (include LovyanGFX first)
#include <WiFi.h>
#include <HTTPClient.h>
#include <ArduinoJson.h>          // v7
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>
#include <freertos/semphr.h>

#if __has_include("secrets.h")
  #include "secrets.h"            // WIFI_SSID / WIFI_PASS / BACKEND_URL (gitignored)
#else
  #error "Copy src/secrets.h.example to src/secrets.h and fill in WiFi + BACKEND_URL"
#endif

// Which pane the CONTENT view streams. esp32-oracle is 52-esp32 window 1.
#ifndef CAP_TARGET
#define CAP_TARGET "52-esp32:1"
#endif

// ── Framebuffer ────────────────────────────────────────────────────────────
static LGFX_Sprite spr;           // 320x480 RGB565 framebuffer in PSRAM (~300KB)

// ── Model dimensions ─────────────────────────────────────────────────────────
static constexpr int MAXLINES = 90;
static constexpr int MAXCOL   = 56;        // chars kept per line (>= the ~52 cols that fit at size 1)
static constexpr int MAXSESS  = 48;

// ── Connection / view state ──────────────────────────────────────────────────
enum class Net  { BOOT, WIFI_WAIT, WIFI_FAIL, HTTP_ERR, OK };
enum class Mode { CONTENT, LIST };

struct Sess { char name[40]; uint8_t windows; bool active; };

// ── The shared model ─────────────────────────────────────────────────────────
// Everything the renderer reads, gathered into ONE struct so publish/snapshot is a
// single memcpy. Three instances exist:
//   g_shared  — the published copy (written by netTask under g_lock, read by loop()
//               under g_lock via snapshot())
//   g_ui      — loop()-local snapshot; rendered lock-free after the snapshot copy
//   s_stage   — netTask-private staging; the GET + JSON parse fill THIS with no lock
// Note: `scroll`, the fit toggle, and the active mode are UI-OWNED and kept OUTSIDE
// this struct as plain globals — netTask must never write them, and publish() must
// not clobber them (it can't, since they aren't in Model).
struct Model {
  // CONTENT view
  char     lines[MAXLINES][MAXCOL + 1];
  uint16_t colr[MAXLINES][MAXCOL];        // per-char SGR fg (RGB565), parallel to lines[]
  int      nlines;
  // LIST view
  Sess    sess[MAXSESS];
  int     count;
  char    node[16];
  char    oracle[16];
  // status
  Net     net;
  int     lastHttp;
  uint32_t lastOk;
};

static Model g_shared;            // published (mutex-guarded)
static Model g_ui;                // UI snapshot (rendered)
static Model s_stage;             // netTask-private staging

// ── UI-owned state (never touched by netTask) ────────────────────────────────
static int   g_scroll = 0;        // LIST scroll position
static bool  g_fit    = false;    // CONTENT font: false = SMALL/size1 dense, true = BIG/size2
static Mode  g_mode   = Mode::CONTENT;     // default: stream esp32-oracle content

// ── Cross-task signalling ────────────────────────────────────────────────────
static SemaphoreHandle_t g_lock = nullptr;
static volatile Mode     g_modeReq = Mode::CONTENT;   // UI -> net: which view to fetch (single aligned word)

// ── Colors (RGB565) ──────────────────────────────────────────────────────────
static constexpr uint16_t C_BG=0x0000, C_HDR=0x07FF, C_RULE=0x4208, C_ACTIVE=0xFFFF,
  C_IDLE=0x8C71, C_DOT=0x07E0, C_WIN=0x05FF, C_WARN=0xFD20, C_FAIL=0xF800, C_TEXT=0xCE59;

static constexpr int W=320, Hgt=480, HDR=28, ROW_H=20;
static constexpr int VIS_ROWS = (Hgt - HDR) / ROW_H;

// ── Derive the backend base ("http://host:port") from BACKEND_URL (".../api/ls") ─
static String backendBase() {
  String b = BACKEND_URL;
  int i = b.indexOf("/api/");
  if (i > 0) b = b.substring(0, i);
  return b;
}

// ── Publish / snapshot — the ONLY two locked sections ─────────────────────────
// Each is a single sizeof(Model) memcpy (microseconds). No HTTP, no JSON, no render
// happens under the lock. publish() preserves nothing UI-owned because nothing
// UI-owned lives in Model — that invariant is what keeps a fetch from resetting the
// user's scroll / fit / mode.
static void publish(const Model& src) {
  if (!g_lock) { g_shared = src; return; }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  memcpy(&g_shared, &src, sizeof(Model));
  xSemaphoreGive(g_lock);
}

static void snapshot(Model& dst) {
  if (!g_lock) { dst = g_shared; return; }
  xSemaphoreTake(g_lock, portMAX_DELAY);
  memcpy(&dst, &g_shared, sizeof(Model));
  xSemaphoreGive(g_lock);
}

// ── WiFi (runs in setup(), synchronous, before any UI competes) ──────────────
static void wifiBegin() {
  WiFi.mode(WIFI_STA);
  WiFi.setSleep(false);
  WiFi.setAutoReconnect(true);            // auto-rejoin on a transient AP drop
  WiFi.begin(WIFI_SSID, WIFI_PASS);
  s_stage.net = Net::WIFI_WAIT;
  publish(s_stage);
  uint32_t t0 = millis();
  while (WiFi.status() != WL_CONNECTED && millis() - t0 < 15000) delay(200);
  bool ok = WiFi.status() == WL_CONNECTED;
  s_stage.net = ok ? Net::OK : Net::WIFI_FAIL;
  publish(s_stage);
  Serial.printf("[wifi] %s ip=%s\n", ok ? "OK" : "TIMEOUT", WiFi.localIP().toString().c_str());
}

// ── ANSI SGR → RGB565 color (ported from the desktop tmux-tray ANSIParser) ─────
static constexpr uint16_t rgb565(uint8_t r, uint8_t g, uint8_t b) {
  return (uint16_t)(((r & 0xF8) << 8) | ((g & 0xFC) << 3) | (b >> 3));
}
static const uint16_t SGR16[16] = {   // 0-7 normal, 8-15 bright
  rgb565(0,0,0),       rgb565(205,72,72),   rgb565(92,184,92),   rgb565(209,179,87),
  rgb565(92,148,219),  rgb565(184,112,199), rgb565(87,184,189),  rgb565(199,199,199),
  rgb565(115,115,115), rgb565(245,108,102), rgb565(128,224,128), rgb565(245,219,112),
  rgb565(128,184,245), rgb565(219,148,235), rgb565(128,230,235), rgb565(245,245,245),
};
static uint16_t xterm256(int n) {
  if (n < 16)  return SGR16[n < 0 ? 7 : n];
  if (n < 232) { int x = n - 16; static const uint8_t L[6] = {0,95,135,175,215,255};
                 return rgb565(L[(x/36)%6], L[(x/6)%6], L[x%6]); }
  if (n < 256) { int v = 8 + (n - 232) * 10; return rgb565(v, v, v); }
  return C_TEXT;
}
// Update current fg/bold from an SGR param string [a,b) (chars between ESC[ and 'm').
static void applySGR(const char* a, const char* b, uint16_t& fg, bool& bold) {
  if (a >= b) { fg = C_TEXT; bold = false; return; }        // ESC[m == reset
  int code[12], nc = 0, v = 0; bool have = false;
  for (const char* p = a; p <= b; p++) {
    if (p < b && *p >= '0' && *p <= '9') { v = v*10 + (*p - '0'); have = true; }
    else { if (nc < 12) code[nc++] = have ? v : 0; v = 0; have = false; }
  }
  for (int i = 0; i < nc; i++) {
    int c = code[i];
    if      (c == 0)  { fg = C_TEXT; bold = false; }
    else if (c == 1)  bold = true;
    else if (c == 22) bold = false;
    else if (c >= 30 && c <= 37) fg = SGR16[(bold ? 8 : 0) + (c - 30)];
    else if (c == 39) fg = C_TEXT;
    else if (c >= 90 && c <= 97) fg = SGR16[8 + (c - 90)];
    else if (c == 38) {
      if      (i+2 < nc && code[i+1] == 5) { fg = xterm256(code[i+2]);                     i += 2; }
      else if (i+4 < nc && code[i+1] == 2) { fg = rgb565(code[i+2], code[i+3], code[i+4]); i += 4; }
    }
  }
}

// ── CONTENT: PARSE ANSI into dst.lines + per-char dst.colr (keep color, no wrap) ─
// Same tail-keeping + no-soft-wrap behavior, but instead of stripping color we
// resolve each glyph's fg from the live SGR state and store it parallel in colr[].
static void contentToLines_into(Model& dst, const char* s) {
  dst.nlines = 0; int col = 0; dst.lines[0][0] = '\0';
  uint16_t fg = C_TEXT; bool bold = false;
  auto pushLine = [&]() {
    dst.lines[dst.nlines][col] = '\0';
    if (dst.nlines < MAXLINES - 1) dst.nlines++;
    else { for (int k = 1; k < MAXLINES; k++) {            // scroll up: text + colors together
      memcpy(dst.lines[k-1], dst.lines[k], MAXCOL + 1);
      memcpy(dst.colr[k-1],  dst.colr[k],  MAXCOL * sizeof(uint16_t));
    } }
    col = 0; dst.lines[dst.nlines][0] = '\0';
  };
  for (const char* p = s; *p; ) {
    unsigned char c = (unsigned char)*p;
    if (c == 0x1B) {                                       // ESC — parse SGR, skip the rest
      p++;
      if (*p == '[') {
        p++;
        const char* ps = p;
        while (*p && !(*p >= '@' && *p <= '~')) p++;       // params until the final byte
        if (*p == 'm') applySGR(ps, p, fg, bold);          // SGR → update fg/bold
        if (*p) p++;
      }
      else if (*p == ']') { while (*p && (unsigned char)*p != 0x07) p++; if (*p) p++; }   // OSC…BEL
      else if (*p) p++;
      continue;
    }
    if (c == '\r') { p++; continue; }
    if (c == '\n') { pushLine(); p++; continue; }
    if (c < 0x20 && c != '\t') { p++; continue; }
    if (col < MAXCOL) { dst.lines[dst.nlines][col] = (char)c; dst.colr[dst.nlines][col] = fg; col++; }
    p++;
  }
  dst.lines[dst.nlines][col] = '\0';
  if (col > 0) dst.nlines++;
}

// ── netTask fetchers — fill s_stage, NEVER hold the lock during GET/parse ──────
// fetchContent: GET /api/capture?target=…, filter-parse "content", strip ANSI.
static bool netFetchContent() {
  if (WiFi.status() != WL_CONNECTED) { s_stage.net = Net::WIFI_FAIL; WiFi.reconnect(); return false; }
  HTTPClient http;
  http.begin(backendBase() + "/api/capture?target=" + CAP_TARGET);
  http.useHTTP10(true);                            // close-after-body → clean EOF for the ~16KB capture
  http.setConnectTimeout(6000); http.setTimeout(12000);
  int code = http.GET(); s_stage.lastHttp = code;
  if (code != 200) { Serial.printf("[cap] %d\n", code); http.end(); s_stage.net = Net::HTTP_ERR; return false; }
  JsonDocument filter; filter["content"] = true;
  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[cap] json %s\n", err.c_str()); s_stage.net = Net::HTTP_ERR; return false; }
  contentToLines_into(s_stage, doc["content"] | "");
  s_stage.net = Net::OK; s_stage.lastOk = millis();
  Serial.printf("[cap] %d lines\n", s_stage.nlines);
  return true;
}

// fetchSessions: GET /api/ls, filter-parse, fill s_stage.sess.
static bool netFetchSessions() {
  if (WiFi.status() != WL_CONNECTED) { s_stage.net = Net::WIFI_FAIL; WiFi.reconnect(); return false; }
  HTTPClient http;
  http.begin(BACKEND_URL);
  http.useHTTP10(true);                            // close-after-body → clean EOF, no chunked read-hang
  http.setConnectTimeout(6000); http.setTimeout(12000);
  int code = http.GET(); s_stage.lastHttp = code;
  if (code != 200) { Serial.printf("[http] ls %d\n", code); http.end(); s_stage.net = Net::HTTP_ERR; return false; }

  JsonDocument filter;
  filter["node"] = true; filter["oracle"] = true;
  JsonObject fs = filter["sessions"].add<JsonObject>();
  fs["name"] = true;
  JsonObject fw = fs["windows"].add<JsonObject>();
  fw["active"] = true;

  JsonDocument doc;
  DeserializationError err = deserializeJson(doc, http.getStream(), DeserializationOption::Filter(filter));
  http.end();
  if (err) { Serial.printf("[json] ls %s\n", err.c_str()); s_stage.net = Net::HTTP_ERR; return false; }

  strlcpy(s_stage.node,   doc["node"]   | "?", sizeof(s_stage.node));
  strlcpy(s_stage.oracle, doc["oracle"] | "?", sizeof(s_stage.oracle));
  s_stage.count = 0;
  for (JsonObject s : doc["sessions"].as<JsonArray>()) {
    if (s_stage.count >= MAXSESS) break;
    Sess& d = s_stage.sess[s_stage.count];
    strlcpy(d.name, s["name"] | "?", sizeof(d.name));
    JsonArray w = s["windows"].as<JsonArray>();
    d.windows = (uint8_t)w.size();
    d.active = false;
    for (JsonObject win : w) if (win["active"] | false) { d.active = true; break; }
    s_stage.count++;
  }
  s_stage.net = Net::OK; s_stage.lastOk = millis();
  Serial.printf("[json] %d sessions\n", s_stage.count);
  return true;
}

// ── Renderers (read g_ui — a lock-free local snapshot — NEVER under the lock) ──
static void drawStale() {
  if (g_ui.net != Net::OK || (g_ui.lastOk && millis() - g_ui.lastOk > 15000)) {
    spr.setTextDatum(textdatum_t::top_right); spr.setTextColor(C_WARN);
    spr.setTextSize(2); spr.drawString("stale", W - 6, 4);
  }
}

static void drawBanner(const char* l1, const char* l2, uint16_t color) {
  spr.fillSprite(C_BG);
  spr.setTextDatum(textdatum_t::middle_center);
  spr.setTextColor(color); spr.setTextSize(3);
  spr.drawString(l1, W/2, Hgt/2 - 24);
  if (l2 && l2[0]) { spr.setTextSize(2); spr.setTextColor(C_IDLE); spr.drawString(l2, W/2, Hgt/2 + 20); }
  axs::push((const uint16_t*)spr.getBuffer());
}

static void renderList() {
  spr.fillSprite(C_BG);
  spr.setTextDatum(textdatum_t::top_left); spr.setTextColor(C_HDR); spr.setTextSize(2);
  char hdr[48]; snprintf(hdr, sizeof(hdr), "%s/%s  %d sess", g_ui.node, g_ui.oracle, g_ui.count);
  spr.drawString(hdr, 6, 4);
  drawStale();
  spr.drawFastHLine(0, HDR - 2, W, C_RULE);
  // clamp the UI-owned scroll to the freshly snapshotted count
  if (g_scroll > g_ui.count - VIS_ROWS) g_scroll = g_ui.count - VIS_ROWS;
  if (g_scroll < 0) g_scroll = 0;
  int last = min(g_scroll + VIS_ROWS, g_ui.count);
  for (int i = g_scroll; i < last; i++) {
    Sess& s = g_ui.sess[i]; int y = HDR + (i - g_scroll) * ROW_H;
    if (s.active) spr.fillCircle(10, y + ROW_H/2, 4, C_DOT);
    spr.setTextSize(2); spr.setTextColor(s.active ? C_ACTIVE : C_IDLE);
    spr.setTextDatum(textdatum_t::middle_left); spr.drawString(s.name, 22, y + ROW_H/2);
    char wc[8]; snprintf(wc, sizeof(wc), "%uw", s.windows);
    spr.setTextDatum(textdatum_t::middle_right); spr.setTextColor(C_WIN);
    spr.drawString(wc, W - 8, y + ROW_H/2);
  }
  if (g_ui.count > VIS_ROWS) {
    int barH = max(8, (Hgt - HDR) * VIS_ROWS / g_ui.count);
    int barY = HDR + (Hgt - HDR) * g_scroll / g_ui.count;
    spr.fillRect(W - 3, barY, 3, barH, C_RULE);
  }
  axs::push((const uint16_t*)spr.getBuffer());
}

static void renderContent() {
  spr.fillSprite(C_BG);
  spr.setTextDatum(textdatum_t::top_left); spr.setTextColor(C_HDR); spr.setTextSize(2);
  spr.drawString("esp32-oracle", 6, 4);
  drawStale();
  // mode hint, top-right just left of the "stale" slot — BIG (size2) or S (size1)
  spr.setTextDatum(textdatum_t::top_right); spr.setTextColor(C_IDLE); spr.setTextSize(1);
  spr.drawString(g_fit ? "BIG" : "S", W - 60, 6);
  spr.drawFastHLine(0, HDR - 2, W, C_RULE);

  // Scale-dependent geometry. Glyph is 6*sz wide, (8|16)*sz... actually the default
  // LovyanGFX font is 6x8 at size 1; line height = 8*sz + 2 gutter.
  const int sz   = g_fit ? 2 : 1;
  const int gw   = 6 * sz;                  // glyph width
  const int LH   = 8 * sz + 2;              // line height incl. gutter
  const int top  = HDR + 2;
  int rows = (Hgt - top) / LH;              // BIG ~28, S ~52 rows
  int cols = (W - 8) / gw;                  // BIG ~26, S ~52 cols
  if (cols > MAXCOL) cols = MAXCOL;
  int start = g_ui.nlines > rows ? g_ui.nlines - rows : 0;   // tail (newest)

  spr.setTextSize(sz); spr.setTextDatum(textdatum_t::top_left);
  char run[MAXCOL + 1];
  int y = top;
  for (int i = start; i < g_ui.nlines; i++) {
    const char*     L  = g_ui.lines[i];
    const uint16_t* CL = g_ui.colr[i];
    int n   = (int)strnlen(L, MAXCOL);
    int vis = n > cols ? cols : n;                // visible glyphs (truncate, never wrap)
    int x = 4, j = 0;
    while (j < vis) {                             // draw runs of identical color
      uint16_t rc = CL[j];
      int k = j; while (k < vis && CL[k] == rc) k++;
      int rl = k - j;
      memcpy(run, L + j, rl); run[rl] = '\0';
      spr.setTextColor(rc);
      spr.drawString(run, x, y);
      x += rl * gw;
      j = k;
    }
    if (n > cols && cols > 0) {                   // line was cut → '>' marker
      spr.setTextColor(C_WARN);
      spr.drawString(">", 4 + (cols - 1) * gw, y);
    }
    y += LH;
  }
  axs::push((const uint16_t*)spr.getBuffer());
}

static void renderCurrent() { (g_mode == Mode::CONTENT) ? renderContent() : renderList(); }

// Snapshots the shared model into g_ui (one short lock), then renders the snapshot.
static void renderState() {
  snapshot(g_ui);
  switch (g_ui.net) {
    case Net::BOOT:      drawBanner("fleet-pulse", "booting...", C_HDR);          break;
    case Net::WIFI_WAIT: drawBanner("WiFi", WIFI_SSID, C_HDR);                    break;
    case Net::WIFI_FAIL:
    case Net::HTTP_ERR: {
      // A transient WiFi/HTTP drop should NOT blank the screen — keep the last-good
      // view with the "stale" flag, and only show a banner if we have nothing yet.
      bool haveData = (g_mode == Mode::CONTENT) ? (g_ui.nlines > 0) : (g_ui.count > 0);
      if (haveData) { renderCurrent(); break; }
      if (g_ui.net == Net::WIFI_FAIL) drawBanner("WiFi", "reconnecting...", C_WARN);
      else { char l2[40]; snprintf(l2, sizeof(l2), "GET %d  retrying", g_ui.lastHttp); drawBanner("BACKEND", l2, C_WARN); }
      break;
    }
    case Net::OK:        renderCurrent();                                         break;
  }
}

// ── netTask — CORE 0: owns WiFi + HTTPClient; the only place a blocking GET runs ─
// Loop: read the UI's requested mode, fetch it (with a single retry that's now
// harmless — it's off the UI thread), publish the staging Model, then pace.
static void netTask(void*) {
  for (;;) {
    Mode want = g_modeReq;                 // single aligned word — safe to read lock-free
    bool ok = (want == Mode::CONTENT) ? netFetchContent() : netFetchSessions();
    if (!ok) {                             // retry once: rides out a transient -5/-11
      vTaskDelay(pdMS_TO_TICKS(150));
      ok = (want == Mode::CONTENT) ? netFetchContent() : netFetchSessions();
    }
    publish(s_stage);                      // one locked memcpy; loop() picks it up next paint
    vTaskDelay(pdMS_TO_TICKS(want == Mode::CONTENT ? 2000 : 5000));
  }
}

void setup() {
  Serial.begin(115200); delay(300);
  Serial.println("\n=== esp32-fleet-pulse === boot (CONTENT stream: " CAP_TARGET ")");
  axs::init(/*warmup=*/false);
  axs::fill_solid(C_BG);
  spr.setColorDepth(16); spr.setPsram(true);
  if (!spr.createSprite(W, Hgt)) Serial.println("[fb] createSprite FAILED — PSRAM?");

  g_lock = xSemaphoreCreateMutex();
  if (!g_lock) Serial.println("[rtos] mutex create FAILED");

  // initial published state
  s_stage.net = Net::BOOT; s_stage.nlines = 0; s_stage.count = 0;
  s_stage.lastHttp = 0; s_stage.lastOk = 0;
  strlcpy(s_stage.node, "?", sizeof(s_stage.node));
  strlcpy(s_stage.oracle, "?", sizeof(s_stage.oracle));
  publish(s_stage);
  renderState();

  wifiBegin();                                       // synchronous in setup — no UI competing yet
  renderState();

  // First fetch synchronously so the screen has data before the task spins up.
  if (WiFi.status() == WL_CONNECTED) {
    g_modeReq = g_mode;
    bool ok = (g_mode == Mode::CONTENT) ? netFetchContent() : netFetchSessions();
    if (!ok) { delay(150); (g_mode == Mode::CONTENT) ? netFetchContent() : netFetchSessions(); }
    publish(s_stage);
  }
  renderState();

  // netTask on CORE 0 (loop()/Arduino runs on CORE 1). 8KB stack: the deepest call
  // is a filtered deserializeJson of the ~16KB capture + HTTPClient. Raise to 12KB
  // if a stack canary / Guru Meditation shows up.
  BaseType_t r = xTaskCreatePinnedToCore(netTask, "net", 8192, nullptr, 1, nullptr, 0);
  Serial.printf("[rtos] netTask create: %s\n", r == pdPASS ? "OK" : "FAIL");
}

void loop() {
  static uint32_t nextPaint  = 0;
  static bool     wasTouched = false;
  static uint32_t lastTap    = 0;

  // Touch every pass — NEVER blocked by a fetch (that's on core 0 now).
  // Edge-triggered: act only on the not-touched -> touched transition, 350ms bounce.
  int16_t tx, ty;
  bool touched = axs::read_touch(&tx, &ty);
  if (touched && !wasTouched && millis() - lastTap > 350) {
    lastTap = millis();
    // Top-right corner tap while in CONTENT → toggle font fit (instant, zero HTTP).
    bool cornerTap = (g_mode == Mode::CONTENT) && (tx > W - 60) && (ty < HDR + 24);
    if (cornerTap) {
      g_fit = !g_fit;
      Serial.printf("[touch] fit -> %s\n", g_fit ? "BIG" : "S");
      renderState();                                 // re-render already-fetched lines
    } else {
      g_mode = (g_mode == Mode::CONTENT) ? Mode::LIST : Mode::CONTENT;
      g_modeReq = g_mode;                            // tell netTask to fetch the new view
      Serial.printf("[touch] -> %s\n", g_mode == Mode::CONTENT ? "CONTENT" : "LIST");
      renderState();                                 // instant feedback with whatever's published
    }
  }
  wasTouched = touched;

  // ~100ms paint timer — pick up whatever netTask last published. No fetch here.
  if (millis() >= nextPaint) {
    nextPaint = millis() + 100;
    renderState();
  }

  delay(20);                                         // pace every path (fleet learning)
}
