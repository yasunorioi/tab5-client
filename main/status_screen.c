// status_screen.c — see status_screen.h.
//
// Client status page: fix quality, dual-antenna attitude, and ellipsoidal height
// from the SBF stream, plus WiFi/Mosaic link state and the NMEA skyplot. The
// large cut/fill readout replaces the height line once the survey/plane UX lands
// (docs/todo.md — cut/fill display).

#include "status_screen.h"
#include "display.h"

#include <string.h>
#include <math.h>
#include "esp_log.h"
#include "esp_check.h"
#include "esp_timer.h"
#include "esp_lvgl_port.h"
#include "lvgl.h"

#include "gnss_state.h"
#include "usb_cdc_source.h"
#include "wifi_sta.h"
#include "gnss_view.h"

static const char *TAG = "status_ui";

// Palette. C_BG is chosen to survive RGB565 quantization identically to the
// boot pre-fill (display_fill 0x08A4) so the area below the widgets — which LVGL
// leaves untouched — is a seamless match to the LVGL-painted background.
#define C_BG      0x081420
#define C_TITLE   0xFFFFFF
#define C_OK      0x33DD66
#define C_WARN    0xFFCC33
#define C_BAD     0xFF5555
#define C_IDLE    0x8899AA

static lv_obj_t *s_fix, *s_att, *s_height, *s_wifi, *s_mosaic;

static lv_obj_t *mk_line(lv_obj_t *parent)
{
    lv_obj_t *l = lv_label_create(parent);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(C_IDLE), 0);
    lv_obj_set_style_pad_bottom(l, 14, 0);
    lv_label_set_long_mode(l, LV_LABEL_LONG_CLIP);
    lv_obj_set_width(l, LV_PCT(100));
    lv_label_set_text(l, "");
    return l;
}

static void set_line(lv_obj_t *l, uint32_t color, const char *text)
{
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_label_set_text(l, text);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 28, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_START,
                          LV_FLEX_ALIGN_START);

    lv_obj_t *title = lv_label_create(scr);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(C_TITLE), 0);
    lv_obj_set_style_pad_bottom(title, 28, 0);
    lv_label_set_text(title, "TAB5 RTK LEVELER");

    s_fix    = mk_line(scr);
    s_att    = mk_line(scr);
    s_height = mk_line(scr);
    s_wifi   = mk_line(scr);
    s_mosaic = mk_line(scr);

    // GNSS skyplot + C/N0 bars below the status lines (portrait has the room).
    gnss_view_build(scr);
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    char buf[96];
    int64_t now = esp_timer_get_time();

    gnss_snapshot_t g;
    gnss_state_snapshot(&g);
    int64_t age_ms = g.last_block_us ? (now - g.last_block_us) / 1000 : -1;
    bool fresh = (age_ms >= 0 && age_ms < 3000);

    // Fix quality (mode + satellites), with CRC health folded into the colour.
    uint32_t col;
    if (!fresh) {
        col = C_BAD;
        snprintf(buf, sizeof(buf), "Fix    no SBF");
    } else if (g.pvt_valid) {
        // RTK fixed(4)/float(5) or a fixed location(3) is a usable solution.
        bool solved = (g.pvt.mode_type >= 3);
        col = solved ? (g.crc_failed == 0 ? C_OK : C_WARN) : C_WARN;
        snprintf(buf, sizeof(buf), "Fix    %s   sv %u",
                 sbf_pvt_mode_str(g.pvt.mode_type), g.pvt.nr_sv);
    } else {
        col = C_WARN;
        snprintf(buf, sizeof(buf), "Fix    waiting...");
    }
    set_line(s_fix, col, buf);

    // Dual-antenna attitude (pitch/roll for the blade, heading for reference).
    if (g.att_valid && !isnan(g.att.pitch_deg)) {
        double hd = isnan(g.att.heading_deg) ? 0.0 : g.att.heading_deg;
        snprintf(buf, sizeof(buf), "Att    P%+.1f  R%+.1f  H%.0f",
                 g.att.pitch_deg, g.att.roll_deg, hd);
        set_line(s_att, fresh ? C_OK : C_IDLE, buf);
    } else {
        set_line(s_att, C_IDLE, "Att    —");
    }

    // Ellipsoidal height + vertical accuracy (cut/fill readout lands here later).
    if (g.pvt_valid && !isnan(g.pvt.height_m)) {
        if (!isnan(g.pvt.v_accuracy_m))
            snprintf(buf, sizeof(buf), "Height %.3f m  +/-%.0fcm",
                     g.pvt.height_m, g.pvt.v_accuracy_m * 100.0);
        else
            snprintf(buf, sizeof(buf), "Height %.3f m", g.pvt.height_m);
        set_line(s_height, fresh ? C_OK : C_IDLE, buf);
    } else {
        set_line(s_height, C_IDLE, "Height —");
    }

    // WiFi.
    wifi_status_t w;
    wifi_sta_status(&w);
    if (w.connected) {
        snprintf(buf, sizeof(buf), "WiFi   %s  %s", w.ssid, w.ip);
        set_line(s_wifi, C_OK, buf);
    } else if (w.ssid[0]) {
        snprintf(buf, sizeof(buf), "WiFi   %s  connecting...", w.ssid);
        set_line(s_wifi, C_WARN, buf);
    } else {
        set_line(s_wifi, C_IDLE, "WiFi   not set (wifiset)");
    }

    // Mosaic / USB.
    usb_cdc_status_t us;
    usb_cdc_source_status(&us);
    if (us.cdc_open && us.stream_itf != 0xFF) {
        snprintf(buf, sizeof(buf), "Mosaic %04X:%04X  itf%d", us.vid, us.pid,
                 (int8_t)us.stream_itf);
        set_line(s_mosaic, C_OK, buf);
    } else if (us.device_attached) {
        set_line(s_mosaic, C_WARN, "Mosaic attached, searching COM...");
    } else {
        set_line(s_mosaic, C_BAD, "Mosaic —  (no receiver)");
    }

    gnss_view_update();   // skyplot + C/N0 bars from the NMEA snapshot
}

esp_err_t status_screen_start(void)
{
    const lvgl_port_cfg_t pcfg = ESP_LVGL_PORT_INIT_CONFIG();
    ESP_RETURN_ON_ERROR(lvgl_port_init(&pcfg), TAG, "lvgl_port_init");

    const lvgl_port_display_cfg_t disp_cfg = {
        .io_handle     = display_io(),
        .panel_handle  = display_panel(),
        .buffer_size   = TAB5_LCD_H_RES * 120,   // partial buffer, PSRAM
        .double_buffer = true,
        .hres          = TAB5_LCD_H_RES,
        .vres          = TAB5_LCD_V_RES,
        .color_format  = LV_COLOR_FORMAT_RGB565,
        .flags = { .buff_spiram = true },
    };
    const lvgl_port_display_dsi_cfg_t dsi_cfg = { .flags = { .avoid_tearing = false } };

    lv_display_t *disp = lvgl_port_add_disp_dsi(&disp_cfg, &dsi_cfg);
    if (!disp) { ESP_LOGE(TAG, "lvgl_port_add_disp_dsi failed"); return ESP_FAIL; }

    if (!lvgl_port_lock(1000)) { ESP_LOGE(TAG, "lvgl lock timeout"); return ESP_FAIL; }
    build_ui();
    refresh_cb(NULL);                       // paint once immediately
    lv_timer_create(refresh_cb, 1000, NULL);
    lvgl_port_unlock();

    ESP_LOGI(TAG, "status UI up");
    return ESP_OK;
}
