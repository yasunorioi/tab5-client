// leveler.h — the cut/fill application layer: ties gnss_state (live PVT) to
// cutfill.c (survey + balance plane) and exposes a single status the panel and
// console read. This is where cutfill.c is actually driven from real fixes.
//
// Workflow (operator, via console or on-panel buttons later):
//   1. Drive the field; `survey add` at points (or a future auto-sampler) —
//      the first point pins the local origin.
//   2. `survey fit` → least-squares balance plane (cut ≈ fill), or `flat` to
//      level to the current height, everywhere.
//   3. Live: leveler_get() returns delta = current height − target-plane height.
//      >0 CUT (grind down), <0 FILL (build up).
//
// Thread-safe: the panel/console tasks call leveler_get(); the console task
// mutates via survey/fit calls. All guarded by one mutex.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef enum {
    LEVELER_MODE_NONE = 0,   // no target plane set yet
    LEVELER_MODE_BALANCE,    // least-squares balance plane from the survey
    LEVELER_MODE_FLAT,       // flat plane at a captured height
} leveler_mode_t;

// Continuous-recording mode: while on, leveler_record_tick() auto-samples the fix
// as the tractor drives (distance-gated) into the survey cloud, and — in PERIMETER
// mode — also into the boundary polygon traced around the field edge.
typedef enum {
    LEVELER_REC_OFF = 0,
    LEVELER_REC_PERIMETER,   // trace the field boundary + collect cloud
    LEVELER_REC_SURVEY,      // collect interior cloud only (no boundary)
} leveler_rec_t;

typedef struct {
    bool           has_origin;
    leveler_mode_t mode;
    leveler_rec_t  rec;
    uint32_t       survey_points;   // survey cloud size
    uint32_t       boundary_pts;    // boundary polygon vertices
    double         area_m2;         // live boundary area (shoelace)

    bool   fix_usable;    // current PVT has a valid lat/lon/height
    bool   have_delta;    // delta_m below is valid (fix usable AND a plane is set)
    double delta_m;       // current height − target height; >0 CUT, <0 FILL
    int    state;         // cutfill_classify(delta): -1 FILL, 0 ON GRADE, +1 CUT

    // Earthwork volumes against the active plane (leveler_compute_volumes()).
    bool   vol_valid;
    double cut_m3, fill_m3, net_m3;
} leveler_status_t;

// Dead-band (metres) for the ON-GRADE classification. RTK vertical is ~±2 cm, so
// call anything within ±3 cm "on grade" rather than chattering CUT/FILL.
#define LEVELER_DEADBAND_M 0.03

void leveler_init(void);

// Add the current fix as a survey point (first point pins the origin). Returns
// false if there is no usable fix right now.
bool leveler_survey_add_current(void);

// Discard the survey and any fitted plane (origin is kept until re-added).
void leveler_survey_clear(void);

// Fit the least-squares balance plane from the collected survey. Returns false
// with fewer than 3 usable points.
bool leveler_fit_balance(void);

// Set a flat target plane at the current fix's height (levels the whole field to
// where the blade is now). Returns false if there is no usable fix.
bool leveler_set_flat_here(void);

// Snapshot the current leveling state (computes the live delta from the latest
// PVT against the active plane).
void leveler_get(leveler_status_t *out);

// Start/stop continuous recording. In PERIMETER mode each auto-sampled fix also
// extends the boundary polygon; in SURVEY mode only the cloud grows.
void leveler_record_set(leveler_rec_t mode);

// Distance-gated auto-sampler — call periodically (e.g. from the 4 Hz UI timer).
// Adds the current fix to the cloud/boundary once the tractor has moved far
// enough since the last sample. No-op when recording is off or there is no fix.
void leveler_record_tick(void);

// Compute cut/fill volumes of the survey cloud against the active plane, within
// the boundary polygon. Returns false without a plane, a ≥3-vertex boundary, and
// survey points. Result is cached and surfaced via leveler_get().
bool leveler_compute_volumes(void);

// ── plan-view map support (map_view.c) ───────────────────────────────────────
// Active plane z = a·E + b·N + c (c relative to the datum). False if none set.
bool leveler_get_plane(double *a, double *b, double *c);
// Current fix projected into the local E/N frame (for the position marker).
// False if there is no fix or no origin yet.
bool leveler_current_en(double *east_m, double *north_m);
// Load a synthetic tilted+undulating field (boundary + cloud + balance plane) so
// the map can be checked on the bench without driving. TEST AID.
void leveler_demo_field(void);
