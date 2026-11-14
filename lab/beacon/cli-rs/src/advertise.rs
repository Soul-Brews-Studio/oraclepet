//! Platform-gated BLE advertise function.
//!
//! * macOS: real CoreBluetooth via [`crate::ble_macos`]
//! * other native: not implemented (returns error)
//! * WASM/WASI: dry-run (prints to stderr)

use anyhow::Result;

#[cfg(target_os = "macos")]
pub fn advertise(payload: &str, duration: u64) -> Result<()> {
    crate::ble_macos::advertise(payload, duration)
}

#[cfg(all(not(target_os = "macos"), not(target_family = "wasm")))]
pub fn advertise(_payload: &str, _duration: u64) -> Result<()> {
    anyhow::bail!(
        "native BLE advertising is currently macOS-only. \
         Run with --dry-run to see what would be sent, or implement \
         a BlueZ backend in src/ble_linux.rs."
    )
}

#[cfg(target_family = "wasm")]
pub fn advertise(payload: &str, duration: u64) -> Result<()> {
    eprintln!("[wasm] would advertise: oracle:{payload} (duration={duration}s)");
    Ok(())
}

#[cfg(test)]
#[cfg(target_family = "wasm")]
mod tests {
    use super::*;

    #[test]
    fn wasm_advertise_is_noop_ok() {
        // In the WASI build this should always succeed (prints to stderr).
        assert!(advertise("Op|100|10|1h", 1).is_ok());
    }
}
