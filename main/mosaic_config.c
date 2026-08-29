// mosaic_config.c — see mosaic_config.h.

#include "mosaic_config.h"

#include <string.h>
#include <stdio.h>
#include "esp_log.h"
#include "nvs.h"

#include "usb_cdc_source.h"

static const char *TAG = "mosaic_cfg";

#define NMEA_NVS_NS "nmeacfg"

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
// satellite azimuth/elevation/SNR; GGA carries the position. Emitted on a
// SEPARATE port (USB2 = itf2 on this P3H) so the SBF stream on USB1 stays pure —
// the box opens itf2 read-only for display (see nmea_source.c). USB2 by name, so
// it applies regardless of which command interface we send it on.
#define NMEA_PORT   "USB2"
#define NMEA_STREAM "Stream1"
#define NMEA_MSGS   "GGA+GSV"
#define NMEA_RATE   "sec1"

// External RS232 outputs on COM1/COM2 (38400) for the machine controller /
// auto-steer, taken off the mosaic-go's serial ports through an RS232 transceiver.
// Fully independent of the USB SBF path — RTCM3 comes in over USB (NTRIP), so this
// NMEA carries the RTK-corrected position (quality 4). NMEA output streams are
// numbered separately from SBF, so Stream2/Stream3 here do not clash with the
// panel NMEA (Stream1) or the SBF stream. Each port's on/off + messages + rate are
// operator-configurable (settings_view.c) and persisted; the defaults are GGA @
// 10 Hz, with COM1 enabled and COM2 disabled out of the box.
#define NMEA_MSG_DEFAULT  NMEA_MSG_GGA
#define NMEA_RATE_DEFAULT NMEA_RATE_10HZ

// Per-port static config + NVS keys. Port 0 (COM1) keeps the original keys
// ("msgs"/"rate") for backward compatibility with configs saved before COM2 was
// added; the "en"/"baud" keys are new (defaults derived in cfg_get). Port 1
// (COM2) uses its own keys and is disabled by default.
static const struct {
    const char *com;      // receiver port name
    const char *stream;   // NMEA output stream (distinct per port)
    const char *k_msgs;   // NVS key: message bitmask
    const char *k_rate;   // NVS key: rate index
    const char *k_en;     // NVS key: enabled flag
    const char *k_baud;   // NVS key: baud index
    bool        def_en;   // default enabled when unset in NVS
} COM_PORTS[MOSAIC_COM_COUNT] = {
    { "COM1", "Stream2", "msgs",  "rate",  "en",  "baud",  true  },
    { "COM2", "Stream3", "msgs1", "rate1", "en1", "baud1", false },
};

// Bit → Septentrio message mnemonic (order = the bit order in mosaic_config.h).
static const struct { uint16_t bit; const char *name; } NMEA_MSGS_TBL[] = {
    { NMEA_MSG_GGA, "GGA" }, { NMEA_MSG_RMC, "RMC" }, { NMEA_MSG_VTG, "VTG" },
    { NMEA_MSG_GSA, "GSA" }, { NMEA_MSG_ZDA, "ZDA" }, { NMEA_MSG_GSV, "GSV" },
};
static const char *NMEA_RATE_CMD[NMEA_RATE_COUNT] = {
    "msec100", "msec200", "msec500", "sec1", "off",
};
static const char *NMEA_RATE_LABEL[NMEA_RATE_COUNT] = {
    "10Hz", "5Hz", "2Hz", "1Hz", "OFF",
};
static const char *NMEA_BAUD_CMD[NMEA_BAUD_COUNT] = {
    "baud4800", "baud9600", "baud19200", "baud38400", "baud57600", "baud115200",
};
static const char *NMEA_BAUD_LABEL[NMEA_BAUD_COUNT] = {
    "4800", "9600", "19200", "38400", "57600", "115200",
};

const char *mosaic_nmea_rate_str(uint8_t rate)
{
    return rate < NMEA_RATE_COUNT ? NMEA_RATE_LABEL[rate] : "?";
}

