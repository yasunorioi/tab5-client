// sbf_parser.h — Septentrio SBF streaming parser for mosaic-go G5 P3H.
//
// Pure C (stdint/string/math only) — NO ESP-IDF dependency, so the same code
// builds on the host for the fixture self-test (tools/sbf_selftest.c) and on the
// esp32p4 target. Reference implementation: tools/parse_sbf.py (kept in sync).
//
// Feed arbitrary USB CDC chunks with sbf_parser_feed(); the parser frames on the
// $@ sync, validates CRC-16-CCITT over ID..end, decodes the four blocks we
// provision (PVTGeodetic / AttEuler / DOP / ReceiverStatus) and keeps the latest
// decoded value of each plus running counters. cut/fill reads sbf->pvt.height_m.
//
// Framing: sync $@ (0x24 0x40), CRC(u16 LE), ID(u16 LE), Length(u16 LE). ID low
// 13 bits = block number, high 3 = revision. Length is the whole block including
// the 8-byte header and is a multiple of 4. CRC-16-CCITT (poly 0x1021, init 0)
// is taken over the Length-4 bytes starting at the ID field (block+4).
//
// ⚠ Do-Not-Use sentinels are surfaced as NAN (floats/doubles) and all-ones
// (integers), matching SBF. Consumers must check (isnan / 0xFF..) before use;
// the indoor fixtures decode almost entirely to Do-Not-Use, which is correct.
#pragma once

#include <stdbool.h>
#include <stdint.h>

#define SBF_MAX_BLOCK 512u    // longest block we accept; longer ones are skipped
#define SBF_BUF_CAP   1024u   // must exceed SBF_MAX_BLOCK so a full block + slack fits

// Do-Not-Use sentinels (integers passed through as-is from the wire).
#define SBF_DNU_U1 0xFFu
#define SBF_DNU_U2 0xFFFFu
#define SBF_DNU_U4 0xFFFFFFFFu

// ── Decoded blocks ───────────────────────────────────────────────────────────
// Every SBF block starts with TOW(u32, ms of week) + WNc(u16, week number).
// tow_ms == SBF_DNU_U4 / wnc == SBF_DNU_U2 mean "not set" (no valid time yet).

typedef struct {
    uint32_t tow_ms;
    uint16_t wnc;
    uint8_t  mode_raw;       // full Mode byte
    uint8_t  mode_type;      // mode_raw & 0x0F — see sbf_pvt_mode_str()
    uint8_t  error;          // 0 = no error; see sbf_pvt_error_str()
    uint8_t  nr_sv;          // 0xFF = Do-Not-Use
    double   lat_deg;        // NAN if Do-Not-Use
    double   lon_deg;        // NAN if Do-Not-Use
    double   height_m;       // ellipsoidal height — cut/fill uses THIS (no geoid)
    float    h_accuracy_m;   // NAN if Do-Not-Use (wire = 0xFFFF)
    float    v_accuracy_m;   // NAN if Do-Not-Use
} sbf_pvtgeodetic_t;

typedef struct {
    uint32_t tow_ms;
    uint16_t wnc;
    uint8_t  nr_sv;          // 0xFF = Do-Not-Use
    uint8_t  error;
    uint16_t mode;
    float    heading_deg;    // NAN if Do-Not-Use
    float    pitch_deg;      // NAN if Do-Not-Use
    float    roll_deg;       // NAN if Do-Not-Use
} sbf_atteuler_t;

typedef struct {
    uint32_t tow_ms;
    uint16_t wnc;
    uint8_t  nr_sv;          // 0xFF = Do-Not-Use
    float    pdop;           // NAN if Do-Not-Use (wire = 0)
    float    tdop;
    float    hdop;
    float    vdop;
    float    hpl_m;
    float    vpl_m;
} sbf_dop_t;

typedef struct {
    uint32_t tow_ms;
    uint16_t wnc;
    uint8_t  cpu_load_pct;   // 0xFF = Do-Not-Use
    uint8_t  ext_error;
    uint32_t uptime_s;
    uint32_t rx_state;
    uint32_t rx_error;       // non-zero ⇒ receiver error latched (cf. "ERROR: SW,")
    uint8_t  agc_subblocks;
    int16_t  temperature_c;  // INT16_MIN if Do-Not-Use (wire = 0)
} sbf_receiverstatus_t;

// ── Parser state ─────────────────────────────────────────────────────────────
typedef struct {
    uint8_t  buf[SBF_BUF_CAP];
    uint32_t len;            // bytes currently buffered

    // Running tallies since init.
    uint32_t valid_blocks;
    uint32_t crc_failed;

    // Latest decoded value of each block, per-block counts (stall/rate checks),
    // and a *_valid flag set true once one has been seen.
    sbf_pvtgeodetic_t    pvt;      uint32_t pvt_count;      bool pvt_valid;
    sbf_atteuler_t       att;      uint32_t att_count;      bool att_valid;
    sbf_dop_t            dop;      uint32_t dop_count;      bool dop_valid;
    sbf_receiverstatus_t rxstatus; uint32_t rxstatus_count; bool rxstatus_valid;
} sbf_parser_t;

// Reset a parser to empty. Call once before the first feed.
void sbf_parser_init(sbf_parser_t *p);

// Feed a chunk of received bytes. Complete, CRC-valid blocks are decoded and
// stored; partial trailing bytes are retained for the next call. Returns the
// number of CRC-valid blocks decoded in THIS call.
uint32_t sbf_parser_feed(sbf_parser_t *p, const uint8_t *data, uint32_t len);

// Human-readable strings for PVTGeodetic Mode/Error (never NULL).
const char *sbf_pvt_mode_str(uint8_t mode_type);
const char *sbf_pvt_error_str(uint8_t error);
