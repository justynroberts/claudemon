#pragma once
#include <lvgl.h>

// Inspired-by palette — warm coral accent on deep charcoal. Generic
// dashboard aesthetic, no trademarked assets.
namespace theme {
    inline lv_color_t bg()        { return lv_color_hex(0x14110F); }  // near-black charcoal
    inline lv_color_t bg_card()   { return lv_color_hex(0x1F1B17); }  // slightly lifted card
    inline lv_color_t bg_edge()   { return lv_color_hex(0x2A2520); }  // hairline borders
    inline lv_color_t text()      { return lv_color_hex(0xF5F1EA); }  // warm off-white
    inline lv_color_t text_mute() { return lv_color_hex(0x8A857E); }  // muted body
    inline lv_color_t accent()    { return lv_color_hex(0xCC785C); }  // warm coral
    inline lv_color_t accent_dim(){ return lv_color_hex(0x7A4636); }  // coral pressed/dim
    inline lv_color_t ok()        { return lv_color_hex(0x6FBF73); }
    inline lv_color_t warn()      { return lv_color_hex(0xE6B566); }
    inline lv_color_t err()       { return lv_color_hex(0xE5604C); }
}
