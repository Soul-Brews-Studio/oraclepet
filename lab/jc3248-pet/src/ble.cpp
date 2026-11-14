#include "ble.h"

#include <Arduino.h>
#include <ArduinoJson.h>
#include <BLEDevice.h>
#include <BLEServer.h>
#include <BLEUtils.h>
#include <string.h>

// ---------------------------------------------------------------------------
// NUS UUIDs (de-facto serial-over-BLE standard).
// ---------------------------------------------------------------------------
#define NUS_SERVICE_UUID "6e400001-b5a3-f393-e0a9-e50e24dcca9e"
#define NUS_RX_UUID      "6e400002-b5a3-f393-e0a9-e50e24dcca9e"  // client → device (WRITE)
#define NUS_TX_UUID      "6e400003-b5a3-f393-e0a9-e50e24dcca9e"  // device → client (NOTIFY)

// ---------------------------------------------------------------------------
// Shared RX byte buffer.
//
// The RX onWrite callback runs on the BLE task and only appends raw bytes here,
// guarded by a portMUX_TYPE critical section. blePoll() (main thread) drains
// the buffer under the same lock, then scans/parses lines OUTSIDE the lock so
// ArduinoJson never runs on the BLE callback thread.
//
// Cap at 1KB; a single heartbeat line is well under that. On overflow (a peer
// that never sends '\n') we reset the buffer rather than block the BLE task.
// ---------------------------------------------------------------------------
static const size_t       RX_CAP = 1024;
static portMUX_TYPE       rxMux  = portMUX_INITIALIZER_UNLOCKED;
static uint8_t            rxBuf[RX_CAP];
static volatile size_t    rxLen  = 0;

// Line-assembly buffer owned exclusively by blePoll() / the main thread.
static char               lineBuf[RX_CAP];
static size_t             lineLen = 0;

static BLEServer*         server    = nullptr;
static BLECharacteristic* txChar    = nullptr;  // declared for protocol completeness / future TX
static BLECharacteristic* rxChar    = nullptr;
static volatile bool      connected = false;
static bool               firstSnapshotLogged = false;

// ---------------------------------------------------------------------------
// RX callback — BLE task. Keep tiny: append bytes under the critical section.
// ---------------------------------------------------------------------------
class RxCallbacks : public BLECharacteristicCallbacks {
  void onWrite(BLECharacteristic* c) override {
    String v = c->getValue();
    size_t n = v.length();
    if (n == 0) return;
    const uint8_t* p = (const uint8_t*)v.c_str();
    portENTER_CRITICAL(&rxMux);
    for (size_t i = 0; i < n; i++) {
      if (rxLen >= RX_CAP) { rxLen = 0; }  // overflow: drop what we have, resync on next '\n'
      rxBuf[rxLen++] = p[i];
    }
    portEXIT_CRITICAL(&rxMux);
  }
};

// ---------------------------------------------------------------------------
// Server callbacks — track connection state, re-advertise on disconnect.
// ---------------------------------------------------------------------------
class ServerCallbacks : public BLEServerCallbacks {
  void onConnect(BLEServer* s) override {
    connected = true;
    Serial.println("[ble] connected");
  }
  void onDisconnect(BLEServer* s) override {
    connected = false;
    Serial.println("[ble] disconnected");
    BLEDevice::startAdvertising();
  }
};

void bleInit(const char* deviceName) {
  BLEDevice::init(deviceName);
  BLEDevice::setMTU(517);

  server = BLEDevice::createServer();
  server->setCallbacks(new ServerCallbacks());

  BLEService* svc = server->createService(NUS_SERVICE_UUID);

  // TX: open notify (+ read). NimBLE auto-adds the 2902 CCCD for NOTIFY chars,
  // so no manual BLE2902 / encrypted descriptor. No bonding required for v1.
  txChar = svc->createCharacteristic(
    NUS_TX_UUID,
    BLECharacteristic::PROPERTY_READ | BLECharacteristic::PROPERTY_NOTIFY
  );

  // RX: open write (with and without response). No encryption.
  rxChar = svc->createCharacteristic(
    NUS_RX_UUID,
    BLECharacteristic::PROPERTY_WRITE | BLECharacteristic::PROPERTY_WRITE_NR
  );
  rxChar->setCallbacks(new RxCallbacks());

  svc->start();

  BLEAdvertising* adv = BLEDevice::getAdvertising();
  adv->addServiceUUID(NUS_SERVICE_UUID);
  adv->setScanResponse(true);
  adv->setMinPreferred(0x06);
  adv->setMaxPreferred(0x12);
  BLEDevice::startAdvertising();
  Serial.printf("[ble] advertising as '%s'\n", deviceName);
}

