//! Formatting helpers: model code mapping, ctx_max lookup, reset humanisation.

use crate::types::Model;

/// Map a model display name or id to a 2-char family code.
/// Matches the firmware's lookup table.
pub fn model_code(model: &Model) -> &'static str {
    let name = model
        .display_name
        .as_deref()
        .or(model.id.as_deref())
        .unwrap_or("");
    let lower = name.to_ascii_lowercase();
    if lower.contains("opus") {
        "Op"
    } else if lower.contains("sonnet") {
        "So"
    } else if lower.contains("haiku") {
        "Ha"
    } else {
        "??"
    }
}

/// Context-window size in K tokens for a given model code.
/// Mirrors the firmware lookup; default is 200K (haiku-class) for safety.
pub fn model_max_k(code: &str) -> u64 {
    match code {
        "Op" | "So" => 1000,
        "Ha" => 200,
        _ => 200,
    }
}

/// Parse the major.minor version out of a model name and pack it into one
/// byte: `major << 4 | minor`. Accepts both the display form
/// ("Opus 4.8 (1M context)" → 0x48) and the id form with dash separators
/// ("claude-opus-4-8" → 0x48, "claude-haiku-4-5-20251001" → 0x45) by taking
/// the first `digit <sep> digit` where sep is '.' or '-'.
///
/// Sent over the wire so the device shows the live version without a
/// firmware reflash when the model bumps (4.8 → 4.9 → 5.0 …).
/// Returns 0 if no version pattern is found.
pub fn parse_model_version(name: &str) -> u8 {
    let bytes = name.as_bytes();
    for i in 1..bytes.len().saturating_sub(1) {
        if bytes[i] == b'.' || bytes[i] == b'-' {
            let maj = bytes[i - 1];
            let min = bytes[i + 1];
            if maj.is_ascii_digit() && min.is_ascii_digit() {
                let major = (maj - b'0') & 0x0F;
                let minor = (min - b'0') & 0x0F;
                return (major << 4) | minor;
            }
        }
    }
    0
}

/// Render a packed version byte back to "M.N", or "" if unknown (0).
pub fn fmt_model_version(packed: u8) -> String {
    if packed == 0 {
        String::new()
    } else {
        format!("{}.{}", packed >> 4, packed & 0x0F)
    }
}

/// Family name for a model code — the part that doesn't change between
/// versions. The version number rides alongside as a separate byte.
pub fn model_family(code: &str) -> &'static str {
    match code {
        "Op" => "OPUS",
        "So" => "SONNET",
        "Ha" => "HAIKU",
        _ => "MODEL",
    }
}

/// Shorten a repo label for the BLE wire's 8-byte project field by
/// dropping suffixes that every sibling shares (so they don't help
/// distinguish projects): `-oracle`, `-app`. The freed characters let
/// the meaningful stem survive truncation.
///   "esp32-oracle"      → "esp32"
///   "timekeeper-oracle" → "timekeeper"  (caller still truncates to 8)
///   "digger-app"        → "digger"
///   "co-homelab"        → "co-homelab"  (no shared suffix; unchanged)
pub fn short_project(label: &str) -> String {
    for suffix in ["-oracle", "-app"] {
        if let Some(stem) = label.strip_suffix(suffix)
            && !stem.is_empty()
        {
            return stem.to_string();
        }
    }
    label.to_string()
}

