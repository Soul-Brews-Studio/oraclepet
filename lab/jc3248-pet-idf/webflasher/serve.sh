#!/usr/bin/env bash
# serve.sh — local HTTP server for the esp-web-tools flasher page.
# Web Serial requires HTTPS or localhost; localhost satisfies it.
set -euo pipefail
cd "$(dirname "$0")"
[ -f manifest-bufo.json ] || { echo "no manifests — run ./stage.sh first"; exit 1; }
for port in $(seq 8123 8199); do
  if ! lsof -nP -iTCP:"$port" -sTCP:LISTEN >/dev/null 2>&1; then
    echo "→ http://localhost:$port/  (desktop Chrome/Edge/Opera · Ctrl-C to stop)"
    exec python3 -m http.server "$port"
  fi
done
echo "no free port in 8123..8199" >&2; exit 1
