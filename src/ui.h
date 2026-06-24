#pragma once

namespace ui {

void begin();              // builds widgets — must be called on core 1 (LVGL)
void tick();               // pulls a snapshot from store, refreshes labels/charts
void pump_anim();          // step in-flight animations; call every loop iteration
void refresh_ap_labels();  // force-update SSID/password labels; call after net::begin()

// Show the AP-mode "join this SSID" splash. Replaces the dashboard until
// net::state() returns Online.
void show_ap_splash();
void show_dashboard();

}
