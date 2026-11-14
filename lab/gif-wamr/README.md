# gif-wamr — run our gif decoder as WASM on the ESP32 via WAMR ✅

Goal: load `gifapp.wasm` (our AnimatedGIF decoder, from `../gif-wasm`) on the
ESP32-S3 with the WebAssembly Micro Runtime and read the decoded framebuffer back
in the host. Status: **WORKING on hardware** (ESP-IDF v6.0.1, WAMR 2.4.0).

## Proof (on-chip serial)
```
I (3569) gif-wamr: module loaded
I (3579) gif-wamr: instantiated
I (3589) gif-wamr: _initialize ran
I (5509) gif-wamr: WAMR decoded: frames=2  96x100  fb@0x90010
I (5509) gif-wamr:   px(0,0)   RGBA = 0,0,0,255
I (5509) gif-wamr:   center    RGBA = 123,154,99,255
I (5509) gif-wamr: >>> WAMR ran our GIF decoder ON THE ESP32 — sandboxed wasm, native pixels <<<
I (5509) gif-wamr: [0] alive — internal free 363 KB
```
`frames=2` and `96×100` match the wasmtime selftest (`gifapp_selftest` → `0x600064`);
the center pixel `123,154,99` is a real decoded frog pixel; no boot loop after.
Decode takes ~1.9 s under the classic interpreter — slow but correct.

## What it is
- **`wasm/gifapp.wasm`** — reactor (`gifapp_run/width/height/fb/selftest`, embedded
  busy.gif, ZERO imports, 512 KB→2 MB memory). Built with zig; `make` in `wasm/`,
  `make verify` runs it under wasmtime. 22 797 bytes.
- **`main/gif_wamr_main.c`** — host: copy wasm→RAM → load → instantiate →
  `_initialize` → 2-arg `wasm_runtime_lookup_function` → `wasm_runtime_call_wasm`
  → `addr_app_to_native` → read RGBA. Runs on a **pthread** (see fix #6).

## Build / flash / watch
```
(cd wasm && make && make verify)               # gifapp.wasm + host sanity
cp wasm/gifapp.wasm main/gifapp.wasm           # EMBED_FILES copy (manual)
make set-target && make build && make flash     # PORT=/dev/cu.usbmodem2101
# serial capture (USB-Serial-JTAG, reconnect-aware): see /tmp/serial_read.py pattern
```

## The six fixes (cache-error-panic → working)
Each was a distinct wall; in order:

1. **WAMR 1.3.2 → 2.4.0** — 1.3.2's CMakeLists `cmake_minimum_required(<3.5)` fails
   on CMake 4.x. 2.4.0 is modern. (Also: 2.4.0 uses 2-arg `wasm_runtime_lookup_function`.)

2. **ESP-IDF v6 POSIX patch** (`managed_components/`, gitignored, re-fetched on
   clean — RE-APPLY after a clean): WAMR's `espidf_file.c` + `espidf_platform.c`
   use `fstatat`/`futimens`/`utimensat`/`mkdirat`/`renameat` + `struct stat` that
   **v6 removed from these headers**. WAMR *defines* them (ENOSYS) in
   `espidf_platform.c` but neither file `#include <sys/stat.h>`/`<time.h>` and
   `espidf_file.c` lacks the prototypes. Fix = add those includes to both + the 5
   prototypes to `espidf_file.c`; bump the WAMR CMakeLists `cmake_minimum_required`
   to 3.16. *(WAMR 2.4.0 officially targets IDF v5.1–5.5; v6 is unsupported.)*

3. **Copy the wasm into internal RAM before `wasm_runtime_load`** — `EMBED_FILES`
   puts the module in **read-only flash** (.rodata). WAMR modifies the bytes in
   place during load; writing flash-mapped memory **cache-error-panics** (decoded:
   `load_export_section → wasm_const_str_list_insert → b_memmove_s`, EXCVADDR 0x0).
   Fix = `heap_caps_malloc(MALLOC_CAP_INTERNAL)` + `memcpy`, load from the copy.

4. **`CONFIG_WAMR_ENABLE_REF_TYPES=y`** — zig/LLVM emits the reference-types
   encoding (call_indirect table index + element-section format) even for a plain
   indirect-call table. WAMR's esp-idf component leaves REF_TYPES off →
   `load failed: ... reference types feature which is disabled`.

5. **`-fno-jump-tables`** (wasm build) — WAMR's `br_table` validator
   (`wasm_loader.c:12549`) requires every target to have the same arity as the
   default, but doesn't account for the **stack-polymorphic (unreachable) case**
   the spec allows. LLVM at `-O2` emits exactly that in dead code →
   `type mismatch: br_table targets must all use same result type`. No jump tables
   → no `br_table` → loads clean. (Switch statements become if/else chains.)

6. **Classic interpreter + run on a pthread**:
   - `CONFIG_WAMR_INTERP_CLASSIC=y` — the *fast* interp pre-computes a bounded
     per-function operand-offset table; our big decoder functions overflow it
     (`fast interpreter offset overflow` at load). Classic has no such limit.
   - WAMR's `os_self_thread()` calls `pthread_self()`, which ESP-IDF asserts on
     unless the caller is a real POSIX thread (`pthread.c:583`,
     `"Failed to find current thread ID!"` — the main FreeRTOS task isn't mapped).
     WAMR's own comment: *"xTaskCreate is not enough, look at product_mini."* So
     the whole load/call sequence runs inside a `pthread_create()`'d thread.

Plus a **boot-marker delay** in `app_main` (5×600 ms) so the host serial can
reattach after the USB-Serial-JTAG re-enumerates on reset — otherwise the one-shot
WAMR logs print into a detached console and the capture misses everything.

## sdkconfig knobs that matter
```
CONFIG_WAMR_ENABLE_LIBC_WASI=n      # zero imports — avoids espidf_file.c too
CONFIG_WAMR_ENABLE_REF_TYPES=y      # fix #4
CONFIG_WAMR_INTERP_CLASSIC=y        # fix #6a
CONFIG_SPIRAM_USE_MALLOC=y + ALWAYSINTERNAL=8192   # wasm linear mem -> PSRAM,
                                                   # WAMR structs -> internal RAM
```

## What this proves
Our decoder is a correct, import-free wasm reactor (verified under wasmtime AND
on-device), and WAMR can build + run it on ESP-IDF v6 on the ESP32-S3. Next, if
desired: feed the decoded RGBA framebuffer to the AXS15231 panel (the pet's
display HAL) so the on-screen frog is literally drawn by sandboxed wasm.
