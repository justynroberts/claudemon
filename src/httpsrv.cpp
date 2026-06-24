#include "httpsrv.h"

namespace server {

// The WebServer instance is owned by the net_task on core 0. Route
// registration and the handleClient() pump both happen inside net.cpp.
// This file is intentionally just a shared accessor so main.cpp can stop
// touching network state from core 1.

static WebServer s_http(80);

WebServer& http() { return s_http; }

void begin() {
    // Intentionally empty — net::begin() owns startup of the listener.
}

void loop() {
    // Intentionally empty — net_task on core 0 pumps the server.
}

}  // namespace server
