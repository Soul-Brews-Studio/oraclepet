#!/usr/bin/env bash
# Regenerate golden/vectors.json from the Swift PetProtocol (the reference implementation):
# the generator is the OraclePetVectors target in lab/desk-pet-mac (Sources/OraclePetVectors).
set -euo pipefail
cd "$(dirname "$0")/.."
swift build -q --package-path ../desk-pet-mac --product OraclePetVectors
"$(swift build --package-path ../desk-pet-mac --show-bin-path)/OraclePetVectors" > golden/vectors.json
echo "golden/vectors.json: $(wc -c < golden/vectors.json | tr -d ' ') bytes"
