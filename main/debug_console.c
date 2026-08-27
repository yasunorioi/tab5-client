// debug_console.c — see debug_console.h.

#include "debug_console.h"

#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <stdint.h>
#include <math.h>
#include "esp_console.h"
#include "esp_log.h"
#include "esp_timer.h"
#include "sdkconfig.h"

#include "gnss_state.h"
#include "leveler.h"
#include "usb_cdc_source.h"
#include "wifi_sta.h"
#include "display.h"
#include "backlight.h"
#include "touch.h"
#include "nmea_source.h"
#include "board_power.h"
#include "web_server.h"
#include "driver/i2c_master.h"

static const char *TAG = "console";

// ── commands ────────────────────────────────────────────────────────────────

static int cmd_stats(int argc, char **argv)
{
    (void)argc; (void)argv;
    gnss_snapshot_t g;
    gnss_state_snapshot(&g);

    int64_t age_ms = g.last_block_us ? (esp_timer_get_time() - g.last_block_us) / 1000 : -1;
    printf("sbf blocks=%lu  crc_fails=%lu\n",
           (unsigned long)g.valid_blocks, (unsigned long)g.crc_failed);
    if (age_ms < 0) {
        printf("last valid block: (none yet)\n");
    } else {
        printf("last valid block: %lld ms ago\n", (long long)age_ms);
    }
    if (g.pvt_valid) {
        printf("fix: %s  err=%s  sv=%u\n", sbf_pvt_mode_str(g.pvt.mode_type),
               sbf_pvt_error_str(g.pvt.error), g.pvt.nr_sv);
    }
    return 0;
}

static int cmd_sbf(int argc, char **argv)
{
    (void)argc; (void)argv;
    gnss_snapshot_t g;
    gnss_state_snapshot(&g);

    printf("valid_blocks=%lu  crc_fails=%lu\n",
           (unsigned long)g.valid_blocks, (unsigned long)g.crc_failed);
    if (g.pvt_valid) {
        printf("PVTGeodetic: %s  err=%s  sv=%u\n", sbf_pvt_mode_str(g.pvt.mode_type),
               sbf_pvt_error_str(g.pvt.error), g.pvt.nr_sv);
        if (!isnan(g.pvt.lat_deg))
            printf("  lat=%.7f lon=%.7f  h(ell)=%.3f m\n",
                   g.pvt.lat_deg, g.pvt.lon_deg, g.pvt.height_m);
        if (!isnan(g.pvt.h_accuracy_m))
            printf("  hAcc=%.3f  vAcc=%.3f m\n", g.pvt.h_accuracy_m, g.pvt.v_accuracy_m);
    }
    if (g.att_valid && !isnan(g.att.pitch_deg))
        printf("AttEuler: heading=%.2f pitch=%.2f roll=%.2f deg\n",
               g.att.heading_deg, g.att.pitch_deg, g.att.roll_deg);
    if (g.dop_valid && !isnan(g.dop.hdop))
        printf("DOP: hdop=%.2f vdop=%.2f pdop=%.2f\n",
               g.dop.hdop, g.dop.vdop, g.dop.pdop);
    if (g.rxstatus_valid)
        printf("RxStatus: uptime=%lus cpu=%u%% temp=%dC rx_error=0x%lX\n",
               (unsigned long)g.rxstatus.uptime_s, g.rxstatus.cpu_load_pct,
               g.rxstatus.temperature_c, (unsigned long)g.rxstatus.rx_error);
    return 0;
}

static int cmd_survey(int argc, char **argv)
{
    if (argc >= 2) {
        if (!strcmp(argv[1], "add")) {
            printf(leveler_survey_add_current() ? "point added\n" : "no usable fix yet\n");
        } else if (!strcmp(argv[1], "clear")) {
            leveler_survey_clear();
            printf("survey + plane cleared\n");
        } else if (!strcmp(argv[1], "fit")) {
            printf(leveler_fit_balance() ? "balance plane fitted (cut ~= fill)\n"
                                         : "need >= 3 survey points\n");
        } else {
            printf("usage: survey [add|clear|fit]\n");
        }
        return 0;
    }
    leveler_status_t s;
    leveler_get(&s);
    const char *mode = s.mode == LEVELER_MODE_BALANCE ? "balance"
                     : s.mode == LEVELER_MODE_FLAT    ? "flat" : "none";
    printf("survey points=%lu  origin=%d  mode=%s\n",
           (unsigned long)s.survey_points, s.has_origin, mode);
    return 0;
}

