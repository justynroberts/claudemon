# Fonts & images (generated LVGL assets)

These `.c` files are generated; the sources live here too so they can be rebuilt.

## Font — Outfit 600 + FontAwesome icons (`hud_*`)

`hud_12 … hud_48` are `lv_font_t` at sizes 12/14/18/22/28/48, 4 bpp. Each merges
Outfit 600 (ASCII 0x20–0x7E) with a handful of FontAwesome glyphs so labels can
show icons (wifi/bolt/clock/calendar/chart/fire/up/down). Regenerate with
`lv_font_conv` (npm i -g lv_font_conv):

```bash
FA=".pio/libdeps/esp32-4848S040CIY1/lvgl/scripts/built_in_font/FontAwesome5-Solid+Brands+Regular.woff"
ICONS="0xF1EB,0xF0E7,0xF017,0xF073,0xF201,0xF06D,0xF077,0xF078"
for sz in 12 14 18 22 28 48; do
  lv_font_conv --font outfit-600.ttf --range 0x20-0x7E \
    --font "$FA" --range "$ICONS" \
    --size $sz --bpp 4 --format lvgl -o hud_$sz.c --force-fast-kern-format --no-compress
done
```

Icon UTF-8 macros live in `ui.cpp` (`ICON_WIFI`, `ICON_CLOCK`, …). To swap the
text font, drop a new TTF here and rerun. Bold/cool TTFs come from Fontsource:
`https://cdn.jsdelivr.net/fontsource/fonts/<family>@latest/latin-<weight>-normal.ttf`.

## Logo — `claude_logo` (150×32, ARGB8888)

From `claude_logo-source.png` (the Claude wordmark, coral #D97757), resized and
converted with LVGL's bundled `scripts/LVGLImage.py`:

```bash
sips --resampleWidth 150 claude_logo-source.png --out /tmp/claude_logo.png
python LVGLImage.py --ofmt C --cf ARGB8888 -o . /tmp/claude_logo.png   # needs: pypng pillow lz4
```

## Build note

The generated files probe `#include "lvgl.h"` then fall back to `"lvgl/lvgl.h"`
(which doesn't resolve in this tree). `platformio.ini` defines
`-D LV_LVGL_H_INCLUDE_SIMPLE` to force the working include — keep it.
