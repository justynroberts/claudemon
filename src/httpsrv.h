#pragma once
#include <WiFi.h>
#include <WebServer.h>

namespace server {

void begin();
void loop();

// Shared WebServer used by both /ingest and the captive portal.
WebServer& http();

}
