#!/usr/bin/env bash
# broadcast-status — show Claude Code usage on the BLE beacon dashboard.
#
# Reads the JSON Claude Code pipes to its statusline (cached at
# /tmp/claude-statusline/raw.json on every render), packs the fields
# the firmware needs into a 5-part pipe-delimited payload, and
# broadcasts it via ble-beacon.py.
#
# Wire format (after the `oracle:` BLE-prefix):
#
#   <model>|<ctx_used_k>|<q7d_pct>|<reset>
#
#   model      Op | So | Ha           (2-char family code; firmware maps
#                                      Op→1M, So→1M, Ha→200K for ctx_max)
#   ctx_used_k 0..N                   (rounded to thousands)
#   q7d_pct    0..100                 (weekly quota used %)
#   reset      "3d11h" | "2h17m" | …  (humanised resets_at countdown)
#
# Example:        Op|216|39|3d11h        → 15 bytes
# BLE budget:     31-byte adv packet − 3 (Flags AD) − 2 (AD header) = 26 bytes
#                 for the name. `oracle:` eats 7, payload must fit in 19 bytes.
#                 (Tested: 27-byte names get silently dropped by macOS.)
#
# Usage:
#   ./broadcast-status.sh                  # single 60s broadcast
#   BLE_DURATION=300 ./broadcast-status.sh # 5-minute broadcast
#   ./broadcast-status.sh --loop           # rebroadcast every 30s, forever
#
# Sanity:
#   The statusline JSON is shared across ALL Claude Code instances on this
#   machine — every render overwrites `/tmp/claude-statusline/raw.json` with
#   whichever session most-recently rendered. To avoid broadcasting foreign
#   sessions' data we filter on `.session_id`:
#
#   BROADCAST_SESSION=<first-N-chars-of-session-id>   # default: auto-detect
#                                                       latest jsonl in cwd
#
#   We also skip cycles where ctx_used jumps > 50% vs the previous sample
#   within 30s — that's not normal session growth, it's a race.

set -euo pipefail

JSON="${CLAUDE_STATUSLINE_JSON:-/tmp/claude-statusline/raw.json}"
DURATION="${BLE_DURATION:-60}"
LOOP_EVERY="${LOOP_EVERY:-30}"

# Auto-detect the target session from the most recently modified jsonl
# in the current project dir.  Override with BROADCAST_SESSION=...
auto_session() {
  # Walk up from cwd looking for a Claude Code project dir. The dir name is
  # the path with `/` and `.` replaced by `-`. Repo root projects exist, but
  # subdir cwds do not, so we try parents.
  local cwd encoded proj_dir latest fname
  cwd="$(pwd)"
  while [ -n "$cwd" ] && [ "$cwd" != "/" ]; do
    encoded="$(printf '%s' "$cwd" | sed 's|/|-|g; s|\.|-|g')"
    proj_dir="$HOME/.claude/projects/${encoded}"
    if [ -d "$proj_dir" ]; then
      latest="$(ls -t "$proj_dir"/*.jsonl 2>/dev/null | head -1)"
      if [ -n "$latest" ]; then
        fname="${latest##*/}"
        fname="${fname%.jsonl}"
        echo "${fname:0:8}"
        return 0
      fi
    fi
    cwd="${cwd%/*}"  # strip last path segment
  done
  return 0  # don't error if we found nothing; just leave TARGET empty
}

TARGET_SESSION="${BROADCAST_SESSION:-$(auto_session)}"
if [ -n "$TARGET_SESSION" ]; then
  echo "target session: $TARGET_SESSION" >&2
else
  echo "target session: <none — will broadcast any session that writes the JSON>" >&2
fi

mode="once"
[ "${1:-}" = "--loop" ] && mode="loop"

# Sanity-gate state: last accepted used_k value + timestamp
LAST_USED_K=0
LAST_SAMPLE_TS=0

# Map a Claude Code model id or display name to a 2-char family code.
# Firmware lookup table (in beacon-display.yaml) turns this back into a
# friendly "OPUS 4.7" / context-window-size label.
model_code() {
  local m="${1,,}"  # lowercase
  case "$m" in
    *opus*)   echo "Op" ;;
    *sonnet*) echo "So" ;;
    *haiku*)  echo "Ha" ;;
    *)        echo "??" ;;
  esac
}

