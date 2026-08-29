// settings_view.c — see settings_view.h.

#include "settings_view.h"
#include "mosaic_config.h"
#include "status_screen.h"

#include <stdint.h>
#include "esp_err.h"

#define C_ON   0x2E9E4A   // selected / enabled (green)
#define C_OFF  0x24374A   // unselected (dark)
#define C_NAV  0x18406A   // nav / apply buttons

static const struct { uint16_t bit; const char *name; } MSGS[] = {
    { NMEA_MSG_GGA, "GGA" }, { NMEA_MSG_RMC, "RMC" }, { NMEA_MSG_VTG, "VTG" },
    { NMEA_MSG_GSA, "GSA" }, { NMEA_MSG_ZDA, "ZDA" }, { NMEA_MSG_GSV, "GSV" },
};
#define N_MSGS (sizeof(MSGS) / sizeof(MSGS[0]))

#define N_RATES NMEA_RATE_SELECTABLE   // rate buttons (10/5/2/1 Hz; no OFF)

static lv_obj_t *s_screen;
static lv_obj_t *s_port_btn[MOSAIC_COM_COUNT];
static lv_obj_t *s_onoff_btn;
static lv_obj_t *s_onoff_lbl;
static lv_obj_t *s_msg_btn[N_MSGS];
static lv_obj_t *s_rate_btn[N_RATES];
static lv_obj_t *s_status_lbl;

// Working copies for BOTH ports (edited by the toggles; committed on Apply).
// s_cur is the port currently shown/edited; the port selector switches it.
static uint8_t  s_cur;
static bool     s_en[MOSAIC_COM_COUNT];
static uint16_t s_msgs[MOSAIC_COM_COUNT];
static uint8_t  s_rate[MOSAIC_COM_COUNT];

static void refresh_highlights(void)
{
    for (int p = 0; p < MOSAIC_COM_COUNT; p++)
        lv_obj_set_style_bg_color(s_port_btn[p],
            lv_color_hex(p == s_cur ? C_ON : C_OFF), 0);

    bool on = s_en[s_cur];
    lv_obj_set_style_bg_color(s_onoff_btn, lv_color_hex(on ? C_ON : 0x7A2E2E), 0);
    lv_label_set_text(s_onoff_lbl, on ? "OUTPUT: ON" : "OUTPUT: OFF");

    for (size_t i = 0; i < N_MSGS; i++)
        lv_obj_set_style_bg_color(s_msg_btn[i],
            lv_color_hex((s_msgs[s_cur] & MSGS[i].bit) ? C_ON : C_OFF), 0);
    for (int i = 0; i < N_RATES; i++)
        lv_obj_set_style_bg_color(s_rate_btn[i],
            lv_color_hex(i == s_rate[s_cur] ? C_ON : C_OFF), 0);
}

void settings_view_refresh(void)
{
    for (int p = 0; p < MOSAIC_COM_COUNT; p++) {
        bool en; uint16_t m; uint8_t r;
        mosaic_nmea_cfg_get((mosaic_com_t)p, &en, &m, &r);
        s_en[p] = en; s_msgs[p] = m; s_rate[p] = r;
    }
    if (s_screen) {
        refresh_highlights();
        if (s_status_lbl) lv_label_set_text(s_status_lbl, "");
    }
}

static void on_port(lv_event_t *e)
{
    s_cur = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    refresh_highlights();
}

static void on_onoff(lv_event_t *e)
{
    (void)e;
    s_en[s_cur] = !s_en[s_cur];
    refresh_highlights();
}

static void on_msg(lv_event_t *e)
{
    int i = (int)(intptr_t)lv_event_get_user_data(e);
    s_msgs[s_cur] ^= MSGS[i].bit;
    refresh_highlights();
}

static void on_rate(lv_event_t *e)
{
    s_rate[s_cur] = (uint8_t)(intptr_t)lv_event_get_user_data(e);
    refresh_highlights();
}

// One-shot: clear the Apply confirmation after a few seconds.
static void clear_status_cb(lv_timer_t *t)
{
    (void)t;
    if (s_status_lbl) lv_label_set_text(s_status_lbl, "");
}

