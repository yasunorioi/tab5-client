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
// Also enables GGA+GSV NMEA on USB2 for the on-panel GNSS view.
//
// Applied to the receiver's CURRENT (RAM) config only — NOT saved to its boot
// config. The box re-provisions on every power-up, so it stays the source of
// truth and the Mosaic's own saved state is left untouched (no flash wear).
//
// Requires a CDC interface already open (usb_cdc_send_command). Returns ESP_OK
// if the receiver acknowledged the command ($R:, not $R?).
esp_err_t mosaic_provision(void);
