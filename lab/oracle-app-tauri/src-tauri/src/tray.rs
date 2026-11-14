//! macOS menu-bar tray (built into Tauri 2.x — no extra crates).
//!
//! Setup runs once on app startup. Builds a menu with:
//!   * payload + last-broadcast timestamp (info-only)
//!   * "Session ▸" submenu listing all detected sessions sorted by recency
//!   * Show/Hide window + Quit
//!
//! Selecting a session updates the shared [`TargetHandle`] and persists
//! it to disk. The broadcaster thread reads the handle on every cycle.

use crate::sessions::{self, SessionEntry};
use crate::state::{self, PersistedState, TargetHandle};
use crate::status::{self, StatusJson, tray_title};
use std::path::PathBuf;
use std::sync::mpsc::{Receiver, Sender, TryRecvError};
use std::time::Duration;
use tauri::menu::{CheckMenuItem, Menu, MenuEvent, MenuItem, PredefinedMenuItem, Submenu};
use tauri::tray::TrayIconBuilder;
use tauri::{AppHandle, Emitter, Manager, Wry};

pub const SESSION_LIMIT: usize = 12;
const SUBMENU_ID: &str = "session-submenu";

pub struct TrayContext {
    pub target: TargetHandle,
    pub status_tx: Sender<StatusJson>,
}

/// Build the tray icon + menu and attach it to the app. Returns
/// immediately; the menu lives for the lifetime of the app.
pub fn install(app: &AppHandle<Wry>, ctx: TrayContext) -> tauri::Result<()> {
    let payload_item = MenuItem::with_id(app, "payload", "📡 waiting for broadcast…", false, None::<&str>)?;
    let last_item = MenuItem::with_id(app, "last", "last: —", false, None::<&str>)?;
    let show_item = MenuItem::with_id(app, "show", "Show window", true, None::<&str>)?;
    let hide_item = MenuItem::with_id(app, "hide", "Hide window", true, None::<&str>)?;
    let quit_item = MenuItem::with_id(app, "quit", "Quit oracle-app", true, None::<&str>)?;

    let session_submenu = build_session_submenu(app, &ctx.target)?;

    let menu = Menu::with_items(
        app,
        &[
            &payload_item,
            &last_item,
            &PredefinedMenuItem::separator(app)?,
            &session_submenu,
            &PredefinedMenuItem::separator(app)?,
            &show_item,
            &hide_item,
            &PredefinedMenuItem::separator(app)?,
            &quit_item,
        ],
    )?;

    let target_for_handler = ctx.target.clone();
    let status_tx_for_handler = ctx.status_tx.clone();
    let _tray = TrayIconBuilder::with_id("oracle-tray")
        .icon(
            app.default_window_icon()
                .cloned()
                .unwrap_or_else(|| tauri::image::Image::from_bytes(include_bytes!("../icons/icon.png")).expect("icon")),
        )
        .menu(&menu)
        .show_menu_on_left_click(true)
        .tooltip("oracle-app: Claude Code status broadcaster")
        .title("ψ starting…")
        .on_menu_event(move |app, event| {
            handle_menu_event(app, event, &target_for_handler, &status_tx_for_handler)
        })
        .build(app)?;

    Ok(())
}

/// Build the "Session ▸" submenu — listing recent sessions with a
/// "Follow active" entry at the top.
fn build_session_submenu(
    app: &AppHandle<Wry>,
    target: &TargetHandle,
) -> tauri::Result<Submenu<Wry>> {
    let sessions = sessions::list(SESSION_LIMIT);
    let current = target.get();

    // "Follow active" is the no-filter mode (target = None).
    let follow_checked = current.is_none();
    let follow_item = CheckMenuItem::with_id(
        app,
        "session:follow",
        "Follow active session (no filter)",
        true,
        follow_checked,
        None::<&str>,
    )?;

    let mut items: Vec<Box<dyn tauri::menu::IsMenuItem<Wry>>> = vec![
        Box::new(follow_item),
        Box::new(PredefinedMenuItem::separator(app)?),
    ];

    if sessions.is_empty() {
        let no_sessions = MenuItem::with_id(
            app,
            "session:none",
            "no sessions found in ~/.claude/projects/",
            false,
            None::<&str>,
        )?;
        items.push(Box::new(no_sessions));
    } else {
        for s in &sessions {
            items.push(Box::new(build_session_item(app, s, current.as_deref())?));
        }
    }

    let item_refs: Vec<&dyn tauri::menu::IsMenuItem<Wry>> = items.iter().map(|b| b.as_ref()).collect();
    Submenu::with_id_and_items(app, SUBMENU_ID, "Session ▸", true, &item_refs)
}

fn build_session_item(
    app: &AppHandle<Wry>,
    s: &SessionEntry,
    current: Option<&str>,
) -> tauri::Result<CheckMenuItem<Wry>> {
    let checked = current == Some(s.id.as_str());
    let label = format!("{}  ·  {}  ·  {}", s.id, s.repo_label, s.age_human);
    CheckMenuItem::with_id(
        app,
        format!("session:{}", s.id),
        label,
        true,
        checked,
        None::<&str>,
    )
}

