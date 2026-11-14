//! The Discord voice link, as @discordjs/voice 0.19.2 does it:
//!
//!   wss://<endpoint>/?v=8   op 0 Identify (max_dave_protocol_version 1) → op 8 Hello → op 2 Ready
//!   UDP                     IP discovery (74 bytes) → op 1 Select Protocol (AES-GCM or XChaCha20)
//!                           → op 4 Session Description (secret_key, dave_protocol_version)
//!   DAVE                    our key package (26) → external sender (25) → proposals (27) → our
//!                           commit (28) → announce commit (29) or welcome (30) → ready (23) → execute (22)
//!   media, every 20 ms      Opus → DAVE frame encryption → RTP + transport AEAD → UDP
//!
//! One thread owns the voice WebSocket; `send_opus` runs on the audio thread and shares the media
//! state (UDP socket, RTP counters, DAVE session) through a mutex.

use std::collections::HashSet;
use std::net::UdpSocket;
use std::sync::mpsc::{channel, Receiver, RecvTimeoutError, Sender};
use std::sync::{Arc, Mutex};
use std::time::{Duration, Instant};

use log::{info, warn};
use serde_json::{json, Value};

use crate::crypto::{self, Cipher, RtpStream};
use crate::dave::{self, Dave, Reply};
use crate::gateway::VoiceCreds;
use crate::ws::{Msg, Ws};

const SILENCE: [u8; 3] = [0xF8, 0xFF, 0xFE];

struct Media {
    udp: UdpSocket,
    rtp: RtpStream,
    dave: Dave,
    speaking: bool,
    ws: Arc<Ws>,
    keepalive: u32,
}

pub struct Voice {
    media: Mutex<Option<Media>>,
    creds_tx: Mutex<Sender<VoiceCreds>>,
}

enum Ended {
    NewCreds(VoiceCreds),
    Closed(u16),
}

impl Voice {
    /// Starts the voice thread; it waits for credentials from the gateway.
    pub fn start() -> anyhow::Result<Arc<Voice>> {
        let (tx, rx) = channel();
        let v = Arc::new(Voice { media: Mutex::new(None), creds_tx: Mutex::new(tx) });
        let me = v.clone();
        // DAVE's MLS work (P-256, HPKE, commits) runs on this thread: a big stack (PSRAM)
        std::thread::Builder::new().name("voice".into()).stack_size(96 * 1024).spawn(move || me.run(rx))?;
        Ok(v)
    }

    /// New credentials from the gateway: (re)connect with them.
    pub fn connect(&self, creds: VoiceCreds) {
        let _ = self.creds_tx.lock().unwrap().send(creds);
    }

    pub fn ready(&self) -> bool {
        self.media.lock().unwrap().is_some()
    }

    /// One 20 ms Opus frame: DAVE (when on), RTP, transport encryption, UDP.
    pub fn send_opus(&self, opus: &[u8]) {
        let mut guard = self.media.lock().unwrap();
        let Some(m) = guard.as_mut() else { return };
        if !m.speaking {
            m.speaking = true;
            let _ = m.ws.send_text(&speaking(1, m.rtp.ssrc));
        }
        let payload = m.dave.encrypt(opus);
        match m.rtp.seal(&payload) {
            Ok(packet) => {
                let _ = m.udp.send(&packet);
            }
            Err(e) => warn!("voice: {e}"),
        }
    }

    /// Speaking off: five silence frames, then the flag.
    pub fn silence(&self) {
        let mut guard = self.media.lock().unwrap();
        let Some(m) = guard.as_mut() else { return };
        if !m.speaking {
            return;
        }
        for _ in 0..5 {
            if let Ok(packet) = m.rtp.seal(&SILENCE) {
                let _ = m.udp.send(&packet);
            }
        }
        m.speaking = false;
        let _ = m.ws.send_text(&speaking(0, m.rtp.ssrc));
    }

    fn with_dave<T>(&self, f: impl FnOnce(&mut Dave) -> T) -> Option<T> {
        self.media.lock().unwrap().as_mut().map(|m| f(&mut m.dave))
    }

    fn run(self: Arc<Self>, rx: Receiver<VoiceCreds>) {
        let mut next: Option<VoiceCreds> = None;
        loop {
            let creds = match next.take() {
                Some(c) => c,
                None => match rx.recv() {
                    Ok(c) => c,
                    Err(_) => return,
                },
            };
            // a dropped link is tried again with the same session (crash, network), up to three
            // times; kicked, bad auth or a dead session wait for new credentials from the gateway
            for attempt in 0..3u32 {
                let ended = self.link(&creds, &rx);
                *self.media.lock().unwrap() = None;
                match ended {
                    Ok(Ended::NewCreds(c)) => {
                        next = Some(c);
                        break;
                    }
                    Ok(Ended::Closed(code)) if matches!(code, 4004 | 4006 | 4014 | 4017 | 4022) => break,
                    Ok(Ended::Closed(_)) => {}
                    Err(e) => warn!("voice: {e:#}"),
                }
                std::thread::sleep(Duration::from_secs(2 << attempt));
                if let Ok(c) = rx.try_recv() {
                    next = Some(c);
                    break;
                }
            }
        }
    }

