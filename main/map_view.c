// map_view.c — see map_view.h.

#include "map_view.h"
#include "display.h"
#include "leveler.h"
#include "fieldmap.h"
#include "status_screen.h"   // status_screen_show_map (Work button)

#include <math.h>
#include <string.h>
#include <stdio.h>

#include "esp_heap_caps.h"
#include "esp_log.h"

static const char *TAG = "map";

// Off-screen canvas geometry (fits the 720-wide panel with margins; the volume
// readout sits above it and the Work/Vol buttons below).
#define MAP_W   680
#define MAP_H   900
#define MARGIN  16
#define DEADBAND_M 0.03   // matches LEVELER_DEADBAND_M (ON GRADE band)

static lv_obj_t *s_screen, *s_canvas, *s_vollabel;
static uint16_t *s_buf;

// ── direct RGB565 buffer drawing ─────────────────────────────────────────────
static uint16_t rgb565(uint32_t c)
{
    uint16_t r = (c >> 16) & 0xFF, g = (c >> 8) & 0xFF, b = c & 0xFF;
    return (uint16_t)(((r >> 3) << 11) | ((g >> 2) << 5) | (b >> 3));
}

static inline void put_px(int x, int y, uint16_t c)
{
    if ((unsigned)x < MAP_W && (unsigned)y < MAP_H) s_buf[y * MAP_W + x] = c;
}

static void fill_rect(int x0, int y0, int x1, int y1, uint16_t c)
{
    if (x0 > x1) { int t = x0; x0 = x1; x1 = t; }
    if (y0 > y1) { int t = y0; y0 = y1; y1 = t; }
    if (x0 < 0) x0 = 0;
    if (y0 < 0) y0 = 0;
    if (x1 >= MAP_W) x1 = MAP_W - 1;
    if (y1 >= MAP_H) y1 = MAP_H - 1;
    for (int y = y0; y <= y1; y++)
        for (int x = x0; x <= x1; x++) s_buf[y * MAP_W + x] = c;
}

// Bresenham line, 2 px thick.
static void draw_line(int x0, int y0, int x1, int y1, uint16_t c)
{
    int dx = abs(x1 - x0), sx = x0 < x1 ? 1 : -1;
    int dy = -abs(y1 - y0), sy = y0 < y1 ? 1 : -1;
    int err = dx + dy;
    for (;;) {
        put_px(x0, y0, c); put_px(x0 + 1, y0, c); put_px(x0, y0 + 1, c);
        if (x0 == x1 && y0 == y1) break;
        int e2 = 2 * err;
        if (e2 >= dy) { err += dy; x0 += sx; }
        if (e2 <= dx) { err += dx; y0 += sy; }
    }
}

static void draw_disc(int cx, int cy, int r, uint16_t c)
{
    for (int dy = -r; dy <= r; dy++)
        for (int dx = -r; dx <= r; dx++)
            if (dx * dx + dy * dy <= r * r) put_px(cx + dx, cy + dy, c);
}

// Cut/fill colour by signed deviation (ground − plane), brighter with depth.
static uint16_t heat(double dev)
{
    if (dev > DEADBAND_M) {          // ground high → CUT (orange)
        double t = fmin(dev / 0.30, 1.0);
        return rgb565((0xC8 + (int)(0x37 * t)) << 16 | (0x60 - (int)(0x30 * t)) << 8 | 0x28);
    } else if (dev < -DEADBAND_M) {  // ground low → FILL (blue)
        double t = fmin(-dev / 0.30, 1.0);
        return rgb565(0x2A << 16 | (0x78 + (int)(0x30 * t)) << 8 | (0xB4 + (int)(0x40 * t)));
    }
    return rgb565(0x2E9E4A);         // ON GRADE (green)
}

// Bottom nav/action buttons.
static void on_work(lv_event_t *e) { (void)e; status_screen_show_map(false); }
static void on_vol(lv_event_t *e)
{
    (void)e;
    leveler_compute_volumes();   // recompute (float MLS — quick)
    map_view_update();
}

static void map_button(lv_obj_t *parent, const char *text, lv_event_cb_t cb)
{
    lv_obj_t *btn = lv_button_create(parent);
    lv_obj_set_flex_grow(btn, 1);
    lv_obj_set_height(btn, 96);
    lv_obj_set_style_bg_color(btn, lv_color_hex(0x18406A), 0);
    lv_obj_set_style_radius(btn, 10, 0);
    lv_obj_add_event_cb(btn, cb, LV_EVENT_CLICKED, NULL);
    lv_obj_t *l = lv_label_create(btn);
    lv_obj_set_style_text_font(l, &lv_font_montserrat_28, 0);
    lv_label_set_text(l, text);
    lv_obj_center(l);
}