fn handle_menu_event(
    app: &AppHandle<Wry>,
    event: MenuEvent,
    target: &TargetHandle,
    status_tx: &Sender<StatusJson>,
) {
    let id = event.id.as_ref();
    match id {
        "quit" => app.exit(0),
        "show" => {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.show();
                let _ = window.set_focus();
            }
        }
        "hide" => {
            if let Some(window) = app.get_webview_window("main") {
                let _ = window.hide();
            }
        }
        id if id.starts_with("session:") => {
            let suffix = &id["session:".len()..];
            let new_target = if suffix == "follow" { None } else { Some(suffix.to_string()) };
            eprintln!("tray: selected session = {new_target:?}");
            target.set(new_target.clone());
            if let Err(e) = state::save(&PersistedState { target_session: new_target.clone() }) {
                eprintln!("warn: couldn't persist session selection: {e}");
            }
            // Push an immediate snapshot so window + tray title update
            // within ms instead of waiting for the broadcaster cycle.
            if let Some(snap) = immediate_snapshot(new_target.as_deref()) {
                let _ = status_tx.send(snap);
            }
            rebuild_menu(app, target);
        }
        _ => {}
    }
}

/// Build a fresh StatusJson the moment the user picks a session, so the
/// UI updates immediately. For pinned mode reads the session's .jsonl
/// + account-wide q7d from raw.json. For follow mode just snapshots
/// raw.json.
fn immediate_snapshot(target: Option<&str>) -> Option<StatusJson> {
    let raw_path = PathBuf::from(crate::broadcaster::DEFAULT_JSON);
    match target {
        None => status::snapshot(&raw_path).ok(),
        Some(id) => build_pinned_status(id, &raw_path),
    }
}

fn build_pinned_status(session_id_prefix: &str, raw_path: &std::path::Path) -> Option<StatusJson> {
    let usage = sessions::latest_usage(session_id_prefix)?;
    let raw = oracle_beacon::read_statusline(raw_path).ok()?;
    let model = oracle_beacon::Model {
        display_name: Some(usage.model_display.clone()),
        id: None,
    };
    let code = oracle_beacon::model_code(&model);
    let model_ver = oracle_beacon::parse_model_version(&usage.model_display);
    let max_k = oracle_beacon::model_max_k(code);
    let ctx_pct = if max_k > 0 { usage.ctx_used_k * 100 / max_k } else { 0 };
    let reset = oracle_beacon::fmt_reset(raw.rate_limits.seven_day.resets_at, oracle_beacon::now_secs());
    Some(StatusJson {
        model_code: code.to_string(),
        model_friendly: status::friendly_name(code, model_ver),
        ctx_used_k: usage.ctx_used_k,
        ctx_max_k: max_k,
        ctx_pct,
        q7d_pct: raw.rate_limits.seven_day.used_percentage,
        reset_human: reset,
        session_id: session_id_prefix.to_string(),
        project: sessions::extract_project_label_from_jsonl_dir(session_id_prefix)
            .unwrap_or_else(|| "active".to_string()),
    })
}

fn rebuild_menu(app: &AppHandle<Wry>, target: &TargetHandle) {
    if let Some(tray) = app.tray_by_id("oracle-tray")
        && let Ok(new_menu) = build_full_menu(app, target)
    {
        let _ = tray.set_menu(Some(new_menu));
    }
}

fn build_full_menu(app: &AppHandle<Wry>, target: &TargetHandle) -> tauri::Result<Menu<Wry>> {
    let payload_item = MenuItem::with_id(app, "payload", "📡 waiting for broadcast…", false, None::<&str>)?;
    let last_item = MenuItem::with_id(app, "last", "last: —", false, None::<&str>)?;
    let show_item = MenuItem::with_id(app, "show", "Show window", true, None::<&str>)?;
    let hide_item = MenuItem::with_id(app, "hide", "Hide window", true, None::<&str>)?;
    let quit_item = MenuItem::with_id(app, "quit", "Quit oracle-app", true, None::<&str>)?;
    let session_submenu = build_session_submenu(app, target)?;
    Menu::with_items(
        app,
        &[
            &payload_item,
            &last_item,
            &PredefinedMenuItem::separator(app)?,
            &session_submenu,
            &PredefinedMenuItem::separator(app)?,
            &show_item,
            &hide_item,
            &PredefinedMenuItem::separator(app)?,
            &quit_item,
        ],
    )
}

/// Drain `status_rx` and apply each StatusJson to the tray title + emit to JS.
pub fn spawn_pump(app: AppHandle<Wry>, status_rx: Receiver<StatusJson>) {
    std::thread::spawn(move || {
        loop {
            match status_rx.try_recv() {
                Ok(status) => {
                    if let Some(tray) = app.tray_by_id("oracle-tray") {
                        let _ = tray.set_title(Some(tray_title(&status)));
                    }
                    let _ = app.emit("status-update", &status);
                }
                Err(TryRecvError::Empty) => std::thread::sleep(Duration::from_millis(500)),
                Err(TryRecvError::Disconnected) => return,
            }
        }
    });
}