static int cmd_flat(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf(leveler_set_flat_here() ? "flat target set at current height\n"
                                   : "no usable fix yet\n");
    return 0;
}

static int cmd_cutfill(int argc, char **argv)
{
    (void)argc; (void)argv;
    leveler_status_t s;
    leveler_get(&s);
    if (!s.have_delta) {
        printf(s.mode == LEVELER_MODE_NONE ? "no target plane — run 'survey fit' or 'flat'\n"
                                           : "no usable fix right now\n");
        return 0;
    }
    const char *st = s.state > 0 ? "CUT" : s.state < 0 ? "FILL" : "ON GRADE";
    printf("%s  delta=%+.3f m (%+.0f cm)\n", st, s.delta_m, s.delta_m * 100.0);
    return 0;
}

static int cmd_usb(int argc, char **argv)
{
    (void)argc; (void)argv;
    usb_cdc_status_t u;
    usb_cdc_source_status(&u);
    printf("host_installed=%d  device_attached=%d  cdc_open=%d\n",
           u.host_installed, u.device_attached, u.cdc_open);
    if (u.device_attached) {
        printf("attached VID=0x%04X PID=0x%04X\n", u.vid, u.pid);
        printf("topology: n=%u if[%s]\n", u.num_interfaces, u.topo);
        printf("cur_itf=%d  stream_itf=%d\n", (int8_t)u.cur_itf, (int8_t)u.stream_itf);
    }
    return 0;
}

static int cmd_mosaic(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: mosaic <septentrio command>\n");
        printf("  e.g. mosaic \\r\\n          (bare prompt -> reveals the port Cd)\n");
        printf("       mosaic setSBFOutput, Stream1, USB1, PVTGeodetic, msec100\n");
        return 0;
    }
    // Rejoin argv[1..] with single spaces — the console splits on whitespace,
    // but Septentrio commands carry spaces (e.g. "setSBFOutput, USB1, ...").
    char cmd[224];
    size_t pos = 0;
    for (int i = 1; i < argc && pos < sizeof(cmd) - 1; i++) {
        int w = snprintf(cmd + pos, sizeof(cmd) - pos, "%s%s",
                         i > 1 ? " " : "", argv[i]);
        if (w > 0) pos += (size_t)w;
    }

    char reply[1024];
    size_t n = 0;
    esp_err_t err = usb_cdc_send_command(cmd, reply, sizeof(reply), &n, 1500);
    if (err == ESP_ERR_INVALID_STATE) {
        printf("no Mosaic CDC interface open — check `usb`\n");
        return 0;
    }
    if (err != ESP_OK) {
        printf("command TX failed: %s\n", esp_err_to_name(err));
        return 0;
    }
    // Non-printables → '.', so a Septentrio `$R:` reply stays legible even when
    // SBF binary from a streaming port is teed into the capture alongside it.
    printf("--- reply (%u B) ---\n", (unsigned)n);
    for (size_t i = 0; i < n; i++) {
        char c = reply[i];
        putchar((c == '\r' || c == '\n' || (c >= 0x20 && c < 0x7f)) ? c : '.');
    }
    printf("\n--- end ---\n");
    return 0;
}

static int cmd_i2cscan(int argc, char **argv)
{
    (void)argc; (void)argv;
    i2c_master_bus_handle_t bus = board_i2c_bus();
    if (!bus) { printf("board I2C bus not up\n"); return 0; }
    printf("scanning Tab5 system I2C (SDA31/SCL32):\n");
    int found = 0;
    for (uint8_t a = 0x08; a <= 0x77; a++) {
        if (i2c_master_probe(bus, a, 50) == ESP_OK) {
            const char *hint = "";
            switch (a) {
            case 0x43: hint = " (PI4IOE #1 — LCD/TP/CAM reset)"; break;
            case 0x44: hint = " (PI4IOE #2 — USB5V/WLAN)"; break;
            case 0x5D: case 0x14: hint = " (GT911 touch — ILI9881C rev?)"; break;
            case 0x48: hint = " (touch/other)"; break;
            default: break;
            }
            printf("  0x%02X%s\n", a, hint);
            found++;
        }
    }
    printf("%d device(s)\n", found);
    return 0;
}

