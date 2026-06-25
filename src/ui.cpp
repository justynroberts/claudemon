#include "ui.h"
#include "theme.h"
#include "store.h"
#include "net.h"
#include "config.h"
#include <Arduino.h>
#include <lvgl.h>
#include <esp32_smartdisplay.h>

// Generated assets, compiled from src/fonts/*.c (C linkage).
extern "C" {
    extern const lv_font_t hud_12, hud_14, hud_18, hud_22, hud_28, hud_48;
    extern const lv_image_dsc_t claude_logo;
}

namespace ui {

// FontAwesome glyphs baked into the hud_* fonts (UTF-8 of the codepoints).
#define ICON_WIFI  "\xEF\x87\xAB"   // F1EB
#define ICON_BOLT  "\xEF\x83\xA7"   // F0E7
#define ICON_CLOCK "\xEF\x80\x97"   // F017
#define ICON_CAL   "\xEF\x81\xB3"   // F073
#define ICON_CHART "\xEF\x88\x81"   // F201
#define ICON_FIRE  "\xEF\x81\xAD"   // F06D

// ---------------------------------------------------------------- helpers
static void fmt_tokens(uint64_t n, char* out, size_t n_out) {
    if      (n >= 1000000000ULL) snprintf(out, n_out, "%.2fB", n / 1.0e9);
    else if (n >= 1000000ULL)    snprintf(out, n_out, "%.2fM", n / 1.0e6);
    else if (n >= 1000ULL)       snprintf(out, n_out, "%.1fK", n / 1.0e3);
    else                         snprintf(out, n_out, "%llu", (unsigned long long)n);
}

static bool set_text_if_diff(lv_obj_t* lbl, const char* s, char* cache, size_t cap) {
    if (strncmp(cache, s, cap - 1) == 0) return false;
    strncpy(cache, s, cap - 1);
    cache[cap - 1] = 0;
    lv_label_set_text(lbl, s);
    return true;
}

// intensity ramp: dim coral -> coral -> amber for the hottest buckets.
static lv_color_t heat(uint32_t v, uint32_t mx) {
    if (!mx || !v) return theme::accent_dim();
    uint32_t p = (uint32_t)((uint64_t)v * 100 / mx);
    if (p >= 66) return theme::warn();     // amber peaks
    if (p >= 30) return theme::accent();    // coral mid
    return theme::accent_dim();             // low
}

// ---------------------------------------------------------------- screens
static lv_obj_t* s_ap_screen = nullptr;
static lv_obj_t* s_dash      = nullptr;

// status bar
static lv_obj_t* s_status_ssid = nullptr;
static lv_obj_t* s_status_ip   = nullptr;
static lv_obj_t* s_status_dot  = nullptr;

// hero
static lv_obj_t* s_hero_total = nullptr;
static lv_obj_t* s_hero_rate  = nullptr;
static lv_obj_t* s_hero_day   = nullptr;

// hero count-up animation
static bool      s_anim_active = false;
static uint64_t  s_anim_from   = 0;
static uint64_t  s_anim_to     = 0;
static uint32_t  s_anim_start  = 0;
static char      s_total_cache[24] = "";
static constexpr uint32_t ANIM_MS = 550;

// activity graph -----------------------------------------------------------
static constexpr int      GRAPH_BARS    = 56;
static constexpr uint32_t AGG_BUCKET_MS = 3000;                  // 3s/bar
static constexpr int      RATE_BUCKETS  = 60000 / AGG_BUCKET_MS; // last 60s
static lv_obj_t* s_bars[GRAPH_BARS];
static int32_t   s_bar_last_h[GRAPH_BARS];
static uint32_t  s_agg_hist[GRAPH_BARS];
static int       s_agg_head         = GRAPH_BARS - 1;
static uint32_t  s_agg_bucket_start = 0;
static uint64_t  s_last_grand       = 0;
static constexpr int GRAPH_BASE_Y = 102;   // baseline within graph card
static constexpr int GRAPH_MAX_H  = 60;
static constexpr int GRAPH_CARD_W = 456;
static constexpr int GRAPH_MARGIN = 14;    // inset so bars clear the rounded corners
static constexpr int GRAPH_BAR_W  = 6;
// Evenly spread bars between the insets so the rightmost never clips the border.
static inline int graph_bar_x(int i) {
    return GRAPH_MARGIN + i * (GRAPH_CARD_W - 2 * GRAPH_MARGIN - GRAPH_BAR_W) / (GRAPH_BARS - 1);
}

// ranked rows with per-env sparklines --------------------------------------
static constexpr int N_ROWS    = 4;
static constexpr int SPARK_BARS = 22;
struct Row {
    lv_obj_t* rank;
    lv_obj_t* name;
    lv_obj_t* tokens;
    lv_obj_t* spark[SPARK_BARS];
    int32_t   spark_h[SPARK_BARS];
    char      last_name[32];
    char      last_tok[24];
};
static Row s_rows[N_ROWS];
static constexpr int ROW_Y0 = 266, ROW_H = 52;
static constexpr int SPARK_X0 = 40, SPARK_PITCH = 18, SPARK_W = 13;
static constexpr int SPARK_MAX_H = 14;
static constexpr int SPARK_TOP   = 28;   // sparkline offset within a row (leaves space below)

// ---------------------------------------------------------------- build
static void build_mark(lv_obj_t* parent, int x, int y, int size) {
    lv_obj_t* mark = lv_obj_create(parent);
    lv_obj_set_size(mark, size, size);
    lv_obj_set_pos(mark, x, y);
    lv_obj_set_style_bg_opa(mark, LV_OPA_TRANSP, 0);
    lv_obj_set_style_border_color(mark, theme::accent(), 0);
    lv_obj_set_style_border_width(mark, 2, 0);
    lv_obj_set_style_radius(mark, 4, 0);
    lv_obj_set_style_pad_all(mark, 0, 0);
    lv_obj_clear_flag(mark, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_t* dot = lv_obj_create(mark);
    int ds = size / 4;
    lv_obj_set_size(dot, ds, ds);
    lv_obj_center(dot);
    lv_obj_set_style_bg_color(dot, theme::accent(), 0);
    lv_obj_set_style_border_width(dot, 0, 0);
    lv_obj_set_style_radius(dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_clear_flag(dot, LV_OBJ_FLAG_SCROLLABLE);
}

static lv_obj_t* mk_card(lv_obj_t* parent, int x, int y, int w, int h) {
    lv_obj_t* c = lv_obj_create(parent);
    lv_obj_set_size(c, w, h);
    lv_obj_set_pos(c, x, y);
    lv_obj_set_style_bg_color(c, theme::bg_card(), 0);
    lv_obj_set_style_bg_opa(c, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(c, theme::bg_edge(), 0);
    lv_obj_set_style_border_width(c, 1, 0);
    lv_obj_set_style_radius(c, 12, 0);
    lv_obj_set_style_pad_all(c, 0, 0);
    lv_obj_clear_flag(c, LV_OBJ_FLAG_SCROLLABLE);
    return c;
}

static lv_obj_t* mk_label(lv_obj_t* parent, const char* txt, const lv_font_t* font,
                          lv_color_t color, int x, int y) {
    lv_obj_t* l = lv_label_create(parent);
    lv_label_set_text(l, txt);
    lv_obj_set_style_text_font(l, font, 0);
    lv_obj_set_style_text_color(l, color, 0);
    lv_obj_set_pos(l, x, y);
    return l;
}

static lv_obj_t* mk_bar(lv_obj_t* parent, int x, int y, int w, int h, lv_color_t col) {
    lv_obj_t* b = lv_obj_create(parent);
    lv_obj_set_size(b, w, h);
    lv_obj_set_pos(b, x, y);
    lv_obj_set_style_bg_color(b, col, 0);
    lv_obj_set_style_bg_opa(b, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(b, 0, 0);
    lv_obj_set_style_radius(b, 1, 0);
    lv_obj_set_style_pad_all(b, 0, 0);
    lv_obj_clear_flag(b, LV_OBJ_FLAG_SCROLLABLE);
    return b;
}

static void build_status_bar(lv_obj_t* parent) {
    lv_obj_t* bar = lv_obj_create(parent);
    lv_obj_set_size(bar, 480, 44);
    lv_obj_set_pos(bar, 0, 0);
    lv_obj_set_style_bg_color(bar, theme::bg(), 0);
    lv_obj_set_style_bg_opa(bar, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(bar, theme::bg_edge(), 0);
    lv_obj_set_style_border_side(bar, LV_BORDER_SIDE_BOTTOM, 0);
    lv_obj_set_style_border_width(bar, 1, 0);
    lv_obj_set_style_pad_all(bar, 0, 0);
    lv_obj_clear_flag(bar, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t* logo = lv_image_create(bar);
    lv_image_set_src(logo, &claude_logo);
    lv_obj_set_pos(logo, 12, 6);

    s_status_dot = lv_obj_create(bar);
    lv_obj_set_size(s_status_dot, 8, 8);
    lv_obj_set_style_bg_color(s_status_dot, theme::warn(), 0);
    lv_obj_set_style_border_width(s_status_dot, 0, 0);
    lv_obj_set_style_radius(s_status_dot, LV_RADIUS_CIRCLE, 0);
    lv_obj_set_pos(s_status_dot, 320, 18);
    lv_obj_clear_flag(s_status_dot, LV_OBJ_FLAG_SCROLLABLE);

    s_status_ssid = mk_label(bar, "", &hud_12, theme::text_mute(), 336, 8);
    s_status_ip   = mk_label(bar, "", &hud_12, theme::text_mute(), 336, 22);
}

static void build_hero(lv_obj_t* parent) {
    lv_obj_t* card = mk_card(parent, 12, 52, 456, 92);
    mk_label(card, "TOTAL TOKENS", &hud_12, theme::text_mute(), 14, 8);
    // Number sits with clear space above the card's lower edge so it never
    // crowds the graph card below it.
    s_hero_total = mk_label(card, "0", &hud_48, theme::accent(), 13, 26);

    // Live rate (per-min) and projected per-day, stacked on the right.
    s_hero_rate = mk_label(card, "", &hud_18, theme::ok(), 0, 30);
    lv_obj_align(s_hero_rate, LV_ALIGN_TOP_RIGHT, -14, 30);
    s_hero_day = mk_label(card, "", &hud_14, theme::text_mute(), 0, 56);
    lv_obj_align(s_hero_day, LV_ALIGN_TOP_RIGHT, -14, 58);
}

static void build_graph(lv_obj_t* parent) {
    lv_obj_t* card = mk_card(parent, 12, 148, GRAPH_CARD_W, 110);
    // Icon in its own larger label so the glyph isn't clipped by the small text line.
    mk_label(card, ICON_CHART, &hud_18, theme::text_mute(), 14, 13);
    mk_label(card, "ACTIVITY", &hud_12, theme::text_mute(), 44, 19);
    mk_label(card, "live - tokens / 3s", &hud_12, theme::accent_dim(), 326, 19);
    for (int i = 0; i < GRAPH_BARS; i++) {
        s_bars[i] = mk_bar(card, graph_bar_x(i), GRAPH_BASE_Y - 2, GRAPH_BAR_W, 2, theme::accent_dim());
        s_bar_last_h[i] = 2;
    }
}

static void build_rows(lv_obj_t* parent) {
    for (int i = 0; i < N_ROWS; i++) {
        int y = ROW_Y0 + i * ROW_H;
        char rk[4]; snprintf(rk, sizeof(rk), "%d", i + 1);
        s_rows[i].rank = mk_label(parent, rk, &hud_14, theme::accent_dim(), 14, y + 2);
        s_rows[i].name = mk_label(parent, "", &hud_18, theme::text(), 40, y);
        lv_label_set_long_mode(s_rows[i].name, LV_LABEL_LONG_DOT);
        lv_obj_set_width(s_rows[i].name, 220);

        s_rows[i].tokens = lv_label_create(parent);
        lv_label_set_text(s_rows[i].tokens, "");
        lv_obj_set_style_text_font(s_rows[i].tokens, &hud_18, 0);
        lv_obj_set_style_text_color(s_rows[i].tokens, theme::accent(), 0);
        lv_obj_align(s_rows[i].tokens, LV_ALIGN_TOP_RIGHT, -14, y);

        int sy = y + SPARK_TOP;  // sparkline area, leaves clear space below the row
        for (int k = 0; k < SPARK_BARS; k++) {
            s_rows[i].spark[k] = mk_bar(parent, SPARK_X0 + k * SPARK_PITCH, sy + SPARK_MAX_H - 2,
                                        SPARK_W, 2, theme::accent_dim());
            s_rows[i].spark_h[k] = 2;
        }
        s_rows[i].last_name[0] = 0;
        s_rows[i].last_tok[0]  = 0;

        // Divider between projects — sits below the sparkline with space under it.
        if (i < N_ROWS - 1) {
            lv_obj_t* div = lv_obj_create(parent);
            lv_obj_set_size(div, GRAPH_CARD_W - 28, 1);
            lv_obj_set_pos(div, 14, y + ROW_H - 8);
            lv_obj_set_style_bg_color(div, theme::bg_edge(), 0);
            lv_obj_set_style_bg_opa(div, LV_OPA_40, 0);
            lv_obj_set_style_border_width(div, 0, 0);
            lv_obj_set_style_radius(div, 0, 0);
            lv_obj_clear_flag(div, LV_OBJ_FLAG_SCROLLABLE);
        }
    }
}

static void build_dashboard() {
    s_dash = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_dash, theme::bg(), 0);
    lv_obj_set_style_bg_opa(s_dash, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_dash, 0, 0);
    lv_obj_set_style_pad_all(s_dash, 0, 0);
    lv_obj_clear_flag(s_dash, LV_OBJ_FLAG_SCROLLABLE);

    build_status_bar(s_dash);
    build_hero(s_dash);
    build_graph(s_dash);
    build_rows(s_dash);
}

// Reset-warning overlay (lives on the top layer, above whichever screen is up).
static lv_obj_t* s_reset_overlay = nullptr;
static lv_obj_t* s_reset_lbl     = nullptr;

static void build_reset_overlay() {
    s_reset_overlay = lv_obj_create(lv_layer_top());
    lv_obj_set_size(s_reset_overlay, 380, 110);
    lv_obj_center(s_reset_overlay);
    lv_obj_set_style_bg_color(s_reset_overlay, lv_color_hex(0x2A0F0A), 0);
    lv_obj_set_style_bg_opa(s_reset_overlay, LV_OPA_COVER, 0);
    lv_obj_set_style_border_color(s_reset_overlay, theme::err(), 0);
    lv_obj_set_style_border_width(s_reset_overlay, 2, 0);
    lv_obj_set_style_radius(s_reset_overlay, 14, 0);
    lv_obj_clear_flag(s_reset_overlay, LV_OBJ_FLAG_SCROLLABLE);
    lv_obj_add_flag(s_reset_overlay, LV_OBJ_FLAG_HIDDEN);

    s_reset_lbl = lv_label_create(s_reset_overlay);
    lv_obj_set_style_text_font(s_reset_lbl, &hud_22, 0);
    lv_obj_set_style_text_color(s_reset_lbl, theme::err(), 0);
    lv_obj_set_style_text_align(s_reset_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_label_set_text(s_reset_lbl, "");
    lv_obj_center(s_reset_lbl);
}

void reset_hint(int secs) {
    static int last = -2;
    if (secs == last || !s_reset_overlay) return;
    last = secs;
    if (secs < 0) {
        lv_obj_add_flag(s_reset_overlay, LV_OBJ_FLAG_HIDDEN);
    } else {
        char b[64];
        snprintf(b, sizeof(b), ICON_BOLT "  WIPING WIFI IN %d\nrelease to cancel", secs);
        lv_label_set_text(s_reset_lbl, b);
        lv_obj_center(s_reset_lbl);
        lv_obj_clear_flag(s_reset_overlay, LV_OBJ_FLAG_HIDDEN);
    }
    lv_refr_now(NULL);
}

// AP splash widgets
static lv_obj_t* s_ap_ssid_lbl  = nullptr;
static lv_obj_t* s_ap_pass_lbl  = nullptr;
static lv_obj_t* s_ap_error_lbl = nullptr;

static void build_ap_screen() {
    s_ap_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_ap_screen, theme::bg(), 0);
    lv_obj_set_style_bg_opa(s_ap_screen, LV_OPA_COVER, 0);
    lv_obj_set_style_border_width(s_ap_screen, 0, 0);
    lv_obj_set_style_pad_all(s_ap_screen, 0, 0);
    lv_obj_clear_flag(s_ap_screen, LV_OBJ_FLAG_SCROLLABLE);

    build_mark(s_ap_screen, 220, 100, 40);

    lv_obj_t* title = mk_label(s_ap_screen, "setup mode", &hud_28, theme::text(), 0, 0);
    lv_obj_align(title, LV_ALIGN_TOP_MID, 0, 160);
    lv_obj_t* hint = mk_label(s_ap_screen, "join this network:", &hud_14, theme::text_mute(), 0, 0);
    lv_obj_align(hint, LV_ALIGN_TOP_MID, 0, 210);

    s_ap_ssid_lbl = lv_label_create(s_ap_screen);
    lv_label_set_text(s_ap_ssid_lbl, "starting...");
    lv_obj_set_style_text_color(s_ap_ssid_lbl, theme::accent(), 0);
    lv_obj_set_style_text_font(s_ap_ssid_lbl, &hud_22, 0);
    lv_obj_set_width(s_ap_ssid_lbl, 480);
    lv_obj_set_style_text_align(s_ap_ssid_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ap_ssid_lbl, 0, 234);

    s_ap_pass_lbl = lv_label_create(s_ap_screen);
    lv_label_set_text(s_ap_pass_lbl, "");
    lv_obj_set_style_text_color(s_ap_pass_lbl, theme::text_mute(), 0);
    lv_obj_set_style_text_font(s_ap_pass_lbl, &hud_14, 0);
    lv_obj_set_width(s_ap_pass_lbl, 480);
    lv_obj_set_style_text_align(s_ap_pass_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ap_pass_lbl, 0, 270);

    s_ap_error_lbl = lv_label_create(s_ap_screen);
    lv_label_set_text(s_ap_error_lbl, "");
    lv_obj_set_style_text_color(s_ap_error_lbl, theme::warn(), 0);
    lv_obj_set_style_text_font(s_ap_error_lbl, &hud_14, 0);
    lv_obj_set_width(s_ap_error_lbl, 480);
    lv_obj_set_style_text_align(s_ap_error_lbl, LV_TEXT_ALIGN_CENTER, 0);
    lv_obj_set_pos(s_ap_error_lbl, 0, 306);

    lv_obj_t* footer = mk_label(s_ap_screen, "open http://192.168.4.1 in a browser",
                                &hud_12, theme::text_mute(), 0, 0);
    lv_obj_align(footer, LV_ALIGN_BOTTOM_MID, 0, -32);
}

void begin() {
    build_ap_screen();
    build_dashboard();
    build_reset_overlay();
    s_agg_bucket_start = millis();
    show_ap_splash();
}

static bool s_showing_ap = false;

static void load_screen(lv_obj_t* scr) {
    if (!scr) return;
    lv_scr_load(scr);
    lv_refr_now(NULL);
    lv_refr_now(NULL);
}

void show_ap_splash() {
    if (s_showing_ap) return;
    s_showing_ap = true;
    load_screen(s_ap_screen);
}

void show_dashboard() {
    if (!s_showing_ap) return;
    s_showing_ap = false;
    load_screen(s_dash);
}

static bool refresh_status_bar() {
    static char last_ssid[48] = "";
    static char last_ip[24]   = "";
    static net::State last_st = (net::State)-1;
    bool changed = false;
    auto st = net::state();
    if (st != last_st) {
        last_st = st;
        lv_obj_set_style_bg_color(s_status_dot,
            st == net::State::Online ? theme::ok() : theme::warn(), 0);
        changed = true;
    }
    char ssid_text[48];
    if (st == net::State::Online)
        snprintf(ssid_text, sizeof(ssid_text), ICON_WIFI " %s", net::ssid().c_str());
    else if (st == net::State::AP)
        strcpy(ssid_text, ICON_WIFI " AP mode");
    else
        strcpy(ssid_text, "connecting...");
    const char* ip_text =
        st == net::State::Online ? net::ip().c_str() :
        st == net::State::AP     ? net::ip().c_str() : "";
    changed |= set_text_if_diff(s_status_ssid, ssid_text, last_ssid, sizeof(last_ssid));
    changed |= set_text_if_diff(s_status_ip,   ip_text,   last_ip,   sizeof(last_ip));
    return changed;
}

static void refresh_ap_screen() {
    if (!s_ap_ssid_lbl || !s_ap_pass_lbl) return;
    auto st = net::state();
    if (st == net::State::Connecting) {
        lv_label_set_text(s_ap_ssid_lbl, "connecting to:");
        lv_label_set_text(s_ap_pass_lbl, config::current().wifi_ssid.c_str());
        if (s_ap_error_lbl) {
            uint8_t wl = net::sta_wl_status();
            const char* wl_str =
                wl == 255 ? "starting..." :
                wl == 0   ? "idle" :
                wl == 1   ? "SSID not found" :
                wl == 3   ? "connected! waiting for IP..." :
                wl == 4   ? "WRONG PASSWORD (auth rejected)" :
                wl == 6   ? "disconnected" : "...";
            lv_label_set_text(s_ap_error_lbl, wl_str);
        }
    } else if (st == net::State::AP) {
        String ssid = net::ap_ssid();
        lv_label_set_text(s_ap_ssid_lbl, ssid.length() ? ssid.c_str() : "claudemon");
        char pw[64];
        snprintf(pw, sizeof(pw), "password: %s", net::ap_password().c_str());
        lv_label_set_text(s_ap_pass_lbl, pw);
        if (s_ap_error_lbl) {
            String err   = net::sta_fail_reason();
            String tried = config::current().wifi_ssid;
            String msg;
            if (tried.length() == 0)    msg = "no saved network";
            else if (net::was_online()) { msg = tried + ": connected then dropped";
                                          if (err.length()) msg += " (" + err + ")"; }
            else if (err.length())      msg = tried + ": " + err;
            lv_label_set_text(s_ap_error_lbl, msg.c_str());
        }
    } else {
        String ssid = net::ap_ssid();
        lv_label_set_text(s_ap_ssid_lbl, ssid.length() ? ssid.c_str() : "claudemon");
        lv_label_set_text(s_ap_pass_lbl, "");
        if (s_ap_error_lbl) {
            String rst = "boot: " + net::last_reset_reason();
            lv_label_set_text(s_ap_error_lbl, rst.c_str());
        }
    }
}

void refresh_ap_labels() { refresh_ap_screen(); }

static void agg_update(uint64_t grand) {
    uint32_t now = millis();
    if (grand < s_last_grand) s_last_grand = grand;
    uint32_t delta = (uint32_t)(grand - s_last_grand);
    s_last_grand = grand;
    while (now - s_agg_bucket_start >= AGG_BUCKET_MS) {
        s_agg_head = (s_agg_head + 1) % GRAPH_BARS;
        s_agg_hist[s_agg_head] = 0;
        s_agg_bucket_start += AGG_BUCKET_MS;
    }
    s_agg_hist[s_agg_head] += delta;
}

static bool refresh_graph() {
    uint32_t maxv = 1;
    for (int i = 0; i < GRAPH_BARS; i++)
        if (s_agg_hist[i] > maxv) maxv = s_agg_hist[i];
    bool changed = false;
    for (int i = 0; i < GRAPH_BARS; i++) {
        int bucket = (s_agg_head + 1 + i) % GRAPH_BARS;  // oldest..newest L->R
        uint32_t v = s_agg_hist[bucket];
        int32_t h = (int32_t)((uint64_t)v * GRAPH_MAX_H / maxv);
        if (h < 2) h = 2;
        lv_color_t col = (i == GRAPH_BARS - 1 && v > 0) ? theme::ok() : heat(v, maxv);
        if (h != s_bar_last_h[i]) {
            lv_obj_set_size(s_bars[i], GRAPH_BAR_W, h);
            lv_obj_set_pos(s_bars[i], graph_bar_x(i), GRAPH_BASE_Y - h);
            lv_obj_set_style_bg_color(s_bars[i], col, 0);
            s_bar_last_h[i] = h;
            changed = true;
        }
    }
    return changed;
}

static bool refresh_hero(uint64_t grand) {
    // Total: kick a count-up animation toward the new value. pump_anim() renders
    // the intermediate numbers + repaints; we don't set the label text here.
    if (grand != s_anim_to) {
        s_anim_from   = s_anim_to;
        s_anim_to     = grand;
        s_anim_start  = millis();
        s_anim_active = true;
    }

    static char last_rate[24] = "";
    static char last_day[24]  = "";
    uint64_t rate = 0;   // tokens in the last 60 s
    for (int i = 0; i < RATE_BUCKETS && i < GRAPH_BARS; i++)
        rate += s_agg_hist[(s_agg_head - i + GRAPH_BARS) % GRAPH_BARS];

    char tb[16]; fmt_tokens(rate, tb, sizeof(tb));
    char rb[24]; snprintf(rb, sizeof(rb), ICON_CLOCK " +%s/min", tb);
    bool rchanged = set_text_if_diff(s_hero_rate, rb, last_rate, sizeof(last_rate));
    if (rchanged) {
        lv_obj_set_style_text_color(s_hero_rate, rate ? theme::ok() : theme::text_mute(), 0);
        lv_obj_align(s_hero_rate, LV_ALIGN_TOP_RIGHT, -14, 30);
    }

    uint64_t day = rate * 1440ULL;   // projected from the last-60 s rate
    char db[16]; fmt_tokens(day, db, sizeof(db));
    char dbuf[24]; snprintf(dbuf, sizeof(dbuf), ICON_CAL " ~%s/day", db);
    bool dchanged = set_text_if_diff(s_hero_day, dbuf, last_day, sizeof(last_day));
    if (dchanged) lv_obj_align(s_hero_day, LV_ALIGN_TOP_RIGHT, -14, 58);

    return rchanged || dchanged;
}

void pump_anim() {
    if (!s_anim_active) return;
    uint32_t e = millis() - s_anim_start;
    float t = (e >= ANIM_MS) ? 1.0f : (float)e / ANIM_MS;
    float ease = 1.0f - (1.0f - t) * (1.0f - t);   // ease-out quad
    uint64_t cur;
    if (t >= 1.0f) { cur = s_anim_to; s_anim_active = false; }
    else if (s_anim_to >= s_anim_from)
        cur = s_anim_from + (uint64_t)((double)(s_anim_to - s_anim_from) * ease);
    else
        cur = s_anim_from - (uint64_t)((double)(s_anim_from - s_anim_to) * ease);
    char b[24]; fmt_tokens(cur, b, sizeof(b));
    if (set_text_if_diff(s_hero_total, b, s_total_cache, sizeof(s_total_cache)))
        lv_refr_now(NULL);
}

static bool refresh_rows() {
    static store::Env snap[store::MAX_ENVS];
    store::snapshot(snap);

    int order[store::MAX_ENVS];
    uint64_t tot[store::MAX_ENVS];
    int n = 0;
    for (int i = 0; i < store::MAX_ENVS; i++) {
        if (!snap[i].active) continue;
        tot[i] = snap[i].total_input + snap[i].total_output
               + snap[i].total_cache_create + snap[i].total_cache_read;
        order[n++] = i;
    }
    for (int a = 1; a < n; a++) {
        int v = order[a]; int b = a - 1;
        while (b >= 0 && tot[order[b]] < tot[v]) { order[b + 1] = order[b]; b--; }
        order[b + 1] = v;
    }

    bool changed = false;
    for (int r = 0; r < N_ROWS; r++) {
        Row& row = s_rows[r];
        if (r < n) {
            store::Env& e = snap[order[r]];
            changed |= set_text_if_diff(row.name, e.label, row.last_name, sizeof(row.last_name));
            char tb[24]; fmt_tokens(tot[order[r]], tb, sizeof(tb));
            changed |= set_text_if_diff(row.tokens, tb, row.last_tok, sizeof(row.last_tok));

            // per-env sparkline: last SPARK_BARS minute buckets, oldest -> newest
            uint32_t mx = 1;
            for (int k = 0; k < SPARK_BARS; k++) {
                int idx = (e.history_head - (SPARK_BARS - 1) + k + store::HISTORY_BUCKETS * 2)
                          % store::HISTORY_BUCKETS;
                if (e.history[idx] > mx) mx = e.history[idx];
            }
            for (int k = 0; k < SPARK_BARS; k++) {
                int idx = (e.history_head - (SPARK_BARS - 1) + k + store::HISTORY_BUCKETS * 2)
                          % store::HISTORY_BUCKETS;
                uint32_t v = e.history[idx];
                int32_t h = (int32_t)((uint64_t)v * SPARK_MAX_H / mx);
                if (h < 2) h = 2;
                int sy = ROW_Y0 + r * ROW_H + SPARK_TOP;
                if (h != row.spark_h[k]) {
                    lv_obj_set_size(row.spark[k], SPARK_W, h);
                    lv_obj_set_pos(row.spark[k], SPARK_X0 + k * SPARK_PITCH, sy + SPARK_MAX_H - h);
                    lv_obj_set_style_bg_color(row.spark[k], heat(v, mx), 0);
                    row.spark_h[k] = h;
                    changed = true;
                }
            }
        } else {
            changed |= set_text_if_diff(row.name, "", row.last_name, sizeof(row.last_name));
            changed |= set_text_if_diff(row.tokens, "", row.last_tok, sizeof(row.last_tok));
            for (int k = 0; k < SPARK_BARS; k++) {
                if (row.spark_h[k] != 0) {
                    lv_obj_set_size(row.spark[k], SPARK_W, 1);
                    lv_obj_set_style_bg_color(row.spark[k], theme::bg_card(), 0);
                    row.spark_h[k] = 0;
                    changed = true;
                }
            }
        }
    }
    return changed;
}

void tick() {
    static uint32_t last = 0;
    if (millis() - last < 500) return;
    last = millis();

    if (net::state() != net::State::Online) {
        refresh_ap_screen();
        if (!s_showing_ap) show_ap_splash();
        return;
    }
    if (s_showing_ap) show_dashboard();

    uint64_t grand = store::grand_total_tokens();
    agg_update(grand);

    bool changed = false;
    changed |= refresh_status_bar();
    changed |= refresh_hero(grand);
    changed |= refresh_graph();
    changed |= refresh_rows();

    // LVGL 9 + RGB DMA on this panel doesn't reliably flush invalidations on
    // its own; force a repaint only when something changed.
    if (changed) lv_refr_now(NULL);
}

}  // namespace ui
