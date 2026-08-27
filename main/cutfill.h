// cutfill.h — cut/fill (切土/盛土) computation for the land-leveler display.
//
// Pure C (stdint/stdbool/math only) — NO ESP-IDF dependency, so it builds on the
// host for tools/cutfill_selftest.c and on the esp32p4 target unchanged.
//
// Pipeline:
//   1. cutfill_geo_init()  — pin a local tangent-plane origin at the first fix.
//   2. cutfill_project()   — lat/lon → local East/North metres (equirectangular;
//      horizontal scale error over a field is irrelevant to the height result).
//   3. Drive the field, cutfill_survey_add() each PVTGeodetic fix.
//   4. cutfill_fit_balance() — least-squares plane z = a·E + b·N + c through the
//      survey. This IS the earthwork-balance plane: LS makes Σ(z−ẑ)=0, so cut
//      volume ≈ fill volume. (Or cutfill_plane_flat / cutfill_fit_slope.)
//   5. Live: cutfill_delta() = measured height − target-plane height.
//      delta > 0 ⇒ ground too HIGH ⇒ CUT (切土). delta < 0 ⇒ FILL (盛土).
//
// Heights are the ellipsoidal height from PVTGeodetic (sbf_pvtgeodetic_t.height_m).
// No geoid: this is a purely relative comparison within one field. All internal
// height maths is done relative to the origin height h0 for conditioning; the
// public delta is in metres of real elevation difference.
#pragma once

#include <stdbool.h>
#include <stdint.h>

// Local tangent-plane origin. Set once, at the first valid fix (or a benchmark).
typedef struct {
    double lat0_rad;   // origin latitude  (radians)
    double lon0_rad;   // origin longitude (radians)
    double coslat0;    // cos(lat0), cached
    double h0;         // origin ellipsoidal height (m) — the height datum
    bool   set;
} cutfill_geo_t;

// Least-squares accumulators for the balance-plane fit. E/N are metres from the
// origin; z is height relative to h0. Incremental: add points as you drive.
typedef struct {
    double   sE, sN, sZ;        // Σ
    double   sEE, sNN, sEN;     // Σ of products (design matrix)
    double   sEZ, sNZ;          // Σ E·z, Σ N·z
    uint32_t n;
} cutfill_survey_t;

// A target plane: z_target(E,N) = a·E + b·N + c, where c is relative to h0.
typedef struct {
    double a;      // slope along East (m per m, i.e. rise/run)
    double b;      // slope along North
    double c;      // offset at the origin, relative to h0
    bool   valid;
} cutfill_plane_t;

// ── Geo origin / projection ──────────────────────────────────────────────────
void cutfill_geo_init(cutfill_geo_t *g, double lat_rad, double lon_rad, double h0);

// lat/lon (radians) → local East/North metres relative to the origin.
void cutfill_project(const cutfill_geo_t *g, double lat_rad, double lon_rad,
                     double *east_m, double *north_m);

// ── Survey accumulation ──────────────────────────────────────────────────────
void cutfill_survey_reset(cutfill_survey_t *s);

// Add one surveyed point given directly in local metres (E,N) and height h.
// h is the absolute ellipsoidal height; it is stored relative to g->h0.
void cutfill_survey_add_en(cutfill_survey_t *s, const cutfill_geo_t *g,
                           double east_m, double north_m, double h);

// Convenience: project lat/lon then add. Requires the origin to be set.
void cutfill_survey_add_ll(cutfill_survey_t *s, const cutfill_geo_t *g,
                           double lat_rad, double lon_rad, double h);

// ── Plane construction ───────────────────────────────────────────────────────
// Earthwork-balance plane: least-squares fit. Returns false (plane->valid=false)
// with fewer than 3 non-degenerate points; caller should fall back to flat.
bool cutfill_fit_balance(const cutfill_survey_t *s, cutfill_plane_t *plane);

// Flat target at a chosen absolute elevation (a=b=0).
void cutfill_plane_flat(cutfill_plane_t *plane, const cutfill_geo_t *g,
                        double target_height);

// Fixed designed grade (a,b given as rise/run, e.g. 0.001 = 0.1%), balance
// offset chosen so Σ(z−ẑ)=0 over the survey. Returns false if empty.
bool cutfill_fit_slope(const cutfill_survey_t *s, double a, double b,
                       cutfill_plane_t *plane);

// ── Query ────────────────────────────────────────────────────────────────────
// Target ellipsoidal height (absolute) of the plane at local (E,N).
double cutfill_target_height(const cutfill_geo_t *g, const cutfill_plane_t *plane,
                             double east_m, double north_m);

// cut/fill at a live fix: measured height − target height, in metres.
//   > 0  ground above target → CUT  (切土)
//   < 0  ground below target → FILL (盛土)
double cutfill_delta(const cutfill_geo_t *g, const cutfill_plane_t *plane,
                     double lat_rad, double lon_rad, double height);

// Classify a delta with a symmetric dead-band (metres). Returns -1 FILL,
// 0 ON-GRADE, +1 CUT. Label string via cutfill_state_str().
int  cutfill_classify(double delta_m, double deadband_m);
const char *cutfill_state_str(int state);   // "CUT" / "FILL" / "ON GRADE"
