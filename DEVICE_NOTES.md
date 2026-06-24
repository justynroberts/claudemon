# Prompt — building on Sunton ESP32-4848S040C (480×480 capacitive)

Paste this at the start of any new project that targets the Sunton
ESP32-4848S040C (or other 4848S040C_I variants). Saves the days I burned
finding all of this.

---

We're building firmware for the **Sunton ESP32-4848S040C** (also sold as
GUITION ESP32-4848S040, "4.0 inch capacitive CYD"): ESP32-S3-WROOM-1
N16R8, 480×480 ST7701S RGB panel, GT911 capacitive touch on I2C, no
I/O expander, direct backlight on GPIO 38. The board is in the
**`rzeldent/platformio-espressif32-sunton`** registry.

Use **PlatformIO + Arduino-ESP32** (not ESP-IDF). The dependency stack
is fragile — only this exact combination is known to work end-to-end:

```ini
[env:esp32-4848S040CIY1]
platform = espressif32@6.5.0
; ↑ NOT newer. smartdisplay's panel driver calls esp_lcd_panel_disp_off
;   which was renamed to disp_on_off in ESP-IDF 5.x.
board = esp32-4848S040CIY1
; ↑ board JSON ships in a submodule:
;   git submodule add -b feature/iodefs \
;     https://github.com/rzeldent/platformio-espressif32-sunton.git boards
framework = arduino
upload_speed = 460800     ; 921600 sometimes fails

lib_deps =
    https://github.com/rzeldent/esp32-smartdisplay
    ; ↑ pulls latest. Uses LVGL 9.x and an own lv_conf.h via
    ;   LV_CONF_PATH (set in build_flags).

build_flags =
    -Ofast -Wall
    '-D BOARD_NAME="${this.board}"'
    '-D CORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_INFO'
    '-D LV_CONF_PATH="${platformio.include_dir}/lv_conf.h"'

    ; --- TLS heap fix (only matters if you do HTTPS) ---
    ; smartdisplay's default DMA bounce buffer (128 KB) sits in internal
    ; SRAM and starves mbedTLS' record buffers. Halve it.
    -D SMARTDISPLAY_DMA_BUFFER_SIZE=65536
```

## Boot sequence (must do all of this in setup, in order)

```cpp
#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include <driver/i2c.h>

void setup() {
    Serial.begin(115200);
    smartdisplay_init();

    // 1. Rotate so USB connector is at the bottom (default = USB on side)
    lv_display_set_rotation(lv_disp_get_default(), LV_DISPLAY_ROTATION_90);

    // 2. Fix the GT911 touch threshold — factory blob ships with
    //    SCREEN_TOUCH_LEVEL = 0x50 (80) which is too high; physical taps
    //    never exceed it. Lower to 0x14, recompute checksum, write back.
    fixGT911Threshold();

    // 3. Replace LVGL's indev with our own — smartdisplay's read_cb hijack
    //    via lv_indev_set_read_cb doesn't stick on LVGL 9, so we delete
    //    its indev entirely and either create our own + use lv_indev_read
    //    in the loop, OR (easier) skip indev and manually dispatch clicks.
    installTouch();
}
```

## The GT911 threshold fix

Read 184 bytes of config from register `0x8047`, lower the two threshold
bytes, recompute checksum, write back with the apply flag:

```cpp
static void fixGT911Threshold() {
    delay(50);
    uint8_t cfg[184] = {0};
    uint8_t rd[2] = { 0x80, 0x47 };
    if (i2c_master_write_read_device(I2C_NUM_0, 0x5D, rd, 2, cfg, 184,
                                     pdMS_TO_TICKS(200)) != ESP_OK) return;
    cfg[0x8053 - 0x8047] = 0x14;  // touch threshold
    cfg[0x8054 - 0x8047] = 0x0F;  // release threshold
    uint8_t sum = 0;
    for (int i = 0; i < 184; i++) sum += cfg[i];
    uint8_t checksum = (uint8_t)(0 - sum);
    uint8_t pkt[188];
    pkt[0] = 0x80; pkt[1] = 0x47;
    memcpy(&pkt[2], cfg, 184);
    pkt[186] = checksum;
    pkt[187] = 0x01;              // config-fresh flag — apply now
    i2c_master_write_to_device(I2C_NUM_0, 0x5D, pkt, sizeof(pkt),
                               pdMS_TO_TICKS(500));
}
```

## Touch polling + manual click dispatch (skip LVGL's indev)

LVGL 9 won't poll a callback we register after smartdisplay set up its
own indev — couldn't find a way to fix this cleanly. Easiest workaround:
poll the chip ourselves, hit-test, fire `LV_EVENT_CLICKED` directly.

