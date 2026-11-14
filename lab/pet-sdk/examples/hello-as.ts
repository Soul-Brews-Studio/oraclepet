// hello, in AssemblyScript: counts taps under the pet, a heart every fifth. Same app as hello.c.
import * as pet from "../assemblyscript/pet";

let taps = 0;

export function on_start(): void {
  pet.caption("hello from AS");
  pet.status("app: hello (AssemblyScript)");
}

export function on_tap(x: i32, y: i32): void {
  taps++;
  pet.caption("AS taps: " + taps.toString());
  pet.sound(pet.BLIP_UP);
  if (taps % 5 == 0) pet.react();
}
