# jc3248-pet-idf — Guition JC3248W535 desk-pet port, ESP-IDF v6
# STEP 1: hello-world. Serial banner + chip/flash/PSRAM probe only.
# No display/touch/BLE/audio yet — those land in later steps, which is why
# we deliberately do NOT set MINIMAL_BUILD here (esp_lcd/i2s/nimble come later).
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)

# Which pet hardware (main/board.h): jc3248 (default), lcd28 (2.8" ILI9341 board) or sc01.
#   idf.py -B build-lcd28 -DPET_BOARD=lcd28 -DSDKCONFIG=build-lcd28/sdkconfig build
# Each board has its own project name, which is what the BLE OTA name check compares,
# so one board's image can never be installed on the other.
if(NOT DEFINED PET_BOARD)
  set(PET_BOARD jc3248)
endif()
set(PET_BOARD ${PET_BOARD} CACHE STRING "pet hardware: jc3248, lcd28, sc01 or qemu")
if(PET_BOARD STREQUAL "lcd28")
  project(oraclepet_lcd28)
elseif(PET_BOARD STREQUAL "sc01")
  # WT32-SC01 Plus: quad PSRAM instead of octal (sdkconfig.defaults.sc01)
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.sc01")
  project(oraclepet_sc01)
elseif(PET_BOARD STREQUAL "qemu")
  # Espressif QEMU (esp32s3 machine): virtual RGB panel, console on UART0 (sdkconfig.defaults.qemu)
  #   idf.py -B build-qemu -DPET_BOARD=qemu -DSDKCONFIG=build-qemu/sdkconfig build
  set(SDKCONFIG_DEFAULTS "sdkconfig.defaults;sdkconfig.defaults.qemu")
  project(oraclepet_qemu)
else()
  project(jc3248_pet_idf)
endif()

# STEP 12: espressif/wasm-micro-runtime 2.4.0 does not build on IDF v6 as shipped:
# core/shared/platform/esp-idf/espidf_{platform,file}.c use `struct stat` and
# S_ISBLK without including <sys/stat.h> (IDF v6 headers stopped pulling it in).
# Force-include it for that component only, instead of patching managed_components/.
idf_component_get_property(wamr_lib espressif__wasm-micro-runtime COMPONENT_LIB)
# espidf_file.c (the WASI file layer, unused here: WASI is off and --gc-sections
# drops it) also calls renameat(), which IDF v6's newlib does not declare.
target_compile_options(${wamr_lib} PRIVATE -include sys/stat.h -Wno-error=implicit-function-declaration)

# STEP 12: WAMR 2.4.0's esp-idf os_realloc() returns a re-aligned payload pointer without
# moving the payload, so data shifts by 4 bytes after some reallocs. That scrambled the
# loader (false "br_table" rejections of Rust/AssemblyScript apps) and corrupted the heap.
# Swap in the fixed allocator from wamr-overlay/ (see the file header).
get_target_property(wamr_srcs ${wamr_lib} SOURCES)
list(FILTER wamr_srcs EXCLUDE REGEX "platform/esp-idf/espidf_malloc\\.c$")
list(APPEND wamr_srcs ${CMAKE_CURRENT_LIST_DIR}/wamr-overlay/espidf_malloc.c)
set_property(TARGET ${wamr_lib} PROPERTY SOURCES ${wamr_srcs})
