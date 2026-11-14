//! pet.zig — the pet SDK for Zig. Same API as ../pet.h (module "pet").
//!   ../build.sh examples/hello-zig.zig
pub const State = enum(i32) { idle = 0, busy = 1, attention = 2, sleep = 3 };
pub const Sound = enum(i32) { ribbit = 0, blip_up = 1, blip_down = 2 };

const raw = struct {
    extern "pet" fn show(state: i32) void;
    extern "pet" fn react() void;
    extern "pet" fn caption(s: [*]const u8, len: i32) void;
    extern "pet" fn status(s: [*]const u8, len: i32) void;
    extern "pet" fn pack(s: [*]const u8, len: i32) i32;
    extern "pet" fn sound(kind: i32) void;
    extern "pet" fn millis() i32;
    extern "pet" fn log(s: [*]const u8, len: i32) void;
    extern "pet" fn tone(hz: i32, ms: i32) void;
    extern "pet" fn play(s: [*]const u8, len: i32) i32;
    extern "pet" fn say(s: [*]const u8, len: i32) i32;
};

pub fn show(s: State) void { raw.show(@intFromEnum(s)); }
pub fn react() void { raw.react(); }
pub fn caption(s: []const u8) void { raw.caption(s.ptr, @intCast(s.len)); }
pub fn status(s: []const u8) void { raw.status(s.ptr, @intCast(s.len)); }
pub fn pack(s: []const u8) bool { return raw.pack(s.ptr, @intCast(s.len)) != 0; }
pub fn sound(k: Sound) void { raw.sound(@intFromEnum(k)); }
pub fn millis() i32 { return raw.millis(); }
pub fn log(s: []const u8) void { raw.log(s.ptr, @intCast(s.len)); }
pub fn tone(hz: i32, ms: i32) void { raw.tone(hz, ms); }
pub fn play(s: []const u8) bool { return raw.play(s.ptr, @intCast(s.len)) != 0; }
pub fn say(s: []const u8) bool { return raw.say(s.ptr, @intCast(s.len)) != 0; }
