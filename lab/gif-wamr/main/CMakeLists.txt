# gif-wamr — run a WASM module on the ESP32-S3 via WAMR (the WebAssembly Micro Runtime).
#
# Two hosts share the same proven WAMR load/instantiate/call plumbing; pick one
# at configure time (each defines its own app_main, so exactly one is compiled):
#
#   idf.py build                              # default: gif host  (decode gifapp.wasm on-chip)
#   idf.py -DWAMR_HOST=chaiklang build        # chaiklang host (run a sibling's submitted .wasm)
#   idf.py -DWAMR_HOST=sentinel build         # sentinel host (our own esp32-wasm submission .wasm)
#
# The chaiklang host hardware-verifies PR #1 of the esp32-wasm workshop
# (lion_pulse(100)=5050, route(3,4)=15); the sentinel host hardware-verifies our
# own submission (flame(10)=385, sense(3,4)=25) — both on a real ESP32-S3. Kept
# here as additive proof artifacts — nothing is deleted.
#   idf.py -DWAMR_HOST=smoke build            # smoke host: load+run EVERY PR's wasm (fleet flash-test)
set(WAMR_HOST "gif" CACHE STRING "WAMR host to build: gif | chaiklang | sentinel | sombo | smoke")

if(WAMR_HOST STREQUAL "chaiklang")
    set(WAMR_SRC  "chaiklang_host.c")
    set(WAMR_WASMS "chaiklang.wasm")
elseif(WAMR_HOST STREQUAL "sentinel")
    set(WAMR_SRC  "sentinel_host.c")
    set(WAMR_WASMS "sentinel.wasm")
elseif(WAMR_HOST STREQUAL "sombo")
    set(WAMR_SRC  "sombo_host.c")
    set(WAMR_WASMS "gifcore.wasm")
elseif(WAMR_HOST STREQUAL "smoke")
    set(WAMR_SRC  "smoke_all_host.c")
    set(WAMR_WASMS "pr05.wasm" "pr06.wasm" "pr07.wasm" "pr08.wasm" "pr09.wasm" "pr10.wasm" "pr11.wasm" "pr12.wasm" "pr14.wasm")
else()
    set(WAMR_SRC  "gif_wamr_main.c")
    set(WAMR_WASMS "gifapp.wasm")
endif()
message(STATUS "gif-wamr: WAMR_HOST=${WAMR_HOST} → SRC=${WAMR_SRC} EMBED=${WAMR_WASMS}")

idf_component_register(SRCS "${WAMR_SRC}"
                       PRIV_REQUIRES esp_psram
                       EMBED_FILES ${WAMR_WASMS})
