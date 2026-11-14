//! Binary wire protocol for the BLE beacon — 17-byte packed payload
//! base64-encoded into the BLE local name with an "OR" magic prefix.
//!
//! Wire format on the BLE local name (max 26 bytes total):
//!
//!   "OR" + base64(17-byte payload)   = 2 + 24 = 26 bytes ✓
//!
//! Payload layout (17 bytes, little-endian):
//!
//!   offset  field            bytes  notes
//!     0     version(4)|model(4) 1   high nibble = version, low = model code
//!                                    model:  1=Op, 2=So, 3=Ha, 0=??
//!     1     ctx_used_k        u16   0..65535
//!     3     q7d_pct            u8   0..100
//!     4     reset_min         u16   minutes until 7-day reset (≈ 45-day max)
//!     6     sent_min          u16   sender unix-time/60 mod 65536
//!     8     project[8]        8B    ASCII null-padded, truncated repo name
//!    16     model_ver         u8    major<<4|minor (0x48 = 4.8), 0=unknown
//!    17     crc8              u8    CRC-8/Maxim over bytes 0..16
//!                                  ─────
//!                                   18 bytes ✓
//!
//! The model version rides over the wire (not hardcoded in firmware) so
//! the device shows the live version — 4.8 today, 4.9/5.0 tomorrow — with
//! no reflash. The model_code nibble names the family (OPUS/SONNET/HAIKU).
//!
//! The firmware lambda decodes by detecting the "OR" prefix on a BLE
//! advert, base64-decoding the rest, verifying CRC, and updating widgets.

pub const PROTO_VERSION: u8 = 1;
pub const PAYLOAD_BYTES: usize = 18;
pub const WIRE_PREFIX: &str = "OR";
pub const PROJECT_LEN: usize = 8;

#[derive(Debug, Clone, PartialEq, Eq)]
pub struct Payload {
    pub version: u8,       // 0..15
    pub model_code: u8,    // 1=Op, 2=So, 3=Ha, 0=unknown
    pub ctx_used_k: u16,
    pub q7d_pct: u8,
    pub reset_min: u16,
    pub sent_min: u16,
    pub project: [u8; PROJECT_LEN], // ASCII, null-padded
    pub model_ver: u8,     // packed major<<4|minor (0x48 = 4.8), 0 = unknown
}

#[derive(Debug, PartialEq, Eq)]
pub enum DecodeError {
    MissingPrefix,
    WrongLength { got: usize, want: usize },
    InvalidBase64,
    BadCrc { got: u8, expect: u8 },
    BadVersion(u8),
}

/// Map a 2-char model code string to the 1-nibble enum used on the wire.
pub fn model_code_to_byte(code: &str) -> u8 {
    match code {
        "Op" => 1,
        "So" => 2,
        "Ha" => 3,
        _ => 0,
    }
}

pub fn byte_to_model_code(b: u8) -> &'static str {
    match b {
        1 => "Op",
        2 => "So",
        3 => "Ha",
        _ => "??",
    }
}

/// Truncate a project label to PROJECT_LEN bytes, null-padded.
pub fn pack_project(label: &str) -> [u8; PROJECT_LEN] {
    let mut out = [0u8; PROJECT_LEN];
    for (i, b) in label.as_bytes().iter().take(PROJECT_LEN).enumerate() {
        out[i] = *b;
    }
    out
}

pub fn unpack_project(packed: &[u8; PROJECT_LEN]) -> String {
    let end = packed.iter().position(|&b| b == 0).unwrap_or(PROJECT_LEN);
    String::from_utf8_lossy(&packed[..end]).into_owned()
}

/// CRC-8/Maxim — same polynomial Dallas/Maxim 1-Wire devices use.
/// Tiny (~10 lines), good detection for short payloads.
pub fn crc8_maxim(data: &[u8]) -> u8 {
    let mut crc: u8 = 0;
    for byte in data {
        let mut b = *byte;
        for _ in 0..8 {
            let mix = (crc ^ b) & 0x01;
            crc >>= 1;
            if mix != 0 {
                crc ^= 0x8C;
            }
            b >>= 1;
        }
    }
    crc
}

pub fn encode_bytes(p: &Payload) -> [u8; PAYLOAD_BYTES] {
    let mut out = [0u8; PAYLOAD_BYTES];
    out[0] = ((p.version & 0x0F) << 4) | (p.model_code & 0x0F);
    out[1..3].copy_from_slice(&p.ctx_used_k.to_le_bytes());
    out[3] = p.q7d_pct;
    out[4..6].copy_from_slice(&p.reset_min.to_le_bytes());
    out[6..8].copy_from_slice(&p.sent_min.to_le_bytes());
    out[8..16].copy_from_slice(&p.project);
    out[16] = p.model_ver;
    out[17] = crc8_maxim(&out[0..17]);
    out
}

