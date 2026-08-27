// mosaic_config.c — see mosaic_config.h.

#include "mosaic_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"

#include "usb_cdc_source.h"

static const char *TAG = "mosaic_cfg";

// The SBF stream the client reads. On the P3H the streaming CDC interface (itf0)
// carries port USB1 (docs/hardware-findings.md); the command targets USB1 by
// name so it applies regardless of which COM we send it on.
#define SBF_PORT   "USB1"
#define SBF_STREAM "Stream1"

// The block set the client decodes (sbf_parser.c): position + dual-antenna
// attitude + fix quality + receiver health, at 10 Hz.
//   PVTGeodetic   4007  — ellipsoidal height for cut/fill, mode/accuracy
//   AttEuler      5938  — heading/pitch/roll (leveling attitude)
//   DOP           4001  — fix geometry quality
//   ReceiverStatus 4014 — uptime/temperature/rx_error health
#define SBF_MSGS   "PVTGeodetic+AttEuler+ReceiverStatus+DOP"
#define SBF_RATE   "msec100"

// Left/right dual-antenna mounting correction (docs/hardware-findings.md). Must
// be applied before attitude reads as pitch/roll rather than heading-only.
#define ATT_OFFSET "setAttitudeOffset, 90, 0"

// NMEA for the on-panel GNSS view (skyplot + C/N0 bars). GSV carries per-
// satellite azimuth/elevation/SNR; GGA carries the base position. Emitted on a
// SEPARATE port (USB2 = itf4) so the RTCM3 caster stream on USB1 stays pure —
// the box opens itf4 read-only for display (see nmea_source.c). USB2 by name, so
// it applies regardless of which command interface we send it on.
#define NMEA_PORT   "USB2"
#define NMEA_STREAM "Stream1"
#define NMEA_MSGS   "GGA+GSV"
#define NMEA_RATE   "sec1"

// Find `needle` in the first `len` bytes of `hay` (which may contain NUL/binary,
// since a reply captured from a streaming port carries teed RTCM3 bytes).
static bool buf_contains(const char *hay, size_t len, const char *needle)
{
    size_t nlen = strlen(needle);
    if (nlen == 0 || len < nlen) return false;
    for (size_t i = 0; i + nlen <= len; i++) {
        if (memcmp(hay + i, needle, nlen) == 0) return true;
    }
    return false;
}

// Best-effort: enable GGA+GSV on USB2 for the panel's GNSS view. Sent on the
// command channel (USB1/itf2); targets USB2 by name. Logs only — a failure here
// costs the display feature, never the RTCM3 caster path.
static void provision_nmea(void)
{
    char cmd[128];
    snprintf(cmd, sizeof(cmd),
             "setNMEAOutput, " NMEA_STREAM ", " NMEA_PORT ", " NMEA_MSGS ", " NMEA_RATE);
    char reply[256];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "NMEA provision TX failed: %s", esp_err_to_name(err));
        return;
    }
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected the NMEA config ($R?)");
        return;
    }
    ESP_LOGI(TAG, "Mosaic NMEA output provisioned on " NMEA_PORT " (" NMEA_MSGS ")");
}

esp_err_t mosaic_provision(void)
{
    // Apply the dual-antenna attitude offset first, then start the SBF stream.
    // Both are RAM-only (no exeCopyConfigFile), re-applied every boot — the box
    // stays the source of truth and the receiver's NVM is left untouched.
    char reply[512];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(ATT_OFFSET, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "attitude-offset TX failed: %s", esp_err_to_name(err));
        return err;
    }
    // A $R? here is non-fatal (attitude may already be set); log and continue to
    // the SBF output, which is the command that actually gates the data path.
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected setAttitudeOffset ($R?) — continuing");
    }

    char cmd[256];
    snprintf(cmd, sizeof(cmd),
             "setSBFOutput, " SBF_STREAM ", " SBF_PORT ", " SBF_MSGS ", " SBF_RATE);
    n = 0;
    err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SBF provision TX failed: %s", esp_err_to_name(err));
        return err;
    }

    // Septentrio acks a good command with "$R:" and rejects with "$R?". Scan the
    // raw capture (not a C-string — it may hold binary) for the error marker.
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected the SBF config ($R?)");
        return ESP_FAIL;
    }
    if (n == 0) {
        // The port accepted the bytes but answered nothing — it isn't the
        // receiver's command interface. Signal the caller to retry on the next
        // interface it opens.
        ESP_LOGW(TAG, "no reply on this interface — not a command port");
        return ESP_ERR_TIMEOUT;
    }
    if (!buf_contains(reply, n, "$R:")) {
        // Bytes but no ack — likely buried under SBF binary on an already-
        // streaming port. The command almost certainly applied; best-effort OK.
        ESP_LOGW(TAG, "no $R: ack in %u B reply (buried in stream?) — assuming applied", (unsigned)n);
        provision_nmea();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Mosaic SBF output provisioned on " SBF_PORT " (" SBF_MSGS " @" SBF_RATE ")");
    provision_nmea();
    return ESP_OK;
}
