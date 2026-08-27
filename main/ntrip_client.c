// ntrip_client.c — see ntrip_client.h.

#include "ntrip_client.h"
#include "usb_cdc_source.h"

#include <string.h>
#include <stdio.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_http_client.h"
#include "nvs.h"

static const char *TAG = "ntrip";

#define NVS_NS "ntrip"

// Built-in default base (docs/hardware-findings.md). Used until overridden.
#define DEF_HOST  "rtk.toiso.fit"
#define DEF_PORT  2101
#define DEF_MOUNT "eniwa-bd982"

// Reconnect backoff bounds.
#define BACKOFF_MIN_MS 1000
#define BACKOFF_MAX_MS 15000

// Live status. Single writer (the client task); readers tolerate a torn field.
static volatile bool     s_connected;
static volatile uint64_t s_bytes_in;
static volatile uint32_t s_reconnects;
static char              s_host[64] = DEF_HOST;
static uint16_t          s_port     = DEF_PORT;
static char              s_mount[32] = DEF_MOUNT;
static char              s_last_msg[48] = "idle";

static void set_msg(const char *m)
{
    strlcpy(s_last_msg, m, sizeof(s_last_msg));
}

void ntrip_client_status(ntrip_client_status_t *out)
{
    if (!out) return;
    out->configured = s_host[0] != '\0';
    out->connected  = s_connected;
    strlcpy(out->host, s_host, sizeof(out->host));
    out->port = s_port;
    strlcpy(out->mount, s_mount, sizeof(out->mount));
    out->bytes_in   = s_bytes_in;
    out->reconnects = s_reconnects;
    strlcpy(out->last_msg, s_last_msg, sizeof(out->last_msg));
}

// ── creds (NVS) ──────────────────────────────────────────────────────────────
static void creds_load(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READONLY, &h) != ESP_OK) return;   // keep defaults
    size_t n = sizeof(s_host);  nvs_get_str(h, "host", s_host, &n);
    n = sizeof(s_mount);        nvs_get_str(h, "mount", s_mount, &n);
    uint16_t p; if (nvs_get_u16(h, "port", &p) == ESP_OK) s_port = p;
    nvs_close(h);
}

void ntrip_client_set(const char *host, uint16_t port, const char *mount)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) != ESP_OK) return;
    nvs_set_str(h, "host", host ? host : "");
    nvs_set_str(h, "mount", mount ? mount : "");
    nvs_set_u16(h, "port", port);
    nvs_commit(h);
    nvs_close(h);
    // Update the live copy so the next connect cycle uses it (task re-reads too).
    strlcpy(s_host, host ? host : "", sizeof(s_host));
    strlcpy(s_mount, mount ? mount : "", sizeof(s_mount));
    s_port = port;
    ESP_LOGI(TAG, "caster set to %s:%u/%s", s_host, s_port, s_mount);
}

void ntrip_client_forget(void)
{
    nvs_handle_t h;
    if (nvs_open(NVS_NS, NVS_READWRITE, &h) == ESP_OK) {
        nvs_erase_all(h);
        nvs_commit(h);
        nvs_close(h);
    }
    s_host[0] = '\0';
    ESP_LOGW(TAG, "caster creds erased — idling");
}

// ── one connect + stream cycle ───────────────────────────────────────────────
// Opens the mountpoint and pipes de-chunked RTCM3 to the receiver until the
// stream drops. Returns when disconnected (caller backs off + retries).
static void stream_once(void)
{
    char url[128];
    snprintf(url, sizeof(url), "http://%s:%u/%s", s_host, s_port, s_mount);

    esp_http_client_config_t cfg = {
        .url = url,
        .timeout_ms = 8000,        // a healthy base sends ≥1 Hz; 8 s silence = dead
        .buffer_size = 1024,
        .disable_auto_redirect = true,
    };
    esp_http_client_handle_t cl = esp_http_client_init(&cfg);
    if (!cl) { set_msg("init failed"); return; }

    // NTRIP v2 handshake headers. No auth / no GGA for this single-base mount.
    esp_http_client_set_header(cl, "Ntrip-Version", "Ntrip/2.0");
    esp_http_client_set_header(cl, "User-Agent", "NTRIP tab5-client/1.0");

    esp_err_t err = esp_http_client_open(cl, 0);   // GET, no request body
    if (err != ESP_OK) {
        set_msg("connect failed");
        ESP_LOGW(TAG, "open %s: %s", url, esp_err_to_name(err));
        esp_http_client_cleanup(cl);
        return;
    }
    // Read the response headers so status/Transfer-Encoding are parsed; from here
    // esp_http_client_read() yields de-chunked body bytes.
    esp_http_client_fetch_headers(cl);
    int status = esp_http_client_get_status_code(cl);
    if (status != 200) {
        char m[48]; snprintf(m, sizeof(m), "HTTP %d", status); set_msg(m);
        ESP_LOGW(TAG, "%s -> HTTP %d", url, status);
        esp_http_client_close(cl);
        esp_http_client_cleanup(cl);
        return;
    }

    s_connected = true;
    set_msg("streaming");
    ESP_LOGI(TAG, "connected %s — piping RTCM3 to receiver", url);

    uint8_t buf[1024];
    while (1) {
        int r = esp_http_client_read(cl, (char *)buf, sizeof(buf));
        if (r <= 0) {           // 0 = closed/timeout, <0 = error → reconnect
            set_msg("stream ended");
            break;
        }
        // Forward de-chunked RTCM3 straight to the receiver. A USB write failure
        // means the CDC interface isn't open (yet) — drop this chunk, keep reading.
        if (usb_cdc_write(buf, (size_t)r, 1000) == ESP_OK) {
            s_bytes_in += (uint64_t)r;
        }
    }

    s_connected = false;
    esp_http_client_close(cl);
    esp_http_client_cleanup(cl);
}

static void ntrip_task(void *arg)
{
    (void)arg;
    creds_load();

    int backoff = BACKOFF_MIN_MS;
    while (1) {
        if (s_host[0] == '\0') {          // unconfigured → idle, re-check slowly
            set_msg("idle (no caster)");
            vTaskDelay(pdMS_TO_TICKS(5000));
            creds_load();
            continue;
        }

        stream_once();                    // returns on disconnect
        s_reconnects++;

        // Exponential backoff, capped, so a down caster doesn't hammer the link.
        vTaskDelay(pdMS_TO_TICKS(backoff));
        backoff = backoff < BACKOFF_MAX_MS ? backoff * 2 : BACKOFF_MAX_MS;
        // A clean prior stream (bytes flowed) resets the backoff for a fast retry.
        if (s_connected == false && s_bytes_in > 0) backoff = BACKOFF_MIN_MS;
        creds_load();                     // pick up any `ntripset` change
    }
}

void ntrip_client_start(void)
{
    // 6 KiB: TLS is unused (plain HTTP) but esp_http_client + lwIP recv paths are
    // still stack-hungry.
    xTaskCreate(ntrip_task, "ntrip", 6144, NULL, 5, NULL);
}
