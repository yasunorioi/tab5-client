// sbf_parser.c — see sbf_parser.h. Pure C, no ESP-IDF dependency.
//
// The scan mirrors tools/parse_sbf.py / tools/parse-sbf.ps1 exactly: walk the
// buffer, latch on the $@ sync, sanity-check Length, CRC-check ID..end, decode,
// and advance by Length. On a bad sync/length/CRC, advance one byte and re-scan
// (a false $@ inside data resynchronises this way). Bytes for a block that has
// not fully arrived yet are kept and re-scanned on the next feed.

#include "sbf_parser.h"

#include <math.h>
#include <string.h>

#define SBF_SYNC1 0x24u  // '$'
#define SBF_SYNC2 0x40u  // '@'

// ── Little-endian, alignment-safe field reads (memcpy, so no unaligned traps) ─
static uint16_t rd_u16(const uint8_t *b, uint32_t off)
{
    return (uint16_t)(b[off] | ((uint16_t)b[off + 1] << 8));
}
static uint32_t rd_u32(const uint8_t *b, uint32_t off)
{
    return (uint32_t)b[off] | ((uint32_t)b[off + 1] << 8) |
           ((uint32_t)b[off + 2] << 16) | ((uint32_t)b[off + 3] << 24);
}
static float rd_f32(const uint8_t *b, uint32_t off)
{
    float v;
    memcpy(&v, b + off, 4);   // wire is IEEE-754 LE; host (x86/riscv) is LE
    return v;
}
static double rd_f64(const uint8_t *b, uint32_t off)
{
    double v;
    memcpy(&v, b + off, 8);
    return v;
}

// SBF floats/doubles use -2e10 as the Do-Not-Use sentinel.
static float f32_dnu(float v) { return v <= -2.0e10f ? NAN : v; }
static double f64_dnu(double v) { return v <= -2.0e10 ? NAN : v; }

// CRC-16-CCITT (poly 0x1021, init 0), applied over buf[off .. off+len).
static uint16_t crc16_ccitt(const uint8_t *buf, uint32_t off, uint32_t len)
{
    uint16_t crc = 0;
    for (uint32_t i = off; i < off + len; i++) {
        crc ^= (uint16_t)((uint16_t)buf[i] << 8);
        for (int k = 0; k < 8; k++) {
            crc = (crc & 0x8000) ? (uint16_t)((crc << 1) ^ 0x1021)
                                 : (uint16_t)(crc << 1);
        }
    }
    return crc;
}

// ── Per-block decoders. `b` points at the block start (sync at b[0]); offsets
//    are header-inclusive to match the @NN notation in docs/hardware-findings. ─
static void decode_pvtgeodetic(sbf_parser_t *p, const uint8_t *b)
{
    sbf_pvtgeodetic_t *o = &p->pvt;
    o->tow_ms   = rd_u32(b, 8);
    o->wnc      = rd_u16(b, 12);
    o->mode_raw = b[14];
    o->mode_type = (uint8_t)(o->mode_raw & 0x0F);
    o->error    = b[15];
    double lat = rd_f64(b, 16), lon = rd_f64(b, 24);   // radians
    o->lat_deg  = lat <= -2.0e10 ? NAN : lat * (180.0 / M_PI);
    o->lon_deg  = lon <= -2.0e10 ? NAN : lon * (180.0 / M_PI);
    o->height_m = f64_dnu(rd_f64(b, 32));              // ellipsoidal
    o->nr_sv    = b[74];
    uint16_t hacc = rd_u16(b, 90), vacc = rd_u16(b, 92);   // cm, 0xFFFF = DNU
    o->h_accuracy_m = (hacc == SBF_DNU_U2) ? NAN : hacc / 100.0f;
    o->v_accuracy_m = (vacc == SBF_DNU_U2) ? NAN : vacc / 100.0f;
    p->pvt_count++;
    p->pvt_valid = true;
}

static void decode_atteuler(sbf_parser_t *p, const uint8_t *b)
{
    sbf_atteuler_t *o = &p->att;
    o->tow_ms  = rd_u32(b, 8);
    o->wnc     = rd_u16(b, 12);
    o->nr_sv   = b[14];
    o->error   = b[15];
    o->mode    = rd_u16(b, 16);
    o->heading_deg = f32_dnu(rd_f32(b, 20));
    o->pitch_deg   = f32_dnu(rd_f32(b, 24));
    o->roll_deg    = f32_dnu(rd_f32(b, 28));
    p->att_count++;
    p->att_valid = true;
}

static void decode_dop(sbf_parser_t *p, const uint8_t *b)
{
    sbf_dop_t *o = &p->dop;
    o->tow_ms = rd_u32(b, 8);
    o->wnc    = rd_u16(b, 12);
    o->nr_sv  = b[14];
    // xDOP: u16 scaled by 100, 0 = Do-Not-Use.
    uint16_t pd = rd_u16(b, 16), td = rd_u16(b, 18),
             hd = rd_u16(b, 20), vd = rd_u16(b, 22);
    o->pdop = pd ? pd / 100.0f : NAN;
    o->tdop = td ? td / 100.0f : NAN;
    o->hdop = hd ? hd / 100.0f : NAN;
    o->vdop = vd ? vd / 100.0f : NAN;
    o->hpl_m = f32_dnu(rd_f32(b, 24));
    o->vpl_m = f32_dnu(rd_f32(b, 28));
    p->dop_count++;
    p->dop_valid = true;
}

