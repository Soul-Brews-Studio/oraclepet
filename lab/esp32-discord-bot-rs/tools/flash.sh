#!/bin/zsh
# Put the bot into ota_0 of the 2.8in pet board over USB and boot it. Nothing else is touched:
# the pet firmware stays in ota_1, the GIF packs in storage, the WiFi in NVS, the bootloader and
# the partition table as they are. Back to the pet: tools/boot-pet.sh.
set -e
cd "$(dirname "$0")/.."
MAC="84:c7:bb:78:d3:50"
P=""
for c in /dev/cu.usbmodem*; do esptool --chip esp32s3 --port "$c" read-mac 2>/dev/null | grep -q "$MAC" && P=$c && break; done
[ -n "$P" ] || { echo "the 2.8in board ($MAC) is not on USB"; exit 4; }
echo "2.8in board on $P"
esptool --chip esp32s3 --port "$P" -b 460800 write-flash 0x10000 target/bot.bin | tail -2
# otadata erased = boot ota_0 (the pet's table: "erased = ota_0")
esptool --chip esp32s3 --port "$P" erase-region 0xd000 0x2000 | tail -1
echo "$P"