pub fn decode_bytes(bytes: &[u8]) -> Result<Payload, DecodeError> {
    if bytes.len() != PAYLOAD_BYTES {
        return Err(DecodeError::WrongLength { got: bytes.len(), want: PAYLOAD_BYTES });
    }
    let expect = crc8_maxim(&bytes[0..17]);
    if bytes[17] != expect {
        return Err(DecodeError::BadCrc { got: bytes[17], expect });
    }
    let version = (bytes[0] >> 4) & 0x0F;
    if version != PROTO_VERSION {
        return Err(DecodeError::BadVersion(version));
    }
    let model_code = bytes[0] & 0x0F;
    let mut project = [0u8; PROJECT_LEN];
    project.copy_from_slice(&bytes[8..16]);
    Ok(Payload {
        version,
        model_code,
        ctx_used_k: u16::from_le_bytes([bytes[1], bytes[2]]),
        q7d_pct: bytes[3],
        reset_min: u16::from_le_bytes([bytes[4], bytes[5]]),
        sent_min: u16::from_le_bytes([bytes[6], bytes[7]]),
        project,
        model_ver: bytes[16],
    })
}

// ── Wire encode / decode (LocalName ↔ Payload) ───────────────────────────────
// Base64 helpers live in `crate::base64_codec` so this module stays under
// the 350-line cap.

pub fn encode_wire(p: &Payload) -> String {
    format!("{WIRE_PREFIX}{}", crate::base64_codec::encode(&encode_bytes(p)))
}

pub fn decode_wire(name: &str) -> Result<Payload, DecodeError> {
    let rest = name.strip_prefix(WIRE_PREFIX).ok_or(DecodeError::MissingPrefix)?;
    let bytes = crate::base64_codec::decode(rest).ok_or(DecodeError::InvalidBase64)?;
    decode_bytes(&bytes)
}

#[cfg(test)]
mod tests {
    use super::*;

    fn fixture() -> Payload {
        Payload {
            version: PROTO_VERSION,
            model_code: 1,
            ctx_used_k: 457,
            q7d_pct: 41,
            reset_min: 5050,
            sent_min: 12345,
            project: pack_project("esp32-oracle"),
            model_ver: 0x48,
        }
    }

    #[test]
    fn round_trip_bytes() {
        let p = fixture();
        let bytes = encode_bytes(&p);
        let back = decode_bytes(&bytes).unwrap();
        assert_eq!(p, back);
    }

    #[test]
    fn round_trip_wire() {
        let p = fixture();
        let wire = encode_wire(&p);
        assert!(wire.starts_with("OR"));
        let back = decode_wire(&wire).unwrap();
        assert_eq!(p, back);
    }

    #[test]
    fn wire_fits_ble_budget() {
        let p = fixture();
        let wire = encode_wire(&p);
        assert!(wire.len() <= 26, "wire {wire:?} ({} chars) overflows 26-byte budget", wire.len());
        assert_eq!(wire.len(), 26, "expected exactly 26, got {}", wire.len());
    }

    #[test]
    fn crc_catches_corruption() {
        let p = fixture();
        let mut bytes = encode_bytes(&p);
        bytes[3] ^= 0xFF; // flip q7d_pct
        let err = decode_bytes(&bytes).unwrap_err();
        assert!(matches!(err, DecodeError::BadCrc { .. }), "got {err:?}");
    }

    #[test]
    fn decode_rejects_missing_prefix() {
        assert!(matches!(
            decode_wire("XXabcdefgh"),
            Err(DecodeError::MissingPrefix)
        ));
    }

    #[test]
    fn decode_rejects_bad_base64() {
        assert!(matches!(decode_wire("OR!!!@##"), Err(DecodeError::InvalidBase64)));
    }

    #[test]
    fn decode_rejects_wrong_length() {
        assert!(matches!(
            decode_bytes(&[0u8; 5]),
            Err(DecodeError::WrongLength { got: 5, want: 18 })
        ));
    }

    #[test]
    fn decode_rejects_wrong_version() {
        let mut p = fixture();
        p.version = 9;
        let bytes = encode_bytes(&p);
        assert!(matches!(decode_bytes(&bytes), Err(DecodeError::BadVersion(9))));
    }

    #[test]
    fn model_code_round_trip() {
        for code in ["Op", "So", "Ha", "??"] {
            assert_eq!(byte_to_model_code(model_code_to_byte(code)), code);
        }
    }

    #[test]
    fn pack_project_truncates_to_8() {
        let packed = pack_project("timekeeper-oracle");
        assert_eq!(&packed, b"timekeep");
        assert_eq!(unpack_project(&packed), "timekeep");
    }

    #[test]
    fn pack_project_null_pads_short() {
        let packed = pack_project("vol");
        assert_eq!(packed[0..3], *b"vol");
        assert_eq!(packed[3..], [0u8; 5]);
        assert_eq!(unpack_project(&packed), "vol");
    }

    #[test]
    fn pack_project_empty_is_all_nulls() {
        let packed = pack_project("");
        assert_eq!(packed, [0u8; PROJECT_LEN]);
        assert_eq!(unpack_project(&packed), "");
    }

    #[test]
    fn crc8_maxim_matches_known_vectors() {
        // CRC-8/Maxim "123456789" → 0xA1
        assert_eq!(crc8_maxim(b"123456789"), 0xA1);
        // Empty input → 0
        assert_eq!(crc8_maxim(b""), 0);
    }

    #[test]
    fn full_pipeline_known_payload() {
        let p = fixture();
        let wire = encode_wire(&p);
        assert_eq!(wire.len(), 26);
        let back = decode_wire(&wire).unwrap();
        assert_eq!(back, p);
        assert_eq!(unpack_project(&back.project), "esp32-or");
    }
}
