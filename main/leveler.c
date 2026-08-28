// leveler.c — see leveler.h.

#include "leveler.h"
#include "cutfill.h"
#include "fieldmap.h"
#include "gnss_state.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DEG2RAD (M_PI / 180.0)

// Auto-sample spacing while recording: add a point once the tractor has moved
// this far since the last sample. ~1 m gives a dense-enough cloud without
// flooding the buffers on a slow pass.
#define RECORD_GATE_M 1.0

// Grid resolution for the volume integration (metres).
#define VOL_CELL_M 1.0

static cutfill_geo_t     s_geo;      // local tangent-plane origin (pinned once)
static cutfill_survey_t  s_survey;   // least-squares accumulators (for the plane)
static cutfill_plane_t   s_plane;    // active target plane
static leveler_mode_t    s_mode;
static uint32_t          s_points;
static uint32_t          s_boundary;

static leveler_rec_t     s_rec;      // continuous-recording mode
static bool              s_have_last; // a prior sample exists this recording run
static double            s_last_e, s_last_n;

static fieldmap_result_t s_vol;      // last computed volumes
static bool              s_vol_valid;

static SemaphoreHandle_t s_lock;

void leveler_init(void)
{
    memset(&s_geo, 0, sizeof(s_geo));
    cutfill_survey_reset(&s_survey);
    memset(&s_plane, 0, sizeof(s_plane));
    s_mode = LEVELER_MODE_NONE;
    s_points = 0;
    s_boundary = 0;
    s_rec = LEVELER_REC_OFF;
    s_have_last = false;
    memset(&s_vol, 0, sizeof(s_vol));
    s_vol_valid = false;
    fieldmap_init();
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

// Pull a usable fix (valid lat/lon/height) from gnss_state. Returns false if the
// current PVT is Do-Not-Use (no fix). lat/lon come back in radians.
static bool current_fix(double *lat_rad, double *lon_rad, double *height_m)
{
    gnss_snapshot_t g;
    gnss_state_snapshot(&g);
    if (!g.pvt_valid || isnan(g.pvt.lat_deg) || isnan(g.pvt.lon_deg) ||
        isnan(g.pvt.height_m)) {
        return false;
    }
    *lat_rad  = g.pvt.lat_deg * DEG2RAD;
    *lon_rad  = g.pvt.lon_deg * DEG2RAD;
    *height_m = g.pvt.height_m;
    return true;
}

// Add one fix to the survey cloud (and, if perimeter-recording, the boundary).
// Caller holds s_lock and has a set origin.
static void add_sample(double lat, double lon, double h, bool as_boundary)
{
    double e, n;
    cutfill_project(&s_geo, lat, lon, &e, &n);
    fieldmap_point_add(e, n, h - s_geo.h0);        // for volume interpolation + map
    cutfill_survey_add_en(&s_survey, &s_geo, e, n, h);  // for the balance-plane fit
    s_points++;
    if (as_boundary) {
        fieldmap_boundary_add(e, n);
        s_boundary++;
    }
    s_last_e = e;
    s_last_n = n;
    s_have_last = true;
}

bool leveler_survey_add_current(void)
{
    double lat, lon, h;
    if (!current_fix(&lat, &lon, &h)) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_geo.set) {
        cutfill_geo_init(&s_geo, lat, lon, h);   // first point pins the origin
    }
    add_sample(lat, lon, h, s_rec == LEVELER_REC_PERIMETER);
    xSemaphoreGive(s_lock);
    return true;
}

void leveler_survey_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cutfill_survey_reset(&s_survey);
    fieldmap_reset();
    memset(&s_plane, 0, sizeof(s_plane));
    s_mode = LEVELER_MODE_NONE;
    s_points = 0;
    s_boundary = 0;
    s_rec = LEVELER_REC_OFF;
    s_have_last = false;
    s_vol_valid = false;
    xSemaphoreGive(s_lock);
}

bool leveler_fit_balance(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = cutfill_fit_balance(&s_survey, &s_plane);
    if (ok) s_mode = LEVELER_MODE_BALANCE;
    s_vol_valid = false;   // plane changed; stale volumes no longer apply
    xSemaphoreGive(s_lock);
    return ok;
}

bool leveler_set_flat_here(void)
{
    double lat, lon, h;
    if (!current_fix(&lat, &lon, &h)) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_geo.set) {
        cutfill_geo_init(&s_geo, lat, lon, h);
    }
    cutfill_plane_flat(&s_plane, &s_geo, h);   // level everything to here
    s_mode = LEVELER_MODE_FLAT;
    s_vol_valid = false;
    xSemaphoreGive(s_lock);
    return true;
}

void leveler_record_set(leveler_rec_t mode)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    s_rec = mode;
    s_have_last = false;   // restart the distance gate for a fresh run
    xSemaphoreGive(s_lock);
}

void leveler_record_tick(void)
{
    if (s_rec == LEVELER_REC_OFF) return;
    double lat, lon, h;
    if (!current_fix(&lat, &lon, &h)) return;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (s_rec == LEVELER_REC_OFF) { xSemaphoreGive(s_lock); return; }
    if (!s_geo.set) {
        cutfill_geo_init(&s_geo, lat, lon, h);
    }
    double e, n;
    cutfill_project(&s_geo, lat, lon, &e, &n);
    bool far_enough = true;
    if (s_have_last) {
        double de = e - s_last_e, dn = n - s_last_n;
        far_enough = (de * de + dn * dn) >= (RECORD_GATE_M * RECORD_GATE_M);
    }
    if (far_enough) {
        add_sample(lat, lon, h, s_rec == LEVELER_REC_PERIMETER);
    }
    xSemaphoreGive(s_lock);
}

bool leveler_compute_volumes(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = false;
    if (s_plane.valid) {
        ok = fieldmap_compute(s_plane.a, s_plane.b, s_plane.c, VOL_CELL_M, &s_vol);
    }
    s_vol_valid = ok;
    xSemaphoreGive(s_lock);
    return ok;
}

void leveler_get(leveler_status_t *out)
{
    if (out == NULL) return;
    memset(out, 0, sizeof(*out));

    double lat, lon, h;
    bool fix = current_fix(&lat, &lon, &h);

    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->has_origin    = s_geo.set;
    out->mode          = s_mode;
    out->rec           = s_rec;
    out->survey_points = s_points;
    out->boundary_pts  = s_boundary;
    out->area_m2       = fieldmap_area();
    out->fix_usable    = fix;
    if (fix && s_plane.valid) {
        out->delta_m    = cutfill_delta(&s_geo, &s_plane, lat, lon, h);
        out->have_delta = true;
        out->state      = cutfill_classify(out->delta_m, LEVELER_DEADBAND_M);
    }
    out->vol_valid = s_vol_valid;
    if (s_vol_valid) {
        out->cut_m3  = s_vol.cut_m3;
        out->fill_m3 = s_vol.fill_m3;
        out->net_m3  = s_vol.net_m3;
    }
    xSemaphoreGive(s_lock);
}
