//! StatusJson — the serialized snapshot the frontend + tray consume.
//!
//! Wraps `oracle_beacon::Sample` with display-friendly extras
//! (`model_friendly`, `ctx_max_k`, `ctx_pct`) so the JS side doesn't
//! need to know about the model→max lookup.

use serde::Serialize;
use std::path::PathBuf;

#[derive(Serialize, Clone, Debug)]
pub struct StatusJson {
    pub model_code: String,
    pub model_friendly: String,
    pub ctx_used_k: u64,
    pub ctx_max_k: u64,
    pub ctx_pct: u64,
    pub q7d_pct: u64,
    pub reset_human: String,
    pub session_id: String,
    /// Repo/project this context belongs to (e.g. "esp32-oracle"). The
    /// window header shows it so you always know WHICH project's context
    /// you're looking at — the pinned/active session can change.
    pub project: String,
}

/// Resolve the project label for a session id (full repo name, not the
/// wire-shortened form — the window has room for the full thing).
fn project_for(session_id: &str) -> String {
    crate::sessions::extract_project_label_from_jsonl_dir(session_id)
        .unwrap_or_else(|| "active".to_string())
}

/// Build the display name from the model code (family) + packed version
/// byte. The version is data, not hardcoded — "OPUS 4.8" today, "OPUS 5.0"
/// tomorrow, with no code change. Falls back to family-only if the version
/// is unknown (0).
pub fn friendly_name(code: &str, model_ver: u8) -> String {
    let family = oracle_beacon::model_family(code);
    let ver = oracle_beacon::fmt_model_version(model_ver);
    if ver.is_empty() {
        family.to_string()
    } else {
        format!("{family} {ver}")
    }
}

pub fn snapshot(json_path: &PathBuf) -> Result<StatusJson, String> {
    let statusline = oracle_beacon::read_statusline(json_path).map_err(|e| e.to_string())?;
    let sample = oracle_beacon::extract_sample(&statusline, oracle_beacon::now_secs());
    Ok(from_sample(&sample))
}

pub fn from_sample(sample: &oracle_beacon::Sample) -> StatusJson {
    let max_k = oracle_beacon::model_max_k(sample.model_code);
    let ctx_pct = if max_k > 0 { sample.ctx_used_k * 100 / max_k } else { 0 };
    StatusJson {
        model_code: sample.model_code.to_string(),
        model_friendly: friendly_name(sample.model_code, sample.model_ver),
        ctx_used_k: sample.ctx_used_k,
        ctx_max_k: max_k,
        ctx_pct,
        q7d_pct: sample.q7d_pct,
        reset_human: sample.reset_human.clone(),
        session_id: sample.session_id.clone(),
        project: project_for(&sample.session_id),
    }
}

/// Compact tray title — `ψ Op 23% q39%` — capped so macOS doesn't elide.
pub fn tray_title(s: &StatusJson) -> String {
    format!("ψ {} {}% q{}%", s.model_code, s.ctx_pct, s.q7d_pct)
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::io::Write;

    fn write_tmp(name: &str, content: &str) -> PathBuf {
        let p = std::env::temp_dir().join(format!("tauri-status-test-{name}.json"));
        let mut f = std::fs::File::create(&p).unwrap();
        f.write_all(content.as_bytes()).unwrap();
        p
    }

    const OPUS: &str = r#"{
        "session_id": "af022cb2-aaa",
        "model": {"display_name": "Opus 4.8 (1M context)"},
        "context_window": {"current_usage": {
            "input_tokens": 1, "cache_creation_input_tokens": 1895,
            "cache_read_input_tokens": 234370, "output_tokens": 456
        }},
        "rate_limits": {"seven_day": {"used_percentage": 39, "resets_at": 1780279200}}
    }"#;

    #[test]
    fn friendly_name_builds_family_plus_version() {
        assert_eq!(friendly_name("Op", 0x48), "OPUS 4.8");
        assert_eq!(friendly_name("So", 0x46), "SONNET 4.6");
        assert_eq!(friendly_name("Ha", 0x45), "HAIKU 4.5");
        // future versions just work — no code change
        assert_eq!(friendly_name("Op", 0x50), "OPUS 5.0");
        // unknown version → family only
        assert_eq!(friendly_name("Op", 0), "OPUS");
        // unknown family → generic
        assert_eq!(friendly_name("", 0x48), "MODEL 4.8");
        assert_eq!(friendly_name("Garbage", 0), "MODEL");
    }

    #[test]
    fn snapshot_round_trips_opus() {
        let path = write_tmp("opus", OPUS);
        let s = snapshot(&path).unwrap();
        assert_eq!(s.model_code, "Op");
        assert_eq!(s.model_friendly, "OPUS 4.8");
        assert_eq!(s.ctx_used_k, 236);
        assert_eq!(s.ctx_max_k, 1000);
        assert_eq!(s.ctx_pct, 23);
        assert_eq!(s.q7d_pct, 39);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn snapshot_missing_file_errors() {
        let path = PathBuf::from("/tmp/tauri-status-no-such-99999.json");
        assert!(snapshot(&path).is_err());
    }

    #[test]
    fn snapshot_invalid_json_errors() {
        let path = write_tmp("invalid", "[not json");
        assert!(snapshot(&path).is_err());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn json_keys_stable() {
        let s = StatusJson {
            model_code: "Op".into(),
            model_friendly: "OPUS 4.8".into(),
            ctx_used_k: 236,
            ctx_max_k: 1000,
            ctx_pct: 23,
            q7d_pct: 39,
            reset_human: "3d11h".into(),
            session_id: "af022cb2".into(),
            project: "esp32-oracle".into(),
        };
        let j = serde_json::to_string(&s).unwrap();
        assert!(j.contains(r#""model_friendly":"OPUS 4.8""#));
        assert!(j.contains(r#""ctx_pct":23"#));
        assert!(j.contains(r#""reset_human":"3d11h""#));
        assert!(j.contains(r#""project":"esp32-oracle""#));
    }

    #[test]
    fn tray_title_short_format() {
        let s = StatusJson {
            model_code: "Op".into(),
            model_friendly: "OPUS 4.8".into(),
            ctx_used_k: 236,
            ctx_max_k: 1000,
            ctx_pct: 23,
            q7d_pct: 39,
            reset_human: "3d11h".into(),
            session_id: "af022cb2".into(),
            project: "esp32-oracle".into(),
        };
        assert_eq!(tray_title(&s), "ψ Op 23% q39%");
    }

    #[test]
    fn from_sample_maps_fields() {
        let sample = oracle_beacon::Sample {
            session_id: "abc".into(),
            model_code: "Ha",
            model_ver: 0x45,
            ctx_used_k: 50,
            q7d_pct: 25,
            reset_human: "1h".into(),
        };
        let s = from_sample(&sample);
        assert_eq!(s.model_code, "Ha");
        assert_eq!(s.model_friendly, "HAIKU 4.5");
        assert_eq!(s.ctx_max_k, 200);
        assert_eq!(s.ctx_pct, 25); // 50/200 = 25%
    }
}
