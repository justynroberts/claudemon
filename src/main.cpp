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
static uint32_t s_reset_dialog_ms    = 0;

// A sustained ~3 s hold ARMS the reset by showing a confirm dialog; the WiFi
// wipe only happens on an explicit CONFIRM tap. So an accidental hold can't
// reset the device. The dialog auto-dismisses after RESET_DIALOG_TIMEOUT.
static constexpr uint32_t RESET_ARM_MS        = 3000;
static constexpr uint32_t RESET_DIALOG_TIMEOUT = 15000;

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

    // Touch handling. GT911 occasionally sends npts=0 during a hold, so we
    // tolerate a 300 ms gap before considering the finger truly lifted.
    static bool was_pressed = false;
    static uint32_t last_click_ms = 0;
    bool pressed = touch::pressed();
    uint32_t now = millis();

    if (pressed) s_last_press_ms = now;
    bool held = pressed || (now - s_last_press_ms < 300);
    bool tap  = pressed && !was_pressed && now - last_click_ms > 250;

    if (ui::reset_dialog_visible()) {
        // While the confirm dialog is up, taps go to its buttons only.
        s_long_press_started = 0;
        if (tap) {
            switch (ui::reset_dialog_tap(touch::x(), touch::y())) {
                case ui::ResetTap::Confirm:
                    ui::hide_reset_dialog();
                    net::enter_config_mode();   // wipes WiFi + reboots
                    break;
                case ui::ResetTap::Cancel:
                    ui::hide_reset_dialog();
                    break;
                case ui::ResetTap::None:
                    break;                       // tapped outside buttons — keep open
            }
            last_click_ms = now;
        }
        if (now - s_reset_dialog_ms > RESET_DIALOG_TIMEOUT) ui::hide_reset_dialog();
    } else {
        // Long hold arms the reset dialog (no wipe yet).
        if (held && s_long_press_started == 0) s_long_press_started = now;
        if (!held) s_long_press_started = 0;
        if (s_long_press_started && now - s_long_press_started > RESET_ARM_MS) {
            s_long_press_started = 0;
            s_last_press_ms = 0;
            ui::show_reset_dialog();
            s_reset_dialog_ms = now;
        } else if (tap) {
            touch::dispatch_click(touch::x(), touch::y());
            last_click_ms = now;
        }
    }
    was_pressed = pressed;

    ui::tick();
    ui::pump_anim();   // smooth count-up between ticks
    delay(5);
}
