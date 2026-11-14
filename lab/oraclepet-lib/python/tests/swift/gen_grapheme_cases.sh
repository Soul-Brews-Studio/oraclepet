#!/bin/sh
# Re-answer tests/grapheme_cases.json with Swift: for every (msg, who) the file holds, write the
# line Heartbeat.jsonLine() makes, compiled straight from the PetProtocol sources the Mac app
# ships (no package target needed). macOS + swiftc only; the JSON is committed.
set -eu
here=$(cd "$(dirname "$0")" && pwd)
src="$here/../../../../desk-pet-mac/Sources/PetProtocol"
json="$here/../grapheme_cases.json"
t=$(mktemp -d); trap 'rm -rf "$t"' EXIT
cat > "$t/main.swift" <<'EOF'
import Foundation
let url = URL(fileURLWithPath: CommandLine.arguments[1])
var doc = try! JSONSerialization.jsonObject(with: Data(contentsOf: url)) as! [String: Any]
doc["cases"] = (doc["cases"] as! [[String: Any]]).map { c -> [String: Any] in
    var c = c
    let hb = Heartbeat(total: 3, running: 1, waiting: 2, msg: c["msg"] as! String,
                       epoch: 1792547421, tzoff: 25200, who: c["who"] as? String)
    c["line"] = String(decoding: hb.jsonLine(), as: UTF8.self)
    return c
}
try! JSONSerialization.data(withJSONObject: doc, options: [.prettyPrinted, .sortedKeys]).write(to: url)
EOF
swiftc -O -module-name PP -o "$t/gen" "$src/OTA.swift" "$src/PetSound.swift" "$src/Heartbeat.swift" "$t/main.swift"
"$t/gen" "$json"
echo "rewrote $json"
