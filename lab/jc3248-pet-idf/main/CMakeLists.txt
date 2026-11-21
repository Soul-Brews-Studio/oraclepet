set(PET_SRCS
    "jc3248_pet_idf_main.c"
    "fs.c"             # step 5: LittleFS mount at /littlefs
    "gif.cpp"          # step 6: AnimatedGIF renderer (C++)
    "hud_gfx.c"        # step 7: dependency-free RGB565 font + primitives
    "hud.c"            # step 7: bottom HUD strip (name + status + dot)
    "audio.c"          # step 8: i2s_std tone synth (the ribbit)
    "ble.c"            # step 9: NimBLE NUS heartbeat bridge
    "petclock.c"       # step 10: top-strip clock (host NTP over BLE)
    "ota.c"            # step 11: firmware update over BLE (ota_0/ota_1)
    "petapp.c"         # step 12: pet apps on WebAssembly (WAMR host)
)

# Side-by-side native sketch of moremas/build-with-claude's Cardputer Buddy UI.
# OFF by default so the comparison code stays preserved but does not add dead
# object code to every per-pack firmware until it is wired into a debug surface.
# The board layer (main/board.h): display + touch drivers per board; audio.c and the
# rest are shared (#if PET_BOARD_LCD28 where they differ).
list(APPEND PET_SRCS "wifi.c" "voice.c" "improv.c")   # VOICE.md: the mic over WiFi
list(APPEND PET_SRCS "dbk.c")   # DBK link v1: PM2.5 over ESP-NOW + BLE, pet status sync (esp32-oracle #32)
if(PET_BOARD STREQUAL "lcd28")
    list(APPEND PET_SRCS "display_ili9341.c" "touch_ft6336.c" "board_i2c.c")
    message(STATUS "pet: board = lcd28 (2.8in ILI9341 240x320, FT6336, ES8311)")
elseif(PET_BOARD STREQUAL "sc01")
    list(APPEND PET_SRCS "display_st7796.c" "touch_ft6336.c" "board_i2c.c")
    message(STATUS "pet: board = sc01 (WT32-SC01 Plus: ST7796 320x480 i80, FT6336, NS4168)")
elseif(PET_BOARD STREQUAL "qemu")
    list(APPEND PET_SRCS "display_qemu.c" "touch_qemu.c" "qemu_demo.c")
    message(STATUS "pet: board = qemu (Espressif QEMU virtual RGB panel 320x480, no touch/radio/audio)")
else()
    list(APPEND PET_SRCS "display.c" "touch.c")
    message(STATUS "pet: board = jc3248 (AXS15231B 320x480)")
endif()

option(CARDPUTER_PORT_SKETCH "Compile the Cardputer Buddy comparison sketch" OFF)
if(CARDPUTER_PORT_SKETCH)
    list(APPEND PET_SRCS "cardputer_buddy_port.c")
    message(STATUS "pet: compiling Cardputer Buddy comparison sketch")
endif()

idf_component_register(SRCS ${PET_SRCS}
                       PRIV_REQUIRES
                            spi_flash
                            esp_psram
                            esp_timer
                            esp_lcd            # esp_lcd_panel_io_spi, panel ops
                            esp_driver_spi     # QSPI master (IDF v6 split out of "driver")
                            esp_driver_ledc    # backlight PWM
                            esp_driver_gpio    # gpio
                            esp_driver_i2s     # step 8: native i2s_std TX (NS4168 amp)
                            esp_lcd_axs15231b  # managed component (idf_component.yml)
                            esp_lcd_touch      # touch driver (pulled by esp_lcd_axs15231b)
                            esp_lcd_ili9341    # lcd28: the 2.8in panel
                            esp_codec_dev      # lcd28: ES8311 codec setup
                            esp_lcd_qemu_rgb   # qemu: Espressif QEMU's virtual RGB panel
                            esp_wifi esp_netif esp_event   # VOICE.md: WiFi for the mic stream
                            tcp_transport      # VOICE.md: WebSocket from a PSRAM-stack task
                            espressif__mdns
                            esp_driver_i2c     # i2c_master bus for touch
                            bt                 # step 9: NimBLE host + controller
                            nvs_flash          # step 9: NimBLE PHY-cal / bond store
                            app_update         # step 11: esp_ota_* (BLE OTA)
                            esp_app_format     # step 11: esp_app_desc_t of the incoming image
                            bootloader_support # step 11: esp_app_format.h (image + segment headers)
                            pthread            # step 12: WAMR needs a real POSIX thread
                            wasm-micro-runtime # step 12: WAMR (managed component espressif/wasm-micro-runtime)
                            joltwallet__littlefs  # esp_littlefs runtime (idf_component.yml)
                            animatedgif_prebuilt  # step 6: AnimatedGIF as prebuilt .a + .h (source: third_party_src/AnimatedGIF)
                       INCLUDE_DIRS ".")

# ── Multiple-firmware: build-time default pack ───────────────────────────────
# All packs (bufo + cat + cat-orange + cat-pet) are baked into the one LittleFS
# image; PET_DEFAULT_PACK picks which one boots. One firmware .bin per pack:
#   idf.py -DPET_DEFAULT_PACK=cat-orange build
# Unset → gif_setup() falls back to the first pack found (legacy behaviour).
# tools/build-packs.sh builds every variant at once.
if(DEFINED PET_DEFAULT_PACK)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_DEFAULT_PACK="${PET_DEFAULT_PACK}")
    message(STATUS "pet: default pack = ${PET_DEFAULT_PACK}")
endif()

if(CARDPUTER_PORT_SKETCH)
    target_compile_definitions(${COMPONENT_LIB} PRIVATE CARDPUTER_PORT_SKETCH=1)
endif()
if(PET_BOARD STREQUAL "lcd28")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_BOARD_LCD28=1)
elseif(PET_BOARD STREQUAL "sc01")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_BOARD_SC01=1)
elseif(PET_BOARD STREQUAL "qemu")
    target_compile_definitions(${COMPONENT_LIB} PRIVATE PET_BOARD_QEMU=1)
endif()

# ── Step 5: build + flash the LittleFS asset image ───────────────────────────
# littlefs_create_partition_image() is provided by the joltwallet/littlefs
# managed component (registered via idf_component.yml). It packs the contents of
# the given source dir into a LittleFS image sized to the "storage" partition
# (from partitions.csv) and, with FLASH_IN_PROJECT, hooks it into `idf.py flash`
# so `make flash` writes the assets alongside the app — no manual esptool step.
#
#   storage   = partition NAME in partitions.csv (the 3MB littlefs region)
#   ../data   = project-relative source dir staged inside the project
#               (lab/jc3248-pet-idf/data/), holding characters/bufo/*.gif.
#               The image preserves this tree, so files mount as
#               /littlefs/characters/bufo/idle_0.gif — matching the Arduino
#               app's "/characters/<name>" layout (pet.cpp scans /characters/).
littlefs_create_partition_image(storage ../data FLASH_IN_PROJECT)
