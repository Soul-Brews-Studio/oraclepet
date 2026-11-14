//! The accepted-sample value plus the loop's running state.

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Sample {
    pub session_id: String,
    pub model_code: &'static str,
    /// Packed model version `major<<4 | minor` (e.g. 0x48 = 4.8), or 0 if
    /// the display name carried no version. Travels over the wire so the
    /// device shows the live version without a firmware reflash.
    pub model_ver: u8,
    pub ctx_used_k: u64,
    pub q7d_pct: u64,
    pub reset_human: String,
}

impl Sample {
    /// Pipe-delimited payload, no `oracle:` BLE prefix.
    pub fn payload(&self) -> String {
        format!(
            "{}|{}|{}|{}",
            self.model_code, self.ctx_used_k, self.q7d_pct, self.reset_human
        )
    }

    /// Compact menu-bar title (≤ ~22 ASCII chars so macOS doesn't elide).
    pub fn tray_title(&self, max_k: u64) -> String {
        let ctx_pct = if max_k > 0 {
            self.ctx_used_k * 100 / max_k.max(1)
        } else {
            0
        };
        format!("ψ {} {}% q{}%", self.model_code, ctx_pct, self.q7d_pct)
    }
}

/// Loop state — the last accepted sample's used_k + timestamp.
/// Drives the sanity gate.
#[derive(Default, Debug, Clone, Copy)]
pub struct LoopState {
    pub last_used_k: u64,
    pub last_sample_ts: u64,
}

#[cfg(test)]
mod tests {
    use super::*;

    fn sample(used: u64, q7d: u64) -> Sample {
        Sample {
            session_id: "af022cb2".into(),
            model_code: "Op",
            model_ver: 0x48,
            ctx_used_k: used,
            q7d_pct: q7d,
            reset_human: "3d11h".into(),
        }
    }

    #[test]
    fn payload_format_is_pipe_delimited() {
        let s = sample(236, 39);
        assert_eq!(s.payload(), "Op|236|39|3d11h");
    }

    #[test]
    fn payload_total_fits_ble_budget_for_typical_values() {
        let s = sample(236, 39);
        let total = format!("oracle:{}", s.payload());
        assert!(total.len() <= 26, "oracle:{} overflows 26-byte budget", s.payload());
    }

    #[test]
    fn payload_handles_max_values() {
        let mut s = sample(999, 100);
        s.reset_human = "99d23h".into();
        let total = format!("oracle:{}", s.payload());
        // Op|999|100|99d23h → 17 bytes + oracle: 7 = 24 ≤ 26 ✓
        assert!(total.len() <= 26, "worst case oracle:{} overflows", s.payload());
    }

    #[test]
    fn tray_title_short_format() {
        let s = sample(236, 39);
        assert_eq!(s.tray_title(1000), "ψ Op 23% q39%");
    }

    #[test]
    fn tray_title_zero_max_handled() {
        let s = sample(236, 39);
        assert!(s.tray_title(0).contains("0%"));
    }

    #[test]
    fn tray_title_caps_at_22_bytes() {
        // Worst case: Ha|999k context with 100% & 100%
        let s = Sample {
            session_id: "abc".into(),
            model_code: "Ha",
            model_ver: 0x45,
            ctx_used_k: 200,
            q7d_pct: 100,
            reset_human: "now".into(),
        };
        let t = s.tray_title(200);
        assert!(t.len() <= 22, "tray title too long: {t:?} ({} bytes)", t.len());
    }

    #[test]
    fn loop_state_default_is_zero() {
        let s = LoopState::default();
        assert_eq!(s.last_used_k, 0);
        assert_eq!(s.last_sample_ts, 0);
    }

    #[test]
    fn sample_clone_works() {
        let s = sample(100, 50);
        let cloned = s.clone();
        assert_eq!(s, cloned);
    }
}
