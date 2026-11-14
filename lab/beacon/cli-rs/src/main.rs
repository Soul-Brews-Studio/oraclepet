// oracle-beacon CLI — thin entry point. All logic lives in the lib crate.

use anyhow::Result;
use clap::Parser;
use oracle_beacon::{
    Decision, LoopState, advertise, auto_session, decide, extract_sample, now_secs, read_statusline,
};
use std::path::PathBuf;
use std::thread::sleep;
use std::time::Duration;

/// Claude Code BLE status broadcaster.
#[derive(Parser, Debug)]
#[command(version, about, long_about = None)]
struct Args {
    /// Path to Claude Code's statusline JSON cache.
    #[arg(long, env = "CLAUDE_STATUSLINE_JSON", default_value = "/tmp/claude-statusline/raw.json")]
    json: PathBuf,

    /// Target session ID prefix (first 8 chars). If empty, broadcasts any
    /// session's data. Defaults to auto-detect from cwd.
    #[arg(long, env = "BROADCAST_SESSION")]
    session: Option<String>,

    /// How long to advertise per broadcast (seconds).
    #[arg(long, env = "BLE_DURATION", default_value_t = 60)]
    duration: u64,

    /// Loop forever, re-reading + re-broadcasting every N seconds.
    #[arg(long)]
    r#loop: bool,

    /// In loop mode, seconds between broadcast restarts.
    #[arg(long, env = "LOOP_EVERY", default_value_t = 30)]
    loop_every: u64,

    /// Don't actually advertise; print what would be sent. Always on for WASM.
    #[arg(long)]
    dry_run: bool,
}

fn run_once(args: &Args, state: &mut LoopState, target: Option<&str>) -> Result<()> {
    let statusline = read_statusline(&args.json)?;
    let now = now_secs();
    let sample = extract_sample(&statusline, now);

    match decide(&sample, target, state, now) {
        Decision::Accept => {
            state.last_used_k = sample.ctx_used_k;
            state.last_sample_ts = now;
            let payload = sample.payload();
            eprintln!(
                "broadcasting: oracle:{} ({} chars, {}s window)",
                payload,
                payload.chars().count(),
                args.duration
            );
            if args.dry_run {
                println!("oracle:{payload}");
                Ok(())
            } else {
                advertise(&payload, args.duration)
            }
        }
        Decision::SkipForeignSession => {
            eprintln!(
                "skip: foreign session {} (want {}), used={}k",
                sample.session_id,
                target.unwrap_or(""),
                sample.ctx_used_k
            );
            Ok(())
        }
        Decision::SkipSanityJump { from, to, elapsed } => {
            let jump_pct = to.abs_diff(from) * 100 / from.max(1);
            eprintln!("skip: ctx jumped {jump_pct}% ({from}k → {to}k) in {elapsed}s — likely race");
            Ok(())
        }
    }
}

fn main() -> Result<()> {
    let args = Args::parse();

    // Force dry-run on WASM/WASI — no native BLE there.
    #[cfg(target_family = "wasm")]
    let args = Args { dry_run: true, ..args };

    let target = args.session.clone().or_else(auto_session);
    if let Some(ref t) = target {
        eprintln!("target session: {t}");
    } else {
        eprintln!("target session: <none — will broadcast any session that writes the JSON>");
    }

    let mut state = LoopState::default();

    if args.r#loop {
        eprintln!("loop mode — refreshing every {}s. Ctrl-C to stop.", args.loop_every);
        loop {
            if let Err(e) = run_once(&args, &mut state, target.as_deref()) {
                eprintln!("cycle error: {e:#}");
            }
            sleep(Duration::from_secs(args.loop_every));
        }
    } else {
        run_once(&args, &mut state, target.as_deref())
    }
}
