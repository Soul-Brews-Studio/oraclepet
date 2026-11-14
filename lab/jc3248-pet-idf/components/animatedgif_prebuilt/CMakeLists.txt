# animatedgif_prebuilt — AnimatedGIF shipped as a PRECOMPILED static library.
#
# This component carries NO source: just the public header (include/AnimatedGIF.h)
# and the precompiled archive (lib/libAnimatedGIF.a). It is a drop-in replacement
# for the old source component "AnimatedGIF" (now preserved under
# ../../third_party_src/AnimatedGIF). A consumer just adds `animatedgif_prebuilt`
# to its REQUIRES and #include <AnimatedGIF.h> — same as before.
#
# ── How the .a was produced ───────────────────────────────────────────────────
#   target   : esp32s3 (Xtensa LX7)         toolchain: ESP-IDF v6.0.1
#   sources  : third_party_src/AnimatedGIF/src/AnimatedGIF.cpp (+ gif.inl)
#   defines  : NO_SIMD     (force the portable-C decode path; no esp-dsp asm)
#   options  : -include idf_compat.h  (Arduino millis()/delay() shim, never run)
#              -O2 -Wno-unused-but-set-variable -Wno-error=unused-function
#   => the .a's only external refs are esp_timer_get_time() + vTaskDelay()
#      (from the shim), hence REQUIRES esp_timer freertos below.
#
# ── ABI caveat ────────────────────────────────────────────────────────────────
# A precompiled .a is locked to its toolchain + target + C++ ABI. This archive
# links into ANOTHER ESP-IDF v6 / esp32s3 project, but will NOT drop into an
# Arduino/ESPHome (arduino-esp32) build — different framework + libstdc++. To
# target Arduino/ESPHome, rebuild the .a with that toolchain (or just use source).
#
# To regenerate: move third_party_src/AnimatedGIF back to components/, build, then
# copy build/esp-idf/AnimatedGIF/libAnimatedGIF.a back into lib/.

idf_component_register(INCLUDE_DIRS "include")

# Register the prebuilt archive and propagate its runtime dependencies.
add_prebuilt_library(animatedgif "lib/libAnimatedGIF.a"
                     REQUIRES esp_timer freertos)

# This component has no SRCS, so ${COMPONENT_LIB} is an INTERFACE target; link the
# prebuilt archive into it so anything REQUIRing us pulls in the .a + the header.
target_link_libraries(${COMPONENT_LIB} INTERFACE animatedgif)
