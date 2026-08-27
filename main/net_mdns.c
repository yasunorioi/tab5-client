// net_mdns.c — see net_mdns.h.

#include "net_mdns.h"

#include "mdns.h"
#include "esp_log.h"

static const char *TAG = "mdns";

esp_err_t net_mdns_start(const char *hostname, uint16_t admin_port)
{
    esp_err_t err = mdns_init();
    if (err != ESP_OK) {
        ESP_LOGE(TAG, "mdns_init failed: %s", esp_err_to_name(err));
        return err;
    }

    // <hostname>.local + a human-readable instance name.
    err = mdns_hostname_set(hostname);
    if (err != ESP_OK) ESP_LOGW(TAG, "hostname_set: %s", esp_err_to_name(err));
    mdns_instance_name_set("tab5-client");

    // Status web UI — best effort (may be disabled on some builds). The client
    // is a rover display, not a caster, so no _ntrip._tcp service is advertised.
    mdns_txt_item_t http_txt[] = { { "path", "/" } };
    err = mdns_service_add("tab5-client status", "_http", "_tcp", admin_port,
                           http_txt, sizeof(http_txt) / sizeof(http_txt[0]));
    if (err != ESP_OK) {
        ESP_LOGW(TAG, "add _http._tcp:%u: %s", (unsigned)admin_port,
                 esp_err_to_name(err));
    }

    ESP_LOGI(TAG, "mDNS up: %s.local  _http._tcp:%u",
             hostname, (unsigned)admin_port);
    return ESP_OK;
}
