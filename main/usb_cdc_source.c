// usb_cdc_source.c — USB Host CDC-ACM driver skeleton. See usb_cdc_source.h.
//
// Compiles clean for esp32p4 on ESP-IDF 5.4.4 with usb_host_cdc_acm 2.4.0.
// Runtime is UNTESTED — no hardware yet. Marked TODO(hw) where a real device is
// needed (VID/PID/interface, HS-OTG port + VBUS).
//
// Data flow:
//   USB HS OTG  ->  cdc_acm_host data_cb  ->  gnss_state_feed()  ->  SBF parser
//
// Bring-up order (see README milestones):
//   M0: device attaches, new_dev_cb logs VID/PID + every interface descriptor.
//       Fill in MOSAIC_VID / MOSAIC_PID / MOSAIC_CDC_ITF from that log.
//   M1: data_cb fires; the SBF parser latches $@ sync + CRC-valid blocks.

#include "usb_cdc_source.h"

#include "freertos/FreeRTOS.h"
#include "freertos/task.h"
#include "freertos/semphr.h"
#include "freertos/stream_buffer.h"
#include "esp_log.h"
#include <string.h>
#include <stdio.h>
#include "usb/usb_host.h"
#include "usb/usb_helpers.h"     // usb_parse_next_descriptor_of_type
#include "usb/cdc_acm_host.h"

#include "gnss_state.h"     // client sink: USB bytes -> SBF parser (valid-block gate)
#include "mosaic_config.h"  // push SBF output config to the receiver on attach
#include "mosaic_usb.h"     // MOSAIC_VID / MOSAIC_PID (shared with nmea_source.c)

static const char *TAG = "usb_cdc";

// Shared status for the `usb` console command. Plain writes from the driver
// tasks / callbacks; reads are advisory (no lock needed for a status snapshot).
static volatile usb_cdc_status_t s_status;

void usb_cdc_source_status(usb_cdc_status_t *out)
{
    if (out) *out = s_status;
}

// ── Device identity ─────────────────────────────────────────────────────────
// Client receiver (docs/hardware-findings.md): mosaic-go G5 P3H enumerates as
// PID 0x8231 with only TWO CDC-ACM COMs and no mass-storage interface:
//   itf 0/1 = USB1 (SBF stream, provisioned by mosaic_provision)
//   itf 2/3 = USB2 (NMEA GGA+GSV for the on-panel GNSS view; opened by nmea_source)
// Unlike the caster's bench unit, itf0 DOES answer commands here. Which COM
// carries the SBF is still a config choice, so we SWEEP the comm interfaces and
// latch the one that produces CRC-valid SBF blocks (see cdc_task).
// MOSAIC_VID / MOSAIC_PID come from mosaic_usb.h (shared with nmea_source.c).

// bInterfaceNumber of the CDC *communications* interface carrying SBF. On this
// P3H that is USB1 = itf0, and ONLY itf0 — itf2 (USB2) is the NMEA port that
// nmea_source opens for the skyplot. The old sweep also tried itf2, so whenever
// itf0 went quiet the sweep hopped onto itf2 and collided with nmea_source's
// claim ("EP already allocated"), wedging the USB host. SBF lives on itf0, so we
// stay there and just re-provision if it goes quiet.
static const uint8_t MOSAIC_COM_ITFS[] = {0};
#define MOSAIC_SWEEP_DWELL_MS  6000   // wait this long for bytes before hopping
#define MOSAIC_OPEN_RETRIES    3      // open retries before hopping off an un-openable itf

// USB CDC ignores the real line rate, but the Mosaic virtual COM may honor it.
// SBF @10Hz (~1.7 kB/s) is light; keep margin.
#define MOSAIC_BAUD       460800

