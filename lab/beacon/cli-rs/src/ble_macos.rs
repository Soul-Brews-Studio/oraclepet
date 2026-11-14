// Native macOS BLE advertiser using CoreBluetooth via objc2.
//
// Replaces the subprocess call to `ble-beacon.py`. Same result on the
// wire — advertises `oracle:<payload>` as the BLE local name for the
// requested duration, then stops cleanly.
//
// Why pure objc2 and not bluest/btleplug: those crates expose
// CoreBluetooth's central role only on macOS (scanning, connecting).
// Outbound advertising needs `CBPeripheralManager`, which neither
// surface — so we hit objc2-core-bluetooth directly.

#![cfg(target_os = "macos")]

use anyhow::{Result, bail};
use dispatch2::{DispatchQueue, DispatchQueueAttr};
use objc2::rc::Retained;
use objc2::runtime::{AnyObject, NSObject, NSObjectProtocol, ProtocolObject};
use objc2::{AllocAnyThread, define_class};
use objc2_core_bluetooth::{
    CBAdvertisementDataLocalNameKey, CBManagerState, CBPeripheralManager,
    CBPeripheralManagerDelegate,
};
use objc2_foundation::{NSDictionary, NSString};
use std::thread::sleep;
use std::time::{Duration, Instant};

// ── Delegate ─────────────────────────────────────────────────────────────────
// CBPeripheralManagerDelegate has exactly one required method —
// peripheralManagerDidUpdateState: — and Apple will not even instantiate
// the manager if the delegate's class doesn't implement it.  We don't
// need any state in the delegate; we poll `manager.state()` from the
// caller thread to detect PoweredOn.

define_class!(
    #[unsafe(super(NSObject))]
    #[name = "OracleBeaconDelegate"]
    struct Delegate;

    unsafe impl NSObjectProtocol for Delegate {}

    unsafe impl CBPeripheralManagerDelegate for Delegate {
        // Required protocol method — caller polls manager.state() so no logic here.
        #[unsafe(method(peripheralManagerDidUpdateState:))]
        fn _did_update_state(&self, _mgr: &CBPeripheralManager) {}
    }
);

impl Delegate {
    fn new() -> Retained<Self> {
        let this = Self::alloc().set_ivars(());
        unsafe { objc2::msg_send![super(this), init] }
    }
}

// ── Public advertise() ───────────────────────────────────────────────────────

pub fn advertise(payload: &str, duration_secs: u64) -> Result<()> {
    // Caller provides the FULL local name (binary protocol "OR<base64>" 26 bytes
    // or any legacy format). We don't prepend anything — each call site owns
    // its wire prefix.
    let full_name = payload.to_string();
    if full_name.len() > 28 {
        bail!(
            "BLE local name budget overflow: {} bytes (max 28). Payload: {payload:?}",
            full_name.len()
        );
    }
    eprintln!(
        "[ble-macos] advertising local_name={full_name:?} ({} bytes) for {duration_secs}s",
        full_name.len()
    );

    // CoreBluetooth runs on a dispatch queue; we feed it a serial private
    // one so callbacks don't need a main run-loop in this process.
    let queue = DispatchQueue::new("com.oracle-beacon.cb-queue", DispatchQueueAttr::SERIAL);

    let delegate = Delegate::new();
    let delegate_proto: &ProtocolObject<dyn CBPeripheralManagerDelegate> =
        ProtocolObject::from_ref(&*delegate);

    let manager: Retained<CBPeripheralManager> = unsafe {
        CBPeripheralManager::initWithDelegate_queue(
            CBPeripheralManager::alloc(),
            Some(delegate_proto),
            Some(&queue),
        )
    };

    // Wait for PoweredOn, max 5s.
    let start = Instant::now();
    let powered_on = loop {
        let state = unsafe { manager.state() };
        match state {
            s if s == CBManagerState::PoweredOn => break true,
            s if s == CBManagerState::Unsupported => {
                bail!("BLE not supported on this device")
            }
            s if s == CBManagerState::Unauthorized => {
                bail!(
                    "BLE access not authorized — grant Bluetooth permission in \
                     System Settings → Privacy & Security → Bluetooth"
                )
            }
            s if s == CBManagerState::PoweredOff => {
                bail!("Bluetooth is off — turn it on first")
            }
            _ => {}
        }
        if start.elapsed() > Duration::from_secs(5) {
            break false;
        }
        sleep(Duration::from_millis(50));
    };
    if !powered_on {
        bail!("CBManager didn't reach PoweredOn within 5s");
    }

    // Build advertisement data: { CBAdvertisementDataLocalNameKey: "oracle:<payload>" }
    let key: &NSString = unsafe { CBAdvertisementDataLocalNameKey };
    let name = NSString::from_str(&full_name);
    let value: &AnyObject = name.as_ref();
    let dict: Retained<NSDictionary<NSString, AnyObject>> =
        NSDictionary::from_slices(&[key], &[value]);

    unsafe { manager.startAdvertising(Some(&dict)) };
    eprintln!("[ble-macos] advertising started");

    // Wait for duration, then stop.
    sleep(Duration::from_secs(duration_secs));

    unsafe { manager.stopAdvertising() };
    eprintln!("[ble-macos] advertising stopped");

    // Drop order matters: manager goes out of scope here, the delegate
    // afterwards. CoreBluetooth registration is unbound when the
    // CBPeripheralManager is released.

    Ok(())
}
