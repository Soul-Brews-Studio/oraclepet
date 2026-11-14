#!/bin/zsh
# Boot the pet firmware again (it stayed in ota_1 while the bot ran from ota_0).
set -e
. ~/esp/esp-idf-v6/export.sh >/dev/null 2>&1
MAC="84:c7:bb:78:d3:50"
P=""
for c in /dev/cu.usbmodem*; do esptool --chip esp32s3 --port "$c" read-mac 2>/dev/null | grep -q "$MAC" && P=$c && break; done
[ -n "$P" ] || { echo "the 2.8in board ($MAC) is not on USB"; exit 4; }
python "$IDF_PATH/components/app_update/otatool.py" --port "$P" switch_ota_partition --slot 1
