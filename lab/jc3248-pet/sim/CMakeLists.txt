cmake_minimum_required(VERSION 3.20)
# C + CXX: main_pet.c stays C; gifplayer.cpp + AnimatedGIF.cpp are C++.
project(pet_sim C CXX)

set(CMAKE_C_STANDARD 11)
set(CMAKE_C_STANDARD_REQUIRED ON)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)

# When invoked via emcmake/emcc, CMAKE_SYSTEM_NAME == "Emscripten".
# We still want LVGL's SDL drivers — Emscripten provides SDL2 via -sUSE_SDL=2.
if(EMSCRIPTEN)
  message(STATUS "Building for Emscripten / WASM")
endif()

# ── LVGL v9 pulled at configure time ───────────────────────────────
include(FetchContent)
FetchContent_Declare(
  lvgl
  GIT_REPOSITORY https://github.com/lvgl/lvgl.git
  GIT_TAG v9.2.2
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _skip_auto_add   # don't run lvgl's CMakeLists yet
)

# Make LVGL find OUR lv_conf.h before configuring the lib
set(LV_CONF_PATH ${CMAKE_CURRENT_SOURCE_DIR}/lv_conf.h CACHE STRING "" FORCE)
set(LV_CONF_BUILD_DISABLE_EXAMPLES ON CACHE BOOL "" FORCE)
set(LV_CONF_BUILD_DISABLE_DEMOS ON CACHE BOOL "" FORCE)

FetchContent_MakeAvailable(lvgl)

# LVGL CMake GLOBs all *.S into the build target — on Apple Silicon (and any
# non-Cortex-M target) this drags in ARM Helium / NEON asm we can't compile.
# Drop them; the C fallbacks are used automatically.
file(REMOVE
  ${lvgl_SOURCE_DIR}/src/draw/sw/blend/helium/lv_blend_helium.S
  ${lvgl_SOURCE_DIR}/src/draw/sw/blend/neon/lv_blend_neon.S
)
add_subdirectory(${lvgl_SOURCE_DIR} ${lvgl_BINARY_DIR})

# ── AnimatedGIF (bitbank2) — the firmware's GIF decoder ─────────────
# Pinned to git tag 2.2.0 — the newest GitHub release tag. (The device build
# vendors PlatformIO registry build 2.2.2, which has the same public API:
# playFrame(bool, int*, void* pUser), GIFDRAW->pUser, GIF_PALETTE_RGB565_LE.)
# If 2.2.0 ever vanishes, drop GIT_TAG and rely on GIT_SHALLOW + default branch.
FetchContent_Declare(
  animatedgif
  GIT_REPOSITORY https://github.com/bitbank2/AnimatedGIF.git
  GIT_TAG 2.2.0
  GIT_SHALLOW TRUE
  SOURCE_SUBDIR _skip_auto_add   # don't run AnimatedGIF's own CMakeLists
)
FetchContent_MakeAvailable(animatedgif)
# We compile AnimatedGIF's single .cpp into pet_sim ourselves (see below).

# ── SDL2 ───────────────────────────────────────────────────────────
if(EMSCRIPTEN)
  # Emscripten provides SDL2 as a built-in port. Set both -s and -l flags.
  set(SDL2_CFLAGS "-sUSE_SDL=2")
  set(SDL2_LIBS   "-sUSE_SDL=2")
  separate_arguments(SDL2_CFLAGS UNIX_COMMAND "${SDL2_CFLAGS}")
  separate_arguments(SDL2_LIBS UNIX_COMMAND "${SDL2_LIBS}")
else()
  # Apple Silicon brew prefix is /opt/homebrew
  list(APPEND CMAKE_PREFIX_PATH /opt/homebrew /usr/local)
  find_package(SDL2 QUIET)
  if(NOT SDL2_FOUND)
    # Fallback to sdl2-config (Homebrew always ships it)
    execute_process(COMMAND sdl2-config --cflags
                    OUTPUT_VARIABLE SDL2_CFLAGS OUTPUT_STRIP_TRAILING_WHITESPACE)
    execute_process(COMMAND sdl2-config --libs
                    OUTPUT_VARIABLE SDL2_LIBS OUTPUT_STRIP_TRAILING_WHITESPACE)
    separate_arguments(SDL2_CFLAGS UNIX_COMMAND "${SDL2_CFLAGS}")
    separate_arguments(SDL2_LIBS UNIX_COMMAND "${SDL2_LIBS}")
  endif()
endif()

