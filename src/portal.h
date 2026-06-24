#pragma once

namespace portal {

// Mounts captive-portal routes on the shared WebServer (see server.cpp).
// Hits to /, /generate_204, /hotspot-detect.html, etc. return the config
// form. POST /portal/save persists creds and reboots into STA.
void mount();

}
