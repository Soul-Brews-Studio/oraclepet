# jc3248-pet-idf — Guition JC3248W535 desk-pet port, ESP-IDF v6
# STEP 1: hello-world. Serial banner + chip/flash/PSRAM probe only.
# No display/touch/BLE/audio yet — those land in later steps, which is why
# we deliberately do NOT set MINIMAL_BUILD here (esp_lcd/i2s/nimble come later).
cmake_minimum_required(VERSION 3.22)

include($ENV{IDF_PATH}/tools/cmake/project.cmake)
project(jc3248_pet_idf)
