#!/usr/bin/env bash
set -euo pipefail

# Capture the three-screen desk setup plus a small AppleScript context note.
# Usage:
#   scripts/capture-three-screens.sh [output-dir]
#
# Defaults to /tmp so it can be run during a flash/debug session without
# changing the worktree. The manifest records display metadata, frontmost app,
# and the exact capture paths.

stamp="$(date '+%Y-%m-%d_%H%M%S')"
out_dir="${1:-/tmp/jc_flash_trace/screen-captures/$stamp}"
mkdir -p "$out_dir"

manifest="$out_dir/manifest.txt"
: > "$manifest"

{
  echo "capture_started: $(date '+%Y-%m-%d %H:%M:%S %Z')"
  echo "output_dir: $out_dir"
  echo
  echo "== displays =="
} >> "$manifest"

system_profiler SPDisplaysDataType >> "$manifest" 2>&1 || true

{
  echo
  echo "== applescript-context =="
  printf "frontmost: "
} >> "$manifest"

osascript -e 'tell application "System Events" to get the name of every process whose frontmost is true' \
  >> "$manifest" 2>&1 || true

{
  printf "frontmost_windows: "
} >> "$manifest"

osascript -e 'tell application "System Events"
  set frontApp to first application process whose frontmost is true
  get name of every window of frontApp
end tell' \
  >> "$manifest" 2>&1 || true

{
  echo
  echo "== screenshots =="
} >> "$manifest"

for display_id in 1 2 3; do
  png="$out_dir/display-$display_id.png"
  err="$out_dir/display-$display_id.err"
  if screencapture -x -D "$display_id" "$png" 2>"$err"; then
    rm -f "$err"
    file "$png" | tee -a "$manifest"
    if command -v sips >/dev/null 2>&1; then
      preview="$out_dir/display-$display_id.preview.png"
      cp "$png" "$preview"
      sips -Z 1200 "$preview" >/dev/null 2>&1 || true
      file "$preview" | tee -a "$manifest"
    fi
  else
    echo "display-$display_id: capture failed: $(cat "$err")" | tee -a "$manifest"
  fi
done

echo "capture_finished: $(date '+%Y-%m-%d %H:%M:%S %Z')" >> "$manifest"
echo "$out_dir"
