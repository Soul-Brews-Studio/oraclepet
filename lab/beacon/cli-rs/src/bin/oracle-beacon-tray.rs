// oracle-beacon-tray — macOS menu-bar app that broadcasts Claude Code
// status over BLE and shows the live values in the menu bar.
//
// Architecture:
//   * Main thread runs winit's EventLoop + tray-icon's macOS shim
//   * Background thread runs the read+decide+advertise loop
//   * Channels:
//       - sample_rx: latest accepted Sample (broadcaster → tray)
//       - quit signal via Arc<AtomicBool> (tray → broadcaster)
//
// The tray title updates after every accepted broadcast. The menu shows
// the current payload, last broadcast timestamp, and a Quit item.

use anyhow::Result;
use oracle_beacon::{
    Decision, LoopState, Sample, advertise, auto_session, decide, extract_sample, model_max_k,
    now_secs, read_statusline,
};
use std::path::PathBuf;
use std::sync::Arc;
use std::sync::atomic::{AtomicBool, Ordering};
use std::sync::mpsc::{Receiver, Sender, TryRecvError, channel};
use std::thread;
use std::time::{Duration, Instant};

use tray_icon::menu::{Menu, MenuEvent, MenuItem, PredefinedMenuItem};
use tray_icon::{Icon, TrayIcon, TrayIconBuilder};
use winit::application::ApplicationHandler;
use winit::event::StartCause;
use winit::event_loop::{ActiveEventLoop, ControlFlow, EventLoop};
use winit::window::WindowId;

// ── Config (could move to CLI args later) ────────────────────────────────────
const JSON_PATH: &str = "/tmp/claude-statusline/raw.json";
const DURATION_SECS: u64 = 60;
const LOOP_EVERY_SECS: u64 = 30;
const POLL_TRAY_EVERY: Duration = Duration::from_millis(500);

// ── Broadcaster thread ───────────────────────────────────────────────────────

fn broadcaster_thread(
    target: Option<String>,
    sample_tx: Sender<Sample>,
    quit: Arc<AtomicBool>,
) -> Result<()> {
    let mut state = LoopState::default();
    let json_path = PathBuf::from(JSON_PATH);
    while !quit.load(Ordering::Relaxed) {
        let now = now_secs();
        match read_statusline(&json_path) {
            Ok(statusline) => {
                let sample = extract_sample(&statusline, now);
                match decide(&sample, target.as_deref(), &state, now) {
                    Decision::Accept => {
                        state.last_used_k = sample.ctx_used_k;
                        state.last_sample_ts = now;
                        let payload = sample.payload();
                        eprintln!("broadcasting: oracle:{payload} ({} bytes)", payload.len());
                        let _ = sample_tx.send(sample);
                        if let Err(e) = advertise(&payload, DURATION_SECS) {
                            eprintln!("advertise error: {e:#}");
                        }
                    }
                    Decision::SkipForeignSession => {
                        eprintln!("skip: foreign session {}", sample.session_id);
                    }
                    Decision::SkipSanityJump { from, to, elapsed } => {
                        eprintln!("skip: jump {from}k→{to}k in {elapsed}s");
                    }
                }
            }
            Err(e) => eprintln!("read error: {e:#}"),
        }
        // Sleep with periodic quit-check so we don't block shutdown.
        let deadline = Instant::now() + Duration::from_secs(LOOP_EVERY_SECS);
        while Instant::now() < deadline {
            if quit.load(Ordering::Relaxed) {
                return Ok(());
            }
            thread::sleep(Duration::from_millis(200));
        }
    }
    Ok(())
}

// ── Tray icon (16×16 RGBA) — embedded generated solid square ────────────────
// Keeps the binary self-contained. Color = ψ-amber 0xFFD27F.
fn build_icon() -> Icon {
    let size = 22usize;
    let mut rgba = Vec::with_capacity(size * size * 4);
    // Solid amber rounded square with darker border.
    for y in 0..size {
        for x in 0..size {
            let dx = (x as i32 - size as i32 / 2).abs();
            let dy = (y as i32 - size as i32 / 2).abs();
            let max = (size as i32 / 2) - 1;
            let border = dx == max || dy == max;
            let (r, g, b, a) = if border { (0x88, 0x66, 0x33, 0xFF) } else { (0xFF, 0xD2, 0x7F, 0xFF) };
            rgba.extend_from_slice(&[r, g, b, a]);
        }
    }
    Icon::from_rgba(rgba, size as u32, size as u32).expect("16x16 rgba is always valid")
}

