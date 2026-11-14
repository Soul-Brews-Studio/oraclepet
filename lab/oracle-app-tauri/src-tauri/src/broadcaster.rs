//! BLE broadcaster thread.
//!
//! Reads `/tmp/claude-statusline/raw.json` for the live (active) session OR
//! reads the pinned session's `.jsonl` directly when the user has selected
//! one in the tray. Either way, applies the sanity gate and advertises
//! via `oracle_beacon::advertise()`.
//!
//! Each accepted sample is pushed to the tray + frontend via `status_tx`.
//!
//! Loop cadence is tight (5s per advert + read) so a click in the menu
//! shows on the device within ~5 seconds. Exits cleanly when `quit`
//! flips true.

use crate::sessions::{self, extract_project_label_from_jsonl_dir};
use crate::state::TargetHandle;
use crate::status::{StatusJson, from_sample};
use oracle_beacon::binary::{Payload, encode_wire, model_code_to_byte, pack_project};
use oracle_beacon::{
    Decision, LoopState, Model, Sample, advertise, decide, extract_sample, fmt_reset,
    model_code, now_secs, read_statusline,
};
use std::path::{Path, PathBuf};
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::Sender;
use std::thread;
use std::time::{Duration, Instant};

pub const DURATION_SECS: u64 = 5;
/// Pause between non-advertising loop iterations (skip / no-data) so the
/// loop never busy-spins. The Accept path is paced by advertise()'s own
/// DURATION_SECS sleep instead.
pub const POLL_SECS: u64 = 3;
pub const DEFAULT_JSON: &str = "/tmp/claude-statusline/raw.json";

pub struct BroadcasterConfig {
    pub json_path: PathBuf,
    pub target: TargetHandle,
    pub status_tx: Sender<StatusJson>,
    pub quit: Arc<AtomicBool>,
}

pub fn run(cfg: BroadcasterConfig) {
    let mut state = LoopState::default();
    let mut last_seen_k: u64 = 0; // raw value last iteration — detects a persisted jump
    let mut last_log = String::new(); // dedupe repeated skip/none logs
    while !cfg.quit.load(Ordering::Relaxed) {
        let now = now_secs();
        let target = cfg.target.get();

        // Two paths: pinned session (read .jsonl + account-wide q7d) vs
        // follow active (use /tmp/raw.json as-is, no filter).
        let sample = if let Some(ref id) = target {
            build_pinned_sample(id, &cfg.json_path, now)
        } else {
            read_statusline(&cfg.json_path)
                .ok()
                .map(|s| extract_sample(&s, now))
        };

        // `advertise()` itself sleeps DURATION_SECS, which paces the Accept
        // path. Every OTHER path must pace itself or the loop busy-spins
        // (was pinning a CPU core + flooding stdout with skip lines).
        let mut advertised = false;

        match sample {
            Some(s) => {
                let mut decision = decide(&s, None, &state, now);
                // A jump that PERSISTS across a poll is a real change, not a
                // transient race on the shared JSON — accept it instead of
                // rejecting for the full 30s gate window.
                if matches!(decision, Decision::SkipSanityJump { .. }) && s.ctx_used_k == last_seen_k {
                    decision = Decision::Accept;
                }
                last_seen_k = s.ctx_used_k;

                match decision {
                    Decision::Accept => {
                        state.last_used_k = s.ctx_used_k;
                        state.last_sample_ts = now;
                        let _ = cfg.status_tx.send(from_sample(&s));
                        let project = resolve_project_for_sample(&s, target.as_deref());
                        let wire = encode_payload(&s, &project, now);
                        eprintln!(
                            "broadcasting: {} ({} bytes, project={project:?}, model_ver={:#04x}, sent_min={})",
                            wire,
                            wire.len(),
                            s.model_ver,
                            (now / 60) as u16,
                        );
                        if let Err(e) = advertise(&wire, DURATION_SECS) {
                            eprintln!("advertise error: {e:#}");
                        }
                        advertised = true;
                    }
                    Decision::SkipForeignSession => {}
                    Decision::SkipSanityJump { from, to, elapsed } => {
                        log_once(&mut last_log, format!("skip: jump {from}k→{to}k in {elapsed}s"));
                    }
                }
            }
            None => log_once(
                &mut last_log,
                format!(
                    "no data ({})",
                    if target.is_some() { "pinned session has no jsonl yet" } else { "raw.json missing" }
                ),
            ),
        }

        if !advertised {
            interruptible_sleep(&cfg.quit, POLL_SECS);
        }
    }
}

