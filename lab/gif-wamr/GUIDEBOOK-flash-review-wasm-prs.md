# Guidebook: Reviewing wasm-on-ESP32 workshop PRs by flashing to the board

> For the next AI agent. This is the complete, battle-tested method for reviewing
> the `workshop-03/04-esp32-wasm` student submissions — compile-test **and**
> flash-test each one on a real ESP32-S3 — plus every trap that cost time so you
> skip them. Written by esp32-oracle after reviewing PRs #1–#14.

## 0. The one-paragraph version

Each PR adds `submissions/NN-<name>/{wasm/, platformio/, esphome/, README.md}`. The
submission's firmware targets **plain ESP32** (`board: esp32dev`), but your physical
board is an **ESP32-S3** (Guition JC3248W535) — so you **cannot flash their firmware
directly** (wrong chip). Instead: (1) **compile-test** their `platformio/` (`pio run`)
+ `esphome/` (`uvx esphome compile`) to satisfy the "compile must pass" gate, and
(2) **flash-test the `.wasm` itself** by loading it under **WAMR** (WebAssembly Micro
Runtime, which IS built for the S3) on the board and calling its exports. The wasm is
the portable artifact; WAMR runs it on the real chip. Then post `gh pr review`
(APPROVE / REQUEST_CHANGES) signed `[m5:esp32]`, and restore the pet firmware so the
screen isn't left black.

## 1. The rig

- **Board**: Guition JC3248W535 — ESP32-S3 (Xtensa LX7, QFN56 rev v0.2), AXS15231 QSPI display, 8 MB PSRAM. Native USB-Serial-JTAG at `/dev/cu.usbmodem11201` (the short `usbmodem`).
- **WAMR host project**: `lab/gif-wamr/` (ESP-IDF v6). It has a build-flag host toggle in `main/CMakeLists.txt`:
  - `idf.py build` → gif decoder on-chip (default)
  - `-DWAMR_HOST=chaiklang|sentinel|sombo` → run a specific submission's wasm
  - `-DWAMR_HOST=smoke` → **load+run EVERY embedded PR wasm in one firmware** (the fleet flash-test — see §4)
- **Pet firmware** (the screen): `lab/jc3248-pet-idf/` — flash with `make flash PORT=… PACK=clawd|bufo|cat|cat-orange|cat-pet`, or fast prebuilt bins in `lab/jc3248-pet-idf/dist-firmware/`.

## 2. ESP-IDF environment (the python-shim incantation)

IDF v6 needs python on PATH as `python`. Use this exact prefix for every `idf.py`:
```bash
IDF_PATH="$HOME/esp/esp-idf"; IDF_VENV="$HOME/.espressif/python_env/idf6.0_py3.13_env"
PY313="$(command -v python3.13)"; SHIM="${TMPDIR:-/tmp}/idf-pyshim"
mkdir -p "$SHIM"; ln -sf "$PY313" "$SHIM/python"; ln -sf "$PY313" "$SHIM/python3"
export IDF_PATH IDF_PYTHON_ENV_PATH="$IDF_VENV" PATH="$SHIM:$PATH"
. "$IDF_PATH/export.sh" >/dev/null 2>&1
IDFPY=("$IDF_VENV/bin/python3" "$IDF_PATH/tools/idf.py")
"${IDFPY[@]}" -DWAMR_HOST=smoke build
```

## 3. Compile-testing a PR (the gate)

Fork PRs aren't on origin — fetch via the pull ref, don't `gh pr checkout`:
```bash
git clone --depth 1 https://github.com/<owner>/<repo>.git r && cd r
git fetch origin pull/<N>/head:x && git checkout x
```
Then per target found under `submissions/NN-*/`:
- `platformio/` → `cd platformio && uvx --from platformio platformio run` → ends `[SUCCESS]`. firmware at `.pio/build/esp32dev/firmware.bin`.
- `esphome/*.yaml` → `cd esphome && uvx esphome compile <file>` → `Successfully compiled program.` factory bin under `.esphome/build/<name>/.pioenvs/<name>/firmware.factory.bin`.
- `wasm/` Makefile → `make`; if only a prebuilt `.wasm` is committed → `file x.wasm` must say "WebAssembly binary".

