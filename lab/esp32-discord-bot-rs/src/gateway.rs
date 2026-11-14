//! The Discord gateway (v10, JSON): log the bot in, keep the heartbeat, ask to join one voice
//! channel (op 4), and hand the voice link what it needs: our voice session id
//! (VOICE_STATE_UPDATE) and the voice server's token and endpoint (VOICE_SERVER_UPDATE).

use std::sync::mpsc::{channel, RecvTimeoutError};
use std::thread::sleep;
use std::time::{Duration, Instant};

use log::{error, info, warn};
use serde_json::{json, Value};

use crate::ws::{Msg, Ws};

const URL: &str = "wss://gateway.discord.gg/?v=10&encoding=json";
/// GUILD_VOICE_STATES (1 << 7): our own voice state. Nothing else: no message content, no members.
const INTENTS: u64 = 1 << 7;

pub struct Settings {
    pub token: String,
    pub guild_id: String,
    pub channel_id: String,
}

#[derive(Clone, Debug)]
pub struct VoiceCreds {
    pub user_id: String,
    pub guild_id: String,
    pub channel_id: String,
    pub session_id: String,
    pub token: String,
    pub endpoint: String,
}

enum Ended {
    Reconnect,
    /// a close code after which logging in again cannot help (bad token, intents, version)
    Fatal(u16),
}

fn fatal(code: u16) -> bool {
    code == 4004 || (4010..=4014).contains(&code)
}

/// Runs for ever: connects, and on any drop waits and logs in again (no resume: this is a lab).
pub fn run(settings: Settings, on_voice: impl Fn(VoiceCreds)) -> ! {
    let mut backoff = Duration::from_secs(3);
    loop {
        match session(&settings, &on_voice) {
            Ok(Ended::Reconnect) => backoff = Duration::from_secs(3),
            Ok(Ended::Fatal(code)) => {
                error!("gateway refused us ({code}): check the token and intents; next try in 10 min");
                sleep(Duration::from_secs(600));
                continue;
            }
            Err(e) => warn!("gateway: {e:#}"),
        }
        sleep(backoff);
        backoff = (backoff * 2).min(Duration::from_secs(120));
    }
}

fn session(s: &Settings, on_voice: &impl Fn(VoiceCreds)) -> anyhow::Result<Ended> {
    let (tx, rx) = channel();
    let ws = Ws::connect(URL, tx)?;
    let mut seq: Option<u64> = None;
    let mut beat: Option<(Duration, Instant)> = None;
    let mut user_id = String::new();
    let mut voice_session: Option<String> = None;
    let mut voice_server: Option<(String, String)> = None;
    loop {
        let wait = beat.map_or(Duration::from_secs(60), |(_, next)| next.saturating_duration_since(Instant::now()));
        match rx.recv_timeout(wait) {
            Ok(Msg::Text(text)) => {
                let Ok(m) = serde_json::from_str::<Value>(&text) else { continue };
                if let Some(n) = m["s"].as_u64() {
                    seq = Some(n);
                }
                let d = &m["d"];
                match m["op"].as_u64() {
                    Some(10) => {
                        let iv = Duration::from_millis(d["heartbeat_interval"].as_u64().unwrap_or(41_250));
                        let jitter = unsafe { esp_idf_svc::sys::esp_random() } % 1000;
                        beat = Some((iv, Instant::now() + iv * jitter / 1000));
                        ws.send_text(
                            &json!({"op": 2, "d": {"token": s.token, "intents": INTENTS,
                                "properties": {"os": "esp-idf", "browser": "oraclepet", "device": "oraclepet"}}})
                            .to_string(),
                        )?;
                        info!("gateway: hello, identifying");
                    }
                    Some(0) => match m["t"].as_str().unwrap_or("") {
                        "READY" => {
                            user_id = d["user"]["id"].as_str().unwrap_or("").to_string();
                            info!("gateway: READY as {} ({user_id})", d["user"]["username"].as_str().unwrap_or("?"));
                            ws.send_text(
                                &json!({"op": 4, "d": {"guild_id": s.guild_id, "channel_id": s.channel_id,
                                    "self_mute": false, "self_deaf": true}})
                                .to_string(),
                            )?;
                        }
                        "VOICE_STATE_UPDATE" if d["user_id"] == user_id.as_str() && d["guild_id"] == s.guild_id.as_str() => {
                            if d["channel_id"].is_null() {
                                warn!("gateway: we are out of the voice channel (kicked or moved out)");
                                voice_session = None;
                            } else {
                                voice_session = d["session_id"].as_str().map(str::to_string);
                            }
                        }
                        "VOICE_SERVER_UPDATE" if d["guild_id"] == s.guild_id.as_str() => {
                            match (d["token"].as_str(), d["endpoint"].as_str()) {
                                (Some(t), Some(e)) => voice_server = Some((t.to_string(), e.to_string())),
                                _ => warn!("gateway: voice server update without an endpoint (server moving)"),
                            }
                        }
                        _ => {}
                    },
                    Some(1) => send_beat(&ws, seq)?,
                    Some(7) => return Ok(Ended::Reconnect),
                    Some(9) => {
                        warn!("gateway: invalid session");
                        sleep(Duration::from_secs(3));
                        return Ok(Ended::Reconnect);
                    }
                    _ => {} // 11: heartbeat ACK
                }
                if let (Some(sid), Some((token, endpoint))) = (&voice_session, &voice_server) {
                    info!("gateway: voice server {endpoint}");
                    on_voice(VoiceCreds {
                        user_id: user_id.clone(),
                        guild_id: s.guild_id.clone(),
                        channel_id: s.channel_id.clone(),
                        session_id: sid.clone(),
                        token: token.clone(),
                        endpoint: endpoint.clone(),
                    });
                    voice_server = None; // a new server update starts a new link
                }
            }
            Ok(Msg::Closed { code, reason }) => {
                warn!("gateway closed: {code} {reason}");
                return Ok(if fatal(code) { Ended::Fatal(code) } else { Ended::Reconnect });
            }
            Ok(_) => {}
            Err(RecvTimeoutError::Timeout) => {}
            Err(RecvTimeoutError::Disconnected) => anyhow::bail!("gateway socket gone"),
        }
        if let Some((iv, next)) = beat {
            if Instant::now() >= next {
                send_beat(&ws, seq)?;
                beat = Some((iv, Instant::now() + iv));
            }
        }
    }
}

fn send_beat(ws: &Ws, seq: Option<u64>) -> anyhow::Result<()> {
    ws.send_text(&json!({"op": 1, "d": seq}).to_string())
}