const char *mosaic_nmea_baud_str(uint8_t baud)
{
    return baud < NMEA_BAUD_COUNT ? NMEA_BAUD_LABEL[baud] : "?";
}

const char *mosaic_com_name(mosaic_com_t port)
{
    return port < MOSAIC_COM_COUNT ? COM_PORTS[port].com : "?";
}

void mosaic_nmea_cfg_get(mosaic_com_t port, bool *enabled, uint16_t *msg_mask,
                         uint8_t *rate, uint8_t *baud)
{
    if (port >= MOSAIC_COM_COUNT) return;
    uint16_t m = NMEA_MSG_DEFAULT;
    uint8_t  r = NMEA_RATE_DEFAULT;
    uint8_t  b = NMEA_BAUD_DEF;
    uint8_t  en = COM_PORTS[port].def_en;
    bool     have_en = false;
    nvs_handle_t h;
    if (nvs_open(NMEA_NVS_NS, NVS_READONLY, &h) == ESP_OK) {
        nvs_get_u16(h, COM_PORTS[port].k_msgs, &m);
        nvs_get_u8(h, COM_PORTS[port].k_rate, &r);
        nvs_get_u8(h, COM_PORTS[port].k_baud, &b);
        have_en = (nvs_get_u8(h, COM_PORTS[port].k_en, &en) == ESP_OK);
        nvs_close(h);
    }
    // Migrate pre-COM2 configs: back then rate==OFF meant "disabled". If the new
    // enabled key is absent, derive on/off from that legacy sentinel.
    if (!have_en && r == NMEA_RATE_OFF) en = 0;
    if (enabled)  *enabled = en != 0;
    if (msg_mask) *msg_mask = m;
    // OFF is no longer a selectable rate — clamp the stored value to a real rate.
    if (rate) *rate = r < NMEA_RATE_SELECTABLE ? r : NMEA_RATE_DEFAULT;
    if (baud) *baud = b < NMEA_BAUD_COUNT ? b : NMEA_BAUD_DEF;
}

// Build "setNMEAOutput, <stream>, <COMx>, <GGA+RMC+...>, <rate>" for a port into
// `out`. Returns false if the port is disabled or has no messages, in which case
// *out is the disabling command ("...COMx, none").
static bool build_nmea_cmd(mosaic_com_t port, bool enabled, uint16_t msgs,
                           uint8_t rate, char *out, size_t outlen)
{
    if (!enabled || msgs == 0 || rate >= NMEA_RATE_OFF) {
        snprintf(out, outlen, "setNMEAOutput, %s, %s, none",
                 COM_PORTS[port].stream, COM_PORTS[port].com);
        return false;
    }
    char list[64];
    size_t pos = 0;
    for (size_t i = 0; i < sizeof(NMEA_MSGS_TBL) / sizeof(NMEA_MSGS_TBL[0]); i++) {
        if (msgs & NMEA_MSGS_TBL[i].bit) {
            int w = snprintf(list + pos, sizeof(list) - pos, "%s%s",
                             pos ? "+" : "", NMEA_MSGS_TBL[i].name);
            if (w > 0) pos += (size_t)w;
        }
    }
    snprintf(out, outlen, "setNMEAOutput, %s, %s, %s, %s",
             COM_PORTS[port].stream, COM_PORTS[port].com, list, NMEA_RATE_CMD[rate]);
    return true;
}

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

// Send a port's NMEA output command built from (enabled,msgs,rate). Returns the
// receiver result. Best-effort at boot; also used by the live apply path.
static esp_err_t send_com_nmea(mosaic_com_t port, bool enabled, uint16_t msgs,
                               uint8_t rate)
{
    char cmd[160], reply[256];
    size_t n = 0;
    build_nmea_cmd(port, enabled, msgs, rate, cmd, sizeof(cmd));
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s NMEA TX failed: %s", COM_PORTS[port].com,
                 esp_err_to_name(err));
        return err;
    }
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected %s NMEA output ($R?)", COM_PORTS[port].com);
        return ESP_FAIL;
    }
    ESP_LOGI(TAG, "Mosaic %s NMEA provisioned (%s)", COM_PORTS[port].com,
             enabled ? mosaic_nmea_rate_str(rate) : "OFF");
    return ESP_OK;
}

