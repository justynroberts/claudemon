# claudemon — development notes

Practical guide for building more on this device. Pairs with:
- `DEVICE_NOTES.md` — hardware bring-up (panel, GT911 touch, toolchain pins). Read first if anything hardware misbehaves.
- `CLAUDE.md` — boot sequence + recurring gotchas.
- `src/fonts/README.md` — regenerating fonts/images.

## What it is

Wireless **Claude Code token monitor**. A host-side tailer watches `~/.claude/projects/*/*.jsonl`, aggregates token usage, and POSTs it to the device on the LAN. The device renders a live dashboard. Board: Sunton **ESP32-4848S040C** (480×480 RGB, ESP32-S3 N16R8, GT911 touch).

## Build / flash / monitor

```bash
pio run                 # build
pio run -t upload       # flash over USB (auto-resets, takes ~45s)
```

**Serial monitor needs a TTY** — `pio device monitor` fails when scripted/backgrounded
(`termios.error`). To capture boot logs reliably, drive DTR/RTS to reset and read the
port with pyserial (see `scripts/serial_capture.py`). Use the PlatformIO-bundled python
or any python with `pyserial`.

> First boot right after a flash often fails the first WiFi associate (cold RF) and
> recovers on the next reset/power-cycle. Don't judge connectivity by the post-flash boot.

## Source map

| File | Responsibility |
|------|----------------|
| `src/main.cpp` | boot sequence (panel → touch → UI → net), core-1 loop: `lv_timer_handler` + touch + `ui::tick()` + `ui::pump_anim()` |
| `src/net.cpp` | core-0 task: WiFi STA/AP, captive portal, HTTP `/ingest` + `/status`, mDNS |
| `src/config.cpp` | NVS-persisted settings (ssid/pass/name/secret), namespace `claudemon` |
| `src/store.cpp` | thread-safe token store: per-env totals, per-model rollups, 60-min history ring |
| `src/ui.cpp` | LVGL dashboard + AP splash (see UI section) |
| `src/touch.cpp` | GT911 polling, threshold patch, release hysteresis |
| `src/portal.cpp` | setup web form (network scan dropdown, secret display) |
| `host/claudemon-tailer.py` | host log tailer → device ingest |

LVGL runs **only on core 1**. All WiFi/HTTP runs on a **core-0 task** (`net_task`). Cross-core
data goes through `store`'s mutex-guarded snapshot — never call LVGL from core 0.

## Non-negotiable gotchas (learned the hard way)

- **WiFi connect**: never `WiFi.mode(WIFI_OFF)` / `disconnect(true)` right before `WiFi.begin()` — it powers the radio down and STA never associates. Set `WIFI_STA` once, then per attempt `disconnect(false,false)` → `begin()`, 25 s × 3. (`net.cpp::try_sta_connect`)
- **Screen won't update**: LVGL 9 + RGB DMA on this panel doesn't reliably flush invalidations. `ui::tick()` forces `lv_refr_now(NULL)` only when something changed; `load_screen()` does it twice on screen switch. A frozen screen with working HTTP = core 1 hung, not dead.
- **Stack**: Arduino loop stack is 8 KB; local arrays >512 B in `tick()` overflow silently and look like DMA bugs. Keep big buffers `static`.
- **Wire**: never `Wire.begin()` — smartdisplay already owns I2C0. Use ESP-IDF `i2c_master_*`.
- See `CLAUDE.md` for the full GT911 / boot-order list.

## UI system (`ui.cpp`)

Two standalone screens (`lv_obj_create(NULL)`), switched with `load_screen()`:
`s_ap_screen` (setup splash) and `s_dash` (dashboard). `tick()` runs every 500 ms,
pulls a `store::snapshot`, diffs each widget (`set_text_if_diff`), and forces one
repaint if anything changed.

Dashboard layout (480×480): status bar → hero (total + per-min/day rate) → ACTIVITY
graph → 4 project rows. The graph is its own 3-second aggregate ring (`s_agg_hist`),
decoupled from the per-env 60-min history so it scrolls smoothly. Per-env sparklines
read each env's `history[]`.