static void usb_lib_task(void *arg)
{
    const usb_host_config_t host_config = {
        .skip_phy_setup = false,
        .intr_flags = ESP_INTR_FLAG_LEVEL1,
    };
    ESP_ERROR_CHECK(usb_host_install(&host_config));
    ESP_LOGI(TAG, "usb_host installed");
    s_status.host_installed = true;
    xTaskNotifyGive((TaskHandle_t)arg);   // tell starter the host stack is up

    while (1) {
        uint32_t event_flags;
        usb_host_lib_handle_events(portMAX_DELAY, &event_flags);
        if (event_flags & USB_HOST_LIB_EVENT_FLAGS_NO_CLIENTS) {
            ESP_LOGI(TAG, "no more clients, freeing devices");
            usb_host_device_free_all();
        }
    }
}

// Fired by the CDC driver when a new CDC-ACM device shows up. Milestone-0: log
// everything so we can fill in VID/PID/interface. `cdc_acm_host_desc_print`
// dumps the full descriptor tree (composite interfaces included).
// Walk the active config descriptor's interfaces and record a compact summary
// (bInterfaceNumber:class/subclass per alt-0 interface) into s_status.topo, so
// the composite layout is readable from the periodic line — no need to catch
// the one-shot cdc_acm_host_desc_print(). This is what tells us which itf index
// is the RTCM3-streaming CDC-Data port vs the ECM/notification interfaces.
//
// Reference classes on the Mosaic composite:
//   02/02 = CDC-Comm (ACM control)      0a/00 = CDC-Data (the byte stream)
//   02/06 = CDC-Comm (ECM control)      02/ff = RNDIS-ish vendor control
static void log_topology(usb_device_handle_t usb_dev)
{
    const usb_config_desc_t *cfg;
    if (usb_host_get_active_config_descriptor(usb_dev, &cfg) != ESP_OK) {
        return;
    }
    char tmp[USB_TOPO_MAX];
    size_t pos = 0;
    uint8_t n = 0;
    tmp[0] = '\0';

    int offset = 0;
    const usb_standard_desc_t *d = (const usb_standard_desc_t *)cfg;
    while ((d = usb_parse_next_descriptor_of_type(
                    d, cfg->wTotalLength,
                    USB_B_DESCRIPTOR_TYPE_INTERFACE, &offset)) != NULL) {
        const usb_intf_desc_t *itf = (const usb_intf_desc_t *)d;
        ESP_LOGI(TAG, "  itf %u alt %u: class=0x%02X sub=0x%02X proto=0x%02X eps=%u",
                 itf->bInterfaceNumber, itf->bAlternateSetting,
                 itf->bInterfaceClass, itf->bInterfaceSubClass,
                 itf->bInterfaceProtocol, itf->bNumEndpoints);
        // Only alt-0 in the compact summary; alt settings just add noise there.
        if (itf->bAlternateSetting == 0 && pos + 12 < sizeof(tmp)) {
            int w = snprintf(tmp + pos, sizeof(tmp) - pos, "%s%u:%02x/%02x",
                             n ? " " : "", itf->bInterfaceNumber,
                             itf->bInterfaceClass, itf->bInterfaceSubClass);
            if (w > 0) {
                pos += (size_t)w;
                n++;
            }
        }
    }
    s_status.num_interfaces = n;
    strlcpy((char *)s_status.topo, tmp, sizeof(s_status.topo));
    ESP_LOGI(TAG, "topology: n=%u if[%s]", n, tmp);
}

static void new_dev_cb(usb_device_handle_t usb_dev)
{
    const usb_device_desc_t *desc;
    if (usb_host_get_device_descriptor(usb_dev, &desc) == ESP_OK) {
        ESP_LOGI(TAG, "CDC device attached: VID=0x%04X PID=0x%04X",
                 desc->idVendor, desc->idProduct);
        s_status.device_attached = true;
        s_status.vid = desc->idVendor;
        s_status.pid = desc->idProduct;
    }
    log_topology(usb_dev);
}

static SemaphoreHandle_t s_disconnected;

// The handle of the currently-open CDC interface, for command TX. NULL when no
// interface is open. Set on open, cleared on close/disconnect.
static volatile cdc_acm_dev_hdl_t s_cdc;