/// Humanise a Unix epoch reset time as "Xd Yh" / "Xh Ym" / "Xm" / "now".
pub fn fmt_reset(resets_at: i64, now: u64) -> String {
    let secs = resets_at - (now as i64);
    if secs <= 0 {
        "now".into()
    } else if secs >= 86400 {
        format!("{}d{}h", secs / 86400, (secs % 86400) / 3600)
    } else if secs >= 3600 {
        format!("{}h{}m", secs / 3600, (secs % 3600) / 60)
    } else {
        format!("{}m", secs / 60)
    }
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::types::Model;

    fn m(name: Option<&str>, id: Option<&str>) -> Model {
        Model {
            display_name: name.map(String::from),
            id: id.map(String::from),
        }
    }

    #[test]
    fn model_code_maps_opus_family() {
        assert_eq!(model_code(&m(Some("Opus 4.7 (1M context)"), None)), "Op");
        assert_eq!(model_code(&m(Some("OPUS"), None)), "Op");
        assert_eq!(model_code(&m(None, Some("claude-opus-4-7"))), "Op");
    }

    #[test]
    fn model_code_maps_sonnet_family() {
        assert_eq!(model_code(&m(Some("Sonnet 4.6"), None)), "So");
        assert_eq!(model_code(&m(None, Some("claude-sonnet-4-6"))), "So");
    }

    #[test]
    fn model_code_maps_haiku_family() {
        assert_eq!(model_code(&m(Some("Haiku 4.5"), None)), "Ha");
        assert_eq!(model_code(&m(None, Some("claude-haiku-4-5"))), "Ha");
    }

    #[test]
    fn model_code_returns_unknown_for_others() {
        assert_eq!(model_code(&m(Some("GPT-9"), None)), "??");
        assert_eq!(model_code(&m(None, Some("gemini-2"))), "??");
        assert_eq!(model_code(&m(None, None)), "??");
        assert_eq!(model_code(&m(Some(""), None)), "??");
    }

    #[test]
    fn model_code_prefers_display_name_over_id() {
        let m = m(Some("Opus 4.7"), Some("claude-haiku-4-5"));
        assert_eq!(model_code(&m), "Op");
    }

    #[test]
    fn model_max_k_maps_correctly() {
        assert_eq!(model_max_k("Op"), 1000);
        assert_eq!(model_max_k("So"), 1000);
        assert_eq!(model_max_k("Ha"), 200);
        assert_eq!(model_max_k("??"), 200);
        assert_eq!(model_max_k(""), 200);
        assert_eq!(model_max_k("Garbage"), 200);
    }

    #[test]
    fn parse_model_version_dot_form() {
        assert_eq!(parse_model_version("Opus 4.8 (1M context)"), 0x48);
        assert_eq!(parse_model_version("Sonnet 4.6"), 0x46);
        assert_eq!(parse_model_version("Haiku 4.5"), 0x45);
        assert_eq!(parse_model_version("Opus 5.0"), 0x50);
    }

    #[test]
    fn parse_model_version_dash_id_form() {
        // model id from .jsonl uses dashes
        assert_eq!(parse_model_version("claude-opus-4-8"), 0x48);
        assert_eq!(parse_model_version("claude-sonnet-4-6"), 0x46);
        // date suffix must not win — first N-M is the version
        assert_eq!(parse_model_version("claude-haiku-4-5-20251001"), 0x45);
    }

    #[test]
    fn parse_model_version_zero_when_no_version() {
        assert_eq!(parse_model_version("Opus"), 0);
        assert_eq!(parse_model_version(""), 0);
        assert_eq!(parse_model_version("no version here"), 0);
        assert_eq!(parse_model_version("4."), 0);
        assert_eq!(parse_model_version(".8"), 0);
    }

    #[test]
    fn parse_model_version_takes_first_match() {
        // "1M context" has no digit-sep-digit; first valid is 4.8
        assert_eq!(parse_model_version("Opus 4.8 (1M context)"), 0x48);
    }

    #[test]
    fn fmt_model_version_round_trips() {
        assert_eq!(fmt_model_version(0x48), "4.8");
        assert_eq!(fmt_model_version(0x46), "4.6");
        assert_eq!(fmt_model_version(0x50), "5.0");
        assert_eq!(fmt_model_version(0), "");
    }

    #[test]
    fn model_family_maps_codes() {
        assert_eq!(model_family("Op"), "OPUS");
        assert_eq!(model_family("So"), "SONNET");
        assert_eq!(model_family("Ha"), "HAIKU");
        assert_eq!(model_family("??"), "MODEL");
        assert_eq!(model_family(""), "MODEL");
    }

    #[test]
    fn short_project_strips_shared_suffixes() {
        assert_eq!(short_project("esp32-oracle"), "esp32");
        assert_eq!(short_project("timekeeper-oracle"), "timekeeper");
        assert_eq!(short_project("xiaoer-oracle"), "xiaoer");
        assert_eq!(short_project("digger-app"), "digger");
    }

    #[test]
    fn short_project_leaves_unsuffixed_unchanged() {
        assert_eq!(short_project("co-homelab"), "co-homelab");
        assert_eq!(short_project("1-features"), "1-features");
        assert_eq!(short_project(""), "");
    }

    #[test]
    fn short_project_never_empties() {
        // a label that is exactly the suffix keeps itself (stem would be empty)
        assert_eq!(short_project("-oracle"), "-oracle");
    }

    #[test]
    fn fmt_reset_days_and_hours() {
        // 3 days, 11 hours
        let target = 1_000_000 + (3 * 86400) + (11 * 3600);
        assert_eq!(fmt_reset(target, 1_000_000), "3d11h");
    }

    #[test]
    fn fmt_reset_hours_and_minutes() {
        // 1 hour, 1 minute, 1 second
        let target = 1_000_000 + 3600 + 60 + 1;
        assert_eq!(fmt_reset(target, 1_000_000), "1h1m");
    }

    #[test]
    fn fmt_reset_minutes_only() {
        let target = 1_000_000 + 600;
        assert_eq!(fmt_reset(target, 1_000_000), "10m");
    }

    #[test]
    fn fmt_reset_now_when_past_or_equal() {
        assert_eq!(fmt_reset(1_000_000, 1_000_005), "now");
        assert_eq!(fmt_reset(1_000_000, 1_000_000), "now");
    }

    #[test]
    fn fmt_reset_boundary_at_one_day() {
        // exactly 24h → 1d0h
        assert_eq!(fmt_reset(1_000_000 + 86400, 1_000_000), "1d0h");
    }

    #[test]
    fn fmt_reset_boundary_at_one_hour() {
        // exactly 1h → 1h0m
        assert_eq!(fmt_reset(1_000_000 + 3600, 1_000_000), "1h0m");
    }
}
