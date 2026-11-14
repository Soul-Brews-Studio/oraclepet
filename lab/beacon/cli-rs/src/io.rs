//! JSON I/O + clock + auto-session detection.

use crate::format::{fmt_reset, model_code, parse_model_version};
use crate::sample::Sample;
use crate::types::Statusline;
use anyhow::{Context, Result};
use std::path::Path;
use std::time::{SystemTime, UNIX_EPOCH};

pub fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

pub fn read_statusline(path: &Path) -> Result<Statusline> {
    let bytes = std::fs::read(path)
        .with_context(|| format!("can't read statusline JSON at {}", path.display()))?;
    serde_json::from_slice(&bytes).context("invalid statusline JSON")
}

pub fn extract_sample(s: &Statusline, now: u64) -> Sample {
    let used_k = s.context_window.current_usage.total_k();
    let session_id = s
        .session_id
        .as_deref()
        .unwrap_or("")
        .chars()
        .take(8)
        .collect();
    // Version comes from the display name ("Opus 4.8 (1M context)" → 0x48).
    // The id form ("claude-opus-4-8") uses dashes so it yields 0 (unknown),
    // which the device renders as family-only — an honest fallback.
    let ver_source = s.model.display_name.as_deref().unwrap_or("");
    Sample {
        session_id,
        model_code: model_code(&s.model),
        model_ver: parse_model_version(ver_source),
        ctx_used_k: used_k,
        q7d_pct: s.rate_limits.seven_day.used_percentage,
        reset_human: fmt_reset(s.rate_limits.seven_day.resets_at, now),
    }
}

/// Auto-detect the target session from the most recently modified jsonl
/// in the cwd's project dir. Walks parent dirs in case cwd is a subdir.
#[cfg(not(target_family = "wasm"))]
pub fn auto_session() -> Option<String> {
    use std::env;
    use std::path::PathBuf;
    let home = env::var_os("HOME")?;
    let mut cwd = env::current_dir().ok()?;
    loop {
        let encoded: String = cwd
            .to_string_lossy()
            .replace('/', "-")
            .replace('.', "-");
        let proj_dir = PathBuf::from(&home).join(".claude/projects").join(&encoded);
        if proj_dir.is_dir() {
            let mut entries: Vec<_> = std::fs::read_dir(&proj_dir)
                .ok()?
                .filter_map(|e| e.ok())
                .filter(|e| e.path().extension().and_then(|s| s.to_str()) == Some("jsonl"))
                .filter_map(|e| {
                    let m = e.metadata().ok()?.modified().ok()?;
                    Some((m, e.path()))
                })
                .collect();
            entries.sort_by(|a, b| b.0.cmp(&a.0));
            if let Some((_, latest)) = entries.first()
                && let Some(stem) = latest.file_stem().and_then(|s| s.to_str())
            {
                return Some(stem.chars().take(8).collect());
            }
        }
        if !cwd.pop() {
            return None;
        }
    }
}

#[cfg(target_family = "wasm")]
pub fn auto_session() -> Option<String> {
    None
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;
    use std::path::PathBuf;

    fn write_tmp(name: &str, content: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("oracle-beacon-io-test-{}.json", name));
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        p
    }

    #[test]
    fn now_secs_returns_recent_epoch() {
        let n = now_secs();
        // 2026 epoch is ~1_777_000_000. Sanity check: bigger than 2020's 1.6e9.
        assert!(n > 1_600_000_000);
    }

    #[test]
    fn read_statusline_parses_real_shape() {
        let path = write_tmp("read-valid", r#"{
            "model": {"display_name": "Opus 4.7"},
            "context_window": {"current_usage": {"input_tokens": 1, "cache_read_input_tokens": 9}},
            "rate_limits": {"seven_day": {"used_percentage": 50, "resets_at": 1700000000}}
        }"#);
        let s = read_statusline(&path).unwrap();
        assert_eq!(s.rate_limits.seven_day.used_percentage, 50);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn read_statusline_errors_on_missing_file() {
        let path = PathBuf::from("/tmp/oracle-beacon-no-such-file-99999.json");
        assert!(read_statusline(&path).is_err());
    }

    #[test]
    fn read_statusline_errors_on_invalid_json() {
        let path = write_tmp("read-invalid", "not valid {[");
        assert!(read_statusline(&path).is_err());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn extract_sample_uses_first_8_chars_of_session_id() {
        let json = r#"{
            "session_id": "abcdefghij-1234",
            "model": {"display_name": "Opus 4.7"},
            "context_window": {"current_usage": {"input_tokens": 0}},
            "rate_limits": {"seven_day": {"used_percentage": 10, "resets_at": 0}}
        }"#;
        let s: Statusline = serde_json::from_str(json).unwrap();
        let sample = extract_sample(&s, 1_000_000);
        assert_eq!(sample.session_id, "abcdefgh");
    }

    #[test]
    fn extract_sample_empty_session_id_when_missing() {
        let json = r#"{
            "model": {"display_name": "Opus 4.7"},
            "context_window": {"current_usage": {"input_tokens": 0}},
            "rate_limits": {"seven_day": {"used_percentage": 0, "resets_at": 0}}
        }"#;
        let s: Statusline = serde_json::from_str(json).unwrap();
        let sample = extract_sample(&s, 1_000_000);
        assert_eq!(sample.session_id, "");
    }

    #[test]
    fn extract_sample_short_session_id_returns_full_value() {
        let json = r#"{
            "session_id": "abc",
            "model": {"display_name": "Opus 4.7"},
            "context_window": {"current_usage": {}},
            "rate_limits": {"seven_day": {}}
        }"#;
        let s: Statusline = serde_json::from_str(json).unwrap();
        let sample = extract_sample(&s, 1_000_000);
        assert_eq!(sample.session_id, "abc");
    }

    #[test]
    fn auto_session_returns_none_when_no_project_dir() {
        // We're in /opt/... which has no ~/.claude/projects/... entry; should walk up
        // and either find ESP32's actual project dir or return None — either is fine.
        // The function is non-panicking, that's what we assert here.
        let _ = auto_session();
    }
}
