# animatedgif_prebuilt

AnimatedGIF (Larry Bank / bitbank2, v2.2.2, Apache-2.0) as a **precompiled static
library + header** — "just `.a` and `.h`". A drop-in replacement for the source
component; the pet builds and links against this instead of recompiling the LZW
decoder every clean build.

```
animatedgif_prebuilt/
├── CMakeLists.txt          # add_prebuilt_library(...) — no SRCS
├── include/AnimatedGIF.h   # the ONLY header a consumer needs (self-contained under IDF)
└── lib/libAnimatedGIF.a    # precompiled for esp32s3, ESP-IDF v6.0.1
```

## Use it

```cmake
idf_component_register(SRCS "your.cpp"
                       PRIV_REQUIRES animatedgif_prebuilt)   # that's it
```
```cpp
#include <AnimatedGIF.h>     // self-contained: no Arduino.h, no idf_compat.h
```

## Why the header is self-contained

`AnimatedGIF.h` guards its Arduino include:
```c
#if defined(ARDUINO)
#include <Arduino.h>
#else
#include <stdio.h> <stdint.h> <string.h> <stdlib.h>   // + stubs memcpy_P / PROGMEM
#endif
```
Under ESP-IDF (`ARDUINO` undefined) it pulls only standard C headers. The
`idf_compat.h` shim that maps `millis()`/`delay()` onto `esp_timer`/`freertos` is
only force-included when **compiling** the `.cpp`; those calls are baked into the
`.a` as references to `esp_timer_get_time()` + `vTaskDelay()` — which is why the
component `REQUIRES esp_timer freertos`.

## ABI / toolchain lock (important)

A `.a` is locked to its **toolchain + target + C++ ABI**. This archive links into
another **ESP-IDF v6 / esp32s3** project, but will **not** drop into an
Arduino/ESPHome (arduino-esp32) build — different framework and libstdc++. For
those, rebuild the `.a` with that toolchain, or use the source.

## Regenerate the .a

```bash
# from lab/jc3248-pet-idf/
mv third_party_src/AnimatedGIF components/AnimatedGIF   # source back as a component
make build
cp build/esp-idf/AnimatedGIF/libAnimatedGIF.a components/animatedgif_prebuilt/lib/
cp components/AnimatedGIF/src/AnimatedGIF.h    components/animatedgif_prebuilt/include/
mv components/AnimatedGIF third_party_src/AnimatedGIF    # re-disable the source
```

Source preserved at `../../third_party_src/AnimatedGIF/` (Nothing is Deleted).