**Animations**: `pump_anim()` (called every loop iteration) runs the hero count-up,
diff-gated so it only repaints on a visible digit change — avoids constant full-screen
flicker. Add new animations the same way: step in `pump_anim`, repaint on change only.

**To add a widget**: build it in a `build_*` helper, cache its last value, update it in a
`refresh_*` that returns `changed`, OR it into `tick()`'s `changed`. Don't repaint per widget.

## Fonts & icons

`hud_12 … hud_48` = Outfit 600 + a FontAwesome subset, merged via `lv_font_conv`.
Regen steps in `src/fonts/README.md`. Key points:
- `platformio.ini` defines **`-D LV_LVGL_H_INCLUDE_SIMPLE`** — required or the generated
  `.c` files fail with `lvgl/lvgl.h: No such file`.
- Reference assets from C++ with `extern "C" { extern const lv_font_t hud_18; ... }`.
- Icon UTF-8 macros (`ICON_WIFI`, `ICON_CLOCK`, …) are at the top of `ui.cpp`.
- **Icon clipping**: a tall FontAwesome glyph inlined into a small text label clips a few
  px at the top (it's the glyph vs the small line box; moving the label down won't help).
  Give the icon its own larger label next to the word.
- Images: LVGL's `scripts/LVGLImage.py` (`--ofmt C --cf ARGB8888`); needs `pypng pillow lz4`
  in a venv (Homebrew python is PEP-668 locked). `sips --resampleWidth` to size first.

## Host tailer

```bash
/usr/bin/python3 host/claudemon-tailer.py    # config: ~/.config/claudemon/tailer.toml
```

- **Use `/usr/bin/python3`** (Apple-signed → has macOS 15 Local Network access). A Homebrew
  python gets `OSError(65, 'No route to host')` to the LAN device even though name
  resolution and `curl` work — grant it Local Network in System Settings, or just use
  `/usr/bin/python3`. The tailer ships a built-in TOML parser so 3.9 needs no `pip`.
- Endpoints: `POST /ingest` (Bearer = device secret, JSON array of `{env,model,input,output,cache_create,cache_read}`), `GET /status`.
- The tailer primes-forward on first run (only tracks new usage). To backfill history, run
  `scan({}, groups)` with empty state once and push. Device store is RAM-only — a reboot clears it.
- Device secret is shown on the setup portal; it lives in NVS, not exposed over `/status`.

## Releasing / distribution

```bash
./scripts/build-release.sh          # builds + merges -> dist/claudemon-full.bin
                                    #   and updates docs/firmware/claudemon-full.bin
gh release create vX.Y.Z dist/claudemon-full.bin --title ... --notes ...
git add docs/firmware && git commit -m "release vX.Y.Z" && git push
```

- **Single binary**: `claudemon-full.bin` is bootloader+partitions+app merged at
  offset `0x0` (ESP32-S3). Flash with `esptool.py write_flash 0x0 ...` or `scripts/flash.sh`.
- **Web installer**: `docs/` is an [ESP Web Tools](https://esphome.github.io/esp-web-tools/)
  page served via GitHub Pages (`https://justynroberts.github.io/claudemon/`). The
  `docs/firmware/claudemon-full.bin` copy is what the browser flashes, so re-run
  `build-release.sh` and commit `docs/firmware/` on every release. Pages is set to
  serve from `main` `/docs`.
- **Desktop**: `host/install.sh` installs a `launchd` agent under `/usr/bin/python3`.
  Reliability hinges on that interpreter (macOS 15 Local Network access) — see the
  Host tailer section.

## Adding a new feature — checklist

1. New persisted setting → add to `config::Settings` + load/save.
2. New data from host → extend `store::ingest` + the `/ingest` JSON schema.
3. New UI → `build_*` + `refresh_*` (diff + return changed), wire into `tick()`.
4. New font size/icon → regen per `src/fonts/README.md`, add `extern` decl.
5. Test connectivity with `curl http://claudemon.local/status` before assuming a UI bug.
