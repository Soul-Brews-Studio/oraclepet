#!/bin/sh
# Write tests/parity_vectors.json from the Swift PetProtocol the Mac app ships: the generator is
# the OraclePetParity target in lab/desk-pet-mac. macOS + Swift only; the JSON is committed,
# so the Python tests run anywhere without Swift.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
pkg="$here/../../../../desk-pet-mac"
swift build -q --package-path "$pkg" --product OraclePetParity
"$(swift build --package-path "$pkg" --show-bin-path)/OraclePetParity" > "$here/../parity_vectors.json"
echo "wrote $here/../parity_vectors.json"
