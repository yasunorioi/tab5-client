// logger.c — see logger.h.

#include "logger.h"
#include "gnss_state.h"
#include "leveler.h"

#include <math.h>
#include <stdio.h>
#include <string.h>
#include <sys/stat.h>

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_vfs_fat.h"
#include "driver/sdmmc_host.h"
#include "sdmmc_cmd.h"
#include "sd_pwr_ctrl_by_on_chip_ldo.h"

static const char *TAG = "logger";

// Tab5 microSD (M5Tab5-UserDemo BSP): SDMMC slot 0, 4-bit, on-chip LDO ch4 @3.3V.
#define SD_CLK  GPIO_NUM_43
#define SD_CMD  GPIO_NUM_44
#define SD_D0   GPIO_NUM_39
#define SD_D1   GPIO_NUM_40
#define SD_D2   GPIO_NUM_41
#define SD_D3   GPIO_NUM_42
#define SD_LDO_CHAN 4
#define SD_LDO_MV   3300

#define MOUNT_POINT "/sdcard"
#define WRITE_PERIOD_MS 1000
#define FLUSH_EVERY 8            // fsync every N rows so a power cut loses little

static bool               s_mounted;
static volatile bool      s_want_logging;   // requested by logger_set()
static sdmmc_card_t      *s_card;
static sd_pwr_ctrl_handle_t s_pwr;

// Owned solely by the writer task (no lock needed).
static FILE    *s_file;
static uint32_t s_rows;
static char     s_path[40];

static esp_err_t mount_sd(void)
{
    // Config faithful to M5Stack's Tab5 BSP (slot 0, 4-bit, on-chip LDO ch4 @3.3V).
    //
    // ⚠ BLOCKED on this unit (2026-08): the card answers CMD (CID reads: MANF 0x92)
    // but every DATA-line read returns zeros (SCR sd_spec=0 bus_width=0), so mount
    // fails with FR_NO_FILESYSTEM (13) even on a freshly FAT32-formatted card, in
    // both 4-bit and 1-bit, with and without the pwr_ctrl LDO. The on-chip LDO also
    // warns "voltage 0 out of [500,2700]". Symptoms = the card is powered enough to
    // answer commands but data transfer fails — a Tab5 SD power/signal integration
    // detail, NOT a logic bug (this file matches M5's BSP). To resolve, next session:
    //   - reseat / try a known-good card first (cheapest check)
    //   - confirm whether SD VDD is the on-chip LDO or a board rail + an I/O-expander
    //     enable (cf. board_power.c USB5V_EN); the P4 internal LDO may not reach 3.3V
    //   - diff init order/sdkconfig against a WORKING M5Tab5-UserDemo build
    esp_err_t err;
    sd_pwr_ctrl_ldo_config_t ldo_cfg = { .ldo_chan_id = SD_LDO_CHAN };
    if (s_pwr == NULL) {
        err = sd_pwr_ctrl_new_on_chip_ldo(&ldo_cfg, &s_pwr);
        if (err != ESP_OK) {
            ESP_LOGE(TAG, "SD LDO init failed: %s", esp_err_to_name(err));
            return err;
        }
    }

    sdmmc_host_t host = SDMMC_HOST_DEFAULT();
    host.slot = SDMMC_HOST_SLOT_0;
    host.max_freq_khz = SDMMC_FREQ_HIGHSPEED;
    host.pwr_ctrl_handle = s_pwr;

    sdmmc_slot_config_t slot = SDMMC_SLOT_CONFIG_DEFAULT();
    slot.width = 4;
    slot.clk = SD_CLK; slot.cmd = SD_CMD;
    slot.d0 = SD_D0; slot.d1 = SD_D1; slot.d2 = SD_D2; slot.d3 = SD_D3;

    esp_vfs_fat_sdmmc_mount_config_t mcfg = {
        .format_if_mount_failed = false,
        .max_files = 3,
        .allocation_unit_size = 16 * 1024,
    };
    err = esp_vfs_fat_sdmmc_mount(MOUNT_POINT, &host, &slot, &mcfg, &s_card);
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "SD mount failed: %s (no card?)", esp_err_to_name(err));
        return err;
    }
    ESP_LOGI(TAG, "SD mounted: %s %lluMB", s_card->cid.name,
             ((uint64_t)s_card->csd.capacity * s_card->csd.sector_size) >> 20);
    return ESP_OK;
}