lv_obj_t *map_view_build(void)
{
    s_screen = lv_obj_create(NULL);
    lv_obj_set_style_bg_color(s_screen, lv_color_hex(0x081420), 0);
    lv_obj_set_style_pad_all(s_screen, 0, 0);
    lv_obj_remove_flag(s_screen, LV_OBJ_FLAG_SCROLLABLE);

    s_vollabel = lv_label_create(s_screen);
    lv_obj_set_style_text_font(s_vollabel, &lv_font_montserrat_28, 0);
    lv_obj_set_style_text_color(s_vollabel, lv_color_hex(0xFFFFFF), 0);
    lv_obj_align(s_vollabel, LV_ALIGN_TOP_MID, 0, 16);
    lv_label_set_text(s_vollabel, "FIELD MAP");

    s_buf = heap_caps_malloc((size_t)MAP_W * MAP_H * sizeof(uint16_t), MALLOC_CAP_SPIRAM);
    if (!s_buf) {
        ESP_LOGE(TAG, "canvas buffer alloc failed (%d B)", MAP_W * MAP_H * 2);
        return s_screen;
    }
    s_canvas = lv_canvas_create(s_screen);
    lv_canvas_set_buffer(s_canvas, s_buf, MAP_W, MAP_H, LV_COLOR_FORMAT_RGB565);
    lv_obj_align(s_canvas, LV_ALIGN_TOP_MID, 0, 64);

    // Bottom button row: back to the work screen + recompute volumes.
    lv_obj_t *row = lv_obj_create(s_screen);
    lv_obj_remove_style_all(row);
    lv_obj_set_width(row, LV_PCT(94));
    lv_obj_set_height(row, LV_SIZE_CONTENT);
    lv_obj_align(row, LV_ALIGN_BOTTOM_MID, 0, -20);
    lv_obj_set_flex_flow(row, LV_FLEX_FLOW_ROW);
    lv_obj_set_flex_align(row, LV_FLEX_ALIGN_SPACE_BETWEEN, LV_FLEX_ALIGN_CENTER,
                          LV_FLEX_ALIGN_CENTER);
    lv_obj_set_style_pad_column(row, 16, 0);
    map_button(row, "< Work", on_work);
    map_button(row, "Vol",    on_vol);

    uint16_t bg = rgb565(0x0A1A2A);
    for (int i = 0; i < MAP_W * MAP_H; i++) s_buf[i] = bg;
    return s_screen;
}

void map_view_update(void)
{
    if (!s_buf) return;

    uint16_t bg = rgb565(0x0A1A2A);
    for (int i = 0; i < MAP_W * MAP_H; i++) s_buf[i] = bg;

    double emin, emax, nmin, nmax;
    if (!fieldmap_bbox(&emin, &emax, &nmin, &nmax) || fieldmap_boundary_count() < 3) {
        lv_label_set_text(s_vollabel, "no field - record perim");
        lv_obj_invalidate(s_canvas);
        return;
    }

    double espan = emax - emin, nspan = nmax - nmin;
    if (espan < 1e-6) espan = 1.0;
    if (nspan < 1e-6) nspan = 1.0;
    double sc = fmin((MAP_W - 2.0 * MARGIN) / espan, (MAP_H - 2.0 * MARGIN) / nspan);
    double ox = MARGIN + ((MAP_W - 2.0 * MARGIN) - espan * sc) / 2.0;
    double oy = MARGIN + ((MAP_H - 2.0 * MARGIN) - nspan * sc) / 2.0;
    #define PX(e) ((int)lround(ox + ((e) - emin) * sc))
    #define PY(n) ((int)lround(oy + (nmax - (n)) * sc))   // North up

    double a = 0, b = 0, c = 0;
    bool hasplane = leveler_get_plane(&a, &b, &c);

    // Heatmap: ~45 cells across the larger dimension (each cell is one MLS eval).
    double cell = fmax(espan, nspan) / 45.0;
    if (cell < 0.3) cell = 0.3;
    int cellpx = (int)ceil(cell * sc) + 1;
    for (double n = nmin + cell * 0.5; n < nmax; n += cell) {
        for (double e = emin + cell * 0.5; e < emax; e += cell) {
            if (!fieldmap_inside(e, n)) continue;
            uint16_t col;
            if (hasplane) {
                double dev = fieldmap_surface_at(e, n) - (a * e + b * n + c);
                col = heat(dev);
            } else {
                col = rgb565(0x445566);
            }
            int x = PX(e), y = PY(n);
            fill_rect(x - cellpx / 2, y - cellpx / 2, x + cellpx / 2, y + cellpx / 2, col);
        }
    }

    // Boundary polygon.
    uint16_t bc = rgb565(0xFFE066);
    uint32_t nb = fieldmap_boundary_count();
    for (uint32_t i = 0; i < nb; i++) {
        double e0, n0, e1, n1;
        fieldmap_boundary_get(i, &e0, &n0);
        fieldmap_boundary_get((i + 1) % nb, &e1, &n1);
        draw_line(PX(e0), PY(n0), PX(e1), PY(n1), bc);
    }

    // Live tractor position.
    double ce, cn;
    if (leveler_current_en(&ce, &cn))
        draw_disc(PX(ce), PY(cn), 9, rgb565(0xFF33CC));

    lv_obj_invalidate(s_canvas);

    // Volume readout — shows the LAST computed volumes (leveler_compute_volumes()
    // is a heavy grid pass, run once on screen entry / the vol button, not every
    // repaint). Falls back to the live area + point count.
    leveler_status_t st;
    leveler_get(&st);
    char buf[112];
    if (st.vol_valid)
        snprintf(buf, sizeof(buf), "cut %.0f  fill %.0f  net %+.0f m3   %.0f m2",
                 st.cut_m3, st.fill_m3, st.net_m3, st.area_m2);
    else
        snprintf(buf, sizeof(buf), "area %.0f m2   pts %lu", st.area_m2,
                 (unsigned long)st.survey_points);
    lv_label_set_text(s_vollabel, buf);

    #undef PX
    #undef PY
}
