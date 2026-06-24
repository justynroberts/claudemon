#pragma once
#include <Arduino.h>

namespace net {

enum class State {
    Booting,
    AP,          // captive portal up, waiting for config
    Connecting,  // STA attempt in progress
    Online,      // STA up, IP assigned
    Failed,      // gave up, will retry
};

void begin();    // decides AP vs STA from stored config
void loop();     // service DNS / retries

State    state();
String   ip();
String   ssid();
int      rssi();
String   ap_ssid();    // SSID we expose in AP mode
String   ap_password(); // password for the AP mode SSID

// Force fallback to AP/config mode (e.g. after a long press).
void enter_config_mode();

// Called by the portal save handler — restarts cleanly from the net task
// so the HTTP response has time to reach the browser first.
void schedule_restart();

// HTML <option> tags built from the last WiFi scan; empty if no scan yet.
String scan_html();

// Human-readable reason the last STA connection attempt failed; empty on success.
String sta_fail_reason();

// True if we reached Online state at least once this boot (connected then dropped).
bool was_online();

// Raw wl_status_t value updated every 200 ms during a STA connection attempt.
// 255 = not yet started. 0=IDLE 1=NO_SSID 3=CONNECTED 4=CONNECT_FAILED 6=DISCONNECTED
uint8_t sta_wl_status();

// Reset reason from last boot (ESP_RST_* enum value as string).
String last_reset_reason();

}  // namespace net
