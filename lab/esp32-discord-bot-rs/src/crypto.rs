//! RTP packets and Discord's transport encryption (the "rtpsize" AEAD modes), as @discordjs/voice
//! 0.19.2 builds them:
//!
//! packet = header(12) ‖ AEAD(payload, aad = header) ‖ tag(16) ‖ counter(4, BE)
//!
//! The nonce is the 4-byte big-endian counter (incremented before each packet) followed by zeros:
//! 12 bytes for AES-256-GCM, 24 for XChaCha20-Poly1305. Discord's docs: "You must support
//! aead_xchacha20_poly1305_rtpsize. You should prefer to use aead_aes256_gcm_rtpsize when it is
//! available."

use aes_gcm::aead::{Aead, KeyInit, Payload};
use aes_gcm::Aes256Gcm;
use chacha20poly1305::XChaCha20Poly1305;

pub const AES_GCM: &str = "aead_aes256_gcm_rtpsize";
pub const XCHACHA: &str = "aead_xchacha20_poly1305_rtpsize";

/// Opus RTP payload type, as Discord's clients send it.
const PAYLOAD_TYPE: u8 = 0x78;
/// 20 ms at Opus' 48 kHz clock.
pub const TIMESTAMP_STEP: u32 = 960;

/// Pick our mode from the ones the voice server offers (op 2 Ready `modes`).
pub fn choose_mode(offered: &[String]) -> Option<&'static str> {
    [AES_GCM, XCHACHA].into_iter().find(|m| offered.iter().any(|o| o == m))
}

pub enum Cipher {
    Aes(Box<Aes256Gcm>),
    XChaCha(Box<XChaCha20Poly1305>),
}

impl Cipher {
    pub fn new(mode: &str, key: &[u8]) -> anyhow::Result<Self> {
        Ok(match mode {
            AES_GCM => Cipher::Aes(Box::new(Aes256Gcm::new_from_slice(key).map_err(|_| anyhow::anyhow!("bad AES key length"))?)),
            XCHACHA => Cipher::XChaCha(Box::new(
                XChaCha20Poly1305::new_from_slice(key).map_err(|_| anyhow::anyhow!("bad XChaCha key length"))?,
            )),
            other => anyhow::bail!("unsupported encryption mode {other}"),
        })
    }
}

/// The state of one outgoing RTP stream.
pub struct RtpStream {
    pub ssrc: u32,
    pub sequence: u16,
    pub timestamp: u32,
    pub counter: u32,
    pub cipher: Cipher,
}

impl RtpStream {
    pub fn new(ssrc: u32, cipher: Cipher, sequence: u16, timestamp: u32) -> Self {
        RtpStream { ssrc, sequence, timestamp, counter: 0, cipher }
    }

    /// One encrypted packet for `payload` (an Opus frame, already DAVE-encrypted if DAVE is on).
    /// Advances sequence and timestamp for the next one.
    pub fn seal(&mut self, payload: &[u8]) -> anyhow::Result<Vec<u8>> {
        let mut header = [0u8; 12];
        header[0] = 0x80;
        header[1] = PAYLOAD_TYPE;
        header[2..4].copy_from_slice(&self.sequence.to_be_bytes());
        header[4..8].copy_from_slice(&self.timestamp.to_be_bytes());
        header[8..12].copy_from_slice(&self.ssrc.to_be_bytes());
        self.counter = self.counter.wrapping_add(1);
        let counter = self.counter.to_be_bytes();
        let sealed = match &self.cipher {
            Cipher::Aes(c) => {
                let mut nonce = [0u8; 12];
                nonce[..4].copy_from_slice(&counter);
                c.encrypt((&nonce).into(), Payload { msg: payload, aad: &header })
            }
            Cipher::XChaCha(c) => {
                let mut nonce = [0u8; 24];
                nonce[..4].copy_from_slice(&counter);
                c.encrypt((&nonce).into(), Payload { msg: payload, aad: &header })
            }
        }
        .map_err(|_| anyhow::anyhow!("transport encryption failed"))?;
        let mut packet = Vec::with_capacity(12 + sealed.len() + 4);
        packet.extend_from_slice(&header);
        packet.extend_from_slice(&sealed);
        packet.extend_from_slice(&counter);
        self.sequence = self.sequence.wrapping_add(1);
        self.timestamp = self.timestamp.wrapping_add(TIMESTAMP_STEP);
        Ok(packet)
    }
}

/// UDP IP discovery request: type 1, length 70, our SSRC, then zeros (74 bytes).
pub fn discovery_request(ssrc: u32) -> [u8; 74] {
    let mut q = [0u8; 74];
    q[0..2].copy_from_slice(&1u16.to_be_bytes());
    q[2..4].copy_from_slice(&70u16.to_be_bytes());
    q[4..8].copy_from_slice(&ssrc.to_be_bytes());
    q
}

/// The reply (type 2): our public address as a NUL-terminated string at byte 8, port in the
/// last two bytes (big-endian).
pub fn discovery_reply(r: &[u8]) -> Option<(String, u16)> {
    if r.len() != 74 || r[0..2] != 2u16.to_be_bytes() {
        return None;
    }
    let text = &r[8..72];
    let end = text.iter().position(|&b| b == 0).unwrap_or(text.len());
    let ip = std::str::from_utf8(&text[..end]).ok()?.to_string();
    Some((ip, u16::from_be_bytes([r[72], r[73]])))
}
