//! Enumerate Claude Code sessions on this machine.
//!
//! Walks `~/.claude/projects/*/*.jsonl`, extracts the session id and a
//! human-readable "repo label" from the encoded directory name, and
//! returns the most-recently-modified entries first.

use serde::Serialize;
use std::time::{SystemTime, UNIX_EPOCH};

/// One entry per `.jsonl` file. The id is the first 8 chars of the
/// filename stem (same shape the broadcaster filters on).
#[derive(Debug, Clone, Serialize)]
pub struct SessionEntry {
    pub id: String,                 // first 8 chars
    pub repo_label: String,         // best-effort repo name (`esp32-oracle`)
    pub last_modified_secs: u64,    // unix epoch
    pub age_human: String,          // "5m ago", "2h ago", …
}

/// Live usage snapshot read directly from a session's .jsonl (its most
/// recent assistant message with a `usage` field). Used when the tray
/// pins to an inactive session — we still get fresh data without
/// waiting for that session to render its statusline.
#[derive(Debug, Clone)]
pub struct UsageSnapshot {
    pub model_display: String, // e.g. "claude-opus-4-7[1m]" — fed to model_code()
    pub ctx_used_k: u64,
}

/// Extract a short, friendly repo label from the encoded project-dir name.
///
/// The encoding replaces `/` AND `.` with `-`, so we can't perfectly
/// reverse it. Heuristics:
///   1. If we see `github-com-`, take everything after (typical for ghq)
///      and join the last two dash-segments → `esp32-oracle`.
///   2. Otherwise take the last two dash-segments → `hermes-discord`.
pub fn extract_repo_label(encoded: &str) -> String {
    let s = encoded.trim_start_matches('-');
    let after = s.split("github-com-").nth(1).unwrap_or(s);
    let segs: Vec<&str> = after.split('-').collect();
    if segs.len() >= 2 {
        segs[segs.len() - 2..].join("-")
    } else if !segs.is_empty() {
        segs[0].to_string()
    } else {
        encoded.to_string()
    }
}

/// Humanise an age in seconds: "just now" → "Xm ago" → "Xh ago" → "Xd ago".
pub fn humanize_age(secs_ago: u64) -> String {
    if secs_ago < 60 {
        "just now".into()
    } else if secs_ago < 3600 {
        format!("{}m ago", secs_ago / 60)
    } else if secs_ago < 86400 {
        format!("{}h ago", secs_ago / 3600)
    } else if secs_ago < 86400 * 30 {
        format!("{}d ago", secs_ago / 86400)
    } else {
        "1mo+ ago".into()
    }
}

fn now_secs() -> u64 {
    SystemTime::now()
        .duration_since(UNIX_EPOCH)
        .map(|d| d.as_secs())
        .unwrap_or(0)
}

/// Scan `~/.claude/projects/*/*.jsonl` and return up to `limit` entries
/// sorted by most-recently-modified first. Quietly skips unreadable dirs.
pub fn list(limit: usize) -> Vec<SessionEntry> {
    let home = match std::env::var_os("HOME") {
        Some(h) => h,
        None => return Vec::new(),
    };
    let root = std::path::PathBuf::from(&home).join(".claude/projects");
    let mut out: Vec<SessionEntry> = Vec::new();

    let project_dirs = match std::fs::read_dir(&root) {
        Ok(d) => d,
        Err(_) => return Vec::new(),
    };

    for entry in project_dirs.flatten() {
        let path = entry.path();
        if !path.is_dir() {
            continue;
        }
        let dir_name = path.file_name().and_then(|s| s.to_str()).unwrap_or("");
        let repo_label = extract_repo_label(dir_name);

        let inner = match std::fs::read_dir(&path) {
            Ok(d) => d,
            Err(_) => continue,
        };
        for f in inner.flatten() {
            let fp = f.path();
            if fp.extension().and_then(|s| s.to_str()) != Some("jsonl") {
                continue;
            }
            let mtime = match f.metadata().ok().and_then(|m| m.modified().ok()) {
                Some(t) => t,
                None => continue,
            };
            let mtime_secs = mtime.duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
            let stem = fp.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            let id: String = stem.chars().take(8).collect();
            if id.is_empty() {
                continue;
            }
            out.push(SessionEntry {
                id,
                repo_label: repo_label.clone(),
                last_modified_secs: mtime_secs,
                age_human: humanize_age(now_secs().saturating_sub(mtime_secs)),
            });
        }
    }

    out.sort_by(|a, b| b.last_modified_secs.cmp(&a.last_modified_secs));
    out.truncate(limit);
    out
}

