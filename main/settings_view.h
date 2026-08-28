// settings_view.h — COM1 NMEA output settings screen (LVGL).
//
// Lets the operator pick which NMEA messages (GGA/RMC/VTG/GSA/ZDA/GSV) and at
// what rate go out COM1 (the RS232 machine-control feed), Apply to persist +
// send to the receiver now, and it re-applies on every boot. See mosaic_config.c
// for the config model + provisioning.
#pragma once

#include "lvgl.h"

// Build the settings screen (call once, under the LVGL lock). Returns the screen.
lv_obj_t *settings_view_build(void);

// Reload the working copy from the saved config (call when the screen is shown).
void settings_view_refresh(void);