// Open the next free /sdcard/leveler_NNN.csv and write the header.
static void open_file(void)
{
    for (int i = 0; i < 1000; i++) {
        char p[40];
        snprintf(p, sizeof(p), MOUNT_POINT "/leveler_%03d.csv", i);
        struct stat st;
        if (stat(p, &st) == 0) continue;    // exists — try next
        s_file = fopen(p, "w");
        if (s_file) {
            strlcpy(s_path, p, sizeof(s_path));
            s_rows = 0;
            fprintf(s_file, "tow_ms,lat_deg,lon_deg,height_m,fix,sv,"
                            "hacc_m,vacc_m,pitch_deg,roll_deg,heading_deg,"
                            "cutfill_m,state\n");
            fflush(s_file);
            ESP_LOGI(TAG, "logging to %s", p);
        } else {
            ESP_LOGE(TAG, "cannot open %s", p);
        }
        return;
    }
    ESP_LOGE(TAG, "too many log files");
}

static void close_file(void)
{
    if (s_file) {
        fclose(s_file);
        s_file = NULL;
        ESP_LOGI(TAG, "log closed (%lu rows)", (unsigned long)s_rows);
    }
    s_path[0] = '\0';
}

static void fnum(char *b, size_t n, double v, int dec)
{
    if (isnan(v)) b[0] = '\0';
    else snprintf(b, n, "%.*f", dec, v);
}

static void write_row(void)
{
    gnss_snapshot_t g;
    gnss_state_snapshot(&g);
    if (!g.pvt_valid || isnan(g.pvt.lat_deg)) return;   // only log real fixes

    leveler_status_t lv;
    leveler_get(&lv);

    char ha[16], va[16], pi[16], ro[16], he[16], cf[16];
    fnum(ha, sizeof ha, g.pvt.h_accuracy_m, 3);
    fnum(va, sizeof va, g.pvt.v_accuracy_m, 3);
    fnum(pi, sizeof pi, g.att_valid ? g.att.pitch_deg   : NAN, 2);
    fnum(ro, sizeof ro, g.att_valid ? g.att.roll_deg    : NAN, 2);
    fnum(he, sizeof he, g.att_valid ? g.att.heading_deg : NAN, 1);
    fnum(cf, sizeof cf, lv.have_delta ? lv.delta_m : NAN, 3);
    const char *state = !lv.have_delta ? "" :
                        lv.state > 0 ? "CUT" : lv.state < 0 ? "FILL" : "ONGRADE";

    fprintf(s_file, "%lu,%.7f,%.7f,%.3f,%s,%u,%s,%s,%s,%s,%s,%s,%s\n",
            (unsigned long)g.pvt.tow_ms, g.pvt.lat_deg, g.pvt.lon_deg,
            g.pvt.height_m, sbf_pvt_mode_str(g.pvt.mode_type), g.pvt.nr_sv,
            ha, va, pi, ro, he, cf, state);
    s_rows++;
    if ((s_rows % FLUSH_EVERY) == 0) fflush(s_file);
}

static void logger_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(WRITE_PERIOD_MS));
        if (s_want_logging && !s_file) open_file();
        if (!s_want_logging && s_file) close_file();
        if (s_want_logging && s_file)  write_row();
    }
}

bool logger_init(void)
{
    if (mount_sd() != ESP_OK) {
        s_mounted = false;
        return false;
    }
    s_mounted = true;
    s_want_logging = true;   // auto-start: insert a card and it logs

    // 4 KiB stack: fprintf + FATFS write path.
    if (xTaskCreate(logger_task, "logger", 4096, NULL, 4, NULL) != pdPASS) {
        ESP_LOGE(TAG, "logger task create failed");
        return false;
    }
    return true;
}

void logger_set(bool on)
{
    s_want_logging = on;   // the task opens/closes the file on the next tick
}

void logger_status_get(logger_status_t *out)
{
    if (!out) return;
    out->mounted = s_mounted;
    out->logging = s_want_logging && s_file != NULL;
    out->rows = s_rows;
    strlcpy(out->path, s_path, sizeof(out->path));
}
