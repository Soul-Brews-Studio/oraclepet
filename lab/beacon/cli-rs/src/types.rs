//! Serde structs mirroring the JSON Claude Code's statusline pipes.
//!
//! See `/tmp/claude-statusline/raw.json` for a live example. We capture
//! only the fields the broadcaster needs; everything else is ignored.

use serde::Deserialize;

#[derive(Debug, Deserialize)]
pub struct Statusline {
    pub session_id: Option<String>,
    pub model: Model,
    pub context_window: ContextWindow,
    pub rate_limits: RateLimits,
}

#[derive(Debug, Deserialize)]
pub struct Model {
    pub display_name: Option<String>,
    pub id: Option<String>,
}

#[derive(Debug, Deserialize)]
pub struct ContextWindow {
    pub current_usage: CurrentUsage,
}

#[derive(Debug, Deserialize, Default)]
pub struct CurrentUsage {
    #[serde(default)]
    pub input_tokens: u64,
    #[serde(default)]
    pub cache_creation_input_tokens: u64,
    #[serde(default)]
    pub cache_read_input_tokens: u64,
    #[serde(default)]
    pub output_tokens: u64,
}

impl CurrentUsage {
    /// Sum of all four token buckets, rounded down to K.
    pub fn total_k(&self) -> u64 {
        (self.input_tokens
            + self.cache_creation_input_tokens
            + self.cache_read_input_tokens
            + self.output_tokens)
            / 1000
    }
}

#[derive(Debug, Deserialize)]
pub struct RateLimits {
    pub seven_day: SevenDay,
}

#[derive(Debug, Deserialize, Default)]
pub struct SevenDay {
    #[serde(default)]
    pub used_percentage: u64,
    #[serde(default)]
    pub resets_at: i64,
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn current_usage_total_k_sums_all_buckets() {
        let u = CurrentUsage {
            input_tokens: 1,
            cache_creation_input_tokens: 1895,
            cache_read_input_tokens: 234370,
            output_tokens: 456,
        };
        // 1 + 1895 + 234370 + 456 = 236722 → 236k
        assert_eq!(u.total_k(), 236);
    }

    #[test]
    fn current_usage_total_k_zero_for_empty() {
        let u = CurrentUsage::default();
        assert_eq!(u.total_k(), 0);
    }

    #[test]
    fn current_usage_total_k_rounds_down() {
        let u = CurrentUsage { input_tokens: 999, ..Default::default() };
        assert_eq!(u.total_k(), 0);
        let u = CurrentUsage { input_tokens: 1000, ..Default::default() };
        assert_eq!(u.total_k(), 1);
        let u = CurrentUsage { input_tokens: 1999, ..Default::default() };
        assert_eq!(u.total_k(), 1);
    }

    #[test]
    fn statusline_round_trips() {
        let json = r#"{
            "session_id": "abc12345-def",
            "model": {"display_name": "Opus 4.7", "id": "claude-opus-4-7"},
            "context_window": {
                "current_usage": {"input_tokens": 100, "cache_read_input_tokens": 200, "output_tokens": 50, "cache_creation_input_tokens": 10}
            },
            "rate_limits": {"seven_day": {"used_percentage": 42, "resets_at": 1700000000}}
        }"#;
        let s: Statusline = serde_json::from_str(json).unwrap();
        assert_eq!(s.session_id.as_deref(), Some("abc12345-def"));
        assert_eq!(s.model.display_name.as_deref(), Some("Opus 4.7"));
        assert_eq!(s.context_window.current_usage.total_k(), 0); // 360 → 0
        assert_eq!(s.rate_limits.seven_day.used_percentage, 42);
        assert_eq!(s.rate_limits.seven_day.resets_at, 1700000000);
    }

    #[test]
    fn statusline_tolerates_missing_optional_fields() {
        let json = r#"{
            "model": {},
            "context_window": {"current_usage": {}},
            "rate_limits": {"seven_day": {}}
        }"#;
        let s: Statusline = serde_json::from_str(json).unwrap();
        assert_eq!(s.session_id, None);
        assert_eq!(s.model.display_name, None);
        assert_eq!(s.model.id, None);
        assert_eq!(s.context_window.current_usage.total_k(), 0);
        assert_eq!(s.rate_limits.seven_day.used_percentage, 0);
        assert_eq!(s.rate_limits.seven_day.resets_at, 0);
    }
}
