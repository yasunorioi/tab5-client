// settings_view.h — COM1/COM2 NMEA output settings screen (LVGL).
//
// Lets the operator pick a COM port (COM1/COM2, the two RS232 machine-control
// feeds), toggle its output on/off, and pick which NMEA messages (GGA/RMC/VTG/
// GSA/ZDA/GSV) and at what rate it emits. Apply persists both ports + sends to the
// receiver now, and it re-applies on every boot. See mosaic_config.c for the
// config model + provisioning.
#pragma once

#include "lvgl.h"

// Build the settings screen (call once, under the LVGL lock). Returns the screen.
lv_obj_t *settings_view_build(void);

// Reload the working copy from the saved config (call when the screen is shown).
void settings_view_refresh(void);
