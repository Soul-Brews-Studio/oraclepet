//! oracle-app-tauri — entry point. Spawns the BLE broadcaster and
//! installs the tray on setup; the frontend polls `get_status` via the
//! tauri command + listens for `status-update` events. Tray submenu
//! lets the user switch which Claude Code session is broadcast.
//!
//! No MQTT anywhere — just BLE + tray.

pub mod broadcaster;
pub mod sessions;
pub mod state;
pub mod status;
pub mod tray;

use crate::state::TargetHandle;
use crate::status::{StatusJson, snapshot};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::AtomicBool;
use std::sync::mpsc::channel;
use std::thread;

#[tauri::command]
fn get_status() -> Result<StatusJson, String> {
    let path = std::env::var("CLAUDE_STATUSLINE_JSON")
        .map(PathBuf::from)
        .unwrap_or_else(|_| PathBuf::from(broadcaster::DEFAULT_JSON));
    snapshot(&path)
}

#[tauri::command]
fn list_sessions() -> Vec<sessions::SessionEntry> {
    sessions::list(tray::SESSION_LIMIT)
}

#[cfg_attr(mobile, tauri::mobile_entry_point)]
pub fn run() {
    // Initial target: prefer persisted selection, else auto-detect from cwd.
    let persisted = state::load();
    let initial_target = persisted
        .target_session
        .or_else(oracle_beacon::auto_session);
    let target = TargetHandle::new(initial_target);

    tauri::Builder::default()
        .invoke_handler(tauri::generate_handler![get_status, list_sessions])
        .setup({
            let target = target.clone();
            move |app| {
                let app_handle = app.handle().clone();

                // Spawn the BLE broadcaster thread.
                let (status_tx, status_rx) = channel::<StatusJson>();
                let quit = Arc::new(AtomicBool::new(false));
                let cfg = broadcaster::BroadcasterConfig {
                    json_path: std::env::var("CLAUDE_STATUSLINE_JSON")
                        .map(PathBuf::from)
                        .unwrap_or_else(|_| PathBuf::from(broadcaster::DEFAULT_JSON)),
                    target: target.clone(),
                    status_tx: status_tx.clone(),
                    quit: Arc::clone(&quit),
                };
                thread::spawn(move || broadcaster::run(cfg));

                tray::install(
                    &app_handle,
                    tray::TrayContext { target: target.clone(), status_tx: status_tx.clone() },
                )?;
                tray::spawn_pump(app_handle.clone(), status_rx);

                Ok(())
            }
        })
        .on_window_event(|window, event| {
            if let tauri::WindowEvent::CloseRequested { api, .. } = event {
                let _ = window.hide();
                api.prevent_close();
            }
        })
        .run(tauri::generate_context!())
        .expect("error while running tauri application");
}