/// Print a line only when it differs from the previous one — stops a
/// stuck condition (e.g. a persistent sanity-skip) from flooding stdout.
fn log_once(last: &mut String, msg: String) {
    if *last != msg {
        eprintln!("{msg}");
        *last = msg;
    }
}

/// Sleep `secs`, waking early (every 200ms) if quit flips — so a non-
/// advertising loop iteration paces itself without delaying shutdown.
fn interruptible_sleep(quit: &Arc<AtomicBool>, secs: u64) {
    let deadline = Instant::now() + Duration::from_secs(secs);
    while Instant::now() < deadline {
        if quit.load(Ordering::Relaxed) {
            return;
        }
        thread::sleep(Duration::from_millis(200));
    }
}

fn encode_payload(s: &Sample, project: &str, now: u64) -> String {
    let payload = Payload {
        version: oracle_beacon::binary::PROTO_VERSION,
        model_code: model_code_to_byte(s.model_code),
        ctx_used_k: s.ctx_used_k.min(u16::MAX as u64) as u16,
        q7d_pct: s.q7d_pct.min(100) as u8,
        reset_min: reset_human_to_minutes(&s.reset_human),
        sent_min: ((now / 60) % 65536) as u16,
        // Strip the shared "-oracle"/"-app" suffix so the meaningful stem
        // survives the 8-byte field ("esp32-oracle" → "esp32").
        project: pack_project(&oracle_beacon::short_project(project)),
        model_ver: s.model_ver,
    };
    encode_wire(&payload)
}

/// Parse "3d10h" / "5h12m" / "45m" / "now" back into total minutes.
fn reset_human_to_minutes(s: &str) -> u16 {
    if s == "now" || s.is_empty() {
        return 0;
    }
    let mut total: u32 = 0;
    let mut current: u32 = 0;
    for c in s.chars() {
        if let Some(d) = c.to_digit(10) {
            current = current.saturating_mul(10).saturating_add(d);
        } else {
            let unit_min = match c {
                'd' => 60 * 24,
                'h' => 60,
                'm' => 1,
                _ => 0,
            };
            total = total.saturating_add(current.saturating_mul(unit_min));
            current = 0;
        }
    }
    total.min(u16::MAX as u32) as u16
}

/// Figure out which project label to embed in the binary payload. For
/// pinned sessions, we resolve to the actual repo dir name. For follow
/// mode we use whatever session is writing the JSON (best-effort).
fn resolve_project_for_sample(s: &Sample, target: Option<&str>) -> String {
    if let Some(t) = target {
        return extract_project_label_from_jsonl_dir(t).unwrap_or_else(|| "active".into());
    }
    extract_project_label_from_jsonl_dir(&s.session_id).unwrap_or_else(|| "active".into())
}

/// Build a Sample from a pinned session's .jsonl (for ctx_used + model)
/// combined with /tmp/raw.json's account-wide q7d_pct + reset.
fn build_pinned_sample(session_id_prefix: &str, raw_json_path: &Path, now: u64) -> Option<Sample> {
    let usage = sessions::latest_usage(session_id_prefix)?;
    let raw = read_statusline(raw_json_path).ok()?;
    let model = Model {
        display_name: Some(usage.model_display.clone()),
        id: None,
    };
    Some(Sample {
        session_id: session_id_prefix.to_string(),
        model_code: model_code(&model),
        model_ver: oracle_beacon::parse_model_version(&usage.model_display),
        ctx_used_k: usage.ctx_used_k,
        q7d_pct: raw.rate_limits.seven_day.used_percentage,
        reset_human: fmt_reset(raw.rate_limits.seven_day.resets_at, now),
    })
}

#[cfg(test)]
mod tests {
    use super::*;
    use std::sync::mpsc::channel;

    #[test]
    fn config_constants_sane() {
        assert_eq!(DURATION_SECS, 5);
        assert!(DEFAULT_JSON.starts_with("/tmp"));
    }

    #[test]
    fn run_exits_promptly_when_quit_set() {
        let (tx, _rx) = channel();
        let quit = Arc::new(AtomicBool::new(true));
        let cfg = BroadcasterConfig {
            json_path: PathBuf::from("/tmp/no-such-broadcaster-test.json"),
            target: TargetHandle::new(None),
            status_tx: tx,
            quit: Arc::clone(&quit),
        };
        let start = Instant::now();
        run(cfg);
        assert!(start.elapsed() < Duration::from_secs(2));
    }

    #[test]
    fn build_pinned_sample_returns_none_when_session_missing() {
        let path = PathBuf::from("/tmp/no-such-pinned-raw.json");
        let sample = build_pinned_sample("zzzzzzzz", &path, 1_000_000);
        assert!(sample.is_none());
    }
}
