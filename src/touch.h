#pragma once
#include <stdint.h>

namespace touch {
    // Lower the GT911 touch threshold from the factory 0x50 (taps never
    // exceed it) to 0x14/0x0F. Call once after smartdisplay_init().
    void fix_threshold();

    // Poll the GT911. Call from loop() at >= 50 Hz. Updates internal state;
    // call pressed()/x()/y() to read.
    void poll();

    // True while a finger is down (with ~100 ms debounce for status=0).
    bool pressed();
    int  x();
    int  y();

    // Walks the active screen and fires LV_EVENT_CLICKED on the deepest
    // clickable widget at (x, y). Replaces smartdisplay's indev (which
    // doesn't fire reliably on LVGL 9).
    void dispatch_click(int x, int y);
}
