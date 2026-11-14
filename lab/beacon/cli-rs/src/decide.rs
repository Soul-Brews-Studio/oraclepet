//! Session filter + sanity gate.
//!
//! The statusline JSON is shared across all Claude Code instances on a
//! machine — every render overwrites `/tmp/claude-statusline/raw.json`.
//! Without filtering, the broadcaster would intermittently send foreign
//! sessions' data. Layer A drops cycles where `.session_id` doesn't
//! match the target. Layer B catches whatever Layer A missed: a sudden
//! >50% jump in `ctx_used_k` within 30s is almost certainly a race
//! (cache reads accumulate gradually, never spike).

use crate::sample::{LoopState, Sample};

#[derive(Debug, PartialEq, Eq)]
pub enum Decision {
    Accept,
    SkipForeignSession,
    SkipSanityJump { from: u64, to: u64, elapsed: u64 },
}

pub fn decide(
    sample: &Sample,
    target_session: Option<&str>,
    state: &LoopState,
    now: u64,
) -> Decision {
    // Layer A
    if let Some(target) = target_session
        && !target.is_empty()
        && sample.session_id != target
    {
        return Decision::SkipForeignSession;
    }
    // Layer B
    if state.last_used_k > 0 {
        let elapsed = now.saturating_sub(state.last_sample_ts);
        if elapsed < 30 && elapsed > 0 {
            let delta_abs = sample.ctx_used_k.abs_diff(state.last_used_k);
            let jump_pct = delta_abs * 100 / state.last_used_k.max(1);
            if jump_pct > 50 {
                return Decision::SkipSanityJump {
                    from: state.last_used_k,
                    to: sample.ctx_used_k,
                    elapsed,
                };
            }
        }
    }
    Decision::Accept
}

#[cfg(test)]
mod tests {
    use super::*;

    fn s(session: &str, used: u64) -> Sample {
        Sample {
            session_id: session.into(),
            model_code: "Op",
            model_ver: 0x48,
            ctx_used_k: used,
            q7d_pct: 39,
            reset_human: "3d11h".into(),
        }
    }

    #[test]
    fn accepts_when_no_target_set() {
        let sample = s("af022cb2", 100);
        let state = LoopState::default();
        assert_eq!(decide(&sample, None, &state, 1_000_000), Decision::Accept);
    }

    #[test]
    fn accepts_when_target_empty() {
        let sample = s("af022cb2", 100);
        let state = LoopState::default();
        assert_eq!(decide(&sample, Some(""), &state, 1_000_000), Decision::Accept);
    }

    #[test]
    fn rejects_foreign_session() {
        let sample = s("deadbeef", 100);
        let state = LoopState::default();
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_000), Decision::SkipForeignSession);
    }

    #[test]
    fn accepts_matching_session() {
        let sample = s("af022cb2", 100);
        let state = LoopState::default();
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_000), Decision::Accept);
    }

    #[test]
    fn rejects_50pct_jump_within_30s() {
        let state = LoopState { last_used_k: 200, last_sample_ts: 1_000_000 };
        let sample = s("af022cb2", 712);
        let d = decide(&sample, Some("af022cb2"), &state, 1_000_005);
        assert_eq!(d, Decision::SkipSanityJump { from: 200, to: 712, elapsed: 5 });
    }

    #[test]
    fn rejects_50pct_drop_within_30s() {
        let state = LoopState { last_used_k: 200, last_sample_ts: 1_000_000 };
        let sample = s("af022cb2", 50); // 75% drop
        let d = decide(&sample, Some("af022cb2"), &state, 1_000_005);
        assert_eq!(d, Decision::SkipSanityJump { from: 200, to: 50, elapsed: 5 });
    }

    #[test]
    fn accepts_small_growth() {
        let state = LoopState { last_used_k: 236, last_sample_ts: 1_000_000 };
        let sample = s("af022cb2", 245); // 4% growth
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_005), Decision::Accept);
    }

    #[test]
    fn accepts_when_elapsed_exceeds_30s() {
        // Even a huge jump is allowed if more than 30s elapsed (genuine session growth).
        let state = LoopState { last_used_k: 200, last_sample_ts: 1_000_000 };
        let sample = s("af022cb2", 712);
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_035), Decision::Accept);
    }

    #[test]
    fn accepts_first_sample_no_prior_state() {
        let state = LoopState::default(); // last_used_k = 0
        let sample = s("af022cb2", 712); // no prior, any value ok
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_000), Decision::Accept);
    }

    #[test]
    fn accepts_when_elapsed_zero() {
        // Same-second case — gate is skipped (avoids divide-by-zero on rapid repeats).
        let state = LoopState { last_used_k: 200, last_sample_ts: 1_000_000 };
        let sample = s("af022cb2", 712);
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_000), Decision::Accept);
    }

    #[test]
    fn rejects_exactly_above_50pct_threshold() {
        let state = LoopState { last_used_k: 100, last_sample_ts: 1_000_000 };
        // 51k delta on 100k base = 51% jump → reject
        let sample = s("af022cb2", 151);
        assert!(matches!(
            decide(&sample, Some("af022cb2"), &state, 1_000_005),
            Decision::SkipSanityJump { .. }
        ));
    }

    #[test]
    fn accepts_exactly_at_50pct_threshold() {
        let state = LoopState { last_used_k: 100, last_sample_ts: 1_000_000 };
        // 50k delta on 100k base = exactly 50% (NOT > 50%) → accept
        let sample = s("af022cb2", 150);
        assert_eq!(decide(&sample, Some("af022cb2"), &state, 1_000_005), Decision::Accept);
    }
}
