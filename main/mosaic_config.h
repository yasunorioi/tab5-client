// mosaic_config.h — self-provisioning of the Septentrio mosaic-go rover output.
//
// The product goal is flash-and-go: plug the mosaic-go into the box and it just
// works, without the operator having to configure the receiver. On boot the box
// pushes its canonical config to the Mosaic over the open CDC command channel:
// the dual-antenna attitude offset + the SBF block set the client decodes.

#pragma once

#include "esp_err.h"

// Provision the Mosaic for the client on the USB port the box reads (USB1):
//   1. setAttitudeOffset, 90, 0 — left/right dual-antenna mounting correction
//      (so AttEuler reads as pitch/roll, not heading-only).
//   2. setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP,
//      msec100 — the 10 Hz block set sbf_parser.c decodes.
// Also enables GGA+GSV NMEA on USB2 (on-panel GNSS view) and 10 Hz GGA on COM1
// at 38400 baud (external RS232 machine controller / auto-steer).
//
// Applied to the receiver's CURRENT (RAM) config only — NOT saved to its boot
// config. The box re-provisions on every power-up, so it stays the source of
// truth and the Mosaic's own saved state is left untouched (no flash wear).
//
// Requires a CDC interface already open (usb_cdc_send_command). Returns ESP_OK
// if the receiver acknowledged the command ($R:, not $R?).
esp_err_t mosaic_provision(void);

// ── COM1 RS232 NMEA output config (operator-set, NVS-persisted) ───────────────
// The external machine-control / auto-steer feed on COM1. Which messages and at
// what rate is configurable from the panel (settings_view.c) and re-applied on
// every boot by mosaic_provision(). Defaults to GGA @ 10 Hz.
#define NMEA_MSG_GGA 0x01u
#define NMEA_MSG_RMC 0x02u
#define NMEA_MSG_VTG 0x04u
#define NMEA_MSG_GSA 0x08u
#define NMEA_MSG_ZDA 0x10u
#define NMEA_MSG_GSV 0x20u

typedef enum {
    NMEA_RATE_10HZ = 0,   // msec100
    NMEA_RATE_5HZ,        // msec200
    NMEA_RATE_2HZ,        // msec500
    NMEA_RATE_1HZ,        // sec1
    NMEA_RATE_OFF,        // disable the COM1 NMEA stream
    NMEA_RATE_COUNT,
} nmea_rate_t;

// Current COM1 NMEA config (from NVS, or the GGA@10Hz default).
void mosaic_nmea_cfg_get(uint16_t *msg_mask, uint8_t *rate);

// Persist a new COM1 NMEA config to NVS and, if a CDC interface is open, apply it
// to the receiver immediately. Returns the receiver's apply result (ESP_OK if
// no interface is open yet — it will take effect on the next provision).
esp_err_t mosaic_nmea_cfg_apply(uint16_t msg_mask, uint8_t rate);

// Human-readable rate label ("10Hz".."OFF") for the UI.
const char *mosaic_nmea_rate_str(uint8_t rate);
