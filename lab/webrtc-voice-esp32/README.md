# webrtc-voice-esp32 — hear the pet in a browser, over WebRTC, on the LAN

> C, ESP-IDF, ESP32-S3: the 2.8in pet, D350. The pet's ES8311 mic is sent as an Opus audio track
> over WebRTC, plus a DataChannel for text. The pet itself is the signaling server: a browser
> opens `https://<pet-ip>/webrtc/test` and talks to it directly. There is no cloud, no TURN and
> no room id.

The base is Espressif's `esp-webrtc-solution` `solutions/doorbell_local` at commit `557dd14`,
the snapshot Neo's lab 13 pinned (laris-co/esp32-p4-eye-labs, S3 camera → WebRTC). The demo code
is Public Domain / CC0. What changes for the pet:

| doorbell_local | this lab |
|---|---|
| camera + H.264 | none: audio only |
| `esp_board_manager` board YAML | the pet's own ES8311 bring-up (`main/mic.c`, as the pet firmware and xiaozhi do) |
| WiFi from `settings.h` | the WiFi the pet got over Improv (NVS `wifi`: `ssid`, `pass`) |
| own partition table | the pet's: this app goes in `ota_0`; the pet stays in `ota_1` |

## Build and flash

```bash
tools/prepare.sh     # esp-webrtc-solution @557dd14 into .work/, demo TLS cert into main/certs/
tools/build.sh       # IDF build
tools/flash.sh       # ota_0 only + otadata erased: this app boots (back: ../esp32-discord-bot-rs/tools/boot-pet.sh)
```

Then open `https://<pet-ip>/webrtc/test` (the log prints the IP). The certificate is the demo's
self-signed one, so the browser warns once.

## Status

- 2026-10-21: **works** on the D350.
  - Chrome on the same LAN pressed Listen, and the call held for 40 s or more.
  - The audio track was live and unmuted: Opus from the ES8311.
  - The data channel worked both ways: browser → pet text, pet → browser echo, plus a status line
    each second.
  - Encryption: ICE to 192.168.1.162, then DTLS-SRTP with ECDHE-ECDSA-AES256.

Three fixes were needed beyond the port:

1. **Chrome hides its LAN IP behind an mDNS name** (`xxxx.local`) in ICE candidates. The ESP32
   cannot resolve it and rejected the browser's checks ("Not found for bind remote"). The
   signaling server swaps in the HTTPS client's real IP (`unmask_mdns` in
   `webrtc_http_server.c`).
2. **The page handled signaling messages concurrently.** The pet's candidates arrived right
   behind its offer, before `setRemoteDescription` finished, and were lost. It now handles them
   one at a time and queues candidates until the offer is applied.
3. **A repeated LISTEN POST restarted a live call.** The pet now ignores LISTEN while a call is
   connecting or up.

---

🤖 Nexus Oracle (Claude, AI) for Nat · laris-co/nexus-oracle#130
