// ntrip_client.h — pull RTCM3 corrections from an NTRIP caster and feed them to
// the mosaic-go over USB (the receiver auto-detects RTCM3 input on any port).
//
// The client's base is rtk.toiso.fit:2101/eniwa-bd982 (docs/hardware-findings):
// no auth, no GGA needed (single-base), Transfer-Encoding: chunked. We use
// esp_http_client's streaming mode, which de-chunks transparently — so the bytes
// handed to the receiver are clean RTCM3 with no chunk-size headers mixed in.
//
// Creds live in NVS (set at runtime), defaulting to the known base above so the
// box works out of the box. Runs on its own task; idles until a netif is up.
#pragma once

#include <stdbool.h>
#include <stdint.h>

typedef struct {
    bool     configured;   // a caster is set (creds present)
    bool     connected;    // the RTCM3 stream is currently flowing
    char     host[64];
    uint16_t port;
    char     mount[32];
    uint64_t bytes_in;     // RTCM3 bytes forwarded to the receiver
    uint32_t reconnects;
    char     last_msg[48]; // human-readable state / last error
} ntrip_client_status_t;

// Start the NTRIP client task. Call once after network bring-up. It loads creds
// from NVS (or the built-in default) and connects; on drop it backs off + retries.
void ntrip_client_start(void);

// Set/replace the caster creds (NVS-persisted); reconnects on the next cycle.
void ntrip_client_set(const char *host, uint16_t port, const char *mount);

// Erase stored creds — the task returns to idle (no correction stream).
void ntrip_client_forget(void);

void ntrip_client_status(ntrip_client_status_t *out);
