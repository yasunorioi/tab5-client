// app_main.c — tab5-client entry point.
//
// Data path: USB CDC source -> gnss_state (SBF parser) -> cut/fill + panel UI.
// A debug console lets you inspect what the Mosaic is actually sending. The
// caster half of the parent project (rtcm_sink/monitor/upstream/ntripcaster) has
// been removed — this box consumes the receiver's SBF, it doesn't serve RTCM3.

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "esp_log.h"
#include "esp_err.h"
#include "esp_event.h"
#include "esp_netif.h"

#include "gnss_state.h"
#include "leveler.h"
#include "logger.h"
#include "usb_cdc_source.h"
#include "nmea_source.h"
#include "debug_console.h"
#include "net_mdns.h"
#include "board_power.h"
#include "wifi_sta.h"
#include "display.h"
#include "backlight.h"
#include "touch.h"
#include "status_screen.h"
#include "web_server.h"

static const char *TAG = "tab5-client";

#define ADMIN_PORT   WEB_ADMIN_PORT // status web UI (served by web_server.c)

// Periodic liveness line so `idf.py monitor` shows activity without needing to
// type console commands. Reads the SBF-derived state; there is no byte drain
// here — the USB driver callback feeds gnss_state directly.
static void status_log_task(void *arg)
{
    (void)arg;
    while (1) {
        vTaskDelay(pdMS_TO_TICKS(5000));
        gnss_snapshot_t g;
        gnss_state_snapshot(&g);
        usb_cdc_status_t u;
        usb_cdc_source_status(&u);
        // Fold USB + SBF state into one line so the box is diagnosable from any
        // capture window without needing to catch the boot log (the USB-Serial-
        // JTAG console drops one-shot events).
        const char *mode = g.pvt_valid ? sbf_pvt_mode_str(g.pvt.mode_type) : "-";
        ESP_LOGI(TAG, "usb[host=%d attach=%d open=%d %04X:%04X n=%u if[%s] "
                 "cur=%d stream=%d] sbf: %lu blk, %lu CRCfail  fix=%s sv=%u",
                 u.host_installed, u.device_attached, u.cdc_open, u.vid, u.pid,
                 u.num_interfaces, u.topo, (int8_t)u.cur_itf, (int8_t)u.stream_itf,
                 (unsigned long)g.valid_blocks, (unsigned long)g.crc_failed,
                 mode, g.pvt_valid ? g.pvt.nr_sv : 0);
    }
}