static int cmd_touch(int argc, char **argv)
{
    (void)argc; (void)argv;
    if (!touch_present()) {
        printf("no touch controller (GT911 rev, or not up)\n");
        return 0;
    }
    bool valid = false; int x = 0, y = 0;
    esp_err_t err = touch_read_point(&valid, &x, &y);
    if (err != ESP_OK) {
        printf("touch read error: %s\n", esp_err_to_name(err));
        return 1;
    }
    int64_t idle_ms = (esp_timer_get_time() - touch_last_activity_us()) / 1000;
    printf("touch: %s", valid ? "DOWN" : "up");
    if (valid) printf("  x=%d y=%d", x, y);
    printf("   last activity %lld ms ago\n", (long long)idle_ms);
    return 0;
}

static int cmd_nmea(int argc, char **argv)
{
    (void)argc; (void)argv;
    nmea_status_t s;
    nmea_source_status(&s);
    printf("nmea itf2=%s  %lu B  GGA=%lu GSV=%lu  sats=%u\n",
           s.itf_open ? "open" : "closed", (unsigned long)s.bytes,
           (unsigned long)s.gga_sentences, (unsigned long)s.gsv_sentences,
           s.sat_count);
    if (s.gga_sentences)
        printf("  GGA: lat=%.6f lon=%.6f fixq=%d sats=%d\n",
               s.lat, s.lon, s.fix_quality, s.gga_sats);
    for (uint8_t i = 0; i < s.sat_count; i++) {
        printf("  %c%02u  el=%3d az=%3d cn0=%2d\n",
               s.sats[i].talker, s.sats[i].prn,
               s.sats[i].elev, s.sats[i].azim, s.sats[i].cn0);
    }
    if (s.last_line[0]) printf("  last: %s\n", s.last_line);
    return 0;
}

static int cmd_disp(int argc, char **argv)
{
    // disp [red|green|blue|white|black|<hex RGB565>] [backlight%]
    uint16_t c = 0xFFFF;
    if (argc >= 2) {
        if      (!strcmp(argv[1], "red"))   c = 0xF800;
        else if (!strcmp(argv[1], "green")) c = 0x07E0;
        else if (!strcmp(argv[1], "blue"))  c = 0x001F;
        else if (!strcmp(argv[1], "white")) c = 0xFFFF;
        else if (!strcmp(argv[1], "black")) c = 0x0000;
        else c = (uint16_t)strtol(argv[1], NULL, 16);
    }
    int bl = (argc >= 3) ? atoi(argv[2]) : 100;
    display_fill(c);
    backlight_manual(bl);   // set + suspend the adaptive controller (debug)
    printf("filled 0x%04X, backlight %d%%\n", c, bl);
    return 0;
}

static int cmd_wifiset(int argc, char **argv)
{
    if (argc < 2) {
        printf("usage: wifiset <ssid> [password]\n");
        return 0;
    }
    printf("saving WiFi credentials and rebooting to connect...\n");
    wifi_set_creds(argv[1], argc >= 3 ? argv[2] : "");  // does not return (reboots)
    return 0;
}

static int cmd_wifireset(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("erasing WiFi credentials and rebooting...\n");
    wifi_forget();  // does not return (reboots)
    return 0;
}

static int cmd_webadmin(int argc, char **argv)
{
    if (argc < 3) {
        printf("usage: webadmin <user> <pass>\n");
        printf("  set HTTP Basic-auth creds for the /admin config page + writes\n");
        return 0;
    }
    web_admin_set_creds(argv[1], argv[2]);
    printf("admin creds saved (user=%s) — /admin writes enabled\n", argv[1]);
    return 0;
}

static int cmd_webadminreset(int argc, char **argv)
{
    (void)argc; (void)argv;
    web_admin_forget();
    printf("admin creds erased — /admin writes disabled (503)\n");
    return 0;
}

