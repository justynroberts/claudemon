# CLAUDE.md

This file provides guidance to Claude Code (claude.ai/code) when working with code in this repository.

## What this is

`claudemon` — a new firmware project targeting the **Sunton ESP32-4848S040C** ("4-inch capacitive CYD", 480×480 ST7701S RGB panel, GT911 capacitive touch, ESP32-S3 N16R8). The actual feature direction is TBD; this repo currently holds only the device-bring-up notes inherited from a sibling project (`../cyd-rundeck-s3-clean`).

Before touching `platformio.ini`, the GT911 setup, or the LVGL indev wiring, **read `DEVICE_NOTES.md`** — it's the consolidated record of multiple days lost to this hardware/toolchain combination.

## Current state

The repo has no `platformio.ini`, no `src/`, and no `boards/` submodule yet. First task in any session that's actually building something is bootstrapping the PlatformIO project — copy the `[env:esp32-4848S040CIY1]` block from `DEVICE_NOTES.md` verbatim and run the first-time setup below.

## First-time setup

```bash
# Board JSON lives in a submodule — must exist at ./boards/ before pio run works
git submodule add -b feature/iodefs \
  https://github.com/rzeldent/platformio-espressif32-sunton.git boards
git submodule update --init
```

`lv_conf.h` must be placed at `include/lv_conf.h` (the `LV_CONF_PATH` build flag points there).

## Toolchain pins (do not bump without reading DEVICE_NOTES.md)

- `platform = espressif32@6.5.0` — newer platforms rename `esp_lcd_panel_disp_off` to `disp_on_off`, which smartdisplay calls. Bumping breaks the build.
- `board = esp32-4848S040CIY1` — JSON comes from the `rzeldent/platformio-espressif32-sunton` submodule on the `feature/iodefs` branch. After clone, run `git submodule update --init`.
- `framework = arduino` — not ESP-IDF.
- `upload_speed = 460800` — 921600 sometimes fails on this board.
- `lib_deps` pulls `https://github.com/rzeldent/esp32-smartdisplay` (LVGL 9.x).
- Build flag `-D SMARTDISPLAY_DMA_BUFFER_SIZE=65536` is required if HTTPS is in scope — halves the default 128 KB DMA bounce so mbedTLS has heap for its record buffers.

## Non-negotiable boot sequence

In `setup()`, in this order:

1. `smartdisplay_init()`.
2. `lv_display_set_rotation(... LV_DISPLAY_ROTATION_90)` — puts USB at the bottom.
3. **Patch the GT911 touch threshold.** Factory blob ships with `SCREEN_TOUCH_LEVEL = 0x50`, which physical taps never exceed — taps register as nothing. Read 184 bytes of config from `0x8047`, lower the threshold bytes (e.g. `0x14`/`0x0F`), recompute checksum, write back with the apply flag (`pkt[187] = 0x01`). Full code in `DEVICE_NOTES.md`.
4. Replace smartdisplay's LVGL indev with our own. On LVGL 9 the read_cb is a struct field, so `lv_indev_set_read_cb` doesn't stick — `lv_indev_delete` the existing one and either install our own or skip indev entirely and dispatch `LV_EVENT_CLICKED` manually (the manual-dispatch path is in `DEVICE_NOTES.md` and is the simpler workaround).

## Recurring gotchas

- **`lv_refr_now(NULL)` after every label/text update.** LVGL 9 + DMA doesn't auto-refresh on invalidation the way LVGL 8 did; stale text otherwise.
- **`WiFi.localIP()` is `0.0.0.0` briefly after `WL_CONNECTED`** — gate "connected" UI on `WiFi.localIP() != IPAddress(0,0,0,0)`.
- **Never `Wire.begin()`** — smartdisplay already installed the I2C driver on `I2C_NUM_0`. Use ESP-IDF `i2c_master_*_device` calls.
- **TLS handshakes are slow** — bump `HTTPClient` timeout to ~20 s. Prefer `WiFiClient` over `WiFiClientSecure` for `http://` URLs.
- **GT911 status `0x00` means "no new data", not "finger lifted"** — only treat as release after ~100 ms with no update. Always ack the chip by writing `0` to `0x814E` after a read.
- **Threading**: if any work moves off-core, LVGL must stay pinned to core 1. Cross-core hand-off via a mutex-guarded snapshot, not direct LVGL calls.

## Reference implementation

The sibling project `../cyd-rundeck-s3-clean` is a known-working end-to-end build for this exact board (captive portal → STA → polling dashboard with abort over HTTPS). Read its `src/main.cpp` for the canonical boot sequence, and `src/rundeck/` for the threading pattern. Do not copy its UI wholesale unless that's what's wanted — `claudemon` will have its own product direction.

## Build / flash / monitor (once a `platformio.ini` exists)

```bash
pio run                 # build default env
pio run -t upload       # flash over USB
pio device monitor      # serial @ 115200 with exception decoder
```
