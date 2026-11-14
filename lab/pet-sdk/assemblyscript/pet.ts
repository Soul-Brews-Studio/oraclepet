// pet.ts — the pet SDK for AssemblyScript. Same API as ../pet.h (module "pet").
//   ../build.sh examples/hello-as.ts
// Strings are UTF-16 in AssemblyScript; they go to the pet as UTF-8.
@external("pet", "show")    declare function rawShow(state: i32): void;
@external("pet", "react")   declare function rawReact(): void;
@external("pet", "caption") declare function rawCaption(s: usize, len: i32): void;
@external("pet", "status")  declare function rawStatus(s: usize, len: i32): void;
@external("pet", "pack")    declare function rawPack(s: usize, len: i32): i32;
@external("pet", "sound")   declare function rawSound(kind: i32): void;
@external("pet", "millis")  declare function rawMillis(): i32;
@external("pet", "log")     declare function rawLog(s: usize, len: i32): void;
@external("pet", "tone")    declare function rawTone(hz: i32, ms: i32): void;
@external("pet", "play")    declare function rawPlay(s: usize, len: i32): i32;
@external("pet", "say")     declare function rawSay(s: usize, len: i32): i32;

export const IDLE: i32 = 0, BUSY: i32 = 1, ATTENTION: i32 = 2, SLEEP: i32 = 3;
export const RIBBIT: i32 = 0, BLIP_UP: i32 = 1, BLIP_DOWN: i32 = 2;

function utf8(s: string): ArrayBuffer { return String.UTF8.encode(s); }

export function show(state: i32): void { rawShow(state); }
export function react(): void { rawReact(); }
export function caption(s: string): void { const b = utf8(s); rawCaption(changetype<usize>(b), b.byteLength); }
export function status(s: string): void { const b = utf8(s); rawStatus(changetype<usize>(b), b.byteLength); }
export function pack(s: string): bool { const b = utf8(s); return rawPack(changetype<usize>(b), b.byteLength) != 0; }
export function sound(kind: i32): void { rawSound(kind); }
export function millis(): i32 { return rawMillis(); }
export function log(s: string): void { const b = utf8(s); rawLog(changetype<usize>(b), b.byteLength); }
export function tone(hz: i32, ms: i32): void { rawTone(hz, ms); }
export function play(s: string): bool { const b = utf8(s); return rawPlay(changetype<usize>(b), b.byteLength) != 0; }
export function say(s: string): bool { const b = utf8(s); return rawSay(changetype<usize>(b), b.byteLength) != 0; }

// AssemblyScript reports errors through abort(); the pet has no "env" module, so trap instead.
export function petAbort(msg: usize, file: usize, line: u32, col: u32): void { unreachable(); }