    fn link(&self, creds: &VoiceCreds, creds_rx: &Receiver<VoiceCreds>) -> anyhow::Result<Ended> {
        let user_id: u64 = creds.user_id.parse()?;
        let channel_id: u64 = creds.channel_id.parse()?;
        let (tx, rx) = channel();
        let ws = Arc::new(Ws::connect(&format!("wss://{}/?v=8", creds.endpoint), tx)?);
        ws.send_text(
            &json!({"op": 0, "d": {"server_id": creds.guild_id, "user_id": creds.user_id,
                "session_id": creds.session_id, "token": creds.token,
                "max_dave_protocol_version": davey::DAVE_PROTOCOL_VERSION}})
            .to_string(),
        )?;
        let mut seq_ack: i64 = -1;
        let mut beat: Option<(Duration, Instant)> = None;
        let mut ssrc = 0u32;
        let mut udp: Option<UdpSocket> = None;
        let mut connected: HashSet<u64> = HashSet::new();
        let mut next_keepalive = Instant::now();
        loop {
            if let Ok(c) = creds_rx.try_recv() {
                return Ok(Ended::NewCreds(c));
            }
            match rx.recv_timeout(Duration::from_millis(50)) {
                Ok(Msg::Text(text)) => {
                    let Ok(m) = serde_json::from_str::<Value>(&text) else { continue };
                    if let Some(s) = m["seq"].as_i64() {
                        seq_ack = s;
                    }
                    let d = &m["d"];
                    match m["op"].as_u64().unwrap_or(u64::MAX) {
                        8 => {
                            let iv = Duration::from_millis(d["heartbeat_interval"].as_f64().unwrap_or(13_750.0) as u64);
                            beat = Some((iv, Instant::now() + iv));
                        }
                        2 => {
                            ssrc = d["ssrc"].as_u64().unwrap_or(0) as u32;
                            let ip = d["ip"].as_str().unwrap_or("").to_string();
                            let port = d["port"].as_u64().unwrap_or(0) as u16;
                            let modes: Vec<String> = d["modes"]
                                .as_array()
                                .map(|a| a.iter().filter_map(|x| x.as_str().map(String::from)).collect())
                                .unwrap_or_default();
                            let mode = crypto::choose_mode(&modes)
                                .ok_or_else(|| anyhow::anyhow!("no encryption mode we support in {modes:?}"))?;
                            let sock = UdpSocket::bind("0.0.0.0:0")?;
                            sock.connect((ip.as_str(), port))?;
                            sock.set_read_timeout(Some(Duration::from_secs(2)))?;
                            let (my_ip, my_port) = discover(&sock, ssrc)?;
                            info!("voice: ssrc {ssrc}, media {ip}:{port}, we are {my_ip}:{my_port}, mode {mode}");
                            ws.send_text(
                                &json!({"op": 1, "d": {"protocol": "udp",
                                    "data": {"address": my_ip, "port": my_port, "mode": mode}}})
                                .to_string(),
                            )?;
                            udp = Some(sock);
                        }
                        4 => {
                            let mode = d["mode"].as_str().unwrap_or("");
                            let key: Vec<u8> = d["secret_key"]
                                .as_array()
                                .map(|a| a.iter().filter_map(|x| x.as_u64().map(|v| v as u8)).collect())
                                .unwrap_or_default();
                            let dave_version = d["dave_protocol_version"].as_u64().unwrap_or(0) as u16;
                            let cipher = Cipher::new(mode, &key)?;
                            let sock = udp.take().ok_or_else(|| anyhow::anyhow!("session description before Ready"))?;
                            let started = Instant::now();
                            let (dave, reply) = Dave::new(dave_version, user_id, channel_id);
                            let (sequence, timestamp) =
                                unsafe { (esp_idf_svc::sys::esp_random() as u16, esp_idf_svc::sys::esp_random()) };
                            *self.media.lock().unwrap() = Some(Media {
                                udp: sock,
                                rtp: RtpStream::new(ssrc, cipher, sequence, timestamp),
                                dave,
                                speaking: false,
                                ws: ws.clone(),
                                keepalive: 0,
                            });
                            send_reply(&ws, reply)?;
                            info!(
                                "voice: ready, mode {mode}, DAVE v{dave_version} (session set up in {} ms)",
                                started.elapsed().as_millis()
                            );
                        }
                        11 => {
                            for id in d["user_ids"].as_array().into_iter().flatten() {
                                if let Some(n) = id.as_str().and_then(|s| s.parse().ok()) {
                                    connected.insert(n);
                                }
                            }
                        }
                        13 => {
                            if let Some(n) = d["user_id"].as_str().and_then(|s| s.parse::<u64>().ok()) {
                                connected.remove(&n);
                            }
                        }
                        21 => {
                            let tid = d["transition_id"].as_u64().unwrap_or(0) as u16;
                            let version = d["protocol_version"].as_u64().unwrap_or(0) as u16;
                            if self.with_dave(|dv| dv.prepare_transition(tid, version)) == Some(true) {
                                ws.send_text(&json!({"op": 23, "d": {"transition_id": tid}}).to_string())?;
                            }
                        }
                        22 => {
                            let tid = d["transition_id"].as_u64().unwrap_or(0) as u16;
                            self.with_dave(|dv| dv.execute_transition(tid));
                        }
                        24 => {
                            let epoch = d["epoch"].as_u64().unwrap_or(0);
                            let version = d["protocol_version"].as_u64().unwrap_or(0) as u16;
                            if let Some(r) = self.with_dave(|dv| dv.prepare_epoch(epoch, version)) {
                                send_reply(&ws, r)?;
                            }
                        }
                        _ => {} // 6 heartbeat ACK, 5 speaking of others, 18/20 client flags...
                    }
                }
                Ok(Msg::Binary(b)) => {
                    if b.len() < 3 {
                        continue;
                    }
                    seq_ack = u16::from_be_bytes([b[0], b[1]]) as i64;
                    let (op, payload) = (b[2], &b[3..]);
                    let started = Instant::now();
                    let reply = self.with_dave(|dv| match op {
                        dave::OP_EXTERNAL_SENDER => {
                            dv.set_external_sender(payload);
                            Reply::default()
                        }
                        dave::OP_PROPOSALS => dv.process_proposals(payload, &connected),
                        dave::OP_ANNOUNCE_COMMIT => dv.process_commit(payload),
                        dave::OP_WELCOME => dv.process_welcome(payload),
                        _ => Reply::default(),
                    });
                    info!("voice: DAVE op {op} ({} bytes) took {} ms", payload.len(), started.elapsed().as_millis());
                    if let Some(r) = reply {
                        send_reply(&ws, r)?;
                    }
                }
                Ok(Msg::Closed { code, reason }) => {
                    warn!("voice closed: {code} {} {reason}", close_name(code));
                    return Ok(Ended::Closed(code));
                }
                Err(RecvTimeoutError::Timeout) => {}
                Err(RecvTimeoutError::Disconnected) => anyhow::bail!("voice socket gone"),
            }
            let now = Instant::now();
            if let Some((iv, next)) = beat {
                if now >= next {
                    let t = unsafe { esp_idf_svc::sys::esp_timer_get_time() } / 1000;
                    ws.send_text(&json!({"op": 3, "d": {"t": t, "seq_ack": seq_ack}}).to_string())?;
                    beat = Some((iv, now + iv));
                }
            }
            if now >= next_keepalive {
                // keep the NAT mapping while quiet, as discord.js does: an 8-byte counter every 5 s
                if let Some(m) = self.media.lock().unwrap().as_mut() {
                    let mut ka = [0u8; 8];
                    ka[..4].copy_from_slice(&m.keepalive.to_le_bytes());
                    m.keepalive = m.keepalive.wrapping_add(1);
                    let _ = m.udp.send(&ka);
                }
                next_keepalive = now + Duration::from_secs(5);
            }
        }
    }
}