# resets_at (epoch) → human countdown: "Xd Yh" / "Xh Ym" / "Xm" / "now"
fmt_reset() {
  local secs=$(( $1 - $(date +%s) ))
  if   [ "$secs" -le 0 ];     then echo "now"
  elif [ "$secs" -ge 86400 ]; then echo "$((secs/86400))d$(((secs%86400)/3600))h"
  elif [ "$secs" -ge 3600 ];  then echo "$((secs/3600))h$(((secs%3600)/60))m"
  else                              echo "$((secs/60))m"
  fi
}

format_payload() {
  if [ ! -f "$JSON" ]; then
    echo "??|0|0|n/a"
    return 1
  fi
  # TAB delimiter — model display name contains spaces ("Opus 4.7 (1M context)")
  # so whitespace split via `read` would mangle it.
  local sid_full sid model_raw ctx_used_k q7d_pct q7d_resets
  IFS=$'\t' read -r sid_full model_raw ctx_used_k q7d_pct q7d_resets < <(jq -r '
    [
      (.session_id // ""),
      (.model.display_name // .model.id // "?"),
      (((.context_window.current_usage | ((.input_tokens//0)+(.cache_creation_input_tokens//0)+(.cache_read_input_tokens//0)+(.output_tokens//0))) / 1000) | floor),
      (.rate_limits.seven_day.used_percentage // 0 | floor),
      (.rate_limits.seven_day.resets_at // 0)
    ] | @tsv
  ' "$JSON")
  sid="${sid_full:0:8}"

  # ── Layer A: session filter ────────────────────────────────────
  # If a target session is set and the JSON belongs to a different
  # session, skip this cycle — that's foreign data.
  if [ -n "$TARGET_SESSION" ] && [ "$sid" != "$TARGET_SESSION" ]; then
    echo "skip: foreign session $sid (want $TARGET_SESSION), used=${ctx_used_k}k" >&2
    return 2
  fi

  # ── Layer B: sanity gate (no insane jumps) ─────────────────────
  # ctx_used can only grow within a session (cache reads accumulate).
  # If we see a >50% drop OR a >50% jump within 30s vs the last
  # accepted sample, it's almost certainly a race-condition on the
  # shared JSON file. Skip and warn.
  local now=$(date +%s)
  local elapsed=$(( now - LAST_SAMPLE_TS ))
  if [ "$LAST_USED_K" -gt 0 ] && [ "$elapsed" -lt 30 ] && [ "$elapsed" -gt 0 ]; then
    local delta_abs=$(( ctx_used_k - LAST_USED_K ))
    [ "$delta_abs" -lt 0 ] && delta_abs=$(( -delta_abs ))
    local jump_pct=$(( delta_abs * 100 / LAST_USED_K ))
    if [ "$jump_pct" -gt 50 ]; then
      echo "skip: ctx jumped ${jump_pct}% (${LAST_USED_K}k → ${ctx_used_k}k) in ${elapsed}s — likely race" >&2
      return 3
    fi
  fi
  LAST_USED_K="$ctx_used_k"
  LAST_SAMPLE_TS="$now"

  printf '%s|%s|%s|%s' \
    "$(model_code "$model_raw")" \
    "$ctx_used_k" \
    "$q7d_pct" \
    "$(fmt_reset "$q7d_resets")"
}

broadcast_once() {
  local payload="$1"
  echo "broadcasting: oracle:${payload} (${#payload} chars, ${DURATION}s window)" >&2
  cd "$(dirname "$0")"
  BLE_DURATION="$DURATION" uv run ble-beacon.py "$payload"
}

if [ "$mode" = "once" ]; then
  payload="$(format_payload)" || { echo "format skipped — nothing to broadcast" >&2; exit "$?"; }
  broadcast_once "$payload"
else
  echo "loop mode — refreshing every ${LOOP_EVERY}s. Ctrl-C to stop." >&2
  trap 'echo "loop interrupted" >&2; exit 0' INT TERM
  while :; do
    if payload="$(format_payload)"; then
      BLE_DURATION="$LOOP_EVERY" broadcast_once "$payload" || true
    else
      # Skipped (foreign session or sanity gate). Sleep + retry.
      sleep 5
    fi
  done
fi