static int cmd_wifidrop(int argc, char **argv)
{
    (void)argc; (void)argv;
    printf("forcing WiFi disconnect — watch for reconnect + GOT_IP\n");
    wifi_sta_drop();
    return 0;
}

static void register_cmds(void)
{
    const esp_console_cmd_t cmds[] = {
        { .command = "stats", .help = "SBF block/CRC counters + fix + staleness", .func = cmd_stats },
        { .command = "sbf",   .help = "decoded SBF: PVT/Att/DOP/RxStatus latest values", .func = cmd_sbf },
        { .command = "survey", .help = "cut/fill survey: survey [add|clear|fit] (no arg = status)", .hint = "[add|clear|fit]", .func = cmd_survey },
        { .command = "flat",  .help = "set a flat cut/fill target at the current height", .func = cmd_flat },
        { .command = "cutfill", .help = "current cut/fill delta vs the active plane", .func = cmd_cutfill },
        { .command = "usb",   .help = "USB host / CDC attach state",          .func = cmd_usb   },
        { .command = "mosaic", .help = "send a raw Septentrio command to the Mosaic + print reply", .hint = "<command>", .func = cmd_mosaic },
        { .command = "disp", .help = "fill the panel with a color (light-up test): disp <red|green|blue|white|black|hex> [bl%]", .hint = "[color] [bl%]", .func = cmd_disp },
        { .command = "touch", .help = "read the touch point + idle time (idle-off test)", .func = cmd_touch },
        { .command = "nmea", .help = "itf2 NMEA state: GGA/GSV counts + per-sat el/az/cn0", .func = cmd_nmea },
        { .command = "i2cscan", .help = "probe the Tab5 system I2C bus (identify touch IC -> panel rev)", .func = cmd_i2cscan },
        { .command = "wifiset", .help = "set WiFi creds + reboot to join: wifiset <ssid> [pass]", .hint = "<ssid> [pass]", .func = cmd_wifiset },
        { .command = "wifireset", .help = "erase stored WiFi creds + reboot", .func = cmd_wifireset },
        { .command = "wifidrop", .help = "force a STA disconnect (reconnect test)", .func = cmd_wifidrop },
        { .command = "webadmin", .help = "set /admin Basic-auth creds: webadmin <user> <pass>", .hint = "<user> <pass>", .func = cmd_webadmin },
        { .command = "webadminreset", .help = "erase /admin creds (disables writes)", .func = cmd_webadminreset },
    };
    for (size_t i = 0; i < sizeof(cmds) / sizeof(cmds[0]); i++) {
        ESP_ERROR_CHECK(esp_console_cmd_register(&cmds[i]));
    }
}

void debug_console_start(void)
{
    esp_console_repl_t *repl = NULL;
    esp_console_repl_config_t repl_config = ESP_CONSOLE_REPL_CONFIG_DEFAULT();
    repl_config.prompt = "tab5>";
    repl_config.max_cmdline_length = 256;   // Septentrio cmd lines get long

    esp_console_register_help_command();
    register_cmds();

    // Pick the REPL transport to match the configured console device so it works
    // whether the Tab5 exposes UART or USB-Serial-JTAG for monitor.
#if defined(CONFIG_ESP_CONSOLE_USB_SERIAL_JTAG)
    esp_console_dev_usb_serial_jtag_config_t hw = ESP_CONSOLE_DEV_USB_SERIAL_JTAG_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_serial_jtag(&hw, &repl_config, &repl));
#elif defined(CONFIG_ESP_CONSOLE_USB_CDC)
    esp_console_dev_usb_cdc_config_t hw = ESP_CONSOLE_DEV_USB_CDC_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_usb_cdc(&hw, &repl_config, &repl));
#else
    esp_console_dev_uart_config_t hw = ESP_CONSOLE_DEV_UART_CONFIG_DEFAULT();
    ESP_ERROR_CHECK(esp_console_new_repl_uart(&hw, &repl_config, &repl));
#endif

    ESP_ERROR_CHECK(esp_console_start_repl(repl));
    ESP_LOGI(TAG, "debug console ready (type 'help')");
}
