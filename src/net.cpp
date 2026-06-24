#include "net.h"
#include "config.h"
#include "store.h"
#include "httpsrv.h"
#include "portal.h"
#include <WiFi.h>
#include <WebServer.h>
#include <DNSServer.h>
#include <ESPmDNS.h>
#include <ArduinoJson.h>
#include <esp_system.h>
#include <freertos/FreeRTOS.h>
#include <freertos/task.h>

// Per CLAUDE.md: LVGL must stay pinned to core 1. All WiFi / DNS / HTTP
// work runs on a dedicated task pinned to core 0. The main loop on core 1
// only touches LVGL + touch — pumping DNS/HTTP from there starves the RGB
// panel's DMA enough to underrun the pixel clock (full-screen black flicker).

namespace net {

static volatile State   s_state = State::Booting;
static String           s_ap_ssid;
static String           s_ap_pass;
static DNSServer        s_dns;
static TaskHandle_t     s_task = nullptr;
static String           s_scan_html;
static String           s_sta_fail_reason;
static volatile bool    s_was_online      = false;
static volatile uint8_t s_sta_wl_status   = 255;  // live wl_status_t during STA attempt
static volatile bool    s_restart_pending = false;

static void make_ap_creds() {
    String mac = WiFi.macAddress();
    mac.replace(":", "");
    s_ap_ssid = "claudemon-" + mac.substring(mac.length() - 4);
    s_ap_pass = "claudemon";
}

static bool auth_ok_local(WebServer& w) {
    const String& secret = config::current().shared_secret;
    if (secret.length() == 0) return true;
    String hdr = w.header("Authorization");
    if (!hdr.startsWith("Bearer ")) return false;
    return hdr.substring(7) == secret;
}

static void handle_ingest_local() {
    auto& w = server::http();
    if (!auth_ok_local(w)) { w.send(401, "application/json", "{\"error\":\"unauthorized\"}"); return; }
    if (!w.hasArg("plain")) { w.send(400, "application/json", "{\"error\":\"empty body\"}"); return; }
    JsonDocument doc;
    if (deserializeJson(doc, w.arg("plain"))) {
        w.send(400, "application/json", "{\"error\":\"bad json\"}");
        return;
    }
    auto ingest_one = [](JsonObject obj) {
        const char* env   = obj["env"]   | "default";
        const char* model = obj["model"] | "unknown";
        uint64_t input        = obj["input"]        | 0;
        uint64_t output       = obj["output"]       | 0;
        uint64_t cache_create = obj["cache_create"] | 0;
        uint64_t cache_read   = obj["cache_read"]   | 0;
        store::ingest(env, model, input, output, cache_create, cache_read);
    };
    if (doc.is<JsonArray>())  for (JsonObject o : doc.as<JsonArray>()) ingest_one(o);
    else if (doc.is<JsonObject>()) ingest_one(doc.as<JsonObject>());
    w.send(200, "application/json", "{\"ok\":true}");
}

static void handle_status_local() {
    auto& w = server::http();
    JsonDocument doc;
    doc["state"]  = s_state == State::Online ? "online" :
                    s_state == State::AP     ? "ap" : "connecting";
    doc["ip"]     = ip();
    doc["device"] = config::current().device_name;
    doc["envs"]   = store::active_env_count();
    String body;
    serializeJson(doc, body);
    w.send(200, "application/json", body);
}

// Bring up AP + captive portal and pump DNS/HTTP until WiFi creds arrive
// (or a reboot fires from the form handler).
static void run_ap_portal() {
    // Scan while still in STA mode — AP is not up yet. Use async scan so the
    // task yields every 100 ms and doesn't trigger the RTC WDT.
    s_scan_html = "";
    WiFi.mode(WIFI_STA);
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(100));
    // async, no hidden, active, 300 ms/chan — matches the proven reference.
    WiFi.scanNetworks(/*async=*/true, /*hidden=*/false, /*passive=*/false, 300);
    uint32_t scan_start = millis();
    while (WiFi.scanComplete() == WIFI_SCAN_RUNNING &&
           millis() - scan_start < 6000) {
        vTaskDelay(pdMS_TO_TICKS(100));
    }
    int n = WiFi.scanComplete();
    if (n > 0) {
        for (int i = 0; i < n && i < 24; i++) {
            s_scan_html += "<option value=\"";
            s_scan_html += WiFi.SSID(i);
            s_scan_html += "\">";
            s_scan_html += WiFi.SSID(i);
            s_scan_html += "</option>";
        }
    }
    WiFi.scanDelete();

