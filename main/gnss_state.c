// gnss_state.c — see gnss_state.h.

#include "gnss_state.h"

#include "freertos/FreeRTOS.h"
#include "freertos/semphr.h"
#include "esp_timer.h"

static sbf_parser_t      s_parser;
static SemaphoreHandle_t s_lock;
static volatile int64_t  s_last_block_us;   // esp_timer time of last valid block

void gnss_state_init(void)
{
    sbf_parser_init(&s_parser);
    s_last_block_us = 0;
    if (s_lock == NULL) {
        s_lock = xSemaphoreCreateMutex();
    }
}

uint32_t gnss_state_feed(const uint8_t *data, uint32_t len)
{
    if (s_lock == NULL) {
        return 0;   // init not run yet — drop rather than corrupt
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    uint32_t decoded = sbf_parser_feed(&s_parser, data, len);
    xSemaphoreGive(s_lock);

    if (decoded > 0) {
        s_last_block_us = esp_timer_get_time();   // single writer (driver task)
    }
    return decoded;
}

void gnss_state_snapshot(gnss_snapshot_t *out)
{
    if (out == NULL) return;
    if (s_lock == NULL) {
        *out = (gnss_snapshot_t){0};   // empty snapshot before init
        return;
    }
    xSemaphoreTake(s_lock, portMAX_DELAY);
    out->valid_blocks = s_parser.valid_blocks;
    out->crc_failed   = s_parser.crc_failed;
    out->pvt = s_parser.pvt;               out->pvt_valid = s_parser.pvt_valid;
    out->att = s_parser.att;               out->att_valid = s_parser.att_valid;
    out->dop = s_parser.dop;               out->dop_valid = s_parser.dop_valid;
    out->rxstatus = s_parser.rxstatus;     out->rxstatus_valid = s_parser.rxstatus_valid;
    xSemaphoreGive(s_lock);
    out->last_block_us = s_last_block_us;
}

uint32_t gnss_state_valid_blocks(void)
{
    // Monotonic counter written only by the feeding task; a 32-bit aligned read is
    // atomic on the P4, and a threshold comparison tolerates a momentarily stale
    // value — so no lock is needed for this hot-path gate.
    return s_parser.valid_blocks;
}

int64_t gnss_state_last_block_us(void)
{
    return s_last_block_us;
}

bool gnss_state_fresh(int64_t max_age_us)
{
    int64_t last = s_last_block_us;
    if (last == 0) return false;
    return (esp_timer_get_time() - last) < max_age_us;
}