// Set a port's serial baud (8N1). Best-effort; the COM ports are distinct from
// our USB1 command channel, so changing their baud never disturbs the SBF path.
static esp_err_t send_com_settings(mosaic_com_t port, uint8_t baud)
{
    if (baud >= NMEA_BAUD_COUNT) baud = NMEA_BAUD_DEF;
    char cmd[128], reply[256];
    size_t n = 0;
    snprintf(cmd, sizeof(cmd), "setCOMSettings, %s, %s",
             COM_PORTS[port].com, NMEA_BAUD_CMD[baud]);
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 2000);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "%s settings TX failed", COM_PORTS[port].com);
        return err;
    }
    if (buf_contains(reply, n, "$R?")) {
        ESP_LOGW(TAG, "Mosaic rejected setCOMSettings %s ($R?) — continuing",
                 COM_PORTS[port].com);
    }
    return ESP_OK;
}

// Best-effort: RS232 NMEA on both COM ports for external consumers (blade
// controller / auto-steer), from the operator's saved config. Sets each port's
// baud first (even when disabled, so a later live-enable needs no reboot), then
// the NMEA output. Logs only — a failure here costs the RS232 feed, never the SBF
// data path.
static void provision_com_ports(void)
{
    for (int p = 0; p < MOSAIC_COM_COUNT; p++) {
        bool en; uint16_t msgs; uint8_t rate, baud;
        mosaic_nmea_cfg_get((mosaic_com_t)p, &en, &msgs, &rate, &baud);
        if (send_com_settings((mosaic_com_t)p, baud) != ESP_OK) continue;
        send_com_nmea((mosaic_com_t)p, en, msgs, rate);
    }
}

esp_err_t mosaic_nmea_cfg_apply(mosaic_com_t port, bool enabled,
                                uint16_t msg_mask, uint8_t rate, uint8_t baud)
{
    if (port >= MOSAIC_COM_COUNT) return ESP_ERR_INVALID_ARG;
    if (rate >= NMEA_RATE_SELECTABLE) rate = NMEA_RATE_DEFAULT;
    if (baud >= NMEA_BAUD_COUNT) baud = NMEA_BAUD_DEF;
    nvs_handle_t h;
    if (nvs_open(NMEA_NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_set_u16(h, COM_PORTS[port].k_msgs, msg_mask);
        nvs_set_u8(h, COM_PORTS[port].k_rate, rate);
        nvs_set_u8(h, COM_PORTS[port].k_en, enabled ? 1 : 0);
        nvs_set_u8(h, COM_PORTS[port].k_baud, baud);
        nvs_commit(h);
        nvs_close(h);
    }
    // Apply now if the receiver's command interface is open; otherwise it takes
    // effect on the next provision (usb_cdc_send_command returns INVALID_STATE).
    // Set the baud first, then the NMEA output.
    esp_err_t err = send_com_settings(port, baud);
    if (err == ESP_OK) err = send_com_nmea(port, enabled, msg_mask, rate);
    return err == ESP_ERR_INVALID_STATE ? ESP_OK : err;
}

// All the auxiliary (non-SBF) outputs: panel NMEA on USB2 + RS232 NMEA on the COM
// ports. Each is independent and best-effort, so one being rejected never blocks
// the others.
static void provision_aux_outputs(void)
{
    provision_nmea();
    provision_com_ports();
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
        provision_aux_outputs();
        return ESP_OK;
    }
    ESP_LOGI(TAG, "Mosaic SBF output provisioned on " SBF_PORT " (" SBF_MSGS " @" SBF_RATE ")");
    provision_aux_outputs();
    return ESP_OK;
}