    s_state = State::AP;
    WiFi.mode(WIFI_AP);
    WiFi.softAP(s_ap_ssid.c_str(), s_ap_pass.c_str());
    vTaskDelay(pdMS_TO_TICKS(150));
    IPAddress ap_ip = WiFi.softAPIP();
    log_i("AP up: %s @ %s", s_ap_ssid.c_str(), ap_ip.toString().c_str());

    s_dns.setErrorReplyCode(DNSReplyCode::NoError);
    s_dns.start(53, "*", ap_ip);

    portal::mount();
    auto& http = server::http();
    http.on("/ingest", HTTP_POST, handle_ingest_local);
    http.on("/status", HTTP_GET,  handle_status_local);
    const char* required[] = { "Authorization" };
    http.collectHeaders(required, 1);
    http.begin();

    for (;;) {
        if (s_restart_pending) {
            vTaskDelay(pdMS_TO_TICKS(1200));
            ESP.restart();
        }
        s_dns.processNextRequest();
        http.handleClient();
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static bool try_sta_connect(uint32_t timeout_ms) {
    const auto& cfg = config::current();
    // Minimal, proven-on-this-board sequence (matches ../cyd-rundeck-s3-clean):
    // keep the radio ON, drop any prior association, then begin(). Crucially we
    // do NOT toggle WIFI_OFF or call disconnect(true) here — disconnect(true)
    // powers the radio down, and begin() on a powered-down radio never
    // associates. Caller sets WIFI_STA mode once before the retry loop.
    WiFi.disconnect(false, false);
    vTaskDelay(pdMS_TO_TICKS(50));
    WiFi.setHostname(cfg.device_name.length() ? cfg.device_name.c_str() : "claudemon");
    WiFi.begin(cfg.wifi_ssid.c_str(), cfg.wifi_pass.c_str());
    s_state = State::Connecting;
    s_sta_wl_status = 255;
    uint32_t start = millis();
    wl_status_t final_st = WL_IDLE_STATUS;
    while (millis() - start < timeout_ms) {
        wl_status_t st = (wl_status_t)WiFi.status();
        s_sta_wl_status = (uint8_t)st;
        if (st == WL_CONNECTED && WiFi.localIP() != IPAddress(0, 0, 0, 0)) {
            return true;
        }
        // Exit early on definitive rejection — no point waiting the full timeout.
        if (st == WL_CONNECT_FAILED || st == WL_NO_SSID_AVAIL) {
            final_st = st;
            break;
        }
        vTaskDelay(pdMS_TO_TICKS(150));
    }
    if (final_st == WL_CONNECT_FAILED || (wl_status_t)WiFi.status() == WL_CONNECT_FAILED)
        s_sta_fail_reason = "wrong password";
    else if (final_st == WL_NO_SSID_AVAIL || (wl_status_t)WiFi.status() == WL_NO_SSID_AVAIL)
        s_sta_fail_reason = "network not found";
    else
        s_sta_fail_reason = "timed out — check password";
    return false;
}

static void run_sta_server() {
    s_was_online = true;
    s_state = State::Online;
    log_i("STA up: %s", WiFi.localIP().toString().c_str());

    // Advertise <device_name>.local so the host tailer's default
    // device_url = http://claudemon.local resolves without a static IP.
    const String& host = config::current().device_name;
    if (MDNS.begin(host.length() ? host.c_str() : "claudemon")) {
        MDNS.addService("http", "tcp", 80);
        log_i("mDNS: %s.local", host.length() ? host.c_str() : "claudemon");
    } else {
        log_w("mDNS start failed");
    }

    auto& http = server::http();
    http.on("/ingest", HTTP_POST, handle_ingest_local);
    http.on("/status", HTTP_GET,  handle_status_local);
    const char* required[] = { "Authorization" };
    http.collectHeaders(required, 1);
    http.begin();

    uint32_t last_link_check = millis();
    for (;;) {
        http.handleClient();
        // Cheap reconnect supervision.
        if (millis() - last_link_check > 5000) {
            last_link_check = millis();
            if (WiFi.status() != WL_CONNECTED) {
                log_w("STA dropped, reconnecting");
                s_state = State::Connecting;
                WiFi.disconnect();
                vTaskDelay(pdMS_TO_TICKS(500));
                if (try_sta_connect(30000)) {
                    s_state = State::Online;
                } else {
                    log_w("reconnect failed, restarting net task");
                    return;  // task returns; caller decides what next
                }
            }
        }
        vTaskDelay(pdMS_TO_TICKS(2));
    }
}

static void net_task(void*) {
    make_ap_creds();
    for (;;) {
        bool connected = false;
        if (config::is_configured()) {
            // Set STA mode ONCE, then retry. Don't write creds to the WiFi
            // driver's own NVS — config:: owns persistence.
            WiFi.persistent(false);
            WiFi.mode(WIFI_STA);
            vTaskDelay(pdMS_TO_TICKS(100));
            for (int attempt = 0; attempt < 3 && !connected; ++attempt) {
                if (attempt > 0) {
                    log_w("STA retry %d", attempt);
                    vTaskDelay(pdMS_TO_TICKS(1500));
                }
                connected = try_sta_connect(25000);
            }
        }
        if (connected) {
            run_sta_server();
        } else {
            log_w("no creds or STA failed after retries — opening portal");
            run_ap_portal();
        }
        vTaskDelay(pdMS_TO_TICKS(1000));
    }
}

// ----------------------------------------------------------------- API

void begin() {
    make_ap_creds();
    // Pinned to core 0 so it never competes with LVGL on core 1.
    // 16 KB: MDNS + WebServer + WiFi callbacks each chew ~2-3 KB of stack.
    xTaskCreatePinnedToCore(net_task, "net", 16384, nullptr, 1, &s_task, 0);
}

void loop() {
    // No-op on purpose. Network work runs on the core-0 task; the main
    // core-1 loop must never touch DNS / WebServer / WiFi here.
}

State    state()       { return s_state; }
String   ip() {
    if (s_state == State::Online) return WiFi.localIP().toString();
    if (s_state == State::AP)     return WiFi.softAPIP().toString();
    return "";
}
String   ssid()        { return s_state == State::Online ? WiFi.SSID() : String(); }
int      rssi()        { return WiFi.RSSI(); }
String   ap_ssid()     { return s_ap_ssid; }
String   ap_password() { return s_ap_pass; }

void enter_config_mode() {
    log_i("forcing config mode — wiping creds and restarting");
    config::clear();
    vTaskDelay(pdMS_TO_TICKS(200));
    ESP.restart();
}

void schedule_restart()  { s_restart_pending = true; }
String scan_html()       { return s_scan_html; }
String sta_fail_reason() { return s_sta_fail_reason; }
bool   was_online()      { return s_was_online; }
uint8_t sta_wl_status()  { return s_sta_wl_status; }

String last_reset_reason() {
    switch (esp_reset_reason()) {
        case ESP_RST_POWERON:  return "power-on";
        case ESP_RST_SW:       return "sw-reset";
        case ESP_RST_PANIC:    return "PANIC";
        case ESP_RST_INT_WDT:  return "INT-WDT";
        case ESP_RST_TASK_WDT: return "TASK-WDT";
        case ESP_RST_WDT:      return "WDT";
        case ESP_RST_DEEPSLEEP:return "deep-sleep";
        case ESP_RST_BROWNOUT: return "brownout";
        default:               return "rst(" + String((int)esp_reset_reason()) + ")";
    }
}

}  // namespace net