# Hook up SDL2 once.
if(SDL2_FOUND AND NOT EMSCRIPTEN)
  get_target_property(_sdl2_inc SDL2::SDL2 INTERFACE_INCLUDE_DIRECTORIES)
  target_include_directories(lvgl PUBLIC ${_sdl2_inc})
endif()
target_compile_definitions(lvgl PRIVATE LV_CONF_INCLUDE_SIMPLE=1)
if(NOT SDL2_FOUND OR EMSCRIPTEN)
  target_compile_options(lvgl PUBLIC ${SDL2_CFLAGS})
endif()

function(add_sim_target tgt src)
  # Extra sources (e.g. the C++ gifplayer + AnimatedGIF) come in via ARGN.
  add_executable(${tgt} ${src} ${ARGN})
  target_include_directories(${tgt} PRIVATE
    ${CMAKE_CURRENT_SOURCE_DIR}
    ${animatedgif_SOURCE_DIR}/src)
  if(SDL2_FOUND AND NOT EMSCRIPTEN)
    target_link_libraries(${tgt} PRIVATE lvgl SDL2::SDL2)
  else()
    target_compile_options(${tgt} PRIVATE ${SDL2_CFLAGS})
    target_link_libraries(${tgt} PRIVATE lvgl ${SDL2_LIBS})
  endif()
  target_compile_definitions(${tgt} PRIVATE
    LV_CONF_INCLUDE_SIMPLE=1
    LV_LVGL_H_INCLUDE_SIMPLE=1
  )

  if(EMSCRIPTEN)
    # Emit a JS factory + .wasm sidecar (+ .data for preloaded assets). React host
    # calls globalThis.PetSim({canvas, print, printErr}) — needs MODULARIZE.
    # The bufo character pack is preloaded into MEMFS at /bufo, and the C
    # default pack path is overridden to /bufo via PET_SIM_DEFAULT_PACK.
    set_target_properties(${tgt} PROPERTIES SUFFIX ".js")
    # AnimatedGIF.h pulls <Arduino.h> unless __MACH__ / __LINUX__ / PICO_BUILD /
    # __MCUXPRESSO is set. We use __MACH__ ALONE — and only __MACH__ — because:
    #   * it satisfies AnimatedGIF.h:15 (stdio branch, no Arduino.h),
    #   * it keeps readFLASH defined (gif.inl is `#ifndef __LINUX__`),
    #   * it excludes the millis()/delay() turbo-timing block (cpp:266-308 is
    #     `#if !defined(__MACH__) && !defined(__LINUX__)`),
    #   * and crucially it gives the EXACT code paths the proven native macOS
    #     build compiles. Native (which renders gifs fine) has __MACH__ set and
    #     PICO_BUILD UNSET. Adding PICO_BUILD was the lone divergence from native
    #     — it flips on gif.inl:40's file-API branch native never compiles — and
    #     it was breaking GIF decode under Emscripten (GIF_EARLY_EOF / err 6).
    target_compile_definitions(${tgt} PRIVATE
      PET_SIM_DEFAULT_PACK="/bufo"
      __MACH__=1
    )
    target_link_options(${tgt} PRIVATE
      -sUSE_SDL=2
      -sINITIAL_MEMORY=67108864    # 64 MB (gif decode + 2 MB LVGL heap)
      -sALLOW_MEMORY_GROWTH=1
      -sEXIT_RUNTIME=0
      -sMODULARIZE=1
      -sEXPORT_NAME=PetSim
      # EMBED, not preload: bake the bufo pack into the module so the files are
      # present + populated SYNCHRONOUSLY at FS init. --preload-file emits a
      # separate .data fetched via readAsync as a run-dependency, and under
      # MODULARIZE that path delivered an empty byteArray here → every GIF
      # mounted at /bufo but with size 0 (GIF_EARLY_EOF). Embedding sidesteps
      # the async data load entirely. (~556 KB baked in; fine for a sim.)
      "--embed-file=${CMAKE_CURRENT_SOURCE_DIR}/../data/characters/bufo@/bufo"
    )
  endif()
endfunction()

# ── Executable: the pet desk-pet simulator ────────────────────────
# main_pet.c (C) + gifplayer.cpp (C/C++ bridge) + AnimatedGIF.cpp (decoder).
add_sim_target(pet_sim main_pet.c
  gifplayer.cpp
  ${animatedgif_SOURCE_DIR}/src/AnimatedGIF.cpp)

# Suppress harmless LVGL warnings during build
if(CMAKE_C_COMPILER_ID MATCHES "Clang|GNU")
  target_compile_options(lvgl PRIVATE -Wno-unused-function -Wno-unused-variable)
endif()