bool     bleConnected() { return connected; }
uint32_t blePasskey()   { return 0; }  // open chars, no pairing — always 0

// ---------------------------------------------------------------------------
// Parse one '\n'-terminated JSON line into *out. Heartbeat fields per
// REFERENCE.md: total/running/waiting/tokens/msg + optional prompt{tool,hint}.
// Runs on the calling (main) thread.
// ---------------------------------------------------------------------------
static bool parseLine(const char* line, BleSnapshot* out) {
  JsonDocument doc;
  if (deserializeJson(doc, line)) return false;  // not valid JSON — ignore

  out->total   = doc["total"]   | 0;
  out->running = doc["running"] | 0;
  out->waiting = doc["waiting"] | 0;
  out->tokens  = doc["tokens"]  | 0L;
  out->reset5h = doc["reset5h"] | -1L;   // seconds until 5h usage window resets (optional)
  out->reset7d = doc["reset7d"] | -1L;   // seconds until 7d usage window resets (optional)
  out->pct5h   = doc["pct5h"]   | -1;    // 5h window usage percent (optional)
  out->pct7d   = doc["pct7d"]   | -1;    // 7d window usage percent (optional)
  out->brightness = doc["brightness"] | -1;  // backlight 0..255 from the app (optional)

  const char* m = doc["msg"];
  if (m) strlcpy(out->msg, m, sizeof(out->msg));
  else   out->msg[0] = 0;

  JsonObject pr = doc["prompt"];
  if (!pr.isNull()) {
    const char* tool = pr["tool"] | "";
    const char* hint = pr["hint"] | "";
    snprintf(out->prompt, sizeof(out->prompt), "%s: %s", tool, hint);
  } else {
    out->prompt[0] = 0;
  }

  if (!firstSnapshotLogged) {
    firstSnapshotLogged = true;
    Serial.printf("[ble] snapshot total=%d running=%d waiting=%d tokens=%ld\n",
                  out->total, out->running, out->waiting, out->tokens);
  }
  return true;
}

bool blePoll(BleSnapshot* out) {
  // Drain whatever the RX callback has buffered (under lock, copy only).
  uint8_t  scratch[RX_CAP];
  size_t   n;
  portENTER_CRITICAL(&rxMux);
  n = rxLen;
  if (n > 0) memcpy(scratch, rxBuf, n);
  rxLen = 0;
  portEXIT_CRITICAL(&rxMux);

  // Append into the line-assembly buffer (main-thread owned, no lock needed).
  for (size_t i = 0; i < n; i++) {
    if (lineLen >= sizeof(lineBuf) - 1) lineLen = 0;  // overflow: drop partial line
    lineBuf[lineLen++] = (char)scratch[i];
  }

  // Scan for complete '\n'-terminated lines. Parse each; keep the last full
  // snapshot. If multiple arrived this call, *out reflects the newest.
  bool got = false;
  size_t start = 0;
  for (size_t i = 0; i < lineLen; i++) {
    char c = lineBuf[i];
    if (c == '\n' || c == '\r') {
      size_t segLen = i - start;
      if (segLen > 0) {
        char tmp[RX_CAP];
        if (segLen > sizeof(tmp) - 1) segLen = sizeof(tmp) - 1;
        memcpy(tmp, &lineBuf[start], segLen);
        tmp[segLen] = 0;
        if (tmp[0] == '{') {
          if (parseLine(tmp, out)) got = true;
        }
      }
      start = i + 1;
    }
  }

  // Preserve the unterminated remainder for the next call.
  if (start > 0) {
    size_t rem = lineLen - start;
    if (rem > 0) memmove(lineBuf, &lineBuf[start], rem);
    lineLen = rem;
  }

  return got;
}
