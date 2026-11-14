//! pet.rs — the pet SDK for Rust (no_std). Same API as ../pet.h (module "pet").
//! Include it from your app with `#[path = "../rust/pet.rs"] mod pet;`, then `../build.sh app.rs`.
#[link(wasm_import_module = "pet")]
extern "C" {
    #[link_name = "show"]    fn raw_show(state: i32);
    #[link_name = "react"]   fn raw_react();
    #[link_name = "caption"] fn raw_caption(s: *const u8, len: i32);
    #[link_name = "status"]  fn raw_status(s: *const u8, len: i32);
    #[link_name = "pack"]    fn raw_pack(s: *const u8, len: i32) -> i32;
    #[link_name = "sound"]   fn raw_sound(kind: i32);
    #[link_name = "millis"]  fn raw_millis() -> i32;
    #[link_name = "log"]     fn raw_log(s: *const u8, len: i32);
    #[link_name = "tone"]    fn raw_tone(hz: i32, ms: i32);
    #[link_name = "play"]    fn raw_play(s: *const u8, len: i32) -> i32;
    #[link_name = "say"]     fn raw_say(s: *const u8, len: i32) -> i32;
}

#[allow(dead_code)]
pub enum State { Idle = 0, Busy = 1, Attention = 2, Sleep = 3 }
#[allow(dead_code)]
pub enum Sound { Ribbit = 0, BlipUp = 1, BlipDown = 2 }

#[allow(dead_code)] pub fn show(s: State) { unsafe { raw_show(s as i32) } }
#[allow(dead_code)] pub fn react() { unsafe { raw_react() } }
#[allow(dead_code)] pub fn caption(s: &str) { unsafe { raw_caption(s.as_ptr(), s.len() as i32) } }
#[allow(dead_code)] pub fn status(s: &str) { unsafe { raw_status(s.as_ptr(), s.len() as i32) } }
#[allow(dead_code)] pub fn pack(s: &str) -> bool { unsafe { raw_pack(s.as_ptr(), s.len() as i32) != 0 } }
#[allow(dead_code)] pub fn sound(k: Sound) { unsafe { raw_sound(k as i32) } }
#[allow(dead_code)] pub fn millis() -> i32 { unsafe { raw_millis() } }
#[allow(dead_code)] pub fn log(s: &str) { unsafe { raw_log(s.as_ptr(), s.len() as i32) } }
#[allow(dead_code)] pub fn tone(hz: i32, ms: i32) { unsafe { raw_tone(hz, ms) } }
#[allow(dead_code)] pub fn play(s: &str) -> bool { unsafe { raw_play(s.as_ptr(), s.len() as i32) != 0 } }
#[allow(dead_code)] pub fn say(s: &str) -> bool { unsafe { raw_say(s.as_ptr(), s.len() as i32) != 0 } }

/// A tiny fixed buffer for building captions without an allocator.
pub struct Text { buf: [u8; 24], len: usize }
#[allow(dead_code)]
impl Text {
    pub fn new() -> Self { Text { buf: [0; 24], len: 0 } }
    pub fn str(mut self, s: &str) -> Self { for b in s.bytes() { if self.len < 24 { self.buf[self.len] = b; self.len += 1; } } self }
    pub fn int(mut self, mut v: i32) -> Self {
        let mut t = [0u8; 11]; let mut n = 0;
        if v < 0 { self = self.str("-"); v = -v; }
        loop { t[n] = b'0' + (v % 10) as u8; n += 1; v /= 10; if v == 0 { break; } }
        while n > 0 { n -= 1; if self.len < 24 { self.buf[self.len] = t[n]; self.len += 1; } }
        self
    }
    pub fn as_str(&self) -> &str { core::str::from_utf8(&self.buf[..self.len]).unwrap_or("") }
}

#[panic_handler]
fn panic(_: &core::panic::PanicInfo) -> ! { core::arch::wasm32::unreachable() }
