#pragma once

namespace ui {

void begin();              // builds widgets — must be called on core 1 (LVGL)
void tick();               // pulls a snapshot from store, refreshes labels/charts
void pump_anim();          // step in-flight animations; call every loop iteration
void refresh_ap_labels();  // force-update SSID/password labels; call after net::begin()

// Reset-confirm dialog. A long-press ARMS it (shows the dialog); the actual
// WiFi wipe only happens when the user taps CONFIRM — so an accidental hold
// can't reset the device on its own.
enum class ResetTap { None, Confirm, Cancel };
void      show_reset_dialog();
void      hide_reset_dialog();
bool      reset_dialog_visible();
ResetTap  reset_dialog_tap(int x, int y);  // classify a tap while the dialog is up

// Show the AP-mode "join this SSID" splash. Replaces the dashboard until
// net::state() returns Online.
void show_ap_splash();
void show_dashboard();

}