// Set once we've pushed the RTCM3 output config to the attached Mosaic; reset on
// disconnect so a freshly-plugged receiver gets provisioned again.
static volatile bool s_provisioned;

static void cdc_event_cb(const cdc_acm_host_dev_event_data_t *event, void *user_ctx)
{
    switch (event->type) {
    case CDC_ACM_HOST_ERROR:
        ESP_LOGE(TAG, "CDC error %d", event->data.error);
        break;
    case CDC_ACM_HOST_DEVICE_DISCONNECTED:
        ESP_LOGW(TAG, "Mosaic disconnected");
        // Close on the driver's terms and let the open loop retry.
        s_cdc = NULL;
        s_provisioned = false;   // re-provision whatever gets plugged in next
        cdc_acm_host_close(event->data.cdc_hdl);
        s_status.cdc_open = false;
        xSemaphoreGive(s_disconnected);
        break;
    case CDC_ACM_HOST_SERIAL_STATE:
        ESP_LOGD(TAG, "serial state 0x%04X", event->data.serial_state.val);
        break;
    default:
        break;
    }
}

// Bytes seen since the current interface was opened — the sweep uses this to
// decide whether the bound COM is the one actually streaming SBF.
static volatile uint32_t s_rx_since_open;

// Command-reply capture: while s_cmd_capture is set, cdc_data_cb tees a COPY of
// every received byte into s_cmd_rx so usb_cdc_send_command() can read the
// Mosaic's ASCII reply. The bytes still go to the SBF parser as usual, so
// capturing never interrupts a live stream.
static StreamBufferHandle_t s_cmd_rx;
static volatile bool        s_cmd_capture;

// The hot path. Runs in the CDC driver task context (NOT an ISR), so a
// non-blocking StreamBuffer send is safe. Return true = we consumed the data.
static bool cdc_data_cb(const uint8_t *data, size_t data_len, void *user_arg)
{
    s_rx_since_open += data_len;
    if (s_cmd_capture && s_cmd_rx) {
        xStreamBufferSend(s_cmd_rx, data, data_len, 0);   // tee a copy; drop on full
    }
    gnss_state_feed(data, (uint32_t)data_len);   // USB bytes -> SBF parser
    return true;
}

esp_err_t usb_cdc_write(const uint8_t *data, size_t len, uint32_t timeout_ms)
{
    cdc_acm_dev_hdl_t cdc = s_cdc;
    if (cdc == NULL || data == NULL) return ESP_ERR_INVALID_STATE;
    // Raw pass-through to the receiver (RTCM3 corrections from the NTRIP client).
    // The Mosaic auto-detects RTCM3 input on any port; no framing needed here.
    return cdc_acm_host_data_tx_blocking(cdc, data, len, timeout_ms);
}

esp_err_t usb_cdc_send_command(const char *cmd, char *reply, size_t reply_max,
                               size_t *reply_len, uint32_t timeout_ms)
{
    if (reply_len) *reply_len = 0;
    cdc_acm_dev_hdl_t cdc = s_cdc;
    if (cdc == NULL || cmd == NULL) return ESP_ERR_INVALID_STATE;

    if (s_cmd_rx == NULL) {
        s_cmd_rx = xStreamBufferCreate(2048, 1);
        if (s_cmd_rx == NULL) return ESP_ERR_NO_MEM;
    }
    xStreamBufferReset(s_cmd_rx);
    s_cmd_capture = true;

    esp_err_t err = cdc_acm_host_data_tx_blocking(cdc, (const uint8_t *)cmd,
                                                  strlen(cmd), timeout_ms);
    if (err == ESP_OK) {
        static const uint8_t crlf[2] = {'\r', '\n'};
        err = cdc_acm_host_data_tx_blocking(cdc, crlf, sizeof(crlf), timeout_ms);
    }

    // Drain the reply: wait up to timeout_ms for the FIRST byte (a command port
    // may be slow to answer), then read until a ~300 ms idle gap (reply complete
    // on a silent port) or the buffer fills (a port also streaming RTCM3 never
    // idles). Zero bytes after the full wait ⇒ this port isn't answering commands.
    size_t got = 0;
    if (reply && reply_max > 0) {
        bool first = true;
        while (got < reply_max - 1) {
            TickType_t wait = pdMS_TO_TICKS(first ? timeout_ms : 300);
            size_t n = xStreamBufferReceive(s_cmd_rx, (uint8_t *)reply + got,
                                            reply_max - 1 - got, wait);
            if (n == 0) break;
            got += n;
            first = false;
        }
        reply[got] = '\0';
    }
    if (reply_len) *reply_len = got;
    s_cmd_capture = false;
    return err;
}

