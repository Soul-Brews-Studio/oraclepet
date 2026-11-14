//! Hand-rolled base64 (RFC 4648 standard alphabet, no padding).
//! Kept minimal so the wire-format module stays under the 350-line cap.

const ALPHABET: &[u8; 64] =
    b"ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/";

pub fn encode(data: &[u8]) -> String {
    let mut out = String::with_capacity(data.len().div_ceil(3) * 4);
    for chunk in data.chunks(3) {
        let b0 = chunk[0];
        let b1 = chunk.get(1).copied().unwrap_or(0);
        let b2 = chunk.get(2).copied().unwrap_or(0);
        out.push(ALPHABET[(b0 >> 2) as usize] as char);
        out.push(ALPHABET[(((b0 & 0x03) << 4) | (b1 >> 4)) as usize] as char);
        if chunk.len() > 1 {
            out.push(ALPHABET[(((b1 & 0x0F) << 2) | (b2 >> 6)) as usize] as char);
        }
        if chunk.len() > 2 {
            out.push(ALPHABET[(b2 & 0x3F) as usize] as char);
        }
    }
    out
}

pub fn decode(input: &str) -> Option<Vec<u8>> {
    fn lookup(c: u8) -> Option<u8> {
        match c {
            b'A'..=b'Z' => Some(c - b'A'),
            b'a'..=b'z' => Some(c - b'a' + 26),
            b'0'..=b'9' => Some(c - b'0' + 52),
            b'+' => Some(62),
            b'/' => Some(63),
            _ => None,
        }
    }
    let bytes = input.as_bytes();
    let mut out = Vec::with_capacity(bytes.len() * 3 / 4);
    for chunk in bytes.chunks(4) {
        let mut v = [0u8; 4];
        for (i, &b) in chunk.iter().enumerate() {
            v[i] = lookup(b)?;
        }
        out.push((v[0] << 2) | (v[1] >> 4));
        if chunk.len() > 2 {
            out.push((v[1] << 4) | (v[2] >> 2));
        }
        if chunk.len() > 3 {
            out.push((v[2] << 6) | v[3]);
        }
    }
    Some(out)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn round_trip_arbitrary_lengths() {
        for bytes in &[
            &b""[..],
            &b"f"[..],
            &b"fo"[..],
            &b"foo"[..],
            &b"foob"[..],
            &b"fooba"[..],
            &b"foobar"[..],
            &[0u8, 0xFF, 0x80, 0x7F, 0x01],
        ] {
            let encoded = encode(bytes);
            let decoded = decode(&encoded).unwrap();
            assert_eq!(&decoded[..], *bytes);
        }
    }

    #[test]
    fn known_vectors() {
        // RFC 4648 examples, no padding
        assert_eq!(encode(b""), "");
        assert_eq!(encode(b"f"), "Zg");
        assert_eq!(encode(b"fo"), "Zm8");
        assert_eq!(encode(b"foo"), "Zm9v");
        assert_eq!(encode(b"foob"), "Zm9vYg");
        assert_eq!(encode(b"foobar"), "Zm9vYmFy");
    }

    #[test]
    fn decode_rejects_invalid_chars() {
        assert!(decode("AB!@").is_none());
    }
}
