// map_view.h — plan-view field map screen (LVGL).
//
// A second LVGL screen showing the surveyed field from above: the boundary
// polygon, a cut/fill heatmap (each grid cell coloured by ground-minus-plane),
// the live tractor position, and the total cut/fill volumes. Renders into an
// off-screen RGB565 canvas buffer with direct pixel writes (fast; repainted at
// ~1 Hz, not every UI frame). status_screen.c owns screen switching.
#pragma once

#include "lvgl.h"

// Build the map screen (call once, under the LVGL lock). Returns the screen obj.
lv_obj_t *map_view_build(void);

// Repaint the map from the current fieldmap/leveler state + update the volume
// readout. Call under the LVGL lock (e.g. from the status refresh timer).
void map_view_update(void);
