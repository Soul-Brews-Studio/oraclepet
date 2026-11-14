//! oracle-beacon — shared logic for the CLI, tray, and WASI binaries.
//!
//! Modules are kept ≤ 350 lines each; the lib.rs is just declarations
//! and re-exports. See `README.md` for the wire format and
//! architecture overview.

#[cfg(target_os = "macos")]
pub mod ble_macos;

pub mod advertise;
pub mod base64_codec;
pub mod binary;
pub mod decide;
pub mod format;
pub mod io;
pub mod sample;
pub mod types;

// Re-export the small public API so callers can `use oracle_beacon::*`
// and get everything they need without diving into submodules.
pub use advertise::advertise;
pub use decide::{Decision, decide};
pub use format::{
    fmt_model_version, fmt_reset, model_code, model_family, model_max_k, parse_model_version,
    short_project,
};
pub use io::{auto_session, extract_sample, now_secs, read_statusline};
pub use sample::{LoopState, Sample};
pub use types::{ContextWindow, CurrentUsage, Model, RateLimits, SevenDay, Statusline};
