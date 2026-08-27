// gnss_state.h — shared, thread-safe holder of the SBF-derived receiver state.
//
// The USB CDC driver task feeds raw SBF bytes in via gnss_state_feed(); any other
// task (display, backlight, console, cut/fill) reads a consistent snapshot with
// gnss_state_snapshot(). This is the client-side replacement for rtcm_monitor's
// role as the one place the rest of the box looks to see "what is the receiver
// doing".
//
// Wraps one sbf_parser_t (latest PVTGeodetic/AttEuler/DOP/ReceiverStatus + counts)
// and stamps the arrival time of the last CRC-valid block so consumers can judge
// stream freshness (e.g. backlight brownout detection, "no fix" UI).
#pragma once

#include <stdbool.h>
#include <stdint.h>

#include "sbf_parser.h"

// Lightweight snapshot for consumers: the decoded blocks + counters WITHOUT the
// parser's internal reassembly buffer (which is ~1 KB — too heavy to copy onto a
// task stack). ~250 bytes, safe to hold as a local.
typedef struct {
    uint32_t valid_blocks;
    uint32_t crc_failed;
    int64_t  last_block_us;      // esp_timer time of the last valid block, 0 if none

    sbf_pvtgeodetic_t    pvt;      bool pvt_valid;
    sbf_atteuler_t       att;      bool att_valid;
    sbf_dop_t            dop;      bool dop_valid;
    sbf_receiverstatus_t rxstatus; bool rxstatus_valid;
} gnss_snapshot_t;

// Initialise the parser + mutex. Call once before any feed/get (app_main).
void gnss_state_init(void);

// Feed a chunk of received SBF bytes (USB CDC driver task context). Decodes any
// complete blocks and, if at least one arrived, stamps the freshness clock.
// Returns the number of CRC-valid blocks decoded in this call.
uint32_t gnss_state_feed(const uint8_t *data, uint32_t len);

// Copy a consistent lightweight snapshot into *out (excludes the parser buffer).
void gnss_state_snapshot(gnss_snapshot_t *out);

// Scalar CRC-valid block count — cheap, for the USB source's interface-sweep gate
// (avoids copying a whole snapshot onto the driver task's small stack).
uint32_t gnss_state_valid_blocks(void);

// esp_timer timestamp (microseconds) of the last CRC-valid block; 0 if none yet.
int64_t gnss_state_last_block_us(void);

// True if a CRC-valid block arrived within the last max_age_us microseconds.
bool gnss_state_fresh(int64_t max_age_us);
