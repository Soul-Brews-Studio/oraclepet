cmake_minimum_required(VERSION 3.20)
project(oracle_term_sim C)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)

# LVGL-WASM replica of the ESPHome terminal viewer (terminal.yaml).
# Same LVGL UI (one recolor lv_label, unscii mono) as the JC3248W535, but in a
# browser via Emscripten+SDL2. The ANSI->#RRGGBB recolor logic is lifted from
# the fetch_pane lambda. Driven by a JS fetch() of maw /api/capture (shell.html).
#
# Native:  cmake -B build && cmake --build build && ./build/oracle_term_sim
# WASM:    emcmake cmake -B build-wasm && cmake --build build-wasm
#          → build-wasm/oracle_term_sim.html  (serve it; see web/serve note)

if(EMSCRIPTEN)
  message(STATUS "Building oracle_term_sim for Emscripten / WASM")
endif()

# ── LVGL v9 (same gen as the device) ───────────────────────────────
include(FetchContent)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.5.0          # match the device exactly (lv_label_set_recolor + LV_LABEL_LONG_MODE_CLIP)
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _skip_auto_add
)
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)
FetchContent_MakeAvailable(lvgl)

# Drop ARM Helium/NEON .S (can't compile on Apple Silicon / wasm)
file(REMOVE
  ${lvgl_SOURCE_DIR}/src/draw/sw/blend/helium/lv_blend_helium.S
  ${lvgl_SOURCE_DIR}/src/draw/sw/blend/neon/lv_blend_neon.S)
add_subdirectory(${lvgl_SOURCE_DIR} ${lvgl_BINARY_DIR})

# ── SDL2 (Emscripten built-in port, else Homebrew) ─────────────────
if(EMSCRIPTEN)
  set(SDL2_CFLAGS "-sUSE_SDL=2")
  separate_arguments(SDL2_CFLAGS UNIX_COMMAND "${SDL2_CFLAGS}")
else()
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    execute_process(COMMAND sdl2-config --cflags OUTPUT_VARIABLE SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND sdl2-config --libs   OUTPUT_VARIABLE SDL2_LIBS   OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(SDL2_CFLAGS UNIX_COMMAND "${SDL2_CFLAGS}")
    separate_arguments(SDL2_LIBS   UNIX_COMMAND "${SDL2_LIBS}")
  endif()
endif()

target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
if(NOT SDL2_FOUND OR EMSCRIPTEN)
  target_compile_options(lvgl PUBLIC ${SDL2_CFLAGS})
endif()
if(SDL2_FOUND AND NOT EMSCRIPTEN)
  get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(lvgl PUBLIC ${_sdl2_inc})
endif()

add_executable(oracle_term_sim main_term.c fonts_gen/term_font_16.c)
target_include_directories(oracle_term_sim PRIVATE ${CMAKE_CURRENT_SOURCE_DIR})
target_compile_definitions(oracle_term_sim PRIVATE LV_CONF_INCLUDE_SIMPLE=1 LV_LVGL_H_INCLUDE_SIMPLE=1)
if(SDL2_FOUND AND NOT EMSCRIPTEN)
  target_link_libraries(oracle_term_sim PRIVATE lvgl SDL2::SDL2)
else()
  target_compile_options(oracle_term_sim PRIVATE ${SDL2_CFLAGS})
  target_link_libraries(oracle_term_sim PRIVATE lvgl ${SDL2_LIBS})
endif()

if(EMSCRIPTEN)
  # -DSIM_MODULE=ON  → ES module (oracle_term_sim.mjs + .wasm) for the React/Vite app
  # default          → standalone .html with the built-in shell (quick preview)
  if(SIM_MODULE)
    # globalThis.createOracleTerm factory (loadable via <script>) — matches the
    # sim-gallery mount: globalThis[factory]({canvas,print,printErr}). Fonts are
    # baked into the .wasm (fonts_gen .c) so there is NO .data / preload.
    set_target_properties(oracle_term_sim PROPERTIES SUFFIX ".js")
    target_link_options(oracle_term_sim PRIVATE
      -sUSE_SDL=2 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0
      -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
      -sMODULARIZE=1 -sEXPORT_NAME=createOracleTerm -sENVIRONMENT=web)
  else()
    set_target_properties(oracle_term_sim PROPERTIES SUFFIX ".html")
    target_link_options(oracle_term_sim PRIVATE
      -sUSE_SDL=2 -sINITIAL_MEMORY=67108864 -sALLOW_MEMORY_GROWTH=1 -sEXIT_RUNTIME=0
      -sEXPORTED_RUNTIME_METHODS=ccall,cwrap
      --shell-file=${CMAKE_CURRENT_SOURCE_DIR}/web/shell.html)
  endif()
endif()

if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lvgl PRIVATE -Wno-unused-function -Wno-unused-variable)
endif()
