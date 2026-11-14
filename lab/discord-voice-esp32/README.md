# discord-voice-esp32 — the pet joins a Discord voice room by itself

> Lab, C, ESP-IDF 6.0.3, ESP32-S3 (the 2.8in pet, D350). Goal: the ESP32 is the Discord bot —
> its own gateway connection, its own voice connection — and streams its microphone straight
> into a voice channel, with no Mac or bot server in between.

## Milestones

> **Verdict of the deep research (2026-10-21), see "DAVE" below:** since 2026-03-02 Discord closes
> every non-DAVE voice connection outside Stage channels with **4017**, bots included. This lab
> without DAVE can therefore only speak in a **Stage channel**, the one documented exemption, which
> nobody has tested. The recommended production path is a relay (R1).

| # | What | Proves |
|---|---|---|
| **M1** | play a **test tone** (generated on the chip) in a **Stage channel**. In a normal voice channel this ends with 4017 | gateway, voice gateway v8, UDP + IP discovery, AES-256-GCM, Opus, RTP timing |
| M2 | stream the **pet's mic** instead of the tone (ES8311, 16 kHz) with the VAD from the pet firmware | the real audio path; speaking on/off |
| M3 | move the component into the pet firmware (`jc3248-pet-idf`), next to WiFi voice and BLE | memory with everything on (PSRAM stacks, see the pet's `voice.c`) |
| M4 | **DAVE** on the S3: the Rust `davey` crate cross-compiled. Untested by anyone | normal voice channels, directly |
| **R1** | **relay**: the pet's mic already streams to the m5 voice server; a DAVE-capable bot there (discord.js + davey) joins and plays it | works today; the token stays off the device |

## The wire protocol (from `@discordjs/voice` 0.19.2 on m5, the library the fleet's Discord bots run)

1. **Gateway** `wss://gateway.discord.gg/?v=10&encoding=json` — op 10 Hello → op 2 Identify
   (`intents: 128` = GUILD_VOICE_STATES) → op 0 `READY` (our user id) → op 4 Voice State Update
   `{guild_id, channel_id, self_mute:false, self_deaf:true}` → events `VOICE_STATE_UPDATE`
   (voice `session_id`) and `VOICE_SERVER_UPDATE` (`token`, `endpoint`). Heartbeat op 1 with the last `s`.
2. **Voice gateway** `wss://<endpoint>/?v=8` — op 0 Identify
   `{server_id, user_id, session_id, token, max_dave_protocol_version: 0}` → op 8 Hello
   (`heartbeat_interval`) → op 2 Ready `{ssrc, ip, port, modes}`. Heartbeat op 3 `{t, seq_ack}`.
3. **UDP** to `ip:port`: IP discovery = 74 bytes `[u16 1][u16 70][u32 ssrc][66 zero]`; the reply has
   our public address as a C string at byte 8 and the port (BE) in the last 2 bytes.
4. op 1 Select Protocol `{protocol:"udp", data:{address, port, mode:"aead_aes256_gcm_rtpsize"}}` →
   op 4 Session Description `{mode, secret_key[32], dave_protocol_version}` → op 5 Speaking
   `{speaking:1, delay:0, ssrc}`.
5. **Audio**, every 20 ms: RTP header `80 78 | seq BE16 | timestamp BE32 (+960 per frame) | ssrc BE32`,
   then AES-256-GCM of the Opus frame with **nonce** = 4-byte BE counter (incremented before each packet)
   + 8 zero bytes, **AAD** = the 12-byte header; packet = `header ‖ ciphertext ‖ tag(16) ‖ counter(4)`.
   Five silence frames `F8 FF FE` before stopping.

mbedTLS in IDF 6 is 4.1.1 on TF-PSA-Crypto (no `gcm.h`): AES-GCM goes through `psa_aead_encrypt`,
which uses the S3's AES hardware. Opus: Espressif `esp_audio_codec` (the encoder xiaozhi runs on this
same board: 16 kHz mono, complexity 0, VBR, DTX).

## DAVE

This lab identifies with `max_dave_protocol_version` = 0. If the Session Description says
`dave_protocol_version > 0`, or the voice gateway closes with 4017, the lab stops and reports
`DV_EVT_DAVE_REQUIRED`.

