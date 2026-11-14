#!/usr/bin/env bash
# Build the firmware on this machine and publish it to the web flasher, without
# waiting for the CI firmware build.
#
#   tools/site/publish-local.sh              build + upload + redeploy Pages
#   SKIP_BUILD=1 tools/site/publish-local.sh  upload the dist/ you already have
#
# 1. builds dist/ with tools/site/build-firmware.sh (needs ESP-IDF v6.0.3)
# 2. uploads dist/ as firmware.tar.gz to the GitHub release "firmware-latest"
# 3. runs the Pages workflow with firmware=release, which deploys those files
#
# IDF: set IDF_PATH (default ~/esp/esp-idf-v6). If export.sh picks the wrong
# python venv, set IDF_PYTHON_ENV_PATH to the venv ESP-IDF installed.
set -euo pipefail
ROOT="$(cd "$(dirname "$0")/../.." && pwd)"
REPO="${REPO:-Soul-Brews-Studio/oraclepet}"
TAG=firmware-latest
cd "$ROOT"

if [ -z "${SKIP_BUILD:-}" ]; then
  export IDF_PATH="${IDF_PATH:-$HOME/esp/esp-idf-v6}"
  # shellcheck disable=SC1091
  . "$IDF_PATH/export.sh" >/dev/null
  rm -rf dist
  tools/site/build-firmware.sh
fi
[ -f dist/SHA256SUMS ] || { echo "no dist/SHA256SUMS: build first" >&2; exit 1; }

tar -czf /tmp/oraclepet-firmware.tar.gz -C dist .
SRC="$(git rev-parse --short HEAD)$(git diff --quiet || echo -dirty)"
NOTES="Firmware for the web flasher, built locally from $SRC with $(idf.py --version 2>/dev/null || echo ESP-IDF).

$(cat dist/SHA256SUMS)"

if gh release view "$TAG" -R "$REPO" >/dev/null 2>&1; then
  gh release upload "$TAG" /tmp/oraclepet-firmware.tar.gz --clobber -R "$REPO"
  gh release edit "$TAG" --notes "$NOTES" -R "$REPO" >/dev/null
else
  gh release create "$TAG" /tmp/oraclepet-firmware.tar.gz --prerelease \
     --title "Web flasher firmware (latest local build)" --notes "$NOTES" -R "$REPO"
fi

gh workflow run pages.yml -R "$REPO" --ref main -f firmware=release
echo "uploaded $TAG from $SRC; Pages redeploy started:"
echo "  gh run list -R $REPO --workflow pages.yml --limit 1"
