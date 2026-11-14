# AnimatedGIF — vendored from Larry Bank's bitbank2/AnimatedGIF v2.2.2
# (Apache-2.0, see LICENSE). Source copied verbatim from the proven Arduino
# build's PlatformIO libdeps; only AnimatedGIF.cpp + AnimatedGIF.h + gif.inl are
# vendored. The C++ class API (open/begin/playFrame/getCanvasWidth/...) is what
# main/gif.cpp drives, mirroring lab/jc3248-pet/src/pet.cpp 1:1.
#
# ── SIMD .S files are intentionally NOT vendored ───────────────────────────────
# The library ships s3_transparent.S / p4_transparent.S (ESP32-S3 / -P4 SIMD
# transparent-pixel merge). They are wired in gif.inl ONLY under
#   (defined(ARDUINO_ESP32S3_DEV) || defined(ARDUINO_ESP32P4_DEV)) && !NO_SIMD
# i.e. Arduino-core defines that DO NOT exist in an ESP-IDF build. They also
# #include "dsps_fft2r_platform.h" (esp-dsp), which we don't depend on. So under
# plain ESP-IDF the pure-C cookPixels / mergeTransparent path in gif.inl runs and
# the asm is unreachable. We additionally define NO_SIMD as belt-and-suspenders so
# the SIMD branch can never be selected even if an Arduino define leaks in.
#
# If you later want the S3 SIMD path: re-copy src/s3_transparent.S, add it to SRCS
# below gated on CONFIG_IDF_TARGET_ESP32S3, add an esp-dsp REQUIRES, drop NO_SIMD
# for s3, and add the matching ARDUINO_ESP32S3_DEV (or patch the gif.inl guard).

# esp_timer + freertos: only needed to satisfy idf_compat.h's millis()/delay()
# shims (force-included below). The decoder itself has no IDF dependency.
idf_component_register(
    SRCS "src/AnimatedGIF.cpp"
    INCLUDE_DIRS "src"
    REQUIRES esp_timer freertos
)

# Force the portable C decode path (see note above); never the Arduino SIMD asm.
target_compile_definitions(${COMPONENT_LIB} PRIVATE NO_SIMD)

# Arduino-shim: AnimatedGIF.cpp's bSync pacing path references Arduino millis()/
# delay() (compiled-but-never-executed under IDF — see idf_compat.h). Force-include
# the shim into every TU so the vendored sources stay byte-identical to upstream.
target_compile_options(${COMPONENT_LIB} PRIVATE
    -include "${CMAKE_CURRENT_SOURCE_DIR}/idf_compat.h"
    # gif.inl is a huge LZW decoder; -O2 is worth it for frame pacing. It also has
    # a few intentional unused-fn / fall-through shapes from upstream we don't want
    # to fail -Werror on under the parent's warnings-as-errors.
    -O2
    -Wno-unused-but-set-variable
    -Wno-error=unused-function
)
