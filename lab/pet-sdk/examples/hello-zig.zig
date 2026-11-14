// hello, in Zig: counts taps under the pet, a heart every fifth. Same app as hello.c.
const pet = @import("pet");
const std = @import("std");

var taps: i32 = 0;

export fn on_start() void {
    pet.caption("hello from zig");
    pet.status("app: hello (zig)");
}

export fn on_tap(x: i32, y: i32) void {
    _ = x;
    _ = y;
    taps += 1;
    var buf: [24]u8 = undefined;
    const text = std.fmt.bufPrint(&buf, "zig taps: {d}", .{taps}) catch "zig taps: many";
    pet.caption(text);
    pet.sound(.blip_up);
    if (@rem(taps, 5) == 0) pet.react();
}