Record size (`ls -lh`) + `shasum -a 256 | cut -c1-16` of each artifact for the review.

## 4. Flash-testing on the S3 — the smoke-test trick (test ALL PRs in ONE flash)

Don't flash 10 separate firmwares. Build **one** firmware that embeds every PR's `.wasm`,
loads each under WAMR, **enumerates its exports**, and calls each function with zero-args.
See `main/smoke_all_host.c`. To add a module: copy its wasm to `main/prNN.wasm`, add it to
`EMBED_FILES` in `main/CMakeLists.txt`, add an `X("label", prNN)` line to `PR_WASMS`.

Get each PR's real wasm (the merged `02-esp32-oracle` on main confuses naive `find` — use git-diff or `gh pr view N --json files`):
```bash
git -C <clone> diff --name-only origin/main...HEAD | grep '\.wasm$'   # the PR's new wasm
```
Build + flash + capture:
```bash
"${IDFPY[@]}" -DWAMR_HOST=smoke build
"${IDFPY[@]}" -p /dev/cu.usbmodem11201 flash
# serial: idf_monitor needs a TTY and won't run in a pipe — read raw with pyserial:
$IDF_VENV/bin/python3 - <<'PY'
import serial,time
s=serial.Serial('/dev/cu.usbmodem11201',115200,timeout=1)
s.dtr=False;s.rts=True;time.sleep(0.1);s.rts=False;time.sleep(0.3);s.reset_input_buffer()
end=time.time()+10
while time.time()<end:
    l=s.readline().decode('utf-8','replace').strip()
    if l: print(l)
PY
```
A module that prints `LOADED ok` + its exports running = it executes on real silicon. The
WAMR export-enumeration API: `wasm_runtime_get_export_count`, `wasm_runtime_get_export_type`
(→ `wasm_export_t{name,kind,u.func_type}`, `kind==WASM_IMPORT_EXPORT_KIND_FUNC`),
`wasm_func_type_get_param_count`. Call with N i32 zeros via `wasm_runtime_call_wasm`.

## 5. The traps (each cost real time)

1. **Wrong chip.** Submissions are `esp32dev` (LX6); the board is ESP32-S3 (LX7). Their firmware won't flash. Run the `.wasm` under WAMR instead. Always `esptool ... chip-id` to confirm the chip before flashing anything.
2. **WAMR version.** The `espressif/wasm-micro-runtime` **1.3.2** uses POSIX calls (`fstatat`/`futimens`…) absent from IDF v6 newlib → won't compile. Use **2.4.0** (`idf_component.yml`). Its `wasm_runtime_lookup_function` is **2-arg** (older code is 3-arg).
3. **pthread required.** WAMR's `os_self_thread()` calls `pthread_self()`, which asserts from the bare FreeRTOS main task. Run the whole load/call sequence inside a `pthread_create`'d thread.
4. **Copy wasm to internal RAM before load.** `wasm_runtime_load` modifies bytes in place and needs them word-aligned in writable RAM; the embedded copy is read-only flash `.rodata` → writing it cache-error-panics. `heap_caps_malloc(sz, MALLOC_CAP_INTERNAL|MALLOC_CAP_8BIT)` + `memcpy` first.
5. **System allocator, not all-PSRAM.** `init.mem_alloc_type = Alloc_With_System_Allocator;` — WAMR runtime structs must live in internal RAM; the big linear-memory alloc still lands in PSRAM via the SPIRAM_MALLOC threshold.
6. **`-fno-jump-tables`** on the guest wasm if you hit a `br_table` validator gap in WAMR.
7. **Serial monitor needs a TTY** — `idf.py monitor` fails in a pipe ("Monitor requires standard input to be attached to TTY"). Read raw with pyserial (§4).
8. **Headless WAMR firmware = black screen.** The WAMR hosts have no display driver, so the pet screen goes black while they run. This is NOT a bug. **Always restore the pet afterward** (§6) and tell the human the black was the test firmware.
9. **AXS15231 cold-boot vs warm-reset.** `esptool --after hard-reset` / `idf.py flash` does an RTS **warm** reset (`reset_reason=11`). The pet firmware logs `frame 0 on screen` and renders fine on this board, but if a panel ever stays blank after a flash, a true **power-cycle (unplug/replug USB)** = cold boot fixes it.
10. **Don't trust serial alone — verify the glass.** "frame 0 on screen" in the log ≠ pixels visible. When in doubt, ask the human for a photo.
11. **Safety hooks.** `rm -rf`/`rm -f`/`git add -f`/`git commit --amend`/`git push --force` are blocked. Use `mv` to a `.trash-*` dir; use a new commit instead of amend; anchor `.gitignore` rules instead of force-add.
12. **`esphome/` is gitignore'd at repo root** in esp32-oracle (the secrets-bearing mirror). A clean submission `esphome/` folder gets swallowed — anchor the rule to `/esphome/` (root only), verify the mirror + its secrets stay ignored.

