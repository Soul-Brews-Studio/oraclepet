//! Shared mutable target session — set by the tray menu, read by the
//! broadcaster thread. Persists to `~/.config/oracle-app-tauri/state.json`
//! so the selection survives restarts.

use serde::{Deserialize, Serialize};
use std::path::PathBuf;
use std::sync::{Arc, Mutex};

#[derive(Debug, Clone, Serialize, Deserialize, Default)]
pub struct PersistedState {
    /// First 8 chars of session_id, or None for "follow active" (no filter).
    pub target_session: Option<String>,
}

/// Thread-safe handle the tray + broadcaster both clone.
#[derive(Clone)]
pub struct TargetHandle {
    inner: Arc<Mutex<Option<String>>>,
}

impl TargetHandle {
    pub fn new(initial: Option<String>) -> Self {
        Self { inner: Arc::new(Mutex::new(initial)) }
    }

    pub fn get(&self) -> Option<String> {
        self.inner.lock().expect("target-session mutex").clone()
    }

    pub fn set(&self, v: Option<String>) {
        *self.inner.lock().expect("target-session mutex") = v;
    }
}

pub fn config_path() -> PathBuf {
    let home = std::env::var_os("HOME")
        .map(PathBuf::from)
        .unwrap_or_else(|| PathBuf::from("."));
    home.join(".config/oracle-app-tauri/state.json")
}

pub fn load() -> PersistedState {
    let path = config_path();
    std::fs::read(&path)
        .ok()
        .and_then(|bytes| serde_json::from_slice::<PersistedState>(&bytes).ok())
        .unwrap_or_default()
}

pub fn save(state: &PersistedState) -> std::io::Result<()> {
    let path = config_path();
    if let Some(parent) = path.parent() {
        std::fs::create_dir_all(parent)?;
    }
    let bytes = serde_json::to_vec_pretty(state).map_err(std::io::Error::other)?;
    std::fs::write(&path, bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn handle_round_trips() {
        let h = TargetHandle::new(Some("af022cb2".into()));
        assert_eq!(h.get(), Some("af022cb2".into()));
        h.set(Some("deadbeef".into()));
        assert_eq!(h.get(), Some("deadbeef".into()));
        h.set(None);
        assert_eq!(h.get(), None);
    }

    #[test]
    fn handle_clone_shares_state() {
        let a = TargetHandle::new(None);
        let b = a.clone();
        a.set(Some("xyz".into()));
        assert_eq!(b.get(), Some("xyz".into()));
    }

    #[test]
    fn persisted_state_serde_round_trip() {
        let s = PersistedState { target_session: Some("af022cb2".into()) };
        let j = serde_json::to_string(&s).unwrap();
        assert!(j.contains("af022cb2"));
        let back: PersistedState = serde_json::from_str(&j).unwrap();
        assert_eq!(back.target_session, Some("af022cb2".into()));
    }

    #[test]
    fn persisted_default_is_none() {
        assert_eq!(PersistedState::default().target_session, None);
    }

    #[test]
    fn load_returns_default_when_no_file() {
        // Pretend HOME is somewhere with no state.json — load returns default.
        let saved_home = std::env::var_os("HOME");
        // SAFETY: single-test access to env; tests in this module are not parallel-hostile.
        unsafe {
            std::env::set_var("HOME", "/tmp/oracle-app-state-no-such-dir-99999");
        }
        let s = load();
        assert_eq!(s.target_session, None);
        // Restore
        unsafe {
            if let Some(h) = saved_home {
                std::env::set_var("HOME", h);
            } else {
                std::env::remove_var("HOME");
            }
        }
    }
}
