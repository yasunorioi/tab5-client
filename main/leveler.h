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

typedef struct {
    bool           has_origin;
    leveler_mode_t mode;
    uint32_t       survey_points;

    bool   fix_usable;    // current PVT has a valid lat/lon/height
    bool   have_delta;    // delta_m below is valid (fix usable AND a plane is set)
    double delta_m;       // current height − target height; >0 CUT, <0 FILL
    int    state;         // cutfill_classify(delta): -1 FILL, 0 ON GRADE, +1 CUT
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