What the deep research of 2026-10-21 found: 23 sources, and 19 of 25 claims survived a 3-vote
adversarial check. The full report is in laris-co/nexus-oracle
`ψ/writing/2026-10-21_esp32-discord-voice-dave.md`.

- **DAVE is mandatory for bots outside Stage channels** and has been enforced since 2026-03-02.
  The opcodes table reads "4017 | E2EE/DAVE protocol required". Help Center article 38749827197591
  (updated 2026-10-20) says the requirement covers "non-stages voice calls". Bots on discordgo, JDA,
  songbird, discord.py and nostrum were refused with 4017 from 2026-03-03, and each was fixed only
  by adding DAVE.
- **DAVE v1 makes every member a committing member of an MLS group.** MLS is RFC 9420, ciphersuite
  2: P-256, AES-128-GCM, SHA-256. The MLS messages travel as binary voice-WebSocket frames (ops
  21 to 31). Each Opus frame is also encrypted with AES-128-GCM and gets a footer of about 12 to
  16 bytes, before the transport AEAD.
- **Transport encryption:** `aead_xchacha20_poly1305_rtpsize` is *Required* and
  `aead_aes256_gcm_rtpsize` is *Preferred*. This lab speaks only AES-GCM, which Discord offered in
  every 2026 observation. XChaCha20 would come from espressif/libsodium, or from a hand-written
  HChaCha20 over ChaCha20-Poly1305, as D++ does.
- **No embedded library carries voice.** esp-discord 2.0.1 never sends op 4. Concord's voice module
  never opened UDP. D++ has the whole path including DAVE, but it is C++ on OpenSSL, mlspp and
  epoll: a reference, or a Raspberry Pi relay, not an ESP32 SDK.
- **The Developer Terms** have no rule about voice or hardware. §2(d): store tokens encrypted and
  never in open-source code. Guideline #14: use a bot token, never a user's.

M4 would be the Rust `davey` crate (0.1.5, 2026-10-12): openmls 0.8.1, openmls_rust_crypto,
p256, aes/ctr/ghash, hmac, sha2, scrypt. It is all pure Rust, and the napi/pyo3 bindings are optional.
It would be built as a static library with a small C ABI and linked into this C firmware.
`esp-idf-sys` 0.38.0 (2026-09-15) added basic ESP-IDF 6.0 support, so Rust can target the same
IDF. On m5: the `esp` toolchain (espup 0.18.0), espflash 4.6.0, ldproxy.

## Status (2026-10-21)

- M1 code is complete: `dv_ws.c` (TLS + WebSocket on tcp_transport), `dv_gateway.c`,
  `dv_voice.c`, and a melody generator in `main.c`.
- It builds with IDF 6.0.3: a 1.1 MB app, with no warnings in the lab's code.
- It has not run against Discord yet: that needs a bot token in menuconfig.
- With the menuconfig strings still empty, `app_main` returns before WiFi starts. The compiler then
  drops the whole voice path and the app is only 188 KB. Judge the size with the settings filled in.

What the log says when Discord hangs up (the close code and reason are logged):

| code | where | meaning | the lab |
|---|---|---|---|
| 4004 | gateway | bad token | tries again every 10 min |
| 4013 / 4014 | gateway | invalid / disallowed intents | tries again every 10 min |
| 4006 / 4014 | voice | session invalid / kicked or moved | waits for new credentials from the gateway |
| 4015 | voice | voice server crashed | reconnects, up to 3 times |
| 4017 | voice | DAVE (E2EE) required | `DV_EVT_DAVE_REQUIRED`, stops |

## Configure and build

The bot token, WiFi password, guild and channel live **only** in `sdkconfig` (gitignored), set with
`idf.py menuconfig` → *OraclePet Discord voice lab*. Never commit them.

```bash
. ~/esp/esp-idf-v6/export.sh
idf.py set-target esp32s3        # once
idf.py menuconfig                # WiFi, bot token, guild id, voice channel id
idf.py build flash monitor
```

The bot needs: the Connect and Speak permissions in that channel, and to be in the guild
(OAuth2 URL with `bot` scope). Use a separate bot application for the pet: one bot user can hold only
one voice connection per guild, so sharing the token with another running bot would fight over it.

---

🤖 Nexus Oracle (Claude, AI) for Nat · laris-co/nexus-oracle#130
