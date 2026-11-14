#!/bin/zsh
# Build with the ESP-IDF on this machine (log: /tmp/issue130/fw/rtc-build.log).
cd "$(dirname "$0")/.."
[ -d .work/esp-webrtc-solution ] || tools/prepare.sh || exit 1
[ -d /tmp/issue130/fw/pyshim ] && export PATH="/tmp/issue130/fw/pyshim:$PATH"
. ~/esp/esp-idf-v6/export.sh >/dev/null 2>&1 || { echo "export.sh failed"; exit 2; }
[ -f sdkconfig ] || idf.py set-target esp32s3 > /tmp/issue130/fw/rtc-set-target.log 2>&1 || { tail -20 /tmp/issue130/fw/rtc-set-target.log; rm -rf build sdkconfig; exit 3; }
idf.py build > /tmp/issue130/fw/rtc-build.log 2>&1
RC=$?
rg --no-filename "error:|undefined reference|FAILED|Project build complete|overflow|CMake Error" /tmp/issue130/fw/rtc-build.log | sed 's|.*/lab/webrtc-voice-esp32/||' | sort -u | head -30
ls -la build/webrtc_voice_lab.bin 2>/dev/null | awk '{print $5, $9}'
exit $RC