// ── winit Application ────────────────────────────────────────────────────────

struct TrayApp {
    _tray: TrayIcon,
    sample_rx: Receiver<Sample>,
    quit: Arc<AtomicBool>,
    payload_item: MenuItem,
    last_recv_item: MenuItem,
    quit_id: tray_icon::menu::MenuId,
}

impl ApplicationHandler for TrayApp {
    fn resumed(&mut self, _event_loop: &ActiveEventLoop) {
        // No window — pure tray app.
    }

    fn new_events(&mut self, event_loop: &ActiveEventLoop, cause: StartCause) {
        // Poll tray + broadcaster channels on each tick.
        if matches!(cause, StartCause::Init | StartCause::Poll | StartCause::ResumeTimeReached { .. }) {
            // Drain sample updates from broadcaster
            loop {
                match self.sample_rx.try_recv() {
                    Ok(sample) => {
                        let max_k = model_max_k(sample.model_code);
                        let title = sample.tray_title(max_k);
                        self._tray.set_title(Some(&title));
                        self.payload_item.set_text(format!("📡 oracle:{}", sample.payload()));
                        let now = chrono_now_string();
                        self.last_recv_item.set_text(format!("last broadcast: {now}"));
                    }
                    Err(TryRecvError::Empty) => break,
                    Err(TryRecvError::Disconnected) => break,
                }
            }
            // Drain menu events
            while let Ok(ev) = MenuEvent::receiver().try_recv() {
                if ev.id == self.quit_id {
                    self.quit.store(true, Ordering::Relaxed);
                    event_loop.exit();
                    return;
                }
            }
            // Schedule next poll
            event_loop.set_control_flow(ControlFlow::WaitUntil(Instant::now() + POLL_TRAY_EVERY));
        }
    }

    fn window_event(
        &mut self,
        _event_loop: &ActiveEventLoop,
        _id: WindowId,
        _event: winit::event::WindowEvent,
    ) {
        // No windows.
    }
}

/// Minimal HH:MM:SS clock without pulling chrono — uses std SystemTime.
fn chrono_now_string() -> String {
    use std::time::{SystemTime, UNIX_EPOCH};
    let secs = SystemTime::now().duration_since(UNIX_EPOCH).map(|d| d.as_secs()).unwrap_or(0);
    let hours = (secs / 3600) % 24;
    let mins = (secs / 60) % 60;
    let s = secs % 60;
    // local time = UTC + 7 (GMT+7); good enough for menu display
    let hours_local = (hours + 7) % 24;
    format!("{hours_local:02}:{mins:02}:{s:02}")
}

// ── main ────────────────────────────────────────────────────────────────────

fn main() -> Result<()> {
    let target = auto_session();
    if let Some(ref t) = target {
        eprintln!("target session: {t}");
    } else {
        eprintln!("target session: <none>");
    }

    // Channels
    let (sample_tx, sample_rx) = channel::<Sample>();
    let quit = Arc::new(AtomicBool::new(false));

    // Background broadcaster
    {
        let quit = Arc::clone(&quit);
        let target = target.clone();
        thread::spawn(move || {
            if let Err(e) = broadcaster_thread(target, sample_tx, quit) {
                eprintln!("broadcaster fatal: {e:#}");
            }
        });
    }

    // Tray menu
    let menu = Menu::new();
    let payload_item = MenuItem::new("📡 waiting for broadcast...", false, None);
    let last_recv_item = MenuItem::new("last broadcast: —", false, None);
    let quit_item = MenuItem::new("Quit oracle-beacon", true, None);
    let quit_id = quit_item.id().clone();
    menu.append_items(&[
        &payload_item,
        &last_recv_item,
        &PredefinedMenuItem::separator(),
        &quit_item,
    ])
    .expect("menu append");

    let tray = TrayIconBuilder::new()
        .with_menu(Box::new(menu))
        .with_tooltip("oracle-beacon: Claude Code status broadcaster")
        .with_title("ψ starting...")
        .with_icon(build_icon())
        .build()?;

    let event_loop = EventLoop::builder().build()?;
    event_loop.set_control_flow(ControlFlow::Wait);

    let mut app = TrayApp {
        _tray: tray,
        sample_rx,
        quit: Arc::clone(&quit),
        payload_item,
        last_recv_item,
        quit_id,
    };

    event_loop.run_app(&mut app)?;
    quit.store(true, Ordering::Relaxed);
    Ok(())
}
