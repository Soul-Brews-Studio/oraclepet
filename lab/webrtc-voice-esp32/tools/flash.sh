#!/bin/zsh
# This app into ota_0 of the 2.8in pet board (the pet stays in ota_1, GIFs and WiFi untouched),
# then otadata erased so ota_0 boots. Back to the pet: ../esp32-discord-bot-rs/tools/boot-pet.sh.
set -e
cd "$(dirname "$0")/.."
MAC="84:c7:bb:78:d3:50"
P=""
for c in /dev/cu.usbmodem*; do esptool --chip esp32s3 --port "$c" read-mac 2>/dev/null | grep -q "$MAC" && P=$c && break; done
[ -n "$P" ] || { echo "the 2.8in board ($MAC) is not on USB"; exit 4; }
esptool --chip esp32s3 --port "$P" -b 460800 write-flash 0x10000 build/webrtc_voice_lab.bin | tail -2
esptool --chip esp32s3 --port "$P" erase-region 0xd000 0x2000 | tail -1
echo "$P"