fn speaking(on: u8, ssrc: u32) -> String {
    json!({"op": 5, "d": {"speaking": on, "delay": 0, "ssrc": ssrc}}).to_string()
}

/// op 31 first (when a transition failed), then the binary messages, then op 23.
fn send_reply(ws: &Ws, r: Reply) -> anyhow::Result<()> {
    if let Some(tid) = r.invalid {
        ws.send_text(&json!({"op": 31, "d": {"transition_id": tid}}).to_string())?;
    }
    for (op, payload) in r.binary {
        let mut msg = Vec::with_capacity(1 + payload.len());
        msg.push(op);
        msg.extend_from_slice(&payload);
        ws.send_binary(&msg)?;
    }
    if let Some(tid) = r.ready_for {
        ws.send_text(&json!({"op": 23, "d": {"transition_id": tid}}).to_string())?;
    }
    Ok(())
}

fn discover(sock: &UdpSocket, ssrc: u32) -> anyhow::Result<(String, u16)> {
    let q = crypto::discovery_request(ssrc);
    let mut r = [0u8; 74];
    for _ in 0..3 {
        sock.send(&q)?;
        if let Ok(n) = sock.recv(&mut r) {
            if let Some(found) = crypto::discovery_reply(&r[..n]) {
                return Ok(found);
            }
        }
    }
    anyhow::bail!("UDP IP discovery got no answer")
}

fn close_name(code: u16) -> &'static str {
    match code {
        4004 => "authentication failed",
        4006 => "session no longer valid",
        4009 => "session timeout",
        4011 => "server not found",
        4014 => "disconnected (kicked, moved or channel deleted)",
        4015 => "voice server crashed",
        4016 => "unknown encryption mode",
        4017 => "DAVE (E2EE) required",
        4021 => "rate limited",
        4022 => "call terminated",
        _ => "",
    }
}
