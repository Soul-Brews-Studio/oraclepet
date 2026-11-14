# esp32-discord-bot-rs — the pet is its own Discord voice bot, in Rust, with DAVE

> Rust (esp-idf std, ESP-IDF 6.0.3), ESP32-S3: the 2.8in pet, D350. The board logs in to Discord
> as a bot, joins a voice channel and streams audio into it. No PC or relay bot is in between.

Since 2026-03-02 Discord closes any voice connection without **DAVE** (end-to-end encryption)
outside Stage channels, with code 4017. See the C lab, `../discord-voice-esp32/README.md`, and
laris-co/nexus-oracle `ψ/writing/2026-10-21_esp32-discord-voice-dave.md`.

This firmware does DAVE with the **`davey`** crate. It is what discord.js uses: MLS (openmls) on
RustCrypto, pure Rust, so it builds for the S3.

## How it works

| file | what |
|---|---|
| `src/ws.rs` | WebSocket on `esp_websocket_client` through esp-idf-sys, frames reassembled, Discord's 4xxx close codes kept (esp-idf-svc's wrapper loses both) |
| `src/gateway.rs` | gateway v10: identify (intent GUILD_VOICE_STATES), heartbeat, op 4 join, voice session and server |
| `src/voice.rs` | voice gateway v8: Ready → UDP IP discovery → Select Protocol → Session Description; heartbeat with `seq_ack`; DAVE ops 21-31; UDP keepalive |
| `src/dave.rs` | the DAVE state machine around `davey::DaveSession`, step by step as @discordjs/voice 0.19.2 |
| `src/crypto.rs` | RTP header + transport AEAD: `aead_aes256_gcm_rtpsize` (preferred), `aead_xchacha20_poly1305_rtpsize` (required) |
| `src/opus.rs` | Opus 16 kHz mono via Espressif's `esp_audio_codec` |
| `src/audio.rs` | M1: a melody made on the chip (4 s on, 1 s off) |

Per 20 ms frame: Opus → DAVE frame encryption (AES-128-GCM, per-sender key) → RTP + transport
AEAD → UDP. The MLS work (key package, commits, welcomes: P-256 and HPKE) runs on the voice thread
(96 KB stack in PSRAM). It happens only when someone joins or leaves, and the log times each step.

## Build, flash, go back to the pet

```bash
DISCORD_BOT_TOKEN=… DISCORD_GUILD_ID=… DISCORD_CHANNEL_ID=… tools/build.sh   # target/bot.bin
tools/flash.sh      # writes ota_0 only and erases otadata: the bot boots
tools/boot-pet.sh   # boots the pet firmware again (ota_1)
```

- **One flash, two firmwares.** The bot uses the pet's own partition table: it lives in `ota_0`.
  The pet firmware stays in `ota_1`, the GIF packs in `storage`, and the WiFi in NVS. The bot reads
  that WiFi (the network the pet got over Improv).
- **The token is never in git.** It is compiled into the binary, which stays in `target/`
  (gitignored), so the binary should be treated like the token. Use a bot application that no
  other running program uses for voice in that server: one bot user holds one voice connection
  per guild.

## Milestones

- **M1**: melody into a normal voice channel with DAVE on: proves gateway, voice, UDP, transport
  AEAD, Opus, and DAVE on the S3.
- **M2**: the pet's mic (ES8311) instead of the melody.
- **M3**: inside the pet firmware, next to BLE and the display.

---

🤖 Nexus Oracle (Claude, AI) for Nat · laris-co/nexus-oracle#130
