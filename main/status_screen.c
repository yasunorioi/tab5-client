// status_screen.c — see status_screen.h.
//
// The leveler work screen: a big cut/fill readout + a vertical light-bar guide
// (how far above/below the target plane) + on-panel survey buttons, driven by
// touch. Fed by leveler.c (survey/plane/delta) and gnss_state (fix quality).

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
#include "leveler.h"
#include "map_view.h"
#include "usb_cdc_source.h"
#include "touch.h"

static const char *TAG = "status_ui";

#define C_BG      0x081420
#define C_TITLE   0xFFFFFF
#define C_OK      0x33DD66
#define C_WARN    0xFFCC33
#define C_BAD     0xFF5555
#define C_IDLE    0x8899AA
#define C_CUT     0xFF7744   // remove soil (ground above target)
#define C_FILL    0x44AAFF   // add soil (ground below target)
#define C_TRACK   0x10233A   // light-bar background track

// Light-bar full-scale: ±BAR_CM_FS centimetres of deviation from grade.
#define BAR_CM_FS 30

static lv_obj_t *s_status, *s_cutfill, *s_bar, *s_mode;
static lv_indev_t *s_indev;
static lv_obj_t *s_work_screen, *s_map_screen;

// ── LVGL touch input device ──────────────────────────────────────────────────
// Reads touch.c's debounced cache (no I2C in the LVGL task). Raw coords map 1:1
// to the 720x1280 panel (calibrated: no swap/flip).
static void touch_read_cb(lv_indev_t *indev, lv_indev_data_t *data)
{
    (void)indev;
    int x = 0, y = 0;
    if (touch_get_point(&x, &y)) {
        if (x < 0) x = 0; else if (x >= TAB5_LCD_H_RES) x = TAB5_LCD_H_RES - 1;
        if (y < 0) y = 0; else if (y >= TAB5_LCD_V_RES) y = TAB5_LCD_V_RES - 1;
        data->point.x = x;
        data->point.y = y;
        data->state = LV_INDEV_STATE_PRESSED;
    } else {
        data->state = LV_INDEV_STATE_RELEASED;
    }
}

// ── survey/plane button handlers (run in the LVGL task) ──────────────────────
static void on_flat(lv_event_t *e)  { (void)e; leveler_set_flat_here(); }
static void on_add(lv_event_t *e)   { (void)e; leveler_survey_add_current(); }
static void on_fit(lv_event_t *e)   { (void)e; leveler_fit_balance(); }
static void on_clear(lv_event_t *e) { (void)e; leveler_survey_clear(); }

static void add_button(lv_obj_t *row, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(row);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, 110);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x18406A), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

static void build_ui(void)
{
    lv_obj_t *scr = lv_screen_active();
    lv_obj_set_style_bg_color(scr, lv_color_hex(C_BG), 0);
    lv_obj_set_style_pad_all(scr, 22, 0);
    lv_obj_set_flex_flow(scr, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(scr, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);

    // Compact fix/quality strip.
    s_status = lv_label_create(scr);
    lv_obj_set_style_text_font(s_status, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_status, lv_color_hex(C_IDLE), 0);
    lv_obj_set_style_pad_bottom(s_status, 10, 0);
    lv_label_set_text(s_status, "starting...");

    // Headline cut/fill number — the value watched while working.
    s_cutfill = lv_label_create(scr);
    lv_obj_set_style_text_font(s_cutfill, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(s_cutfill, lv_color_hex(C_IDLE), 0);
    lv_obj_set_style_pad_bottom(s_cutfill, 8, 0);
    lv_label_set_text(s_cutfill, "-- no plane");

    // Vertical light-bar: deviation from grade, symmetrical about centre.
    s_bar = lv_bar_create(scr);
    lv_obj_set_size(s_bar, 150, 560);
    lv_bar_set_range(s_bar, -BAR_CM_FS, BAR_CM_FS);
    lv_bar_set_mode(s_bar, LV_BAR_MODE_SYMMETRICAL);
    lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_TRACK), LV_PART_MAIN);
    lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_OK), LV_PART_INDICATOR);
    lv_obj_set_style_radius(s_bar, 8, LV_PART_MAIN);
    lv_obj_set_style_radius(s_bar, 8, LV_PART_INDICATOR);
    lv_obj_set_style_pad_bottom(s_bar, 12, 0);

    // Survey / plane state line.
    s_mode = lv_label_create(scr);
    lv_obj_set_style_text_font(s_mode, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_mode, lv_color_hex(C_IDLE), 0);
    lv_obj_set_style_pad_bottom(s_mode, 16, 0);
    lv_label_set_text(s_mode, "survey 0 pts");

    // Button row: Flat / Survey+ / Fit / Clear.
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 12, 0);
    add_button(row, "Flat",    on_flat);
    add_button(row, "Survey+", on_add);
    add_button(row, "Fit",     on_fit);
    add_button(row, "Clear",   on_clear);
}