static void on_apply(lv_event_t *e)
{
    (void)e;
    // Commit both ports so edits made on either tab are persisted + applied.
    esp_err_t err = ESP_OK;
    for (int p = 0; p < MOSAIC_COM_COUNT; p++) {
        esp_err_t r = mosaic_nmea_cfg_apply((mosaic_com_t)p, s_en[p],
                                            s_msgs[p], s_rate[p]);
        if (r != ESP_OK) err = r;
    }
    lv_label_set_text(s_status_lbl,
                      err == ESP_OK ? "saved + applied to receiver"
                                    : "saved (apply failed - see log)");
    lv_timer_t *t = lv_timer_create(clear_status_cb, 3000, NULL);
    lv_timer_set_repeat_count(t, 1);   // fire once, then auto-delete
}

static void on_back(lv_event_t *e) { (void)e; status_screen_show_map(false); }

static lv_obj_t *mk_btn(lv_obj_t *parent, const char *text, lv_event_cb_t cb,
                        void *user, int h)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, h);
    lv_obj_set_style_bg_color(btn, lv_color_hex(C_OFF), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, user);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
    return btn;
}

static lv_obj_t *mk_row(lv_obj_t *scr)
{
    lv_obj_t *row = lv_obj_create(scr);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(100));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 10, 0);
    lv_obj_set_style_pad_bottom(row, 12, 0);
    return row;
}

static lv_obj_t *mk_label(lv_obj_t *scr, const char *text, uint32_t color)
{
    lv_obj_t *l = lv_label_create(scr);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(l, lv_color_hex(color), 0);
    lv_obj_set_style_pad_bottom(l, 8, 0);
    lv_label_set_text(l, text);
    return l;
}

lv_obj_t *settings_view_build(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x081420), 0);
    lv_obj_set_style_pad_all(s_screen, 24, 0);
    lv_obj_set_flex_flow(s_screen, LV_FLEX_FLOW_COLUMN);
    lv_obj_set_flex_align(s_screen, LV_FLEX_ALIGN_START, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    lv_obj_t *title = lv_label_create(s_screen);
    lv_obj_set_style_text_font(title, &lv_font_montserrat_48, 0);
    lv_obj_set_style_text_color(title, lv_color_hex(0xFFFFFF), 0);
    lv_obj_set_style_pad_bottom(title, 20, 0);
    lv_label_set_text(title, "NMEA OUT (RS232 @ 38400)");

    // Port selector: COM1 / COM2 — switches which port the toggles below edit.
    mk_label(s_screen, "Port", 0x8FB0CD);
    lv_obj_t *pr = mk_row(s_screen);
    for (int p = 0; p < MOSAIC_COM_COUNT; p++)
        s_port_btn[p] = mk_btn(pr, mosaic_com_name((mosaic_com_t)p), on_port,
                               (void *)(intptr_t)p, 90);

    // Master on/off toggle for the selected port (full-width).
    lv_obj_t *tr = mk_row(s_screen);
    s_onoff_btn = mk_btn(tr, "OUTPUT: ON", on_onoff, NULL, 90);
    s_onoff_lbl = lv_obj_get_child(s_onoff_btn, 0);

    mk_label(s_screen, "Messages", 0x8FB0CD);
    // Two rows of three message toggles.
    lv_obj_t *m1 = mk_row(s_screen);
    for (int i = 0; i < 3; i++)
        s_msg_btn[i] = mk_btn(m1, MSGS[i].name, on_msg, (void *)(intptr_t)i, 90);
    lv_obj_t *m2 = mk_row(s_screen);
    for (int i = 3; i < (int)N_MSGS; i++)
        s_msg_btn[i] = mk_btn(m2, MSGS[i].name, on_msg, (void *)(intptr_t)i, 90);

    mk_label(s_screen, "Rate", 0x8FB0CD);
    lv_obj_t *rr = mk_row(s_screen);
    for (int i = 0; i < N_RATES; i++)
        s_rate_btn[i] = mk_btn(rr, mosaic_nmea_rate_str(i), on_rate,
                               (void *)(intptr_t)i, 90);

    s_status_lbl = mk_label(s_screen, "", 0x5FE08A);

    // Bottom: back + apply.
    lv_obj_t *nav = mk_row(s_screen);
    lv_obj_t *back = mk_btn(nav, "< Work", on_back, NULL, 100);
    lv_obj_set_style_bg_color(back, lv_color_hex(C_NAV), 0);
    lv_obj_t *apply = mk_btn(nav, "Apply", on_apply, NULL, 100);
    lv_obj_set_style_bg_color(apply, lv_color_hex(0x1D5A37), 0);

    settings_view_refresh();
    return s_screen;
}
