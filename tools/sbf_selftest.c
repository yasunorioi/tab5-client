// sbf_selftest.c — host-side verification of main/sbf_parser.c against the real
// capture in tests/fixtures/, with NO ESP-IDF and NO receiver. Proves the C
// parser matches tools/parse_sbf.py (354 blocks / 0 CRC failures) and that its
// streaming reassembly survives chunk boundaries splitting a block.
//
//   cc -Imain -o /tmp/sbf_selftest tools/sbf_selftest.c main/sbf_parser.c -lm
//   /tmp/sbf_selftest tests/fixtures/mosaic-g5-p3h-sbf.bin
//
// Exit status is non-zero if any expectation fails, so it doubles as a test.

#include "sbf_parser.h"

#include <math.h>
#include <stdint.h>
#include <stdio.h>
#include <stdlib.h>

// Expected tally for tests/fixtures/mosaic-g5-p3h-sbf.bin (see tests/fixtures/README.md).
#define EXP_TOTAL 354u
#define EXP_PVT   114u
#define EXP_ATT   114u
#define EXP_DOP   114u
#define EXP_RXS    12u

static void dump_pvt(const sbf_pvtgeodetic_t *o)
{
    printf("  PVTGeodetic  mode=%s error=\"%s\" nrSV=%u\n",
           sbf_pvt_mode_str(o->mode_type), sbf_pvt_error_str(o->error),
           o->nr_sv);
    printf("    height_m=%s h_acc=%s v_acc=%s\n",
           isnan(o->height_m) ? "DNU" : "set",
           isnan(o->h_accuracy_m) ? "DNU" : "set",
           isnan(o->v_accuracy_m) ? "DNU" : "set");
}

int main(int argc, char **argv)
{
    if (argc < 2) {
        fprintf(stderr, "usage: %s <sbf-capture>\n", argv[0]);
        return 2;
    }
    FILE *f = fopen(argv[1], "rb");
    if (!f) { perror("fopen"); return 2; }
    fseek(f, 0, SEEK_END);
    long n = ftell(f);
    fseek(f, 0, SEEK_SET);
    uint8_t *data = malloc((size_t)n);
    if (!data || fread(data, 1, (size_t)n, f) != (size_t)n) {
        fprintf(stderr, "read failed\n"); return 2;
    }
    fclose(f);

    // Feed in 7-byte chunks so block boundaries land mid-chunk — this is what
    // exercises the streaming reassembly, not a single bulk feed.
    sbf_parser_t p;
    sbf_parser_init(&p);
    const uint32_t CHUNK = 7;
    for (long off = 0; off < n; off += CHUNK) {
        uint32_t c = (off + CHUNK <= n) ? CHUNK : (uint32_t)(n - off);
        sbf_parser_feed(&p, data + off, c);
    }
    free(data);

    printf("bytes: %ld   CRC-valid blocks: %u   CRC-failed: %u\n",
           n, p.valid_blocks, p.crc_failed);
    printf("  4001  DOP             x%u\n", p.dop_count);
    printf("  4007  PVTGeodetic     x%u\n", p.pvt_count);
    printf("  4014  ReceiverStatus  x%u\n", p.rxstatus_count);
    printf("  5938  AttEuler        x%u\n", p.att_count);
    printf("\n");
    if (p.pvt_valid) dump_pvt(&p.pvt);
    if (p.rxstatus_valid)
        printf("  ReceiverStatus  uptime=%us cpu=%u%% temp=%dC rx_error=0x%X\n",
               p.rxstatus.uptime_s, p.rxstatus.cpu_load_pct,
               p.rxstatus.temperature_c, p.rxstatus.rx_error);

    // Assertions — the whole point of the self-test.
    int ok = 1;
    #define CHECK(cond, msg) do { if (!(cond)) { printf("FAIL: %s\n", msg); ok = 0; } } while (0)
    CHECK(p.crc_failed == 0,               "CRC failures present");
    CHECK(p.valid_blocks == EXP_TOTAL,     "total block count");
    CHECK(p.pvt_count == EXP_PVT,          "PVTGeodetic count");
    CHECK(p.att_count == EXP_ATT,          "AttEuler count");
    CHECK(p.dop_count == EXP_DOP,          "DOP count");
    CHECK(p.rxstatus_count == EXP_RXS,     "ReceiverStatus count");
    #undef CHECK

    printf("\n%s\n", ok ? "PASS" : "FAIL");
    return ok ? 0 : 1;
}
