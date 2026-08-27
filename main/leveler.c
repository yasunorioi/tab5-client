// leveler.c — see leveler.h.

#include "leveler.h"
#include "cutfill.h"
#include "gnss_state.h"

#include <math.h>
#include <string.h>

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"

#define DEG2RAD (M_PI / 180.0)

static cutfill_geo_t     s_geo;      // local tangent-plane origin (pinned once)
static cutfill_survey_t  s_survey;   // least-squares accumulators
static cutfill_plane_t   s_plane;    // active target plane
static leveler_mode_t    s_mode;
static uint32_t          s_points;
static SemaphoreHandle_t s_lock;

void leveler_init(void)
{
    memset(&s_geo, 0, sizeof(s_geo));
    cutfill_survey_reset(&s_survey);
    memset(&s_plane, 0, sizeof(s_plane));
    s_mode = LEVELER_MODE_NONE;
    s_points = 0;
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

bool leveler_survey_add_current(void)
{
    double lat, lon, h;
    if (!current_fix(&lat, &lon, &h)) return false;

    xSemaphoreTake(s_lock, portMAX_DELAY);
    if (!s_geo.set) {
        cutfill_geo_init(&s_geo, lat, lon, h);   // first point pins the origin
    }
    cutfill_survey_add_ll(&s_survey, &s_geo, lat, lon, h);
    s_points++;
    xSemaphoreGive(s_lock);
    return true;
}

void leveler_survey_clear(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    cutfill_survey_reset(&s_survey);
    memset(&s_plane, 0, sizeof(s_plane));
    s_mode = LEVELER_MODE_NONE;
    s_points = 0;
    xSemaphoreGive(s_lock);
}

bool leveler_fit_balance(void)
{
    xSemaphoreTake(s_lock, portMAX_DELAY);
    bool ok = cutfill_fit_balance(&s_survey, &s_plane);
    if (ok) s_mode = LEVELER_MODE_BALANCE;
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
    xSemaphoreGive(s_lock);
    return true;
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
    out->survey_points = s_points;
    out->fix_usable    = fix;
    if (fix && s_plane.valid) {
        out->delta_m    = cutfill_delta(&s_geo, &s_plane, lat, lon, h);
        out->have_delta = true;
        out->state      = cutfill_classify(out->delta_m, LEVELER_DEADBAND_M);
    }
    xSemaphoreGive(s_lock);
}