/// Find the .jsonl whose filename stem starts with `session_id_prefix`,
/// scan backward for the latest assistant message with a `usage` field,
/// return its token-bucket sum + the model name string. Returns None
/// if nothing parseable was found.
pub fn latest_usage(session_id_prefix: &str) -> Option<UsageSnapshot> {
    let home = std::env::var_os("HOME")?;
    let root = std::path::PathBuf::from(&home).join(".claude/projects");
    let project_dirs = std::fs::read_dir(&root).ok()?;
    for project_entry in project_dirs.flatten() {
        let path = project_entry.path();
        if !path.is_dir() {
            continue;
        }
        let inner = match std::fs::read_dir(&path) {
            Ok(d) => d,
            Err(_) => continue,
        };
        for file_entry in inner.flatten() {
            let fp = file_entry.path();
            let stem = fp.file_stem().and_then(|s| s.to_str()).unwrap_or("");
            if !stem.starts_with(session_id_prefix) {
                continue;
            }
            if let Some(snap) = parse_usage_tail(&fp) {
                return Some(snap);
            }
        }
    }
    None
}

/// Reverse-lookup: given a session_id prefix, find the project dir
/// that contains its .jsonl and return the heuristic repo label.
pub fn extract_project_label_from_jsonl_dir(session_id_prefix: &str) -> Option<String> {
    let home = std::env::var_os("HOME")?;
    let root = std::path::PathBuf::from(&home).join(".claude/projects");
    let project_dirs = std::fs::read_dir(&root).ok()?;
    for project_entry in project_dirs.flatten() {
        let path = project_entry.path();
        if !path.is_dir() {
            continue;
        }
        let dir_name = path.file_name().and_then(|s| s.to_str()).unwrap_or("");
        let inner = match std::fs::read_dir(&path) {
            Ok(d) => d,
            Err(_) => continue,
        };
        for file_entry in inner.flatten() {
            let stem = file_entry.path().file_stem().and_then(|s| s.to_str()).unwrap_or("").to_string();
            if stem.starts_with(session_id_prefix) {
                return Some(extract_repo_label(dir_name));
            }
        }
    }
    None
}

