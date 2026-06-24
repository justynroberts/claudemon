#pragma once
#include <Arduino.h>

namespace config {

struct Settings {
    String wifi_ssid;
    String wifi_pass;
    String device_name;     // mDNS + AP name suffix
    String shared_secret;   // bearer token the host must present on /ingest
};

void load();
void save();
bool is_configured();   // true if wifi_ssid is set
void clear();           // wipe NVS, return to captive portal

const Settings& current();
void set_wifi(const String& ssid, const String& pass);
void set_device(const String& name, const String& secret);

// Generates a random hex token if none stored. Idempotent.
String ensure_secret();

}  // namespace config