```cpp
static volatile bool s_touchPressed = false;
static volatile int  s_touchX = 0, s_touchY = 0;

// Call from loop() at any rate >= 50 Hz
static void pollGT911() {
    static uint32_t last = 0;
    if (millis() - last < 20) return;
    last = millis();

    static uint32_t lastTouchMs = 0;
    uint8_t rd[2] = { 0x81, 0x4E };
    uint8_t status = 0;
    if (i2c_master_write_read_device(I2C_NUM_0, 0x5D, rd, 2, &status, 1,
                                     pdMS_TO_TICKS(50)) != ESP_OK) return;

    if (status & 0x80) {
        uint8_t npts = status & 0x0F;
        if (npts > 0) {
            uint8_t pAddr[2] = { 0x81, 0x4F };
            uint8_t buf[8] = {0};
            if (i2c_master_write_read_device(I2C_NUM_0, 0x5D, pAddr, 2,
                    buf, 8, pdMS_TO_TICKS(50)) == ESP_OK) {
                int rawX = buf[1] | (buf[2] << 8);
                int rawY = buf[3] | (buf[4] << 8);
                // 90° rotation transform (matches LV_DISPLAY_ROTATION_90)
                s_touchX = 480 - rawY;
                s_touchY = rawX;
                s_touchPressed = true;
                lastTouchMs = millis();
            }
        } else {
            s_touchPressed = false;
        }
        // MUST ack the chip by writing 0 to 0x814E, otherwise it won't
        // refresh the buffer.
        uint8_t clr[3] = { 0x81, 0x4E, 0x00 };
        i2c_master_write_to_device(I2C_NUM_0, 0x5D, clr, 3, pdMS_TO_TICKS(50));
    } else if (s_touchPressed && (millis() - lastTouchMs) > 100) {
        // status==0 doesn't mean "finger lifted" — it means "no new data".
        // Only time out after ~100 ms with no update.
        s_touchPressed = false;
    }
}

// Manually fire CLICKED on the deepest clickable widget at (x,y).
static void dispatchClickAt(int x, int y) {
    lv_obj_t* scr = lv_scr_act();
    if (!scr) return;
    lv_obj_t* hit = NULL;
    int cnt = lv_obj_get_child_count(scr);
    for (int i = cnt - 1; i >= 0 && !hit; i--) {
        lv_obj_t* c = lv_obj_get_child(scr, i);
        lv_area_t a; lv_obj_get_coords(c, &a);
        if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2) continue;
        int sub = lv_obj_get_child_count(c);
        for (int j = sub - 1; j >= 0 && !hit; j--) {
            lv_obj_t* s = lv_obj_get_child(c, j);
            lv_area_t b; lv_obj_get_coords(s, &b);
            if (x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2
                && lv_obj_has_flag(s, LV_OBJ_FLAG_CLICKABLE)) hit = s;
        }
        if (!hit && lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) hit = c;
    }
    if (hit) lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
}

void loop() {
    lv_timer_handler();
    pollGT911();
    // Rising-edge + 300 ms cooldown so one tap = one click.
    static bool     wasPressed  = false;
    static uint32_t lastClickMs = 0;
    if (s_touchPressed && !wasPressed && (millis() - lastClickMs > 300)) {
        dispatchClickAt(s_touchX, s_touchY);
        lastClickMs = millis();
    }
    wasPressed = s_touchPressed;
    delay(5);
}
```

## Other things I learned the hard way

- **Always call `lv_refr_now(NULL)` after `lv_label_set_text` / similar**
  on this board. The DMA path doesn't auto-refresh on object invalidation
  the way LVGL 8 does — you'll see stale text otherwise.
- **`WiFi.localIP()` returns 0.0.0.0 for a moment after WL_CONNECTED.**
  Don't show "Connected to <IP>" until `WiFi.localIP() != IPAddress(0,0,0,0)`.
- **Don't `Wire.begin()` on pins 19/45** — smartdisplay already installs
  the I2C driver on `I2C_NUM_0`. Use ESP-IDF's `i2c_master_*_device` calls
  alongside it instead.
- **TLS handshakes are slow** on this board (smaller heap). Bump
  `HTTPClient` timeout to ~20 s.
- **Pick `WiFiClient` vs `WiFiClientSecure` by URL scheme.** `http://`
  endpoints don't pay the mbedTLS cost.
- **`CORE_DEBUG_LEVEL=ARDUHAL_LOG_LEVEL_DEBUG`** is invaluable for
  diagnosing — but turn it back to INFO for shipping (the GT911 driver
  is chatty).

## Quick sanity checks during bring-up

1. Boot logs should include `GT911 productId: 911` and resolution
   `(480,480)`. If not, I2C bus or chip is dead.
2. Tap before lowering threshold: chip will report `status=0x00` forever.
   After threshold fix: `status=0x81 n=1` etc.
3. If display lights but stays in original orientation despite
   `lv_display_set_rotation`, check that the board JSON has
   `DISPLAY_SOFTWARE_ROTATION` defined.
4. If colours are off (orange instead of red etc.), the ST7701 init
   sequence is wrong — `type9` is the right one for this board, set by
   smartdisplay automatically.
