// net_mdns.h — advertise the box over mDNS for zero-config field discovery.
//
// Publishes <hostname>.local plus the status UI (_http._tcp), so a laptop on the
// field WiFi can reach the box by name instead of chasing a DHCP-assigned IP.
//
// REQUIRES an active netif (WiFi via C6/ESP-Hosted, or Ethernet). Until network
// bring-up exists (TODO(hw)) mDNS starts but nothing is reachable.

#pragma once

#include <stdint.h>
#include "esp_err.h"

// Start the mDNS responder and register services. Call once, after network is
// up. `hostname` = the .local label (e.g. "rtk" -> rtk.local).
esp_err_t net_mdns_start(const char *hostname, uint16_t admin_port);
