#include "config.h"
#include <Preferences.h>
#include <esp_random.h>

namespace config {

static Settings s_settings;
static Preferences s_prefs;

static constexpr const char* NS = "claudemon";

void load() {
    s_prefs.begin(NS, true);
    s_settings.wifi_ssid     = s_prefs.getString("ssid",   "");
    s_settings.wifi_pass     = s_prefs.getString("pass",   "");
    s_settings.device_name   = s_prefs.getString("name",   "claudemon");
    s_settings.shared_secret = s_prefs.getString("secret", "");
    s_prefs.end();
}

void save() {
    s_prefs.begin(NS, false);
    s_prefs.putString("ssid",   s_settings.wifi_ssid);
    s_prefs.putString("pass",   s_settings.wifi_pass);
    s_prefs.putString("name",   s_settings.device_name);
    s_prefs.putString("secret", s_settings.shared_secret);
    s_prefs.end();
}

bool is_configured() { return s_settings.wifi_ssid.length() > 0; }

void clear() {
    // Factory-reset WiFi only. The shared secret is deliberately preserved so a
    // re-provision doesn't invalidate every host tailer's config — you copy the
    // secret once and it survives resets.
    s_prefs.begin(NS, false);
    s_prefs.remove("ssid");
    s_prefs.remove("pass");
    s_prefs.remove("name");
    s_prefs.end();
    String keep_secret = s_settings.shared_secret;
    s_settings = {};
    s_settings.shared_secret = keep_secret;
}

const Settings& current() { return s_settings; }

void set_wifi(const String& ssid, const String& pass) {
    s_settings.wifi_ssid = ssid;
    s_settings.wifi_pass = pass;
    save();
}

void set_device(const String& name, const String& secret) {
    if (name.length())   s_settings.device_name   = name;
    if (secret.length()) s_settings.shared_secret = secret;
    save();
}

String ensure_secret() {
    if (s_settings.shared_secret.length() >= 16) return s_settings.shared_secret;
    char buf[33];
    for (int i = 0; i < 16; i++) {
        snprintf(buf + i * 2, 3, "%02x", (unsigned)(esp_random() & 0xff));
    }
    buf[32] = 0;
    s_settings.shared_secret = String(buf);
    save();
    return s_settings.shared_secret;
}

}  // namespace config
