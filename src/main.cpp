#include <Arduino.h>
#include <esp32_smartdisplay.h>
#include "config.h"
#include "store.h"
#include "net.h"
#include "httpsrv.h"
#include "touch.h"
#include "ui.h"

// LVGL must stay pinned to core 1. Network + HTTP run on core 0 via the
// stock Arduino loopTask, so we don't need a manual task split here; loop()
// is already on core 1 by default.

static uint32_t s_long_press_started = 0;
static uint32_t s_last_press_ms      = 0;

void setup() {
    Serial.begin(115200);
    delay(150);
    log_i("claudemon boot");

    config::load();
    store::init();

    // --- panel + touch -----------------------------------------------------
    smartdisplay_init();
    lv_display_set_rotation(lv_disp_get_default(), LV_DISPLAY_ROTATION_0);
    touch::fix_threshold();

    // Drop smartdisplay's indev — we dispatch clicks manually on LVGL 9.
    lv_indev_t* idv = lv_indev_get_next(nullptr);
    while (idv) {
        lv_indev_t* next = lv_indev_get_next(idv);
        lv_indev_delete(idv);
        idv = next;
    }

    // --- UI scaffold -------------------------------------------------------
    ui::begin();

    // --- network + http ----------------------------------------------------
    config::ensure_secret();
    net::begin();
    server::begin();
    ui::refresh_ap_labels();  // SSID is known now; update before first render

    log_i("setup done. mode=%s",
          config::is_configured() ? "STA" : "AP/portal");
}

void loop() {
    // Core 1 owns LVGL exclusively — net work is on a core-0 task.
    lv_timer_handler();
    touch::poll();

    // Click dispatch + long-press detection (3 s = wipe config and reboot).
    // GT911 occasionally sends npts=0 during a hold, so we tolerate a 300 ms
    // gap before considering the finger truly lifted.
    static bool was_pressed = false;
    static uint32_t last_click_ms = 0;
    bool pressed = touch::pressed();
    uint32_t now = millis();

    if (pressed) s_last_press_ms = now;

    // "Effectively held" = finger down OR was down within 300 ms.
    bool held = pressed || (now - s_last_press_ms < 300);

    if (held && s_long_press_started == 0) s_long_press_started = now;
    if (!held)                             s_long_press_started = 0;

    if (s_long_press_started && now - s_long_press_started > 3000) {
        s_long_press_started = 0;
        s_last_press_ms = 0;
        net::enter_config_mode();
    }

    if (pressed && !was_pressed && now - last_click_ms > 250) {
        touch::dispatch_click(touch::x(), touch::y());
        last_click_ms = now;
    }
    was_pressed = pressed;

    ui::tick();
    ui::pump_anim();   // smooth count-up between ticks
    delay(5);
}
