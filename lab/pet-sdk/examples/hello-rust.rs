// hello, in Rust: counts taps under the pet, a heart every fifth. Same app as hello.c.
#![no_std]
#[path = "../rust/pet.rs"]
mod pet;

static mut TAPS: i32 = 0;

#[no_mangle]
pub extern "C" fn on_start() {
    pet::caption("hello from rust");
    pet::status("app: hello (rust)");
}

#[no_mangle]
pub extern "C" fn on_tap(_x: i32, _y: i32) {
    let taps = unsafe { TAPS += 1; TAPS };
    pet::caption(pet::Text::new().str("rust taps: ").int(taps).as_str());
    pet::sound(pet::Sound::BlipUp);
    if taps % 5 == 0 { pet::react(); }
}
