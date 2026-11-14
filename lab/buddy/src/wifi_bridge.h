#pragma once
#if defined(BOARD_ATD35_S3) || defined(BOARD_LILYGO_EPD47)

#include <WiFi.h>
#include <Preferences.h>
#include <Arduino.h>

#define WIFI_BRIDGE_PORT 7654

static WiFiServer _wifiServer(WIFI_BRIDGE_PORT);
static WiFiClient _wifiClient;
static bool _wifiUp = false;

static char _wifiSSID[33] = "";
static char _wifiPass[65] = "";
static uint32_t _wifiLastAttempt = 0;

inline void wifiLoadCredentials() {
  Preferences p;
  p.begin("wifi", true);
  String ssid = p.getString("ssid", "");
  String pass = p.getString("pass", "");
  p.end();
  strncpy(_wifiSSID, ssid.c_str(), sizeof(_wifiSSID) - 1);
  strncpy(_wifiPass, pass.c_str(), sizeof(_wifiPass) - 1);
}

inline void wifiSaveCredentials(const char* ssid, const char* pass) {
  strncpy(_wifiSSID, ssid, sizeof(_wifiSSID) - 1);
  strncpy(_wifiPass, pass, sizeof(_wifiPass) - 1);
  Preferences p;
  p.begin("wifi", false);
  p.putString("ssid", ssid);
  p.putString("pass", pass);
  p.end();
  Serial.printf("[wifi] credentials saved: ssid='%s'\n", ssid);
}

inline void wifiInit() {
  if (_wifiSSID[0] == 0) return;
  WiFi.mode(WIFI_STA);
  WiFi.begin(_wifiSSID, _wifiPass);
  _wifiLastAttempt = millis();
  Serial.printf("[wifi] connecting to '%s'...\n", _wifiSSID);
}

inline bool wifiConnected() { return _wifiUp && WiFi.status() == WL_CONNECTED; }
inline bool wifiClientConnected() { return _wifiClient && _wifiClient.connected(); }

inline IPAddress wifiLocalIP() { return WiFi.localIP(); }

inline void wifiPoll() {
  if (_wifiSSID[0] == 0) return;

  if (WiFi.status() == WL_CONNECTED) {
    if (!_wifiUp) {
      _wifiUp = true;
      _wifiServer.begin();
      Serial.printf("[wifi] connected! IP=%s port=%d\n",
                    WiFi.localIP().toString().c_str(), WIFI_BRIDGE_PORT);
    }

    if (_wifiServer.hasClient()) {
      if (_wifiClient && _wifiClient.connected()) {
        _wifiClient.stop();
      }
      _wifiClient = _wifiServer.available();
      Serial.println("[wifi] client connected");
    }
  } else {
    if (_wifiUp) {
      _wifiUp = false;
      Serial.println("[wifi] disconnected");
    }
    if (millis() - _wifiLastAttempt > 30000) {
      WiFi.begin(_wifiSSID, _wifiPass);
      _wifiLastAttempt = millis();
      Serial.println("[wifi] reconnecting...");
    }
  }
}

inline size_t wifiAvailable() {
  if (!_wifiClient || !_wifiClient.connected()) return 0;
  return _wifiClient.available();
}

inline int wifiRead() {
  if (!_wifiClient || !_wifiClient.connected()) return -1;
  return _wifiClient.read();
}

inline size_t wifiWrite(const uint8_t* data, size_t len) {
  if (!_wifiClient || !_wifiClient.connected()) return 0;
  return _wifiClient.write(data, len);
}

// Handle {"wifi_set":{"ssid":"...","pass":"..."}} from Serial/BLE
inline bool wifiHandleCommand(JsonDocument& doc) {
  JsonObject ws = doc["wifi_set"];
  if (ws.isNull()) return false;
  const char* ssid = ws["ssid"];
  const char* pass = ws["pass"];
  if (ssid && ssid[0]) {
    wifiSaveCredentials(ssid, pass ? pass : "");
    wifiInit();
  }
  return true;
}

#endif