static void cdc_task(void *arg)
{
    ESP_ERROR_CHECK(cdc_acm_host_install(NULL));
    ESP_LOGI(TAG, "cdc_acm_host installed");

    // Register the composite new-device logger (milestone-0 discovery) only AFTER
    // cdc_acm_host_install() has allocated the driver object — the register call
    // dereferences that object, so doing it earlier is a NULL store fault.
    cdc_acm_host_register_new_dev_callback(new_dev_cb);

    const cdc_acm_host_device_config_t dev_cfg = {
        .connection_timeout_ms = 5000,
        .out_buffer_size = 512,       // we mostly RX; small TX for config cmds
        .in_buffer_size = 2048,       // TODO(ver): field name/existence varies
        .event_cb = cdc_event_cb,
        .data_cb = cdc_data_cb,
        .user_arg = NULL,
    };

    const cdc_acm_line_coding_t lc = {
        .dwDTERate = MOSAIC_BAUD,
        .bCharFormat = 0,   // 1 stop bit
        .bParityType = 0,   // none
        .bDataBits = 8,
    };

    size_t sweep = 0;      // index into MOSAIC_COM_ITFS
    int open_fails = 0;    // consecutive open failures on the CURRENT sweep itf
    while (1) {
        uint8_t itf = MOSAIC_COM_ITFS[sweep];
        cdc_acm_dev_hdl_t cdc = NULL;
        ESP_LOGI(TAG, "opening Mosaic COM itf=%d (VID=0x%04X PID=0x%04X)...",
                 itf, MOSAIC_VID, MOSAIC_PID);
        esp_err_t err = cdc_acm_host_open(MOSAIC_VID, MOSAIC_PID, itf, &dev_cfg, &cdc);
        if (err != ESP_OK) {
            // Two causes look identical here: the device is still booting (a
            // retry on the SAME itf will succeed shortly), or this itf index
            // simply doesn't exist on this individual (retrying forever never
            // succeeds — e.g. a unit that enumerates only itf {0,2}, not
            // {0,2,4}). Retry a few times for the boot case, then HOP so we can
            // never wedge on a phantom itf and starve the itf that actually
            // carries RTCM3.
            vTaskDelay(pdMS_TO_TICKS(1000));
            if (++open_fails >= MOSAIC_OPEN_RETRIES) {
                ESP_LOGW(TAG, "itf=%d not openable after %d tries — hopping",
                         itf, open_fails);
                open_fails = 0;
                sweep = (sweep + 1) % (sizeof(MOSAIC_COM_ITFS) / sizeof(MOSAIC_COM_ITFS[0]));
            }
            continue;
        }
        open_fails = 0;   // opened OK — reset the per-itf failure counter
        ESP_LOGI(TAG, "Mosaic CDC opened on itf=%d", itf);
        s_cdc = cdc;
        s_status.cdc_open = true;
        s_status.cur_itf = itf;
        s_rx_since_open = 0;

        // Best-effort: some virtual COMs reject SET_LINE_CODING; ignore errors.
        cdc_acm_host_line_coding_set(cdc, &lc);
        cdc_acm_host_set_control_line_state(cdc, /*dtr=*/true, /*rts=*/true);

        // Self-provision the receiver's SBF output (flash-and-go). Sent once per
        // attach on the first interface we open; the command targets USB1 by name,
        // so it applies no matter which COM this is — and on a factory-config
        // Mosaic it's what makes USB1 start streaming SBF for the sweep to latch.
        bool provisioned_here = false;
        if (!s_provisioned) {
            if (mosaic_provision() == ESP_OK) {
                s_provisioned = true;
                provisioned_here = true;
            } else {
                ESP_LOGW(TAG, "provision failed; falling back to Mosaic saved config");
            }
        }

        // Dwell: latch on VALID SBF, not just any bytes. The other Mosaic COM
        // streams NMEA, which would otherwise hijack the sweep. Watch the SBF
        // parser's CRC-valid block count for an advance; raw bytes alone
        // (NMEA/echo) don't qualify. If nothing valid arrives, hop. When we JUST
        // provisioned on this interface, dwell longer: the receiver takes a few
        // seconds to (re)start output after setSBFOutput, and this is very likely
        // the data port (the ack came from it) — hopping away only to circle back
        // wastes a full sweep before first latch.
        int dwell_ms = provisioned_here ? 15000 : MOSAIC_SWEEP_DWELL_MS;
        uint32_t base_blocks = gnss_state_valid_blocks();

        bool disconnected = false;
        bool streaming = false;
        for (int waited = 0; waited < dwell_ms; waited += 250) {
            if (xSemaphoreTake(s_disconnected, pdMS_TO_TICKS(250)) == pdTRUE) {
                disconnected = true;   // handle already closed by cdc_event_cb
                break;
            }
            if (gnss_state_valid_blocks() > base_blocks) { streaming = true; break; }
        }

        if (disconnected) {
            ESP_LOGW(TAG, "Mosaic disconnected (itf=%d), rescanning from itf 0", itf);
            s_status.cur_itf = 0xFF;
            sweep = 0;
            continue;
        }
        if (streaming) {
            s_status.stream_itf = itf;
            ESP_LOGI(TAG, "*** valid SBF on itf=%d (%lu raw B seen) — latched ***",
                     itf, (unsigned long)s_rx_since_open);
            xSemaphoreTake(s_disconnected, portMAX_DELAY);   // stay until unplug
            ESP_LOGW(TAG, "stream itf=%d disconnected, rescanning", itf);
            s_status.cur_itf = 0xFF;
            sweep = 0;
            continue;
        }

        // No valid SBF in the dwell window. The raw-byte count distinguishes a
        // truly silent COM (0 B) from non-SBF chatter like NMEA (>0 B). Hop.
        ESP_LOGW(TAG, "itf=%d no SBF in %d ms (%lu raw B) — hopping",
                 itf, MOSAIC_SWEEP_DWELL_MS, (unsigned long)s_rx_since_open);
        s_cdc = NULL;
        cdc_acm_host_close(cdc);
        s_status.cdc_open = false;
        s_status.cur_itf = 0xFF;
        sweep = (sweep + 1) % (sizeof(MOSAIC_COM_ITFS) / sizeof(MOSAIC_COM_ITFS[0]));
    }
}

esp_err_t usb_cdc_source_start(void)
{
    s_status.cur_itf = 0xFF;
    s_status.stream_itf = 0xFF;

    s_disconnected = xSemaphoreCreateBinary();
    if (s_disconnected == NULL) {
        return ESP_ERR_NO_MEM;
    }

    TaskHandle_t self = xTaskGetCurrentTaskHandle();
    // Host lib task: pinned, generous stack (USB host is stack-hungry).
    if (xTaskCreatePinnedToCore(usb_lib_task, "usb_lib", 4096, self, 5, NULL, 0) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    // Wait until usb_host_install() completed before installing the class driver.
    ulTaskNotifyTake(pdTRUE, portMAX_DELAY);

    // new_dev_cb is registered inside cdc_task, right after cdc_acm_host_install()
    // — it must not run before the driver object exists.
    if (xTaskCreate(cdc_task, "cdc", 4096, NULL, 5, NULL) != pdPASS) {
        return ESP_ERR_NO_MEM;
    }
    return ESP_OK;
}
