# claudemon

Wireless token-usage monitor for Claude Code, running on the **Sunton
ESP32-4848S040C** (480×480 capacitive CYD, ESP32-S3 N16R8).

A host-side tailer watches `~/.claude/projects/*/*.jsonl`, aggregates token
usage per project, and POSTs deltas to the device over the LAN. The device
shows a live dashboard: grand total, per-environment tiles with a 60-minute
sparkline, and a coral-on-charcoal theme.

## Layout

```
boards/                 Sunton board JSONs (git submodule, pinned)
include/lv_conf.h       LVGL 9 config (copied from cyd-rundeck-s3-clean)
src/
  main.cpp              boot sequence + main loop
  config.{h,cpp}        NVS persistence (WiFi + shared secret)
  net.{h,cpp}           AP/STA state machine + DNS hijack
  portal.{h,cpp}        captive-portal HTML form
  server.{h,cpp}        WebServer: /ingest, /status, portal routes
  store.{h,cpp}         per-env aggregates + 60-minute ring buffer
  touch.{h,cpp}         GT911 threshold fix + manual click dispatch
  theme.h               colour palette
  ui.{h,cpp}            LVGL screens (AP splash + dashboard)
host/
  claudemon-tailer.py   host-side log tailer
  README.md             tailer setup + launchd plist
```

## First flash

```bash
git submodule update --init      # pulls boards/
pio run -t upload                # build + flash over USB
pio device monitor               # serial @ 115200
```

## First boot

1. Device comes up in AP mode: SSID `claudemon-XXXX`, password `claudemon`.
2. Join it and a captive-portal page appears. Enter your WiFi creds.
3. Copy the **shared secret** shown on the form — the tailer needs it.
4. Device reboots into STA mode; dashboard becomes `http://claudemon.local`.

## Running the tailer

See `host/README.md`. Short version:

```bash
python3 host/claudemon-tailer.py        # writes default config, exits
$EDITOR ~/.config/claudemon/tailer.toml # set device_url + shared_secret
python3 host/claudemon-tailer.py        # runs continuously
```

## Re-entering setup

Long-press the screen for 5 s — wipes NVS and drops back to AP mode.

## Toolchain notes

Read `CLAUDE.md` and `DEVICE_NOTES.md` before touching `platformio.ini`,
the GT911 setup, or the LVGL indev wiring. There are days of pinned
hardware/toolchain quirks documented there.
