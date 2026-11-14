# pet-sdk — apps for the jc3248 desk pet, in WebAssembly

The pet's firmware is the **pet OS**: display, GIF engine, BLE, clock, OTA.
A **pet app** is a small `.wasm` module that tells the pet what to do through a
fixed API. Apps arrive over Bluetooth as a partial update, so changing what the
pet does never means reflashing the firmware.

| update | what goes over BLE | size | restarts? |
|---|---|---|---|
| full OTA | the firmware (`jc3248_pet_idf.bin`) | ~700 KB, ~25 s | yes, with rollback |
| partial: pack | an animation pack (GIFs + manifest) | ~150 KB, ~6 s | no |
| partial: app | a pet app (`.wasm`) | 3–5 KB, ~1 s | no |

## Build and run an app

```bash
./build.sh examples/hello.c               # -> build/hello.wasm  (needs zig: brew install zig)
cd ../desk-pet-mac
just app ../pet-sdk/build/hello.wasm      # send it to the linked pet and run it
just app-run fleet-mood                   # run an app that is already on the pet
just app-run stop                         # back to the firmware's own behaviour
```

Or open Desk Pet's **Pet Firmware** page: *Pet apps* lists the built-in apps
(`just package` bundles `build/*.wasm`), and you can drop a `.wasm` file on the page.

## Four languages, one ABI

The pet only sees a `.wasm` that imports module `"pet"` and exports the callbacks, so
any language that compiles to wasm32 works. `build.sh` picks the compiler by extension:

| language | SDK file | example | hello size | compiler |
|---|---|---|---|---|
| C | `pet.h` | `examples/hello.c` | 3.5 KB | `zig cc` |
| Zig | `zig/pet.zig` | `examples/hello-zig.zig` | 1.3 KB | `zig build-exe` |
| Rust (`no_std`) | `rust/pet.rs` | `examples/hello-rust.rs` | 4.9 KB | `rustc --target wasm32-unknown-unknown` |
| AssemblyScript | `assemblyscript/pet.ts` | `examples/hello-as.ts` | 6.3 KB | `asc` (npm, installed on first build) |

```bash
./build.sh examples/hello.c examples/hello-zig.zig examples/hello-rust.rs examples/hello-as.ts
```

All four import exactly `pet.caption`, `pet.status`, `pet.sound`, `pet.react` and export
`memory`, `on_start`, `on_tap` (checked with `wasm-objdump -x`).

## The API (`pet.h`)

The pet calls these if you export them:

| export | when |
|---|---|
| `on_start()` | the app was loaded |
| `on_tick(ms)` | about every 100 ms |
| `on_tap(x, y)` | the screen was touched (replaces the default heart) |
| `on_fleet(total, running, waiting)` | each heartbeat from Desk Pet; exporting it means your app decides the state |

Your app can call these:

| call | does |
|---|---|
| `pet_show(PET_IDLE / BUSY / ATTENTION / SLEEP)` | play that state of the current pack |
| `pet_react()` | the heart |
| `pet_caption("text")` | the line under the pet (20 characters) |
| `pet_status("text")` | the HUD status line |
| `pet_pack("neo")` | switch the animation pack (1 = ok) |
| `pet_sound(PET_RIBBIT / BLIP_UP / BLIP_DOWN)` | a sound |
| `pet_millis()` | uptime in ms |
| `pet_log("text")` | the serial console |
| `pet_tone(hz, ms)` | a tone: 50-8000 Hz, 10-2000 ms |
| `pet_play("name")` | play a clip on the pet (`sounds/<name>.ima`); 1 = playing |
| `pet_say("text")` | speak: OraclePet turns the text into the pet's voice (macOS speech, Thai or English) and plays it back on the pet; 1 = asked |

## Examples

- `examples/hello.c`: counts taps under the pet, a heart every fifth.
- `examples/oracle-tour.c`: the seven oracle cats (iris, mother, neo, atlas, calliope,
  volt, pulse) take turns, 20 s each; a tap moves on.
- `examples/talker.c`: the pet speaks: hello on tap, a chime and "someone needs you" when an agent waits.
- `examples/speaker-test.c`: a ribbit, then a blip every 3 s, with the caption naming each sound.
- `examples/fleet-mood.c`: its own fleet rules and captions, plus a celebration when the
  last waiting agent is answered.

## Try an app on the Mac first: petsim

`sim/` builds the pet's own runtime (the same WAMR 2.4 source and config as the firmware) as a
Mac program with the same `pet.*` API, printing what the pet would do:

```bash
sim/build/petsim build/fleet-mood.wasm
#   pet.status   'app: fleet mood'
# · fleet 3 total, 2 running, 1 waiting
#   pet.show     attention
#   pet.caption  '1 needs you'
# · fleet all answered
#   pet.show     busy
#   pet.react    (heart)
#   pet.caption  '2 working'
```

`build.sh` runs `petsim --check` on every app it builds.

## Sound clips (PSND)

`pet_play` and `pet_say` use 16 kHz mono IMA ADPCM clips in `/littlefs/sounds/`: a 16-byte header
(`"PSND"`, version 1, codec 1, sample rate, sample count) and 256-byte blocks (predictor, step index,
504 samples, low nibble first). OraclePet makes them (`PetProtocol/PetSound.swift`, bit-exact with the
firmware decoder in `main/audio.c`); a phrase is a few KB and reaches the pet in about a second.

## Rules the pet enforces

- Freestanding wasm32, no libc, one 64 KB memory page (`build.sh` sets the flags).
- Names: lower-case `a-z 0-9 - _`, up to 23 characters.
- Every callback must return within 2 s. An app that overruns restarts the pet, and the
  next boot runs without it (the Firmware page shows why).
- A trap (out-of-bounds access, unreachable) stops the app; the pet keeps running.

## How it works

- Runtime: WAMR (`espressif/wasm-micro-runtime` 2.4, fast interpreter, builtin libc only)
  in `lab/jc3248-pet-idf/main/petapp.c`.
- Transport: the OTA service in `main/ota.h`: `BEGIN_FILE apps/<name>.wasm`, then `RUN_APP <name>`.
- The running app is remembered in NVS and starts again after a reboot.