void app_main(void)
{
    ESP_LOGI(TAG, "tab5-client boot (ESP32-P4 / Mosaic USB CDC -> SBF -> cut/fill)");

    // Console FIRST. This is a box whose fallback operator interface is the USB-C
    // serial/JTAG console — the REPL must come up before anything that can block
    // or abort. usb_cdc_source_start() blocks app_main until usb_host_install
    // returns (and aborts on failure); if the HS-OTG PHY/VBUS isn't up on real
    // hardware, starting the console after it would leave the box mute. The REPL
    // runs on its own task, so it survives even if USB host bring-up hangs below.
    gnss_state_init();          // SBF parse target for the USB source (client data path)
    leveler_init();             // cut/fill survey + balance plane (driven from PVT)
    debug_console_start();

    // Bring up the TCP/IP stack (tcpip thread + lwIP core) and the default event
    // loop. No actual netif (WiFi/Ethernet) is attached yet — that's TODO(hw) —
    // but this makes lwIP sockets usable, so the web server can bind and mdns can
    // init. Without it, lwip_socket()/mdns_init() have no stack to run on.
    ESP_ERROR_CHECK(esp_netif_init());
    ESP_ERROR_CHECK(esp_event_loop_create_default());

    xTaskCreate(status_log_task, "status_log", 4096, NULL, 4, NULL);

    // Bring up the USB->SBF core BEFORE WiFi. The leveler's core data path
    // (Mosaic USB -> SBF -> panel) must never be gated behind the C6 WiFi
    // coprocessor. esp_wifi_init() blocks (retrying the ESP-Hosted SDIO
    // transport) whenever the C6 doesn't come up after reset; that used to hang
    // app_main before USB ever installed. WiFi now runs async + last.

    // Enable USB-A VBUS BEFORE the host installs. On Tab5 the USB-A 5V rail is
    // gated by an I/O expander (PI4IOE #2, P3 = USB5V_EN); without this a device
    // is only trickle-powered and never enumerates on the P4 HS OTG. Non-fatal
    // if it fails — log and press on so the console/USB host still come up for
    // diagnosis (an unpowered port just means "waiting for Mosaic" forever).
    esp_err_t pwr = board_power_init();
    if (pwr != ESP_OK) {
        ESP_LOGE(TAG, "board_power_init failed (%s) — USB-A VBUS may be off",
                 esp_err_to_name(pwr));
    }

    // Bring up the MIPI-DSI status panel. De-gated like WiFi: a dead/absent panel
    // returns an error here instead of aborting, so the USB->caster core still
    // runs headless. Uses the board I2C bus (LCD-reset expander), so it must come
    // after board_power_init(). A blue fill confirms the panel is alive before
    // the LVGL status UI is wired.
    esp_err_t disp = display_init();
    if (disp != ESP_OK) {
        ESP_LOGE(TAG, "display_init failed (%s) — running headless", esp_err_to_name(disp));
    } else {
        // Pre-fill the panel with the UI's background colour (RGB565 of C_BG =
        // 0x081420). LVGL only paints down to its content, so the area below stays
        // this fill — matching it makes the whole screen a seamless background.
        display_fill(0x08A4);
        // Touch activity source for the idle backlight-off (needs the panel's
        // TP_RST released by display_init). Absent on the GT911 rev — then
        // idle-off just stays disabled.
        touch_init();
        // Adaptive backlight: hold a safe floor until RTCM3 is streaming, then
        // climb toward target only as the supply proves it can sustain it. A
        // fixed 80% here browns out the Mosaic on a thin supply (M3-B finding).
        // Also blanks the panel after an idle spell (wakes on touch).
        backlight_auto_start();
        // LVGL status page (de-gated: a UI fault must not stall the caster).
        esp_err_t ui = status_screen_start();
        if (ui != ESP_OK) {
            ESP_LOGE(TAG, "status_screen_start failed (%s)", esp_err_to_name(ui));
        }
    }

    // Install the USB host + CDC-ACM source. Blocks until usb_host_install()
    // returns; on real hardware this succeeds (host=1). Kept ahead of WiFi so a
    // dead C6 can never starve the box of its byte source.
    ESP_ERROR_CHECK(usb_cdc_source_start());

    // Read-only NMEA (GGA+GSV) on the Mosaic's second COM (itf2) for the panel's
    // GNSS view — independent of the SBF data path on itf0.
    nmea_source_start();

    // microSD CSV work logger — de-gated: a missing/failed card is logged, never
    // fatal. Auto-starts logging if a card mounts.
    if (!logger_init()) {
        ESP_LOGW(TAG, "SD logger unavailable (no card / mount failed)");
    }

    // Advertise as rtk.local for zero-config field discovery of the status UI.
    // NOTE(hw): needs a netif (WiFi via C6/ESP-Hosted or Ethernet) to be
    // reachable. Starting it now is harmless; it becomes discoverable the moment
    // a netif comes up.
    // TODO(product): per-unit unique hostname (e.g. rtk-<serial>) to avoid
    // rtk.local collisions when several boxes share a field LAN.
    net_mdns_start("rtk", ADMIN_PORT);

    // Join the field WiFi via the onboard C6 (ESP-Hosted/SDIO) — LAST, and on
    // its OWN task (see wifi_sta.c): a flaky C6 costs us WiFi, not the box.
    // On GOT_IP it starts the status web UI.
    wifi_sta_start();
}
