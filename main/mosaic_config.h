// mosaic_config.h — self-provisioning of the Septentrio mosaic-go rover output.
//
// The product goal is flash-and-go: plug the mosaic-go into the box and it just
// works, without the operator having to configure the receiver. On boot the box
// pushes its canonical config to the Mosaic over the open CDC command channel:
// the dual-antenna attitude offset + the SBF block set the client decodes.

#pragma once

#include <stdbool.h>
#include <stdint.h>
#include "esp_err.h"

// Provision the Mosaic for the client on the USB port the box reads (USB1):
//   1. setAttitudeOffset, 90, 0 — left/right dual-antenna mounting correction
//      (so AttEuler reads as pitch/roll, not heading-only).
//   2. setSBFOutput, Stream1, USB1, PVTGeodetic+AttEuler+ReceiverStatus+DOP,
//      msec100 — the 10 Hz block set sbf_parser.c decodes.
// Also enables GGA+GSV NMEA on USB2 (on-panel GNSS view) and the operator's saved
// per-port NMEA on COM1/COM2 at 38400 baud (external RS232 machine controller /
// auto-steer; COM1 enabled by default, COM2 off).
//
// Applied to the receiver's CURRENT (RAM) config only — NOT saved to its boot
// config. The box re-provisions on every power-up, so it stays the source of
// truth and the Mosaic's own saved state is left untouched (no flash wear).
//
// Requires a CDC interface already open (usb_cdc_send_command). Returns ESP_OK
// if the receiver acknowledged the command ($R:, not $R?).
esp_err_t mosaic_provision(void);

// ── COMx RS232 NMEA output config (operator-set, NVS-persisted) ───────────────
// The external machine-control / auto-steer feeds. The mosaic-go G5 exposes two
// serial ports (COM1, COM2), each an independent RS232 NMEA output with its own
// on/off, message set and rate. Configurable from the panel (settings_view.c) and
// re-applied on every boot by mosaic_provision(). Defaults: COM1 = GGA @ 10 Hz
// enabled, COM2 = GGA @ 10 Hz disabled.
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
    NMEA_RATE_OFF,        // sentinel: not a selectable rate — the port's `enabled`
                          // master toggle governs on/off (see settings_view.c).
    NMEA_RATE_COUNT,
} nmea_rate_t;
// Number of user-selectable rates (the UI's rate row); excludes the OFF sentinel.
#define NMEA_RATE_SELECTABLE NMEA_RATE_OFF

// Serial baud for a COM port's RS232 feed — the external machine dictates this,
// so it is per-port and operator-selectable. Default 38400 (index NMEA_BAUD_DEF).
typedef enum {
    NMEA_BAUD_4800 = 0,
    NMEA_BAUD_9600,
    NMEA_BAUD_19200,
    NMEA_BAUD_38400,
    NMEA_BAUD_57600,
    NMEA_BAUD_115200,
    NMEA_BAUD_COUNT,
} nmea_baud_t;
#define NMEA_BAUD_DEF NMEA_BAUD_38400

// The two configurable RS232 output ports on the mosaic-go.
typedef enum {
    MOSAIC_COM1 = 0,
    MOSAIC_COM2,
    MOSAIC_COM_COUNT,
} mosaic_com_t;

// "COM1"/"COM2" for a port index (or "?" if out of range).
const char *mosaic_com_name(mosaic_com_t port);

// Current NMEA config for a COM port (from NVS, or the per-port default). Any
// out-parameter may be NULL.
void mosaic_nmea_cfg_get(mosaic_com_t port, bool *enabled, uint16_t *msg_mask,
                         uint8_t *rate, uint8_t *baud);

// Persist a new NMEA config for a COM port to NVS. Persist-only: the Mosaic ignores
// commands once its SBF stream is up, so the change takes effect on the next
// boot-time provision — i.e. after a power cycle of the box. Always returns ESP_OK.
esp_err_t mosaic_nmea_cfg_apply(mosaic_com_t port, bool enabled,
                                uint16_t msg_mask, uint8_t rate, uint8_t baud);

// Human-readable rate label ("10Hz".."1Hz", "OFF" for the sentinel) for the UI.
const char *mosaic_nmea_rate_str(uint8_t rate);

// Human-readable baud label ("4800".."115200") for the UI.
const char *mosaic_nmea_baud_str(uint8_t baud);