static void decode_receiverstatus(sbf_parser_t *p, const uint8_t *b)
{
    sbf_receiverstatus_t *o = &p->rxstatus;
    o->tow_ms       = rd_u32(b, 8);
    o->wnc          = rd_u16(b, 12);
    o->cpu_load_pct = b[14];
    o->ext_error    = b[15];
    o->uptime_s     = rd_u32(b, 16);
    o->rx_state     = rd_u32(b, 20);
    o->rx_error     = rd_u32(b, 24);
    o->agc_subblocks = b[28];
    // Temperature: degrees Celsius with a +100 offset; 0 = Do-Not-Use.
    uint8_t t = b[31];
    o->temperature_c = t ? (int16_t)((int)t - 100) : INT16_MIN;
    p->rxstatus_count++;
    p->rxstatus_valid = true;
}

// Dispatch a CRC-valid block by its block number (ID low 13 bits).
static void decode_block(sbf_parser_t *p, const uint8_t *b, uint16_t block_num)
{
    switch (block_num) {
    case 4007: decode_pvtgeodetic(p, b);    break;
    case 5938: decode_atteuler(p, b);       break;
    case 4001: decode_dop(p, b);            break;
    case 4014: decode_receiverstatus(p, b); break;
    default:   break;   // CRC-valid but not a block we decode; still counted
    }
}

void sbf_parser_init(sbf_parser_t *p)
{
    memset(p, 0, sizeof(*p));
}

uint32_t sbf_parser_feed(sbf_parser_t *p, const uint8_t *data, uint32_t len)
{
    uint32_t decoded = 0;

    for (uint32_t k = 0; k < len; k++) {
        // Drop bytes we cannot buffer; a full block always fits (cap > max block).
        if (p->len >= SBF_BUF_CAP) {
            // Should not happen given the scan below keeps len < one block during
            // accumulation, but stay safe: discard oldest byte to make room.
            memmove(p->buf, p->buf + 1, --p->len);
        }
        p->buf[p->len++] = data[k];

        // Scan whenever we might hold a complete block. Consume from the front.
        uint32_t i = 0;
        while (p->len - i >= 8) {
            if (p->buf[i] != SBF_SYNC1 || p->buf[i + 1] != SBF_SYNC2) {
                i++;
                continue;
            }
            uint16_t crc   = rd_u16(p->buf, i + 2);
            uint16_t ident = rd_u16(p->buf, i + 4);
            uint16_t blen  = rd_u16(p->buf, i + 6);
            if (blen < 8 || (blen % 4) != 0 || blen > SBF_MAX_BLOCK) {
                i++;                       // bogus length ⇒ false sync
                continue;
            }
            if (p->len - i < blen) {
                break;                     // block not fully arrived yet
            }
            if (crc16_ccitt(p->buf, i + 4, (uint32_t)blen - 4) == crc) {
                uint16_t block_num = ident & 0x1FFF;
                decode_block(p, p->buf + i, block_num);
                p->valid_blocks++;
                decoded++;
                i += blen;                 // advance past the whole block
            } else {
                p->crc_failed++;
                i++;                       // CRC miss ⇒ resync one byte on
            }
        }

        // Compact: drop everything we consumed/skipped, keep the tail.
        if (i > 0) {
            memmove(p->buf, p->buf + i, p->len - i);
            p->len -= i;
        }
    }

    return decoded;
}

const char *sbf_pvt_mode_str(uint8_t mode_type)
{
    switch (mode_type) {
    case 0:  return "No PVT";
    case 1:  return "Stand-Alone";
    case 2:  return "Differential";
    case 3:  return "Fixed location";
    case 4:  return "RTK fixed";
    case 5:  return "RTK float";
    case 6:  return "SBAS";
    case 7:  return "moving-base RTK fixed";
    case 8:  return "moving-base RTK float";
    case 10: return "PPP";
    default: return "?";
    }
}

const char *sbf_pvt_error_str(uint8_t error)
{
    switch (error) {
    case 0:  return "No Error";
    case 1:  return "Not enough measurements";
    case 2:  return "Not enough ephemerides";
    case 3:  return "DOP too large";
    case 4:  return "Residuals too large";
    case 5:  return "No convergence";
    case 6:  return "Not enough measurements after outlier rejection";
    case 7:  return "Output prohibited (export laws)";
    case 8:  return "Not enough differential corrections";
    case 9:  return "Base coordinates unavailable";
    case 10: return "Ambiguities not fixed (RTK-fixed only requested)";
    default: return "?";
    }
}
