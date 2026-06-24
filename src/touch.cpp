#include "touch.h"
#include <Arduino.h>
#include <driver/i2c.h>
#include <esp32_smartdisplay.h>

// GT911 fix + polling pattern copied from cyd-rundeck-s3-clean; details in
// DEVICE_NOTES.md.

namespace touch {

static constexpr i2c_port_t I2C_PORT = I2C_NUM_0;
static constexpr uint8_t    GT911_ADDR = 0x5D;

static volatile bool s_pressed = false;
static volatile int  s_x = 0, s_y = 0;
static uint32_t      s_last_touch_ms = 0;

void fix_threshold() {
    delay(50);
    uint8_t cfg[184] = {0};
    uint8_t rd[2] = { 0x80, 0x47 };
    if (i2c_master_write_read_device(I2C_PORT, GT911_ADDR, rd, 2,
                                     cfg, sizeof(cfg),
                                     pdMS_TO_TICKS(200)) != ESP_OK) {
        log_w("GT911: config read failed");
        return;
    }
    cfg[0x8053 - 0x8047] = 0x14;  // touch threshold
    cfg[0x8054 - 0x8047] = 0x0F;  // release threshold
    uint8_t sum = 0;
    for (size_t i = 0; i < sizeof(cfg); i++) sum += cfg[i];
    uint8_t checksum = (uint8_t)(0 - sum);

    uint8_t pkt[188];
    pkt[0] = 0x80; pkt[1] = 0x47;
    memcpy(&pkt[2], cfg, sizeof(cfg));
    pkt[186] = checksum;
    pkt[187] = 0x01;  // config-fresh flag — apply now
    if (i2c_master_write_to_device(I2C_PORT, GT911_ADDR, pkt, sizeof(pkt),
                                   pdMS_TO_TICKS(500)) != ESP_OK) {
        log_w("GT911: config write failed");
    } else {
        log_i("GT911: threshold patched to 0x14/0x0F");
    }
}

void poll() {
    static uint32_t last = 0;
    if (millis() - last < 20) return;
    last = millis();

    uint8_t rd[2] = { 0x81, 0x4E };
    uint8_t status = 0;
    if (i2c_master_write_read_device(I2C_PORT, GT911_ADDR, rd, 2, &status, 1,
                                     pdMS_TO_TICKS(50)) != ESP_OK) {
        return;
    }

    if (status & 0x80) {
        uint8_t npts = status & 0x0F;
        if (npts > 0) {
            uint8_t pAddr[2] = { 0x81, 0x4F };
            uint8_t buf[8] = {0};
            if (i2c_master_write_read_device(I2C_PORT, GT911_ADDR, pAddr, 2,
                    buf, sizeof(buf), pdMS_TO_TICKS(50)) == ESP_OK) {
                int rawX = buf[1] | (buf[2] << 8);
                int rawY = buf[3] | (buf[4] << 8);
                // Matches LV_DISPLAY_ROTATION_0.
                s_x = rawX;
                s_y = rawY;
                s_pressed = true;
                s_last_touch_ms = millis();
            }
        } else {
            s_pressed = false;
        }
        // Ack the chip — required, else it won't refresh the data buffer.
        uint8_t clr[3] = { 0x81, 0x4E, 0x00 };
        i2c_master_write_to_device(I2C_PORT, GT911_ADDR, clr, 3, pdMS_TO_TICKS(50));
    } else if (s_pressed && (millis() - s_last_touch_ms) > 100) {
        // status=0 means "no new data", not "finger lifted". Time out after
        // ~100 ms of silence to call it a release.
        s_pressed = false;
    }
}

bool pressed() { return s_pressed; }
int  x()       { return s_x; }
int  y()       { return s_y; }

void dispatch_click(int x, int y) {
    lv_obj_t* scr = lv_scr_act();
    if (!scr) return;
    lv_obj_t* hit = nullptr;
    int cnt = lv_obj_get_child_count(scr);
    for (int i = cnt - 1; i >= 0 && !hit; i--) {
        lv_obj_t* c = lv_obj_get_child(scr, i);
        lv_area_t a; lv_obj_get_coords(c, &a);
        if (x < a.x1 || x > a.x2 || y < a.y1 || y > a.y2) continue;
        int sub = lv_obj_get_child_count(c);
        for (int j = sub - 1; j >= 0 && !hit; j--) {
            lv_obj_t* s = lv_obj_get_child(c, j);
            lv_area_t b; lv_obj_get_coords(s, &b);
            if (x >= b.x1 && x <= b.x2 && y >= b.y1 && y <= b.y2
                && lv_obj_has_flag(s, LV_OBJ_FLAG_CLICKABLE)) hit = s;
        }
        if (!hit && lv_obj_has_flag(c, LV_OBJ_FLAG_CLICKABLE)) hit = c;
    }
    if (hit) lv_obj_send_event(hit, LV_EVENT_CLICKED, NULL);
}

}  // namespace touch
