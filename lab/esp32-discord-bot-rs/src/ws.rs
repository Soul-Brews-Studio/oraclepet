//! A WebSocket client for the Discord gateways on ESP-IDF's esp_websocket_client, driven directly
//! through esp-idf-sys. esp-idf-svc's wrapper is not used: it reads the close code from the
//! pointer instead of the data (and rejects 4xxx codes), and it drops continuation frames. Discord
//! explains every hang-up in a 4xxx close code (4004 bad token, 4014 kicked, 4017 DAVE required),
//! so those must reach us. Each message arrives whole on a channel.

use std::ffi::{c_void, CString};
use std::sync::mpsc::{sync_channel, Sender};
use std::time::Duration;

use esp_idf_svc::sys::*;
use log::{debug, warn};

pub enum Msg {
    Text(String),
    Binary(Vec<u8>),
    /// the connection ended; `code` 0 when the server sent no close frame
    Closed { code: u16, reason: String },
}

struct Ctx {
    tx: Sender<Msg>,
    /// tells `connect` how the attempt ended: true connected, false failed
    connected: Option<std::sync::mpsc::SyncSender<bool>>,
    buf: Vec<u8>,
    binary: bool,
    close_code: u16,
    close_reason: String,
    ended: bool,
}

pub struct Ws {
    handle: esp_websocket_client_handle_t,
    ctx: *mut Ctx,
}

// esp_websocket_client serialises sends with its own lock; the context is only touched by the
// client's task (events) and by Drop after the client stopped.
unsafe impl Send for Ws {}
unsafe impl Sync for Ws {}

const TIMEOUT_TICKS: u32 = 5000; // CONFIG_FREERTOS_HZ=1000: ms

impl Ws {
    pub fn connect(url: &str, tx: Sender<Msg>) -> anyhow::Result<Ws> {
        let uri = CString::new(url)?;
        let agent = CString::new("DiscordBot (https://github.com/Soul-Brews-Studio/esp32-oracle, 0.1)")?;
        let cfg = esp_websocket_client_config_t {
            uri: uri.as_ptr(),
            user_agent: agent.as_ptr(),
            crt_bundle_attach: Some(esp_crt_bundle_attach),
            buffer_size: 4096,
            task_stack: 8192,
            disable_auto_reconnect: true,
            network_timeout_ms: 10_000,
            ..Default::default()
        };
        let (ctx_tx, ctx_rx) = sync_channel(1);
        let ctx = Box::into_raw(Box::new(Ctx {
            tx,
            connected: Some(ctx_tx),
            buf: Vec::new(),
            binary: false,
            close_code: 0,
            close_reason: String::new(),
            ended: false,
        }));
        let handle = unsafe { esp_websocket_client_init(&cfg) };
        if handle.is_null() {
            drop(unsafe { Box::from_raw(ctx) });
            anyhow::bail!("esp_websocket_client_init failed");
        }
        let ws = Ws { handle, ctx };
        unsafe {
            esp!(esp_websocket_register_events(
                handle,
                esp_websocket_event_id_t_WEBSOCKET_EVENT_ANY,
                Some(on_event),
                ctx as *mut c_void,
            ))?;
            esp!(esp_websocket_client_start(handle))?;
        }
        // a failed attempt reports at once (error/disconnect), so the caller retries in seconds
        // instead of waiting out the whole timeout
        match ctx_rx.recv_timeout(Duration::from_secs(20)) {
            Ok(true) => {}
            Ok(false) => anyhow::bail!("connection to {url} failed"),
            Err(_) => anyhow::bail!("no connection to {url} after 20 s"),
        }
        Ok(ws)
    }

    pub fn send_text(&self, s: &str) -> anyhow::Result<()> {
        let n = unsafe { esp_websocket_client_send_text(self.handle, s.as_ptr() as _, s.len() as _, TIMEOUT_TICKS) };
        anyhow::ensure!(n >= 0, "websocket send failed");
        Ok(())
    }

    pub fn send_binary(&self, b: &[u8]) -> anyhow::Result<()> {
        let n = unsafe { esp_websocket_client_send_bin(self.handle, b.as_ptr() as _, b.len() as _, TIMEOUT_TICKS) };
        anyhow::ensure!(n >= 0, "websocket send failed");
        Ok(())
    }
}

impl Drop for Ws {
    fn drop(&mut self) {
        unsafe {
            esp_websocket_client_destroy(self.handle); // stops the client task first
            drop(Box::from_raw(self.ctx));
        }
    }
}

unsafe extern "C" fn on_event(arg: *mut c_void, _base: esp_event_base_t, id: i32, data: *mut c_void) {
    let ctx = &mut *(arg as *mut Ctx);
    let ev = &*(data as *const esp_websocket_event_data_t);
    #[allow(non_upper_case_globals)]
    match id {
        esp_websocket_event_id_t_WEBSOCKET_EVENT_CONNECTED => {
            if let Some(c) = ctx.connected.take() {
                let _ = c.try_send(true);
            }
        }
        esp_websocket_event_id_t_WEBSOCKET_EVENT_DATA => {
            let chunk = if ev.data_len > 0 && !ev.data_ptr.is_null() {
                core::slice::from_raw_parts(ev.data_ptr as *const u8, ev.data_len as usize)
            } else {
                &[]
            };
            match ev.op_code {
                0..=2 => {
                    if ev.op_code != 0 && ev.payload_offset == 0 {
                        ctx.buf.clear(); // a new message (0 = continuation of the last one)
                        ctx.binary = ev.op_code == 2;
                    }
                    ctx.buf.extend_from_slice(chunk);
                    let frame_done = ev.payload_offset + ev.data_len >= ev.payload_len;
                    if frame_done && ev.fin {
                        let body = std::mem::take(&mut ctx.buf);
                        let msg = if ctx.binary {
                            Msg::Binary(body)
                        } else {
                            Msg::Text(String::from_utf8_lossy(&body).into_owned())
                        };
                        let _ = ctx.tx.send(msg);
                    }
                }
                8 => {
                    // close: [code u16 BE][reason]
                    if chunk.len() >= 2 {
                        ctx.close_code = u16::from_be_bytes([chunk[0], chunk[1]]);
                        ctx.close_reason = String::from_utf8_lossy(&chunk[2..]).into_owned();
                    }
                    debug!("ws: close frame {} {}", ctx.close_code, ctx.close_reason);
                }
                _ => {} // ping/pong: the client answers pings itself
            }
        }
        esp_websocket_event_id_t_WEBSOCKET_EVENT_DISCONNECTED | esp_websocket_event_id_t_WEBSOCKET_EVENT_CLOSED => {
            if let Some(c) = ctx.connected.take() {
                let _ = c.try_send(false); // never connected: `connect` fails now
            }
            if !ctx.ended {
                ctx.ended = true;
                let _ = ctx.tx.send(Msg::Closed { code: ctx.close_code, reason: std::mem::take(&mut ctx.close_reason) });
            }
        }
        esp_websocket_event_id_t_WEBSOCKET_EVENT_ERROR => {
            warn!("ws: error event");
            if let Some(c) = ctx.connected.take() {
                let _ = c.try_send(false);
            }
        }
        _ => {}
    }
}