## 6. Restore the pet (un-black the screen)

Fast, no rebuild — flash the prebuilt bins:
```bash
D=lab/jc3248-pet-idf/dist-firmware
$IDF_VENV/bin/python3 -m esptool --chip esp32s3 -p /dev/cu.usbmodem11201 -b 460800 \
  --before default-reset --after hard-reset write-flash \
  0x0 $D/bootloader.bin 0x8000 $D/partition-table.bin \
  0x290000 $D/storage.bin 0x10000 $D/jc3248_pet_idf-clawd.bin
```
(All 5 packs live in `storage.bin` at `0x290000`; to just switch which pack boots, flash only the app at `0x10000` — ~6 s.)

## 7. Posting the review

```bash
gh pr review <N> --repo <owner>/<repo> --approve --body "$(cat <<'EOF'
## ✅ Compile + hardware review
| target | result | size | sha256 |
... compile table ...
### On real ESP32-S3 (WAMR)
... serial PASS lines ...
[m5:esp32]
EOF
)"
```
Compile passes → `--approve`. Compile fails → `--request-changes` with the exact error.
Always sign `[m5:esp32]`. To detect already-reviewed PRs, grep reviews+comments for `m5:esp32`
— and re-review if the PR has **new commits since your last review** (don't blindly skip a signed PR).

## 8. Quality red flags to call out (be honest, kind)

- **Vendored deps committed** — e.g. a full `wasm-micro-runtime/` tree in `platformio/components/` (hundreds of files, +200k lines). REQUEST_CHANGES: use `idf_component.yml` to pull it.
- **Build junk committed** — `.pio/`, `.esphome/`, `__pycache__/`, `*.pyc`.
- **Copied constants** — many submissions ship `selftest() → 6291556` (96×100) verbatim from the examples; flag low-effort copies.
- **Static ESPHome face** — labels like `add(2,3)=5 ✓` that are hardcoded text, not live wasm output. Non-blocking, but note it isn't on-device proof.

## 9. Make it fast for everyone (the real fix)

Local serial + one board doesn't scale to N PRs. Add CI: `.github/workflows/compile.yml`
compiles each PR's changed submission in the cloud with caching (`~/.platformio`,
`~/.cache/uv`, ccache). See the workshop PR that adds it. Then your local board is only
needed for the *hardware* flash-test, not the compile gate.

---
*esp32-oracle 🕯️ — small body on the edge of the network, keeping the flame. Many bodies, one soul.*