static void refresh_cb(lv_timer_t *t)
{
    (void)t;
    char buf[96];

    leveler_record_tick();   // distance-gated auto-sampler (no-op unless recording)

    // When the map screen is up, repaint it (throttled — the heatmap MLS pass is
    // heavier than the work-screen labels) and skip the work-widget updates.
    if (s_map_screen && lv_screen_active() == s_map_screen) {
        static int mtick;
        if (++mtick >= 4) { mtick = 0; map_view_update(); }   // ~1 Hz at a 250 ms timer
        return;
    }

    gnss_snapshot_t g;
    gnss_state_snapshot(&g);
    leveler_status_t lv;
    leveler_get(&lv);

    int64_t now = esp_timer_get_time();
    int64_t age_ms = g.last_block_us ? (now - g.last_block_us) / 1000 : -1;
    bool fresh = (age_ms >= 0 && age_ms < 3000);

    // Fix / quality strip.
    if (!fresh) {
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_BAD), 0);
        lv_label_set_text(s_status, "no SBF");
    } else if (g.pvt_valid) {
        uint32_t col = (g.pvt.mode_type >= 3) ? C_OK : C_WARN;
        if (!isnan(g.pvt.v_accuracy_m))
            snprintf(buf, sizeof(buf), "%s  sv%u  +/-%.0fcm",
                     sbf_pvt_mode_str(g.pvt.mode_type), g.pvt.nr_sv,
                     g.pvt.v_accuracy_m * 100.0);
        else
            snprintf(buf, sizeof(buf), "%s  sv%u", sbf_pvt_mode_str(g.pvt.mode_type),
                     g.pvt.nr_sv);
        lv_obj_set_style_text_color(s_status, lv_color_hex(col), 0);
        lv_label_set_text(s_status, buf);
    } else {
        lv_obj_set_style_text_color(s_status, lv_color_hex(C_WARN), 0);
        lv_label_set_text(s_status, "waiting for fix...");
    }

    // Headline cut/fill + light-bar.
    if (lv.have_delta) {
        double cm = lv.delta_m * 100.0;
        uint32_t cc = lv.state > 0 ? C_CUT : lv.state < 0 ? C_FILL : C_OK;
        if (lv.state > 0)      snprintf(buf, sizeof(buf), "CUT  %.0f cm", cm);
        else if (lv.state < 0) snprintf(buf, sizeof(buf), "FILL %.0f cm", -cm);
        else                   snprintf(buf, sizeof(buf), "ON GRADE");
        lv_obj_set_style_text_color(s_cutfill, lv_color_hex(cc), 0);
        lv_label_set_text(s_cutfill, buf);

        int v = (int)lround(cm);
        if (v >  BAR_CM_FS) v =  BAR_CM_FS;
        if (v < -BAR_CM_FS) v = -BAR_CM_FS;
        lv_bar_set_value(s_bar, v, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(cc), LV_PART_INDICATOR);
    } else {
        lv_obj_set_style_text_color(s_cutfill, lv_color_hex(C_IDLE), 0);
        lv_label_set_text(s_cutfill, lv.mode != LEVELER_MODE_NONE ? "-- no fix"
                                                                  : "-- no plane");
        lv_bar_set_value(s_bar, 0, LV_ANIM_OFF);
        lv_obj_set_style_bg_color(s_bar, lv_color_hex(C_TRACK), LV_PART_INDICATOR);
    }

    // Survey / plane state.
    const char *mode = lv.mode == LEVELER_MODE_BALANCE ? "balance"
                     : lv.mode == LEVELER_MODE_FLAT    ? "flat" : "no plane";
    if (g.pvt_valid && !isnan(g.pvt.height_m))
        snprintf(buf, sizeof(buf), "%s  %lu pts  h%.2fm", mode,
                 (unsigned long)lv.survey_points, g.pvt.height_m);
    else
        snprintf(buf, sizeof(buf), "%s  %lu pts", mode,
                 (unsigned long)lv.survey_points);
    lv_label_set_text(s_mode, buf);
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

    // Register the touch input device (only if the controller is present).
    if (touch_present()) {
        s_indev = lv_indev_create();
        lv_indev_set_type(s_indev, LV_INDEV_TYPE_POINTER);
        lv_indev_set_read_cb(s_indev, touch_read_cb);
        lv_indev_set_display(s_indev, disp);
    } else {
        ESP_LOGW(TAG, "no touch controller — buttons will be inert");
    }

    build_ui();
    s_work_screen = lv_screen_active();     // build_ui() painted onto the default screen
    s_map_screen  = map_view_build();       // second screen (plan-view field map)
    refresh_cb(NULL);                       // paint once immediately
    lv_timer_create(refresh_cb, 250, NULL); // 4 Hz — responsive cut/fill
    lvgl_port_unlock();

    ESP_LOGI(TAG, "leveler UI up");
    return ESP_OK;
}

void status_screen_show_map(bool show)
{
    if (!s_map_screen || !s_work_screen) return;
    if (show) leveler_compute_volumes();    // one-shot heavy pass before the lock
    if (!lvgl_port_lock(500)) return;
    lv_screen_load(show ? s_map_screen : s_work_screen);
    if (show) map_view_update();            // paint immediately on switch
    lvgl_port_unlock();
}
