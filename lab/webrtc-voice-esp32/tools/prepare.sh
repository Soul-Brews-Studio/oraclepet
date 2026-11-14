#!/bin/zsh
# Fetch Espressif's esp-webrtc-solution at the commit Neo's lab 13 pinned (557dd14) into .work/,
# and copy doorbell_local's self-signed demo certificate for the local HTTPS signaling server.
set -e
cd "$(dirname "$0")/.."
SHA=557dd14b8dbc69d57f102c7455c16d636026dc2a
if [ ! -d .work/esp-webrtc-solution ]; then
  git clone -q https://github.com/espressif/esp-webrtc-solution.git .work/esp-webrtc-solution
fi
git -C .work/esp-webrtc-solution checkout -q $SHA
mkdir -p main/certs
for f in servercert.pem prvtkey.pem; do
  git -C .work/esp-webrtc-solution show ${SHA}:solutions/doorbell_local/main/certs/$f > main/certs/$f
done
echo "esp-webrtc-solution at $(git -C .work/esp-webrtc-solution log --oneline -1)"
