#!/bin/zsh
# Build the bot for the 2.8in pet board (D350) against the ESP-IDF on this machine (v6.0.3).
# The Discord settings come from the environment of whoever runs it, never from a file in git:
#   DISCORD_BOT_TOKEN                      the bot's token
#   DISCORD_GUILD_ID, DISCORD_CHANNEL_ID   the server and the voice channel to join
# Output: target/bot.bin, an app image for ota_0 (tools/flash.sh).
set -e
cd "$(dirname "$0")/.."
# espup's environment first (LIBCLANG_PATH for bindgen), then IDF's: IDF 6 checks that its own
# GCC (esp-15.2.0) comes first on PATH, and espup's is esp-16.2.0
. ~/export-esp.sh
# IDF 6's export.sh needs Python 3.13 first on PATH on m5, or it leaves IDF_PATH unset
[ -d /tmp/issue130/fw/pyshim ] && export PATH="/tmp/issue130/fw/pyshim:$PATH"
. ~/esp/esp-idf-v6/export.sh >/dev/null 2>&1 || { echo "export.sh failed"; exit 2; }
[ -n "$IDF_PATH" ] || { echo "export.sh left IDF_PATH unset (python?)"; exit 2; }
export PATH="$HOME/.cargo/bin:$PATH"
[ -n "$DISCORD_BOT_TOKEN" ] || echo "note: DISCORD_BOT_TOKEN is not set: the firmware will only log that"
cargo build --release
ELF=target/xtensa-esp32s3-espidf/release/esp32-discord-bot
espflash save-image --chip esp32s3 --flash-size 8mb "$ELF" target/bot.bin
ls -la target/bot.bin | awk '{print $5, $9}'
