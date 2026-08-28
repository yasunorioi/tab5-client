// fieldmap.h — field boundary + survey cloud → cut/fill volumes.
//
// Pure C (stdint/stdbool/math/stdlib only) — NO ESP-IDF, so it builds on the host
// for tools/fieldmap_selftest.c and on the esp32p4 target unchanged.
//
// The spec (docs/design.md): drive the field perimeter to trace the boundary and
// collect a survey point cloud, then auto-compute the earthwork-balance plane and
// the cut/fill VOLUMES (not just the per-point delta that leveler.c already does).
//
// This module works entirely in the local ENU frame (metres East/North, height
// relative to the survey datum h0) — the caller projects lat/lon→E,N and h→h-h0
// via cutfill.c, so fieldmap stays a pure geometry/volume calculator.
//
//   boundary  : ordered polygon vertices traced by driving the perimeter.
//   points    : the survey cloud (perimeter + any interior passes).
//   compute   : grid the polygon, IDW-interpolate the surface per cell, integrate
//               (surface − plane) into cut (surface above plane) and fill (below).
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    double   area_m2;   // boundary polygon area (shoelace)
    double   cut_m3;    // volume where the ground is ABOVE the plane (remove)
    double   fill_m3;   // volume where the ground is BELOW the plane (add)
    double   net_m3;    // cut − fill (≈0 for a well-balanced plane)
    double   cut_mean_m;   // mean cut depth over the cut area
    double   fill_mean_m;  // mean fill depth over the fill area
    uint32_t cells;     // grid cells integrated (inside the boundary)
    bool     valid;
} fieldmap_result_t;

// Allocate the boundary/point buffers (idempotent). Call once before use.
bool fieldmap_init(void);

// Clear the boundary and the survey cloud (keeps the buffers allocated).
void fieldmap_reset(void);

// Append an ordered boundary vertex (traced while driving the perimeter).
void fieldmap_boundary_add(double east_m, double north_m);
uint32_t fieldmap_boundary_count(void);

// Append a survey point (perimeter + interior). z is height relative to the datum.
void fieldmap_point_add(double east_m, double north_m, double z);
uint32_t fieldmap_point_count(void);

// Boundary polygon area in m² (shoelace, absolute). 0 with fewer than 3 vertices.
double fieldmap_area(void);

// Compute cut/fill volumes against the target plane z = a·E + b·N + c (c relative
// to the same datum as the points' z). cell_m is the grid resolution in metres
// (e.g. 1.0). Returns false (out->valid=false) if there is no usable boundary
// (<3 vertices) or no survey points. To bound compute time the grid is capped;
// cell_m is enlarged automatically for very large fields.
bool fieldmap_compute(double a, double b, double c, double cell_m,
                      fieldmap_result_t *out);
