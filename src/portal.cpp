#include "portal.h"
#include "config.h"
#include "net.h"
#include "httpsrv.h"
#include <WebServer.h>
#include <WiFi.h>

namespace portal {

// Minimal inline-styled form. Inspired-by warm-on-charcoal aesthetic; no
// third-party assets so the device never has to leave AP mode to render it.
static const char INDEX_HTML[] PROGMEM = R"HTML(<!doctype html><html><head>
<meta charset="utf-8"><meta name="viewport" content="width=device-width,initial-scale=1">
<title>claudemon setup</title>
<style>
:root { color-scheme: dark; }
body { background:#14110F; color:#F5F1EA; font-family:-apple-system,BlinkMacSystemFont,'Segoe UI',Roboto,sans-serif; margin:0; padding:24px; }
.card { max-width:420px; margin:24px auto; background:#1F1B17; border:1px solid #2A2520; border-radius:14px; padding:24px; }
h1 { margin:0 0 4px 0; font-size:22px; }
.sub { color:#8A857E; font-size:13px; margin-bottom:20px; }
label { display:block; margin:14px 0 6px; font-size:13px; color:#8A857E; }
input,select { width:100%; box-sizing:border-box; padding:11px 12px; background:#14110F; color:#F5F1EA; border:1px solid #2A2520; border-radius:8px; font-size:15px; }
input:focus,select:focus { outline:none; border-color:#CC785C; }
select { -webkit-appearance:none; appearance:none; cursor:pointer; }
button { width:100%; margin-top:22px; padding:13px; background:#CC785C; color:#14110F; border:0; border-radius:8px; font-size:15px; font-weight:600; cursor:pointer; }
.note { margin-top:18px; padding:12px; background:#14110F; border-radius:8px; font-size:12px; color:#8A857E; line-height:1.55; }
code { color:#F5F1EA; background:#2A2520; padding:1px 5px; border-radius:4px; }
.mark { display:inline-block; width:22px; height:22px; border:2px solid #CC785C; border-radius:4px; position:relative; vertical-align:-5px; margin-right:8px; }
.mark::after { content:""; position:absolute; inset:0; margin:auto; width:6px; height:6px; background:#CC785C; border-radius:50%; }
</style></head><body>
<div class="card">
  <h1><span class="mark"></span>claudemon</h1>
  <div class="sub">token monitor &mdash; first-time setup</div>
  %STATUS%
  <form method="POST" action="/portal/save">
    <label>WiFi Network</label>
    <select name="ssid" required style="font-size:17px;padding:14px 12px;height:54px">
      <option value="" disabled selected>select network&hellip;</option>
      %SCAN%
    </select>
    <label>WiFi Password <span style="color:#CC785C;font-size:11px">(visible so you can verify)</span></label>
    <input name="pass" type="text" value="%PASS%" autocomplete="off" autocorrect="off" autocapitalize="none" spellcheck="false">
    <label>Device name (mDNS)</label>
    <input name="name" value="%NAME%" autocomplete="off">
    <button type="submit">Save &amp; connect</button>
  </form>
  <div class="note">
    Ingest endpoint after reboot: <code>http://%NAME%.local/ingest</code><br>
    Shared secret (Bearer): <code>%SECRET%</code>
  </div>
  <form method="POST" action="/portal/reset" style="margin-top:12px">
    <button type="submit" style="background:#2A2520;color:#8A857E;font-size:13px;padding:10px">
      Reset saved settings
    </button>
  </form>
</div>
</body></html>)HTML";

static void send_index() {
    String body = String(INDEX_HTML);
    const auto& s = config::current();

    // Build a status banner showing the last connection result.
    String status_html;
    String err   = net::sta_fail_reason();
    String tried = s.wifi_ssid;
    if (net::was_online()) {
        status_html = "<div style='background:#2A1A0F;border:1px solid #CC785C;border-radius:8px;"
                      "padding:12px;margin-bottom:14px;font-size:13px;color:#CC785C'>"
                      "&#9888; Connected to <b>" + tried + "</b> then lost the link.</div>";
    } else if (err.length() && tried.length()) {
        String colour = err.indexOf("wrong") >= 0 ? "#CC3333" : "#CC785C";
        status_html = "<div style='background:#1A0A0A;border:1px solid " + colour + ";border-radius:8px;"
                      "padding:12px;margin-bottom:14px;font-size:13px;color:" + colour + "'>"
                      "&#10007; <b>" + tried + "</b>: " + err + "</div>";
    }

    body.replace("%STATUS%", status_html);
    body.replace("%SCAN%",   net::scan_html());
    body.replace("%PASS%",   s.wifi_pass);
    body.replace("%NAME%",   s.device_name.length() ? s.device_name : String("claudemon"));
    body.replace("%SECRET%", s.shared_secret);
    server::http().send(200, "text/html", body);
}

static void handle_save() {
    auto& w = server::http();
    String ssid = w.arg("ssid");
    String pass = w.arg("pass");
    String name = w.arg("name");
    if (ssid.length() == 0) {
        w.send(400, "text/html",
               "<html><body style='background:#14110F;color:#F5F1EA;font-family:sans-serif;padding:32px'>"
               "No SSID selected. <a href='/' style='color:#CC785C'>Go back</a></body></html>");
        return;
    }
    config::set_wifi(ssid, pass);
    if (name.length()) config::set_device(name, "");
    w.send(200, "text/html",
           "<html><body style='background:#14110F;color:#F5F1EA;font-family:sans-serif;padding:32px'>"
           "<h2 style='color:#CC785C'>Saved.</h2>Connecting&hellip; device will reboot.</body></html>");
    net::schedule_restart();  // net_task restarts after response has been sent
}

static void handle_reset() {
    auto& w = server::http();
    config::clear();
    w.send(200, "text/html",
           "<html><body style='background:#14110F;color:#F5F1EA;font-family:sans-serif;padding:32px'>"
           "<h2 style='color:#CC785C'>Settings cleared.</h2>Rebooting&hellip;</body></html>");
    net::schedule_restart();
}

void mount() {
    auto& w = server::http();
    auto idx = [](){ send_index(); };
    // Common captive-portal probe paths so iOS/Android pop the sheet.
    w.on("/",                       idx);
    w.on("/generate_204",           idx);
    w.on("/gen_204",                idx);
    w.on("/hotspot-detect.html",    idx);
    w.on("/library/test/success.html", idx);
    w.on("/ncsi.txt",               idx);
    w.on("/connecttest.txt",        idx);
    w.on("/portal/save",            HTTP_POST, handle_save);
    w.on("/portal/reset",           HTTP_POST, handle_reset);
    w.onNotFound(idx);
}

}  // namespace portal