fn parse_usage_tail(path: &std::path::Path) -> Option<UsageSnapshot> {
    let content = std::fs::read_to_string(path).ok()?;
    let mut model_seen: Option<String> = None;
    // Walk backward — most recent assistant message with usage wins.
    for line in content.lines().rev() {
        let v: serde_json::Value = match serde_json::from_str(line) {
            Ok(v) => v,
            Err(_) => continue,
        };
        if v.get("type").and_then(|t| t.as_str()) != Some("assistant") {
            continue;
        }
        let msg = match v.get("message") {
            Some(m) => m,
            None => continue,
        };
        if model_seen.is_none()
            && let Some(model) = msg.get("model").and_then(|m| m.as_str())
        {
            model_seen = Some(model.to_string());
        }
        if let Some(usage) = msg.get("usage") {
            let input = usage.get("input_tokens").and_then(|v| v.as_u64()).unwrap_or(0);
            let cache_creation = usage.get("cache_creation_input_tokens").and_then(|v| v.as_u64()).unwrap_or(0);
            let cache_read = usage.get("cache_read_input_tokens").and_then(|v| v.as_u64()).unwrap_or(0);
            let output = usage.get("output_tokens").and_then(|v| v.as_u64()).unwrap_or(0);
            let ctx_used_k = (input + cache_creation + cache_read + output) / 1000;
            return Some(UsageSnapshot {
                model_display: model_seen.unwrap_or_default(),
                ctx_used_k,
            });
        }
    }
    None
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn humanize_age_within_minute() {
        assert_eq!(humanize_age(0), "just now");
        assert_eq!(humanize_age(59), "just now");
    }

    #[test]
    fn humanize_age_minutes() {
        assert_eq!(humanize_age(60), "1m ago");
        assert_eq!(humanize_age(599), "9m ago");
        assert_eq!(humanize_age(3599), "59m ago");
    }

    #[test]
    fn humanize_age_hours() {
        assert_eq!(humanize_age(3600), "1h ago");
        assert_eq!(humanize_age(7200), "2h ago");
        assert_eq!(humanize_age(86399), "23h ago");
    }

    #[test]
    fn humanize_age_days() {
        assert_eq!(humanize_age(86400), "1d ago");
        assert_eq!(humanize_age(86400 * 6), "6d ago");
    }

    #[test]
    fn humanize_age_month_plus() {
        assert_eq!(humanize_age(86400 * 31), "1mo+ ago");
        assert_eq!(humanize_age(86400 * 365), "1mo+ ago");
    }

    #[test]
    fn extract_repo_label_github_style() {
        assert_eq!(
            extract_repo_label("-opt-Code-github-com-Soul-Brews-Studio-esp32-oracle"),
            "esp32-oracle"
        );
        assert_eq!(
            extract_repo_label("-opt-Code-github-com-laris-co-homelab"),
            "co-homelab"
        );
    }

    #[test]
    fn extract_repo_label_no_github_anchor() {
        // Last 2 segments only.
        assert_eq!(extract_repo_label("-Users-nat--claude-channels-hermes-discord"), "hermes-discord");
    }

    #[test]
    fn extract_repo_label_single_segment() {
        assert_eq!(extract_repo_label("-foo"), "foo");
    }

    #[test]
    fn extract_repo_label_empty() {
        // Defensive: empty string in, ok out.
        assert_eq!(extract_repo_label(""), "");
    }

    #[test]
    fn list_is_non_panicking_even_when_no_home() {
        // If HOME isn't a Claude Code projects dir, list returns empty rather than panic.
        let _ = list(10);
    }

    #[test]
    fn entry_serializes_to_camel_keys() {
        let e = SessionEntry {
            id: "abc12345".into(),
            repo_label: "esp32-oracle".into(),
            last_modified_secs: 1_700_000_000,
            age_human: "5m ago".into(),
        };
        let json = serde_json::to_string(&e).unwrap();
        assert!(json.contains(r#""id":"abc12345""#));
        assert!(json.contains(r#""repo_label":"esp32-oracle""#));
        assert!(json.contains(r#""age_human":"5m ago""#));
    }

    #[test]
    fn parse_usage_tail_extracts_latest_usage() {
        // Build a minimal jsonl with two assistant messages — the LATEST one wins.
        let content = r#"{"type":"user","message":{}}
{"type":"assistant","message":{"model":"claude-haiku-4-5","usage":{"input_tokens":10,"cache_read_input_tokens":50,"cache_creation_input_tokens":5,"output_tokens":35}}}
{"type":"user","message":{}}
{"type":"assistant","message":{"model":"claude-opus-4-7[1m]","usage":{"input_tokens":1,"cache_read_input_tokens":99000,"cache_creation_input_tokens":1000,"output_tokens":500}}}
"#;
        let path = std::env::temp_dir().join(format!("usage-tail-test-{}.jsonl", std::process::id()));
        std::fs::write(&path, content).unwrap();
        let snap = parse_usage_tail(&path).unwrap();
        assert_eq!(snap.model_display, "claude-opus-4-7[1m]");
        // 1 + 99000 + 1000 + 500 = 100501 → 100k
        assert_eq!(snap.ctx_used_k, 100);
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn parse_usage_tail_skips_lines_without_usage() {
        let content = r#"{"type":"user","message":{}}
{"type":"assistant","message":{"model":"x"}}
{"type":"summary","gitBranch":"main"}
"#;
        let path = std::env::temp_dir().join(format!("usage-tail-nouse-{}.jsonl", std::process::id()));
        std::fs::write(&path, content).unwrap();
        assert!(parse_usage_tail(&path).is_none());
        let _ = std::fs::remove_file(&path);
    }

    #[test]
    fn parse_usage_tail_returns_none_for_missing_file() {
        let path = std::path::PathBuf::from("/tmp/no-such-jsonl-file-99999.jsonl");
        assert!(parse_usage_tail(&path).is_none());
    }

    #[test]
    fn latest_usage_returns_none_for_unknown_prefix() {
        assert!(latest_usage("zzzzzzzz").is_none());
    }
}
